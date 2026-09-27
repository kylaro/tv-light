#!/usr/bin/env bash
# Flashes firmware/build/tvlight.uf2 onto the Pico.
# If our firmware is already running, it is rebooted into BOOTSEL via the
# 1200-baud "touch"; otherwise hold BOOTSEL while plugging the Pico in.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# A local build wins; otherwise use the committed prebuilt image (no toolchain needed).
UF2="$ROOT/firmware/build/tvlight.uf2"
[ -f "$UF2" ] || UF2="$ROOT/firmware/prebuilt/tvlight.uf2"
BOOTSEL_BAUD=1200
WAIT_SECONDS=30

find_mount() { findmnt -rn -o TARGET -S LABEL=RPI-RP2 2>/dev/null | head -1; }

if [ -z "$(find_mount)" ]; then
  port=$(ls /dev/serial/by-id/*TV_Light* /dev/serial/by-id/*Raspberry_Pi* 2>/dev/null | head -1 || true)
  if [ -n "$port" ]; then
    echo "Rebooting $port into BOOTSEL..."
    stty -F "$port" "$BOOTSEL_BAUD" || true
  else
    echo "Hold BOOTSEL and plug in the Pico..."
  fi
fi

for _ in $(seq "$WAIT_SECONDS"); do
  mnt=$(find_mount)
  if [ -z "$mnt" ]; then
    dev=$(readlink -f /dev/disk/by-label/RPI-RP2 2>/dev/null || true)
    [ -b "${dev:-}" ] && udisksctl mount -b "$dev" >/dev/null 2>&1 || true
    mnt=$(find_mount)
  fi
  if [ -n "$mnt" ]; then
    cp "$UF2" "$mnt/" && sync
    echo "Flashed $UF2 -> $mnt"
    exit 0
  fi
  sleep 1
done
echo "No RPI-RP2 drive appeared." >&2
exit 1
