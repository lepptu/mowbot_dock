#!/bin/bash
# ExecStart wrapper for mowbot-dock-agent.service.
#
# Loads the web-UI parameter overrides (written by the dock MQTT bridge's
# param_control, see deploy/config/topics.yaml) when the file exists, so the
# agent already runs the operator's complete_a / complete_v_min / complete_s /
# topup_interval_s before the bridge re-applies them. Missing file = firmware
# defaults; a value outside the agent's ranges is logged and replaced by the
# default (the agent never refuses to start over it).
set -e
. /opt/ros/jazzy/setup.sh
. /home/ubuntu/mowbot_dock/mowbot_dock_ws/install/setup.sh

OVERRIDES=/home/ubuntu/mowbot_dock/data/dock_overrides.yaml
ARGS=()
if [ -f "$OVERRIDES" ]; then
  ARGS=(--ros-args --params-file "$OVERRIDES")
fi
exec /home/ubuntu/mowbot_dock/mowbot_dock_ws/install/mowbot_dock/lib/mowbot_dock/dock_agent_node "${ARGS[@]}"
