#!/bin/sh
# Remove the module. Fails loudly if something still holds /dev/vws open,
# because that is almost always a vwsd left running in another terminal.
set -e

SUDO=
[ "$(id -u)" -eq 0 ] || SUDO=sudo

if ! lsmod | grep -q '^vws'; then
	echo "unload.sh: vws is not loaded"
	exit 0
fi

if ! $SUDO rmmod vws; then
	echo "unload.sh: rmmod failed; who still has the device open?" >&2
	$SUDO fuser -v /dev/vws 2>&1 || true
	exit 1
fi

[ -c /dev/vws ] && $SUDO rm -f /dev/vws
echo "unload.sh: removed"
