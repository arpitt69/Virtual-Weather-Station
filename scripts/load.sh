#!/bin/sh
# Insert vws.ko and make /dev/vws readable. Re-run safely: an already loaded
# module is removed first.
set -e

cd "$(dirname "$0")/.."

SAMPLE_RATE=${SAMPLE_RATE:-20}
FIFO_DEPTH=${FIFO_DEPTH:-1024}
DAY_SECONDS=${DAY_SECONDS:-300}

if [ ! -f kernel/vws.ko ]; then
	echo "load.sh: kernel/vws.ko not built; run 'make kernel' first" >&2
	exit 1
fi

SUDO=
[ "$(id -u)" -eq 0 ] || SUDO=sudo

if lsmod | grep -q '^vws'; then
	echo "load.sh: vws already loaded, removing it first"
	$SUDO rmmod vws
fi

$SUDO insmod kernel/vws.ko \
	sample_rate="$SAMPLE_RATE" \
	fifo_depth="$FIFO_DEPTH" \
	day_seconds="$DAY_SECONDS"

# udev creates the node from the class registration; give it a moment, then
# fall back to mknod if the system has no udev running.
for _ in 1 2 3 4 5 6 7 8 9 10; do
	[ -c /dev/vws ] && break
	sleep 0.1
done

if [ ! -c /dev/vws ]; then
	MAJOR=$(awk '$2 == "vws" { print $1 }' /proc/devices)
	[ -n "$MAJOR" ] || { echo "load.sh: cannot find the vws major number" >&2; exit 1; }
	$SUDO mknod /dev/vws c "$MAJOR" 0
fi

# vwsd and vwsctl need write access for the configuration ioctls.
$SUDO chmod 0666 /dev/vws

echo "load.sh: loaded at ${SAMPLE_RATE} Hz, FIFO ${FIFO_DEPTH}, day ${DAY_SECONDS}s"
ls -l /dev/vws
echo
cat /proc/vws/stats
