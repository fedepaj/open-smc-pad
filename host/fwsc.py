"""M-VAVE .fwsc container helpers (format from M-UPGRADE OtaFormat::tryParse/pack).

A .fwsc is a JieLi UFW image with one metadata byte inserted after every 0x2F data bytes
for the first N (36 or 20) blocks of 0x30 bytes. Metadata char for 1-based block i is
(ord(c) + i) & 0xFF, spelling "NAME_VER" (VER = up to 3 digits), then 0x7D up to block N.
"""
BLK, DATA = 0x30, 0x2F


def parse(raw: bytes, nblocks: int):
    """Mimic OtaFormat::tryParse. Returns (name, version, ufw) or None."""
    if len(raw) < nblocks * BLK:
        return None
    name, ver, scale, state, ufw = "", 0, 100, 0, bytearray()
    for i in range(1, nblocks + 1):
        blk = raw[(i - 1) * BLK:i * BLK]
        ufw += blk[:DATA]
        m = blk[DATA]
        c = (m - i) & 0xFF
        if state == 0:
            if c == ord("_"):
                state = 1
            else:
                name += chr(c)
        elif state == 1:
            if m == 0x7D:
                state = 2
            elif 0x30 <= c <= 0x39:
                if scale == 0:
                    return None
                ver += scale * (c - 0x30)
                scale //= 10
        elif m != 0x7D:
            return None
    if state != 2:
        return None
    ufw += raw[nblocks * BLK:]
    return name, ver, bytes(ufw)


def parse_any(raw: bytes):
    return parse(raw, 36) or parse(raw, 20)


def pack(ufw: bytes, name: str, version: int, nblocks: int = 20) -> bytes:
    meta = f"{name}_{version:03d}"
    if len(meta) >= nblocks or len(ufw) < nblocks * DATA:
        raise ValueError("name too long or image too small")
    out = bytearray()
    for i in range(1, nblocks + 1):
        out += ufw[(i - 1) * DATA:i * DATA]
        out.append((ord(meta[i - 1]) + i) & 0xFF if i <= len(meta) else 0x7D)
    out += ufw[nblocks * DATA:]
    return bytes(out)
