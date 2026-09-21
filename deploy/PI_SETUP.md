# Dock Pi one-time setup (Raspberry Pi 3 Model B)

## 1. Flash the SD card
Raspberry Pi Imager → **Ubuntu Server 24.04 LTS (64-bit)**. In the imager
settings: hostname `mowbot-dock`, username **`ubuntu`** (required — the
install/ tree bakes in `/home/ubuntu/...` paths), enable SSH, configure WiFi.

Optionally add a DHCP reservation for the Pi's MAC in the router.

## 2. First boot
```bash
ssh ubuntu@<dock-pi-ip>
sudo apt update && sudo apt upgrade -y
sudo usermod -aG dialout ubuntu        # serial access to the Nano
```

## 2b. Static IP (dock Pi = 192.168.1.91)
Edit `/etc/netplan/50-cloud-init.yaml`: under `wifis: wlan0:` replace
`dhcp4: true` with (keep the existing `access-points:` block):
```yaml
      dhcp4: false
      addresses: [192.168.1.91/24]
      routes:
        - to: default
          via: 192.168.1.1
      nameservers:
        addresses: [192.168.1.1, 1.1.1.1]
```
Then stop cloud-init from regenerating the file, and apply (SSH drops —
reconnect at 192.168.1.91):
```bash
echo 'network: {config: disabled}' | sudo tee /etc/cloud/cloud.cfg.d/99-disable-network-config.cfg
sudo netplan apply
```
Make sure the router's DHCP pool excludes (or reserves) 192.168.1.91.

**Access-point pinning (2026-09-15, updated 2026-09-21).** All access points
share the SSID and the Pi picks one at boot and never roams (wpa_supplicant
`bgscan` roaming wedges the brcmfmac driver — do not enable it). Pin the AP
with the best signal by adding `bssid:` under the SSID entry. Current pin is
the Raspberry Pi AP next to the dock (~-55 dBm, 72 Mbit/s):
```yaml
      access-points:
        "Inteno-661A":
          bssid: "b8:fb:b3:58:54:92"
          auth: ...
```
Survey candidates with `sudo iw dev wlan0 scan | grep -E "^BSS|signal|SSID"`,
but trust the *associated* `iw dev wlan0 link` signal over the scan value —
scan RSSI has been off by up to 30 dB for some APs. Switch live without
stranding the Pi: set the new bssid with `wpa_cli set_network 0 bssid`,
`disconnect`/`reconnect`, ping the gateway for 60 s, and revert the bssid +
`enable_network 0` on failure (run detached with `setsid nohup`). If the
pinned AP is down the Pi is offline: remove the `bssid:` line (or pin the
next-best AP, currently the f4:1e:57:7b:77:8e extender at about -73 dBm) and
`sudo netplan apply`.

## 2c. WiFi stability fix (mandatory)
Symptom (hit 2026-09-06): the Pi falls off WiFi entirely a few minutes after
boot — ping dead, but the system and services keep running. dmesg fills with
`brcmf_sdio_txfail` / `brcmf_sdio_bus_sleep: error while changing bus sleep
state -110`: the Pi 3B's BCM43430 wedges on the SDIO bus when WiFi power save
(on by default) puts it to sleep, and only a driver reload or reboot revives it.

Fix: disable power save at boot, plus a watchdog that reloads brcmfmac if the
gateway stops answering. The units live in `deploy/` (present on the Pi after
the first deploy — before that, scp the four `wifi-*` files over):
```bash
sudo apt install -y iw
sudo install -m 755 ~/mowbot_dock/deploy/wifi-watchdog.sh /usr/local/bin/wifi-watchdog.sh
sudo cp ~/mowbot_dock/deploy/wifi-powersave-off.service \
        ~/mowbot_dock/deploy/wifi-watchdog.service \
        ~/mowbot_dock/deploy/wifi-watchdog.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now wifi-powersave-off.service wifi-watchdog.timer
```
Verify with `iw dev wlan0 get power_save` → "Power save: off". The watchdog
logs under journal tag `wifi-watchdog`; it does not auto-reboot — if a driver
reload ever fails to recover the link, that shows up in its log.

## 2d. Memory cushion — zram swap (mandatory)
The Pi 3B has ~900 MB RAM and the image ships with **no swap**. Any memory
spike then thrashes the box unresponsive (ping still answers, but sshd/zenoh
stop responding) until it recovers or is power-cycled. Confirmed trigger
(2026-09-06): opening **VS Code Remote-SSH** against the Pi — its Node server +
extension host + file watcher indexing `mowbot_dock/` (incl. the large
`build/`/`install/` trees) ate ~430 MB and drove free memory to ~24 MB, load
to 25. apt/snap auto-refresh and colcon builds do the same.

Fix: a compressed-RAM swap device (zram) — a cushion that never touches the SD
card. Units live in `deploy/`:
```bash
sudo install -m 755 ~/mowbot_dock/deploy/zram-swap.sh /usr/local/bin/zram-swap.sh
sudo cp ~/mowbot_dock/deploy/zram-swap.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now zram-swap.service
```
Verify with `swapon --show` → `/dev/zram0 … 1024M`. Even so, **prefer not to run
VS Code Remote-SSH on this Pi** — edit locally and `deploy.sh`. If you must,
exclude the workspace's build trees from the file watcher and keep extensions
minimal.

## 3. Install the ROS 2 Jazzy runtime
Gotcha (hit 2026-08-19): the Ubuntu 24.04.3 preinstalled Pi image ships with
only `noble` + `noble-security` apt suites, but has `noble-updates` package
revisions preinstalled — ROS install then fails with `-dev` exact-version
conflicts. Fix first:
```bash
sudo sed -i 's/^Suites: noble$/Suites: noble noble-updates/' /etc/apt/sources.list.d/ubuntu.sources
sudo apt update && sudo apt upgrade -y
```
Then:
```bash
sudo apt install -y curl
export ROS_APT_SOURCE_VERSION=$(curl -s https://api.github.com/repos/ros-infrastructure/ros-apt-source/releases/latest | grep -F "tag_name" | awk -F\" '{print $4}')
curl -L -o /tmp/ros2-apt-source.deb "https://github.com/ros-infrastructure/ros-apt-source/releases/download/${ROS_APT_SOURCE_VERSION}/ros2-apt-source_${ROS_APT_SOURCE_VERSION}.$(. /etc/os-release && echo $VERSION_CODENAME)_all.deb"
sudo apt install -y /tmp/ros2-apt-source.deb
sudo apt update
sudo apt install -y ros-jazzy-ros-base ros-jazzy-rmw-zenoh-cpp
```

## 4. SSH key for deploys
On the dev machine: `ssh-copy-id ubuntu@<dock-pi-ip>`

## 5. First deploy
On the dev machine:
```bash
~/mowbot_dock/mowbot_dock_builder/deploy.sh ubuntu@<dock-pi-ip>
```

## 6. Manual smoke test (before systemd)
On the Pi, with the Nano plugged into USB:
```bash
. /opt/ros/jazzy/setup.bash
. ~/mowbot_dock/mowbot_dock_ws/install/setup.bash
export RMW_IMPLEMENTATION=rmw_zenoh_cpp
# terminal 1:
ZENOH_ROUTER_CONFIG_URI=~/mowbot_dock/deploy/zenoh-dock-router.json5 /opt/ros/jazzy/lib/rmw_zenoh_cpp/rmw_zenohd
# terminal 2:
~/mowbot_dock/mowbot_dock_ws/install/mowbot_dock/lib/mowbot_dock/dock_agent_node
# expect: "opened /dev/ttyUSB0 @ 115200", "state: (none) -> IDLE", EVT:BOOT logged
```

## 7. Enable the services
```bash
sudo cp ~/mowbot_dock/deploy/zenoh-dock-router.service /etc/systemd/system/
sudo cp ~/mowbot_dock/deploy/mowbot-dock-agent.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now zenoh-dock-router mowbot-dock-agent
journalctl -fu mowbot-dock-agent   # watch it come up
```

Also add `export RMW_IMPLEMENTATION=rmw_zenoh_cpp` to `~/.bashrc` on the Pi
so interactive `ros2 topic` commands see the dock topics.

## 8. Verify from the robot side
With both routers up, on the robot:
```bash
ros2 topic echo --once /dock/state
```

## 9. Dock MQTT bridge (web UI) — second `mowbot_mqtt_bridge` instance
Spec: mowbot_plans / Docking plan / `05_WEB_UI.md` §5.1. The bridge source is
the `mowbot_dock_ws/src/mowbot_mqtt_bridge` submodule (`git submodule update
--init` after cloning; rebuild the Docker image once for the new build deps),
cross-built and shipped by the same `dock-build.sh` + `deploy.sh`.

### 9a. Runtime libraries
```bash
sudo apt install -y libpaho-mqttpp3-1 libpaho-mqtt1.3 libyaml-cpp0.8 \
  ros-jazzy-nav2-msgs ros-jazzy-ublox-ubx-msgs
```

### 9b. Broker credentials
The LXC mosquitto account is `dock` (password in
`/root/mowbot-mqtt-credentials.txt` on the LXC). On the Pi:
```bash
cp ~/mowbot_dock/deploy/config/secrets.yaml.example ~/mowbot_dock/deploy/config/secrets.yaml
nano ~/mowbot_dock/deploy/config/secrets.yaml     # paste the password
chmod 600 ~/mowbot_dock/deploy/config/secrets.yaml
```
`secrets.yaml` is gitignored and `deploy.sh` never deletes it.

### 9c. Passwordless sudo for the web-UI restart / reboot / shutdown buttons
The bridge's launch/power managers exec `sudo -n systemctl …` (no shell).
Scoped sudoers line (matches the exact argv the bridge builds):
```bash
sudo tee /etc/sudoers.d/mowbot-dock-bridge >/dev/null <<'EOF'
ubuntu ALL=(root) NOPASSWD: /usr/bin/systemctl --no-block restart mowbot-dock-agent.service, /usr/bin/systemctl --no-block restart zenoh-dock-router.service, /usr/bin/systemctl --no-block start mowbot-dock-agent.service, /usr/bin/systemctl --no-block start zenoh-dock-router.service, /usr/bin/systemctl --no-block stop mowbot-dock-agent.service, /usr/bin/systemctl --no-block stop zenoh-dock-router.service, /usr/bin/systemctl restart mowbot-dock-agent, /usr/bin/systemctl restart mowbot-dock-mqtt-bridge, /usr/bin/systemctl reboot, /usr/bin/systemctl poweroff
EOF
sudo chmod 440 /etc/sudoers.d/mowbot-dock-bridge && sudo visudo -cf /etc/sudoers.d/mowbot-dock-bridge
```
(The two `restart <unit>` entries without `--no-block` are what `deploy.sh`
runs.) The Logs tab reads journald: `ubuntu` must be in `adm`
(`id ubuntu`; Ubuntu's first user is by default).

### 9d. Enable the unit
```bash
sudo cp ~/mowbot_dock/deploy/mowbot-dock-mqtt-bridge.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now mowbot-dock-mqtt-bridge
journalctl -fu mowbot-dock-mqtt-bridge    # expect: connected to 192.168.1.133, HA discovery published
```
Check from the LXC: `mosquitto_sub -v -t 'ros2/dock/#' -u backend -P …` shows
`ros2/dock/bridge_status {"online":true}` and the retained state topics.

### 9e. Runtime parameters (web UI dock settings, firmware ≥ 0.2.4)
The bridge's `param_control` section (topics.yaml) lets the web UI set the
agent's `complete_a` / `complete_v_min` / `complete_s` / `topup_interval_s`.
Values persist in `~/mowbot_dock/data/dock_overrides.yaml` (bridge creates the
directory on the first set; the agent unit's wrapper loads the file when it
exists). Since 2026-09-15 the agent unit runs `deploy/dock-agent-start.sh`
instead of an inline command — re-install the unit once after that deploy:
```bash
sudo cp ~/mowbot_dock/deploy/mowbot-dock-agent.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl restart mowbot-dock-agent   # DTR-resets the Nano
```
Check: `journalctl -u mowbot-dock-agent -o short-iso -n 30` shows `EVT:BOOT`,
`Nano running: syncing runtime parameters (EVT:BOOT)`, four `-> SET:` lines,
the Nano's own four `EVT:CFG:` default lines, then four more `EVT:CFG:` echoes
with the agent's values.
Ad hoc: `ros2 param set /dock_agent complete_s 120` (rejected outside 1–3600).

Reboot / shutdown from the web UI: a halted Pi keeps its USB 5 V, so the Nano
keeps charging on its own interlocks; there is **no remote power-on** — the
Pi stays off until unplugged and re-plugged.
