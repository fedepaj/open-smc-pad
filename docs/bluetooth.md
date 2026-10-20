# Bluetooth: BLE-MIDI and the BLE runtime

## In the Arduino core (2026-10-20)

- **Runtime** `fw/smcpad/arduino/smcpad_ble.c` (API in `smcpad_rt.h`, `smcpad_ble_*`): a GATT
  server on the SDK stack. The caller gives the ATT database in BTstack's compiled format
  (entries `size, flags, handle, type (2/16 bytes), value`, no version byte, zero size ends it);
  dynamic attributes go to its read/write callbacks, which run in the BT stack's task. The
  CCCs (0x2902) are kept by the runtime (found by scanning the database), and a 0x2A05 Service
  Changed value, if present, is indicated on every connection (`ATT_OP_INDICATE`, range
  0x0001-0xFFFF). Local ATT MTU 247. It provides the hooks the SDK's BLE start calls
  (`bt_ble_init` & co., replacing `ble_hogp.c`); the console's BLE-MIDI now sits on it too.
- **Address = chip MAC with the low 3 bytes XORed with an FNV-1a hash of the database**
  (`adv_cfg_t.set_local_addr_tag = USE_SET_LOCAL_ADDRESS_TAG`). Why: macOS (CoreBluetooth)
  keeps the GATT database it first found for an address, and our first database had no Service
  Changed, so a new sketch's services were never seen (bleak listed only the old MIDI service,
  even after reconnecting). One address per database fixes it on any OS; sketches serving the
  same database (all BLEMIDI ones) keep their address and pairing.
- **ArduinoBLE** library (peripheral): GAP (name, appearance), GATT (Service Changed), then the
  sketch's services. Events are counted in the BT task and dispatched by `BLE.poll()`.
  Tested from macOS with bleak (`PadService` example): read, write, descriptor, notify.
- **BLEMIDI** library: on ArduinoBLE, MIDIUSB API. Tested with bleak (all pads, knobs as CC,
  host notes lighting pads) and with Audio MIDI Setup + CoreMIDI.

## The console firmware

Status (2026-10-20): **works end to end with macOS.** Scan (bleak): "SMC-PAD" with the MIDI
service UUID, RSSI -40. GATT: the MIDI characteristic with read / write-without-response /
notify, empty read; BLE-MIDI written to it is parsed on the pad. Audio MIDI Setup > Bluetooth
> Connect gives a CoreMIDI port "SMC-PAD Bluetooth"; with the console's `K` the pads arrive as
note on/off pairs, notes 36..51, velocity following the pressure (44 messages in 60 s of
playing, none lost). USB (CDC console) keeps working alongside.

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
