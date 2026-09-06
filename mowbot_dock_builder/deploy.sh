#!/bin/bash
# Deploy the cross-compiled install/ tree and the deploy/ configs to the
# dock Pi. The workspace path must match the build container's layout:
# ~/mowbot_dock/mowbot_dock_ws (see README).
#
# Usage: ./deploy.sh [user@host]
#   default target: $DOCK_PI, or ubuntu@mowbot-dock.local
set -e

TARGET="${1:-${DOCK_PI:-ubuntu@mowbot-dock.local}}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if [ ! -d "$ROOT/mowbot_dock_ws/install" ]; then
  echo "error: no install/ tree — run dock-build.sh first" >&2
  exit 1
fi

ssh "$TARGET" 'mkdir -p ~/mowbot_dock/mowbot_dock_ws'
rsync -av --delete "$ROOT/mowbot_dock_ws/install/" "$TARGET:mowbot_dock/mowbot_dock_ws/install/"
rsync -av "$ROOT/deploy/" "$TARGET:mowbot_dock/deploy/"

# deploy/config/secrets.yaml (dock broker password) is gitignored and lives
# only on the Pi; the rsync above never deletes, so it survives every deploy.
if ! ssh "$TARGET" 'test -f mowbot_dock/deploy/config/secrets.yaml'; then
  echo "note: no deploy/config/secrets.yaml on the Pi yet - the MQTT bridge will not start (see PI_SETUP.md section 9)"
fi

# Works once the units are installed and sudo is passwordless for systemctl
# (PI_SETUP.md section 9 sudoers line); otherwise restart manually.
for unit in mowbot-dock-agent mowbot-dock-mqtt-bridge; do
  if ssh "$TARGET" "sudo -n systemctl restart $unit 2>/dev/null"; then
    echo "restarted $unit"
  else
    echo "$unit NOT restarted (unit not installed or sudo needs a password)"
  fi
done
echo "deployed"
