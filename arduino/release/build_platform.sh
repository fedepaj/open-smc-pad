#!/usr/bin/env bash
# The Boards Manager platform archive: arduino/smcpad with the precompiled core in system/
# (from `SMCPAD_ARDUINO=1 SMCPAD_TINYUSB=1 TEXT_BASE=0x1E08120 fw/build.sh`), ArduinoCore-API
# copied in (not a symlink), the version stamped, the licenses alongside.
#   arduino/release/build_platform.sh <version> <out dir>
set -euo pipefail
VER="$1"; mkdir -p "$2"; OUT="$(cd "$2" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CORE="$ROOT/fw/smcpad/out/core"
[ -f "$CORE/smcpad_core.o" ] || { echo "no precompiled core in $CORE: run fw/build.sh first" >&2; exit 1; }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
S="$TMP/smcpad-br25"
cp -R "$ROOT/arduino/smcpad" "$S"
rm -rf "$S/system" "$S/tools/host" "$S/tools/toolchain" "$S/tools/blink" "$S/platform.local.txt" "$S/.gitignore"
rm -f "$S/cores/smcpad/api"
cp -R "$ROOT/third_party/ArduinoCore-API/api" "$S/cores/smcpad/api"
mkdir -p "$S/system/include"
cp "$CORE/smcpad_core.o" "$CORE/sdk.ld" "$CORE/libsmcpad_native.a" "$S/system/"
cp "$ROOT/fw/smcpad/arduino/smcpad_rt.h" "$S/system/include/"
python3 "$ROOT/arduino/release/make_template.py" "$S/system/app_template.bin"
python3 - "$S/platform.txt" "$VER" <<'PY'
import re, sys
p, ver = sys.argv[1], sys.argv[2]
s = open(p).read()
open(p, "w").write(re.sub(r"(?m)^version=.*$", "version=" + ver, s))
PY
cp "$ROOT/arduino/LICENSE" "$S/LICENSE"
cp "$ROOT/fw/NOTICE" "$S/system/NOTICE"
cp "$ROOT/fw/LICENSE-APACHE-2.0" "$S/system/LICENSE-APACHE-2.0"
cp "$ROOT/third_party/ArduinoCore-API/LICENSE" "$S/cores/smcpad/api/LICENSE"
find "$S" -name .DS_Store -delete
COPYFILE_DISABLE=1 tar -C "$TMP" -cjf "$OUT/smcpad-br25-$VER.tar.bz2" smcpad-br25
echo "$OUT/smcpad-br25-$VER.tar.bz2"
