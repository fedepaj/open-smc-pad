#!/usr/bin/env python3
"""Dump SMC-PAD flash over USB-MIDI with the stock firmware's SINCO read command (0x23).

Read-only. Usage:  python sinco_dump.py OUT.bin [--type 0] [--start 0] [--size 0x80000] [--chunk 256]
"""
import argparse
import struct
import sys
import time

import mido

from sinco_proto import make_packet, pack7, unpack7, parse_packet

PORT = "SINCO SMC-PAD-Private"


class Link:
    def __init__(self, name=PORT):
        self.inp = mido.open_input(name)
        self.out = mido.open_output(name)

    def send(self, cmd, payload=b""):
        msg = pack7(make_packet(cmd, payload))
        self.out.send(mido.Message("sysex", data=list(msg[1:-1])))

    def recv(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            for m in self.inp.iter_pending():
                if m.type == "sysex":
                    return parse_packet(unpack7(bytes([0xF0, *m.data, 0xF7])))
            time.sleep(0.001)
        return None

    def drain(self):
        while self.recv(0.05):
            pass

    def read(self, ftype, addr, n, retries=3, timeout=1.0):
        payload = bytes([ftype]) + struct.pack("<I", addr) + struct.pack("<I", n)[:3]
        for _ in range(retries):
            self.send(0x23, payload)
            r = self.recv(timeout)
            if r is None:
                self.drain()
                continue
            cmd, body, chk_ok = r
            if cmd != 0x23:
                raise RuntimeError(f"device refused read type={ftype} addr={addr:#x}: {body.hex(' ')}")
            rtype, raddr = body[0], struct.unpack_from("<I", body, 1)[0]
            rlen = int.from_bytes(body[5:8], "little")
            if chk_ok and rtype == ftype and raddr == addr and rlen == n and len(body) == 8 + n:
                return body[8:]
            self.drain()
        raise RuntimeError(f"no valid reply for type={ftype} addr={addr:#x}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--type", type=int, default=0)
    ap.add_argument("--start", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x80000)
    ap.add_argument("--chunk", type=lambda s: int(s, 0), default=256)
    a = ap.parse_args()

    link = Link()
    link.drain()
    data = bytearray()
    t0 = time.time()
    for addr in range(a.start, a.start + a.size, a.chunk):
        n = min(a.chunk, a.start + a.size - addr)
        data += link.read(a.type, addr, n)
        if (addr - a.start) % 0x4000 == 0:
            done = addr - a.start
            print(f"\r{done:#08x}/{a.size:#x}  {done / max(time.time() - t0, 1e-3) / 1024:5.1f} KB/s",
                  end="", file=sys.stderr, flush=True)
    open(a.out, "wb").write(data)
    print(f"\nwrote {len(data)} bytes to {a.out} in {time.time() - t0:.1f}s", file=sys.stderr)


if __name__ == "__main__":
    main()
