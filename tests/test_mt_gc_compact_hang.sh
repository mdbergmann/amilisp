#!/bin/sh
# Regression test: concurrent MP:MAKE-THREAD + EXT:GC-COMPACT at high thread
# count must not hang, corrupt, or crash.  Guards a cluster of thread-vs-moving-
# GC bugs that only surface once several worker threads compact the heap at the
# same time (baseline hangs at 6 workers, which masked the corruption below):
#
#  1. Stop-the-world newborn hang: MAKE-THREAD registered the child in
#     cl_thread_list BEFORE its OS thread existed, so a peer STW that began in
#     that window waited forever for a thread that could never reach a
#     safepoint.  Fix: gc_live flag + cl_gc_thread_online barrier; the STW wait
#     loop only waits for `gc_live` threads.
#
#  2. child->result double-forward: MAKE-THREAD registered
#     gc_roots[0]=&child->result, which aliases the t->result thread-metadata
#     slot that gc_update_thread_roots also forwards.  gc_forward() is not
#     idempotent, so the closure offset was relocated twice → the worker applied
#     a wrong-typed object ("Not a function: heap object type N").
#
#  3. Stale `func` C-local: thread_entry read func=t->result at entry, then the
#     abort-restart setup allocated (compaction possible); the C-local was not
#     forwarded, so cl_vm_apply ran a stale closure offset.  Fix: re-read func
#     from the forwarded t->result just before the apply.
#
#  4. JOIN-THREAD stale tobj double-free: bi_join_thread cached
#     tobj=OBJ_TO_PTR(arg) before cl_gc_leave_safe_region(), which can park the
#     caller across a peer compaction that relocates the wrapper.  The stale
#     pointer left cl_thread_table[id] pointing at the freed worker, so
#     gc_finalize_dead freed it again ("pointer being freed was not allocated").
#     Fix: re-derive tobj after the safe region.
#
#  5. Heap-result staleness: a worker's return value lived only in t->result,
#     but the worker unregisters before JOIN reads it, so a peer compaction
#     swept/relocated the result — JOIN returned garbage.  Fix: publish the
#     result into the GC-managed wrapper (CL_ThreadObj.result), which stays
#     marked+forwarded until JOIN reads it.
#
# Reproduction is probabilistic per run, so each scenario runs several times;
# any hang (timeout), crash (non-zero exit), or missing DONE marker fails.
#
# Run: sh tests/test_mt_gc_compact_hang.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_gc_compact_hang: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_mtcompact_XXXXXX") || exit 1
cleanup() { rm -f "$tmp"; }
trap cleanup EXIT INT TERM

fail=0

run_scenario() {
    name="$1"; marker="$2"; heap="$3"; runs="$4"
    i=0
    while [ "$i" -lt "$runs" ]; do
        i=$((i + 1))
        out=$(CLAMIGA_THREAD_ERRORS=1 "$TIMEOUT" 40 "$CLAMIGA" --no-userinit \
              --heap "$heap" --non-interactive --load "$tmp" </dev/null 2>&1)
        ec=$?
        if [ "$ec" -eq 124 ]; then
            echo "  FAIL $name (run $i/$runs timed out — STW-GC hang)"
            fail=1; return
        elif [ "$ec" -ne 0 ]; then
            echo "  FAIL $name (run $i/$runs ec=$ec — crash / double-free)"
            echo "    $(echo "$out" | tail -3)"
            fail=1; return
        elif ! echo "$out" | grep -q "$marker"; then
            echo "  FAIL $name (run $i/$runs — no $marker marker: corruption)"
            echo "    $(echo "$out" | grep -iE 'CORRUPT|FAIL|THREAD-ERROR|BADRESULT|not of type' | head -3)"
            fail=1; return
        elif echo "$out" | grep -qiE 'THREAD-ERROR|not of type|Unbound|corrupted|freed'; then
            echo "  FAIL $name (run $i/$runs — error markers in output)"
            echo "    $(echo "$out" | grep -iE 'THREAD-ERROR|not of type|Unbound|corrupted|freed' | head -3)"
            fail=1; return
        fi
    done
    echo "  ok  $name ($runs runs clean)"
}

# Scenario A: 6 workers each looping (fresh closure) gc-compact + validate.
# Hits bugs 1-3 (newborn hang, double-forward, stale func).
cat > "$tmp" <<'EOF'
(defun worker (id n)
  (dotimes (i n)
    (let ((s (list id i (* i 2) (list 'k i) "abc")))
      (ext:gc-compact)
      (unless (and (eql (first s) id) (eql (second s) i)
                   (eql (third s) (* i 2))
                   (equal (fourth s) (list 'k i)) (equal (fifth s) "abc"))
        (return-from worker :corrupt))))
  :ok)
(let ((threads nil) (ok t))
  (dotimes (w 6)
    (let ((id w))
      (push (mp:make-thread (lambda () (worker id 200)) :name "w") threads)))
  (dolist (thr threads)
    (unless (eq (mp:join-thread thr) :ok) (setf ok nil)))
  (format t "~a~%" (if ok "COMPACT-DONE" "COMPACT-FAIL")))
EOF
run_scenario "concurrent_compact_6_workers" "COMPACT-DONE" 8M 8

# Scenario B: workers return FRESH heap objects joined under concurrent GC.
# Hits bug 5 (heap-result staleness) and bug 4 (join double-free).
cat > "$tmp" <<'EOF'
(defun mk (id n) (dotimes (i n) (ext:gc-compact))
  (list id (format nil "res-~a" id) (list 'tag id)))
(let ((ok t))
  (dotimes (gen 5)
    (let ((threads nil))
      (dotimes (w 6)
        (let ((id (+ (* gen 6) w)))
          (push (cons id (mp:make-thread (lambda () (mk id 30)) :name "w")) threads)))
      (dolist (pair threads)
        (let* ((id (car pair)) (r (mp:join-thread (cdr pair))))
          (unless (and (consp r) (eql (first r) id)
                       (stringp (second r))
                       (string= (second r) (format nil "res-~a" id))
                       (equal (third r) (list 'tag id)))
            (setf ok nil))))))
  (format t "~a~%" (if ok "RESULT-DONE" "RESULT-FAIL")))
EOF
run_scenario "heap_result_survives_concurrent_gc" "RESULT-DONE" 10M 8

# Scenario C: high thread count + many generations (worker churn) — exercises
# the join/finalize reaping paths under heavy compaction (bug 4).
cat > "$tmp" <<'EOF'
(defun w (id n)
  (dotimes (i n) (let ((s (list id i "x"))) (ext:gc-compact)
                   (unless (eql (car s) id) (return-from w :bad))))
  :ok)
(let ((ok t))
  (dotimes (gen 8)
    (let ((threads nil))
      (dotimes (k 8)
        (let ((id (+ (* gen 8) k)))
          (push (mp:make-thread (lambda () (w id 25)) :name "w") threads)))
      (dolist (thr threads)
        (unless (eq (mp:join-thread thr) :ok) (setf ok nil)))))
  (format t "~a~%" (if ok "CHURN-DONE" "CHURN-FAIL")))
EOF
run_scenario "worker_churn_reaping_under_compaction" "CHURN-DONE" 12M 6

echo ""
if [ "$fail" -eq 0 ]; then
    echo "1 passed, 0 failed, 1 total"
    echo "PASS"
    exit 0
else
    echo "0 passed, 1 failed, 1 total"
    echo "FAIL"
    exit 1
fi
