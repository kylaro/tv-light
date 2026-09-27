#!/usr/bin/env bash
# Fast dev loop: build tvlightd inside the dev container (source bind mounted),
# then run it in the same container with access to PipeWire, D-Bus and /dev.
#   scripts/dev.sh build   # compile only
#   scripts/dev.sh run     # compile and run (Ctrl+C to stop)
#   scripts/dev.sh test    # compile and run unit tests
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE=tvlight-dev
BUILD_DIR=host/build-container
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/tvlight"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

if ! podman image exists "$IMAGE"; then
  podman build --target dev -t "$IMAGE" -f "$ROOT/container/Containerfile" "$ROOT"
fi
mkdir -p "$CONFIG_DIR"

in_container() {
  podman run --rm --init \
    --userns keep-id --security-opt label=disable \
    --network host \
    -v "$ROOT:/src" -w /src \
    -v "$RUNTIME_DIR:$RUNTIME_DIR" -e XDG_RUNTIME_DIR="$RUNTIME_DIR" \
    -e DBUS_SESSION_BUS_ADDRESS="unix:path=$RUNTIME_DIR/bus" \
    -v /dev:/dev:rslave \
    -v "$CONFIG_DIR:/config" \
    "$IMAGE" "$@"
}

build='cmake -S host -B '"$BUILD_DIR"' -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null && cmake --build '"$BUILD_DIR"
case "${1:-run}" in
  build) in_container bash -c "$build" ;;
  test) in_container bash -c "$build && $BUILD_DIR/dsp_test" ;;
  run) in_container bash -c "$build && exec $BUILD_DIR/tvlightd --config /config/config.json --web-dir /src/host/web" ;;
  *) echo "usage: $0 [build|run|test]"; exit 2 ;;
esac
