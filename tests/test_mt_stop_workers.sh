#!/bin/sh
# Process exit on AmigaOS stops workers that are still RUNNING.
#
# Nothing ends a worker task on AmigaOS: one still running when main returns
# executes the unloaded program code and crashes.  main() therefore asks
# every worker to unwind (the DESTROY-THREAD path, cl_thread_stop_workers)
# before it tears anything down; the host keeps its _exit, so the mechanism
# is tested here through MP::%STOP-WORKERS:
#   - a busy loop, a condition wait, a SLEEP and a lock wait all unwind,
#     running their UNWIND-PROTECT cleanups, and leave the registry;
#   - a worker blocked in foreign code (libc sleep) never reaches a
#     safepoint: the call gives up at its bound and counts it as left;
#   - no workers: returns 0 at once; a bad timeout is a TYPE-ERROR.
#
# Run: sh tests/test_mt_stop_workers.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_stop_workers: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/clamiga_stopworkers_XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
fails=0

fail() { echo "FAIL $1"; fails=$((fails + 1)); }
ok()   { echo "  ok  $1"; }

cat > "$tmp/stop.lisp" <<'EOF'
(defvar *cleaned* nil)
(defvar *lk* (mp:make-lock "held"))
(defvar *cv-lock* (mp:make-lock "cv"))
(defvar *cv* (mp:make-condition-variable))
(defvar *go* 0)
(defun worker (tag body)
  (mp:make-thread
   (lambda ()
     (unwind-protect (progn (mp:atomic-incf *go*) (funcall body))
       (mp:with-lock-held (*cv-lock*) (push tag *cleaned*))))
   :name (string tag)))
(format t "NONE ~a~%" (mp::%stop-workers 1000))
(mp:acquire-lock *lk*)
(let ((ths (list (worker :spin (lambda () (loop)))
                 (worker :cond (lambda ()
                                 (mp:with-lock-held (*cv-lock*)
                                   (loop (mp:condition-wait *cv* *cv-lock*)))))
                 (worker :sleep (lambda () (sleep 60)))
                 (worker :lock (lambda () (mp:acquire-lock *lk*))))))
  (loop until (= *go* 4) do (sleep 0.01))
  (sleep 0.2)
  (format t "LEFT ~a~%" (mp::%stop-workers 5000))
  (format t "CLEANED ~a~%"
          (sort (mapcar #'string *cleaned*) #'string<))
  (format t "ALIVE ~a~%" (count-if #'mp:thread-alive-p ths)))
(mp:release-lock *lk*)
(format t "BADMS ~a~%" (handler-case (mp::%stop-workers -1) (type-error () :type-error)))
EOF
out=$($TIMEOUT 30 "$AMILISP" --non-interactive --no-userinit --load "$tmp/stop.lisp" </dev/null 2>&1)
for want in "NONE 0" "LEFT 0" "CLEANED (COND LOCK SLEEP SPIN)" "ALIVE 0" "BADMS TYPE-ERROR"; do
    if printf '%s\n' "$out" | grep -qx "$want"; then ok "stop: $want"
    else fail "stop: missing '$want'"; printf '%s\n' "$out" | sed 's/^/    /'; fi
done

# --- a worker that cannot be interrupted: give up at the bound -----------
# The foreign nap: libc sleep(2) — on Windows the C runtime has no `sleep`,
# the default symbol search reaches kernel32, whose Sleep takes milliseconds.
case "$(uname -s)" in
    MINGW*|MSYS*|CLANGARM64*|CLANG64*|UCRT64*)
        nap='(ffi:call-foreign (ffi:symbol-pointer "Sleep") :void (quote (:uint32)) (quote (2000)))' ;;
    *)  nap='(ffi:call-foreign (ffi:symbol-pointer "sleep") :uint32 (quote (:uint32)) (quote (2)))' ;;
esac
cat > "$tmp/stuck.lisp" <<EOF
(defvar *in* nil)
(mp:make-thread (lambda ()
                  (setq *in* t)
                  $nap))
(loop until *in* do (sleep 0.01))
(sleep 0.1)
(let ((t0 (get-internal-real-time)) (left (mp::%stop-workers 400)))
  (format t "STUCK ~a ~a~%" left
          (if (>= (- (get-internal-real-time) t0)
                  (* 0.3 internal-time-units-per-second))
              :waited :early)))
;; once the foreign call returns, the pending stop takes it down
(format t "LATER ~a~%" (mp::%stop-workers 5000))
EOF
out=$($TIMEOUT 30 "$AMILISP" --non-interactive --no-userinit --load "$tmp/stuck.lisp" </dev/null 2>&1)
for want in "STUCK 1 WAITED" "LATER 0"; do
    if printf '%s\n' "$out" | grep -qx "$want"; then ok "stuck: $want"
    else fail "stuck: missing '$want'"; printf '%s\n' "$out" | sed 's/^/    /'; fi
done

if [ $fails -ne 0 ]; then
    echo "test_mt_stop_workers: $fails failure(s)"
    exit 1
fi
echo "test_mt_stop_workers: all passed"
