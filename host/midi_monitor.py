"""Print what the pad sends on USB-MIDI, with names; optionally drive it.

usage: midi_monitor.py [seconds] [--clock BPM] [--mackie]
  --clock BPM  send MIDI clock (start + 24 ppqn) to the pad, for note repeat sync
  --mackie     light the DAW preset's pads like a DAW would (select / mute on track 1)
"""
import sys
import time

import mido

NAMES = {0x80: "note off", 0x90: "note on", 0xA0: "poly AT", 0xB0: "CC", 0xC0: "program",
         0xD0: "chan AT", 0xE0: "pitch"}


def main():
    args = sys.argv[1:]
    secs = float(args[0]) if args and not args[0].startswith("--") else 60
    port = next((n for n in mido.get_input_names() if "SMC" in n), None)
    if not port:
        sys.exit(f"no SMC-PAD MIDI port in {mido.get_input_names()}")
    out = mido.open_output(next(n for n in mido.get_output_names() if "SMC" in n))
    if "--mackie" in args:
        out.send(mido.Message("note_on", note=0x18, velocity=127))
        out.send(mido.Message("note_on", note=0x10, velocity=127))
    bpm = float(args[args.index("--clock") + 1]) if "--clock" in args else 0
    print(f"listening on {port} for {secs:.0f} s" + (f", clock {bpm} BPM" if bpm else ""), flush=True)
    end = time.time() + secs
    next_clock = time.time()
    if bpm:
        out.send(mido.Message("start"))
    with mido.open_input(port) as inp:
        t0 = time.time()
        while time.time() < end:
            if bpm and time.time() >= next_clock:
                out.send(mido.Message("clock"))
                next_clock += 60 / bpm / 24
            for m in inp.iter_pending():
                b = m.bytes()
                kind = NAMES.get(b[0] & 0xF0, m.type)
                print(f"{time.time() - t0:8.3f}  {kind:9s} ch{(b[0] & 15) + 1:<2d} " +
                      " ".join(str(x) for x in b[1:]), flush=True)
            time.sleep(0.0005)
    if bpm:
        out.send(mido.Message("stop"))


if __name__ == "__main__":
    main()
