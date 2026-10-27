#!/bin/sh
# Install the info screen on an E5 running OpenWrt (e5-linux's openwrt/),
# over SSH:
#
#   ./install.sh [HOST]        (default 192.168.9.1, the E5's USB LAN)
#
# Installs the packages in packages.txt from OpenWrt's repositories (the E5
# needs its WAN up for that), copies root/ onto the device, enables the
# service and (re)starts it.  SSH asks for root's password unless a key is
# set up (LuCI -> System -> Administration -> SSH-Keys).
set -eu
HOST=${1:-192.168.9.1}
HERE="$(cd "$(dirname "$0")" && pwd)"
python3 "$HERE/tools/build-native.py" --check
PKGS=$(grep -v '^#' "$HERE/packages.txt" | tr '\n' ' ')

echo "== packages on $HOST"
ssh "root@$HOST" "apk update >/dev/null && apk add $PKGS"
echo "== files"
REMOTE_DIR=$(ssh "root@$HOST" 'mktemp -d /tmp/e5-screen-install.XXXXXX')
case "$REMOTE_DIR" in /tmp/e5-screen-install.*) ;; *) echo 'Cannot prepare the device staging directory' >&2; exit 1 ;; esac
case "$REMOTE_DIR" in *[!A-Za-z0-9_./-]*) exit 1 ;; esac
cleanup() {
    ssh "root@$HOST" "rm -f '$REMOTE_DIR/install-files' '$REMOTE_DIR/package.tar.gz'; rmdir '$REMOTE_DIR'" >/dev/null 2>&1 || :
}
trap cleanup EXIT
ssh "root@$HOST" "umask 077; cat > '$REMOTE_DIR/install-files'" < "$HERE/root/usr/libexec/e5-infoscreen/install-files"
( cd "$HERE/root" && COPYFILE_DISABLE=1 tar --exclude .DS_Store -czf - . ) |
    ssh "root@$HOST" "umask 077; cat > '$REMOTE_DIR/package.tar.gz' && sh '$REMOTE_DIR/install-files' '$REMOTE_DIR/package.tar.gz'"
ssh "root@$HOST" '
        for f in /etc/uci-defaults/8[0-9]-e5-infoscreen*; do sh "$f" && rm -f "$f"; done;
        for s in t551m-screen-power t551m-sim-status e5-voice-audio; do
            /etc/init.d/$s enable && /etc/init.d/$s restart || exit 1
        done
        /etc/init.d/e5-infoscreen enable && /etc/init.d/e5-infoscreen restart'
echo "== done: the panel shows the info screen in a few seconds"
