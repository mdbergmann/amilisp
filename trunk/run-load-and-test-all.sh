#!/bin/sh
# run-load-and-test-all.sh — run every trunk/load-and-test-*.lisp script in
# turn, save each script's full output, and print a one-line result per script.
#
# Run from the repo ROOT (the lisp scripts use repo-root-relative paths):
#
#   trunk/run-load-and-test-all.sh            # warm FASL cache (fast re-runs)
#   trunk/run-load-and-test-all.sh --cold     # clear the FASL cache before
#                                             # each run -> true cold boot
#   trunk/run-load-and-test-all.sh --tally-dir <DIR>
#                                             # skip running; tally .log files
#                                             # already in <DIR> and print the
#                                             # grand total (for unit-testing)
#
# Per-script full logs:  build/load-and-test-logs/<name>.log
# Summary table:         stdout + build/load-and-test-logs/SUMMARY.txt
#
# Exit status = non-zero if any script failed/timed-out OR any test failed.
#
# New trunk/load-and-test-*.lisp files are picked up automatically; only
# their heap/timeout overrides (below) need adding if the defaults don't fit.

set -u

# AMILISP / LOGDIR / CACHE_PARENT are overridable via the environment so the
# regression test (tests/test_test_extra.sh) can drive the runner against a fake
# binary and a throwaway cache without touching the real build tree.
AMILISP=${AMILISP:-build/host/amilisp}
LOGDIR=${LOGDIR:-build/load-and-test-logs}
CACHE_PARENT=${AMILISP_FASL_CACHE_PARENT:-$HOME/.cache/common-lisp}
# Each script gets its own FASL cache under here (see the per-script isolation
# note below). Kept separate from CACHE_PARENT/amilisp-* so we never disturb
# the user's interactive cache.
PERSCRIPT_CACHE="$CACHE_PARENT/amilisp-test-extra"
COLD=0
TALLY_DIR=""

while [ $# -gt 0 ]; do
  case "$1" in
    --cold) COLD=1; shift ;;
    --tally-dir)
      if [ $# -lt 2 ]; then
        echo "error: --tally-dir requires a directory argument" >&2; exit 2
      fi
      TALLY_DIR="$2"; shift 2 ;;
    -h|--help)
      sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
  esac
done

# Heap (MB) per script. Defaults to 96M for anything not matched, so a new
# load-and-test-*.lisp still runs without editing this file.
heap_for() {
  case "$1" in
      *-5am.lisp|*-closer-mop.lisp|*-cl-spark.lisp) echo 24 ;;
      *-str.lisp|*-fset.lisp)                 echo 64 ;;
      *-cffi.lisp)                            echo 192 ;;
      *-ansi.lisp)                            echo 96 ;;
      *-sento.lisp|*-sento-system.lisp)       echo 192 ;;
      # knx-conn pulls in the full sento actor stack plus usocket/local-time;
      # like the sento scripts it thrashes the moving GC below ~192M.
      *-knx-conn.lisp)                        echo 192 ;;
      # chipi pulls in the full sento+drakma stack and a large test suite; at
      # 96M the moving GC thrashes (50x slower, hits the timeout). Its own
      # script header documents 256M for host — match it. chipi-api is the same
      # web+actor stack (drakma/usocket/jzon/ironclad/snooze) and likewise
      # documents 256M; at 96M the jzon/ironclad paths run under heavy
      # fragmentation and its item/SSE integ tests TYPE-ERROR / drop. Note the
      # globs are distinct: *-chipi.lisp does NOT match *-chipi-api.lisp.
      *-chipi.lisp|*-chipi-api.lisp)          echo 256 ;;
      # clog pulls in a larger dep tree (clack/lack/ironclad/yason/websocket…);
      # its script header documents 256M as required — match it.
      *-clog.lisp)                            echo 256 ;;
      # chipi-ui is chipi-api + clog combined (the union of both dep trees);
      # needs the same 256M headroom.
      *-chipi-ui.lisp)                        echo 256 ;;
    *)                                        echo 96 ;;
  esac
}

# Wall-clock timeout (seconds) per script. Cold sento compiles a large dep
# tree from scratch, so it gets the most headroom.
#
# Note: each script now has its own FASL cache (per-script isolation, see the
# FASL-cache correctness note below), so a script can no longer reuse deps
# compiled by an earlier script — every script bears its full cold-compile cost
# on first run. The chipi* web scripts pull a big stack (drakma/usocket/jzon's
# heavy eisel-lemire, hunchentoot, snooze, sento), so they need cold headroom.
timeout_for() {
  case "$1" in
    *-sento.lisp|*-sento-system.lisp) echo 1800 ;;
    # knx-conn cold-compiles the whole sento dep tree from scratch — same
    # headroom as the sento scripts.
    *-knx-conn.lisp)                  echo 1800 ;;
    # chipi-ui cold-compiles chipi + chipi-api + the whole clog dep tree from
    # scratch, the heaviest of the load-and-test scripts — give it room.
    *-chipi-ui.lisp)                  echo 1800 ;;
    # chipi-api / chipi cold-compile the full web+actor stack (jzon eisel-lemire
    # alone is minutes of bignum-heavy work); 600s is not enough on a cold cache.
    *-chipi-api.lisp|*-chipi.lisp)    echo 1800 ;;
    *-ansi.lisp)                      echo 900 ;;
    *)                                echo 600 ;;
  esac
}

# Classify a finished log by the tally format it prints. One place decides the
# format so summarize() and counts_for() can never disagree. Probe order is
# significant (closer-mop's smoke line has no numbers; check it before the
# numeric formats). Echoes one of: fset | rt | fiveam | closermop | none.
# True (exit 0) if the log carries a amilisp uncaught-error / crash marker.
# Used to tell a script that died mid-run (no tally printed) from one that
# merely uses a tally format we don't recognize.
has_error_marker() {
  grep -qE "ERROR:|Guru|Fatal error|[Uu]nhandled condition|stack overflow" "$1"
}

fmt_kind() {
  log="$1"
  if   grep -q "FSet Results" "$log";                       then echo fset
  elif grep -qE "^passed: [0-9]" "$log";                   then echo rt          # ansi rt:do-tests
  # trunk/load-and-test-closer-mop.lisp's final banner. Matched loosely (the
  # word between "closer-mop" and "OK" is not pinned) because an exact match
  # silently desynced once: the banner went "shim OK" -> "fork OK" and this
  # probe kept looking for the old string, demoting a clean run to "none".
  elif grep -qE '^--- closer-mop .* OK ---' "$log";        then echo closermop   # smoke test, no numbers
  elif grep -qE "Pass: [0-9]+ \(" "$log";                  then echo fiveam      # 5am/str/sento/chipi
  elif grep -q "CLOG loaded: all probed entry points" "$log"; then echo clog      # clog smoke test
  else                                                          echo none
  fi
}

# Extract numeric pass/fail from a log file. Outputs: "<pass> <fail>".
# closermop (no per-test numbers) and unrecognized logs both yield "0 0".
counts_for() {
  log="$1"
  case "$(fmt_kind "$log")" in
    fset)
      pass=$(grep "FSet Results" "$log" | tail -1 | \
             awk '{for(i=1;i<=NF;i++) if($i~/^passed/) {print $(i-1); exit}}')
      fail=$(grep "FSet Results" "$log" | tail -1 | \
             awk '{for(i=1;i<=NF;i++) if($i~/^failed/) {print $(i-1); exit}}')
      echo "${pass:-0} ${fail:-0}" ;;
    rt)
      pass=$(grep -E '^passed: ' "$log" | tail -1 | awk '{print $2}')
      fail=$(grep -E '^failed: ' "$log" | tail -1 | awk '{print $2}')
      echo "${pass:-0} ${fail:-0}" ;;
    fiveam)
      pass=$(grep -E 'Pass: [0-9]+ \(' "$log" | tail -1 | awk '{print $2}')
      fail=$(grep -E 'Fail: [0-9]+ \(' "$log" | tail -1 | awk '{print $2}')
      echo "${pass:-0} ${fail:-0}" ;;
    *)
      echo "0 0" ;;
  esac
}

# Distil a finished log to ONE result line in a uniform shape across all test
# families: "<N> passed, <M> failed". closer-mop's smoke test has no per-test
# numbers; a log with neither a recognized summary nor an error marker is
# flagged so it gets looked at (a clean run always prints one of the known
# tallies — a missing tally means the script died before reporting).
summarize() {
  log="$1"
  case "$(fmt_kind "$log")" in
    closermop|clog)
      echo "smoke test OK" ;;
    none)
      # No tally printed. Distinguish a script that errored out (amilisp prints
      # an uncaught-error / Guru line) from one that simply uses an unknown
      # format, so the table says something actionable instead of "ok".
      if has_error_marker "$log"; then
        echo "errored before printing a summary — check the log"
      else
        echo "?? no recognized summary — check the log"
      fi ;;
    *)
      set -- $(counts_for "$log")
      echo "$1 passed, $2 failed" ;;
  esac
}

# Format a whole-second duration compactly, dropping leading zero units:
#   45 -> "45s", 83 -> "1m23s", 3725 -> "1h02m05s".
fmt_dur() {
  s=$1
  if   [ "$s" -ge 3600 ]; then printf '%dh%02dm%02ds' $((s/3600)) $(((s%3600)/60)) $((s%60))
  elif [ "$s" -ge 60 ];   then printf '%dm%02ds' $((s/60)) $((s%60))
  else                         printf '%ds' "$s"
  fi
}

# --- Tally-only path: aggregate pre-existing logs without running amilisp ---
if [ -n "$TALLY_DIR" ]; then
  if [ ! -d "$TALLY_DIR" ]; then
    echo "error: --tally-dir '$TALLY_DIR' is not a directory" >&2
    exit 2
  fi
  total_pass=0
  total_fail=0
  total_scripts=0
  # KIND echoes fmt_kind's verdict. Without it the tally path only observes
  # counts_for(), and a log misclassified as "none" is indistinguishable from a
  # correctly-classified "closermop" -- both tally 0/0. Printing the kind is what
  # lets tests/test_test_extra.sh pin the probes.
  printf '%-32s %-10s %s\n' "LOG" "KIND" "COUNTS"
  printf '%-32s %-10s %s\n' "---" "----" "------"
  for log in "$TALLY_DIR"/*.log; do
    [ -e "$log" ] || continue
    total_scripts=$((total_scripts + 1))
    counts=$(counts_for "$log")
    c_pass=$(echo "$counts" | awk '{print $1}')
    c_fail=$(echo "$counts" | awk '{print $2}')
    total_pass=$((total_pass + c_pass))
    total_fail=$((total_fail + c_fail))
    printf '%-32s %-10s pass=%-5d fail=%d\n' "$(basename "$log")" "$(fmt_kind "$log")" "$c_pass" "$c_fail"
  done
  echo ""
  echo "=== GRAND TOTAL ==="
  echo "scripts: $total_scripts/$total_scripts ok"
  echo "tests:   $total_pass passed, $total_fail failed"
  [ "$total_fail" -gt 0 ] && exit 1
  exit 0
fi

# --- Normal path: run scripts, then print grand total ---

if [ ! -x "$AMILISP" ]; then
  echo "error: $AMILISP not found or not executable — run 'make host' first" >&2
  exit 2
fi
if ! ls trunk/load-and-test-*.lisp >/dev/null 2>&1; then
  echo "error: no trunk/load-and-test-*.lisp files — run from the repo root" >&2
  exit 2
fi

# FASL-cache correctness.
#
# These scripts hit two distinct cache hazards. Both are defended here by giving
# each script its own FASL cache directory (AMILISP_FASL_CACHE_DIR, honoured by
# make_fasl_cache_path in src/core/builtins_io.c) and invalidating it when the
# amilisp binary changes:
#
#  1. Cross-script *features* poisoning. Scripts deliberately compile the SAME
#     shared library under different *features* (e.g. drakma builds hunchentoot
#     with SSL while load-and-test-hunchentoot pushes :hunchentoot-no-ssl).
#     ASDF's staleness check keys only on source mtime, NOT on *features*, so in
#     a shared cache whichever script runs first wins and the others load FASLs
#     compiled for the wrong feature set (e.g. "No class named SSL-ACCEPTOR").
#     A per-script cache makes every script compile its own deps under its own
#     features — and stays warm across re-runs of that same script.
#
#  2. Stale-binary reuse. The cache is otherwise keyed only by CL_VERSION_STRING
#     + CL_FASL_VERSION; the amilisp binary is rebuilt far more often than the
#     FASL version is bumped (and WIP rebuilds violate that contract by
#     construction), so a fresh binary can silently load bytecode emitted by the
#     previous binary — missing classes, bad CLOS dispatch, SIGSEGV. We
#     fingerprint the binary's contents and wipe the per-script caches whenever
#     it changes; same-binary re-runs stay warm.
#
# --cold wipes each script's cache per-script (below), so the binary guard is a
# no-op there.
if [ "$COLD" -eq 0 ]; then
  BINID_FILE="$PERSCRIPT_CACHE/.amilisp-binid"
  # cksum is POSIX and present on macOS + Linux; output is "<crc> <size> <name>"
  # — keep crc+size, drop the path so a moved tree doesn't force a wipe.
  # The boot and CLOS FASLs count as part of the binary: they hold the macros
  # (LOOP, DEFCLASS, ...) every cached expansion came from, and a fix there
  # leaves the executable untouched.
  # (AMILISP_BOOT_FASLS overrides the list; tests/test_test_extra.sh uses it.)
  BOOT_FASLS=${AMILISP_BOOT_FASLS:-lib/boot.fasl lib/clos.fasl}
  # shellcheck disable=SC2086
  cur_binid=$(cat "$AMILISP" $BOOT_FASLS 2>/dev/null | cksum | awk '{print $1, $2}')
  prev_binid=""
  [ -f "$BINID_FILE" ] && prev_binid=$(cat "$BINID_FILE" 2>/dev/null)
  if [ -n "$cur_binid" ] && [ "$cur_binid" != "$prev_binid" ]; then
    if [ -d "$PERSCRIPT_CACHE" ]; then
      echo ">>> amilisp binary or boot FASLs changed since last run — wiping stale FASL cache" >&2
      rm -rf "$PERSCRIPT_CACHE"
    fi
    mkdir -p "$PERSCRIPT_CACHE"
    printf '%s\n' "$cur_binid" > "$BINID_FILE"
  fi
fi

mkdir -p "$LOGDIR"
SUMMARY="$LOGDIR/SUMMARY.txt"
: > "$SUMMARY"

fail_count=0
total_scripts=0
total_pass=0
total_fail=0
total_time=0
printf '%-32s %-5s %-9s %-8s %s\n' "SCRIPT" "HEAP" "STATUS" "TIME" "RESULT" | tee -a "$SUMMARY"
printf '%-32s %-5s %-9s %-8s %s\n' "------" "----" "------" "----" "------" | tee -a "$SUMMARY"

for f in trunk/load-and-test-*.lisp; do
  name=$(basename "$f")
  heap=$(heap_for "$f")
  tmo=$(timeout_for "$f")
  log="$LOGDIR/${name%.lisp}.log"

  # Per-script FASL cache (see the FASL-cache correctness note above). --cold
  # wipes this script's cache so it truly cold-compiles every time.
  script_cache="$PERSCRIPT_CACHE/${name%.lisp}"
  if [ "$COLD" -eq 1 ]; then
    rm -rf "$script_cache"
  fi

  total_scripts=$((total_scripts + 1))
  echo ">>> $name  (heap ${heap}M, timeout ${tmo}s, cold=$COLD)" >&2
  # stdin from /dev/null so the post-load REPL sees EOF and exits instead
  # of blocking; everything (incl. stderr) captured to the per-script log.
  start_s=$(date +%s)
  AMILISP_FASL_CACHE_DIR="$script_cache" \
    timeout "$tmo" "$AMILISP" --no-userinit --heap "${heap}M" --load "$f" </dev/null >"$log" 2>&1
  ec=$?
  elapsed=$(( $(date +%s) - start_s ))
  total_time=$((total_time + elapsed))

  result=$(summarize "$log")
  if grep -q "load-libs-ql: fetching" "$log"; then
    result="$result  [fetched from dist]"
  fi

  case "$ec" in
    0)   status="ok" ;;
    124) status="TIMEOUT"; result="timed out after ${tmo}s — $result" ;;
    *)   status="exit=$ec" ;;
  esac
  [ "$ec" -eq 0 ] || fail_count=$((fail_count + 1))

  # A clean exit code can still hide a broken run: a script that errored out
  # before printing any recognized tally (e.g. a missing test package) exits 0
  # but never reported. Flag it red rather than letting "ok" mask it.
  if [ "$ec" -eq 0 ] && [ "$(fmt_kind "$log")" = none ] && has_error_marker "$log"; then
    status="ERRORED"
    fail_count=$((fail_count + 1))
  fi

  counts=$(counts_for "$log")
  c_pass=$(echo "$counts" | awk '{print $1}')
  c_fail=$(echo "$counts" | awk '{print $2}')
  total_pass=$((total_pass + c_pass))
  total_fail=$((total_fail + c_fail))

  printf '%-32s %-5s %-9s %-8s %s\n' "$name" "${heap}M" "$status" "$(fmt_dur "$elapsed")" "$result" | tee -a "$SUMMARY"
done

echo "" | tee -a "$SUMMARY"
echo "full logs: $LOGDIR/<name>.log   summary: $SUMMARY" | tee -a "$SUMMARY"
echo "" | tee -a "$SUMMARY"
ok_count=$((total_scripts - fail_count))
{
  echo "=== GRAND TOTAL ==="
  echo "scripts: $ok_count/$total_scripts ok"
  echo "tests:   $total_pass passed, $total_fail failed"
  echo "time:    $(fmt_dur "$total_time") total"
} | tee -a "$SUMMARY"

if [ "$fail_count" -gt 0 ] || [ "$total_fail" -gt 0 ]; then
  exit 1
fi
exit 0
