# HANDOFF — dock side done, robot side next

Status document for whoever (human or Claude) picks up the **robot-side docking
and web UI work** on another machine. The dock is built, deployed, and verified
charging a real robot; this file is the interface contract and operational
knowledge you need. Last updated: 2026-09-06.

Related repos and docs:
- This repo: `git@github.com:lepptu/mowbot_dock.git` (dock Pi software + build/deploy tooling)
- Plans/spec: https://github.com/lepptu/mowbot_plans → `Docking plan/` (01 hardware,
  02 Nano firmware protocol, 03 dock Pi + web UI, 04 robot modifications). Doc 04 is
  the robot-side spec you are implementing.
- File map here: `mowbot_dock_ws/src/mowbot_dock/` (the ROS package),
  `mowbot_dock_builder/` (arm64 cross-build + `deploy.sh`), `deploy/` (everything
  installed on the Pi: systemd units, zenoh config, `PI_SETUP.md`), `TODO.md` (live
  status/roadmap).

## 1. System map

```
DOCK PI 192.168.1.91 (Pi 3B, Ubuntu 24.04, user ubuntu)      ROBOT PI 192.168.1.90 (static)
  Arduino Nano fw 0.1.4 ── USB /dev/ttyUSB0                    robot zenoh router :7447
  mowbot-dock-agent.service (dock_agent_node)                        ▲
  zenoh-dock-router.service ── connects out ────────────────────────┘
                                                       workstation 192.168.1.104 +
All ROS 2 traffic runs rmw_zenoh_cpp.                  laptop Foxglove hang off the
Everything meets at the robot router.                  robot router as zenoh clients.
```

- The dock federates **outward** to `tcp/192.168.1.90:7447`; nothing on the robot
  references the dock's IP. Robot off → dock runs standalone and re-federates
  automatically when the robot returns. Verified working 2026-08-23: all 13
  `dock/*` topics visible through the robot router with zero robot-side config.
- Robot-side nodes need **no setup** to see dock topics — if they're on the robot's
  zenoh mesh, `/dock/*` is just there.
- Ad-hoc inspection from any ROS 2 Jazzy machine (robot on):
  `export RMW_IMPLEMENTATION=rmw_zenoh_cpp ZENOH_CONFIG_OVERRIDE='mode="client";connect/endpoints=["tcp/192.168.1.90:7447"]'`
  then `ros2 topic list`. Substitute `.91` to talk to the dock directly (robot off).

## 2. The dock's ROS 2 interface (the contract)

Published by `dock_agent` at ~10 Hz (one publish per serial status frame):

| Topic | Type | QoS | Meaning |
|---|---|---|---|
| `/dock/state` | `std_msgs/Int32` | latched* | 0 IDLE, 1 SEATED, 2 RAMP, 3 CHARGING, 4 DRAIN, 5 COMPLETE, 6 SELFTEST, 7 FAULT |
| `/dock/microswitch` | `std_msgs/Bool` | latched | true = robot physically seated on contacts |
| `/dock/relay_k1`, `/dock/relay_k2` | `std_msgs/Bool` | latched | 42 V DC relay / mains pilot relay commanded state |
| `/dock/fault` | `std_msgs/Int32` | latched | 0 none, 1 overcurrent, 2 AC weld, 3 no-42V, 4 watchdog silence |
| `/dock/self_test_result` | `std_msgs/Bool` | latched | outcome of last self-test |
| `/dock/charge_current` | `std_msgs/Float32` | volatile | amps into the pack (±0.02 A sensor noise floor) |
| `/dock/charger_voltage` | `std_msgs/Float32` | volatile | divider reading; ~0 when dock cold (AC off) |
| `/dock/event` | `std_msgs/String` | volatile | raw firmware lines: `EVT:BOOT:<ver>`, `EVT:VER:<ver>` (60 s heartbeat), `EVT:SELFTEST:OK|FAIL`, `EVT:EMERGENCY:SWITCH`, `EVT:NOCURRENT` |
| `/dock/battery_state` | `sensor_msgs/BatteryState` | volatile | composed view for the docking server — see §3 |
| `/dock/charge_enable` | `std_msgs/Bool` | latched | echo of the agent's charge permission (added 2026-09-06 for the web UI) |
| `/dock/firmware_version` | `std_msgs/String` | latched | from `EVT:BOOT`/`EVT:VER`, published on change (added 2026-09-06) |

*latched = `transient_local` depth 1: late joiners immediately get the last value.
Subscribe with default (volatile) QoS — that's compatible.

Subscribed by `dock_agent` (all `std_msgs/Bool`):

| Topic | Semantics |
|---|---|
| `/dock/charge_enable_cmd` | level: false = charging forbidden (maintenance). Default true at agent start. |
| `/dock/clear_fault_cmd` | publish `true` once → agent pulses the firmware's edge-triggered clearFault |
| `/dock/self_test_cmd` | publish `true` once → dock runs ≤2 s AC pulse self-test (only honored in IDLE/COMPLETE), result on `/dock/self_test_result` |

## 3. `/dock/battery_state` details (what the docking server consumes)

- `current`: positive = charging (BatteryState convention).
- `voltage` and `percentage`: **NaN whenever `charger_voltage` < 20 V** — the divider
  only sees the pack when a relay path connects it, so "dock cold" ≠ "battery empty".
  Handle NaN.
- `percentage`: linear 33 V → 0.0, 42 V → 1.0 (range 0–1, not 0–100).
- `power_supply_status`: RAMP/CHARGING/DRAIN → `CHARGING`(1); COMPLETE → `FULL`(4);
  SEATED/SELFTEST/FAULT → `NOT_CHARGING`(3); IDLE → `UNKNOWN`(0).
- `present` = microswitch.

**⚠ Critical caveat for "charging finished" logic:** firmware 0.1.4 ends every
charge with `DRAIN → IDLE` *even while the robot stays seated* — spec state 5
(COMPLETE) has never occurred on real hardware, so `FULL` is currently never
reported, and a parked, fully-charged robot shows `status=UNKNOWN, present=true`.
The battery tops up in short bursts (observed: 40 s–3 min CHARGING, ~8 min idle
gaps). Open decision (TODO.md §6): either firmware adds COMPLETE, or the agent
remaps IDLE+seated → FULL/NOT_CHARGING. **Do not build robot-side "charge done"
detection on `status==FULL` until this is resolved** — `present && status==UNKNOWN
&& state==0` currently means "seated, resting between top-ups".

**Charge/docking success detection (for opennav_docking `isCharging`):** use
`status == CHARGING` or `current > ~0.3 A`. Verified real behavior: charging
starts ~200 ms after seating (SEATED→CHARGING nearly immediate; RAMP is ~100 ms
and usually invisible at 10 Hz sampling), so contact → current flow is fast and
reliable. Three real charge cycles observed 2026-08-23, all clean, fault 0.

## 4. Robot-side work to build (per plan doc 04 — none of it started)

1. `opennav_docking` server on the robot; dock pose recorded to
   `~/pi_ws/mowing_data/config/dock.json` ("park + save dock here" flow);
   staging pose ~0.7 m in front; **robot docks backwards** (contacts on rear;
   `dock_backwards`). Blind RTK docking first; camera/AprilTag "look-latch-turn"
   refinement is the designed upgrade path (forward camera only — see plan).
2. Charge detection plugin/config consuming `/dock/battery_state` (see §3 caveat).
3. `dock_manager` bridge node integrating docking with the mission system.
4. ~~Second `mowbot_mqtt_bridge` instance~~ **prepared 2026-09-06** (submodule +
   `deploy/config/topics.yaml` + unit + PI_SETUP §9; build/deploy from the
   workstation still owed). Publishes `ros2/dock/*` (retained: state/microswitch/
   relays/fault/self_test_result/charge_enable/firmware_version; volatile:
   current/voltage/battery_state/event/pi/system), plus `ros2/dock/{logs,launch,
   power}/*` for the web UI's Logs tab and Dock page buttons. Mosquitto `dock`
   account/ACL live on the LXC.
5. Web UI (spec: plans `05_WEB_UI.md`): Phase A Dock page (telemetry +
   maintenance + dock Pi controls), Phase B dock pose (`/api/dock`, map
   markers), Phase C Dock/Undock control.

## 5. Operations (dock side — you shouldn't need to touch it, but when you do)

- Update cycle, from the x86 workstation only:
  `mowbot_dock_builder/dock-build.sh` (arm64 cross-build in Docker/qemu) →
  `mowbot_dock_builder/deploy.sh ubuntu@192.168.1.91` (rsync install/ + deploy/,
  restarts agent). Never build on the Pi.
- Logs: `ssh ubuntu@192.168.1.91 journalctl -fu mowbot-dock-agent` (state
  transitions and EVT lines are logged at INFO). Router: `-u zenoh-dock-router`.
- Services on the Pi (all enabled): `zenoh-dock-router`, `mowbot-dock-agent`,
  `wifi-powersave-off`, `wifi-watchdog.timer`, `zram-swap`.
- Pi stability — two hard-won fixes, already in place (details in `deploy/PI_SETUP.md`
  §2c/§2d): (a) WiFi power save OFF + brcmfmac-reload watchdog — the Pi 3B's
  BCM43430 wedges its SDIO bus under default power save and drops off WiFi;
  (b) 1 GB zram swap — 900 MB RAM with no swap thrashes unresponsive under
  memory spikes. **Do not open VS Code Remote-SSH against the dock Pi** (confirmed
  trigger: its indexer ate ~430 MB and froze the box). Edit locally, deploy.sh.
- Re-flash from scratch: follow `deploy/PI_SETUP.md` top to bottom (includes the
  missing-`noble-updates` apt gotcha, static IP, and the stability fixes).
- Serial: opening the port DTR-resets the Nano (relays drop, charge session
  aborts, `EVT:BOOT` follows) — documented firmware design, not a bug. Agent
  reconnects automatically every 2 s if the port vanishes.
- Firmware watchdog: agent's 10 Hz command frames feed it. ≥2 s of silence during
  a charge → fault 4 (report-only; charging continues on firmware interlocks;
  self-clears when frames resume).

## 6. Open items & untested paths

- COMPLETE-state decision (§3 caveat) — blocks clean "charge done" semantics.
- Fault paths (overcurrent, AC weld, no-42V) have **never fired on real
  hardware** — the first real fault will be new territory.
- `dock/charge_enable_cmd=false`, `clear_fault_cmd`, and `self_test_cmd` are
  implemented and mock-tested but **not yet exercised against the real Nano**.
- A diagnostic "flight recorder" (`dock-flightrec` unit → UDP to workstation
  :9999) may still be running on the Pi from the 2026-09-06 memory debugging —
  remove once zram stability is confirmed (see memory notes / PI_SETUP).
- Cleanup: `dock_smoke_test` package still in the workspace, delete when convenient.
