#!/usr/bin/env bash
# Flash the application over USB through our bootloader (fw/boot, DFU). No Pico, no sudo.
#
#   host/dfu_flash.sh [app_area.bin]     # default: work/flash/fw_area.bin (host/package_fw.sh boot)
#   host/dfu_flash.sh --bootloader [pkg] # update the bootloader itself (work/flash/boot_update.bin)
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
ALT=0
if [ "${1:-}" = "--bootloader" ]; then
    # DFU alt 2: the bootloader checks the package, points sector 0 at the installer and
    # resets; the installer replaces 0x1000..0x9000 and points sector 0 back (docs/bootloader.md)
    ALT=2; shift
    IMAGE="${1:-$ROOT/work/flash/boot_update.bin}"
else
    IMAGE="${1:-$ROOT/work/flash/fw_area.bin}"
fi
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

# the version line the bootloader returns on alt 2 (its USB descriptors never change: macOS
# would take a changed device for a new accessory and hold it back until approved)
boot_version() {
    local f; f="$(mktemp)"; rm -f "$f"
    dfu-util -d "$DFU_ID" -a 2 -U "$f" >/dev/null 2>&1
    if [ -s "$f" ]; then tr -d '\n' < "$f"; else echo "(version not readable: older bootloader)"; fi
    rm -f "$f"
}
echo "== bootloader version $(boot_version)"
echo "== writing $(basename "$IMAGE") ($(wc -c < "$IMAGE" | tr -d ' ') bytes) to alt $ALT"
LOG="$(dfu-util -d "$DFU_ID" -a "$ALT" -D "$IMAGE" 2>&1)"; rc=$?
tr '\r' '\n' <<<"$LOG" | grep -E "Download done|DFU state|Error|error|Done" | grep -v "DFU suffix" | sed 's/^/   /'
if grep -q "dfuERROR" <<<"$LOG"; then
    die "the bootloader refused it (nothing changed); it stays in DFU mode -- dfu-util -a 0 -e goes back"
fi
if [ "$rc" != 0 ]; then
    # alt 2: once the package checks out the bootloader resets into the installer right after
    # the manifestation, so dfu-util loses it while asking for the final status -- expected
    if [ "$ALT" = 2 ] && grep -q "dfuMANIFEST" <<<"$LOG" && ! grep -q "dfuERROR" <<<"$LOG"; then
        echo "   (the bootloader reset into the installer, as it should)"
    else
        die "dfu-util failed: the pad stays in the bootloader, run this again"
    fi
fi

echo "== waiting for the application"
if ! wait_until app_up 30; then
    in_dfu && die "back in the bootloader (version $(boot_version)), not the application" \
           || die "neither the application nor the bootloader came back"
fi
echo "   up on $(app_port)"

if [ "$ALT" = 2 ]; then
    echo "== checking the bootloader version"
    sleep 4                                     # the console must be listening for the touch
    stty -f "$(app_port)" 1200 2>/dev/null; sleep 0.2
    wait_until in_dfu 15 || die "the bootloader did not come up for the check"
    echo "   bootloader version now $(boot_version)"
    dfu-util -d "$DFU_ID" -a 0 -e >/dev/null 2>&1
    wait_until app_up 30 && echo "   back in the application" || die "the application did not come back"
fi
