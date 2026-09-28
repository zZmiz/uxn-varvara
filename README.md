# Uxn / Varvara for Adafruit Fruit Jam

This is an RP2350 port of the maintained [Uxn2](https://git.sr.ht/~rabbits/uxn2)
Varvara runtime for the [Adafruit Fruit Jam](https://www.adafruit.com/product/6200).
It provides the System, Console, Screen, Audio, Controller, Mouse, File and
DateTime devices, an SD-card ROM browser, HDMI video and audio, USB input, and
the Fruit Jam controller ports.

## Build

Install an Arm GNU toolchain and CMake, then clone these dependencies:

```sh
git clone --branch 2.3.1 https://github.com/raspberrypi/pico-sdk.git
git -C pico-sdk submodule update --init
export PICO_SDK_PATH="$PWD/pico-sdk"
```

Build the bootloader-compatible UF2:

```sh
./build.sh
```

The result is `build/picoUxn.uf2`. Copy it into the Uxn entry used by
[pico-bootLoader](https://github.com/fhoedemakers/pico-bootLoader). Create
`/roms/UXN` on the SD card and place legally obtained `.rom` files there.
This repository does not include ROMs or game assets.

## Controls

- Controller directions and A, B, Select and Start map to Varvara controls.
- USB keyboards provide text and mapped controls.
- USB mice provide motion, three buttons and a vertical wheel.
- Start+Select opens the shared settings menu for reset, ROM selection or exit.

File access is restricted to the selected ROM's directory. DateTime returns
`2000-01-01 00:00:00` because Fruit Jam has no real-time clock. Console output
is available over UART0 on GPIO44/GPIO45 at 115200 8N1; stdin and ROM command
line arguments are not implemented. Screen dimensions are capped at 1024x768.

## Upstream and licenses

The port is released under the MIT license in [LICENSE](LICENSE). It includes:

- Uxn2 snapshot `8794ea557cdf282968d3e0fc93714840a42b245c` in
  `third_party/uxn2-upstream.c`, used as the VM and device reference. The ported
  runtime and platform integration are in `src/`.
- `fhoedemakers/pico_shared` at
  `b7a5747b8471bdffaf5021ffcf1ff48fb093d469`. Changes are limited to
  `drivers/pico_hdmi/hstx.c` and `.h` for atomic viewport buffers and audio-drop
  telemetry; `drivers/pico_audio_i2s/audio_i2s.c`, `.h` and `audio_clock.h` for
  fractional 44.1 kHz clocking and underrun/overflow telemetry; and removal of
  one path-normalization example comment in `ffwrappers.cpp` because it matched
  the release privacy scanner. `BoardConfigs.cmake` checks the SDK 2.3.1 Fruit
  Jam header path.
  `settings.h` and `settings.cpp` add the Uxn settings namespace.
- `fhoedemakers/pico_lib` at
  `35f24be6cf11e2e03d3e1b49b34b234c34a6b266`, vendored without changes.
- `Ryzee119/tusb_xinput` at
  `ae4f3a9fd7c57a6c8c36fdba7f7b1abc9b6a1a56`. Its CMake target links the
  Pico SDK `tinyusb_host` target, and its driver-open callback uses the current
  TinyUSB Boolean ABI.

The dependency license files remain in their respective directories. Uxn2's
license is in `third_party/UXN2-LICENSE`.
