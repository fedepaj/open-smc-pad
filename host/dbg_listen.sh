#!/usr/bin/env bash
# Listen to the firmware's debug serial (the USB CDC port our firmware exposes).
#
#   host/dbg_listen.sh                      wait for the pad's port to appear, then print it
#   host/dbg_listen.sh /dev/cu.usbmodemXXX  use this port
#
# The Pico (USB_KEY) is also a cu.usbmodem* port, so with no argument this does not guess:
# it takes a snapshot of the ports that exist now and attaches to the first *new* one, which
# is the pad as it enumerates.  Start it before powering the pad on (or before flashing) and
# leave it running.  Ctrl-C to stop.
set -uo pipefail

JL_VID=19530        # 0x4C4A, the vendor id in the SDK's CDC descriptor ("JL")
SINCO_VID=17235     # 0x4353, the stock firmware
WAIT_S="${WAIT_S:-180}"

ports() { ls /dev/cu.usbmodem* 2>/dev/null | sort; }
has_vid() { ioreg -p IOUSB -l -w 0 2>/dev/null | grep -q "\"idVendor\" = $1"; }

listen() {
    local port="$1"
    local root
    root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    mkdir -p "$root/work"
    local log="$root/work/dbg-$(date +%Y%m%d-%H%M%S).log"
    printf '%s\n' "$port" > "$root/work/dbg_port"     # host/dbg_send.sh picks it up
    echo "listening on $port (Ctrl-C to stop)"
    echo "send commands from another terminal:  host/dbg_send.sh h"
    echo "saving everything to $log"
    echo "---------------------------------------------------------------"
    stty -f "$port" 115200 cs8 -cstopb -parenb raw 2>/dev/null    # CDC ignores these
    # Every line gets a wall-clock timestamp with milliseconds. When the pad resets, the gap
    # between its last line and "port closed" tells a fault (instant) from a hang that the
    # watchdog ends (~8s, the SDK's WDT_8S) -- which look identical without timing.
    cat "$port" | perl -MTime::HiRes=time -MPOSIX=strftime -ne '
        $| = 1; my $t = time;
        printf "%s.%03d  %s", strftime("%H:%M:%S", localtime $t), ($t - int $t) * 1000, $_;
    ' | tee "$log"
    echo "$(perl -MTime::HiRes=time -MPOSIX=strftime -e '
        my $t = time; printf "%s.%03d", strftime("%H:%M:%S", localtime $t), ($t - int $t) * 1000;
    ')  ---- port closed (pad reset or unplugged)" | tee -a "$log"
}

if [ "$#" -ge 1 ]; then
    [ -e "$1" ] || { echo "no such port: $1" >&2; exit 1; }
    listen "$1"
fi

before="$(ports)"
if [ -n "$before" ]; then
    echo "ports already present (ignored):"
    printf '  %s\n' $before
fi
echo "waiting up to ${WAIT_S}s for the pad's serial port to appear..."
echo "(power-cycle the pad, or run 'sudo host/flash_fw.sh' in another terminal)"

deadline=$((SECONDS + WAIT_S))
while [ "$SECONDS" -lt "$deadline" ]; do
    current="$(ports)"
    new="$(comm -13 <(printf '%s\n' $before) <(printf '%s\n' $current) 2>/dev/null | head -1)"
    if [ -n "$new" ] && [ -e "$new" ]; then
        echo
        echo "new port: $new"
        sleep 0.5                       # let the CDC settle before the first read
        listen "$new"
    fi
    # Forget ports that have gone away, so one that comes back counts as new: reflashing or
    # power-cycling the pad gives its port the same name it had before.
    before="$(comm -12 <(printf '%s\n' $before) <(printf '%s\n' $current) 2>/dev/null)"
    sleep 0.5
done

echo
echo "timed out: no new serial port." >&2
if has_vid "$SINCO_VID"; then
    echo "  the pad enumerates as SINCO (0x4353) -- that is the STOCK firmware." >&2
elif has_vid "$JL_VID"; then
    echo "  a JieLi device (0x4C4A) is attached but no port appeared while waiting." >&2
    if [ -n "$(ports)" ]; then
        echo "  its port is probably one of these, already present before I started -- pass it:" >&2
        for p in $(ports); do
            echo "    $0 $p" >&2
        done
    else
        echo "  it exposes no serial port, so that is ROM download mode, not our firmware." >&2
    fi
else
    echo "  no JieLi (0x4C4A) and no SINCO (0x4353) device on the bus: either the pad is off" >&2
    echo "  or unplugged, or our firmware is not getting as far as USB init." >&2
fi
exit 1
