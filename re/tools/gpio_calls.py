"""List calls to JieLi GPIO helpers in a pi32v2 objdump listing, with the pin argument."""
import re
import sys
from collections import defaultdict

FUNCS = {
    "1e0082e": "set_dir", "1e00aac": "set_dir2", "1e008c6": "pull_up", "1e0091c": "pull_down",
    "1e00974": "die", "1e00a04": "out", "1e00816": "gpio2reg",
}
LINE = re.compile(r"^\s*([0-9a-f]{7}):\s+(?:[0-9a-f]{2} )+\s*\t(.*)$")


def pin_name(n):
    if 56 <= n <= 60:
        return ["USBDP", "USBDM", "USB58", "USB59", "USB60"][n - 56]
    return f"P{'ABCD'[n >> 4]}{n & 15}" if 0 <= n < 64 else f"?{n}"


def main(path):
    lines = [m.groups() for m in map(LINE.match, open(path)) if m]
    uses = defaultdict(list)
    for i, (addr, ins) in enumerate(lines):
        m = re.search(r"call .*: ([0-9a-f]{7}) >|goto .*: ([0-9a-f]{7}) >", ins)
        tgt = m and (m.group(1) or m.group(2))
        if tgt not in FUNCS or ("goto" in ins and "call" not in ins and not ins.strip().startswith("goto")):
            continue
        args = {}
        for _, prev in reversed(lines[max(0, i - 8):i]):
            for reg in ("r0", "r1"):
                mm = re.match(rf"\s*{reg} = (-?\d+)\s*$", prev)
                if mm and reg not in args:
                    args[reg] = int(mm.group(1))
            if re.search(r"\b(call|rts|pc =)\b", prev):
                break
        pin = args.get("r0")
        uses[pin_name(pin) if pin is not None else "?(reg)"].append((addr, FUNCS[tgt], args.get("r1")))
    for pin in sorted(uses):
        print(f"{pin:7s}", "  ".join(f"{f}({v})@{a}" for a, f, v in uses[pin]))


if __name__ == "__main__":
    main(sys.argv[1])
