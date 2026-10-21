# Open gaps

What is not done or not known yet (2026-10-21). The history of how the rest got solved is in the
other documents and in git.

## Before the first release

- **Release packages:** the install bundle (installer, bootloader area, a first firmware) for
  `install_from_stock.py --from <dir>` without a source build; the precompiled core for the
  Arduino platform; GrooveBox and SMCPadStock binaries.
- **Boards Manager:** in place for macOS, Linux and Windows (`.github/workflows/arduino.yml`,
  `arduino/release/`), released as v0.1.0 for macOS and Linux. The JieLi toolchain comes from JieLi's
  server (no license file: we may not host it; a weekly job checks the download); on Windows our
  `jieli_fetch.cmd` extracts it from JieLi's installer at the first build.
- **Windows uploads:** dfu-util needs the WinUSB driver for the bootloader (`1209:0002`). Until the
  bootloader carries Microsoft OS 2.0 descriptors (Windows then binds WinUSB by itself), it takes
  Zadig once. Not tried on a Windows machine with the pad yet.

## Firmware

- **MIDI OUT (3.5 mm):** UART1 on PC5 at 31250 baud, as `Serial1`; not wired up (no way to test it
  yet).
- **VM writes (EEPROM, settings) still use the SDK's flash path,** which can hang on this firmware
  (our USRFLASH API goes through `fw/smcpad/smcpad_flash.c` for that reason). They have worked in
  every test; moving `syscfg` onto our driver would remove the risk.
- **Out of memory:** the SDK's `malloc` spins (watchdog reset) instead of returning NULL.
- **USB 5 V is not visible to the chip** (separate charger): "charging" is approximated by "a USB
  host enumerated us"; the charge state itself is unknown.
- Optional **USB HID** (keyboard / mouse) per sketch: not done. Any change to the USB descriptors
  makes macOS on Apple silicon ask the user to allow the accessory again.

## Bootloader

- Power-cut tests during a bootloader self-update and during the install from stock (the
  windows are tens of ms, at the two sector-0 writes) have not been done deliberately.
- The flash's block-protection bits (chip ID 0xCD7013): semantics not confirmed; the installer
  clears them and reports what it saw.
- `install_from_stock.py` was last run end to end before the `jlfs.c` refactor of the
  bootloader: run it again on a stock pad before a release.

## GrooveBox

- Melodic tracks with a scale keyboard, song mode, the browser editor (WebMIDI: backup, names,
  track setup), SysEx pattern dump, MIDI OUT.

## Hardware still to confirm (docs/hardware.md)

- 595 /OE vs 4051 INH on PB5.
- MIDI OUT on PC5: which UART / output channel.
- What exactly the stock firmware's two extra ADC conversions after the 16 pads measured (ours:
  the bandgap reference and VBAT).
