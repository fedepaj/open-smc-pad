#!/bin/sh
# Build and upload the core self-test (arduino/tests/CoreTest), then read its report and check
# its BLE advertising from this machine. Usage: host/core_test.sh [serial port]
set -e
cd "$(dirname "$0")/.."
PORT="${1:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
OUT="${TMPDIR:-/tmp}/smcpad-coretest"
if ! arduino-cli compile --fqbn smcpad:br25:smcpad --output-dir "$OUT" arduino/tests/CoreTest > "$OUT.log" 2>&1; then
    cat "$OUT.log"; exit 1
fi
grep "Sketch uses" "$OUT.log"
arduino-cli upload --fqbn smcpad:br25:smcpad -p "$PORT" --input-dir "$OUT" arduino/tests/CoreTest | tail -1
sleep 2
exec uv run --quiet --with pyserial --with bleak python host/core_test.py "$PORT"
