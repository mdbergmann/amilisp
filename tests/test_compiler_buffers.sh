#!/bin/sh
# Regression tests for the compiler's growable working buffers.
#
# A CL_Compiler used to hold its bytecode and constants as fixed arrays at the
# hard limits (262144 bytes, 8192 entries): 366 KB per block, and the compiler
# pool pre-warms eight blocks, so every amilisp process carried 3 MB of
# off-heap memory before its first form.  The buffers now start small, grow
# on demand up to the same limits, and a pooled block keeps them between
# compiles unless one compile grew them past the keep size.
#
#   sh tests/test_compiler_buffers.sh build/host/amilisp
#
# Covered:
#   1. The pool at rest: eight blocks, each a fraction of the old size
#      (EXT:%COMPILER-POOL-STATS), and no parked buffer above the keep
#      size after any of the compiles below.
#   2. Bytecode growth: functions past the initial buffer, past the keep
#      size and near the limit compile to the right code; a nested lambda
#      grows its own buffer while its parent's is grown too; oversized
#      buffers are not kept by the pool.
#   3. The limit: a function past 262144 bytes signals a clean error that
#      names the limit, from the top level and from a nested lambda, and the
#      pool gets every block back.
#   4. Constants growth: character and string constants past the initial
#      table and past the keep size come back EQL / EQ to the source objects
#      (under CLAMIGA_GC_STRESS the strings move on every allocation, so the
#      grown table must be what the collector forwards); past 8192 a clean
#      error, and the pool gets its blocks back.
#   5. Several threads compiling large functions at once.
#   6. COMPILE-FILE of a large function, loaded into a fresh process.
#
# Also run under the gc-stress binary (make test-gc-stress).

CLAMIGA="${1:-build/host/amilisp}"
case "$CLAMIGA" in
  /*) ;;
  *) CLAMIGA="$(pwd)/$CLAMIGA" ;;
esac

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)

passed=0
failed=0
total=0
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/clamiga_compbuf_$$"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$CLAMIGA" ]; then
    echo "  SKIP  compiler buffers: no binary at $CLAMIGA"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

# Under forced compaction every cons compacts the heap, so the stress run
# keeps the constants modest and leaves out the 8300-constant overflow: that
# one only stores characters (immediates, nothing for the collector to move)
# and the ordinary run covers it.
if [ -n "${CLAMIGA_GC_STRESS:-}" ]; then
    NCONST=1100
    THREAD_ITERS=2
    CONST_OVERFLOW=0
else
    NCONST=2000
    THREAD_ITERS=10
    CONST_OVERFLOW=1
fi

run() {
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 300 "$CLAMIGA" --no-userinit --non-interactive --load "$1" 2>&1 </dev/null
    else
        "$CLAMIGA" --no-userinit --non-interactive --load "$1" 2>&1 </dev/null
    fi
}

check_contains() {
    desc="$1"; pattern="$2"; actual="$3"
    total=$((total + 1))
    if echo "$actual" | grep -q -- "$pattern"; then
        echo "  ok  $desc"
        passed=$((passed + 1))
    else
        echo "  FAIL  $desc (expected /$pattern/)"
        echo "$actual" | tail -6 | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

# Shared helpers.  DAG builds a body that references LEAF 2^K times while
# consing only K lists, so a function of any bytecode size is cheap to build
# (also under forced compaction).  Each leaf increments ACC, so the function
# returns X + 2^K exactly when every copy was compiled.
cat > "$WORK/prelude.lisp" <<'EOF'
(defun dag (leaf k)
  (if (= k 0) leaf (let ((s (dag leaf (1- k)))) (list 'progn s s))))
(defun big-body (k) (dag '(setq acc (+ acc 1)) k))
(defun big-fn (k)
  `(lambda (x) (let ((acc x)) ,(big-body k) acc)))
;; A function returning a fresh list of OBJS, each one a literal in its body:
;; (APPEND (LIST o0 .. o199) (LIST o200 ..) ..) -- one cons per constant, and
;; every call stays under CALL-ARGUMENTS-LIMIT.
(defun const-fn (objs)
  `(lambda ()
     (append ,@(loop while objs
                     collect (cons 'list
                                   (loop repeat 200 while objs
                                         collect (pop objs)))))))
(defun pool () (ext:%compiler-pool-stats))
;; What the pool may keep between compiles: 16 KB of bytecode and 1024
;; constants per block (compiler.c, CL_COMPILER_*_KEEP) -- so no parked
;; buffer is larger than 16 KB, and all of them together fit eight blocks.
(defun pool-bounded-p ()
  (let ((s (pool)))
    (and (<= (fourth s) 16384)
         (<= (third s) (* (first s) (+ 16384 (* 1024 4)))))))
(defun overflow-text (thunk)
  (handler-case (progn (funcall thunk) "NO-ERROR")
    (error (e) (princ-to-string e))))
EOF

# ---------------------------------------------------------------------------
# 1. The pool at rest
# ---------------------------------------------------------------------------
cat > "$WORK/rest.lisp" <<EOF
(load "$WORK/prelude.lisp")
(let ((s (pool)))
  (format t "PARKED ~D~%" (first s))
  ;; The fixed-array block was 375212 bytes on m68k; what is left (the
  ;; block, tagbody and lambda-list tables) is about 80 KB.
  (format t "BLOCK-SMALL ~A~%" (< 0 (second s) 160000))
  (format t "SHAPE ~A~%" (and (= (length s) 6) (every #'integerp s))))
EOF
out=$(run "$WORK/rest.lisp")
check_contains "pool holds eight blocks at rest"          "PARKED 8" "$out"
check_contains "a pooled block no longer holds the limits" "BLOCK-SMALL T" "$out"
check_contains "%compiler-pool-stats is six integers"      "SHAPE T" "$out"

# ---------------------------------------------------------------------------
# 2. Bytecode growth
# ---------------------------------------------------------------------------
cat > "$WORK/grow.lisp" <<EOF
(load "$WORK/prelude.lisp")
(dolist (k '(0 6 11 13))
  (format t "GROW ~D = ~A~%" k
          (= (funcall (compile nil (big-fn k)) 5) (+ 5 (expt 2 k)))))
(format t "CODE-MAX ~A~%" (>= (fifth (pool)) 131072))
(format t "BIG-NOT-KEPT ~A~%" (pool-bounded-p))
(format t "PARKED-AFTER ~D~%" (first (pool)))
;; A nested lambda compiles in its own block while the parent's is live:
;; both grow past the keep size.
(let ((f (compile nil
           \`(lambda (x)
              (let ((acc x))
                ,(big-body 11)
                (funcall (lambda ()
                           (let ((acc (* acc 2)))
                             ,(big-body 11)
                             acc))))))))
  (format t "NESTED ~A~%" (= (funcall f 1) (+ (* 2 (+ 1 2048)) 2048))))
(format t "NESTED-NOT-KEPT ~A~%" (pool-bounded-p))
;; The same big compile again and again: the pool neither grows nor keeps
;; the big buffers.
(dotimes (i 20) (compile nil (big-fn 11)))
(format t "REPEAT ~D ~A~%" (first (pool)) (pool-bounded-p))
EOF
out=$(run "$WORK/grow.lisp")
check_contains "empty body"                                 "GROW 0 = T" "$out"
check_contains "past the initial bytecode buffer"           "GROW 6 = T" "$out"
check_contains "past the keep size"                         "GROW 11 = T" "$out"
check_contains "131 KB of bytecode"                         "GROW 13 = T" "$out"
check_contains "the buffer grew to the size needed"         "CODE-MAX T" "$out"
check_contains "oversized buffers are not kept"             "BIG-NOT-KEPT T" "$out"
check_contains "every block back in the pool"               "PARKED-AFTER 8" "$out"
check_contains "nested lambda grows its own buffer"         "NESTED T" "$out"
check_contains "nested compile keeps the pool bounded"      "NESTED-NOT-KEPT T" "$out"
check_contains "repeated big compiles leave the pool as is" "REPEAT 8 T" "$out"

# ---------------------------------------------------------------------------
# 3. The bytecode limit
# ---------------------------------------------------------------------------
cat > "$WORK/limit.lisp" <<EOF
(load "$WORK/prelude.lisp")
(let ((msg (overflow-text (lambda () (compile nil (big-fn 15))))))
  (format t "TOP-MSG ~A~%" msg))
(format t "TOP-AFTER ~D ~A ~A~%" (first (pool)) (pool-bounded-p)
        (funcall (compile nil '(lambda (a b) (+ a b))) 1 2))
(let ((msg (overflow-text
            (lambda ()
              (compile nil \`(lambda (x)
                               (let ((acc x))
                                 ,(big-body 6)
                                 (funcall (lambda () ,(big-body 15) acc)))))))))
  (format t "NESTED-MSG ~A~%" (search "Bytecode too large" msg)))
(format t "NESTED-AFTER ~D ~A~%" (first (pool)) (pool-bounded-p))
(dotimes (i 5) (overflow-text (lambda () (compile nil (big-fn 15)))))
(format t "REPEAT-AFTER ~D ~A~%" (first (pool))
        (= (funcall (compile nil (big-fn 11)) 0) 2048))
EOF
out=$(run "$WORK/limit.lisp")
check_contains "overflow signals a clean error"             "TOP-MSG Bytecode too large" "$out"
check_contains "the error names the limit"                  "TOP-MSG .*262144 bytes" "$out"
check_contains "the error says what to do"                  "TOP-MSG .*split the form" "$out"
check_contains "after overflow: blocks back, compiler works" "TOP-AFTER 8 T 3" "$out"
check_contains "overflow in a nested lambda"                "NESTED-MSG [0-9]" "$out"
check_contains "nested overflow returns both blocks"        "NESTED-AFTER 8 T" "$out"
check_contains "repeated overflows lose no block"           "REPEAT-AFTER 8 T" "$out"

# ---------------------------------------------------------------------------
# 4. Constants growth and the constants limit
# ---------------------------------------------------------------------------
cat > "$WORK/consts.lisp" <<EOF
(load "$WORK/prelude.lisp")
(dolist (n '(10 100 $NCONST))
  (let* ((objs (loop for i below n collect (code-char (+ 256 i))))
         (v (funcall (compile nil (const-fn objs)))))
    (format t "CHARS ~D ~A~%" n (equal objs v))))
(let* ((objs (loop for i below $NCONST collect (format nil "s~D" i)))
       (f (compile nil (const-fn objs))))
  ;; Allocate between compile and call so a moving collection runs with the
  ;; grown table as the only reference the function has to its strings.
  (dotimes (i 200) (make-string 10))
  (ext:gc)
  (let ((v (funcall f)))
    (format t "STRINGS ~A ~A~%"
            (and (= (length v) (length objs)) (every #'eq objs v))
            (equal objs v))))
(format t "CONST-MAX ~A~%" (>= (sixth (pool)) 1024))
(format t "CONSTS-NOT-KEPT ~A ~D~%" (pool-bounded-p) (first (pool)))
(when (= $CONST_OVERFLOW 1)
  (let* ((objs (loop for i below 8300 collect (code-char (+ 256 i))))
         (msg (overflow-text (lambda () (compile nil (const-fn objs))))))
    (format t "CONST-MSG ~A~%" msg)))
(format t "CONST-AFTER ~D ~A ~A~%" (first (pool)) (pool-bounded-p)
        (funcall (compile nil '(lambda () (list "a" "b" #\\c)))))
EOF
out=$(run "$WORK/consts.lisp")
check_contains "a few character constants"                  "CHARS 10 T" "$out"
check_contains "past the initial constants table"           "CHARS 100 T" "$out"
check_contains "past the keep size"                         "CHARS $NCONST T" "$out"
check_contains "string constants survive growth and a GC"   "STRINGS T T" "$out"
check_contains "the constants table grew to the size needed" "CONST-MAX T" "$out"
check_contains "oversized tables are not kept"              "CONSTS-NOT-KEPT T 8" "$out"
if [ "$CONST_OVERFLOW" = 1 ]; then
    check_contains "too many constants signals a clean error" "CONST-MSG Too many constants" "$out"
    check_contains "the error names the limit"              "CONST-MSG .*8192 distinct" "$out"
fi
check_contains "after the error: blocks back, compiler works" 'CONST-AFTER 8 T (a b c)' "$out"

# ---------------------------------------------------------------------------
# 5. Threads compiling large functions at once
# ---------------------------------------------------------------------------
cat > "$WORK/threads.lisp" <<EOF
(load "$WORK/prelude.lisp")
(defun worker (seed)
  (let ((ok t))
    (dotimes (i $THREAD_ITERS)
      (let ((k (+ 9 (mod (+ seed i) 4))))
        (unless (= (funcall (compile nil (big-fn k)) seed) (+ seed (expt 2 k)))
          (setq ok nil)))
      (let* ((objs (loop for j below 300 collect (code-char (+ 1000 seed j))))
             (v (funcall (compile nil (const-fn objs)))))
        (unless (equal objs v) (setq ok nil))))
    ok))
(let* ((threads (loop for s below 4
                      collect (let ((s s)) (mp:make-thread (lambda () (worker s))))))
       (results (mapcar #'mp:join-thread threads)))
  (format t "THREADS ~A~%" (every #'identity results))
  (format t "THREADS-POOL ~A ~A~%" (>= (first (pool)) 8) (pool-bounded-p)))
EOF
out=$(run "$WORK/threads.lisp")
check_contains "four threads compile large functions"       "THREADS T" "$out"
check_contains "pool intact and bounded after the threads"  "THREADS-POOL T T" "$out"

# ---------------------------------------------------------------------------
# 6. COMPILE-FILE of a large function
# ---------------------------------------------------------------------------
cat > "$WORK/bigfile.lisp" <<EOF
(eval-when (:compile-toplevel :load-toplevel :execute)
  (load "$WORK/prelude.lisp"))
(defmacro with-big-body (k) (big-body k))
(defun big-from-file (x) (let ((acc x)) (with-big-body 12) acc))
EOF
cat > "$WORK/cf.lisp" <<EOF
(compile-file "$WORK/bigfile.lisp" :output-file "$WORK/bigfile.fasl")
(format t "CF-DONE~%")
EOF
cat > "$WORK/cfload.lisp" <<EOF
(load "$WORK/bigfile.fasl")
(format t "CF-RESULT ~A~%" (big-from-file 7))
EOF
out=$(run "$WORK/cf.lisp")
check_contains "compile-file of a 64 KB function"           "CF-DONE" "$out"
out=$(run "$WORK/cfload.lisp")
check_contains "the FASL runs in a fresh process"           "CF-RESULT 4103" "$out"

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
