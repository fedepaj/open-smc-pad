"""Gestures on the SMC-PAD for the emulator, and running it: scenario -> frames -> GIF / PNG.

    from scenario import Scenario, BUTTON
    s = Scenario()
    s.caption("Hold SHIFT and tap a pad: that track is selected")
    s.press(BUTTON.SHIFT); s.tap_pad(4); s.release(BUTTON.SHIFT)
    s.make("path/to/sketch", "out.gif")
"""
import os
import subprocess
import sys
import tempfile
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import render  # noqa: E402

BUTTON = SimpleNamespace(BT=0, PAD_BANK=1, KNOB_BANK=2, LEFT=3, RIGHT=4, PLAY=5, STOP=6, REC=7, SHIFT=8,
                         NOTE_REPEAT=9)
KNOB_DEGREES = 15                                   # how far one detent turns the drawn knob


class Scenario:
    def __init__(self):
        self.t = 0
        self.lines = []
        self.captions = []                          # (from ms, text)
        self.pad_spans = []                         # (pad, from, to, pressure 0..1)
        self.button_spans = []                      # (button, from, to)
        self.knob_turns = []                        # (knob, from, to, delta)
        self._pad_down = {}
        self._button_down = {}

    # ---- time and words
    def wait(self, ms):
        self.t += ms
        return self

    def caption(self, text):
        self.captions.append((self.t, text))
        return self

    # ---- pads
    def pad_down(self, pad, velocity=100, pressure=700):
        self.lines.append(f"{self.t} pad {pad} down {velocity} {pressure}")
        self._pad_down[pad] = (self.t, pressure)
        return self

    def pad_up(self, pad):
        self.lines.append(f"{self.t} pad {pad} up")
        start, pressure = self._pad_down.pop(pad)
        self.pad_spans.append((pad, start, self.t, pressure / 1023))
        return self

    def tap_pad(self, pad, velocity=100, pressure=700, hold=140, gap=110):
        self.pad_down(pad, velocity, pressure).wait(hold).pad_up(pad).wait(gap)
        return self

    def pressure(self, pad, value):
        self.lines.append(f"{self.t} pressure {pad} {value}")
        return self

    # ---- buttons
    def press(self, button):
        self.lines.append(f"{self.t} button {button} down")
        self._button_down[button] = self.t
        return self

    def release(self, button):
        self.lines.append(f"{self.t} button {button} up")
        self.button_spans.append((button, self._button_down.pop(button), self.t))
        return self

    def tap(self, button, hold=140, gap=160):
        return self.press(button).wait(hold).release(button).wait(gap)

    # ---- knobs
    def turn(self, knob, delta, ms=500, gap=150):
        step = 1 if delta > 0 else -1
        for i in range(abs(delta)):
            self.lines.append(f"{self.t + ms * i // max(abs(delta), 1)} knob {knob} {step}")
        self.knob_turns.append((knob, self.t, self.t + ms, delta))
        return self.wait(ms + gap)

    # ---- MIDI from the computer
    def midi(self, *data):
        self.lines.append(f"{self.t} midi " + " ".join(f"{b:02X}" for b in data))
        return self

    # ---- what the renderer asks
    def state_at(self, t):
        pads = {p: pr for p, a, b, pr in self.pad_spans if a <= t < b}
        pads.update({p: pr / 1023 for p, (a, pr) in self._pad_down.items() if a <= t})
        buttons = {b for b, a, z in self.button_spans if a <= t < z}
        buttons |= {b for b, a in self._button_down.items() if a <= t}
        angle = [0.0] * 8
        turning = {}
        for k, a, z, delta in self.knob_turns:
            if t >= z:
                angle[k] += delta * KNOB_DEGREES
            elif t >= a:
                angle[k] += delta * KNOB_DEGREES * (t - a) / (z - a)
                turning[k] = delta
        return {"pads_held": pads, "buttons_held": buttons, "knobs_turning": turning, "knob_angle": angle}

    def caption_at(self, t):
        """None: no caption area at all; "" before the first caption (same frame size)."""
        if not self.captions:
            return None
        text = ""
        for start, words in self.captions:
            if start <= t:
                text = words
        return text

    def text(self):
        return "\n".join(self.lines) + "\n"

    # ---- run the firmware and draw
    def run(self, sketch, duration=None, fps=25):
        duration = duration or self.t + 200
        exe = build(sketch)
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(self.text())
        out = subprocess.run([exe, f.name, str(duration), str(fps)], capture_output=True, text=True, check=True)
        os.unlink(f.name)
        return render.load_frames(out.stdout)

    def make(self, sketch, out, duration=None, fps=25, at=None, scale=1.0):
        frames, midi = self.run(sketch, duration, fps)
        render.render(frames, self, out, at=at, scale=scale)
        return frames, midi


_built = {}


def build(sketch):
    """The emulator for a sketch directory (rebuilt once per run of the script)."""
    sketch = os.path.abspath(sketch)
    if sketch not in _built:
        exe = os.path.join(tempfile.gettempdir(), "smcpad-sim-" + os.path.basename(sketch))
        subprocess.run([os.path.join(HERE, "build.sh"), sketch, exe], check=True, capture_output=True)
        _built[sketch] = exe
    return _built[sketch]
