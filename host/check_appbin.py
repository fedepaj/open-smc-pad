#!/usr/bin/env python3
"""Sanity-check an app.bin against the crt0 that will load it.

app.bin is a plain concatenation (.text, .data, .data_code, overlays) and nothing in it
records where one part ends: the crt0 at the start of .text copies .data and .data_code
from *absolute flash addresses* baked in at link time.  So a packaging mistake (a missing
part, a stray alignment, the wrong order) produces a file that looks fine and a chip that
hangs before it can tell you anything.  This reads those addresses back out of the crt0
and checks they point where the parts actually landed in the file.

    python3 host/check_appbin.py [fw/smcpad/out/sdk.elf] [fw/smcpad/out/app.bin]

The pi32v2 crt0 starts with (see re/disasm/app004.dis for the stock one):
    sp = X / ssp = X / call ... / <zero .bss> / then, per copied section:
    c4 ff <u32 dest>      r4 = destination in RAM
    c1 ff <u32 src>       r1 = source, an absolute flash address
    c2 ff <u32 size>      r2 = byte count
"""
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEXT_VMA = 0x1E00120


def elf_sections(data):
    assert data[:4] == b"\x7fELF", "not an ELF"
    e_shoff, = struct.unpack_from("<I", data, 0x20)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from("<HHH", data, 0x2E)

    def entry(i):
        off = e_shoff + i * e_shentsize
        name, typ, flags, addr, foff, size = struct.unpack_from("<6I", data, off)
        return name, addr, foff, size

    _, _, stroff, _ = entry(e_shstrndx)

    def name_at(x):
        end = data.index(b"\0", stroff + x)
        return data[stroff + x:end].decode()

    out = {}
    for i in range(e_shnum):
        name, addr, foff, size = entry(i)
        out[name_at(name)] = (addr, size)
    return out


def crt0_copies(text, limit=0x80):
    """Yield (dest, src, size) triples from the leading load-immediate sequences.

    A copy is the exact run `r4 = dest` (c4 ff), `r1 = src` (c1 ff), `r2 = size` (c2 ff).
    The .bss clear just before it also ends in a `r2 = size`, but loads its destination
    into r3, so requiring the order keeps the two apart.
    """
    pos, dest, src = 0, None, None
    while pos < min(limit, len(text) - 6):
        op = text[pos:pos + 2]
        if op in (b"\xc4\xff", b"\xc1\xff", b"\xc2\xff", b"\xc3\xff"):
            value, = struct.unpack_from("<I", text, pos + 2)
            if op == b"\xc4\xff":
                dest, src = value, None
            elif op == b"\xc1\xff":
                src = value if dest is not None else None
            elif op == b"\xc2\xff" and dest is not None and src is not None:
                yield dest, src, value
                dest, src = None, None
            else:                                   # r3 = ... : the .bss clear, not a copy
                dest, src = None, None
            pos += 6
            continue
        pos += 2


def main():
    elf_path = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "fw/smcpad/out/sdk.elf"
    bin_path = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "fw/smcpad/out/app.bin"
    elf, app = elf_path.read_bytes(), bin_path.read_bytes()
    secs = elf_sections(elf)

    text_addr, text_size = secs[".text"]
    if text_addr != TEXT_VMA:
        print(f"!! .text is linked at {text_addr:#x}, expected {TEXT_VMA:#x}")
        return 1

    # where each part should sit in the file, in the order the post-build concatenates them
    offsets, cursor = {}, 0
    for name in (".text", ".data", ".data_code"):
        offsets[name] = cursor
        cursor += secs[name][1]

    print(f"{elf_path.name}: .text {text_size:#x} @ {text_addr:#x}, "
          f".data {secs['.data'][1]:#x}, .data_code {secs['.data_code'][1]:#x}")
    print(f"{bin_path.name}: {len(app):#x} bytes "
          f"({len(app) - cursor:#x} of overlays after .data_code)")

    want = {secs[n][0]: (n, TEXT_VMA + offsets[n], secs[n][1]) for n in (".data", ".data_code")}
    ok, seen = True, set()
    for dest, src, size in crt0_copies(app):
        if dest not in want:
            continue
        name, want_src, want_size = want[dest]
        seen.add(name)
        good = (src == want_src and size == want_size)
        ok &= good
        print(f"  crt0 copies {size:#8x} bytes to {dest:#x} from {src:#x} "
              f"-> {name} expects {want_size:#x} from {want_size and want_src:#x}  "
              f"{'OK' if good else 'MISMATCH'}")
    for name in want.values():
        if name[0] not in seen:
            print(f"  !! the crt0 never copies {name[0]} (destination not found)")
            ok = False

    print("app.bin is consistent with its crt0" if ok else "!! app.bin would not boot correctly")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
