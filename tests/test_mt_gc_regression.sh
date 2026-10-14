#!/bin/sh
# Regression test: multi-threaded moving-GC must not corrupt objects whose
# roots are relocated by a peer thread's stop-the-world compaction.
#
# This guards a cluster of GC-safety fixes on multi-thread paths where a value
# lived only in an unprotected C local (or an unmarked per-thread field) across
# an allocating call, so a PEER thread's compaction relocated it and left the
# holder with a stale offset:
#
#   * thread_entry() abort-restart setup: abort_report/abort_handler/abort_tag
#     held unprotected across get_thread_abort_handler / cl_cons / cl_make_restart
#     (corrupted a worker's restart_stack[0]).
#   * SUBSEQ list path: the source cursor `list` reassigned by cl_cdr and left
#     unprotected across cl_cons.
#   * print_list(): the list-walk cursor unprotected across print_obj (which can
#     allocate via a Lisp print-object method).
#   * cl_get_output_stream_string(): the raw `st` (and `stream` offset) held
#     across the result-string allocation, then used for the outbuf reset — under
#     multi-thread FORMAT this reset a peer's live outbuf.
#   * pr_stream / pr_circle_keys: per-thread printer CL_Obj roots that GC never
#     marked or forwarded.
#
# The single-threaded REPL never hit these: it only compacts at its own
# allocation points, where the GC-protection convention already holds; the
# hazard is a PEER relocating a thread that is parked at an arbitrary safepoint.
#
# The scenario runs the main thread and a worker through make-instance + CLOS
# dispatch + FORMAT-of-nested-list + SUBSEQ churn while forcing a moving
# compaction (ext:gc-compact), across several worker generations, each thread
# validating its own results.  Reproduction is probabilistic per run (a stale
# offset often lands on a plausible object), so the binary is run several times;
# any corruption / crash / hang in ANY run fails the test.  Before the fixes at
# least one run corrupts (or crashes); after them every run prints MT-GC-DONE.
#
# Run: sh tests/test_mt_gc_regression.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_gc_regression: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_mtgc_XXXXXX") || exit 1
cleanup() { rm -f "$tmp"; }
trap cleanup EXIT INT TERM

cat > "$tmp" <<'EOF'
(defclass base () ((x :initarg :x :accessor x)))
(defclass d1 (base) ())
(defclass d2 (base) ())
(defgeneric frob (a b))
(defmethod frob ((a d1) (b d2)) (list 'd1d2 (x a) (x b)))

(defun churn (n tag)
  (let ((acc nil))
    (dotimes (i n)
      (let* ((o1 (make-instance 'd1 :x i))
             (o2 (make-instance 'd2 :x (1+ i)))
             (r  (frob o1 o2))
             (s  (format nil "~a:~a:~a" tag i (list i (* i 2) (list 'k i)))))
        (setf acc (cons (cons s r) acc))
        (when (> (length acc) 20) (setf acc (subseq acc 0 20)))
        (when (zerop (mod i 3)) (ext:gc-compact))
        (unless (and (stringp s) (consp r) (eq (car r) 'd1d2) (eql (cadr r) i))
          (return-from churn :corrupt))))
    :ok))

(let ((ok t))
  (dotimes (gen 8)
    (let ((w (mp:make-thread (lambda () (churn 250 "W")) :name "w")))
      (unless (eq (churn 250 "M") :ok) (setf ok nil))
      (unless (eq (mp:join-thread w) :ok) (setf ok nil))))
  (format t "~a~%" (if ok "MT-GC-DONE" "MT-GC-FAIL")))
EOF

# Run several times: reproduction is probabilistic, but the fixed build is
# deterministically clean, so any failing run flags a regression.
RUNS=10
i=0
while [ "$i" -lt "$RUNS" ]; do
    i=$((i + 1))
    out=$("$TIMEOUT" 40 "$CLAMIGA" --no-userinit --heap 4M --non-interactive --load "$tmp" </dev/null 2>&1)
    ec=$?
    if [ "$ec" -eq 124 ]; then
        echo "  FAIL mt_gc_regression (run $i/$RUNS timed out — STW-GC hang)"
        echo "    output: $(echo "$out" | tail -5)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    elif [ "$ec" -ne 0 ]; then
        echo "  FAIL mt_gc_regression (run $i/$RUNS ec=$ec — crash / heap corruption)"
        echo "    output: $(echo "$out" | tail -8)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    elif ! echo "$out" | grep -q "MT-GC-DONE"; then
        echo "  FAIL mt_gc_regression (run $i/$RUNS corrupted — no MT-GC-DONE marker)"
        echo "    output: $(echo "$out" | tail -8)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    elif echo "$out" | grep -qi "MT-GC-FAIL\|not of type\|Unbound\|No class\|corrupted pointer"; then
        echo "  FAIL mt_gc_regression (run $i/$RUNS corruption markers)"
        echo "    output: $(echo "$out" | grep -i 'MT-GC-FAIL\|not of type\|Unbound\|No class\|corrupted' | head -4)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    fi
done

echo "  ok  mt_gc_regression ($RUNS runs clean)"
echo ""
echo "1 passed, 0 failed, 1 total"
echo "PASS"
exit 0
