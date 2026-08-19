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

## 3. Install the ROS 2 Jazzy runtime
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
