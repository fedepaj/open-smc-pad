"""The README diagrams as SVG: architecture, install path, flash map.  python3 docs/img/diagrams.py"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = "font-family='-apple-system, Segoe UI, Helvetica, Arial, sans-serif'"
INK, MUTED = "#24262c", "#6b6f7a"


def svg(w, h, body):
    return (f"<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 {w} {h}' width='{w}' height='{h}' {FONT}>"
            f"<rect width='{w}' height='{h}' rx='14' fill='#fafafb'/>{body}</svg>\n")


def box(x, y, w, h, fill, title, sub="", title_color="#ffffff", sub_color="rgba(255,255,255,0.85)", r=10):
    t = (f"<rect x='{x}' y='{y}' width='{w}' height='{h}' rx='{r}' fill='{fill}'/>"
         f"<text x='{x + 16}' y='{y + (h / 2 if not sub else h / 2 - 6)}' dominant-baseline='middle' font-size='15' "
         f"font-weight='600' fill='{title_color}'>{title}</text>")
    if sub:
        t += (f"<text x='{x + 16}' y='{y + h / 2 + 13}' dominant-baseline='middle' font-size='12' "
              f"fill='{sub_color}'>{sub}</text>")
    return t


def arrow(x1, y1, x2, y2, color=MUTED, label=""):
    t = (f"<line x1='{x1}' y1='{y1}' x2='{x2}' y2='{y2}' stroke='{color}' stroke-width='2' "
         f"marker-end='url(#a)'/>")
    if label:
        t += (f"<text x='{(x1 + x2) / 2 + 8}' y='{(y1 + y2) / 2}' dominant-baseline='middle' font-size='12' "
              f"fill='{MUTED}'>{label}</text>")
    return t


MARKER = ("<defs><marker id='a' viewBox='0 0 10 10' refX='9' refY='5' markerWidth='7' markerHeight='7' "
          f"orient='auto-start-reverse'><path d='M0,0 L10,5 L0,10 z' fill='{MUTED}'/></marker></defs>")


def architecture():
    layers = [
        ("#ff5a36", "Sketches", "GrooveBox sequencer · SMCPadStock (the factory behaviour) · yours"),
        ("#f39c12", "Arduino core + libraries", "SMCPad · MIDIUSB · BLEMIDI · ArduinoBLE · EEPROM · Serial · String"),
        ("#2ecc71", "Runtime  (smcpad_rt.h)", "pads 340 Hz + pressure · knobs · LEDs · µs timing · USB-MIDI · BLE GATT · flash"),
        ("#1fa2c9", "JieLi SDK  +  TinyUSB", "OS, Bluetooth stack (precompiled core)  ·  our BR25 USB driver"),
        ("#5b6cf0", "Bootloader", "USB DFU · self-update · falls back to DFU after a crash loop"),
        ("#3b3e48", "SMC-PAD: JieLi AC6966C", "16 RGB pads · 8 encoders · 10 buttons · USB-C · Bluetooth 5 · MIDI out"),
    ]
    w, h = 900, 470
    body = MARKER
    y = 24
    for color, title, sub in layers:
        body += box(24, y, 600, 62, color, title, sub)
        y += 72
    # the computer side: one column, one link
    body += (f"<text x='660' y='34' font-size='12' font-weight='600' fill='{MUTED}' letter-spacing='1'>"
             "ON YOUR COMPUTER</text>")
    side = [("Arduino IDE / arduino-cli", "upload over DFU (1200-baud touch)"),
            ("Ableton Live · any DAW", "USB-MIDI · BLE-MIDI · MIDI clock"),
            ("SMC_PAD Live script", "Drum Rack colours, transport"),
            ("Emulator", "the same sketch, on the desk → GIFs")]
    y = 48
    for title, sub in side:
        body += (f"<rect x='660' y='{y}' width='216' height='72' rx='10' fill='#fff' stroke='#d9dbe1'/>"
                 f"<text x='676' y='{y + 30}' font-size='14' font-weight='600' fill='{INK}'>{title}</text>"
                 f"<text x='676' y='{y + 51}' font-size='11.5' fill='{MUTED}'>{sub}</text>")
        y += 86
    body += (f"<line x1='642' y1='60' x2='642' y2='380' stroke='#c4c7cf' stroke-width='2' stroke-dasharray='5 5'/>"
             f"<text x='660' y='420' font-size='11.5' fill='{MUTED}'>⟵  over USB-C and Bluetooth</text>")
    return svg(w, h, body)


def install():
    steps = [("#3b3e48", "Stock firmware", "as it ships"),
             ("#5b6cf0", "Installer", "runs once, from RAM"),
             ("#5b6cf0", "Bootloader", "USB DFU, for good"),
             ("#ff5a36", "Your firmware", "GrooveBox, any sketch")]
    w, h = 900, 210
    body = MARKER
    for i, (c, t, s) in enumerate(steps):
        body += box(24 + i * 222, 34, 186, 70, c, t, s)
    notes = ["install_from_stock.py writes the installer and the bootloader into free flash over USB-MIDI,"
             " then repoints the boot entry",
             "the installer copies the bootloader to 0x1000 and gives the boot entry back",
             "dfu-util / Arduino upload: from then on, no tools, no opening the case"]
    for i in range(3):
        body += arrow(212 + i * 222, 69, 242 + i * 222, 69)
    labels = ["1  USB-MIDI", "2  power cycle", "3  DFU upload"]
    for i, (lab, note) in enumerate(zip(labels, notes)):
        body += (f"<text x='24' y='{140 + i * 22}' font-size='12.5' fill='{INK}'>"
                 f"<tspan font-weight='600'>{lab}</tspan><tspan fill='{MUTED}'>  —  {note}</tspan></text>")
    return svg(w, h, body)


def flash_map():
    # (start, end, label, colour); drawn with a square-root scale so the small areas show
    parts = [(0x00000, 0x01000, "boot sector", "#3b3e48"), (0x01000, 0x09000, "bootloader", "#5b6cf0"),
             (0x09000, 0x71000, "application  416 KB", "#ff5a36"), (0x71000, 0x75000, "VM", "#2ecc71"),
             (0x75000, 0x77000, "BT, calib", "#8a8f9c"), (0x77000, 0x7E000, "USRFLASH", "#f39c12"),
             (0x7E000, 0x80000, "key", "#8a8f9c")]
    w, h = 900, 150
    total = sum((e - s) ** 0.5 for s, e, _, _ in parts)
    x = 24.0
    body = ""
    for s, e, label, c in parts:
        width = (e - s) ** 0.5 / total * (w - 48)
        body += f"<rect x='{x:.1f}' y='40' width='{width - 3:.1f}' height='46' rx='6' fill='{c}'/>"
        if width > 60:
            body += (f"<text x='{x + width / 2:.1f}' y='63' text-anchor='middle' dominant-baseline='middle' "
                     f"font-size='12.5' font-weight='600' fill='#fff'>{label}</text>")
        else:
            body += (f"<text x='{x + width / 2:.1f}' y='106' text-anchor='middle' font-size='11' "
                     f"fill='{MUTED}'>{label}</text>")
        if width > 60:
            body += (f"<text x='{x:.1f}' y='30' font-size='10.5' fill='{MUTED}'>0x{s:05X}</text>")
        x += width
    body += (f"<text x='24' y='136' font-size='12' fill='{MUTED}'>512 KB internal flash (square-root scale). "
             "The factory parts are never written; USRFLASH holds sketch data such as GrooveBox patterns.</text>")
    return svg(w, h, body)


for name, make in (("architecture.svg", architecture), ("install.svg", install), ("flash-map.svg", flash_map)):
    with open(os.path.join(HERE, name), "w") as f:
        f.write(make())
    print(name)
