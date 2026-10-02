#!/bin/sh
# Collect reproducible system metadata and sharp-drm pipeline counters.

set -eu

usage()
{
	echo "Usage: $0 OUTPUT [-- COMMAND [ARG...]]" >&2
	exit 2
}

[ "$#" -ge 1 ] || usage
output=$1
shift

if [ "$#" -gt 0 ]; then
	[ "$1" = "--" ] || usage
	shift
fi

stats=/sys/kernel/debug/sharp_drm/stats

write_stats()
{
	if [ -w "$stats" ]; then
		printf '%s\n' "$1" > "$stats"
	else
		printf '%s\n' "$1" | sudo tee "$stats" >/dev/null
	fi
}

if [ ! -e "$stats" ]; then
	if ! mountpoint -q /sys/kernel/debug; then
		sudo mount -t debugfs none /sys/kernel/debug
	fi
fi

[ -r "$stats" ] || {
	echo "Cannot read $stats; is sharp_drm loaded?" >&2
	exit 1
}

write_stats enable
write_stats reset

started_at=$(date --iso-8601=seconds)
workload_rc=0
if [ "$#" -gt 0 ]; then
	"$@" || workload_rc=$?
fi
finished_at=$(date --iso-8601=seconds)

{
	echo "started_at $started_at"
	echo "finished_at $finished_at"
	echo "workload_exit $workload_rc"
	echo "kernel $(uname -srvm)"
	if [ -r /proc/device-tree/model ]; then
		printf 'model '
		tr -d '\000' < /proc/device-tree/model
		echo
	fi
	if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
		printf 'cpu_governor '
		cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
	fi
	if [ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq ]; then
		printf 'cpu0_cur_freq_khz '
		cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
	fi
	for zone in /sys/class/thermal/thermal_zone*/temp; do
		[ -r "$zone" ] || continue
		printf '%s ' "$(basename "$(dirname "$zone")")"
		cat "$zone"
	done
	if command -v vcgencmd >/dev/null 2>&1; then
		vcgencmd get_throttled
	fi
	for parameter in /sys/module/sharp_drm/parameters/*; do
		[ -r "$parameter" ] || continue
		parameter_value=$(cat "$parameter")
		printf 'parameter_%s %s\n' \
			"$(basename "$parameter")" "$parameter_value"
	done
	find /sys/firmware/devicetree/base -name spi-max-frequency -type f \
		-print 2>/dev/null |
	while IFS= read -r frequency; do
		printf 'device_tree_%s ' "${frequency#/sys/firmware/devicetree/base/}"
		od -An -tu4 --endian=big "$frequency" | tr -d ' '
	done
	echo
	echo "[sharp_drm_stats]"
	cat "$stats"
} > "$output"

echo "Wrote $output"
exit "$workload_rc"
