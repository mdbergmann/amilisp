#!/bin/sh
# Deep/wide form compilation and the C-stack guard (reader + compiler).
#
# Regression for the 2026-07-16 Amiga silent-corruption bug: source-loading
# a deeply nested macro tower (lambda-tale-engine amiga-ui.lisp) at a small shell
# stack overflowed the C stack inside READ_EXPR recursion — the compiler's
# stack guard never fired because the reader had no check at all, and the
# per-recursion-level frames were huge (read_expr carried a 4KB #* scan
# buffer; compile_and/or/cond/case/typecase carried 2-4KB patch arrays).
#
# Fixes under test:
#   1. Pending-jump chains threaded through the bytecode replaced the
#      per-frame patch arrays — this also removed the 512-clause cap, so
#      wide AND/OR/COND/CASE/ECASE must now compile and run correctly.
#   2. Nested chains: each nesting level keeps an independent chain head —
#      a 300-deep COND tower must compile correctly.
#   3. read_expr calls cl_check_c_stack: a pathologically deep form must
#      signal a clean, catchable error (never crash or corrupt), and the
#      session must remain usable afterwards.
#
# Run: sh tests/test_stack_depth.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_stack_depth: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/amilisp_stackdepth_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
;; --- 1. Wide clauses: beyond the old MAX_PATCHES=512 cap ------------------
(defun wide-cond (x)
  (macrolet ((gen ()
               `(cond ,@(loop for i from 0 below 600
                              collect `((= x ,i) ,(* i 10)))
                      (t :miss))))
    (gen)))
(format t "WIDE-COND:~A,~A,~A~%" (wide-cond 599) (wide-cond 0) (wide-cond 700))

(defun wide-case (x)
  (macrolet ((gen ()
               `(case x ,@(loop for i from 0 below 600
                                collect `((,(* 2 i) ,(1+ (* 2 i))) ,i))
                      (otherwise :miss))))
    (gen)))
(format t "WIDE-CASE:~A,~A,~A~%" (wide-case 1199) (wide-case 0) (wide-case 1200))

(defun wide-and () (macrolet ((gen () `(and ,@(loop for i from 1 to 600 collect i)))) (gen)))
(defun wide-or  () (macrolet ((gen () `(or ,@(loop for i from 1 to 600 collect nil) :last))) (gen)))
(format t "WIDE-AND-OR:~A,~A~%" (wide-and) (wide-or))
;; The same through a GLOBAL macro: AND/OR expand to one nesting level per
;; argument, and the BLOCK exit analysis ran out of GC roots walking it
;; (process abort) until it learned to stop with roots to spare.
(defmacro gwide-or () `(or ,@(loop for i from 1 to 600 collect nil) :last))
(defmacro gwide-and () `(and ,@(loop for i from 1 to 600 collect i)))
(defun wide-or-g () (gwide-or))
(defun wide-and-g () (flet ((f (x) x)) (f (gwide-and))))
(format t "WIDE-GLOBAL:~A,~A~%" (wide-or-g) (wide-and-g))

;; ECASE fall-through must still build the full (member ...) expected-type
(format t "WIDE-ECASE-HIT:~A~%"
        (macrolet ((gen () `(ecase 99 ,@(loop for i from 0 below 550 collect `(,i ,i))))) (gen)))
(handler-case (macrolet ((gen () `(ecase 999 ,@(loop for i from 0 below 550 collect `(,i ,i)))))
                (gen))
  (type-error () (format t "WIDE-ECASE-MISS:TYPE-ERROR~%")))

;; --- 2. Deep nesting: chains must nest independently ----------------------
;; The form appears exactly ONCE per level (bodyless clause returns the
;; test value), so expansion is linear in depth.
(defun deep-cond (x)
  (macrolet ((gen (n)
               (let ((form 'x))
                 (dotimes (i n) (setf form `(cond ((progn ,form)) (t :nil-hit))))
                 form)))
    (gen 300)))
(format t "DEEP-COND:~A,~A~%" (deep-cond 42) (deep-cond nil))

;; --- 3. Reader stack guard: deep form => clean error, session survives ----
(handler-case
    (let ((s (concatenate 'string
                          (make-string 20000 :initial-element #\()
                          "1"
                          (make-string 20000 :initial-element #\)))))
      (read-from-string s)
      (format t "READ-DEEP:READ-OK~%"))   ; acceptable on a huge C stack
  (error () (format t "READ-DEEP:CAUGHT~%")))
(format t "SESSION-ALIVE:~A~%" (+ 1 2))
EOF

fail() {
    echo "FAIL stack_depth ($1)"
    echo "$out" | tail -10 | sed 's/^/    /'
    echo "0 passed, 1 failed, 1 total"
    exit 1
}

out=$("$TIMEOUT" 120 "$AMILISP" --no-userinit --non-interactive --load "$tmp" \
      </dev/null 2>&1)
status=$?
[ $status -eq 0 ] || fail "exit $status: hang or crash"
printf '%s' "$out" | grep -q "WIDE-COND:5990,0,MISS" \
    || fail "wide COND (>512 clauses) miscompiled"
printf '%s' "$out" | grep -q "WIDE-CASE:599,0,MISS" \
    || fail "wide CASE (>512 clauses) miscompiled"
printf '%s' "$out" | grep -q "WIDE-AND-OR:600,LAST" \
    || fail "wide AND/OR (>512 args) miscompiled"
printf '%s' "$out" | grep -q "WIDE-GLOBAL:LAST,600" \
    || fail "wide AND/OR from a global macro (exit analysis out of GC roots)"
printf '%s' "$out" | grep -q "WIDE-ECASE-HIT:99" \
    || fail "wide ECASE match miscompiled"
printf '%s' "$out" | grep -q "WIDE-ECASE-MISS:TYPE-ERROR" \
    || fail "wide ECASE fall-through lost its TYPE-ERROR"
printf '%s' "$out" | grep -q "DEEP-COND:42,NIL-HIT" \
    || fail "300-deep nested COND miscompiled (chain nesting broken)"
printf '%s' "$out" | grep -Eq "READ-DEEP:(CAUGHT|READ-OK)" \
    || fail "deep read crashed instead of clean error"
printf '%s' "$out" | grep -q "SESSION-ALIVE:3" \
    || fail "session unusable after deep-read guard fired"

echo "  ok  stack_depth (wide/deep conditionals + reader stack guard)"
echo "1 passed, 0 failed, 1 total"
exit 0
