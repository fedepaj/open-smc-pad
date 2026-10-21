#!/usr/bin/env bash
# The Windows packages, cross-built with mingw-w64 (apt install mingw-w64 zip):
#   smcpad-tools-<version>-i686-mingw32.zip            smcpad_pack.exe, jl.exe
#   jieli-toolchain-<toolchain>-i686-mingw32.zip       our fetcher: jieli_fetch.cmd + innoextract;
#                                                       JieLi's compiler arrives at the first build
#   arduino/release/build_windows_tools.sh <version> <out dir>
set -euo pipefail
VER="$1"; mkdir -p "$2"; OUT="$(cd "$2" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TC_VER=20260730.1
CC=i686-w64-mingw32-gcc
INNOEXTRACT_URL=https://github.com/dscharrer/innoextract/releases/download/1.9/innoextract-1.9-windows.zip
INNOEXTRACT_SHA256=6989342c9b026a00a72a38f23b62a8e6a22cc5de69805cf47d68ac2fec993065
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

T="$TMP/smcpad-tools"
mkdir -p "$T"
$CC -O2 -static -D__USE_MINGW_ANSI_STDIO=1 -o "$T/smcpad_pack.exe" "$ROOT/arduino/smcpad/tools/smcpad_pack.c"
$CC -O2 -static -o "$T/jl.exe" "$ROOT/arduino/smcpad/tools/jl_win.c"
cp "$ROOT/arduino/LICENSE" "$T/LICENSE"
(cd "$TMP" && zip -qr "$OUT/smcpad-tools-$VER-i686-mingw32.zip" smcpad-tools)

J="$TMP/jieli-toolchain"
mkdir -p "$J"
curl -sfL -o "$TMP/ie.zip" "$INNOEXTRACT_URL"
echo "$INNOEXTRACT_SHA256  $TMP/ie.zip" | sha256sum -c - >/dev/null
unzip -q "$TMP/ie.zip" -d "$TMP/ie"
cp "$TMP/ie/innoextract.exe" "$J/"
cat "$TMP"/ie/license/* > "$J/innoextract-LICENSE.txt"
sed 's/$/\r/' "$ROOT/arduino/release/windows/jieli_fetch.cmd" > "$J/jieli_fetch.cmd"      # CRLF
sed 's/$/\r/' "$ROOT/arduino/release/windows/README.txt" > "$J/README.txt"
(cd "$TMP" && zip -qr "$OUT/jieli-toolchain-$TC_VER-i686-mingw32.zip" jieli-toolchain)
ls -l "$OUT"/*mingw32.zip
