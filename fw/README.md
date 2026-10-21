# smc-pad-firmware

The low half of the [open SMC-PAD firmware](https://github.com/fedepaj/open-smc-pad): the
**USB DFU bootloader** that replaces the stock firmware for good, and the **firmware runtime**
(JieLi SDK + our drivers) that becomes the precompiled core of the
[Arduino core](https://github.com/fedepaj/smc-pad-arduino).

Most people never build this: the Arduino core ships the result, and releases carry ready
binaries. This is for changing the bootloader, the drivers or the runtime API.

| | |
|---|---|
| `boot/` | the bootloader (9 KB, bare metal, TinyUSB DFU class on our BR25 driver) and the stock → ours installer |
| `smcpad/arduino/` | the runtime: `smcpad_rt.h` (the API sketches build on), pads / keys / encoders / LEDs service, BLE GATT server, timing, storage |
| `smcpad/usb/` | our USB device driver for the BR25 (TinyUSB `dcd_br25.c`), CDC + MIDI descriptors |
| `smcpad/smcpad_flash.c` | flash erase / program with bounded waits (the SDK's own path can hang) |
| `smcpad/` (rest) | the JieLi AC63 SDK application, configured for the SMC-PAD (see [NOTICE](NOTICE)) |
| `build.sh` | builds the firmware in Docker |

## Flash layout

| flash | |
|---|---|
| `0x00000` | factory sector 0 (flash header, uboot, boot pointer): only the boot pointer is ever touched, by the installer |
| `0x01000` | **bootloader** (USB DFU `1209:0002`) |
| `0x09000` | **application**: the sketch linked with the core (416 KB) |
| `0x71000` | VM: settings, EEPROM (16 KB) |
| `0x75000` | factory: Bluetooth pairings, calibration, chip key |
| `0x77000` | USRFLASH (28 KB): sketch data, e.g. GrooveBox patterns |

The bootloader starts the application unless: it was asked to stay (1200-baud touch on the
serial port, as Arduino boards do), there is no valid application, the application's CRC fails,
it crashed four times in a row in its first three seconds, or **SHIFT + PLAY** are held at power
on. Then it is a DFU device: `dfu-util -d 1209:0002 -a 0 -D app.bin`. Alternate 1 reads flash,
alternate 2 takes a bootloader update (written to a staging area, verified, then installed by a
small installer, so a cut power never leaves the pad without one).

The full story, decisions and pitfalls: [docs/bootloader.md](https://github.com/fedepaj/open-smc-pad/blob/main/docs/bootloader.md),
[docs/flash-layout.md](https://github.com/fedepaj/open-smc-pad/blob/main/docs/flash-layout.md).

## Building

This repository is meant to sit at `fw/` in the [umbrella repository](https://github.com/fedepaj/open-smc-pad),
which brings the JieLi SDK, TinyUSB and the Docker file as submodules.

You need Docker (with linux/amd64 emulation on Apple silicon) and the JieLi **Linux toolchain and
post-build tools**, from [pkgman.jieliapp.com](http://pkgman.jieliapp.com/doc/all), unpacked in
`tools/jl-toolchain/jieli-linux-toolchains-*` and `tools/jl-postbuild/jieli-linux-post-build-tools-*`
(they cannot be redistributed).

```sh
fw/boot/build.sh                                                   # bootloader + installer
SMCPAD_ARDUINO=1 SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh   # the Arduino core object
arduino/make_system.sh                                             # -> the Arduino core's system/
```

- `TEXT_BASE=0x1E08120` links behind the bootloader (application at 0x9000).
- `SMCPAD_TINYUSB=1` uses TinyUSB and our driver instead of the SDK's USB stack.
- `SMCPAD_ARDUINO=1` leaves `setup()` / `loop()` undefined and saves the LTO output as
  `out/core/smcpad_core.o` with its linker script: the precompiled core.

Without `SMCPAD_ARDUINO` the build is the bring-up console firmware (`smcpad/examples/smcpad_test`):
a USB serial console for poking pins, pads, LEDs, flash and BLE, which is how the hardware was
mapped.

## License

Our code: MIT ([LICENSE](LICENSE)). The SDK-derived parts: Apache 2.0, see [NOTICE](NOTICE).
