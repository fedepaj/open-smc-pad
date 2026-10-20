#!/usr/bin/env bash
# Prototype of the Arduino core's build recipe: sketch + precompiled core -> app.bin
set -euo pipefail
ROOT=/Users/federicopaglioni/Documents/open_smc_pad
TOOLCHAIN="$(ls -d "$ROOT"/tools/jl-toolchain/jieli-linux-toolchains-*/ | head -1)"
POSTBUILD="$(ls -d "$ROOT"/tools/jl-postbuild/jieli-linux-post-build-tools-*/ | head -1)"
P=${SKETCH_DIR:-$ROOT/arduino/proto}
docker run --rm --platform linux/amd64 -v "$TOOLCHAIN:/opt/jieli:ro" -v "$POSTBUILD:/opt/utils:ro" \
    -v "$ROOT/fw/smcpad/out/core:/core:ro" -v "$ROOT/fw/smcpad/arduino:/rt:ro" \
    -v "$ROOT/sdk/fw-AC63_BT_SDK/cpu/br25/liba:/liba:ro" -v "$P:/p" -w /p jl-build bash -c '
        set -e
        T=/opt/jieli/pi32v2/bin
        $T/clang -target pi32v2 -mcpu=r3 -integrated-as -Oz -fno-common -ffunction-sections -fdata-sections \
            -mllvm -enable-ipra=false -isystem /opt/jieli/pi32v2/include -I/rt -c sketch.c -o sketch.o
        ARCH=$(ls /liba/*.a | tr "\n" " ")
        time $T/ld -o sketch.elf sketch.o /core/smcpad_core.o --start-group $ARCH --end-group \
            -L/opt/jieli/pi32v2/lib/r3 /opt/jieli/pi32v2/lib/r3/libc.a /opt/jieli/pi32v2/lib/r3/libm.a \
            /opt/jieli/pi32v2/lib/r3/libcompiler-rt.a -T/core/sdk.ld --gc-sections --sort-common \
            --dont-complain-call-overflow -Map sketch.map 2>&1 | grep -v "warning" | head -20
        W=$(mktemp -d)
        for s in text data data_code; do $T/objcopy -O binary -j .$s sketch.elf $W/$s.bin; done
        for s in aec wav ape flac m4a amr dts fm mp3 wma; do
            $T/objcopy -O binary -j .overlay_$s sketch.elf $W/${s}o.bin
            /opt/utils/remove_tailing_zeros -i $W/${s}o.bin -o $W/$s.bin -mark ff >/dev/null
        done
        cat $W/text.bin $W/data.bin $W/data_code.bin $W/aec.bin $W/wav.bin $W/ape.bin $W/flac.bin \
            $W/m4a.bin $W/amr.bin $W/dts.bin $W/fm.bin $W/mp3.bin $W/wma.bin > app.bin
        ls -l app.bin sketch.elf
    '
TEXT_BASE=0x1E08120 python3 "$ROOT/host/check_appbin.py" "$P/sketch.elf" "$P/app.bin"
python3 "$ROOT/host/jlimage.py" build --app "$P/app.bin" --area-base 0x9000 --vm-base 0x71000 --vm-size 0x4000 \
    --prct-size 0x71000 --out "$P/sketch_area.bin" | tail -3
