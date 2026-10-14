#!/bin/sh
# Executable specification of the portable-lib-FASL contract.
#
# The binary release (scripts/make-binary-release.sh), `make fasl` and
# `make fasl-amiga` compile lib/ modules on the HOST and ship the FASLs to the
# AmigaOS/MorphOS binaries.  The m68k AmigaOS ones are byte-string builds (no
# CL_WIDE_STRINGS; MorphOS is wide but loads the same FASLs): they have no
# decoder for FASL_TAG_WIDE_STRING, so a string literal with a character
# above U+007F in a shipped module makes REQUIRE fail the whole module with
# BAD_TAG on the Amiga.  AMILISP_FASL_PORTABLE=1 makes the host's
# FASL writer refuse such a string at compile time instead, naming the code
# point and the source line (src/core/fasl.c, FASL_ERR_NONPORTABLE).
#
# Part 1  the guard: a non-ASCII string literal -> no FASL and a diagnostic
#         with U+2014 and the source line; an ASCII literal compiles; with the
#         mode off the same non-ASCII literal compiles and loads (the host's
#         own behaviour is unchanged).
# Part 2  every shipped module compiles in portable mode through
#         scripts/compile-lib-fasls.sh: lib/boot lib/clos lib/ffi
#         lib/gray-streams (the release's FASL_LIBS) and all of lib/amiga/**
#         -- an em dash slipping into an (error "...") message in lib/ fails
#         `make test` here, not the Amiga at REQUIRE time.
# Part 3  the FASLs work from a deployed layout: with the temp root as cwd
#         (lib/<module>.fasl only, no sources next to them) the process boots
#         from the fresh boot.fasl/clos.fasl, REQUIRE loads the lib/amiga FASLs
#         (the "; Loading" line names the .fasl) and the modules work.
#
# Run: sh tests/test_lib_fasl_portable.sh build/host/amilisp

AMILISP="${1:-build/host/amilisp}"
ROOT=$(cd "$(dirname "$0")/.." && pwd)
case "$AMILISP" in /*) ;; *) AMILISP="$(pwd)/$AMILISP" ;; esac
TMPD=$(mktemp -d "${TMPDIR:-/tmp}/test_lib_fasl_portable_XXXXXX") || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM

cd "$ROOT" || exit 1
passed=0
failed=0
ok()   { passed=$((passed + 1)); echo "  ok    $1"; }
fail() { failed=$((failed + 1)); echo "  FAIL  $1"; [ -n "$2" ] && printf '%s\n' "$2" | sed 's/^/        /'; }

run() {  # run [ENV=val ...] -- forms... ; prints amilisp's output
    env "$@" </dev/null 2>&1
}

# ---------------------------------------------------------------- Part 1
echo "=== test_lib_fasl_portable: the AMILISP_FASL_PORTABLE=1 guard ==="
# the em dash is in a RUNTIME string (docstrings are dropped by the compiler)
printf '(defvar *np-msg* "em dash \342\200\224 here")\n(defun np-len () (length *np-msg*))\n' > "$TMPD/np.lisp"
printf '(defvar *ok-msg* "plain ascii -- fine")\n' > "$TMPD/ok.lisp"

out=$(run AMILISP_NO_USERINIT=1 AMILISP_FASL_PORTABLE=1 "$AMILISP" --no-userinit --non-interactive \
        --eval "(compile-file \"$TMPD/np.lisp\" :output-file \"$TMPD/np.fasl\")" --eval '(quit)')
if [ -e "$TMPD/np.fasl" ]; then
    fail "portable mode must not write a FASL with a non-ASCII string literal"
else
    ok "portable mode: no FASL for a non-ASCII string literal"
fi
if echo "$out" | grep -q 'U+2014' && echo "$out" | grep -q 'src-line=1'; then
    ok "diagnostic names the code point (U+2014) and the source line"
else
    fail "diagnostic must name U+2014 and src-line=1" "$out"
fi
if echo "$out" | grep -q 'AMILISP_FASL_PORTABLE'; then
    ok "diagnostic names the knob"
else
    fail "diagnostic must mention AMILISP_FASL_PORTABLE" "$out"
fi

out=$(run AMILISP_NO_USERINIT=1 AMILISP_FASL_PORTABLE=1 "$AMILISP" --no-userinit --non-interactive \
        --eval "(compile-file \"$TMPD/ok.lisp\" :output-file \"$TMPD/ok.fasl\")" --eval '(quit)')
if [ -s "$TMPD/ok.fasl" ]; then
    ok "portable mode: ASCII string literals compile"
else
    fail "ASCII literal must compile in portable mode" "$out"
fi

out=$(run AMILISP_NO_USERINIT=1 AMILISP_FASL_PORTABLE=0 "$AMILISP" --no-userinit --non-interactive \
        --eval "(compile-file \"$TMPD/np.lisp\" :output-file \"$TMPD/np.fasl\")" --eval '(quit)')
if [ -s "$TMPD/np.fasl" ]; then
    ok "mode off: the non-ASCII literal compiles (host behaviour unchanged)"
    out=$(run AMILISP_NO_USERINIT=1 "$AMILISP" --no-userinit --non-interactive \
            --eval "(load \"$TMPD/np.fasl\")" --eval '(format t "~&NP-LEN=~D~%" (np-len))' --eval '(quit)')
    if echo "$out" | grep -q '^NP-LEN=14$'; then
        ok "mode off: the wide string round-trips through the FASL on the host"
    else
        fail "wide string FASL must load on the host (NP-LEN=14)" "$out"
    fi
else
    fail "mode off must still write the FASL" "$out"
fi

# ---------------------------------------------------------------- Part 2
echo "=== test_lib_fasl_portable: every shipped lib module compiles portably ==="
# Mirrors scripts/make-binary-release.sh: FASL_LIBS + lib/amiga/**.
SHIPPED="lib/boot.lisp lib/clos.lisp lib/ffi.lisp lib/gray-streams.lisp lib/dev-commands.lisp lib/dev-repl.lisp lib/dev-tcp.lisp $(find lib/amiga -name '*.lisp' | LC_ALL=C sort | tr '\n' ' ')"
n_src=$(echo $SHIPPED | wc -w | tr -d ' ')
out=$(sh scripts/compile-lib-fasls.sh -o "$TMPD" -b "$AMILISP" $SHIPPED 2>&1)
rc=$?
n_fasl=$(find "$TMPD/lib" -name '*.fasl' | wc -l | tr -d ' ')
if [ $rc -eq 0 ] && [ "$n_fasl" = "$n_src" ]; then
    ok "compile-lib-fasls: all $n_src shipped modules compiled in portable mode"
else
    fail "compile-lib-fasls failed (rc=$rc, $n_fasl/$n_src FASLs)" "$(echo "$out" | tail -20)"
fi

# ---------------------------------------------------------------- Part 3
echo "=== test_lib_fasl_portable: deployed layout (FASLs only, cwd = root) ==="
cat > "$TMPD/deployed.lisp" <<'EOF'
(require "amiga/raw/exec")
(format t "~&MEMF-CHIP=~A~%" (symbol-value (find-symbol "+MEMF-CHIP+" "AMIGA.RAW.EXEC")))
(require "amiga/intuition")
(format t "~&IDCMP-CLOSEWINDOW=~A~%" (symbol-value (find-symbol "+IDCMP-CLOSEWINDOW+" "AMIGA.INTUITION")))
(require "amiga/reaction")
(format t "~&REACTION=~A~%" (if (find-package "AMIGA.REACTION") "ok" "missing"))
(format t "~&GRAY=~A~%" (if (find-package "GRAY") "ok" "missing"))
EOF
out=$(cd "$TMPD" && run AMILISP_NO_USERINIT=1 AMILISP_HOME= "$AMILISP" --no-userinit --non-interactive \
        --heap 64M --eval '(require "gray-streams")' --load deployed.lisp --eval '(quit)')
# (amilisp prints the resolved path -- /private/var/... for a macOS /var/... TMPDIR --
# so match on the unique temp directory name, not the full prefix)
if echo "$out" | grep -q "^; Loading .*/$(basename "$TMPD")/lib/amiga/raw/exec\.fasl"; then
    ok "REQUIRE loads lib/amiga/raw/exec.fasl from the deployed root"
else
    fail "REQUIRE must load the deployed exec.fasl" "$(echo "$out" | head -12)"
fi
if echo "$out" | grep -q '^MEMF-CHIP=2$' && echo "$out" | grep -q '^IDCMP-CLOSEWINDOW=512$' \
   && echo "$out" | grep -q '^REACTION=ok$' && echo "$out" | grep -q '^GRAY=ok$'; then
    ok "the deployed FASLs work (raw exec, curated intuition, reaction, gray-streams)"
else
    fail "deployed modules must work" "$(echo "$out" | grep -v '^; Loading' | head -12)"
fi
if echo "$out" | grep -q '^ERROR:'; then
    fail "no errors while loading the deployed FASLs" "$(echo "$out" | grep '^ERROR:' | head -5)"
else
    ok "no errors while loading the deployed FASLs"
fi

echo "test_lib_fasl_portable: $passed passed, $failed failed"
[ $failed -eq 0 ]
