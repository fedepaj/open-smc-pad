#!/bin/bash
# Write a firmware into the app area through the ROM download mode (UBOOT), verify it and boot
# it. Full replacement: sector 0 / uboot are never touched, the app area 0x1000..0x26000 is
# erased and rewritten.
#
#   host/flash_fw.sh            # our firmware  (work/flash/fw_area_raw.bin)
#   host/flash_fw.sh stock      # the factory firmware back
#
# Runs as you, not root. The one privileged step -- detaching macOS's mass-storage driver from
# the chip -- is the root helper /usr/local/libexec/smcpad-usb-bridge, started with `sudo -n`
# (installed once by host/install-bridge.sh). Everything else is unprivileged.
#
# Getting into download mode needs the Pico (hw/jl-usbkey) clocking the USB_KEY on D+/D- while
# the chip starts. How the chip is made to start depends on what is running:
#  - our firmware: 'J' on its console makes it drop off USB and re-enter the mask ROM -- no
#    hands needed;
#  - nothing on the bus (a firmware stuck in a reset loop): the watchdog restarts it through the
#    ROM every few seconds, so keying is usually enough to catch it;
#  - the stock firmware, or anything else: power-cycle the pad by hand while the key is sent.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODE="${1:-fw}"
SOCK=/var/run/smcpad/jl.sock
HELPER=/usr/local/libexec/smcpad-usb-bridge
BRIDGE_LOG="$ROOT/work/bridge.log"
AREA=0x1000
AREA_LEN=0x25000
UBOOT_VID=19530          # 0x4C4A, JieLi: the ROM loader *and* our firmware
OUR_PID=16725            # 0x4155, our firmware's CDC
APP_VID=17235            # 0x4353, the stock firmware (SINCO)
OUR_NAME="USB Composite Device"         # our firmware with the JieLi SDK's USB stack
OUR_NAME_TUSB="SMC-PAD open fw"         # ... with TinyUSB (pid.codes test id 1209:0001)

case "$MODE" in
    fw)    IMAGE="$ROOT/work/flash/fw_area_raw.bin"
           EXPECTED="$ROOT/work/flash/expected_with_fw.bin"
           VERIFY="$ROOT/work/flash/verify_last.bin" ;;
    stock) IMAGE="$ROOT/work/restore/code_0x1000_0x26000.bin"
           EXPECTED="$ROOT/dumps/original_physical_full.bin"
           VERIFY="$ROOT/work/restore/verify_last.bin" ;;
    *)     echo "usage: $0 [fw|stock]" >&2; exit 2 ;;
esac

PY="$ROOT/host/.venv-uboot/bin/python"
TOOL="$ROOT/tools/jl-uboot-tool/jluboottool.py"

die()  { echo "!! $*" >&2; exit 1; }
note() { echo; echo "== $*"; }

[ "$(id -u)" != 0 ] || die "run this as yourself, not with sudo (only the bridge helper needs root)"
for f in "$IMAGE" "$EXPECTED" "$PY" "$TOOL"; do
    [ -e "$f" ] || die "missing $f"
done
[ -x "$HELPER" ] || die "bridge helper not installed: (cd host/bridge-rs && cargo build --release), then sudo host/install-bridge.sh"
sudo -n -l "$HELPER" >/dev/null 2>&1 || die "no passwordless sudo rule for $HELPER: run sudo host/install-bridge.sh"

# ------------------------------------------------------------------- USB helpers
# Never `ioreg | grep -q` here: grep -q exits on the first match, ioreg dies of SIGPIPE and
# pipefail turns the match into a failure -- at random, depending on buffering.
bus()            { ioreg -p IOUSB -l -w 0 2>/dev/null || true; }
bus_has()        { local b; b="$(bus)"; grep -qiE "$1" <<<"$b"; }
usb_has()        { bus_has "\"idVendor\" = $1\$"; }
our_fw_present() { bus_has "\"idProduct\" = $OUR_PID\$|\"USB Product Name\" = \"SMC.PAD open fw\""; }
our_port()       { local p; p="$(port_of "$OUR_NAME")"; [ -n "$p" ] || p="$(port_of "$OUR_NAME_TUSB")"; echo "$p"; }
stock_present()  { usb_has "$APP_VID"; }
# The loader calls itself "BR25UBOOT1.00" in its *vendor* string; fall back to "a JieLi device
# that is not our firmware" in case another chip words it differently.
uboot_present() {
    local b; b="$(bus)"
    grep -qi '"USB Vendor Name" = "[^"]*UBOOT' <<<"$b" && return 0
    grep -q "\"idVendor\" = $UBOOT_VID\$" <<<"$b" && ! grep -q "\"idProduct\" = $OUR_PID\$" <<<"$b"
}
# Serial port of the USB device with that registry name ("Pico", our firmware's CDC, ...).
port_of() {
    local r; r="$(ioreg -r -n "$1" -l -w 0 2>/dev/null || true)"
    grep -m1 '"IOCalloutDevice"' <<<"$r" | cut -d'"' -f4
}

wait_until() {   # wait_until <predicate fn> <timeout_s> [message]
    local fn="$1" timeout="$2" msg="${3:-}" i=0
    while [ "$i" -lt "$((timeout * 4))" ]; do
        "$fn" && return 0
        [ "$i" = 0 ] && [ -n "$msg" ] && echo "   $msg"
        sleep 0.25; i=$((i + 1))
    done
    return 1
}

pico_port() {
    local p; p="$(port_of Pico)"
    [ -n "$p" ] || die "no Pico on USB -- the USB_KEY sender (hw/jl-usbkey) is needed"
    echo "$p"
}
# Write to a CDC port and keep it open (with a reader draining it) for a moment: a bare
# `printf > port` opens and closes it at once, and our firmware's console loses the command.
port_send() {   # port_send <port> <text>
    local pid
    exec 3<>"$1" || return 1
    stty -f "$1" 115200 raw -echo 2>/dev/null
    cat <&3 > /dev/null & pid=$!
    printf '%s' "$2" >&3
    sleep 0.5
    kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    exec 3>&-
}
pico_cmd() { port_send "$(pico_port)" "$1"; }

# The Pico only starts keying once D+ has stayed low for 10 ms (the pad detached or off), so an
# armed attempt is harmless to a pad still on the bus. One attempt lasts 60 s; re-arm while
# waiting, and release the bus ('x') if nothing came of it.
start_key() { pico_cmd r; echo "   USB_KEY armed on $(pico_port)"; }
key_until_uboot() {   # key_until_uboot <timeout_s> [message]
    local timeout="$1" msg="${2:-}" waited=0 step
    while [ "$waited" -lt "$timeout" ]; do
        start_key
        step=$(( timeout - waited < 50 ? timeout - waited : 50 ))
        wait_until uboot_present "$step" "$msg" && return 0
        msg=""; waited=$((waited + step))
    done
    pico_cmd x
    return 1
}

jl() { "$PY" "$TOOL" --device "$SOCK" "$@"; }

echo "image    $IMAGE  ($(stat -f%z "$IMAGE") bytes)"
echo "expected $EXPECTED"

# ---------------------------------------------------------------- download mode
note "entering download mode"
if uboot_present; then
    echo "   already in download mode"
elif our_fw_present; then
    port="$(our_port)"
    [ -n "$port" ] || die "our firmware is on USB but its serial port was not found"
    echo "   our firmware is running on $port: asking it to jump to the mask ROM"
    start_key                               # arms now, keys once 'J' has dropped D+
    port_send "$port" $'J\r'
    if ! wait_until uboot_present 8 && our_fw_present; then
        echo "   no jump yet: 1200-baud touch, then 'J' once more"
        stty -f "$port" 1200 2>/dev/null; sleep 0.2                 # open at 1200, close: DTR drops
        wait_until uboot_present 5 || port_send "$port" $'J\r'
    fi
    if ! wait_until uboot_present 10; then
        echo "   not caught (a firmware whose 'J' leaves D+ pulled up cannot be): hands needed"
        key_until_uboot 300 "turn the pad OFF, wait a second, and ON again ..." || die "no download mode"
    fi
elif stock_present; then
    echo "   the stock firmware is running and cannot be told to jump: hands needed"
    key_until_uboot 300 "turn the pad OFF, wait a second, and ON again ..." || die "no download mode"
else
    echo "   nothing on the bus: keying and waiting for a watchdog restart to catch it"
    key_until_uboot 15 || key_until_uboot 300 "not caught; power-cycle the pad by hand now ..." \
        || die "no download mode"
fi
echo "   chip is in download mode"

# ----------------------------------------------------------------------- bridge
note "starting the USB bridge (root helper, no password)"
for _ in $(seq 60); do [ -S "$SOCK" ] || break; sleep 0.25; done   # a previous one still idling
rm -f "$BRIDGE_LOG"                     # may be root's, left by the old Python bridge
sudo -n "$HELPER" > "$BRIDGE_LOG" 2>&1 &
for _ in $(seq 60); do [ -S "$SOCK" ] && break; sleep 0.25; done
[ -S "$SOCK" ] || { cat "$BRIDGE_LOG"; die "the bridge did not come up"; }
sed -n 1,2p "$BRIDGE_LOG" | sed 's/^/   /'

# ------------------------------------------------------------- write and verify
note "erase $AREA +$AREA_LEN, write $(basename "$IMAGE"), read back the whole flash"
jl "erase $AREA $AREA_LEN" "write $AREA $IMAGE" "read 0 0x80000 $VERIFY" \
    || die "the tool failed (see above)"
[ -f "$VERIFY" ] || die "no read-back file"

# Strict where it matters, informative elsewhere. Sector 0 (uboot, boot pointer) must be
# untouched and the area we just wrote must match exactly; a difference in either refuses to
# boot. Differences anywhere else are only reported: free flash is where the console's write
# tests and staging images go, so leftovers there say nothing about this install.
note "verifying"
python3 - "$VERIFY" "$EXPECTED" "$AREA" "$AREA_LEN" <<'EOF' || die "verification failed -- read-back kept at $VERIFY"
import sys
got = open(sys.argv[1], "rb").read()
want = open(sys.argv[2], "rb").read()
area, area_len = int(sys.argv[3], 0), int(sys.argv[4], 0)
if len(got) != len(want):
    sys.exit(f"   read-back is {len(got)} bytes, expected {len(want)}")

def diff_ranges(lo, hi):
    out, start = [], None
    for i in range(lo, hi):
        if got[i] != want[i]:
            start = i if start is None else start
        elif start is not None:
            out.append((start, i)); start = None
    if start is not None:
        out.append((start, hi))
    return out

failed = False
for name, lo, hi in [("sector 0 (uboot + boot pointer)", 0, 0x1000),
                     ("app area we just wrote", area, area + area_len)]:
    bad = diff_ranges(lo, hi)
    if bad:
        failed = True
        print(f"   MISMATCH in {name}: " + ", ".join(f"{a:#x}..{b:#x}" for a, b in bad[:6]))
    else:
        print(f"   OK  {name} ({lo:#x}..{hi:#x})")
elsewhere = diff_ranges(area + area_len, len(got))
if elsewhere:
    print("   note: free/data flash differs from the reference at " +
          ", ".join(f"{a:#x}..{b:#x}" for a, b in elsewhere[:6]) +
          (" ..." if len(elsewhere) > 6 else "") + " (test leftovers, not this install)")
sys.exit(1 if failed else 0)
EOF

# ------------------------------------------------------------------------- boot
note "booting it"
jl "reset 1" || echo "   (reset reported an error -- power-cycle the pad by hand)"
if [ "$MODE" = fw ]; then
    our_port_ready() { [ -n "$(our_port)" ]; }
    wait_until our_port_ready 10 && echo "   our firmware is up on $(our_port)" \
        || echo "   our firmware did not show up on USB within 10 s"
else
    wait_until stock_present 10 && echo "   the stock firmware is up" \
        || echo "   the stock firmware did not show up on USB within 10 s"
fi
