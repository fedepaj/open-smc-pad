#!/usr/bin/env bash
# Build the SMC-PAD bootloader (fw/boot) with the JieLi pi32v2 toolchain in Docker.
# Output: fw/boot/out/boot.elf, boot.bin (what goes in the app area at 0x1000), boot.dis, boot.map
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLCHAIN="$(ls -d "$ROOT"/tools/jl-toolchain/jieli-linux-toolchains-*/ | head -1)"
mkdir -p "$ROOT/fw/boot/out"

docker run --rm --platform linux/amd64 \
    -v "$TOOLCHAIN:/opt/jieli:ro" -v "$ROOT/fw:/fw" -v "$ROOT/third_party/tinyusb:/tinyusb:ro" \
    jl-build bash -c '
        set -e
        T=/opt/jieli/pi32v2/bin
        cd /fw/boot
        SRCS="boot.c dfu.c flash.c usb_desc.c libc.c /fw/smcpad/usb/dcd_br25.c
              /tinyusb/src/tusb.c /tinyusb/src/common/tusb_fifo.c /tinyusb/src/device/usbd.c
              /tinyusb/src/device/usbd_control.c /tinyusb/src/class/dfu/dfu_device.c"
        OBJS=
        for s in $SRCS; do
            [ -f "$s" ] || continue
            o=out/$(basename "$s" .c).o
            $T/clang -target pi32v2 -mcpu=r3 -integrated-as -Oz -fno-common -ffreestanding \
                -ffunction-sections -fdata-sections -Wall -Wno-unused-function \
                -mllvm -enable-ipra=false \
                -I. -Iinclude -I/tinyusb/src -c "$s" -o "$o"
            OBJS="$OBJS $o"
        done
        $T/ld -T boot.ld --gc-sections $OBJS -o out/boot.elf -Map out/boot.map
        $T/objcopy -O binary -j .start out/boot.elf out/start.bin
        $T/objcopy -O binary -j .ram out/boot.elf out/ram.bin
        cat out/start.bin out/ram.bin > out/boot.bin
        $T/objdump -d out/boot.elf > out/boot.dis
        $T/objdump -h out/boot.elf | grep -E "start|ram|bss"
    '
ls -l "$ROOT/fw/boot/out/boot.bin"
