#!/bin/sh
# Regression test (GC audit tier 4, batch 6, T2): concurrent interning of
# FRESH symbols (plus defmacro/deftype registrations) across threads must
# not hang.
#
# Pre-fix, the intern slow path allocated the bucket cons while HOLDING
# cl_package_rwlock's write lock (and the definition registrars consed
# under cl_tables_wrlock).  Under MT that allocation can trigger a
# stop-the-world GC which waits for every peer thread to park — but a
# peer blocked on the rdlock (any intern fast path) is not in a safe
# region and can never park: a circular STW-vs-rwlock wait, seen in the
# field as "random MT hangs under load".  The fix pre-conses every cell
# outside the locks and links with plain stores inside them.
#
# Reproduction is scheduler-dependent, so the binary runs several times
# under a hard timeout; any hang/crash in ANY run fails.
#
# When AMILISP_GC_STRESS=1 is already set in the environment (used by
# `make test-gc-stress`, which runs this script a second time against a
# -DDEBUG_GC_STRESS binary — see Makefile), every allocation forces a
# compacting GC, so instead of relying on scheduler luck to land a
# compaction inside the pre-cons-then-locked-link window, it happens on
# EVERY call.  That run also exercises SHADOW and EXPORT (cl_shadow_symbol
# / cl_export_symbol in package.c), the other two pre-cons-then-relink
# sites named in the audit alongside cl_intern_in and
# cl_table_prepend_locked (defmacro/deftype).  Iteration counts are cut
# down for that run because every single allocation now triggers a full
# mark-sweep-compaction.
#
# Run: sh tests/test_mt_intern_stw.sh build/host/amilisp
# Run under stress: AMILISP_GC_STRESS=1 sh tests/test_mt_intern_stw.sh build/host-gcstress/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_intern_stw: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_mtintern_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

if [ -n "$AMILISP_GC_STRESS" ]; then
    runs=1
    run_timeout=90
    cat > "$tmp" <<'EOF'
;; Demand-interned binding table (bindtab.c): every thread looks up the
;; SAME 12 names in one lazy package; a materialisation allocates outside
;; cl_package_rwlock and links under it with a re-check, so the losers of
;; a race must end up with the winner's symbol — all lists EQ, pairwise.
(defpackage "T2-LAZY" (:use "CL"))
(amilisp::%register-binding-table "T2-LAZY"
  (amilisp::%make-binding-table
   (loop for i below 12 collect (list :const (format nil "+L~D+" i) i)))
  nil nil)
(defvar *lazy-names* (loop for i below 12 collect (format nil "+L~D+" i)))
(defvar *threads*
  (let (acc)
    (dotimes (tid 4 acc)
      (push
       (let ((tid tid))
         (mp:make-thread
          (lambda ()
            (let ((pkg (make-package (format nil "T2-PKG-~d" tid))))
              (dotimes (i 15)
                ;; lazy package: concurrent first-touch of the same names
                (when (< i 12)
                  (multiple-value-bind (s st) (find-symbol (nth i *lazy-names*) "T2-LAZY")
                    (unless (and (eq st :external) (eql (symbol-value s) i))
                      (error "lazy symbol ~A wrong: ~A ~A" (nth i *lazy-names*) s st))))
                ;; Fresh names: every intern takes the slow path (symbol +
                ;; bucket-cell alloc, then the write-locked link) while the
                ;; peers hammer the read lock — exactly the pre-fix
                ;; STW-vs-rwlock window.  Under AMILISP_GC_STRESS every one
                ;; of these allocations forces a compaction.
                (intern (format nil "T2-SYM-~d-~d" tid i))
                ;; SHADOW/EXPORT: the other two pre-cons-then-relink sites
                ;; (cl_shadow_symbol / cl_export_symbol in package.c),
                ;; each thread in its own package so races are harmless.
                (let ((sym (intern (format nil "T2-SH-~d-~d" tid i) pkg)))
                  ;; A NAME NOT already interned in pkg, so shadow takes its
                  ;; own slow allocating path instead of the "already
                  ;; present" fast-return (which would skip it entirely).
                  (shadow (format nil "T2-SHADOW-~d-~d" tid i) pkg)
                  (export sym pkg))
                ;; Definition registrars: pre-fix these consed under
                ;; cl_tables_wrlock (same deadlock shape).
                (when (zerop (mod i 5))
                  (eval (list 'defmacro
                              (intern (format nil "T2-MAC-~d-~d" tid i))
                              '(x) '(list 'quote x)))
                  (eval (list 'deftype
                              (intern (format nil "T2-TYP-~d-~d" tid i))
                              '() ''integer))))
              (cons :ok (mapcar (lambda (nm) (find-symbol nm "T2-LAZY")) *lazy-names*))))))
       acc))))
(let ((n 0) (lists nil))
  (dolist (th *threads*)
    (let ((r (mp:join-thread th)))
      (when (and (consp r) (eq (car r) :ok)) (incf n) (push (cdr r) lists))))
  ;; every thread saw the SAME symbol objects
  (unless (every (lambda (l) (every #'eq l (first lists))) lists)
    (error "lazy symbols differ between threads"))
  (format t "T2-DONE=~d~%" n))
EOF
else
    runs=4
    run_timeout=60
    cat > "$tmp" <<'EOF'
;; Demand-interned binding table (bindtab.c): the four threads first-touch
;; the SAME 60 names of one lazy package concurrently (materialisation =
;; allocate outside cl_package_rwlock, link under it with a re-check); the
;; losers of a race must end up with the winner's symbol — all lists EQ.
(defpackage "T2-LAZY" (:use "CL"))
(amilisp::%register-binding-table "T2-LAZY"
  (amilisp::%make-binding-table
   (loop for i below 60 collect (list :const (format nil "+L~D+" i) i)))
  nil nil)
(defvar *lazy-names* (loop for i below 60 collect (format nil "+L~D+" i)))
(defvar *threads*
  (let (acc)
    (dotimes (tid 4 acc)
      (push
       (let ((tid tid))
         (mp:make-thread
          (lambda ()
            (dotimes (i 300)
              ;; lazy package: concurrent first-touch of the same names
              (when (< i 60)
                (multiple-value-bind (s st) (find-symbol (nth i *lazy-names*) "T2-LAZY")
                  (unless (and (eq st :external) (eql (symbol-value s) i))
                    (error "lazy symbol ~A wrong: ~A ~A" (nth i *lazy-names*) s st))))
              ;; Fresh names: every intern takes the slow path (symbol +
              ;; bucket-cell alloc, then the write-locked link) while the
              ;; peers hammer the read lock — exactly the pre-fix
              ;; STW-vs-rwlock window.
              (intern (format nil "T2-SYM-~d-~d" tid i))
              ;; Definition registrars: pre-fix these consed under
              ;; cl_tables_wrlock (same deadlock shape).
              (when (zerop (mod i 25))
                (eval (list 'defmacro
                            (intern (format nil "T2-MAC-~d-~d" tid i))
                            '(x) '(list 'quote x)))
                (eval (list 'deftype
                            (intern (format nil "T2-TYP-~d-~d" tid i))
                            '() ''integer)))
              ;; Allocation pressure so STW GCs actually happen mid-loop.
              (when (zerop (mod i 10))
                (make-string 2048 :initial-element #\x)))
            (cons :ok (mapcar (lambda (nm) (find-symbol nm "T2-LAZY")) *lazy-names*)))))
       acc))))
(let ((n 0) (lists nil))
  (dolist (th *threads*)
    (let ((r (mp:join-thread th)))
      (when (and (consp r) (eq (car r) :ok)) (incf n) (push (cdr r) lists))))
  ;; every thread saw the SAME symbol objects
  (unless (every (lambda (l) (every #'eq l (first lists))) lists)
    (error "lazy symbols differ between threads"))
  (format t "T2-DONE=~d~%" n))
EOF
fi

i=1
while [ $i -le $runs ]; do
    out=$("$TIMEOUT" "$run_timeout" "$AMILISP" --no-userinit --heap 24M \
              --non-interactive --load "$tmp" </dev/null 2>&1)
    status=$?
    if [ $status -ne 0 ]; then
        echo "FAIL mt_intern_stw (run $i/$runs — exit $status: hang or crash)"
        echo "$out" | tail -5 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    if ! printf '%s' "$out" | grep -q "T2-DONE=4"; then
        echo "FAIL mt_intern_stw (run $i/$runs — workers did not all finish)"
        echo "$out" | tail -5 | sed 's/^/    /'
        echo "0 passed, 1 failed, 1 total"
        exit 1
    fi
    i=$((i + 1))
done

echo "  ok  mt_intern_stw ($runs runs clean)"
echo "1 passed, 0 failed, 1 total"
exit 0
