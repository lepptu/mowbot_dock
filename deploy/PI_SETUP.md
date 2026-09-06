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
