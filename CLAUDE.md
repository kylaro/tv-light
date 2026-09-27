# TV Light — ambient TV backlight with bass pumping

## Development style
- Development happens on Fedora Linux; the target is SteamOS (Game Mode via gamescope, Desktop Mode via KDE Plasma Wayland). Terminal commands must work on Fedora.
- C++ everywhere (host daemon C++20, Pico firmware C++17). No MicroPython.
- Prefer constexpr instead of #define.
- No magic numbers — always name a quantity.

## Layout
- `protocol/tvlight_protocol.h` — wire format shared by host and firmware. Change both sides together.
- `firmware/` — RP2040 firmware for the FireFly V1.1 board (`firmware/boards/firefly_v1_1.h`, schematic in ~/firefly-led-controller/FireFlyV1_1). Strip on LED_OUT_3 = GPIO16, status LED GPIO10. GPIO0-4 are board ID straps tied on the PCB: never drive them. Pico SDK 2.1.1 lives in `firmware/external/pico-sdk` (gitignored, copied from ~/macaroni-leds). Build natively on Fedora with `scripts/build-firmware.sh`.
- `host/` — `tvlightd` daemon: PipeWire screen capture (gamescope node or xdg-desktop-portal), PipeWire sink-monitor audio → bass envelope, LED color extraction, USB serial to the Pico, web UI on the LAN.
- `container/Containerfile` — build + runtime image for tvlightd (Fedora based). The host daemon is only ever built inside the container, so the Fedora dev box and the Steam Deck/Steam box run identical binaries.
- `setup.sh` — one-shot install/update (container image, udev rule, systemd user service, prints the web URL). Settings persist in ~/.config/tvlight/config.json.
- `scripts/` — build, run, flash, dev container, udev rule.
