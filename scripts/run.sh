#!/usr/bin/env bash
# Runs the tvlight:latest image in the foreground with what it needs from the host:
#   PipeWire + D-Bus session sockets (in $XDG_RUNTIME_DIR), /dev for the Pico
#   (hotplug safe), host networking for the web UI, and the config directory.
set -euo pipefail
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/tvlight"
mkdir -p "$CONFIG_DIR"
exec podman run --rm --replace --name tvlight --init \
  --userns keep-id --security-opt label=disable \
  --network host \
  -v "$RUNTIME_DIR:$RUNTIME_DIR" -e XDG_RUNTIME_DIR="$RUNTIME_DIR" \
  -e DBUS_SESSION_BUS_ADDRESS="unix:path=$RUNTIME_DIR/bus" \
  -v /dev:/dev:rslave \
  -v "$CONFIG_DIR:/config" \
  tvlight:latest --config /config/config.json "$@"
