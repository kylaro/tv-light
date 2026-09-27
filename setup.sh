#!/usr/bin/env bash
# One-shot setup for tvlightd on SteamOS or Fedora. Run as your normal user
# from the cloned repo (it asks for sudo once, for the USB udev rule).
#   ./setup.sh             install or update, then print the web UI address
#   ./setup.sh --flash     also flash the prebuilt firmware to the controller
#   ./setup.sh --uninstall remove the service (keeps your settings)
#
# Persistence: everything lives under $HOME (repo, container image, settings in
# ~/.config/tvlight/config.json) plus one udev rule in /etc, all of which
# survive reboots and SteamOS updates.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
SERVICE=tvlight.service
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
CONFIG_FILE="${XDG_CONFIG_HOME:-$HOME/.config}/tvlight/config.json"
UDEV_RULE=/etc/udev/rules.d/99-tvlight-pico.rules
DEFAULT_PORT=8080
START_TIMEOUT_S=60

say() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" -ne 0 ] || die "run as your normal user (not sudo); it asks for sudo when needed"
command -v podman >/dev/null || die "podman not found (SteamOS 3.5+ ships it; on Fedora: sudo dnf install podman)"
command -v systemctl >/dev/null || die "systemd is required"

if [ "${1:-}" = "--uninstall" ]; then
  systemctl --user disable --now "$SERVICE" 2>/dev/null || true
  rm -f "$UNIT_DIR/$SERVICE" "$APP_DIR/io.github.kylaro.TvLight.desktop"
  systemctl --user daemon-reload
  echo "Removed the service. Settings kept in $CONFIG_FILE."
  exit 0
fi

say "Building the tvlightd container image (first time takes a few minutes)"
"$ROOT/scripts/build-image.sh"

say "Installing the USB rule for the LED controller"
if [ -f "$UDEV_RULE" ] && cmp -s "$ROOT/scripts/99-tvlight-pico.rules" "$UDEV_RULE"; then
  echo "already installed"
elif sudo install -m 644 "$ROOT/scripts/99-tvlight-pico.rules" "$UDEV_RULE" &&
     sudo udevadm control --reload-rules && sudo udevadm trigger; then
  echo "installed $UDEV_RULE"
else
  echo "warning: could not install the udev rule; the serial port may not be accessible" >&2
fi

say "Installing the systemd user service"
mkdir -p "$UNIT_DIR" "$APP_DIR"
cat > "$UNIT_DIR/$SERVICE" <<UNIT
[Unit]
Description=TV Light ambient backlight daemon
After=pipewire.service
Wants=pipewire.service

[Service]
ExecStart=$ROOT/scripts/run.sh
ExecStop=/usr/bin/env podman stop -t 5 tvlight
Restart=always
RestartSec=3

[Install]
WantedBy=default.target
UNIT
# Gives xdg-desktop-portal an app id, so "allow screen sharing" is remembered.
cat > "$APP_DIR/io.github.kylaro.TvLight.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=TV Light
Exec=$ROOT/scripts/run.sh
NoDisplay=true
DESKTOP
systemctl --user daemon-reload
systemctl --user enable "$SERVICE" >/dev/null
systemctl --user restart "$SERVICE"
# Keep the service running across Game Mode <-> Desktop Mode switches and logouts.
loginctl enable-linger "$USER" 2>/dev/null || sudo loginctl enable-linger "$USER" ||
  echo "warning: could not enable linger; the service only runs while you are logged in" >&2

if [ "${1:-}" = "--flash" ]; then
  say "Flashing firmware"
  "$ROOT/scripts/flash.sh"
fi

say "Waiting for the web UI"
port=$DEFAULT_PORT
if [ -f "$CONFIG_FILE" ]; then
  port=$(sed -n 's/.*"web_port": *\([0-9]*\).*/\1/p' "$CONFIG_FILE" | head -1)
  port=${port:-$DEFAULT_PORT}
fi
for _ in $(seq "$START_TIMEOUT_S"); do
  curl -fs -o /dev/null "http://127.0.0.1:$port/api/state" && break
  sleep 1
done
curl -fs -o /dev/null "http://127.0.0.1:$port/api/state" ||
  die "tvlightd did not come up; check: journalctl --user -u tvlight -e"

echo
echo "TV Light is running. Open this on your phone (same Wi-Fi):"
ip -4 -o addr show scope global | awk -v p="$port" '{split($4, a, "/"); print "  http://" a[1] ":" p "/"}'
echo
echo "Settings are saved automatically to $CONFIG_FILE"
echo "Logs: journalctl --user -u tvlight -f"
echo "In Desktop Mode, approve the screen-share dialog once; Game Mode needs no prompt."
