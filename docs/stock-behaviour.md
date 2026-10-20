# What the stock firmware does

Sources: the user manual (`docs/vendor/SMC-PAD_user_manual.pdf`, local only: vendor files are git-ignored; EN section), the panel print, and the stock settings
area of the original flash dump (`dumps/original_physical_full.bin`, plain, not encrypted).
This is the spec for the "stock" Arduino sketch.

## From the manual

- **BT**: long press turns Bluetooth on/off; LED flashing = advertising, steady = connected.
- **PAD BANK / KNOB BANK**: second bank of pads / of knobs.
- **< / >**: previous / next group of eight tracks in the DAW (Mackie Control).
- **PLAY / STOP / REC**: DAW transport.
- **SHIFT + pads 1-8**: select preset 1-8 (1 = Performance, 2 = DAW, 3-8 user).
- **SHIFT + pads 9-12**: pad velocity curve (pad 12 = full velocity).
- **SHIFT + pads 13 / 14**: transpose down / up.
- **SHIFT + pads 15 / 16**: pad octave range down / up; **SHIFT + 15 + 16**: back to default.
- **Note repeat**: NOTE REPEAT then a pad, or a pad then NOTE REPEAT.
  **SHIFT + NOTE REPEAT** turns the pads into its settings:
  pads 1-8 rate 1/4, 1/4T, 1/8, 1/8T, 1/16, 1/16T, 1/32, 1/32T; pads 9-13 swing off, 1-4;
  pad 14 latch; pad 15 sync (external MIDI clock); pad 16 tap tempo (the pad flashes in time).
  **Holding NOTE REPEAT + knobs 1-4**: rate, swing, tempo (30-300 BPM), latch.
- Pads: velocity and aftertouch; send notes, CC or program change (set by the M-VAVE app).
  Knobs: CC, aftertouch or pitch bend (app).
- Outputs: USB, BLE-MIDI, 3.5 mm MIDI OUT (TRS). Low battery: < and > flash together.
  Charging LED (red / green) is hardware.

## From the settings area (flash 0x77000-0x7DE98)

Eight presets of **3539 bytes** each, back to back from 0x77000 (preset 1 = Performance,
preset 2 = DAW). 0x7E000: `trimdat` (16 x five 16-bit thresholds 220, 550, 720, 850, 950:
per-pad velocity calibration, same for every pad here).

Per preset (offsets in the preset):

- `0x00`: five transport button records, 23 bytes each:
  `type, ?, id, min, max, len, message[len]...` then padding and one byte.
  - Performance: type 2, ids 0x19-0x1D, message **MMC** `F0 7F 7F 06 cc F7`,
    cc = 01 stop, 02 play, 03 deferred play, 04 fast forward, 05 rewind.
  - DAW: type 5 (**note**), notes 0x2E, 0x2F, 0x5E, 0x5D, 0x5F = Mackie Control bank left,
    bank right, play, stop, record.
- then **16 knob records** (8 knobs x 2 knob banks), `type 2 (CC), ...`:
  - Performance: CC **30-45** (0x1E-0x2D), range 0-127, absolute.
  - DAW: the first bank `02 02 ch 1E..25 00 7F`: CC 30-37 with mode 2 (relative, as Mackie
    V-pots) on channels 1-8; second bank as Performance.
- `0xD4`: **pad records, 26 bytes each**: `09 (note on), note, min velocity, max velocity,
  R, G, B, FF, ...`. Eight groups of 16 (the pad octave positions): notes 4-19, 20-35,
  **36-51**, 52-67, 68-83, 84-99, 100-115, 116-; colours per group: lilac (96 C8 F0),
  yellow, magenta, blue, green, red, cyan, ... Default position: notes 36-51 (MPC).
  DAW preset: groups 3 and 8 hold other notes (0x18-0x1C...) with mixed colours: the
  last byte of some records (04) is probably a flag (toggle?); not decoded yet.

Not decoded yet: MIDI channel of the pads (0x09 may be note-on on channel 10, i.e. the
type byte is the status), what the trailing bytes of the pad records mean, the per-preset
note repeat / velocity curve / transpose state, the M-VAVE app's SysEx protocol.
