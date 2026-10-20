# Arduino core for the SMC-PAD (work in progress)

## Use it (development install, macOS / Linux x86-64)

    SMCPAD_ARDUINO=1 SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh   # the precompiled core
    arduino/make_system.sh                                             # -> arduino/smcpad/system, tool links
    mkdir -p ~/Documents/Arduino/hardware/smcpad
    ln -sfn $PWD/arduino/smcpad ~/Documents/Arduino/hardware/smcpad/br25
    arduino-cli compile --fqbn smcpad:br25:smcpad MySketch
    arduino-cli upload  --fqbn smcpad:br25:smcpad -p /dev/cu.usbmodemXXXX MySketch

Board "M-Vave SMC-PAD (open firmware)" then also shows up in the Arduino IDE. On macOS the JieLi
tools run under blink (interpreter mode): a first build (core included) ~30 s, then ~7 s;
upload (1200-baud touch, dfu-util, restart) ~9 s. Needs `python3` and `dfu-util` on the PATH.

## How it is built

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

## What a sketch gets (2026-10-20)

- The Arduino API (ArduinoCore-API 1.5.2): `Serial` (USB CDC), digital I/O on pins 0-19
  (`variants/smcpad/pins_arduino.h`), `millis`/`delay`, `String`, C++ with constructors and
  `new`. The pads, keys, encoders and LEDs are serviced in the background from before `setup()`.
- Libraries in `smcpad/libraries` (examples in each):
  - **SMCPad** -- pads (velocity, pressure), 10 buttons, 8 knobs, button LEDs, RGB pad LEDs;
    callbacks or polling.
  - **MIDIUSB** -- Arduino's MIDIUSB API on the core's USB-MIDI interface.
  - **ArduinoBLE** -- the ArduinoBLE API, peripheral role (services, characteristics typed and
    string, descriptors, advertising, event handlers) on the runtime's GATT server
    (`fw/smcpad/arduino/smcpad_ble.c`, see `docs/bluetooth.md`). The central role (scan,
    connect) compiles but finds nothing.
  - **BLEMIDI** -- BLE-MIDI on ArduinoBLE with the MIDIUSB API (`sendMIDI`/`read`), plus
    `noteOn`/`noteOff`/`controlChange`; macOS Audio MIDI Setup connects to it.
- Every sketch carries the BLE stack (it is in the core object): ~148 KB of the 416 KB area.

Next: DIN MIDI on `Serial1` (UART1, PC5), optional USB HID, a real `realloc`, the test suite,
Windows toolchain, Boards Manager packaging.
