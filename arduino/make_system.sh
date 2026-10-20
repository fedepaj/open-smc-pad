#!/usr/bin/env bash
# Fill arduino/smcpad/system/ from a core build (SMCPAD_ARDUINO=1 SMCPAD_TINYUSB=1
# TEXT_BASE=0x1E08120 fw/build.sh) and set up the development tool links.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S=$ROOT/arduino/smcpad/system
mkdir -p $S/include
cp $ROOT/fw/smcpad/out/core/smcpad_core.o $ROOT/fw/smcpad/out/core/sdk.ld $ROOT/fw/smcpad/out/core/libsmcpad_native.a $S/
cp $ROOT/fw/smcpad/arduino/smcpad_rt.h $S/include/
# directory template (9 entries, VM/PRCT for our layout) + cfg_tool.bin, from jlimage
python3 - "$ROOT" "$S/app_template.bin" <<'PY'
import sys; sys.path.insert(0, sys.argv[1] + "/host")
import jlimage as J
dump = open(sys.argv[1] + "/dumps/SMC-PAD_004_flash_t0.bin", "rb").read()
t = J.read_dir(dump, J.AREA_BASE); c = t[2]
cfg = dump[J.AREA_BASE + c.offset: J.AREA_BASE + c.offset + c.size]
area = J.build_app_area(b"\0" * 0x200, cfg, t, entry_vma=0x1E08120, vm=(0x71000, 0x4000), prct_size=0x71000)
open(sys.argv[2], "wb").write(area[:0x120] + cfg)
PY
T=$ROOT/arduino/smcpad/tools
ln -sfn "$(ls -d $ROOT/tools/jl-toolchain/jieli-linux-toolchains-*/ | head -1)" $T/toolchain
mkdir -p $T/blink
ln -sfn $ROOT/work/blink-src/blink-master/o/blink/blink $T/blink/blink
ln -sfn $ROOT/work/blink-root9 $T/blink/root
ls -l $S
