#!/usr/bin/env bash
# smcpad-tools for this computer: the packer (smcpad_pack), and on macOS what runs the JieLi
# Linux toolchain -- blink (x86-64 emulator, built here at a fixed commit) and the Linux
# libraries the toolchain links against, from Debian 9's archive.
#   arduino/release/build_host_tools.sh <version> <out dir>   -> smcpad-tools-<version>-<host>.tar.gz
set -euo pipefail
VER="$1"; mkdir -p "$2"; OUT="$(cd "$2" && pwd)"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BLINK_COMMIT=f006a4f
DEBS="https://archive.debian.org/debian/pool/main/g/glibc/libc6_2.24-11+deb9u4_amd64.deb
https://archive.debian.org/debian/pool/main/g/gcc-6/libstdc++6_6.3.0-18+deb9u1_amd64.deb
https://archive.debian.org/debian/pool/main/g/gcc-6/libgcc1_6.3.0-18+deb9u1_amd64.deb"

case "$(uname -s)-$(uname -m)" in
    Linux-x86_64)  HOST=x86_64-linux-gnu ;;
    Darwin-arm64)  HOST=arm64-apple-darwin ;;
    Darwin-x86_64) HOST=x86_64-apple-darwin ;;
    *) echo "unsupported host $(uname -s)-$(uname -m)" >&2; exit 1 ;;
esac
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
S="$TMP/smcpad-tools"
mkdir -p "$S"
cc -O2 -o "$S/smcpad_pack" "$ROOT/arduino/smcpad/tools/smcpad_pack.c"
cp "$ROOT/arduino/LICENSE" "$S/LICENSE"

if [ "$(uname -s)" = Darwin ]; then
    # blink, without its VFS (the default)
    git clone -q https://github.com/jart/blink "$TMP/blink-src"
    git -C "$TMP/blink-src" checkout -q "$BLINK_COMMIT"
    MAKE="$(command -v gmake || true)"         # blink wants GNU make 4: brew install make
    [ -n "$MAKE" ] || { echo "GNU make (gmake) needed for blink: brew install make" >&2; exit 1; }
    (cd "$TMP/blink-src" && ./configure >/dev/null && "$MAKE" -j"$(sysctl -n hw.ncpu)" o//blink/blink >/dev/null)
    mkdir -p "$S/blink/root"
    cp "$TMP/blink-src/o//blink/blink" "$S/blink/blink"
    cp "$TMP/blink-src/LICENSE" "$S/blink/LICENSE"
    # the dynamic loader and libraries the toolchain needs (glibc 2.24, libstdc++ of GCC 6)
    mkdir -p "$TMP/debs"
    for u in $DEBS; do
        f="$TMP/debs/$(basename "$u")"
        curl -sfL -o "$f" "$u"
        (mkdir -p "$f.x" && cd "$f.x" && ar x "$f" && mkdir data && tar -xf data.tar.* -C data)
    done
    R="$S/blink/root"
    mkdir -p "$R/lib64" "$R/lib/x86_64-linux-gnu" "$R/usr/lib/x86_64-linux-gnu"
    # a library by name, its symlinks followed inside the package (some are absolute, e.g.
    # /lib64/ld-linux-x86-64.so.2 -> /lib/x86_64-linux-gnu/ld-2.24.so)
    pick() {
        local f d
        f="$(find "$TMP"/debs/*.x/data -name "$1" | head -1)"
        d="${f%%/data/*}/data"
        while [ -L "$f" ]; do
            local t; t="$(readlink "$f")"
            case "$t" in /*) f="$d$t" ;; *) f="$(dirname "$f")/$t" ;; esac
        done
        echo "$f"
    }
    cp "$(pick ld-linux-x86-64.so.2)" "$R/lib64/ld-linux-x86-64.so.2"
    for l in libc.so.6 libdl.so.2 libm.so.6 libpthread.so.0 librt.so.1 libgcc_s.so.1; do
        cp "$(pick "$l")" "$R/lib/x86_64-linux-gnu/$l"
    done
    cp "$(pick libstdc++.so.6)" "$R/usr/lib/x86_64-linux-gnu/libstdc++.so.6"
    cat > "$R/NOTICE" <<'NOTE'
Linux libraries the JieLi toolchain (an x86-64 Linux program, run here through blink) links
against, unchanged from Debian 9 "stretch" binary packages:
  libc6 2.24-11+deb9u4        (GNU C Library: LGPL-2.1+)
  libstdc++6 6.3.0-18+deb9u1  (GCC runtime: GPL-3+ with the GCC Runtime Library Exception)
  libgcc1 6.3.0-18+deb9u1     (GCC runtime: GPL-3+ with the GCC Runtime Library Exception)
Sources: https://archive.debian.org/debian/pool/main/g/glibc/ and .../g/gcc-6/
(glibc_2.24-11+deb9u4.dsc, gcc-6_6.3.0-18+deb9u1.dsc), and the copyright files in each package.
NOTE
fi
find "$S" -name .DS_Store -delete
COPYFILE_DISABLE=1 tar -C "$TMP" -czf "$OUT/smcpad-tools-$VER-$HOST.tar.gz" smcpad-tools
echo "$OUT/smcpad-tools-$VER-$HOST.tar.gz"
