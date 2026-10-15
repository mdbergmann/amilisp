#!/bin/sh
# Regression test: concurrent slow-path dispatch-cache updates must not corrupt
# a generic function's dispatch-cache hash table.
#
# Root cause this guards (lib/clos.lisp %make-dispatch-cache +
# builtins_hashtable.c CL_HT_FLAG_SYNC): a generic function's dispatch cache is
# a hash table that peer threads read AND write concurrently — the main thread
# loading a system while worker / watcher threads (sento, log4cl) dispatch the
# same GF.  The cache is only consulted on the slow (cache-miss) path; the
# monomorphic inline-cache fast path never touches it.  Previously that table
# was a plain lock-free hash table: two threads splicing new entries onto the
# same bucket — or one thread rehashing (relinking every cell into a doubled
# bucket vector and swapping bucket_vec/bucket_count) while another walks it —
# could cross-link the chains, which crashes rarely under contention with wild
# out-of-bounds bucket reads.
#
# Fix: dispatch caches are now created with %MAKE-SYNC-HASH-TABLE, whose
# GETHASH / (SETF GETHASH) / REMHASH / CLRHASH serialize on a global mutex so a
# concurrent reader never observes a half-applied splice or rehash.  All
# consing / bucket-vector growth happens BEFORE the lock, so the critical
# section is allocation-free (never parks at a GC safepoint holding the lock)
# and the monomorphic fast path stays completely lock-free.
#
# The scenario below hammers ONE polymorphic generic function from many threads
# over MANY argument classes at once, starting from an empty cache each round
# (a fresh GF per round) so the empty->filled->rehashed transitions — the exact
# window where two threads splice/grow the same table — happen repeatedly and
# simultaneously.  A cross-linked or half-swapped table shows up as a crash
# (non-zero exit) or a wrong / errored dispatch result.
#
# Run: sh tests/test_mt_dispatch_cache_race.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_mt_dispatch_cache_race: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/amilisp_mtcache_XXXXXX") || exit 1
cleanup() { rm -f "$tmp"; }
trap cleanup EXIT INT TERM

cat > "$tmp" <<'EOF'
;; 40 distinct classes, each with its own method returning its index.  40
;; classes overflow a 16-bucket dispatch cache several times over, forcing
;; multiple rehashes as the cache fills.
(defparameter *n-classes* 40)
(defparameter *classes*
  (let ((v (make-array *n-classes*)))
    (dotimes (i *n-classes*)
      (let ((name (intern (format nil "CACHE-CLASS-~a" i))))
        (eval `(defclass ,name () ()))
        (setf (aref v i) name)))
    v))
;; One instance per class, reused across rounds.
(defparameter *instances*
  (let ((v (make-array *n-classes*)))
    (dotimes (i *n-classes*)
      (setf (aref v i) (make-instance (aref *classes* i))))
    v))

(defparameter *fails* 0)
(defparameter *lk* (mp:make-lock))
(defun note-fail () (mp:with-lock-held (*lk*) (setf *fails* (+ *fails* 1))))

;; (Re)define the GF cache-dispatch target as a fresh GF: each call to
;; INSTALL-GF clears the dispatch cache so the next round of concurrent
;; dispatch refills it from empty (max splice/rehash contention).
(defun install-gf ()
  (eval '(defgeneric cache-dispatch (x)))
  (dotimes (i *n-classes*)
    (eval `(defmethod cache-dispatch ((x ,(aref *classes* i))) ,i))))

;; Each worker dispatches every class, several passes, checking the result.
(defun worker (passes)
  (dotimes (p passes)
    (dotimes (i *n-classes*)
      (let ((r (handler-case (cache-dispatch (aref *instances* i))
                 (error () :FAIL))))
        (unless (eql r i) (note-fail))))))

(defparameter *rounds* 8)
(defparameter *threads-per-round* 8)
(defparameter *passes* 30)

(dotimes (round *rounds*)
  (install-gf)                 ; fresh, empty dispatch cache
  (let ((threads '()))
    (dotimes (w *threads-per-round*)
      (push (mp:make-thread (lambda () (worker *passes*))) threads))
    (dolist (th threads) (mp:join-thread th))))

(format t "MT-CACHE fails=~a~%" *fails*)

;; A genuine no-applicable-method must still error (the sync table must not
;; change dispatch semantics, only make the cache thread-safe).
(defgeneric only-int (x))
(defmethod only-int ((x integer)) :i)
(let ((genuine (handler-case (only-int "s") (error () :caught))))
  (format t "MT-CACHE genuine=~a~%" genuine))
EOF

# A corrupted table crashes non-deterministically, so repeat a few times.
RUNS=5
i=0
while [ "$i" -lt "$RUNS" ]; do
    i=$((i + 1))
    out=$("$TIMEOUT" 60 "$AMILISP" --no-userinit --heap 16M --non-interactive --load "$tmp" </dev/null 2>&1)
    ec=$?
    if [ "$ec" -eq 124 ]; then
        echo "  FAIL mt_dispatch_cache_race (run $i/$RUNS timed out)"
        echo "    output: $(echo "$out" | tail -5)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    elif [ "$ec" -ne 0 ]; then
        echo "  FAIL mt_dispatch_cache_race (run $i/$RUNS ec=$ec — crash, likely a corrupted dispatch-cache table)"
        echo "    output: $(echo "$out" | tail -8)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    elif ! echo "$out" | grep -q "MT-CACHE fails=0"; then
        echo "  FAIL mt_dispatch_cache_race (run $i/$RUNS — concurrent dispatch returned a wrong/errored result)"
        echo "    output: $(echo "$out" | grep 'MT-CACHE' | head -2)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    elif ! echo "$out" | grep -q "MT-CACHE genuine=CAUGHT"; then
        echo "  FAIL mt_dispatch_cache_race (run $i/$RUNS — genuine no-applicable-method masked by sync cache)"
        echo "    output: $(echo "$out" | grep 'MT-CACHE' | head -2)"
        echo ""; echo "0 passed, 1 failed, 1 total"; echo "FAIL"; exit 1
    fi
done

echo "  ok  mt_dispatch_cache_race ($RUNS runs clean)"
echo ""
echo "1 passed, 0 failed, 1 total"
echo "PASS"
exit 0
