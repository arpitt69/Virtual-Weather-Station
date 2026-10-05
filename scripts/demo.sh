#!/bin/bash
# Narrated walk through the failure modes the station is built to survive.
# Run with the module loaded; logs land in ./demo_*.csv.
set -u

cd "$(dirname "$0")/.."

VWSCTL=./user/vwsctl
VWSD=./user/vwsd

say() { printf '\n\033[1;36m== %s\033[0m\n' "$1"; }
note() { printf '   %s\n' "$1"; }

[ -c /dev/vws ] || { echo "demo.sh: /dev/vws missing; run 'make load' first" >&2; exit 1; }

$VWSCTL clear all > /dev/null

say "The bank, as the driver reports it"
$VWSCTL sensors

say "Starting the station (logging to demo_fused.csv / demo_sensors.csv)"
$VWSD --headless --interval 2000 --csv demo > demo_run.log 2>&1 &
VWSD_PID=$!
trap 'kill $VWSD_PID 2>/dev/null; wait $VWSD_PID 2>/dev/null' EXIT
sleep 4
note "both temperature units healthy; watch the 'sources' line"
tail -n 12 demo_run.log

say "1/5  temp_a's ADC wedges (stuck value)"
$VWSCTL inject temp_a stuck
sleep 6
note "temp_a goes FAULTY on repeated identical readings; fusion falls back"
tail -n 12 demo_run.log
$VWSCTL clear temp_a
sleep 5
note "after a sustained clean run it returns through RECOVERING to OK"
tail -n 12 demo_run.log

say "2/5  temp_b starts spiking (15% of samples)"
$VWSCTL inject temp_b spike 15
sleep 6
note "the median/MAD gate throws the spikes away; the rejected count climbs"
tail -n 12 demo_run.log
$VWSCTL clear temp_b
sleep 3

say "3/5  temp_b develops a calibration drift (+50 m degC per sample)"
$VWSCTL inject temp_b drift 50
sleep 8
note "drift is slow enough to pass the outlier gate: this is what redundancy"
note "alone cannot fix, and why the two units' disagreement matters"
tail -n 12 demo_run.log
$VWSCTL clear temp_b
sleep 3

say "4/5  the humidity sensor stops answering (90% dropout)"
$VWSCTL inject humidity dropout 90
sleep 8
note "nothing to reject - the fault shows up as silence, caught by staleness"
tail -n 12 demo_run.log
$VWSCTL clear humidity
sleep 3

say "5/5  both temperature units fail at once"
$VWSCTL inject temp_a stuck
$VWSCTL inject temp_b noise 30000
sleep 8
note "with no usable sensor the station reports UNAVAILABLE rather than"
note "publishing a stale number as though it were current"
tail -n 12 demo_run.log
$VWSCTL clear all

say "Kernel-side counters for the whole run"
cat /proc/vws/stats

kill $VWSD_PID 2>/dev/null
wait $VWSD_PID 2>/dev/null
trap - EXIT

say "Done"
note "full trace:   demo_run.log"
note "fused series: demo_fused.csv"
note "per-sensor:   demo_sensors.csv"
