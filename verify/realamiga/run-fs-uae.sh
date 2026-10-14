#!/bin/sh
# run-fs-uae.sh CONFIG
#
# Launch FS-UAE with the given .fs-uae config and supervise it so that
# `make -f Makefile.cross test-amiga[-lowend]` runs fully unattended —
# no manual closing of the emulator window.
#
# How shutdown happens (three paths, in order of preference):
#   1. Clean self-quit: `call-on-ustartup` runs `C:UAEquit` after the Lisp
#      test suite finishes, so FS-UAE exits on its own within a second or two.
#   2. Sentinel kill: should UAEquit silently do nothing (native integration
#      disabled, etc.), this watchdog sees the "=== run end ===" marker that
#      call-on-ustartup writes at completion and kills FS-UAE after a short
#      grace period.
#   3. Stall / hard kill: if amilisp hangs or Gurus *before* reaching that
#      marker, the log stops growing — kill after STALL_TIMEOUT with no new
#      output, or HARD_TIMEOUT total, whichever comes first.
#
# The log is cleared before launch so a previous run's sentinel can't trigger
# an immediate kill during the ~38 s boot-to-REPL window.
set -u

CONFIG="${1:?usage: run-fs-uae.sh CONFIG.fs-uae}"
FSUAE="verify/realamiga/FS-UAE.app/Contents/MacOS/fs-uae"
# FSUAE_LOG: a custom boot-override that writes its markers elsewhere
# (make-image.sh) names its log here, so the suite log is left alone.
LOG="${FSUAE_LOG:-build/amiga/test-results.log}"

POLL="${POLL:-5}"                       # seconds between checks
SENTINEL_GRACE="${SENTINEL_GRACE:-30}"  # wait this long for UAEquit after the marker
STALL_TIMEOUT="${STALL_TIMEOUT:-600}"   # 10 min with no new log output
HARD_TIMEOUT="${HARD_TIMEOUT:-1800}"    # 30 min absolute ceiling

mkdir -p build/amiga
rm -f "$LOG"
# A leftover boot-override (from an aborted run-amiga.sh session) would
# make call-on-ustartup run the game instead of the test suite — clear it.
# KEEP_BOOT_OVERRIDE=1 keeps a deliberately placed one (an unattended
# custom run that still wants this watchdog, e.g. the game test suites).
[ "${KEEP_BOOT_OVERRIDE:-0}" = 1 ] || rm -f build/amiga/boot-override

# Say what the log actually contained at the moment we gave up.  Without this
# the make output shows only "killing FS-UAE" and the reader has to guess
# whether the run died early, wrote nothing, or was merely slow — and the log
# on disk is read later, after the emulator's own shutdown may have appended
# more.  Printing the size and the final lines here pins the state at kill time.
report_log_state() {
	if [ -f "$LOG" ]; then
		echo "=== Watchdog: log is $(wc -c < "$LOG" | tr -d ' ') bytes, \
$(wc -l < "$LOG" | tr -d ' ') lines; last lines at kill time: ==="
		tail -n 5 "$LOG" | sed 's/^/    /'
	else
		echo "=== Watchdog: $LOG does not exist — the run never wrote anything ==="
	fi
}

kill_fsuae() {
	# SIGTERM first, and give it real time: a clean FS-UAE shutdown closes the
	# emulated hard drives, which is what commits the last of the suite log to
	# the host filesystem.  SIGKILL loses whatever the emulator still held.
	kill "$FSUAE_PID" 2>/dev/null
	for _ in 1 2 3 4 5 6 7 8 9 10; do
		kill -0 "$FSUAE_PID" 2>/dev/null || return
		sleep 1
	done
	echo "=== Watchdog: FS-UAE ignored SIGTERM for 10s — SIGKILL (log may be truncated) ==="
	kill -9 "$FSUAE_PID" 2>/dev/null
}

"$FSUAE" "$CONFIG" &
FSUAE_PID=$!

start=$(date +%s)
last_change=$start
last_size=-1
end_seen=0

while kill -0 "$FSUAE_PID" 2>/dev/null; do
	sleep "$POLL"
	now=$(date +%s)

	if [ -f "$LOG" ]; then
		size=$(wc -c < "$LOG" | tr -d ' ')
	else
		size=0
	fi

	if [ "$size" != "$last_size" ]; then
		last_size=$size
		last_change=$now
	fi

	# Path 2: completion marker present — give UAEquit a grace window, then kill.
	if [ "$end_seen" -eq 0 ] && [ -f "$LOG" ] && grep -q '=== run end ===' "$LOG"; then
		end_seen=$now
	fi
	if [ "$end_seen" -ne 0 ] && [ $((now - end_seen)) -ge "$SENTINEL_GRACE" ]; then
		echo "=== Watchdog: tests finished but FS-UAE still up after ${SENTINEL_GRACE}s — quitting it ==="
		kill_fsuae
		break
	fi

	# Path 3a: no new output for too long (hang / Guru before completion).
	if [ $((now - last_change)) -ge "$STALL_TIMEOUT" ]; then
		echo "=== Watchdog: no log output for ${STALL_TIMEOUT}s — killing FS-UAE ==="
		report_log_state
		kill_fsuae
		break
	fi

	# Path 3b: absolute ceiling.
	if [ $((now - start)) -ge "$HARD_TIMEOUT" ]; then
		echo "=== Watchdog: hard timeout ${HARD_TIMEOUT}s reached — killing FS-UAE ==="
		report_log_state
		kill_fsuae
		break
	fi
done

wait "$FSUAE_PID" 2>/dev/null
exit 0
