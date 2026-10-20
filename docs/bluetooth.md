# Bluetooth: BLE-MIDI

Status (2026-10-20): the stack comes up and advertises; not yet tested from a central (needs
the macOS Bluetooth permission / Audio MIDI Setup, i.e. hands at the Mac).

The stock firmware does standard **BLE-MIDI** (its image carries the service
`03B80E5A-EDE8-4B33-A751-6CE34EC4C700` and characteristic
`7772E5DB-3868-4112-A1A9-F2669D106BF3`, plus BLE OTA files). Ours:
`fw/smcpad/examples/smcpad_test/smcpad_blemidi.c`, on the SDK's BLE stack (BTstack + the
`gatt_common` module, closed controller in `btctrler.a`).

- GATT (BTstack compiled format, written by hand): GAP service with the device name, the MIDI
  service with the MIDI I/O characteristic (read = empty, write without response, notify) and
  its CCC. Advertising: flags + the 128-bit MIDI service UUID; scan response: "SMC-PAD".
  "Just works" pairing with bonding.
- Outgoing: one MIDI message per packet, header + timestamp from `jiffies_msec()`
  (`blemidi_send`). Incoming: full parser (timestamps, running status, real time; SysEx
  skipped for now), complete messages to `blemidi_rx()`.
- Start: console `Y` -> `clk_set("sys", BT_NORMAL_HZ)` (24 MHz, already the boot clock),
  `bt_pll_para`, `btstack_ble_start_before_init`, `btstack_init`; `BT_STATUS_INIT_OK` reaches
  `app_idle.c`'s event handler -> `btstack_ble_start_after_init` -> `bt_ble_init` (profile,
  advertising, enable). It replaces `modules/bt/ble_hogp.c` (HID over GATT) in the build.
- Console `K`: pads -> notes 36..51 (stock pad numbering, bottom-left = 36) over BLE-MIDI,
  velocity from the ADC value, the pad LED lit while held.
- The application grows from ~92 KB to ~160 KB with the stack linked in. USB (TinyUSB CDC)
  and the console keep working with BLE running (selftest passes).

To test from a Mac: console `Y`, then Audio MIDI Setup > Window > Show MIDI Studio >
Bluetooth (Configure Bluetooth) > Connect "SMC-PAD"; then `K` and press pads with a MIDI
monitor open.
