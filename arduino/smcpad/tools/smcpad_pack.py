#!/usr/bin/env python3
"""SMC-PAD sketch packager: ELF -> app.bin -> app area for flash 0x9000 (what the bootloader's
DFU alt 0 takes). Pure Python, no JieLi tools.

    smcpad_pack.py <sketch.elf> <out.bin> [--template system/app_template.bin] [--size]

app.bin is what fw/build.sh makes: .text, .data, .data_code, then the overlays with their
trailing zero bytes removed (what JieLi's remove_tailing_zeros does) (the crt0 copies .data/.data_code from absolute flash addresses, so the
order and sizes must be exactly these). The app area is the JieLi directory (9 entries, the
special ones -- VM, PRCT, BTIF ... -- taken from the template), app.bin, cfg_tool.bin.
"""
import struct
import sys
from pathlib import Path

ENTRY = 0x1E08120                   # app.bin at flash 0x9120 (APP_DIR 0x9000)
AREA_MAX = 0x71000 - 0x9000
OVERLAYS = ["aec", "wav", "ape", "flac", "m4a", "amr", "dts", "fm", "mp3", "wma"]


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def elf_sections(elf):
    if elf[:4] != b"\x7fELF":
        sys.exit("not an ELF file")
    shoff, = struct.unpack_from("<I", elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", elf, 0x2E)

    def hdr(i):
        return struct.unpack_from("<IIIIII", elf, shoff + i * shentsize)

    strtab = hdr(shstrndx)[4]
    out = {}
    for i in range(shnum):
        name_off, typ, flags, addr, off, size = hdr(i)
        name = elf[strtab + name_off: elf.index(b"\0", strtab + name_off)].decode()
        data = b"" if typ == 8 else elf[off:off + size]     # SHT_NOBITS
        out[name] = (addr, data)
    return out


def app_bin(elf):
    s = elf_sections(elf)
    parts = [s[".text"][1], s[".data"][1], s[".data_code"][1]]
    for o in OVERLAYS:
        data = s.get(".overlay_" + o, (0, b""))[1]
        parts.append(data.rstrip(b"\0"))           # remove_tailing_zeros: trailing 0x00 only
    return b"".join(parts), s


def entry_pack(data_crc, offset, size, rest):
    body = struct.pack("<HII", data_crc, offset, size) + rest
    return struct.pack("<H", crc16(body)) + body


def app_area(app, template):
    """template: the 9 directory entries (0x120 bytes) + cfg_tool.bin"""
    dir_t, cfg = template[:0x120], template[0x120:]
    app_off = 0x120
    cfg_off = app_off + len(app)
    cfg_off += -cfg_off % 4
    total = cfg_off + len(cfg)
    area = bytearray(b"\xff" * total)
    area[0x40:0x120] = dir_t[0x40:0x120]                    # cfg entry placeholder + specials
    area[0x20:0x40] = entry_pack(crc16(app), app_off, len(app), dir_t[0x20 + 12:0x40])
    area[0x40:0x60] = entry_pack(crc16(cfg), cfg_off, len(cfg), dir_t[0x40 + 12:0x60])
    area[app_off:app_off + len(app)] = app
    area[cfg_off:cfg_off + len(cfg)] = cfg
    area[0x00:0x20] = entry_pack(crc16(area[0x20:total]), ENTRY, total, dir_t[12:0x20])
    return bytes(area)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) if "=" in a else (a[2:], "1") for a in sys.argv[1:] if a.startswith("--"))
    if len(args) != 2:
        sys.exit(__doc__)
    elf = Path(args[0]).read_bytes()
    app, sections = app_bin(elf)
    text_addr = sections[".text"][0]
    if text_addr != ENTRY:
        sys.exit(f".text is linked at {text_addr:#x}, the bootloader starts applications at {ENTRY:#x}")
    template = Path(opts.get("template", Path(__file__).parent.parent / "system/app_template.bin")).read_bytes()
    area = app_area(app, template)
    if len(area) > AREA_MAX:
        sys.exit(f"sketch too big: {len(area)} bytes, the application area holds {AREA_MAX}")
    Path(args[1]).write_bytes(area)
    if "size" in opts:
        ram = len(sections[".data"][1]) + len(sections.get(".bss", (0, b""))[1])
        print(f"Sketch uses {len(area)} bytes ({100 * len(area) // AREA_MAX}%) of program storage space. "
              f"Maximum is {AREA_MAX} bytes.")


if __name__ == "__main__":
    main()
