#!/bin/sh
# Installs the SMC-PAD control surface script into Ableton Live's User Library.
# Live picks it up at its next start: Settings > Link, Tempo & MIDI > Control Surface "SMC_PAD".
set -e
SRC="$(cd "$(dirname "$0")/.." && pwd)/ableton/SMC_PAD"
DST="$HOME/Music/Ableton/User Library/Remote Scripts/SMC_PAD"
mkdir -p "$DST"
cp "$SRC"/*.py "$DST"/
rm -rf "$DST/__pycache__"
echo "installed in $DST"
