#!/usr/bin/env python3
"""Write/erase SMC-PAD flash over USB-MIDI with the stock firmware's SINCO commands.

Companion to sinco_dump.py (which only reads via 0x23). This exercises the documented
but not-yet-verified write path:

    0x21  flash erase     payload  type u8, addr u32            (erases the 4KB sector at addr)
    0x22  flash write     payload  type u8, addr u32, len u24, data
    0x24  flash checksum   payload  type u8, addr u32, len u32

Everything is the user's own device + documented protocol (docs/sinco-protocol.md).
The firmware's acknowledgement format for 0x21/0x22 is not documented, so success is
judged by reading the bytes back with 0x23 (ground truth), not by the reply.

SAFETY
------
- `probe` is the safe entry point: it round-trips a pattern through one already-erased,
  non-code sector (default 0x76000) and restores it, so it cannot brick the board even
  if a command behaves unexpectedly. Run it first.
- Writing anywhere else requires --yes. Writing into the app/bootloader/key regions is
  refused unless --force is also given, because a bad write there needs the Pico USB_KEY
  recovery path. Keep a full backup (dumps/SMC-PAD_004_flash_t0.bin) handy regardless.

Usage:
    python sinco_flash.py probe [--addr 0x76000] [--type 0]
    python sinco_flash.py erase ADDR [--type 0] --yes
    python sinco_flash.py write ADDR FILE [--type 0] [--chunk 256] --yes [--force]
    python sinco_flash.py checksum ADDR LEN [--type 0]
"""
import argparse
import struct
import sys
import time

from sinco_dump import Link

SECTOR = 0x1000  # JieLi SFC erase granularity

# Regions a stray write must not hit without --force (from flash layout RE).
#   name, start, end(exclusive)
PROTECTED = [
    ("bootloader/app", 0x00000, 0x26000),
    ("key_mac",        0x7F000, 0x80000),
]
# Known-safe erased sectors for the round-trip probe (all 0xFF in the backup, not code).
SAFE_PROBE_SECTORS = (0x76000, 0x74000)


def is_protected(addr, length):
    for name, a, b in PROTECTED:
        if addr < b and addr + length > a:
            return name
    return None


class FlashLink(Link):
    def _ack(self, sent_cmd, timeout):
        """Return (ok, info) for the reply to an erase/write command.

        fw 004 does not echo the command: it answers with a generic status packet
        cmd=0x00, payload=[status], where status 0x00 means success. (Verified by the
        probe: 0x21/0x22 both reply 00:00 and the read-back confirms the effect.)
        """
        r = self.recv(timeout)
        if r is None:
            return None, "timeout"
        cmd, body, chk_ok = r
        status = body[0] if body else 0
        ok = bool(chk_ok and (cmd == 0x00 or cmd == sent_cmd) and status == 0)
        return ok, f"cmd={cmd:#04x} status={status:#04x}"

    def erase(self, ftype, addr, timeout=2.0):
        payload = bytes([ftype]) + struct.pack("<I", addr)
        self.drain()
        self.send(0x21, payload)
        return self._ack(0x21, timeout)

    def write(self, ftype, addr, data, timeout=2.0):
        payload = bytes([ftype]) + struct.pack("<I", addr) + struct.pack("<I", len(data))[:3] + data
        self.drain()
        self.send(0x22, payload)
        return self._ack(0x22, timeout)

    def checksum(self, ftype, addr, length, timeout=2.0):
        payload = bytes([ftype]) + struct.pack("<I", addr) + struct.pack("<I", length)
        self.drain()
        self.send(0x24, payload)
        r = self.recv(timeout)
        if r is None:
            return None, b""
        cmd, body, chk_ok = r
        return (cmd == 0x24 and chk_ok), body


def cmd_probe(link, args):
    addr = args.addr
    if addr % SECTOR:
        sys.exit(f"probe addr {addr:#x} must be 4KB-aligned")
    if addr not in SAFE_PROBE_SECTORS and not args.force:
        sys.exit(f"{addr:#x} is not a known-erased safe sector {SAFE_PROBE_SECTORS}; use --force to override")

    print(f"# probe sector {addr:#x}, type {args.type}")
    before = link.read(args.type, addr, 32)
    print(f"  read-back before : {before.hex(' ')}")
    if any(b != 0xFF for b in before):
        print("  WARNING: sector is not all-0xFF; aborting so nothing is lost. Pick an erased sector.")
        return

    ok, body = link.erase(args.type, addr)
    print(f"  0x21 erase       : {'ack' if ok else 'no/!=ack'}  reply={body!r}")

    pattern = bytes((0xA5 ^ i) & 0xFF for i in range(16))
    ok, body = link.write(args.type, addr, pattern)
    print(f"  0x22 write 16B   : {'ack' if ok else 'no/!=ack'}  reply={body!r}")

    after = link.read(args.type, addr, 32)
    print(f"  read-back after  : {after.hex(' ')}")
    wrote = after[:16] == pattern
    print(f"  => WRITE {'WORKS' if wrote else 'did NOT take (pattern not present)'}")

    # restore: erase back to 0xFF
    ok, _ = link.erase(args.type, addr)
    restored = link.read(args.type, addr, 32)
    clean = all(b == 0xFF for b in restored)
    print(f"  restore (erase)  : {'ok, sector back to 0xFF' if clean else 'NOT clean: ' + restored.hex(' ')}")
    if wrote and clean:
        print("\nResult: fw 004 implements 0x21 erase + 0x22 write for this type. Flash-write path is OPEN.")
    elif not wrote:
        print("\nResult: the pattern never appeared -> 0x21/0x22 are not honored for this type "
              "(try --type 1/2/3), or the write is silently dropped.")


def cmd_erase(link, args):
    if not args.yes:
        sys.exit("refusing to erase without --yes")
    p = is_protected(args.addr, SECTOR)
    if p and not args.force:
        sys.exit(f"{args.addr:#x} is in protected region '{p}'; use --force if you really mean it")
    ok, body = link.erase(args.type, args.addr)
    print(f"erase {args.addr:#x} type {args.type}: {'ack' if ok else 'no/!=ack'}  reply={body!r}")
    back = link.read(args.type, args.addr & ~(SECTOR - 1), 16)
    print(f"  sector head now: {back.hex(' ')}")


def cmd_write(link, args):
    if not args.yes:
        sys.exit("refusing to write without --yes")
    data = open(args.file, "rb").read()
    p = is_protected(args.addr, len(data))
    if p and not args.force:
        sys.exit(f"write to {args.addr:#x}+{len(data):#x} overlaps protected region '{p}'; "
                 f"use --force if you really mean it (needs Pico USB_KEY to recover if it goes wrong)")

    # erase every sector the image touches first
    first = args.addr & ~(SECTOR - 1)
    last = (args.addr + len(data) - 1) & ~(SECTOR - 1)
    for sec in range(first, last + 1, SECTOR):
        ok, body = link.erase(args.type, sec)
        if not ok:
            print(f"  erase {sec:#x}: no ack ({body!r}) -- continuing")

    for off in range(0, len(data), args.chunk):
        chunk = data[off:off + args.chunk]
        ok, body = link.write(args.type, args.addr + off, chunk)
        print(f"\r  write {args.addr + off:#08x} ({off + len(chunk)}/{len(data)})"
              f" {'ok' if ok else '!=ack ' + repr(body)}", end="", file=sys.stderr, flush=True)
    print(file=sys.stderr)

    if args.verify:
        rb = bytearray()
        for off in range(0, len(data), args.chunk):
            rb += link.read(args.type, args.addr + off, min(args.chunk, len(data) - off))
        print(f"verify: {'OK, read-back matches' if bytes(rb) == data else 'MISMATCH'}")


def cmd_checksum(link, args):
    ok, body = link.checksum(args.type, args.addr, args.len)
    print(f"checksum {args.addr:#x}+{args.len:#x} type {args.type}: "
          f"{'ok' if ok else 'no/!=ack'}  reply={body.hex(' ')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    hx = lambda s: int(s, 0)

    p = sub.add_parser("probe", help="safe write/erase round-trip on an erased sector")
    p.add_argument("--addr", type=hx, default=0x76000)
    p.add_argument("--type", type=int, default=0)
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_probe)

    p = sub.add_parser("erase", help="erase the 4KB sector at ADDR")
    p.add_argument("addr", type=hx)
    p.add_argument("--type", type=int, default=0)
    p.add_argument("--yes", action="store_true")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_erase)

    p = sub.add_parser("write", help="write FILE at ADDR (erases the sectors first)")
    p.add_argument("addr", type=hx)
    p.add_argument("file")
    p.add_argument("--type", type=int, default=0)
    p.add_argument("--chunk", type=hx, default=256)
    p.add_argument("--yes", action="store_true")
    p.add_argument("--force", action="store_true")
    p.add_argument("--verify", action="store_true", default=True)
    p.set_defaults(func=cmd_write)

    p = sub.add_parser("checksum", help="ask the device for a flash checksum")
    p.add_argument("addr", type=hx)
    p.add_argument("len", type=hx)
    p.add_argument("--type", type=int, default=0)
    p.set_defaults(func=cmd_checksum)

    args = ap.parse_args()
    link = FlashLink()
    link.drain()
    args.func(link, args)


if __name__ == "__main__":
    main()
