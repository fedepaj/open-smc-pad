#!/bin/sh
# Builds a sketch for the host against the emulated runtime (sim_runtime.cpp): the sketch,
# the platform's libraries and the core are compiled unchanged.
#   arduino/sim/build.sh <sketch dir> [output]      -> a program: <scenario> <ms> [fps]
set -e
SIM="$(cd "$(dirname "$0")" && pwd)"
A="$(cd "$SIM/.." && pwd)"
CORE="$A/smcpad/cores/smcpad"
API="$CORE/api"
SKETCH="$(cd "$1" && pwd)"
OUT="${2:-$SKETCH/sim-$(basename "$SKETCH")}"
RT="$A/smcpad/system/include"
[ -f "$RT/smcpad_rt.h" ] || RT="$A/../fw/smcpad/arduino"
OBJ="$(mktemp -d)"
trap 'rm -rf "$OBJ"' EXIT

INC="-I$CORE -I$A/smcpad/variants/smcpad -I$RT -I$SKETCH"
for l in "$A"/smcpad/libraries/*/src; do INC="$INC -I$l"; done
FLAGS="-O1 -g -w -DSMCPAD_SIM=1"

c() { clang -std=gnu11 $FLAGS $INC -c "$1" -o "$OBJ/$(basename "$1").o"; }
cxx() { clang++ -std=gnu++14 $FLAGS $INC -c "$@"; }

c "$CORE/itoa.c"
c "$SIM/sim_dtostrf.c"
for f in "$SIM/sim_runtime.cpp" "$CORE/wiring.cpp" "$CORE/SerialUSB.cpp" "$API/String.cpp" "$API/Print.cpp" \
         "$API/Stream.cpp" "$API/Common.cpp" "$A"/smcpad/libraries/*/src/*.cpp; do
  cxx "$f" -o "$OBJ/$(basename "$f").o"
done
# the sketch: its .ino files in one translation unit, as the Arduino builder does
cat "$SKETCH"/*.ino > "$OBJ/sketch.cpp"
cxx -include Arduino.h "$OBJ/sketch.cpp" -o "$OBJ/sketch.o"
clang++ "$OBJ"/*.o -o "$OUT"
echo "$OUT"
