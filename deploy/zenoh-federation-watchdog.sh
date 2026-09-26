#!/bin/bash
# Restart the dock's zenoh router when the link to the robot's router is up
# but federation is dead. Seen 2026-09-25 with zenoh 1.6.2 (rmw_zenoh_cpp
# 0.2.9): after a WiFi blip or a robot reboot the router-to-router link comes
# back, but the dock router keeps logging
#   "Could not find corresponding link in routers network for Face{..}"
# and drops every declaration crossing that link — the robot then sees the
# dock topics with zero publishers and refuses docking with `dock_offline`.
# A restart of zenoh-dock-router (fresh zid, fresh link) always cleared it.
# Run from zenoh-federation-watchdog.timer every 3 min, as user ubuntu.
#
#   --check   print the verdict, never restart (run by hand)
ROBOT=192.168.1.90
ROBOT_PORT=7447
UNIT=zenoh-dock-router.service
AGENT=mowbot-dock-agent.service
TAG=zenoh-federation-watchdog
STATE_DIR=${RUNTIME_DIRECTORY:-/run/zenoh-federation-watchdog}
MIN_RESTART_GAP=600   # s between two automatic restarts
MIN_ROUTER_AGE=180    # s the router must have been up before it is judged
SIG='Could not find corresponding link in routers network'
SIG_WINDOW=3min
SIG_MIN=20            # signature lines in the window that count as wedged
CHECK_TIMEOUT=60      # s for the ros2 graph check (~8 s on the Pi 3B)

DRY=0; [ "$1" = "--check" ] && DRY=1
log() { logger -t "$TAG" -- "$*"; [ "$DRY" = 1 ] && echo "$*"; }
mkdir -p "$STATE_DIR" 2>/dev/null || STATE_DIR=/tmp
LAST="$STATE_DIR/last-restart"

# Nothing to heal / nothing to expect: router down (systemd restarts it on
# failure), agent down, router restarted a moment ago, or no link to the
# robot at all (robot off, WiFi down — the router reconnects by itself).
systemctl -q is-active "$UNIT"  || { [ "$DRY" = 1 ] && echo "router not active"; exit 0; }
systemctl -q is-active "$AGENT" || { [ "$DRY" = 1 ] && echo "agent not active"; exit 0; }
started_us=$(systemctl show -p ActiveEnterTimestampMonotonic --value "$UNIT")
now_us=$(awk '{printf "%d", $1 * 1000000}' /proc/uptime)
age=$(( (now_us - ${started_us:-0}) / 1000000 ))
if [ "$age" -lt "$MIN_ROUTER_AGE" ]; then
  [ "$DRY" = 1 ] && echo "router up only ${age}s — not judged yet"; exit 0
fi
if ! ss -Htn state established "( dport = :$ROBOT_PORT )" | grep -q "$ROBOT:$ROBOT_PORT"; then
  [ "$DRY" = 1 ] && echo "no established link to $ROBOT:$ROBOT_PORT — nothing to heal"; exit 0
fi

# Evidence 1: the wedge's own log line in the last few minutes.
sig=$(journalctl -u "$UNIT" --since "-$SIG_WINDOW" --no-pager -o cat 2>/dev/null | grep -c "$SIG")

# Evidence 2: the ROS graph as the ROBOT's router sees it, via a client
# session straight to it (bypasses the dock router entirely). Healthy =
# /dock_agent is listed there.
out=$(cd /tmp && . /opt/ros/jazzy/setup.sh && \
  RMW_IMPLEMENTATION=rmw_zenoh_cpp \
  ZENOH_CONFIG_OVERRIDE="mode=\"client\";connect/endpoints=[\"tcp/$ROBOT:$ROBOT_PORT\"]" \
  timeout "$CHECK_TIMEOUT" ros2 node list --no-daemon 2>/dev/null)
rc=$?
if [ "$rc" -ne 0 ] || [ -z "$out" ]; then
  view=unreachable        # robot router not answering a fresh session
elif grep -qx "/dock_agent" <<<"$out"; then
  view=ok
else
  view=missing            # robot sees a graph, but not the dock agent
fi

wedged=0
[ "$view" = missing ] && wedged=1
[ "$view" != ok ] && [ "$sig" -ge "$SIG_MIN" ] && wedged=1

if [ "$wedged" = 0 ]; then
  [ "$DRY" = 1 ] && echo "ok: robot view=$view, signature lines in $SIG_WINDOW=$sig, router age ${age}s"
  [ "$sig" -ge "$SIG_MIN" ] && log "graph fine on the robot but $sig '$SIG' lines in $SIG_WINDOW — watching"
  exit 0
fi

last=$(cat "$LAST" 2>/dev/null || echo 0); now=$(date +%s)
msg="federation wedged: robot view=$view, $sig signature lines in $SIG_WINDOW, router up ${age}s"
if [ "$DRY" = 1 ]; then echo "WOULD RESTART — $msg"; exit 0; fi
if [ $((now - last)) -lt "$MIN_RESTART_GAP" ]; then
  log "$msg — last automatic restart $((now - last))s ago, holding"; exit 0
fi
log "$msg — restarting $UNIT"
if sudo -n systemctl restart "$UNIT"; then
  echo "$now" > "$LAST"
  log "$UNIT restarted (the agent and bridge reconnect to it by themselves)"
else
  log "restart of $UNIT FAILED (sudo -n systemctl restart)"
fi
