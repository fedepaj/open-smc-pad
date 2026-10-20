#!/usr/bin/env bash
# Build the open SMC-PAD firmware (fw/smcpad) with the JieLi AC63 SDK in Docker.
# Output: fw/smcpad/out/sdk.elf and fw/smcpad/out/app.bin
#
# Needs: Docker with linux/amd64 emulation, image `jl-build` (docker/Dockerfile.jl-build),
# toolchain extracted in tools/jl-toolchain/, SDK cloned in sdk/fw-AC63_BT_SDK.
#
# The firmware replaces the stock one entirely: it is linked at the factory entry point
# 0x1E00120 (app area at flash 0x1000), packaged by host/jlimage.py and written in ROM
# download mode (Pico USB_KEY + host/jl_usb_bridge.py + jl-uboot-tool).
#
# TEXT_BASE=<vma> builds the firmware to run from somewhere other than the factory app area.
# That is what makes a Pico-free install on a stock pad possible: the stock firmware can write
# free flash over USB-MIDI but not the area it is itself executing from, so our image goes into
# free flash, the boot pointer is aimed at it, and from there it is running *outside* 0x1000
# and can rewrite it. Flash offset 0xNNNNN maps to VMA 0x1E00000 + 0xNNNNN - 0x1000 + 0x120.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TEXT_BASE="${TEXT_BASE:-0x1E00120}"
TOOLCHAIN="$(ls -d "$ROOT"/tools/jl-toolchain/jieli-linux-toolchains-*/ | head -1)"
POSTBUILD="$(ls -d "$ROOT"/tools/jl-postbuild/jieli-linux-post-build-tools-*/ | head -1)"
SDK="$ROOT/sdk/fw-AC63_BT_SDK"
APP="$ROOT/fw/smcpad"
OUT="$APP/out"
mkdir -p "$OUT" "$ROOT/build_logs"
# USB stack: SMCPAD_TINYUSB=1 builds with TinyUSB and our BR25 driver (fw/smcpad/usb).
# Where the app area (directory) sits in flash, derived from TEXT_BASE: 0x1000 alone, 0x9000 behind
# the bootloader (fw/boot).
APP_DIR=$(printf '0x%x' $((TEXT_BASE - 0x1E00120 + 0x1000)))
{ printf '#define SMCPAD_TINYUSB %s\n' "${SMCPAD_TINYUSB:-0}"
  printf '#define SMCPAD_APP_DIR %s\n' "$APP_DIR"; } > "$APP/usb/smcpad_usb_build.h"
echo "USB stack: $([ "${SMCPAD_TINYUSB:-0}" = 1 ] && echo TinyUSB || echo JieLi SDK)"

docker image inspect jl-build >/dev/null 2>&1 || \
    docker build --platform linux/amd64 -t jl-build -f "$ROOT/docker/Dockerfile.jl-build" "$ROOT/docker"

# Link sdk.elf on its own: the SDK's default target races the post-build step under -j.
echo "linking at $TEXT_BASE"
docker run --rm --platform linux/amd64 -e TEXT_BASE="$TEXT_BASE" \
    -v "$TOOLCHAIN:/opt/jieli:ro" -v "$POSTBUILD:/opt/utils:ro" \
    -v "$SDK:/work" -v "$APP:/work/apps/smcpad" -v "$OUT:/out" \
    -v "$ROOT/third_party/tinyusb:/work/third_party/tinyusb:ro" \
    jl-build bash -c '
        set -e; ulimit -n 8192
        T=/opt/jieli/pi32v2/bin
        cd apps/smcpad/board/br25
        rm -f ../../../../cpu/br25/tools/sdk.elf
        # regenerate the linker script from sdk_ld.c with our board config (512K flash)
        make pre_build 2>&1 | grep -v "stack size limit" || true
        # ...then aim it wherever TEXT_BASE says, patching the generated script rather than
        # the SDK source (sdk_ld.c hardcodes the factory 0x1E00120).
        if [ "$TEXT_BASE" != "0x1E00120" ]; then
            sed -i "s/ORIGIN =  *0x1E00120/ORIGIN = $TEXT_BASE/" ../../../../cpu/br25/sdk.ld
            grep -n "ORIGIN = $TEXT_BASE" ../../../../cpu/br25/sdk.ld | head -1
        fi
        # 1 KB under UPDATA_BEG taken from the heap for smcpad_fault.c post-mortem
        # records: no section lands there, so the crt0 never clears it.
        sed -i "s/^RAM_LIMIT_H = UPDATA_BEG;/SMCPAD_NOINIT = UPDATA_BEG - 0x400;\nRAM_LIMIT_H = SMCPAD_NOINIT;/" \
            ../../../../cpu/br25/sdk.ld
        grep -q "^SMCPAD_NOINIT" ../../../../cpu/br25/sdk.ld
        # Every ASSERT and exception ends in the mask ROM chip_reset: route it through
        # smcpad_chip_reset (smcpad_fault.c), which records the caller first.
        sed -i "s/^\( *\)chip_reset = ABSOLUTE(0x117fcc);/\1chip_reset = smcpad_chip_reset;\n\1smcpad_rom_chip_reset = ABSOLUTE(0x117fcc);/" \
            ../../../../cpu/br25/sdk.ld
        grep -q "smcpad_rom_chip_reset" ../../../../cpu/br25/sdk.ld
        make -j8 ../../../../cpu/br25/tools/sdk.elf 2>&1 | grep -v "stack size limit"
        cd ../../../../cpu/br25/tools
        cp sdk.elf /out/sdk.elf
        # app.bin exactly as the SDK post-build builds it (cpu/br25/tools/download.sh):
        # .text (XIP, the crt0 copies what follows from flash by absolute address), then
        # .data, .data_code, then each overlay with its trailing 0xff run removed.
        W=$(mktemp -d)
        for s in text data data_code; do $T/objcopy -O binary -j .$s sdk.elf $W/$s.bin; done
        for s in aec wav ape flac m4a amr dts fm mp3 wma; do
            $T/objcopy -O binary -j .overlay_$s sdk.elf $W/${s}o.bin
            /opt/utils/remove_tailing_zeros -i $W/${s}o.bin -o $W/$s.bin -mark ff >/dev/null
        done
        cat $W/text.bin $W/data.bin $W/data_code.bin \
            $W/aec.bin $W/wav.bin $W/ape.bin $W/flac.bin $W/m4a.bin \
            $W/amr.bin $W/dts.bin $W/fm.bin $W/mp3.bin $W/wma.bin > /out/app.bin
        ls -l $W | sed "s/^/  /"
        $T/nm sdk.elf 2>/dev/null | grep -E " _start$" || true
    ' | tee "$ROOT/build_logs/smcpad.log"

ls -la "$OUT"
