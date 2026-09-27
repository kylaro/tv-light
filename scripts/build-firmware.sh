#!/usr/bin/env bash
# Builds the Pico firmware natively (needs arm-none-eabi-gcc-cs-c++ + newlib on Fedora).
set -euo pipefail
cd "$(dirname "$0")/../firmware"
# pioasm in SDK 2.1.1 misses <cstdint>, which GCC 15+ no longer pulls in transitively.
export CXXFLAGS="${CXXFLAGS:-} -include cstdint"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build -j"$(nproc)"
mkdir -p prebuilt && cp build/tvlight.uf2 prebuilt/tvlight.uf2
echo "Firmware: $(pwd)/build/tvlight.uf2 (copied to firmware/prebuilt/)"
