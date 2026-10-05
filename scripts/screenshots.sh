#!/bin/sh
# Recapture every screenshot in docs/screenshots/ from the running programs.
#
# These are real captures, not mock-ups: tools/termshot.py drives each program
# under a pty of a fixed size and renders pyte's final screen buffer - glyphs
# plus per-cell colour - into an SVG. Needs python3-pyte and a loaded module.
set -e

cd "$(dirname "$0")/.."

SHOTS=docs/screenshots
SHOT="python3 tools/termshot.py"

if [ ! -c /dev/vws ]; then
	echo "screenshots.sh: /dev/vws missing; run 'make load' first" >&2
	exit 1
fi
if ! python3 -c 'import pyte' 2>/dev/null; then
	echo "screenshots.sh: python3 module 'pyte' not installed" >&2
	exit 1
fi

mkdir -p "$SHOTS"
./user/vwsctl clear all > /dev/null 2>&1

echo "1/6 dashboard, healthy"
$SHOT --out "$SHOTS/01-dashboard-healthy.svg" --cols 118 --rows 30 --seconds 14 \
	--title "vwsd - all seven sensors healthy, both temperature units fused" \
	-- ./user/vwsd > /dev/null

echo "2/6 dashboard, temp_a wedged"
./user/vwsctl clear all > /dev/null 2>&1
$SHOT --out "$SHOTS/02-dashboard-fault-fallback.svg" --cols 118 --rows 30 --seconds 20 \
	--at "6:./user/vwsctl inject temp_a stuck" \
	--title "vwsd - temp_a's ADC wedged at 6 s: condemned FAULTY, fusion falls back to temp_b" \
	-- ./user/vwsd > /dev/null
./user/vwsctl clear all > /dev/null 2>&1

echo "3/6 vwsctl info and sensors"
$SHOT --out "$SHOTS/03-vwsctl-sensors.svg" --cols 110 --rows 24 --seconds 8 \
	--title "vwsctl - driver info and the emulated sensor bank, over ioctl" \
	-- sh -c './user/vwsctl info; echo; ./user/vwsctl sensors' > /dev/null

echo "4/6 sysfs and procfs"
$SHOT --out "$SHOTS/04-sysfs-procfs.svg" --cols 110 --rows 34 --seconds 8 \
	--title "sysfs as the hardware configuration interface, plus /proc/vws statistics" \
	-- sh -c 'echo "$ ls /sys/class/vws/vws/"; ls /sys/class/vws/vws/; echo;
	          echo "$ ls /sys/class/vws/vws/sensor0/"; ls /sys/class/vws/vws/sensor0/; echo;
	          echo "$ cat .../sensor0/{name,chip_id,registers,value}";
	          cat /sys/class/vws/vws/sensor0/name /sys/class/vws/vws/sensor0/chip_id \
	              /sys/class/vws/vws/sensor0/registers /sys/class/vws/vws/sensor0/value; echo;
	          echo "$ cat /proc/vws/sensors"; cat /proc/vws/sensors' > /dev/null

echo "5/6 overflow and backpressure"
$SHOT --out "$SHOTS/05-overflow-backpressure.svg" --cols 110 --rows 20 --seconds 12 \
	--title "Backpressure: a starved reader at 7000 samples/s - the backlog stays capped at the ring's span" \
	-- sh -c './user/vwsctl rate 1000 >/dev/null; ./user/vwsctl stall 3; ./user/vwsctl rate 20' > /dev/null

echo "6/6 offline test suite"
$SHOT --out "$SHOTS/06-unit-tests.svg" --cols 104 --rows 40 --seconds 12 \
	--title "make unit - 92 offline checks: filters, health machine, derived metrics, fusion" \
	-- sh -c './user/vwstest 2>&1 | tail -40' > /dev/null

echo "screenshots.sh: wrote $(ls "$SHOTS"/*.svg | wc -l) SVGs to $SHOTS/"
