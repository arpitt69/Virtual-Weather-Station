#!/bin/bash
# End-to-end check of every interface the driver exposes, plus the user-space
# pipeline's reaction to an injected fault. Expects the module to be loaded
# (make load) and the binaries built; writes nothing outside /tmp.
set -u

cd "$(dirname "$0")/.."

VWSCTL=./user/vwsctl
VWSD=./user/vwsd
TMP=$(mktemp -d /tmp/vws-selftest.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

SUDO=
[ "$(id -u)" -eq 0 ] || SUDO=sudo

pass=0
fail=0
skipped=0

ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; pass=$((pass + 1)); }
skip() { printf '  \033[33mSKIP\033[0m  %s\n' "$1"; skipped=$((skipped + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; fail=$((fail + 1)); }
check(){ if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi; }
head2(){ printf '\n\033[1m%s\033[0m\n' "$1"; }

head2 "1. module and device node"

lsmod | grep -q '^vws' ; check $? "vws.ko is loaded"
[ -c /dev/vws ]        ; check $? "/dev/vws is a character device"
grep -q ' vws$' /proc/devices ; check $? "major number registered in /proc/devices"

head2 "2. ioctl interface (vwsctl)"

$VWSCTL info > "$TMP/info.txt" 2>&1
check $? "VWS_IOC_GET_INFO"
grep -q 'abi_version:    1' "$TMP/info.txt"
check $? "ABI version matches the header this binary was built against"
grep -q 'sample record:  16 bytes' "$TMP/info.txt"
check $? "struct vws_sample is 16 bytes on both sides of the syscall boundary"

$VWSCTL sensors > "$TMP/sensors.txt" 2>&1
check $? "VWS_IOC_GET_SENSOR for every sensor"
[ "$(grep -c temperature "$TMP/sensors.txt")" = 2 ]
check $? "two redundant temperature sensors present"

$VWSCTL stats > /dev/null 2>&1 ; check $? "VWS_IOC_GET_STATS"
$VWSCTL flush > /dev/null 2>&1 ; check $? "VWS_IOC_FLUSH"

head2 "3. sysfs"

SYS=/sys/class/vws/vws
[ -d "$SYS" ]                  ; check $? "$SYS exists"
[ -f "$SYS/sample_rate" ]      ; check $? "sample_rate attribute"
[ -d "$SYS/sensor0" ]          ; check $? "per-sensor kobject directory"
[ -f "$SYS/sensor0/registers" ]; check $? "emulated register map exported"

orig_rate=$(cat "$SYS/sample_rate")
$SUDO sh -c "echo 50 > $SYS/sample_rate"
[ "$(cat "$SYS/sample_rate")" = 50 ]
check $? "sample_rate write takes effect"
$SUDO sh -c "echo $orig_rate > $SYS/sample_rate"

$SUDO sh -c "echo drift > $SYS/sensor0/fault_mode"
[ "$(cat "$SYS/sensor0/fault_mode")" = drift ]
check $? "fault_mode accepts a mode name"
$SUDO sh -c "echo none > $SYS/sensor0/fault_mode"

$SUDO sh -c "echo 99999999 > $SYS/sensor0/noise_amp" 2>/dev/null
[ $? -ne 0 ]
check $? "out-of-range noise_amp is rejected with an error"

head2 "4. procfs"

[ -f /proc/vws/stats ]   ; check $? "/proc/vws/stats"
[ -f /proc/vws/sensors ] ; check $? "/proc/vws/sensors"
grep -q 'samples_generated' /proc/vws/stats ; check $? "stats are readable"

head2 "5. read / poll data path"

$VWSCTL watch 40 > "$TMP/watch.txt" 2>&1
check $? "poll() + read() returns samples"
[ "$(grep -c '^ *[0-9]' "$TMP/watch.txt")" -ge 40 ]
check $? "40 whole samples delivered"
awk 'NR>1 && $2 == 0 { n++ } END { exit !(n > 0) }' "$TMP/watch.txt"
check $? "sensor 0 appears in the stream"

head2 "6. fusion pipeline, clean run"

$VWSD --headless --duration 4 --interval 1000 --csv "$TMP/clean" \
	> "$TMP/clean.log" 2>&1
check $? "vwsd runs and exits cleanly"
grep -q 'FUSED temp=' "$TMP/clean.log"
check $? "a fused temperature is produced"
grep -q 'temp_a+temp_b' "$TMP/clean.log"
check $? "both temperature sensors contribute while healthy"
grep -q 'dewpoint=' "$TMP/clean.log"
check $? "derived metrics computed"
[ -s "$TMP/clean_fused.csv" ] && [ "$(wc -l < "$TMP/clean_fused.csv")" -gt 2 ]
check $? "CSV log written with data rows"
[ -s "$TMP/clean_sensors.csv" ]
check $? "per-sensor CSV log written"

head2 "7. fusion engine, inverse-variance mode"

# The Kalman path is the default and is covered above; this exercises the
# other estimator, which until now shipped verified only by the offline suite.
$VWSCTL clear all > /dev/null 2>&1
$VWSD --headless --duration 6 --interval 2000 --fusion weighted \
	> "$TMP/weighted.log" 2>&1
check $? "vwsd runs with --fusion weighted"

grep -q 'fusion=inverse-variance' "$TMP/weighted.log"
check $? "reports the inverse-variance estimator, not the Kalman default"

grep -q 'from temp_a+temp_b' "$TMP/weighted.log"
check $? "both temperature units contribute"

# Pull the last reported block: each sensor's accepted value and weight, and
# the fused result. awk's match() needs the pattern as a string, because a
# regex constant passed as a function argument degrades to a boolean.
read -r w_ta w_tb w_wa w_wb w_f <<EOF
$(awk '
function num(line, pat,   s) {
	if (match(line, pat)) {
		s = substr(line, RSTART, RLENGTH)
		gsub(/^[a-zA-Z]+= */, "", s)
		return s + 0
	}
	return 0
}
/^  temp_a / { ta = num($0, "acc= *-?[0-9.]+"); wa = num($0, "w=-?[0-9.]+") }
/^  temp_b / { tb = num($0, "acc= *-?[0-9.]+"); wb = num($0, "w=-?[0-9.]+") }
/^  FUSED temp=/ { f = num($0, "temp=-?[0-9.]+") }
END { printf "%.4f %.4f %.4f %.4f %.4f\n", ta, tb, wa, wb, f }
' "$TMP/weighted.log")
EOF

# An inverse-variance mean is defined by three properties. Each is checked
# separately so a failure says which one broke.
awk -v a="$w_wa" -v b="$w_wb" 'BEGIN { s = a + b; exit !(s > 0.99 && s < 1.01) }'
check $? "the temperature weights are a partition of unity ($w_wa + $w_wb)"

awk -v a="$w_wa" -v b="$w_wb" 'BEGIN { exit !(a > b) }'
check $? "the quieter unit carries the larger weight ($w_wa vs $w_wb)"

awk -v f="$w_f" -v a="$w_ta" -v b="$w_tb" 'BEGIN {
	lo = (a < b ? a : b); hi = (a < b ? b : a)
	exit !(f >= lo - 0.01 && f <= hi + 0.01)
}'
check $? "the fused value lies between its inputs ($w_ta .. $w_tb -> $w_f)"

# The discriminating one. An inverse-variance mean is exactly
#   f = wa*a + wb*b,  so  (f - a) / (b - a) == wb.
# Checking that identity pins the arithmetic down; merely asserting the result
# is "nearer the quieter unit" would also accept a plain average, which sits
# equidistant. Skipped when the two units happen to agree, because the ratio
# is then 0/0 and nothing can be concluded from it.
# Reported as SKIP rather than PASS when the two units happen to agree: the
# ratio is 0/0 there and nothing can be concluded, and a check that silently
# passes when it could not evaluate is exactly how a regression hides.
if awk -v a="$w_ta" -v b="$w_tb" 'BEGIN {
	spread = b - a; if (spread < 0) spread = -spread
	exit !(spread < 0.05)
}'; then
	skip "weighted-mean identity: units agree to within 0.05 ($w_ta vs $w_tb), ratio undefined"
else
	awk -v f="$w_f" -v a="$w_ta" -v b="$w_tb" -v wb="$w_wb" 'BEGIN {
		r = (f - a) / (b - a)
		d = r - wb; if (d < 0) d = -d
		exit !(d < 0.05)
	}'
	check $? "the fused value is exactly the weighted mean: (f-a)/(b-a) == w_b"
fi

# Degradation has to work in this mode too, not only under the Kalman filter.
$VWSCTL inject temp_a stuck > /dev/null 2>&1
$VWSD --headless --duration 6 --interval 1000 --fusion weighted \
	> "$TMP/weighted-fault.log" 2>&1
grep -q 'from temp_b (fallback)' "$TMP/weighted-fault.log"
check $? "weighted fusion falls back to the survivor when temp_a is condemned"
$VWSCTL clear all > /dev/null 2>&1

head2 "8. fault injection and fallback"

$VWSCTL inject temp_a stuck > /dev/null 2>&1
check $? "inject a stuck fault into temp_a by name"
$VWSD --headless --duration 6 --interval 1000 > "$TMP/stuck.log" 2>&1
grep -q 'temp_a .*FAULTY' "$TMP/stuck.log"
check $? "health monitor drives temp_a to FAULTY"
grep -q 'stuck:' "$TMP/stuck.log"
check $? "stuck-value detector gives the reason"
grep -q 'from temp_b (fallback)' "$TMP/stuck.log"
check $? "fusion falls back to the surviving sensor"
$VWSCTL clear all > /dev/null 2>&1
check $? "clear the fault"

$VWSCTL inject temp_b spike 40 > /dev/null 2>&1
$VWSD --headless --duration 6 --interval 1000 > "$TMP/spike.log" 2>&1
awk '/temp_b/ { split($0, f, "acc/rej="); if (f[2] ~ /\/[1-9]/) n++ } END { exit !(n > 0) }' \
	"$TMP/spike.log"
check $? "outlier filter rejects injected spikes"
$VWSCTL clear all > /dev/null 2>&1

$VWSCTL inject humidity dropout 95 > /dev/null 2>&1
$VWSD --headless --duration 8 --interval 1000 > "$TMP/dropout.log" 2>&1
grep -qE 'humidity .*(SUSPECT|FAULTY)' "$TMP/dropout.log"
check $? "silence from a dropout is detected as a fault"
$VWSCTL clear all > /dev/null 2>&1

head2 "9. FIFO overflow and backpressure"

# Starve the reader hard enough to overflow: 7 sensors at 1000 Hz is 7000
# samples/s into a 1024-sample ring, which fills in about 150 ms.
orig_hz=$($VWSCTL info | awk '/sample_rate/ { print $2 }')
$VWSCTL rate 1000 > /dev/null 2>&1
$VWSCTL stall 3 > "$TMP/stall.txt" 2>&1
check $? "vwsctl stall exercises the overflow path"

episodes=$(awk -F: '/^overflow_episodes/ { gsub(/ /, "", $2); print $2 }' "$TMP/stall.txt")
dropped=$(awk -F:  '/^samples_dropped/   { gsub(/ /, "", $2); print $2 }' "$TMP/stall.txt")
age=$(awk -F:      '/^first_sample_age/  { gsub(/[ s]/, "", $2); print $2 }' "$TMP/stall.txt")
resync=$(awk       '/^resync_flagged/    { print $2 }' "$TMP/stall.txt")

[ "${dropped:-0}" -gt 1000 ]
check $? "a starved reader overflows the ring (dropped ${dropped:-?} samples)"

[ "${episodes:-0}" -ge 1 ] && [ "${episodes:-0}" -le 5 ] && [ "${episodes:-0}" -lt "${dropped:-0}" ]
check $? "episodes counted apart from samples (${episodes:-?} episodes, ${dropped:-?} samples)"

# The discriminating assertion. On an overwrite ring the backlog is bounded by
# the ring's own span (~0.15 s here) however long the stall was; on a ring that
# discarded the incoming sample it equalled the stall, measured at 2.999 s.
awk -v a="${age:-99}" 'BEGIN { exit !(a < 1.0) }'
check $? "backlog capped by the ring, not the stall (${age:-?}s old after a 3s stall)"

awk -v a="${age:-99}" 'BEGIN { exit !(a > 0.0) }'
check $? "the returned samples still carry a sane timestamp"

[ "${resync:-0}" -gt 0 ]
check $? "the gap is flagged where the reader sees it (${resync:-?} RESYNC samples)"

$VWSCTL rate "${orig_hz:-20}" > /dev/null 2>&1
check $? "sampling clock restored to ${orig_hz:-20} Hz"

head2 "10. kernel log is clean"

if $SUDO dmesg 2>/dev/null | tail -200 | grep -iE 'vws.*(BUG|WARN|Oops|bad|lock)' > "$TMP/dmesg.txt"; then
	bad "dmesg contains vws warnings:"
	cat "$TMP/dmesg.txt"
else
	ok "no vws BUG/WARNING in the last 200 dmesg lines"
fi

if [ "$skipped" -gt 0 ]; then
	printf '\n\033[1m%d passed, %d failed, %d skipped\033[0m\n' "$pass" "$fail" "$skipped"
else
	printf '\n\033[1m%d passed, %d failed\033[0m\n' "$pass" "$fail"
fi
[ "$fail" = 0 ]
