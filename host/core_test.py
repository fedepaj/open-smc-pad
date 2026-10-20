"""Read CoreTest's report from the pad's USB serial, and look for its BLE advertising.

usage: core_test.py <serial port>   (exit status: 0 = all passed)
"""
import asyncio
import sys
import time

import serial
from bleak import BleakScanner


def read_report(port, timeout=90):
    deadline = time.time() + timeout
    while True:
        try:
            s = serial.Serial(port, 115200, timeout=1)
            break
        except serial.SerialException:
            if time.time() > deadline:
                sys.exit(f"no serial port {port}")
            time.sleep(0.5)
    s.dtr = True                       # the sketch waits for the port to be opened
    lines = []
    while time.time() < deadline:
        line = s.readline().decode(errors="replace").strip()
        if not line:
            continue
        print(line, flush=True)
        lines.append(line)
        if line.startswith("DONE"):
            break
    s.close()
    return lines


async def find_ble(name, timeout=15):
    dev = await BleakScanner.find_device_by_name(name, timeout=timeout)
    return dev


def main():
    lines = read_report(sys.argv[1])
    done = [l for l in lines if l.startswith("DONE")]
    fails = [l for l in lines if l.startswith("FAIL")]
    ble = None
    if any("ble advertising" in l for l in lines):
        ble = asyncio.run(find_ble("SMC-TEST"))
        print(("PASS" if ble else "FAIL") + " host sees BLE advertising" + (f": {ble.address}" if ble else ""))
    if not done:
        print("FAIL no DONE line (crash or hang?)")
    ok = done and not fails and ble is not None
    print("CORE TEST", "PASSED" if ok else "FAILED")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
