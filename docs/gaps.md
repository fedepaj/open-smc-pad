# Open gaps

What is not done or not known yet (2026-10-21). The history of how the rest got solved is in the
other documents and in git.

## Before the first release

- **`cfg_tool.bin` comes from the stock dump.** `arduino/make_system.sh` (the app area template)
  and `host/package_fw.sh` take the configuration blob that follows `app.bin` out of
  `dumps/SMC-PAD_004_flash_t0.bin`. It must be generated from the SDK's own configuration
  (`cpu/br25/tools`, isd_config) so that no release file carries vendor data.
- **Release packages:** the install bundle (installer, bootloader area, a first firmware) for
  `install_from_stock.py --from <dir>` without a source build; the precompiled core for the
  Arduino platform; GrooveBox and SMCPadStock binaries.
- **Boards Manager package:** the JieLi toolchain cannot be redistributed, so the package must
  fetch it (pkgman.jieliapp.com) at install time; macOS needs blink (non-VFS build) and a
  Debian 9 root, today set up by hand.

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
- **Windows hosts:** dfu-util needs WinUSB for the bootloader; the JieLi toolchain has Windows
  builds, the Arduino recipe does not use them yet.

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
