# mowbot_dock ROS 2 package — TODO

Spec source: mowbot_plans / Docking plan / `03_DOCK_PI_ROS2_AND_WEBUI.md`

## 0. Decisions
- [x] Package name confirmed: `mowbot_dock` (decided 2026-08-19)
- [x] Pin the Nano serial protocol version (8-field CSV frame layout + `EVT:*` lines) against `02_ARDUINO_FIRMWARE.md` before coding the parser

## 1. Package scaffold — DONE 2026-08-19
- [x] `ament_cmake` package in `mowbot_dock_ws/src/` with deps: `rclcpp`, `std_msgs`, `sensor_msgs`
- [x] `serial_port.hpp/cpp` — RAII termios wrapper (open/configure/read/write, non-blocking)
- [x] `dock_agent_node` executable skeleton with parameters: serial device (default `/dev/ttyUSB0`), baud, publish rate

## 2. Serial layer (Nano ↔ Pi) — DONE 2026-08-19
- [x] Tolerant CSV parser: exactly 8 numeric fields per frame, discard partial/garbled lines
- [x] `EVT:*` line handling (BOOT, SELFTEST:OK|FAIL, NOCURRENT, EMERGENCY, …)
- [x] 10 Hz command frame TX (chargeEnable / clearFault / selfTest) — feeds Nano watchdog
- [x] Handle DTR reset on port open (aborts charge session by design) + reconnect loop on unplug/EIO

## 3. ROS 2 interface — DONE 2026-08-19
- [x] Publishers (~10 Hz, one per received frame):
  - `dock/state` (Int32, 0–7: IDLE…FAULT)
  - `dock/microswitch`, `dock/relay_k1`, `dock/relay_k2` (Bool)
  - `dock/charge_current`, `dock/charger_voltage` (Float32)
  - `dock/fault` (Int32: 1=overcurrent, 2=AC weld, 3=ramp failure, 4=watchdog)
  - `dock/self_test_result` (Bool), `dock/event` (String passthrough)
  - `dock/battery_state` (sensor_msgs/BatteryState — consumed by robot's docking_server)
- [x] Subscribers with edge detection:
  - `dock/charge_enable_cmd` (false→true re-arms from COMPLETE)
  - `dock/clear_fault_cmd`, `dock/self_test_cmd` (stretched to 3-frame pulses, auto-cleared)
- [x] BatteryState composition: voltage→percentage mapping (33 V empty … 42 V full), power_supply_status from dock state; NaN when divider reads <20 V (dock cold)

## 4. Testing without hardware — DONE 2026-08-19
- [x] Mock Nano: Python script on a pty emitting CSV frames + reacting to command frames (`tools/mock_nano.py`)
- [x] Unit tests for the CSV/EVT parser (gtest via ament) — 11 tests passing
- [x] Cross-build with `dock-build.sh`, confirm aarch64 binary, run against mock inside the container
      (full IDLE→…→COMPLETE session verified over rmw_zenoh; note: Fast DDS discovery is broken
      under qemu, test with RMW_IMPLEMENTATION=rmw_zenoh_cpp + rmw_zenohd in the container)

## 5. Deploy to dock Pi (files prepared 2026-08-19 — untested until the Pi exists)
- [x] `deploy.sh` in `mowbot_dock_builder/`: rsync `install/` + `deploy/` → Pi, restart agent
- [x] Zenoh router config (`deploy/zenoh-dock-router.json5`) with connect endpoint `tcp/192.168.1.90:7447` (robot Pi, static)
- [x] systemd units (`deploy/*.service`): `zenoh-dock-router`, `mowbot-dock-agent` (Restart=on-failure, After=router, `RMW_IMPLEMENTATION=rmw_zenoh_cpp`)
- [x] One-time Pi setup checklist: `deploy/PI_SETUP.md`
- [x] Run PI_SETUP.md on the real Pi (flash, ROS runtime, deploy, enable services) — done 2026-08-19, dock Pi live at 192.168.1.91
- [x] End-to-end smoke test on the Pi with the real Nano attached — agent receives EVT:BOOT:0.1.4 + status frames; dock/* topics visible over WiFi from the workstation
- [ ] Verify zenoh federation dock↔robot with the robot powered on (robot was off during setup)

## 6. Next steps (ordered 2026-08-20)
- [x] Robot-on check (2026-08-23): all 13 dock/* topics visible via the robot router —
      normal workstation setup + Foxglove work without overrides
- [x] First supervised charge (2026-08-23): three clean cycles, fault 0, no EMERGENCY/
      NOCURRENT. RAMP invisible at 10 Hz sampling (~100 ms state) — expected.
- [ ] Decide: firmware 0.1.4 ends charge with DRAIN→IDLE while seated (COMPLETE/state 5
      never occurs; top-up cycling pattern). Either add COMPLETE to firmware per spec,
      or remap agent BatteryState (IDLE && microswitch → FULL/NOT_CHARGING, not UNKNOWN)
      so robot side can detect "done charging". Fault paths still never fired on real HW.
- [ ] Robot side (mowbot repos, per plan doc 04): install `opennav_docking`,
      record dock pose, staging approach, charge detection from dock/battery_state,
      `dock_manager` bridge node → `/dock_robot` end to end
- [ ] Second `mowbot_mqtt_bridge` instance (dock topics.yaml, Mosquitto `dock` user/ACL)
      — makes the web UI robot-off independent
- [ ] Web UI: DockPanel / DockCard + FastAPI `/api/dock` endpoints (robot side)
- [ ] Cleanup: delete `dock_smoke_test`; optional foxglove_bridge unit on the dock Pi
