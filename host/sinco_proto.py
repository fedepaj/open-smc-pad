"""SINCO/M-VAVE USB-MIDI service protocol (reverse engineered from M-UPGRADE).

Packet (8-bit):  00 59 | cmd | len24 LE | payload | ~sum(payload)
Transport: 8-bit packet bit-packed LSB-first into 7-bit bytes, wrapped in F0 .. F7
(no manufacturer ID), on the "SMC-PAD-Private" port. See docs/sinco-protocol.md.
"""

HDR = b"\x00\x59"


def make_packet(cmd: int, payload: bytes = b"") -> bytes:
    n = len(payload)
    chk = (~sum(payload)) & 0xFF
    return HDR + bytes([cmd, n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF]) + payload + bytes([chk])


def pack7(data: bytes) -> bytes:
    out, acc, nbits = bytearray([0xF0]), 0, 0
    for b in data:
        acc |= b << nbits
        nbits += 8
        while nbits >= 7:
            out.append(acc & 0x7F)
            acc >>= 7
            nbits -= 7
    if nbits:
        out.append(acc & 0x7F)
    out.append(0xF7)
    return bytes(out)


def unpack7(sysex: bytes) -> bytes:
    body = sysex[1:-1] if sysex[:1] == b"\xF0" else sysex
    out, acc, nbits = bytearray(), 0, 0
    for b in body:
        acc |= (b & 0x7F) << nbits
        nbits += 7
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8
    return bytes(out)


def parse_packet(pkt: bytes):
    if len(pkt) < 7 or pkt[:2] != HDR:
        return None
    cmd = pkt[2]
    n = int.from_bytes(pkt[3:6], "little")
    payload = pkt[6:6 + n]
    chk_ok = len(pkt) > 6 + n and pkt[6 + n] == (~sum(payload)) & 0xFF
    return cmd, payload, chk_ok
