#!/usr/bin/env bash
# Package fw/smcpad/out/app.bin into a flashable app area and the full image to verify
# against, so that `sudo host/flash_fw.sh` has everything it needs.
#
#   host/package_fw.sh
#
# Produces, in work/flash/:
#   fw_area.bin          the app area as the CPU sees it (plaintext)
#   fw_area_raw.bin      the same SFC-encrypted for flash 0x1000 -- this is what gets written
#   expected_with_fw.bin the whole 512 KB flash as it must read back afterwards
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

python3 host/check_appbin.py

python3 host/jlimage.py build \
    --out work/flash/fw_area.bin \
    --raw work/flash/fw_area_raw.bin

python3 - <<'EOF'
import pathlib

orig = pathlib.Path("dumps/original_physical_full.bin").read_bytes()
raw = pathlib.Path("work/flash/fw_area_raw.bin").read_bytes()
AREA, END = 0x1000, 0x26000
assert len(orig) == 0x80000, len(orig)
assert AREA + len(raw) <= END, f"the area ({len(raw):#x}) does not fit in {END - AREA:#x}"

img = bytearray(orig)
img[AREA:END] = b"\xff" * (END - AREA)          # flash_fw.sh erases the whole app area
img[AREA:AREA + len(raw)] = raw
out = pathlib.Path("work/flash/expected_with_fw.bin")
out.write_bytes(img)
print(f"expected image written to {out} "
      f"(our area {AREA:#x}..{AREA + len(raw):#x}, erased up to {END:#x})")
EOF

echo
echo "ready: sudo host/flash_fw.sh"
