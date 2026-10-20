# The groove box firmware (design)

Our own firmware for the SMC-PAD: a 16-track MIDI step sequencer in the spirit of the
Digitakt and the BeatStep Pro, with a slimmed-down stock controller as its PLAY mode. It is
an Arduino sketch on the core (`arduino/sketches/GrooveBox`), so it builds and uploads like
any other. Agreed with the user on 2026-10-20.

The pad has no display: 16 RGB pads, 10 single-colour button LEDs, 8 endless encoders without
push. The interface rule: **few modes, each recognisable by its colours; any value a knob
changes is shown on the pads while it changes.**

## Model

- **Pattern**: 16 tracks, one per pad. A track is a voice: MIDI channel, note (drum) or
  per-step notes (melodic), length 1-64 steps (pages of 16), speed (1/2x, 1x, 2x), swing.
  Tracks of different lengths run against each other (polymeter).
- **Step**: on/off, velocity, micro-timing (-11..+11 of 24 per step), probability (%) or
  condition (1:2, 2:2, 1:4.. 4:4, fill, not fill), ratchet (1-4 hits), note (melodic tracks),
  up to 4 **p-locks** (a CC value for that step only, on the track's 4 lockable CCs).
- **16 patterns** per project, in flash. Switching is queued to the end of the current
  pattern; patterns can be chained (A -> B -> C, looping).
- **Clock**: internal 30-300 BPM, or follow an incoming MIDI clock (USB / BLE) with
  start / stop / continue. As master the pad sends clock, start and stop to every port, so a
  DAW set to sync to it (Ableton: Sync on the SMC-PAD input) starts and stops with PLAY.

## Modes (the colours tell where you are)

| Mode | Enter | The pads are |
|---|---|---|
| **PLAY** (default) | PAD BANK off | the instruments: play, live record, note repeat, aftertouch |
| **STEP** | PAD BANK (LED on) | the 16 steps of the selected track's page; white playhead; `<` `>` page |
| **MUTE** | SHIFT + PAD BANK | the 16 tracks: tap = mute / unmute |
| **PATTERN** | SHIFT + KNOB BANK | the 16 patterns: tap = queue, hold one + tap others = chain |

Everywhere: **SHIFT + pad** selects the track. **PLAY** starts / stops, **STOP** (pause button)
stops and rewinds, **REC** arms live recording, SHIFT + REC: clear the selected track
(hold 1 s). **SHIFT + NOTE REPEAT**: note repeat settings (as stock). **BT** long press:
Bluetooth on / off.

PLAY mode is the stock controller, slimmed down: pads with page / transpose and velocity
curves, note repeat (rate, swing, latch, sync, tap tempo, pressure-following), polyphonic
aftertouch, knob page B as 8 CCs. No DAW (Mackie) preset and no user presets: the
`SMCPadStock` sketch stays for that.

## Knobs

- **Page A** (KNOB BANK off): the selected track: note, velocity, gate length, track length,
  speed, swing, euclidean hits, euclidean rotation.
- **Page B** (KNOB BANK on): 8 performance CCs (recorded as p-locks while live recording).
- **A step held** (STEP mode): velocity, micro-timing, probability / condition, ratchet,
  then the track's 4 p-lock CCs.
- Turning a knob shows its value on the pads for about a second (a 16-pad bar, or the
  choice among up to 16 options), then the mode's view comes back.

## Pressure

- PLAY: polyphonic aftertouch; note repeat velocity follows the pressure.
- Live recording: the pressure of a held pad is recorded as p-locks of the track's
  pressure CC (one of its 4).
- STEP: how hard a step is pressed sets its velocity.

## Colours

- 16 track colours (a fixed palette); a muted track: its colour at 1/16.
- STEP: step on = track colour, with p-locks = lighter tint, off = dark, playhead = white,
  condition / probability step = pulsing.
- PATTERN: stored = dim, playing = bright, queued = blinking, empty = off.

## Storage

- **USRFLASH, flash 0x77000-0x7E000 (28 KB, 7 sectors)**: above the VM, so stored plain (not
  SFC-encrypted); the stock firmware kept its presets there. BTIF (0x75000, BT pairings),
  USRTRIM (0x7E000) and key_mac (0x7F000) are left alone; the bootloader is nowhere near.
- Patterns are stored sparse (only active steps and p-locks): ~0.5-1.5 KB each. A small log
  structure over the 7 sectors gives wear levelling and power-cut safety (write new copy,
  then mark the old one stale; erase a sector only when all its records are stale).
- Settings (tempo, PLAY mode options, last pattern) stay in EEPROM (VM).
- RAM: the playing pattern, the queued one and the edit buffer; ~10 KB.

## Timing

- Sequencer clock at 96 PPQN from TIMER3 (`micros()`), 24 PPQN in / out for MIDI clock.
- External clock: phase-locked to incoming 0xF8 (interval smoothed); start / continue /
  stop and song position pointer followed.
- Outgoing notes are scheduled with their note-off (gate); all ports get the same events.

## First playable release (the user's pick)

1. Step edit, PLAY / STOP, internal and external clock, clock out.
2. Live recording (quantised to the step, micro-timing kept).
3. P-locks, probability / conditions, ratchets.
4. Patterns, chaining, saving in USRFLASH.

Later: melodic tracks with a scale keyboard, euclidean / random generators, song mode, the
browser editor (WebMIDI: backup, names, track setup), DIN MIDI out.

## Runtime additions needed

- Raw flash for the USRFLASH window only: `smcpad_flash_erase(sector)`,
  `smcpad_flash_write(addr, data, len)`, `smcpad_flash_read(...)`, refusing anything outside
  0x77000-0x7E000.
- Nothing else: MIDI in/out, clock, pads, storage and BLE are there.
