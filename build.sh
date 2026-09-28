#!/bin/sh
set -eu

: "${PICO_SDK_PATH:?Set PICO_SDK_PATH to Raspberry Pi Pico SDK 2.2.0 or later}"
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DPICO_PLATFORM=rp2350-arm-s \
  -DPICO_BOARD=adafruit_fruit_jam \
  -DHW_CONFIG=8 \
  -DENABLE_PIO_USB=0 \
  -DBUILD_FOR_BOOTLOADER=ON
cmake --build build --parallel
