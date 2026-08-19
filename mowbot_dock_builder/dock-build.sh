#!/bin/bash
# Build ~/mowbot_dock/mowbot_dock_ws inside the arm64 mowbot-dock-builder container.
# Extra arguments are appended to colcon build, e.g.:
#   ./dock-build.sh --packages-select mowbot_dock
#
# No --symlink-install on purpose: the install/ tree is shipped to the dock Pi
# and must not reference this machine's src/.
set -e

docker run --rm --platform linux/arm64 \
  -v "$HOME/mowbot_dock/mowbot_dock_ws":/home/ubuntu/mowbot_dock/mowbot_dock_ws \
  -v "$HOME/mowbot_dock/.ccache-mowbot-dock":/home/ubuntu/.ccache \
  mowbot-dock-builder \
  bash -c 'source /opt/ros/jazzy/setup.bash && cd ~/mowbot_dock/mowbot_dock_ws && \
           exec colcon build \
             --cmake-args -DCMAKE_BUILD_TYPE=Release \
                          -DCMAKE_C_COMPILER_LAUNCHER=ccache \
                          -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
             "$@"' colcon-build "$@"
