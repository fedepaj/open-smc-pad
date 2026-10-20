#!/usr/bin/env bash
# Flash the application over USB through our bootloader (fw/boot, DFU). No Pico, no sudo.
#
#   host/dfu_flash.sh [app_area.bin]     # default: work/flash/fw_area.bin (host/package_fw.sh boot)
#
# If the application is running it is asked into the bootloader with the 1200-baud touch (open
# its serial port at 1200 baud, drop DTR), as Arduino boards do; if the bootloader is already
# up (asked for earlier, or no valid application) it is used as it is. The file is the
# application's app area in plaintext: the bootloader encrypts it for the chip, checks the
# directory CRC, and writes the directory last, so an interrupted transfer leaves the pad in
# the bootloader, never with half an application. After a good transfer the bootloader starts
# the new application by itself.
#
# Needs dfu-util (brew install dfu-util).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${1:-$ROOT/work/flash/fw_area.bin}"
DFU_ID=1209:0002                         # fw/boot/usb_desc.c
APP_NAME="SMC-PAD open fw"               # the application's USB product name

die() { echo "!! $*" >&2; exit 1; }
command -v dfu-util >/dev/null || die "dfu-util not found (brew install dfu-util)"
[ -f "$IMAGE" ] || die "no image at $IMAGE (host/package_fw.sh boot)"

in_dfu()   { local l; l="$(dfu-util -l 2>/dev/null)"; grep -q "\[$DFU_ID\]" <<<"$l"; }
app_port() { local r; r="$(ioreg -r -n "$APP_NAME" -l -w 0 2>/dev/null || true)"
             grep -m1 '"IOCalloutDevice"' <<<"$r" | cut -d'"' -f4; }
app_up()   { [ -n "$(app_port)" ]; }
wait_until() {   # wait_until <predicate> <timeout_s>
    local i=0
    while [ "$i" -lt "$(($2 * 4))" ]; do "$1" && return 0; sleep 0.25; i=$((i + 1)); done
    return 1
}

either_up() { app_up || in_dfu; }
wait_until either_up 10 || die "neither the bootloader ($DFU_ID) nor the application is on USB"

if ! in_dfu; then
    port="$(app_port)"
    [ -n "$port" ] || die "neither the bootloader ($DFU_ID) nor the application is on USB"
    echo "== asking the application on $port into the bootloader (1200-baud touch)"
    stty -f "$port" 1200 2>/dev/null; sleep 0.2
    wait_until in_dfu 10 || die "the bootloader did not show up"
fi

echo "== writing $(basename "$IMAGE") ($(wc -c < "$IMAGE" | tr -d ' ') bytes)"
LOG="$(dfu-util -d "$DFU_ID" -a 0 -D "$IMAGE" 2>&1)"; rc=$?
tr '\r' '\n' <<<"$LOG" | grep -E "Download done|DFU state|Error|error|Done" | grep -v "DFU suffix" | sed 's/^/   /'
[ "$rc" = 0 ] || die "dfu-util failed: the pad stays in the bootloader, run this again"

echo "== waiting for the application"
wait_until app_up 20 && echo "   up on $(app_port)" || die "the application did not come back"
