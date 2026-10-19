#!/usr/bin/env bash
# Send a command to the firmware's bring-up console; the reply shows up in the terminal
# running host/dbg_listen.sh (which records the port in work/dbg_port).
#
#   host/dbg_send.sh h            list the commands ('?' works too, but quote it in zsh)
#   host/dbg_send.sh m            show the 595 pin assignment
#   host/dbg_send.sh o b4         drive PB4 high
#   host/dbg_send.sh w ff00       shift 0xff00 into the 595s and latch
#   host/dbg_send.sh s            scan every pin (watch the board)
#   host/dbg_send.sh p            try every clock/data/latch triple
#
# PORT=/dev/cu.usbmodemXXX overrides the recorded port.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-$(cat "$ROOT/work/dbg_port" 2>/dev/null || true)}"

if [ -z "$PORT" ]; then
    echo "no port known: start host/dbg_listen.sh first, or set PORT=/dev/cu.usbmodemXXX" >&2
    exit 1
fi
[ -e "$PORT" ] || { echo "port $PORT is gone (did the pad reset?)" >&2; exit 1; }
[ "$#" -ge 1 ] || { echo "usage: $0 <command> [args...]" >&2; exit 2; }

printf '%s\r' "$*" > "$PORT"
echo "sent: $*"
