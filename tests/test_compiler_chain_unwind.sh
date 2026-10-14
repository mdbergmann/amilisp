#!/bin/sh
# Compiler-chain unwind across non-local exits (anchor-based, both directions).
#
# 1. LEAK direction: a compile-time error caught by HANDLER-CASE longjmps to
#    the VM's NLX landing.  The abandoned cl_compile_env compiler must be
#    freed there — the old protect-flag walk stopped at it and left it on
#    the active-compiler chain forever, so every later "top-level" compile
#    became its child and inherited its stale optimize settings: DECLAIM/
#    PROCLAIM appeared dead for the rest of the session (Amiga suite
#    failures "the safety 0" and "declaim survives fasl round trip").
#
# 2. LIVE direction: a macroexpander performing a non-local exit that lands
#    INSIDE a nested VM run must NOT free the enclosing in-progress
#    compiler (the use-after-free the protect flag was originally added
#    for — babel's INSTANTIATE-CONCRETE-MAPPINGS).  The anchor test keeps
#    any compiler owned by a still-live C frame.
#
# Run: sh tests/test_compiler_chain_unwind.sh build/host/amilisp

CLAMIGA="${1:-build/host/amilisp}"

TIMEOUT=$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true)
if [ -z "$TIMEOUT" ]; then
    echo "SKIP test_compiler_chain_unwind: neither timeout nor gtimeout on PATH"
    exit 0
fi

tmp=$(mktemp "${TMPDIR:-/tmp}/clamiga_chain_XXXXXX") || exit 1
trap 'rm -f "$tmp"' EXIT

cat > "$tmp" <<'EOF'
;; --- 1. Caught compile error must fully unwind the compiler chain ---------
;; The dotted-lambda-list error signals at COMPILE time inside EVAL; the
;; HANDLER-CASE landing must free the abandoned compiler so the DECLAIM
;; below seeds a fresh top-level chain from the proclaimed global.
(handler-case
    (eval '(destructuring-bind (a &key b . c) '(1 :b 2 3 4) (list a b c)))
  (error () nil))
(declaim (optimize (safety 0)))
(format t "DECLAIM-AFTER-ERROR:~A~%"
        (handler-case (eval '(the fixnum "elided"))
          (type-error () "DEAD")))
(declaim (optimize (safety 1)))
(format t "DECLAIM-RESET:~A~%"
        (handler-case (eval '(the fixnum "checked"))
          (type-error () "SIGNALS")))

;; --- 2. THROW across an in-progress compile ------------------------------
;; The CATCH landing predates the compile: the abandoned cl_compile_env and
;; compile_lambda compilers must be freed at the landing.
(defmacro %chain-throwing-macro () (throw 'chain-tag :thrown))
(format t "THROW-TOPLEVEL:~A~%"
        (catch 'chain-tag (eval '(list (%chain-throwing-macro)))))
(format t "THROW-IN-LAMBDA:~A~%"
        (catch 'chain-tag
          (eval '(funcall (lambda () (list (%chain-throwing-macro)))))))

;; --- 3. NLX landing INSIDE the expander's own VM run ----------------------
;; The enclosing in-progress compiler is owned by a live C frame and must
;; survive the landing; the compile then completes normally.
(defmacro %chain-self-catching-macro ()
  (catch 'chain-inner (throw 'chain-inner ''survived)))
(format t "INNER-LANDING:~A~%"
        (eval '(funcall (lambda () (%chain-self-catching-macro)))))

;; --- 4. Pool-recycling stress: chain must stay clean across many cycles ---
(dotimes (i 50)
  (handler-case
      (eval '(destructuring-bind (a &key b . c) '(1 :b 2) (list a b c)))
    (error () nil))
  (catch 'chain-tag (eval '(list (%chain-throwing-macro)))))
(declaim (optimize (safety 0)))
(format t "DECLAIM-AFTER-STRESS:~A~%"
        (handler-case (eval '(the fixnum "elided"))
          (type-error () "DEAD")))
(declaim (optimize (safety 1)))
EOF

fail() {
    echo "FAIL compiler_chain_unwind ($1)"
    echo "$out" | tail -10 | sed 's/^/    /'
    echo "0 passed, 1 failed, 1 total"
    exit 1
}

out=$("$TIMEOUT" 60 "$CLAMIGA" --no-userinit --non-interactive --load "$tmp" \
      </dev/null 2>&1)
status=$?
[ $status -eq 0 ] || fail "exit $status: hang or crash"
printf '%s' "$out" | grep -q "DECLAIM-AFTER-ERROR:elided" \
    || fail "leaked compiler chain: DECLAIM dead after caught compile error"
printf '%s' "$out" | grep -q "DECLAIM-RESET:SIGNALS" \
    || fail "safety 1 not restored (THE check missing)"
printf '%s' "$out" | grep -q "THROW-TOPLEVEL:THROWN" \
    || fail "throw across in-progress compile broke"
printf '%s' "$out" | grep -q "THROW-IN-LAMBDA:THROWN" \
    || fail "throw across compile_lambda broke"
printf '%s' "$out" | grep -q "INNER-LANDING:SURVIVED" \
    || fail "in-progress compiler freed by inner NLX landing (use-after-free)"
printf '%s' "$out" | grep -q "DECLAIM-AFTER-STRESS:elided" \
    || fail "compiler chain dirty after pool-recycling stress"

echo "  ok  compiler_chain_unwind (leak + live-owner directions)"
echo "1 passed, 0 failed, 1 total"
exit 0
