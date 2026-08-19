#!/bin/bash
# Interactive shell inside the arm64 mowbot-dock-builder container,
# with the workspace bind-mounted at ~/mowbot_dock/mowbot_dock_ws.
docker run --rm -it --platform linux/arm64 \
  -v "$HOME/mowbot_dock/mowbot_dock_ws":/home/ubuntu/mowbot_dock/mowbot_dock_ws \
  -v "$HOME/mowbot_dock/.ccache-mowbot-dock":/home/ubuntu/.ccache \
  mowbot-dock-builder \
  bash
