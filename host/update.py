#!/usr/bin/env python3
"""Update the firmware over its own USB console -- no Pico, no download mode, no root.

    host/.venv-serial/bin/python host/update.py [IMAGE]     (default work/flash/fw_area_raw.bin)

IMAGE is what host/flash_fw.sh writes at flash 0x1000 (host/package_fw.sh makes it). The
running firmware stores it in free flash (staging), both sides check its CRC32, then the
firmware copies it over its own app area from RAM, checks the copy and resets. The script
waits for the new firmware and prints its boot report.

If the copy itself is interrupted (power loss), the app area is half written and only the
ROM download mode (host/flash_fw.sh with the Pico) brings the pad back.
"""
import os
import re
import subprocess
import sys
import time
import zlib

import serial

# our firmware with the JieLi SDK's USB stack, and with TinyUSB
NAMES = ("USB Composite Device", "SMC-PAD open fw")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHUNK = 1024
# The JieLi SDK's USB stack loses runs of full 64-byte packets (its bulk read and the
# double-buffered OUT endpoint disagree), so with it the payload goes out 63 bytes per write.
# Our TinyUSB driver handles them (docs/usb-br25.md): one write per chunk.
SDK_STACK_NAME = "USB Composite Device"
SDK_PACKET = 63
AREA_LEN = 0x25000


def find_port_named():
    for name in NAMES:
        out = subprocess.run(["ioreg", "-r", "-n", name, "-l", "-w", "0"],
                             capture_output=True, text=True).stdout
        m = re.search(r'"IOCalloutDevice" = "([^"]+)"', out)
        if m:
            return m.group(1), name
    return None, None


def find_port():
    return find_port_named()[0]


class Console:
    def __init__(self, port):
        self.s = serial.Serial(port, 115200, timeout=0.05)
        self.s.dtr = True
        self.buf = b""
        time.sleep(0.3)
        self.drain(0.5)
        # The first bytes after the port opens are sometimes lost (the host's line-state
        # requests land while they are in flight): sync on the prompt before anything else.
        for _ in range(8):
            self.s.write(b"\r")
            if b">" in self.drain(0.4):
                break
        else:
            raise SystemExit("the console does not answer")

    def drain(self, t):
        end = time.time() + t
        out = b""
        while time.time() < end:
            out += self.s.read(4096)
        return out

    def expect(self, pattern, timeout):
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip().lstrip("> ").strip()
                m = rx.search(text)
                if m:
                    return m
            self.buf += self.s.read(512)
        return None

    def cmd(self, line, pattern, timeout=5.0):
        self.buf = b""
        self.s.write((line + "\r").encode())
        return self.expect(pattern, timeout)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "work/flash/fw_area_raw.bin")
    image = open(path, "rb").read()
    if not 0x100 < len(image) <= AREA_LEN:
        sys.exit(f"{path}: {len(image)} bytes, must be 0x100..{AREA_LEN:#x}")
    crc = zlib.crc32(image) & 0xFFFFFFFF
    port, name = find_port_named()
    if not port:
        sys.exit("our firmware is not on USB")
    packet = SDK_PACKET if name == SDK_STACK_NAME else CHUNK
    print(f"image {path}: {len(image)} bytes, crc32 {crc:08x}; firmware on {port} ({name})")
    c = Console(port)

    m = c.cmd(f"ub {len(image):x}", r"^ub (ok|fail)", 15)
    if not m:
        sys.exit("no answer to 'ub' -- is this a firmware with the update commands?")
    if m.group(1) != "ok":
        sys.exit("the firmware could not erase the staging area")
    t0 = time.time()
    for off in range(0, len(image), CHUNK):
        part = image[off:off + CHUNK]
        want = zlib.crc32(part) & 0xFFFFFFFF
        last = "no 'uw ready'"
        for attempt in range(3):
            if not c.cmd(f"uw {off:x} {len(part):x}", r"^uw ready", 3):
                continue
            for i in range(0, len(part), packet):
                c.s.write(part[i:i + packet])
                c.s.flush()
                if packet < len(part):
                    time.sleep(0.002)
            m = c.expect(r"^uw (ok ([0-9a-f]{8})|fail|timeout.*)", 5)
            if m and m.group(2) and int(m.group(2), 16) == want:
                break
            last = m.group(0) if m else "no answer"
            time.sleep(3)                       # let a half-received chunk time out
        else:
            sys.exit(f"\nchunk at {off:#x} failed three times (last answer: {last})")
        done = off + len(part)
        print(f"\r   staged {done:6d}/{len(image)} bytes ({done / max(time.time() - t0, 1e-3) / 1024:.0f} KB/s)",
              end="", flush=True)
    print()
    m = c.cmd(f"uv {len(image):x} {crc:08x}", r"^uv (ok|bad.*)", 10)
    if not m or m.group(1) != "ok":
        sys.exit(f"staging check failed: {m.group(1) if m else 'no answer'}")
    print("   staging verified; copying over the app area (do not unplug)")
    m = c.cmd(f"uc {len(image):x} {crc:08x}", r"^uc (copying|bad.*|refused)", 10)
    if not m or m.group(1) != "copying":
        sys.exit(f"commit refused: {m.group(1) if m else 'no answer'}")
    try:
        c.s.close()
    except Exception:
        pass

    gone = time.time() + 10
    while time.time() < gone and find_port():
        time.sleep(0.1)
    end = time.time() + 20
    port = None
    while time.time() < end and not port:
        port = find_port()
        time.sleep(0.2)
    if not port:
        sys.exit("the firmware did not come back on USB within 20 s -- recover with host/flash_fw.sh")
    time.sleep(0.5)
    s = serial.Serial(port, 115200, timeout=0.05)
    s.dtr = True
    out = b""
    end = time.time() + 3
    while time.time() < end:
        out += s.read(4096)
    text = out.decode(errors="replace")
    for line in text.splitlines():
        if line.startswith("build ") or "update" in line or line.startswith("!!"):
            print("   " + line.strip())
    if "update applied" not in text:
        sys.exit("the new firmware did not confirm the update (see above)")


if __name__ == "__main__":
    main()
