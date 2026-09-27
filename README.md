# tv-light

This is a vibecoded TV backlight for my living room, driven by a Linux PC.

The PC captures the screen, averages the colors around the edges, and sends them to an RP2040 LED controller over USB. It also listens to the audio going to the speakers, and the bass (<50 Hz) drives part of the brightness. A web page on the local network shows what it's doing and holds all the settings.

## Hardware
- A Linux PC. Tested on Fedora (GNOME). Intended for SteamOS: Game Mode captures through gamescope, Desktop Mode through the screen-share portal.
- A FireFly V1.1 controller (RP2040). The strip goes on `LED_OUT_3` (GPIO16), and the status LED is GPIO10. A plain Pico works too with `-DPICO_BOARD=pico` and the pins changed in `firmware/src/config.h`.
- A WS2812B strip with its own 5 V supply.

## Setup
Clone into your home directory, since that's what survives reboots and SteamOS updates:

```sh
cd ~
git clone https://github.com/kylaro/tv-light.git
cd tv-light
./setup.sh --flash    # hold BOOTSEL while plugging in the controller the very first time
```

`setup.sh` builds the container image, installs a udev rule (asks for sudo), installs and starts a systemd user service, and prints the address to open on your phone. Run it again after `git pull` to update. Leave out `--flash` if the controller already has current firmware. `./setup.sh --uninstall` removes the service.

In Desktop Mode, approve the screen-share dialog once; it's remembered after that.

## Settings
Everything is changed from the web page and saved to `~/.config/tvlight/config.json`, then loaded again on every start.

The LED level is `brightness × (always_on + bass_share × bass)`. The bass share, bass smoothing, low-pass cutoff and color smoothing are all sliders on the page.

## Development
```sh
scripts/build-firmware.sh   # needs arm-none-eabi-gcc; also refreshes firmware/prebuilt/tvlight.uf2
scripts/flash.sh            # reboots a running controller into BOOTSEL and copies the UF2
scripts/dev.sh run          # build + run the daemon from source in the dev container
scripts/dev.sh test         # DSP unit test
```

Layout: `protocol/` is the shared wire format, `firmware/` is the RP2040 code, `host/` is the daemon and web page, and `container/` holds the image.
