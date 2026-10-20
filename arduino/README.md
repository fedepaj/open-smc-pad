# Arduino core for the SMC-PAD (work in progress)

Build model, verified on the pad (2026-10-20):

1. **Once, by us** -- `SMCPAD_ARDUINO=1 SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh`: the JieLi
   SDK + our runtime (`fw/smcpad/arduino`) go through the SDK's LTO link with `setup`/`loop`
   left undefined; the native object the LTO step saves (`sdk.elf.o`) is the precompiled core,
   `fw/smcpad/out/core/smcpad_core.o`, with the linker script next to it. The runtime API
   (`fw/smcpad/arduino/smcpad_rt.h`, listed in `api.symbols`) stays global because a small
   non-LTO object references it during that link; everything else of the SDK is internal, so
   its `delay` & co. cannot clash with Arduino's.
2. **Per sketch** -- compile the sketch with the JieLi clang (no LTO, `-mllvm -enable-ipra=false`),
   link it with `smcpad_core.o` + the SDK archives + newlib with plain `ld` (no plugin, a fraction
   of a second), make `app.bin`, package it as an app area for 0x9000, upload over DFU with the
   1200-baud touch. `proto/build_sketch.sh` does all of it for `proto/sketch.c`: the sketch prints
   on the USB serial; re-upload from a running sketch takes ~7 s.

Relinking the LTO output without the plugin reproduces `.text`/`.data`/`.data_code` of the
normal build byte for byte, which is what makes this sound.

Next: the Arduino API in C++ (`cores/`), C++ constructors via `.init_array` in the core's linker
script, platform.txt/boards.txt, the macOS toolchain under blink (interpreter mode is byte-exact),
libraries for the pads/LEDs/keys/encoders, MIDI (USB, BLE, DIN) and an ArduinoBLE-compatible BLE.
