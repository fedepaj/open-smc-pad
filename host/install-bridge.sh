#!/bin/bash
# One-time setup: install the USB bridge root helper and let the current user run it without a
# password, so flashing through the ROM download mode needs no sudo prompt afterwards.
#
#   cd host/bridge-rs && cargo build --release && cd ../..
#   sudo host/install-bridge.sh
#
# What it does, and why it is safe to allow without a password:
#  - copies host/bridge-rs/target/release/smcpad-usb-bridge to /usr/local/libexec/, owned by
#    root and not writable by anyone else (a helper a user could overwrite would be a free root
#    shell);
#  - adds /etc/sudoers.d/smcpad-usb-bridge allowing exactly that binary, no arguments;
#  - the helper takes no arguments, opens only JieLi download-mode devices, uses one fixed
#    socket in a root-owned directory and links libusb statically.
# To undo:  sudo rm /usr/local/libexec/smcpad-usb-bridge /etc/sudoers.d/smcpad-usb-bridge
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/host/bridge-rs/target/release/smcpad-usb-bridge"
DEST_DIR=/usr/local/libexec
DEST="$DEST_DIR/smcpad-usb-bridge"
RULE=/etc/sudoers.d/smcpad-usb-bridge

die() { echo "!! $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run with sudo"
USER_NAME="${SUDO_USER:-}"
[ -n "$USER_NAME" ] && [ "$USER_NAME" != root ] || die "run through sudo from your own account"
[ -x "$SRC" ] || die "build it first: (cd host/bridge-rs && cargo build --release)"

# Every directory on the way to the binary must be root-owned and not group/world writable.
for d in /usr /usr/local "$DEST_DIR"; do
    [ -e "$d" ] || continue
    owner=$(stat -f %Su "$d"); mode=$(stat -f %Lp "$d")
    [ "$owner" = root ] || die "$d is owned by $owner, not root; refusing"
    case "$mode" in *[2367]?|*?[2367]) die "$d is group/world writable ($mode); refusing" ;; esac
done

install -d -o root -g wheel -m 755 "$DEST_DIR"
install -o root -g wheel -m 755 "$SRC" "$DEST"
echo "installed $DEST"

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
printf '%s ALL=(root) NOPASSWD: %s ""\n' "$USER_NAME" "$DEST" > "$TMP"
# A broken sudoers file can lock sudo out entirely: validate before installing it.
visudo -cf "$TMP" >/dev/null || die "generated sudoers rule failed validation; nothing installed"
install -o root -g wheel -m 440 "$TMP" "$RULE"
visudo -c >/dev/null || { rm -f "$RULE"; die "sudoers broke after install; rule removed"; }
echo "installed $RULE:"
sed 's/^/    /' "$RULE"

echo
echo "check, as $USER_NAME:  sudo -n $DEST   (should say 'no JieLi ROM loader' after ~10s, no prompt)"
