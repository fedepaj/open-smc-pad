#!/usr/bin/env python3
"""Send commands to our firmware's console and show the reply, through host/con_daemon.py.

    host/.venv-serial/bin/python host/console.py [-t SECONDS] [--until REGEX] CMD [CMD ...]

The daemon keeps the port open and logs everything to work/con.log; this writes each command
to the port and prints the log lines that arrive until SECONDS pass without any (or until a
line matches REGEX). A reset shows up as "---- port gone" followed by the next boot's report.
"""
import argparse
import os
import re
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOG = os.path.join(ROOT, "work", "con.log")
FIFO = os.path.join(ROOT, "work", "con.in")


def send(cmd):
    # Never open the serial port here: closing a second descriptor drops DTR and stalls the
    # firmware's console. The daemon writes it for us.
    fd = os.open(FIFO, os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, (cmd + "\r").encode())
    finally:
        os.close(fd)


def follow(f, quiet, until):
    last = time.time()
    while time.time() - last < quiet:
        line = f.readline()
        if not line:
            time.sleep(0.05)
            continue
        last = time.time()
        sys.stdout.write(line)
        sys.stdout.flush()
        if until and re.search(until, line):
            return


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-t", type=float, default=1.5, help="quiet time that ends a reply")
    ap.add_argument("--until", help="stop at the first line matching this regex")
    ap.add_argument("cmds", nargs="+")
    a = ap.parse_args()
    if not os.path.exists(LOG):
        sys.exit("start host/con_daemon.py first")
    with open(LOG) as f:
        f.seek(0, os.SEEK_END)
        for c in a.cmds:
            print(f">> {c}")
            send(c)
            follow(f, a.t, a.until)


if __name__ == "__main__":
    main()
