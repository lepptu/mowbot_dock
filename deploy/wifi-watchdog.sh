#!/bin/bash
# Revive wlan0 if the gateway is unreachable — the BCM43430 sometimes wedges
# on the SDIO bus (brcmf_sdio_bus_sleep -110 spam in dmesg) and only a driver
# reload brings it back. Run from wifi-watchdog.timer every 2 minutes.
GW=192.168.1.1
STATE=/run/wifi-watchdog.fails

if ping -c 3 -W 2 -I wlan0 "$GW" >/dev/null 2>&1; then
  echo 0 > "$STATE"
  exit 0
fi

fails=$(( $(cat "$STATE" 2>/dev/null || echo 0) + 1 ))
echo "$fails" > "$STATE"
logger -t wifi-watchdog "gateway $GW unreachable ($fails consecutive)"

# one failed round can be AP flakiness; act on the second
if [ "$fails" -lt 2 ]; then exit 0; fi

logger -t wifi-watchdog "reloading brcmfmac to recover wlan0"
modprobe -r brcmfmac 2>/dev/null
modprobe -r brcmfmac_wcc 2>/dev/null
modprobe brcmfmac
sleep 8
netplan apply
sleep 15
iw dev wlan0 set power_save off 2>/dev/null

if ping -c 3 -W 2 -I wlan0 "$GW" >/dev/null 2>&1; then
  logger -t wifi-watchdog "wlan0 recovered"
  echo 0 > "$STATE"
else
  logger -t wifi-watchdog "wlan0 still down after driver reload"
fi
