#!/usr/bin/env python3
"""
JieLi BR25 flash image tools for the SMC-PAD (JLFS2 "new firmware" layout).

Everything here was verified against the live board dump (dumps/SMC-PAD_004_flash_t0.bin)
and the vendor update SMC-PAD_005.fwsc.  See docs/flash-layout.md.

Layout of the 512 KB internal flash:

    0x00000  top area: flash header + uboot.boot + isd_config.ini   (encrypted, untouched)
    0x01000  app area: directory (9 entries x 32 B) + app.bin + cfg_tool.bin
    0x255fc  res area: config.dat + md5.bin
    0x26000  VM        (0x27000..0x75000 is erased / free)
    0x75000  BTIF, 0x76000 EXIF, 0x77000 USRFLASH, 0x7e000 USRTRIM, 0x7f000 key_mac

The app area (0x1000..0x26000) is stored SFC-encrypted with chipkey 0x980F; the SINCO
flash read (cmd 0x23) returns it already decrypted, so this module works in the plaintext
domain and only converts when asked (raw=the bytes physically in the flash cells).
"""

import argparse
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

AREA_BASE = 0x1000          # app area base = first SFC-mapped flash offset (VMA 0x1E00000)
AREA_VMA = 0x1E00000
CHIPKEY = 0x980F
ENC_END = 0x26000           # SFC encryption window is AREA_BASE..ENC_END
DIR_ENTRIES = 9             # the SMC-PAD directory holds 9 entries (0x120 bytes)
ENTRY_SIZE = 32


# --------------------------------------------------------------------------- CRC / cipher

def crc16(data, crc=0):
    """JieLi CRC16: CCITT polynomial 0x1021, init 0, not reflected."""
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def enc_cipher(buf, off, size, key=0xFFFF):
    """JieLi "ENC" stream cipher (LFSR keyed XOR), in place."""
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def sfc_cipher(buf, off, size, base=AREA_BASE, key=CHIPKEY, blocksize=32):
    """SFC flash (de)cipher, in place.  Symmetric: same call encrypts and decrypts."""
    for i in range(0, size, blocksize):
        enc_cipher(buf, off + i, min(size - i, blocksize), key ^ ((off + i - base) >> 2))


def sfc_encrypt_at(data, flash_off, key=CHIPKEY):
    """Plaintext bytes meant for flash_off -> the bytes to physically store there.

    Symmetric, so it also decrypts.  The ENC block keys each 32-byte block with
    key ^ (addr >> 2) on the *mapped* address, i.e. the flash offset minus AREA_BASE.
    """
    out = bytearray(data)
    sfc_cipher(out, 0, len(out), base=AREA_BASE - flash_off, key=key)
    return bytes(out)


# --------------------------------------------------------------------------- directory

class Entry:
    """struct jlfs2_file_entry (32 bytes)."""

    __slots__ = ("hdr_crc", "data_crc", "offset", "size", "attr", "resvd", "index", "name", "tag")

    def __init__(self, raw=None):
        if raw is None:
            self.hdr_crc = self.data_crc = 0
            self.offset = self.size = 0
            self.attr = self.resvd = 0xFF
            self.index = 0
            self.name = b"\xff" * 12
            self.tag = b"\xff" * 4
            return
        (self.hdr_crc, self.data_crc, self.offset, self.size,
         self.attr, self.resvd, self.index) = struct.unpack_from("<HHIIBBH", raw)
        self.name = raw[16:28]
        self.tag = raw[28:32]

    def pack(self, fix_crc=True):
        body = struct.pack("<HIIBBH", self.data_crc, self.offset, self.size,
                           self.attr, self.resvd, self.index) + self.name + self.tag
        if fix_crc:
            self.hdr_crc = crc16(body)
        return struct.pack("<H", self.hdr_crc) + body

    @property
    def label(self):
        return (self.name + self.tag).split(b"\0")[0].decode("ascii", "replace")

    def __repr__(self):
        return (f"{self.label:14} off={self.offset:#010x} size={self.size:#08x} "
                f"attr={self.attr:#04x} resvd={self.resvd:#04x} idx={self.index} "
                f"hdr={self.hdr_crc:#06x} data={self.data_crc:#06x} tag={self.tag.hex()}")


def read_dir(image, base=0):
    return [Entry(image[base + i * ENTRY_SIZE: base + (i + 1) * ENTRY_SIZE])
            for i in range(DIR_ENTRIES)]


def build_app_area(app_bin, cfg_tool_bin, template_dir, entry_vma=None, vm=None, prct_size=None):
    """Build a complete app area (directory + app.bin + cfg_tool.bin), plaintext.

    template_dir: the 9 entries of an existing directory; the special-area entries
    (VM/PRCT/BTIF/...) are copied verbatim, the file entries are recomputed.
    entry_vma: value of the app_area_head offset field (the entry point the uboot jumps to);
    defaults to the template's.
    """
    head, app_e, cfg_e = (Entry(e.pack(False)) for e in template_dir[:3])

    app_off = 0x120                                   # app.bin right after the directory
    cfg_off = app_off + len(app_bin)
    if cfg_off % 4:                                   # entries are word aligned in the vendor image
        cfg_off += -cfg_off % 4
    total = cfg_off + len(cfg_tool_bin)

    app_e.offset, app_e.size, app_e.data_crc = app_off, len(app_bin), crc16(app_bin)
    cfg_e.offset, cfg_e.size, cfg_e.data_crc = cfg_off, len(cfg_tool_bin), crc16(cfg_tool_bin)
    head.offset = head.offset if entry_vma is None else entry_vma
    head.size = total

    # The firmware derives the SFC encryption window from the VM entry: everything below
    # vm_saddr is encrypted (verified in the disassembly, docs/flash-layout.md).  A slot in
    # free flash must therefore sit *below* the VM start, or its code is fetched as-is.
    specials = [Entry(e.pack(False)) for e in template_dir[3:]]
    if vm is not None:
        specials[0].offset, specials[0].size = vm
    if prct_size is not None:
        specials[1].size = prct_size

    area = bytearray(b"\xff" * total)
    area[0x00:0x20] = head.pack()                     # placeholder, data_crc fixed below
    area[0x20:0x40] = app_e.pack()
    area[0x40:0x60] = cfg_e.pack()
    for i, e in enumerate(specials, start=3):
        area[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE] = e.pack(vm is not None or prct_size is not None)
    area[app_off:app_off + len(app_bin)] = app_bin
    area[cfg_off:cfg_off + len(cfg_tool_bin)] = cfg_tool_bin

    head.data_crc = crc16(area[0x20:total])           # everything after its own entry
    area[0x00:0x20] = head.pack()
    return bytes(area)


# --------------------------------------------------------------------------- commands

def cmd_info(args):
    img = Path(args.image).read_bytes()
    base = args.base
    print(f"{args.image}: {len(img)} bytes, directory at {base:#x}")
    for i, e in enumerate(read_dir(img, base)):
        ok = crc16(e.pack(False)[2:]) == e.hdr_crc
        extra = ""
        if e.label in ("app.bin", "cfg_tool.bin"):
            st = base + e.offset
            extra = " data_crc OK" if crc16(img[st:st + e.size]) == e.data_crc else " data_crc BAD"
        elif e.label == "app_area_head":
            got = crc16(img[base + 0x20:base + e.size])
            extra = " data_crc OK" if got == e.data_crc else f" data_crc BAD (got {got:#06x})"
        print(f"  {i} {e}  hdr {'OK' if ok else 'BAD'}{extra}")


def cmd_selftest(args):
    """Rebuild the stock app area out of its own pieces and require byte equality."""
    dump = Path(args.dump).read_bytes()
    entries = read_dir(dump, AREA_BASE)
    app = dump[AREA_BASE + entries[1].offset: AREA_BASE + entries[1].offset + entries[1].size]
    cfg = dump[AREA_BASE + entries[2].offset: AREA_BASE + entries[2].offset + entries[2].size]
    area = build_app_area(app, cfg, entries)
    orig = dump[AREA_BASE: AREA_BASE + entries[0].size]
    ok = area == orig
    print(f"rebuilt app area: {len(area):#x} bytes, original {len(orig):#x} bytes -> "
          f"{'IDENTICAL' if ok else 'MISMATCH'}")
    if not ok:
        for i in range(min(len(area), len(orig))):
            if area[i] != orig[i]:
                print(f"  first difference at area offset {i:#x} "
                      f"(flash {AREA_BASE + i:#x}): built {area[i]:#04x} orig {orig[i]:#04x}")
                break
        return 1

    # the SFC cipher must also reproduce the bytes physically in flash
    raw = bytearray(dump)
    sfc_cipher(raw, AREA_BASE, 0x120)
    vendor_raw = Path(args.vendor_raw).read_bytes() if args.vendor_raw else None
    if vendor_raw:
        same = [i for i in range(DIR_ENTRIES)
                if dump[AREA_BASE + i * 32: AREA_BASE + (i + 1) * 32] ==
                Path(args.vendor_dec).read_bytes()[AREA_BASE + i * 32: AREA_BASE + (i + 1) * 32]]
        good = all(bytes(raw[AREA_BASE + i * 32:AREA_BASE + (i + 1) * 32]) ==
                   vendor_raw[AREA_BASE + i * 32:AREA_BASE + (i + 1) * 32] for i in same)
        print(f"SFC cipher check on {len(same)} identical directory entries: "
              f"{'OK' if good else 'FAILED'}")
        if not good:
            return 1
    return 0


def cmd_build(args):
    dump = Path(args.dump).read_bytes()
    entries = read_dir(dump, AREA_BASE)
    app = Path(args.app).read_bytes()
    cfg = (Path(args.cfg).read_bytes() if args.cfg else
           dump[AREA_BASE + entries[2].offset: AREA_BASE + entries[2].offset + entries[2].size])

    # The entry point is where app.bin lands once the area sits at --area-base.
    entry = args.entry
    if entry is None and args.area_base is not None:
        entry = AREA_VMA + args.area_base + 0x120 - AREA_BASE
    vm = (args.vm_base, args.vm_size) if args.vm_base is not None else None
    area = build_app_area(app, cfg, entries, entry_vma=entry, vm=vm, prct_size=args.prct_size)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(area)
    head = read_dir(area)[0]
    base = args.area_base if args.area_base is not None else AREA_BASE
    print(f"app area written to {out}")
    print(f"  app.bin      {len(app):#x} bytes, crc {crc16(app):#06x}")
    print(f"  cfg_tool.bin {len(cfg):#x} bytes, crc {crc16(cfg):#06x}")
    print(f"  area size    {head.size:#x} (flash {base:#x}..{base + head.size:#x})")
    print(f"  entry point  {head.offset:#010x}")
    print(f"  sectors      {base // 0x1000:#x}..{(base + head.size - 1) // 0x1000:#x} "
          f"({(base + head.size - 1) // 0x1000 - base // 0x1000 + 1} to erase)")

    for i, e in enumerate(read_dir(area)):
        if e.label in ("VM", "PRCT"):
            print(f"  {e.label:12} off={e.offset:#x} size={e.size:#x}")
    if args.raw:
        Path(args.raw).write_bytes(sfc_encrypt_at(area, base))
        print(f"  raw (SFC-encrypted for flash {base:#x}) written to {args.raw}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("info", help="dump the directory of a flash image")
    p.add_argument("image")
    p.add_argument("--base", type=lambda s: int(s, 0), default=AREA_BASE)
    p.set_defaults(func=cmd_info)

    p = sub.add_parser("selftest", help="rebuild the stock area and compare byte for byte")
    p.add_argument("--dump", default=str(ROOT / "dumps/SMC-PAD_004_flash_t0.bin"))
    p.add_argument("--vendor-raw", default=str(ROOT / "work/fw/SMC-PAD_005/raw_components/flash.bin"))
    p.add_argument("--vendor-dec", default=str(ROOT / "work/fw/SMC-PAD_005/decrypted_flash.bin"))
    p.set_defaults(func=cmd_selftest)

    p = sub.add_parser("build", help="build an app area around our own app.bin")
    p.add_argument("--app", default=str(ROOT / "fw/smcpad/out/app.bin"))
    p.add_argument("--cfg", help="cfg_tool.bin (default: the one in the dump)")
    p.add_argument("--dump", default=str(ROOT / "dumps/SMC-PAD_004_flash_t0.bin"),
                   help="stock dump, used as the directory template")
    p.add_argument("--entry", type=lambda s: int(s, 0), help="entry point VMA (app_area_head offset)")
    p.add_argument("--area-base", type=lambda s: int(s, 0),
                   help="flash offset the area will be written to (default 0x1000, the stock "
                        "slot); the entry point is derived from it unless --entry is given")
    p.add_argument("--out", default=str(ROOT / "work/flash/app_area.bin"))
    p.add_argument("--vm-base", type=lambda s: int(s, 0),
                   help="move the VM area (its start is where the encryption window ends)")
    p.add_argument("--vm-size", type=lambda s: int(s, 0), default=0x4000)
    p.add_argument("--prct-size", type=lambda s: int(s, 0),
                   help="size of the PRCT (code) area declared in the directory")
    p.add_argument("--raw", help="also write the SFC-encrypted form, for --area-base")
    p.set_defaults(func=cmd_build)

    args = ap.parse_args()
    sys.exit(args.func(args) or 0)


if __name__ == "__main__":
    main()
