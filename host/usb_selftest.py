#!/usr/bin/env python3
"""USB self-test against the bring-up console (either USB stack).

    host/.venv-serial/bin/python host/usb_selftest.py [--cmds N] [--uploads N]

1. commands: N short commands (`R1120`), every one must be answered;
2. TX: the `T` pattern (one 200-byte write, then ten 20-byte writes) must come back intact;
3. RX: N uploads of 1 KB through `uw` written in full 64-byte packets, each with distinct
   content, whose CRC32 the firmware reports back.
Prints one line per test and exits non-zero if any failed.
"""
import argparse
import re
import subprocess
import sys
import time
import zlib

import serial

NAMES = ("SMC-PAD open fw", "USB Composite Device")


def find_port():
    for name in NAMES:
        out = subprocess.run(["ioreg", "-r", "-n", name, "-l", "-w", "0"],
                             capture_output=True, text=True).stdout
        m = re.search(r'"IOCalloutDevice" = "([^"]+)"', out)
        if m:
            return m.group(1)
    return None


class Console:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.02)
        self.s.dtr = True
        time.sleep(0.4)
        self.drain(0.3)
        for _ in range(8):                  # sync on the prompt
            self.s.write(b"\r")
            if b">" in self.drain(0.3):
                break
        else:
            raise SystemExit("the console does not answer")

    def drain(self, t):
        end, out = time.time() + t, b""
        while time.time() < end:
            out += self.s.read(4096)
        return out

    def until(self, pattern, timeout):
        rx, end, out = re.compile(pattern), time.time() + timeout, b""
        while time.time() < end:
            out += self.s.read(4096)
            m = rx.search(out)
            if m:
                return m, out
        return None, out


def test_commands(c, n):
    ok = 0
    for i in range(n):
        addr = 0x1120 + 16 * (i % 64)
        c.s.write(f"R{addr:x}\r".encode())
        m, _ = c.until(rf"{addr:06x}:".encode(), 1.0)
        ok += m is not None
    return ok == n, f"{ok}/{n} answered"


def test_tx(c):
    c.drain(0.2)
    c.s.write(b"T\r")
    m, out = c.until(rb"9{18}\r\n", 3.0)
    alpha = "".join(chr(ord("A") + i % 26) for i in range(200))
    want = ("<" + alpha + ">\r\n" + "".join(str(k) * 18 + "\r\n" for k in range(10))).encode()
    good = m is not None and want in out
    return good, "pattern intact" if good else f"got {out[:80]!r}..."


def test_rx(c, n):
    results = []
    for t in range(n):
        data = b"".join(bytes([(0x40 + 16 * t + i) & 0xFF]) * 64 for i in range(16))
        c.drain(0.1)
        c.s.write(b"uw 0 400\r")
        m, _ = c.until(rb"uw ready", 2.0)
        if not m:
            results.append("no-ready")
            c.drain(3.0)
            continue
        c.s.write(data)
        m, out = c.until(rb"uw (ok ([0-9a-f]{8})|fail|timeout[^\r]*)", 6.0)
        if m and m.group(2) and int(m.group(2), 16) == zlib.crc32(data) & 0xFFFFFFFF:
            results.append("ok")
        else:
            results.append(m.group(1).decode() if m else "no-answer")
            c.drain(0.5)
            c.s.write(b"G\r")
            _, blk = c.until(rb"blk 0f[0-9a-f]{6}\r\n", 2.0)
            bad = []
            for line in re.findall(rb"blk ([0-9a-f]{8})", blk):
                v = int(line, 16)
                i, first, last, diff = v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF
                want = (0x40 + 16 * t + i) & 0xFF
                if first != want or last != want or diff:
                    bad.append(f"blk{i}:first={first:02x} last={last:02x} diff={diff}")
            results.append("[" + ", ".join(bad) + "]")
    good = all(r == "ok" for r in results)
    return good, " ".join(results)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmds", type=int, default=100)
    ap.add_argument("--uploads", type=int, default=8)
    a = ap.parse_args()
    port = find_port()
    if not port:
        sys.exit("our firmware is not on USB")
    c = Console(port)
    failed = False
    for name, fn in (("commands", lambda: test_commands(c, a.cmds)),
                     ("tx pattern", lambda: test_tx(c)),
                     ("rx uploads", lambda: test_rx(c, a.uploads))):
        good, detail = fn()
        failed |= not good
        print(f"{'PASS' if good else 'FAIL'}  {name:11s} {detail}")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
