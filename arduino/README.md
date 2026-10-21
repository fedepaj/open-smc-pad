# smc-pad-arduino

**An Arduino core for the M-Vave SMC-PAD**: write sketches for its 16 RGB velocity pads,
8 knobs, 10 buttons, USB-MIDI and Bluetooth MIDI, and upload them over USB like any Arduino
board. Part of the [open SMC-PAD firmware](https://github.com/fedepaj/open-smc-pad).

<p align="center"><img src="https://raw.githubusercontent.com/fedepaj/groovebox/main/docs/img/play-mode.gif" width="560" alt="A sketch running on the SMC-PAD (in the emulator)"></p>

```cpp
#include <SMCPad.h>
#include <MIDIUSB.h>

void padPressed(int pad, int velocity) {
  MidiUSB.sendMIDI({0x09, 0x99, (uint8_t)(36 + pad), (uint8_t)velocity});
  SMCPad.padColor(pad, 0, velocity * 2, 0);
  SMCPad.show();
}

void padReleased(int pad) {
  MidiUSB.sendMIDI({0x08, 0x89, (uint8_t)(36 + pad), 0});
  SMCPad.padColor(pad, 0);
  SMCPad.show();
}

void setup() {
  SMCPad.onPadPress(padPressed);
  SMCPad.onPadRelease(padReleased);
}

void loop() {
  SMCPad.update();
}
```

## What you get

- **The Arduino API** (ArduinoCore-API 1.5): `Serial` on USB, `millis`/`micros` (µs resolution),
  `delay`, `String`, `snprintf` with floats, C++ with constructors, `new`, `random`.
- **The hardware, serviced in the background** from before `setup()`: pads scanned ~340 times a
  second with velocity and continuous pressure, buttons debounced, knobs decoded, LEDs multiplexed.
- **Libraries** (examples in each):

| library | |
|---|---|
| **SMCPad** | pads (velocity, pressure), buttons, knobs, button LEDs, RGB pad colours; callbacks or polling |
| **MIDIUSB** | Arduino's MIDIUSB API on the pad's USB-MIDI port |
| **BLEMIDI** | MIDI over Bluetooth LE with the same API (macOS, iOS, Android, Windows with a driver) |
| **ArduinoBLE** | the ArduinoBLE API, peripheral role: your own services and characteristics |
| **EEPROM** | 4 KB kept in flash across power-off and uploads (`get`/`put`/`commit`, as on ESP32) |

- **Sketches:** [`sketches/SMCPadStock`](sketches/SMCPadStock) behaves like the factory firmware
  (Performance and DAW/Mackie presets, note repeat, aftertouch …);
  [GrooveBox](https://github.com/fedepaj/groovebox) is a full step sequencer.
- **An emulator** ([`sim/`](sim)): any sketch runs on your computer with the real libraries, and
  scenarios of gestures turn into GIFs.
- **A self-test** ([`tests/CoreTest`](tests/CoreTest), `host/core_test.sh` in the umbrella):
  71 checks on the pad, from start-up and timing to flash and BLE.

### Numbering

- **Pads** 0–15, **0 = bottom-left**, row by row upwards (MPC style: note 36 + pad).
- **Buttons / LEDs** 0–9 in panel order: BT, PAD BANK, KNOB BANK, <, >, PLAY, STOP, REC, SHIFT,
  NOTE REPEAT.
- **Knobs** 0–7 = 1–8 as printed (1 bottom-left); clockwise counts up.

### Memory and limits

416 KB of flash for the sketch (every sketch carries the Bluetooth stack: ~145 KB to start
with), ~100 KB of heap, a 8 KB stack for `setup()`/`loop()`. When the heap runs out the system
`malloc` does not return NULL: it stops, and the watchdog restarts the pad. Pins: the 20 GPIOs
are all used by the hardware, so `digitalWrite` and friends exist but are of little use.
The 3.5 mm MIDI OUT is not driven yet.

## Install

Your pad needs the open firmware's bootloader:
[installing it](https://github.com/fedepaj/open-smc-pad#install) takes a USB cable and a few
minutes. Then uploads are like any native-USB Arduino: the IDE resets the pad into the bootloader
(1200-baud touch) and sends the sketch with `dfu-util`.

**Arduino IDE (Boards Manager)** — macOS or Linux x86-64:

1. *Settings → Additional boards manager URLs*:
   `https://github.com/fedepaj/open-smc-pad/releases/latest/download/package_smcpad_index.json`
2. *Boards Manager*: install **SMC-PAD (open firmware)**. It brings the JieLi compiler (downloaded
   from JieLi), our tools and dfu-util. On Apple silicon dfu-util runs under Rosetta.
3. Board **M-Vave SMC-PAD (open firmware)**, the pad's serial port, Upload.

With arduino-cli: `arduino-cli core install smcpad:br25 --additional-urls <the URL above>`.

**Development install** (to work on the core itself): the umbrella repository with
`fw/build.sh` and `arduino/make_system.sh` (it fills `smcpad/system/` and writes a
`platform.local.txt` pointing at local tools), then
`ln -s "$PWD/arduino/smcpad" ~/Documents/Arduino/hardware/smcpad/br25`. On macOS it needs
`blink` built in `work/blink-src` and a Linux root in `work/blink-root9`, as
`release/build_host_tools.sh` makes them.

Board **M-Vave SMC-PAD (open firmware)** then appears in the Arduino IDE, and
`arduino-cli compile --fqbn smcpad:br25:smcpad MySketch` / `arduino-cli upload ... -p <port>`
work. A first build takes ~30 s on macOS (the core), then ~7 s; an upload ~9 s.

If a sketch hangs or crashes at start, the bootloader notices (four failed starts) and waits
for an upload. You can also hold **SHIFT + PLAY** while switching the pad on.

## How it works

The JieLi SDK is closed in parts and built with LTO; mixing Arduino code into it per sketch
would mean a two-minute LTO link every time. Instead:

1. **Once**: the SDK plus our runtime (`fw/smcpad/arduino`, API in `smcpad_rt.h`) go through the
   SDK's LTO link with `setup`/`loop` left undefined. The native object the LTO step produces is
   the **precompiled core** (`system/smcpad_core.o`, with its linker script). The runtime API
   stays global; everything else of the SDK becomes internal, so its `delay` & co. cannot clash
   with Arduino's.
2. **Per sketch**: the sketch, the libraries and `cores/smcpad` are compiled with the JieLi clang
   (no LTO), linked with the precompiled core by plain `ld` in a fraction of a second, and
   packed (`tools/smcpad_pack.py`) as an application for the bootloader.

Relinking the LTO output without the plugin reproduces the normal build byte for byte, which is
what makes this sound. Details and the pitfalls met on the way are in the comments of
`platform.txt`, `tools/` and the runtime, and in the umbrella's [docs](https://github.com/fedepaj/open-smc-pad/tree/main/docs).

## License

MIT ([LICENSE](LICENSE)). ArduinoCore-API (`cores/smcpad/api`) is LGPL-2.1, from its own repository.
