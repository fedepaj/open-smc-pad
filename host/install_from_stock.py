#!/usr/bin/env python3
"""Install our bootloader + firmware on a pad that runs the stock firmware. No Pico, no sudo.

    host/package_fw.sh boot          # once: work/flash/boot_area.bin, fw_area.bin
    uv run --with mido --with python-rtmidi host/install_from_stock.py [--dry-run]

What happens (fw/boot/install.c has the other half, docs/bootloader.md the whole story):

1. Over USB-MIDI the stock firmware (SINCO 0x21/0x22/0x23) writes, into free flash:
   - the installer as an app area at 0x41000, SFC-encrypted;
   - a staging area at 0x43000: header + the bootloader's app area (plaintext);
   and every byte is read back.
2. It points sector 0's app_dir_head at 0x41000 (read, patch one entry, write, read back;
   on any mismatch the original sector is written back and nothing else has changed).
3. You power-cycle the pad. The uboot now runs the installer, which replaces the stock
   firmware with the bootloader and points sector 0 back at 0x1000 (if the staging area does
   not check out it only points sector 0 back: the stock firmware boots again).
4. The bootloader finds no application, comes up as DFU 1209:0002, and this script hands over
   to host/dfu_flash.sh to write the application.

Sector 0 as the stock firmware reads it is the physical sector XOR an ENC keystream (key
0xFFFF, restarting every 256 bytes); its writes are raw. The file list in it is 32-byte
entries, each ENC-ciphered (key 0xFFFF) on its own, with a CRC16 over their last 30 bytes.
"""
import argparse
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))

import bootpkg                                       # noqa: E402
import jlimage as J                                  # noqa: E402
from sinco_flash import FlashLink                    # noqa: E402

INSTALL_DIR, STAGE, STAGE_END, DEST = bootpkg.INSTALL_DIR, bootpkg.STAGE, bootpkg.STAGE_END, bootpkg.DEST
SECTOR = 0x1000
CHUNK = 1024                    # the stock firmware's write limit
DFU_ID = "1209:0002"


def say(msg):
    print(msg, flush=True)


def die(msg):
    sys.exit(f"!! {msg}")


# ------------------------------------------------------------------ sector 0

def sector0_keystream():
    k = bytearray(SECTOR)
    for off in range(0, SECTOR, 256):
        J.enc_cipher(k, off, 256)
    return bytes(k)


def entries(phys):
    """(offset, decrypted entry bytes) of sector 0's file list."""
    out = []
    for off in range(0x20, 0x200, 32):
        e = bytearray(phys[off:off + 32])
        J.enc_cipher(e, 0, 32)
        if J.crc16(bytes(e[2:])) != struct.unpack_from("<H", e)[0]:
            break
        out.append((off, bytes(e)))
    return out


def app_dir_head(phys):
    for off, e in entries(phys):
        if e[16:28] == b"app_dir_head":
            return off, struct.unpack_from("<I", e, 4)[0]
    die("no app_dir_head in sector 0: not the layout this tool knows")


def with_boot_dir(phys, target):
    off, _ = app_dir_head(phys)
    e = bytearray(phys[off:off + 32])
    J.enc_cipher(e, 0, 32)
    struct.pack_into("<I", e, 4, target)
    struct.pack_into("<H", e, 0, J.crc16(bytes(e[2:])))
    J.enc_cipher(e, 0, 32)
    out = bytearray(phys)
    out[off:off + 32] = e
    return bytes(out)


# ------------------------------------------------------------------ the link

class Pad:
    def __init__(self, dry_run):
        self.link = FlashLink()
        self.dry = dry_run
        self.link.drain()
        self.link.send(0x11)
        r = self.link.recv(1.0)
        if not r or r[0] != 0x11:
            die("the stock firmware does not answer on SINCO SMC-PAD-Private")
        self.name = r[1].rstrip(b"\0").decode(errors="replace")

    def read(self, addr, n):
        out = b""
        while len(out) < n:
            k = min(256, n - len(out))
            out += self.link.read(0, addr + len(out), k)
        return out

    def write(self, addr, data):
        """Erase the sectors, write in 1 KB pieces, read back. addr sector aligned."""
        assert addr % SECTOR == 0
        if self.dry:
            return
        for a in range(addr, addr + len(data), SECTOR):
            ok, info = self.link.erase(0, a)
            if not ok:
                die(f"erase {a:#x} failed ({info})")
        for o in range(0, len(data), CHUNK):
            ok, info = self.link.write(0, addr + o, data[o:o + CHUNK])
            if not ok:
                die(f"write {addr + o:#x} failed ({info})")


def build_installer_area(pad):
    """The installer as an app area for INSTALL_DIR, directory modelled on the pad's own."""
    template = J.read_dir(pad.read(DEST, 0x120))
    cfg_e = template[2]
    return bootpkg.installer_area(template, pad.read(DEST + cfg_e.offset, cfg_e.size))


def build_stage():
    return bootpkg.stage(bootpkg.ERASE_STOCK)


def in_dfu():
    r = subprocess.run(["dfu-util", "-l"], capture_output=True, text=True)
    return f"[{DFU_ID}]" in r.stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dry-run", action="store_true", help="read and check everything, write nothing")
    ap.add_argument("--app", default=str(ROOT / "work/flash/fw_area.bin"),
                    help="application area to write over DFU at the end")
    a = ap.parse_args()

    pad = Pad(a.dry_run)
    say(f"== stock firmware {pad.name} on USB-MIDI")
    if not pad.name.startswith("SMC-PAD"):
        die("not an SMC-PAD")

    K = sector0_keystream()
    phys = bytes(x ^ y for x, y in zip(pad.read(0, SECTOR), K))
    off, boot_dir = app_dir_head(phys)
    say(f"   sector 0: {len(entries(phys))} entries, app_dir_head at +{off:#x} -> {boot_dir:#x}")
    if boot_dir not in (DEST, INSTALL_DIR):
        die(f"sector 0 boots {boot_dir:#x}, expected {DEST:#x}")

    installer = build_installer_area(pad)
    stage = build_stage()
    say(f"   installer {len(installer):#x} bytes at {INSTALL_DIR:#x}, "
        f"staging {len(stage):#x} bytes at {STAGE:#x}")

    used = pad.read(INSTALL_DIR, STAGE_END - INSTALL_DIR)
    if used.count(0xFF) != len(used):
        say("   note: the target area was not blank (an earlier attempt?): it gets erased")

    say("== writing the installer and the staging area")
    raw = J.sfc_encrypt_at(installer, INSTALL_DIR)
    pad.write(INSTALL_DIR, raw)
    pad.write(STAGE, stage)
    if not a.dry_run:
        # above the stock firmware's VM start the read path is raw: compare directly
        if pad.read(INSTALL_DIR, len(raw)) != raw or pad.read(STAGE, len(stage)) != stage:
            die("read-back mismatch in free flash (sector 0 untouched, nothing to undo)")
        say("   read back OK")

    new_phys = with_boot_dir(phys, INSTALL_DIR)
    changed = sum(1 for x, y in zip(phys, new_phys) if x != y)
    say(f"== pointing sector 0 at the installer ({changed} bytes change)")
    if a.dry_run:
        say("   dry run: stopping here, nothing was written")
        return
    if boot_dir != INSTALL_DIR:
        pad.write(0, new_phys)
        back = bytes(x ^ y for x, y in zip(pad.read(0, SECTOR), K))
        if back != new_phys:
            say("!! sector 0 did not read back as written: putting the original back")
            pad.write(0, phys)
            back = bytes(x ^ y for x, y in zip(pad.read(0, SECTOR), K))
            die("restored" if back == phys else
                "COULD NOT RESTORE SECTOR 0 -- do not power off; keep this output and ask")
    say("   sector 0 OK")

    say("\n>> now switch the pad OFF and ON again: the installer replaces the stock firmware\n"
        "   (a few seconds), then the bootloader comes up as DFU " + DFU_ID)
    for _ in range(600):
        if in_dfu():
            break
        time.sleep(0.5)
    else:
        die("the bootloader did not show up within 5 minutes")
    say("== bootloader up: writing the application")
    sys.exit(subprocess.run([str(HERE / "dfu_flash.sh"), a.app]).returncode)


if __name__ == "__main__":
    main()
