#!/usr/bin/env bash
# Flashes the TV Light firmware onto the RP2040 controller.
# If our firmware is already running, it is rebooted into BOOTSEL via the
# 1200-baud "touch"; otherwise hold BOOTSEL while plugging the board in.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# A local build wins; otherwise use the committed prebuilt image (no toolchain needed).
UF2="$ROOT/firmware/build/tvlight.uf2"
[ -f "$UF2" ] || UF2="$ROOT/firmware/prebuilt/tvlight.uf2"
BOOTSEL_BAUD=1200
WAIT_SECONDS=60
LABEL=RPI-RP2

find_port() { ls /dev/serial/by-id/*TV_Light* /dev/serial/by-id/*Raspberry_Pi* 2>/dev/null | head -1 || true; }
# Only trust a mount whose bootloader info file is readable (not a stale mount point).
find_mount() {
  local mnt
  mnt=$(findmnt -rn -o TARGET -S "LABEL=$LABEL" 2>/dev/null | head -1 || true)
  [ -n "$mnt" ] && [ -r "$mnt/INFO_UF2.TXT" ] && echo "$mnt"
  return 0
}

port=$(find_port)
if [ -n "$port" ]; then
  echo "Rebooting $port into BOOTSEL..."
  stty -F "$port" "$BOOTSEL_BAUD" || true
  for _ in $(seq "$WAIT_SECONDS"); do [ -e "$port" ] || break; sleep 1; done
elif [ -z "$(find_mount)" ]; then
  echo "Hold BOOTSEL and plug in the controller..."
fi

mnt=""
for _ in $(seq "$WAIT_SECONDS"); do
  mnt=$(find_mount)
  [ -n "$mnt" ] && break
  dev=$(readlink -f "/dev/disk/by-label/$LABEL" 2>/dev/null || true)
  if [ -b "${dev:-}" ]; then udisksctl mount -b "$dev" >/dev/null 2>&1 || true; fi
  sleep 1
done
[ -n "$mnt" ] || { echo "No $LABEL drive appeared." >&2; exit 1; }

cp "$UF2" "$mnt/" && sync
echo "Copied $UF2 -> $mnt"

for _ in $(seq "$WAIT_SECONDS"); do
  port=$(find_port)
  if [ -n "$port" ]; then
    echo "Controller is running the new firmware ($port)"
    exit 0
  fi
  sleep 1
done
echo "Copied, but the controller did not come back as a serial port." >&2
exit 1
