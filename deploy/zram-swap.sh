#!/bin/bash
# Set up a compressed RAM swap device (zram) on the dock Pi. The Pi 3B has
# ~900 MB RAM and no swap, so any memory spike (VS Code Remote-SSH indexing,
# apt/snap refresh, a colcon build) thrashes it unresponsive. zram gives a
# cushion that degrades gracefully — and unlike a swapfile it never touches
# the SD card. Sized 1 GB with zstd (~2.5-3x), so worst-case real RAM cost is
# ~350-400 MB; lazily allocated, so near-zero when idle.
set -e
modprobe zram num_devices=1 2>/dev/null || true
# wait for the device node (modprobe can return before /dev/zram0 appears)
for i in $(seq 1 20); do [ -b /dev/zram0 ] && break; sleep 0.2; done
# reset if already configured (idempotent restart)
if swapon --show=NAME --noheadings | grep -q /dev/zram0; then
  swapoff /dev/zram0 || true
fi
echo 1 > /sys/block/zram0/reset 2>/dev/null || true
echo zstd > /sys/block/zram0/comp_algorithm 2>/dev/null || true
echo 1G > /sys/block/zram0/disksize
mkswap /dev/zram0
# priority high so zram is preferred; -d discards on swapoff
swapon -p 100 /dev/zram0
sysctl -q vm.swappiness=100
sysctl -q vm.page-cluster=0
swapon --show
