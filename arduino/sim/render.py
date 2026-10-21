"""Draws the SMC-PAD from the emulator's frames, with fingers on what the scenario presses.

    from render import render
    render(frames, scenario, "out.gif")             # an animation
    render(frames, scenario, "out.png", at=1500)    # one still, at 1500 ms

`frames`: the emulator's JSON lines (see sim_runtime.cpp); `scenario`: a Scenario (its gestures
give the fingers, knob angles and captions). Needs Pillow.
"""
import json
import math

from PIL import Image, ImageDraw, ImageFilter, ImageFont

S = 2                                               # supersampling
W, H = 900, 560
# drawn as the pad looks in a dim room: the lit silicone is what you see
BACKGROUND = (250, 250, 251)
BODY = (36, 37, 42)
EDGE = (64, 66, 74)
PAD_OFF = (54, 56, 63)
KNOB = (18, 18, 21)
KNOB_RING = (70, 72, 80)
LABEL = (160, 162, 172)
BUTTON_FACE = (62, 64, 72)
SKIN = (233, 186, 160)

# button LED colours, panel order (docs/hardware.md)
BUTTON_LED = [(60, 110, 255), (255, 150, 30), (255, 150, 30), (255, 255, 255), (255, 255, 255),
              (40, 220, 80), (255, 50, 40), (255, 50, 40), (255, 255, 255), (255, 255, 255)]
BUTTON_NAME = ["BT", "PAD\nBANK", "KNOB\nBANK", "<", ">", "PLAY", "STOP", "REC", "SHIFT", "NOTE\nREPEAT"]


def _font(size):
    for name in ("/System/Library/Fonts/Helvetica.ttc", "/System/Library/Fonts/Supplemental/Arial.ttf",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"):
        try:
            return ImageFont.truetype(name, size * S)
        except OSError:
            pass
    return ImageFont.load_default()


# ---------------------------------------------------------------- geometry (unscaled)

def pad_box(i):
    """Pad i (0 = bottom-left, row by row upwards)."""
    col, row = i % 4, 3 - i // 4
    x, y = 270 + col * 112, 52 + row * 112
    return (x, y, x + 100, y + 100)


def knob_center(k):
    """Knob k (0..7: 1 bottom-left, 2 bottom-right, ... 8 top-right)."""
    col, row = k % 2, 3 - k // 2
    return (105 + col * 95, 112 + row * 112)


def button_box(b):
    y = 44 + b * 47
    return (768, y, 852, y + 39)


# ---------------------------------------------------------------- colours

def led_to_display(rgb):
    """What the eye sees through the white silicone: dim LED values look brighter than linear."""
    r, g, b = (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255
    peak = max(r, g, b)
    if peak == 0:
        return None, 0.0
    level = (peak / 255.0) ** 0.6
    return (int(r * 255 / peak), int(g * 255 / peak), int(b * 255 / peak)), level


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


# ---------------------------------------------------------------- drawing

def _scaled(box):
    return tuple(v * S for v in box)


def draw_frame(frame, state, caption=None):
    img = Image.new("RGB", (W * S, H * S + (60 * S if caption is not None else 0)), BACKGROUND)
    glow = Image.new("RGB", img.size, (0, 0, 0))
    d, g = ImageDraw.Draw(img), ImageDraw.Draw(glow)

    # body with a soft shadow
    shadow = Image.new("L", img.size, 0)
    ImageDraw.Draw(shadow).rounded_rectangle(_scaled((26, 30, W - 14, H - 10)), 30 * S, fill=70)
    shadow = shadow.filter(ImageFilter.GaussianBlur(10 * S))
    img.paste((150, 150, 150), (0, 0), shadow)
    d.rounded_rectangle(_scaled((20, 20, W - 20, H - 20)), 30 * S, fill=BODY, outline=EDGE, width=2 * S)

    small, tiny = _font(13), _font(10)
    # knobs
    for k in range(8):
        cx, cy = knob_center(k)
        d.text((cx * S, (cy - 44) * S), str(k + 1), fill=LABEL, font=small, anchor="mm")
        d.ellipse(_scaled((cx - 33, cy - 33, cx + 33, cy + 33)), fill=KNOB_RING)
        d.ellipse(_scaled((cx - 27, cy - 27, cx + 27, cy + 27)), fill=KNOB)
        a = math.radians(state["knob_angle"][k] - 90)
        d.line(_scaled((cx + 9 * math.cos(a), cy + 9 * math.sin(a), cx + 23 * math.cos(a), cy + 23 * math.sin(a))),
               fill=(235, 235, 235), width=4 * S)

    # pads
    for i in range(16):
        box = pad_box(i)
        color, level = led_to_display(frame["pads"][i])
        fill = PAD_OFF if color is None else mix(PAD_OFF, color, level)
        d.rounded_rectangle(_scaled(box), 9 * S, fill=fill, outline=(78, 80, 90), width=S)
        if color is not None and level > 0.25:
            g.rounded_rectangle(_scaled((box[0] - 6, box[1] - 6, box[2] + 6, box[3] + 6)), 14 * S,
                                fill=tuple(int(c * level ** 1.5 * 0.7) for c in color))
        d.text(((box[0] + 8) * S, (box[1] + 6) * S), str(i + 1), fill=(120, 122, 132), font=tiny)

    # buttons
    for b in range(10):
        box = button_box(b)
        lit = frame["leds"][b]
        face = mix(BUTTON_FACE, BUTTON_LED[b], 0.75) if lit else BUTTON_FACE
        d.rounded_rectangle(_scaled(box), 7 * S, fill=face, outline=(88, 90, 100), width=S)
        if lit:
            g.rounded_rectangle(_scaled((box[0] - 4, box[1] - 4, box[2] + 4, box[3] + 4)), 9 * S,
                                fill=tuple(int(c * 0.7) for c in BUTTON_LED[b]))
        cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        d.multiline_text((cx * S, cy * S), BUTTON_NAME[b], fill=(200, 202, 210) if not lit else (20, 20, 24),
                         font=tiny, anchor="mm", align="center", spacing=0)

    # light spilling out of the pads and buttons
    img = _add_light(img, glow.filter(ImageFilter.GaussianBlur(9 * S)))
    d = ImageDraw.Draw(img)

    # fingers
    d = ImageDraw.Draw(img, "RGBA")
    for i, pressure in state["pads_held"].items():
        _finger(d, img, pad_box(i), pressure)
    for b in state["buttons_held"]:
        _finger(d, img, button_box(b), 0.5, small=True)
    for k, direction in state["knobs_turning"].items():
        _knob_fingers(d, knob_center(k), direction)

    if caption is not None:
        d.text((W / 2 * S, (H + 22) * S), caption, fill=(40, 40, 46), font=_font(19), anchor="mm")
    return img.resize((img.width // S, img.height // S), Image.LANCZOS)


def _add_light(base, light):
    """Additive light, clamped."""
    from PIL import ImageChops
    return ImageChops.add(base, light)


def _finger(d, img, box, pressure, small=False):
    cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2 + (6 if small else 12)
    rx, ry = (16, 22) if small else (22 + 4 * pressure, 30 + 4 * pressure)
    # the finger comes from below: tip ellipse plus a shaft
    d.rounded_rectangle(_scaled((cx - rx, cy, cx + rx, cy + ry * 2.6)), int(rx * S), fill=SKIN + (150,))
    d.ellipse(_scaled((cx - rx, cy - ry, cx + rx, cy + ry)), fill=SKIN + (200,), outline=(190, 140, 115, 220),
              width=2 * S)
    d.ellipse(_scaled((cx - rx * 0.55, cy - ry * 0.75, cx + rx * 0.55, cy - ry * 0.05)), fill=(248, 225, 212, 170))


def _knob_fingers(d, c, direction):
    cx, cy = c
    for side in (-1, 1):
        x = cx + side * 30
        d.ellipse(_scaled((x - 12, cy - 18, x + 12, cy + 18)), fill=SKIN + (200,), outline=(190, 140, 115, 220),
                  width=2 * S)
    # a curved arrow: the direction of the turn
    start, end = (-140, -40) if direction > 0 else (-40, -140)
    d.arc(_scaled((cx - 44, cy - 44, cx + 44, cy + 44)), min(start, end), max(start, end), fill=(30, 120, 220, 230),
          width=4 * S)
    a = math.radians(end)
    tip = (cx + 44 * math.cos(a), cy + 44 * math.sin(a))
    back = math.radians(end - 14 * (1 if direction > 0 else -1))
    for off in (-0.25, 0.25):
        p = (cx + 34 * math.cos(back + off), cy + 34 * math.sin(back + off)) if off < 0 else \
            (cx + 54 * math.cos(back), cy + 54 * math.sin(back))
        d.line(_scaled(tip + p), fill=(30, 120, 220, 230), width=4 * S)


# ---------------------------------------------------------------- frames -> images

def load_frames(text):
    frames, midi = [], []
    for line in text.splitlines():
        o = json.loads(line)
        (frames if "pads" in o else midi).append(o)
    return frames, midi


def render(frames, scenario, out, at=None, scale=1.0):
    def image(f):
        img = draw_frame(f, scenario.state_at(f["t"]), scenario.caption_at(f["t"]))
        return img.resize((int(img.width * scale), int(img.height * scale)), Image.LANCZOS) if scale != 1 else img

    if at is not None:
        f = min(frames, key=lambda f: abs(f["t"] - at))
        image(f).save(out)
        return
    images = [image(f).convert("P", palette=Image.ADAPTIVE, colors=128) for f in frames]
    step = frames[1]["t"] - frames[0]["t"] if len(frames) > 1 else 40
    images[0].save(out, save_all=True, append_images=images[1:], duration=int(step), loop=0, optimize=True,
                   disposal=1)
