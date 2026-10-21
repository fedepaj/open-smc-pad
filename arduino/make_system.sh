#!/usr/bin/env bash
# Fill arduino/smcpad/system/ from a core build (SMCPAD_ARDUINO=1 SMCPAD_TINYUSB=1
# TEXT_BASE=0x1E08120 fw/build.sh) and set up the development tool links.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
S=$ROOT/arduino/smcpad/system
mkdir -p $S/include
cp $ROOT/fw/smcpad/out/core/smcpad_core.o $ROOT/fw/smcpad/out/core/sdk.ld $ROOT/fw/smcpad/out/core/libsmcpad_native.a $S/
cp $ROOT/fw/smcpad/arduino/smcpad_rt.h $S/include/
# directory template (9 entries, VM/PRCT for our layout) + the SDK's cfg_tool.bin
python3 $ROOT/arduino/release/make_template.py $S/app_template.bin
# development tools: the packer, blink + its Linux root on macOS, and platform.local.txt pointing
# the platform at them (a Boards Manager install gets them as packages instead)
H=$ROOT/arduino/smcpad/tools/host
mkdir -p $H
cc -O2 -o $H/smcpad_pack $ROOT/arduino/smcpad/tools/smcpad_pack.c
if [ "$(uname -s)" = Darwin ]; then
    mkdir -p $H/blink
    ln -sfn $ROOT/work/blink-src/blink-master/o/blink/blink $H/blink/blink
    ln -sfn $ROOT/work/blink-root9 $H/blink/root
fi
TC="$(ls -d $ROOT/tools/jl-toolchain/jieli-linux-toolchains-*/ | head -1)"
{ echo "# development install (arduino/make_system.sh): local tools instead of the Boards Manager's"
  echo "tools.jl.toolchain=${TC%/}"
  echo "tools.jl.host=$H"
  echo "tools.dfu-util.cmd=dfu-util"; } > $ROOT/arduino/smcpad/platform.local.txt
ls -l $S
