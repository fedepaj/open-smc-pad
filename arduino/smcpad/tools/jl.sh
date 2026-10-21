#!/usr/bin/env bash
# Run a JieLi pi32v2 tool (clang, clang++, ld, ar, objcopy): natively on Linux x86-64, through
# blink (x86-64 emulator, interpreter mode: byte-exact, the JIT is not) on macOS.
#   jl.sh <toolchain dir> <host tools dir> <tool> [args...]
# toolchain dir: jieli-linux-toolchains-*; host tools dir (macOS): blink/blink = a blink build
# without its VFS, blink/root = the Linux libraries the toolchain links against (glibc 2.24).
TC="$(cd "$1" && pwd -P)"                         # real paths: blink does not follow symlinks
HOSTDIR="$(cd "$2" && pwd -P)"
TOOL="$3"; shift 3
case "$TOOL" in
    clang)   BIN="$TC/common/bin/clang" ;;
    clang++) BIN="$TC/common/bin/clang"; set -- --driver-mode=g++ "$@" ;;   # clang++ is a symlink
    *)       BIN="$TC/pi32v2/bin/$TOOL" ;;
esac
BLINK_BIN="$(readlink "$HOSTDIR/blink/blink" || echo "$HOSTDIR/blink/blink")"
BLINK_ROOT="$(cd "$HOSTDIR/blink/root" 2>/dev/null && pwd -P)"
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) exec "$BIN" "$@" ;;
    Darwin-*)
        # clear the signal mask and handlers first: without it the clang driver never hears
        # its cc1 child exit when the IDE starts us in the background
        BLINK_OVERLAYS="$BLINK_ROOT:" exec /usr/bin/perl -e \
            'use POSIX; sigprocmask(SIG_SETMASK, POSIX::SigSet->new); $SIG{$_}="DEFAULT" for keys %SIG; exec { $ARGV[0] } @ARGV or die' \
            "$BLINK_BIN" -j "$BIN" "$@" ;;
    *) echo "jl.sh: no JieLi toolchain for $(uname -s)-$(uname -m) yet" >&2; exit 1 ;;
esac
