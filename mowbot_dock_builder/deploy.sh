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

# Works once the units are installed and sudo is passwordless for systemctl;
# otherwise restart manually: ssh in, sudo systemctl restart mowbot-dock-agent
if ssh "$TARGET" 'sudo -n systemctl restart mowbot-dock-agent 2>/dev/null'; then
  echo "deployed and agent restarted"
else
  echo "deployed (agent NOT restarted — unit not installed or sudo needs a password)"
fi
