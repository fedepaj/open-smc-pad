"""The guide's animations and pictures, made by running GrooveBox in the SMC-PAD emulator.

    uv run --with pillow python tools/make_images.py [name ...]     # all, or some of them

Every picture is a scenario: gestures on the pad, captions. The emulator compiles the real
sketch (../GrooveBox), so the pictures follow the firmware; rerun after changing it.
The emulator lives in the Arduino core repository (SMCPAD_SIM, default ../arduino/sim).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SIM = os.environ.get("SMCPAD_SIM", os.path.join(ROOT, "..", "arduino", "sim"))
sys.path.insert(0, SIM)
from scenario import BUTTON, Scenario  # noqa: E402

SKETCH = os.path.join(ROOT, "GrooveBox")
OUT = os.path.join(ROOT, "docs", "img")
SCALE = 0.72
FPS = 20


def step_pad(n):
    """STEP mode shows steps in reading order: step 1 top-left .. step 16 bottom-right."""
    n -= 1
    return (3 - n // 4) * 4 + n % 4


def track_pad(track):
    """Tracks (and PLAY mode) count from the bottom-left: track 1 is pad 1."""
    return track - 1


def select_track(s, track):
    return s.press(BUTTON.SHIFT).wait(120).tap_pad(track_pad(track), hold=120, gap=60).release(BUTTON.SHIFT).wait(150)


def beat(s, quick=False):
    """A kick, a snare, closed hats: in STEP mode."""
    gap = 70 if quick else 110
    for n in (1, 5, 9, 13):
        s.tap_pad(step_pad(n), hold=110, gap=gap)
    select_track(s, 3)
    for n in (5, 13):
        s.tap_pad(step_pad(n), hold=110, gap=gap)
    select_track(s, 7)
    for n in (1, 3, 5, 7, 9, 11, 13, 15):
        s.tap_pad(step_pad(n), hold=90, gap=50 if quick else 80)
    return s


# ---------------------------------------------------------------- the pictures

def hero():
    s = Scenario()
    s.caption("STEP mode: tap the steps").tap(BUTTON.PAD_BANK)
    beat(s, quick=True)
    s.caption("PLAY").tap(BUTTON.PLAY).wait(2200)
    s.caption("back to PLAY mode: the pads flash with their tracks").tap(BUTTON.PAD_BANK).wait(2600)
    return s, "hero.gif", {}


def play_mode():
    s = Scenario()
    s.caption("PLAY mode: each pad is a track (a drum voice); hit it to play it").wait(300)
    for p, v in ((0, 110), (2, 90), (6, 70), (6, 50), (0, 120), (2, 100), (6, 60), (10, 90)):
        s.tap_pad(p, velocity=v, pressure=500 + v * 3, hold=120, gap=140)
    s.caption("the pad you hit is the selected track (brighter)").wait(900)
    return s, "play-mode.gif", {}


def step_mode():
    s = Scenario()
    s.caption("PAD BANK: STEP mode, the 16 steps of the selected track").tap(BUTTON.PAD_BANK).wait(400)
    s.caption("step 1 is top-left; tap to turn steps on").wait(200)
    for n in (1, 5, 9, 13):
        s.tap_pad(step_pad(n))
    s.caption("SHIFT + pad 3: the snare track").wait(100)
    select_track(s, 3)
    for n in (5, 13):
        s.tap_pad(step_pad(n))
    s.caption("PLAY: the white light is the playhead").tap(BUTTON.PLAY).wait(2400)
    s.caption("tap a lit step to turn it off").tap_pad(step_pad(13)).wait(1500)
    return s, "step-mode.gif", {}


def step_edit():
    s = Scenario()
    s.tap(BUTTON.PAD_BANK)
    for n in (1, 5, 9, 13):
        s.tap_pad(step_pad(n), hold=80, gap=60)
    s.tap(BUTTON.PLAY).wait(600)
    s.caption("hold a step + knob 4: a ratchet (2-4 hits in the step)").pad_down(step_pad(13))
    s.turn(3, 4, ms=600).pad_up(step_pad(13)).wait(1400)
    s.caption("hold a step + knob 3: how often it plays (probability, then 1:2, 1:4 ...)").pad_down(step_pad(9))
    s.turn(2, 6, ms=800).pad_up(step_pad(9)).wait(1600)
    s.caption("hold a step + knob 1: its velocity").pad_down(step_pad(5))
    s.turn(0, -12, ms=700).pad_up(step_pad(5)).wait(1200)
    return s, "step-edit.gif", {}


def live_record():
    s = Scenario()
    s.caption("REC, then PLAY: what you play is recorded, quantised").tap(BUTTON.REC)
    t0 = s.t                                        # PLAY: the sequencer starts here
    s.tap(BUTTON.PLAY)
    # 120 BPM: a kick on 1 and 3, a snare on 2 and 4, a bit early or late (kept as micro-timing)
    hits = [(0, 0), (2, 500), (0, 1000), (2, 1515), (0, 2000), (2, 2485), (0, 3000), (2, 3510)]
    for pad, at in sorted(hits, key=lambda h: h[1]):
        s.t = t0 + at
        s.tap_pad(pad, hold=90, gap=0)
    s.t = t0 + 4300
    s.caption("PAD BANK: the recorded steps of the selected track").tap(BUTTON.PAD_BANK).wait(2000)
    return s, "live-record.gif", {}


def mute_mode():
    s = Scenario()
    s.tap(BUTTON.PAD_BANK)
    beat(s, quick=True)
    s.tap(BUTTON.PLAY).wait(400)
    s.caption("SHIFT + PAD BANK: MUTE mode, one pad per track").press(BUTTON.SHIFT).wait(100)
    s.tap(BUTTON.PAD_BANK, gap=60).release(BUTTON.SHIFT).wait(1000)
    s.caption("tap a track to mute it, tap again to bring it back").tap_pad(track_pad(7)).wait(1300)
    s.tap_pad(track_pad(1)).wait(1200).tap_pad(track_pad(7)).tap_pad(track_pad(1)).wait(1000)
    return s, "mute-mode.gif", {}


def pattern_mode():
    s = Scenario()
    s.tap(BUTTON.PAD_BANK)
    for n in (1, 5, 9, 13):
        s.tap_pad(step_pad(n), hold=80, gap=60)
    s.caption("SHIFT + KNOB BANK: PATTERN mode, 16 patterns").press(BUTTON.SHIFT).wait(100)
    s.tap(BUTTON.KNOB_BANK, gap=60).release(BUTTON.SHIFT).wait(700)
    s.caption("SHIFT + a pad: copy this pattern there").press(BUTTON.SHIFT).wait(100).tap_pad(1).release(BUTTON.SHIFT)
    s.wait(700)
    s.caption("PLAY, then tap a pattern: it takes over at the end of the bar").tap(BUTTON.PLAY).wait(500)
    s.tap_pad(1).wait(2600)
    s.caption("hold one pattern and tap others: a chain").pad_down(0).wait(200).tap_pad(1).wait(100).pad_up(0)
    s.wait(3000)
    return s, "pattern-mode.gif", {}


def note_repeat():
    s = Scenario()
    s.caption("hold NOTE REPEAT and a pad: it repeats in time").press(BUTTON.NOTE_REPEAT).wait(200)
    s.pad_down(6, velocity=80, pressure=300)
    for p in (450, 650, 850, 950, 700, 400, 250):
        s.wait(300).pressure(6, p)
    s.caption("press harder: louder repeats").wait(300).pad_up(6).release(BUTTON.NOTE_REPEAT).wait(300)
    s.caption("SHIFT + NOTE REPEAT: rate, swing, latch, sync, tap tempo").press(BUTTON.SHIFT).wait(80)
    s.tap(BUTTON.NOTE_REPEAT, gap=60).release(BUTTON.SHIFT).wait(600)
    s.caption("pads 1-8 rate (1/4 .. 1/32T), 9-13 swing").tap_pad(6).wait(400).tap_pad(9).wait(900)
    s.tap(BUTTON.NOTE_REPEAT).wait(300)
    return s, "note-repeat.gif", {}


def euclid():
    s = Scenario()
    s.caption("STEP mode, knob 7: euclidean hits spread over the track").tap(BUTTON.PAD_BANK).wait(300)
    s.turn(6, 10, ms=1400).wait(500)
    s.caption("knob 8: rotate them").turn(7, 6, ms=1000).wait(400)
    s.tap(BUTTON.PLAY).wait(2200)
    return s, "euclid.gif", {}


def tempo():
    s = Scenario()
    s.caption("SHIFT + knob 1: tempo (the pads show the value)").press(BUTTON.SHIFT).wait(150)
    s.turn(0, 40, ms=1200).turn(0, -25, ms=900).release(BUTTON.SHIFT).wait(700)
    return s, "tempo.gif", {}


def stills():
    """One picture per mode."""
    out = []
    s = Scenario()
    s.tap(BUTTON.PAD_BANK)
    beat(s, quick=True)
    s.tap(BUTTON.PLAY).wait(1300)
    t_step = s.t
    s.tap(BUTTON.PAD_BANK).wait(900)
    t_play = s.t
    s.press(BUTTON.SHIFT).wait(80).tap(BUTTON.PAD_BANK, gap=40).release(BUTTON.SHIFT).tap_pad(6).wait(700)
    t_mute = s.t
    s.press(BUTTON.SHIFT).wait(80).tap(BUTTON.PAD_BANK, gap=40).release(BUTTON.SHIFT).wait(100)
    s.press(BUTTON.SHIFT).wait(80).tap(BUTTON.KNOB_BANK, gap=40).release(BUTTON.SHIFT).wait(700)
    t_pattern = s.t
    for name, t in (("mode-step.png", t_step), ("mode-play.png", t_play), ("mode-mute.png", t_mute),
                    ("mode-pattern.png", t_pattern)):
        out.append((s, name, {"at": t - 80}))
    return out


PICTURES = [hero, play_mode, step_mode, step_edit, live_record, mute_mode, pattern_mode, note_repeat,
            euclid, tempo]


def main():
    os.makedirs(OUT, exist_ok=True)
    wanted = set(sys.argv[1:])
    jobs = [p() for p in PICTURES] + stills()
    for s, name, opts in jobs:
        if wanted and os.path.splitext(name)[0] not in wanted:
            continue
        path = os.path.join(OUT, name)
        s.make(SKETCH, path, fps=FPS, scale=SCALE, **opts)
        print(f"{path}  {os.path.getsize(path) // 1024} KB")


if __name__ == "__main__":
    main()
