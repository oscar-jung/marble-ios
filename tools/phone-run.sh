#!/usr/bin/env bash
# One hands-off test cycle on the POCO F5 (marble):
#   1. if a shim is running with the USB log channel, ask it to reset (Android boots)
#   2. if Android is up (adb, ro.soc.model SM7475), `adb reboot bootloader`
#   3. once fastboot shows product "marble", `fastboot boot IMAGE`
#   4. receive the shim's USB log until it has been quiet for QUIET seconds
#
#   tools/phone-run.sh IMAGE [NAME] [QUIET]
#   RECEIVER="tools/usbdump.py OUT" tools/phone-run.sh IMAGE   (any receiver command)
# Log: build-local/logs/<NAME>.log (default NAME = run-<time>).
# Needs the image to be built with shim.usb=1. Nothing is flashed.
set -uo pipefail
IMG="$1"
NAME="${2:-run-$(date +%Y%m%d-%H%M%S)}"
QUIET="${3:-20}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PY="$ROOT/build-local/venv/bin/python"
LOG="$ROOT/build-local/logs/$NAME.log"
mkdir -p "$ROOT/build-local/logs"
[ -f "$IMG" ] || { echo "no image $IMG" >&2; exit 1; }

"$PY" -u "$ROOT/tools/usblog.py" --reboot 2>&1 | sed 's/^/  /' || true

echo "waiting for fastboot (product marble)..."
deadline=$(( $(date +%s) + 240 ))
asked=0
while :; do
	if [ -n "$(fastboot devices 2>/dev/null)" ]; then
		[ "$(fastboot getvar product 2>&1 | awk '/product/{print $2}')" = marble ] && break
		echo "fastboot device is not marble" >&2; exit 1
	fi
	if [ $asked = 0 ] && adb devices 2>/dev/null | awk 'NR>1 && $2=="device"' | grep -q .; then
		if [ "$(adb shell getprop ro.soc.model 2>/dev/null | tr -d '\r')" = SM7475 ]; then
			echo "android up: adb reboot bootloader"
			adb reboot bootloader && asked=1
		fi
	fi
	[ "$(date +%s)" -gt $deadline ] && { echo "timeout waiting for fastboot" >&2; exit 1; }
	sleep 2
done

if [ -n "${RECEIVER:-}" ]; then
	"$PY" -u $RECEIVER &
else
	"$PY" -u "$ROOT/tools/usblog.py" --once --quiet-secs "$QUIET" --wait-secs 90 --out "$LOG" &
fi
RX=$!
sleep 1
timeout 60 fastboot boot "$IMG" 2>&1 | sed 's/^/  /'
wait $RX
echo "log: $LOG"
