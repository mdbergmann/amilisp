#!/bin/sh
# Executable specification for MP:*THREAD-DEATH-HOOKS* (src/core/builtins.c,
# cl_run_thread_death_hooks).
#
# A worker thread that an unhandled error ends was gone without a word, and
# the error that ends most of them -- the heap exhausted -- resets the
# handler and NLX stacks before it unwinds, so no HANDLER-CASE and no
# UNWIND-PROTECT of the thread's own sees it.  The editor's REPL thread died
# that way in the middle of a system load and the editor waited for a RESULT
# that never came.
#
# The hooks are called with (THREAD MESSAGE) on the dying thread:
#   - for a thread that exhausted the heap, with room to run in,
#   - for a thread an ordinary unhandled error ended,
#   - NOT for a thread that returned, took ABORT, or was destroyed,
#   - a hook that fails does not keep the next one from running.
#
# Run: sh tests/test_mt_thread_death_hook.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_thread_death_hook: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_mtdeath_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
(defvar *keep* nil)
(defvar *seen* nil)
(defvar *unwound* nil)
(defun note-death (thread message)
  (push (list (mp:thread-name thread) message
              (eq thread (mp:current-thread))
              ;; The hook allocates: the dead thread's garbage is room again.
              (length (make-string 2000)))
        *seen*))
(format t "INITIAL=~s~%" mp:*thread-death-hooks*)
;; Newest first: the failing hook runs before NOTE-DEATH.
(push 'note-death mp:*thread-death-hooks*)
(push (lambda (thread message)
        (declare (ignore thread message))
        (error "a hook that fails"))
      mp:*thread-death-hooks*)
(push 'no-such-function-anywhere mp:*thread-death-hooks*)

(defun run (name function)
  (let ((thread (mp:make-thread function :name name)))
    (loop repeat 600 while (mp:thread-alive-p thread) do (sleep 0.05))
    (format t "ALIVE-~a=~a~%" name (mp:thread-alive-p thread))))

(run "eater" (lambda ()
               (unwind-protect (loop (push (make-string 10000) *keep*))
                 (setf *unwound* t))))
(setf *keep* nil)
(run "erring" (lambda () (error "plain boom")))
(run "fine" (lambda () 42))
(run "aborting" (lambda () (abort)))
(let ((thread (mp:make-thread (lambda () (loop (sleep 0.05))) :name "destroyed")))
  (sleep 0.2)
  (mp:destroy-thread thread)
  (loop repeat 200 while (mp:thread-alive-p thread) do (sleep 0.05)))

(setf *seen* (reverse *seen*))
(format t "COUNT=~d~%" (length *seen*))
(dolist (entry *seen*)
  (format t "DIED ~a|~a|own-thread=~a|room=~a~%"
          (first entry) (second entry) (third entry) (fourth entry)))
;; What makes the hook necessary: the thread's own cleanup did not run.
(format t "UNWOUND=~a~%" *unwound*)
(format t "MT-DEATH-DONE~%")
EOF

out=$("$TIMEOUT" 120 "$CLAMIGA" --heap 4M --no-userinit --non-interactive --load "$tmp" </dev/null 2>&1)
status=$?
passed=0
fail() {
    echo "FAIL mt_thread_death_hook ($1)"
    echo "$out" | tail -12 | sed 's/^/    /'
    echo "$passed passed, 1 failed, $((passed + 1)) total"
    exit 1
}
want() {
    printf '%s\n' "$out" | grep -q "$2" || fail "$1"
    echo "  ok  $1"
    passed=$((passed + 1))
}

[ $status -eq 0 ] || fail "exit $status: crash/hang"
want "the script ran to its end"                 '^MT-DEATH-DONE$'
want "the list starts empty"                     '^INITIAL=NIL$'
want "every thread ended"                        '^ALIVE-eater=NIL$'
want "heap exhaustion reaches the hook, on the dying thread, with room" \
     '^DIED eater|Heap exhausted (requested [0-9]* bytes)|own-thread=T|room=2000$'
want "an unhandled error reaches the hook"       '^DIED erring|.*plain boom|own-thread=T|room=2000$'
want "a return, an ABORT and a destroy do not"   '^COUNT=2$'
want "heap exhaustion skipped the thread's cleanup (why the hook exists)" '^UNWOUND=NIL$'

echo "$passed passed, 0 failed, $passed total"
exit 0
