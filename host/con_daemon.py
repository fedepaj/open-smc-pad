#!/usr/bin/env python3
"""Keep our firmware's console port open forever and log it, across resets.

    host/.venv-serial/bin/python host/con_daemon.py          (leave it running)

Every line goes to work/con.log with a wall-clock timestamp (ms). When the pad leaves USB the
log says "---- port gone"; when it comes back the daemon reattaches and the boot report lands
in the same log. Send commands with host/con.py.

The port must stay open, and through ONE descriptor: with nobody reading, the firmware's CDC
writes stall and the console stops answering; and closing any second descriptor on the port
drops DTR (HUPCL) for everyone, which also stalls it -- then the 8 s watchdog resets the pad.
So commands come in through a FIFO (work/con.in) and go out through the daemon's own fd.
"""
import os
import re
import subprocess
import time

import serial

# our firmware with the JieLi SDK's USB stack, and with TinyUSB
NAMES = ("USB Composite Device", "SMC-PAD open fw")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "work", "con.log")
FIFO = os.path.join(ROOT, "work", "con.in")     # host/con.py writes commands here


def find_port():
    for name in NAMES:
        out = subprocess.run(["ioreg", "-r", "-n", name, "-l", "-w", "0"],
                             capture_output=True, text=True).stdout
        m = re.search(r'"IOCalloutDevice" = "([^"]+)"', out)
        if m:
            return m.group(1)
    return None


def now():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int((t % 1) * 1000):03d}"


def log(f, text):
    f.write(f"{now()}  {text}\n")
    f.flush()


def main():
    if not os.path.exists(FIFO):
        os.mkfifo(FIFO)
    cmd_fd = os.open(FIFO, os.O_RDWR | os.O_NONBLOCK)  # RDWR: never sees EOF
    with open(LOG, "a", buffering=1) as f:
        log(f, "---- daemon started")
        while True:
            port = find_port()
            if not port:
                time.sleep(0.2)
                continue
            try:
                s = serial.Serial(port, 115200, timeout=0.1)
                s.dtr = True
            except (serial.SerialException, OSError):
                time.sleep(0.3)
                continue
            with open(os.path.join(ROOT, "work", "dbg_port"), "w") as p:
                p.write(port + "\n")
            log(f, f"---- attached to {port}")
            buf = b""
            last_check = time.time()
            while True:
                try:
                    try:
                        cmd = os.read(cmd_fd, 4096)
                    except BlockingIOError:
                        cmd = b""
                    if cmd:
                        s.write(cmd)
                    d = s.read(512)
                    if time.time() - last_check > 0.5:
                        last_check = time.time()
                        if not os.path.exists(port):
                            raise OSError("gone")
                except (serial.SerialException, OSError):
                    if buf:
                        log(f, buf.decode(errors="replace").rstrip("\r"))
                    log(f, "---- port gone (pad reset or left USB)")
                    try:
                        s.close()
                    except Exception:
                        pass
                    break
                if d:
                    buf += d
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        log(f, line.decode(errors="replace").rstrip("\r"))
                elif buf:
                    log(f, buf.decode(errors="replace").rstrip("\r"))   # a prompt, no newline
                    buf = b""


if __name__ == "__main__":
    main()
