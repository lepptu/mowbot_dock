# mowbot_dock ROS 2 package — TODO

Spec source: mowbot_plans / Docking plan / `03_DOCK_PI_ROS2_AND_WEBUI.md`

## 0. Decisions
- [x] Package name confirmed: `mowbot_dock` (decided 2026-08-19)
- [ ] Pin the Nano serial protocol version (8-field CSV frame layout + `EVT:*` lines) against `02_ARDUINO_FIRMWARE.md` before coding the parser

## 1. Package scaffold
- [ ] `ament_cmake` package in `mowbot_dock_ws/src/` with deps: `rclcpp`, `std_msgs`, `sensor_msgs`
- [ ] `serial_port.hpp/cpp` — RAII termios wrapper (open/configure/read/write, non-blocking)
- [ ] `dock_agent_node` executable skeleton with parameters: serial device (default `/dev/ttyUSB0`), baud, publish rate

## 2. Serial layer (Nano ↔ Pi)
- [ ] Tolerant CSV parser: exactly 8 numeric fields per frame, discard partial/garbled lines
- [ ] `EVT:*` line handling (BOOT, SELFTEST:OK|FAIL, NOCURRENT, EMERGENCY, …)
- [ ] 10 Hz command frame TX (chargeEnable / clearFault / selfTest) — feeds Nano watchdog
- [ ] Handle DTR reset on port open (aborts charge session by design) + reconnect loop on unplug/EIO

## 3. ROS 2 interface
- [ ] Publishers (~10 Hz, one per received frame):
  - `dock/state` (Int32, 0–7: IDLE…FAULT)
  - `dock/microswitch`, `dock/relay_k1`, `dock/relay_k2` (Bool)
  - `dock/charge_current`, `dock/charger_voltage` (Float32)
  - `dock/fault` (Int32: 1=overcurrent, 2=AC weld, 3=ramp failure, 4=watchdog)
  - `dock/self_test_result` (Bool), `dock/event` (String passthrough)
  - `dock/battery_state` (sensor_msgs/BatteryState — consumed by robot's docking_server)
- [ ] Subscribers with edge detection:
  - `dock/charge_enable_cmd` (false→true re-arms from COMPLETE)
  - `dock/clear_fault_cmd`, `dock/self_test_cmd`
- [ ] BatteryState composition: voltage→percentage mapping (33 V empty … 42 V full), power_supply_status from dock state

## 4. Testing without hardware
- [ ] Mock Nano: Python script on a pty (socat) emitting CSV frames + reacting to command frames
- [ ] Unit tests for the CSV/EVT parser (gtest via ament)
- [ ] Cross-build with `dock-build.sh`, confirm aarch64 binary, run against mock inside the container

## 5. Deploy to dock Pi
- [ ] `deploy.sh` in `mowbot_dock_builder/`: rsync `install/` → `pi:~/mowbot_dock/mowbot_dock_ws/install/`
- [ ] Zenoh router config on dock Pi with connect endpoint `tcp/192.168.1.90:7447` (robot Pi, static)
- [ ] systemd units: `zenoh-dock-router.service`, `mowbot-dock-agent.service` (Restart=on-failure, After=router, `RMW_IMPLEMENTATION=rmw_zenoh_cpp` in every unit)
- [ ] End-to-end smoke test on the Pi with the real Nano attached

## 6. Later (separate from this package)
- [ ] Second `mowbot_mqtt_bridge` instance (dock topics.yaml, Mosquitto `dock` user/ACL)
- [ ] Web UI: DockPanel / DockCard + FastAPI `/api/dock` endpoints (robot side)
- [ ] `opennav_docking` integration + dock pose recording (robot side)
- [ ] Delete `dock_smoke_test` once the real package builds
