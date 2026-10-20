#!/usr/bin/env bash
# Build the SMC-PAD bootloader (fw/boot) with the JieLi pi32v2 toolchain in Docker.
# Output: fw/boot/out/boot.{elf,bin,dis,map} (the bootloader, app area at 0x1000) and
#         fw/boot/out/install.{elf,bin,dis,map} (the stock -> ours installer, fw/boot/install.c)
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
        # -enable-ipra=false: the JieLi clang otherwise trusts the body of a weak default
        # across the call (docs/bootloader.md, "Pitfalls")
        cc() {
            $T/clang -target pi32v2 -mcpu=r3 -integrated-as -Oz -fno-common -ffreestanding \
                -ffunction-sections -fdata-sections -Wall -Wno-unused-function \
                -mllvm -enable-ipra=false \
                -I. -Iinclude -I/tinyusb/src -c "$1" -o "out/$(basename "$1" .c).o"
        }
        # link <name> <script> <sources...>: out/<name>.elf, .bin (.start then .ram), .dis, .map
        link() {
            name=$1 script=$2; shift 2
            objs=
            for s in "$@"; do
                [ -f "$s" ] || continue
                cc "$s"; objs="$objs out/$(basename "$s" .c).o"
            done
            $T/ld -T $script --gc-sections $objs -o out/$name.elf -Map out/$name.map
            $T/objcopy -O binary -j .start out/$name.elf out/$name.start.bin
            $T/objcopy -O binary -j .ram out/$name.elf out/$name.ram.bin
            cat out/$name.start.bin out/$name.ram.bin > out/$name.bin
            $T/objdump -d out/$name.elf > out/$name.dis
            echo "$name:"; $T/objdump -h out/$name.elf | grep -E "start|ram|bss"
        }
        link boot boot.ld boot.c dfu.c flash.c jlfs.c usb_desc.c libc.c /fw/smcpad/usb/dcd_br25.c \
            /tinyusb/src/tusb.c /tinyusb/src/common/tusb_fifo.c /tinyusb/src/device/usbd.c \
            /tinyusb/src/device/usbd_control.c /tinyusb/src/class/dfu/dfu_device.c
        link install install.ld install.c flash.c jlfs.c libc.c
    '
ls -l "$ROOT/fw/boot/out/boot.bin" "$ROOT/fw/boot/out/install.bin"
