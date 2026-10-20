#!/usr/bin/env bash
# Build the SMC-PAD bootloader (fw/boot) with the JieLi pi32v2 toolchain in Docker.
# Output: fw/boot/out/boot.elf, boot.bin, boot.dis
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLCHAIN="$(ls -d "$ROOT"/tools/jl-toolchain/jieli-linux-toolchains-*/ | head -1)"
BOOT="$ROOT/fw/boot"
mkdir -p "$BOOT/out"

docker run --rm --platform linux/amd64 \
    -v "$TOOLCHAIN:/opt/jieli:ro" -v "$BOOT:/boot" \
    jl-build bash -c '
        set -e
        T=/opt/jieli/pi32v2/bin
        cd /boot
        $T/clang -target pi32v2 -mcpu=r3 -integrated-as -Oz -fno-common \
            -ffunction-sections -fdata-sections -Wall -c boot.c -o out/boot.o
        $T/ld -T boot.ld --gc-sections out/boot.o -o out/boot.elf -Map out/boot.map
        $T/objcopy -O binary -j .text out/boot.elf out/boot.bin
        $T/objdump -d out/boot.elf > out/boot.dis
    '
ls -l "$BOOT/out/boot.bin"
