#!/bin/sh
# Compile lib/ modules to FASLs with the HOST binary, for shipping to the
# Amiga/MorphOS binaries (which load them instead of compiling the source on
# a 68020 at first REQUIRE).
#
#   scripts/compile-lib-fasls.sh [-o OUTROOT] [-b AMILISP] [-D] [FILE...]
#
#   -o OUTROOT   where lib/<module>.fasl lands (default: the repo root, i.e.
#                next to the sources — what `make fasl-amiga` does; the
#                binary release points it at its staging directory)
#   -b AMILISP   host binary (default build/host/amilisp)
#   -D, --no-docstrings
#                bind AMIGA.FFI:*DEFCFUN-DOCSTRINGS* and
#                EXT:*CAPTURE-DOCUMENTATION* to NIL for the run, so the
#                DEFCFUN bindings in the FASLs carry no docstrings (the C
#                prototypes stay in the .lisp sources) and neither do the
#                DEFUN / DEFMACRO / DEFVAR / DEFCLASS ... forms.  ~16 KB of
#                heap per raw OS module on the target; `make fasl-amiga` and
#                the binary release pass it, a host-only build may leave
#                them in.
#   FILE...      sources relative to the repo root (default: every .lisp
#                under lib/amiga/, the ReAction/raw-bindings tree)
#
# Every file is compiled by ONE amilisp process with AMILISP_FASL_PORTABLE=1,
# so a string literal that the byte-string Amiga builds could not load
# (anything above U+007F) fails the run here, on the host, with the source
# line in the diagnostic — instead of the Amiga failing the module with
# BAD_TAG at REQUIRE time.  A file is only accepted when compile-file
# produced its FASL AND printed no ERROR/warning line for it: compile-file
# recovers per form, so a read error (e.g. a package missing at compile
# time) would otherwise yield a FASL that silently lacks forms.
#
# REQUIRE prefers lib/<module>.fasl over lib/<module>.lisp when the FASL's
# header matches the running binary and it is at least as new as the source,
# so the output must be written AFTER the sources it sits next to.
#
# See tests/test_lib_fasl_portable.sh (which runs this on every shipped
# module) and scripts/make-binary-release.sh.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUTROOT="$ROOT"
AMILISP="$ROOT/build/host/amilisp"
HEAP="${LIBFASL_HEAP:-256M}"
DOCSTRINGS=1

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUTROOT="$2"; shift 2 ;;
        -b) AMILISP="$2"; shift 2 ;;
        -D|--no-docstrings) DOCSTRINGS=0; shift ;;
        -h|--help) sed -n '2,36p' "$0"; exit 0 ;;
        --) shift; break ;;
        -*) echo "compile-lib-fasls: unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done

# Absolute already?  A drive-letter path (C:/...) is what the Makefile hands
# the tests as TMPDIR on Windows -- it is absolute for the native amilisp.exe
# the paths end up in, and must not get $(pwd) glued in front.
case "$OUTROOT" in /*|[A-Za-z]:/*) ;; *) OUTROOT="$(pwd)/$OUTROOT" ;; esac
case "$AMILISP" in /*|[A-Za-z]:/*) ;; *) AMILISP="$(pwd)/$AMILISP" ;; esac
[ -x "$AMILISP" ] || { echo "compile-lib-fasls: no host binary at $AMILISP (make host)" >&2; exit 2; }

cd "$ROOT" || exit 2

if [ $# -eq 0 ]; then
    set -- $(find lib/amiga -name '*.lisp' | LC_ALL=C sort)
fi

TMPD=$(mktemp -d "${TMPDIR:-/tmp}/compile-lib-fasls_XXXXXX") || exit 2
trap 'rm -rf "$TMPD"' EXIT INT TERM
DRIVER="$TMPD/driver.lisp"
LOG="$TMPD/log"

# One driver form per file; each prints a marker so the log can be split
# per file and ERROR lines attributed to the module that produced them.
: > "$DRIVER"
if [ "$DOCSTRINGS" = 0 ]; then
    # The switch must be in effect at macroexpansion time, i.e. in the
    # compiling process, before the first DEFCFUN expands.  lib/amiga/ffi.lisp
    # is itself one of the files compiled below; REQUIREing it here first
    # (source or an existing FASL, whichever REQUIRE picks) and then
    # setting the variable is enough — DEFVAR keeps the NIL when the
    # file's own form runs again during its compile.
    # EXT:*CAPTURE-DOCUMENTATION* is read by the compiler for every
    # documented definition, so the compiling process must have it NIL.
    cat >> "$DRIVER" <<'EOF'
(require "amiga/ffi")
(setf amiga.ffi:*defcfun-docstrings* nil)
(setf ext:*capture-documentation* nil)
(format t "~&LIBFASL: DEFCFUN and definition docstrings disabled for this run~%")
EOF
fi
n=0
for src in "$@"; do
    case "$src" in
        lib/*.lisp) ;;
        *) echo "compile-lib-fasls: $src is not a lib/*.lisp path" >&2; exit 2 ;;
    esac
    [ -f "$src" ] || { echo "compile-lib-fasls: $src not found" >&2; exit 2; }
    out="$OUTROOT/${src%.lisp}.fasl"
    mkdir -p "$(dirname "$out")"
    rm -f "$out"
    cat >> "$DRIVER" <<EOF
(format t "~&LIBFASL-BEGIN $src~%")
(compile-file "$src" :output-file "$out")
(format t "~&LIBFASL-END $src~%")
EOF
    n=$((n + 1))
done

AMILISP_NO_USERINIT=1 AMILISP_FASL_PORTABLE=1 "$AMILISP" --no-userinit \
    --non-interactive --heap "$HEAP" --load "$DRIVER" </dev/null > "$LOG" 2>&1
rc=$?

fail=0
for src in "$@"; do
    out="$OUTROOT/${src%.lisp}.fasl"
    # the module's own slice of the log
    slice=$(awk -v s="LIBFASL-BEGIN $src" -v e="LIBFASL-END $src" \
                '$0 == s {on=1; next} $0 == e {on=0} on' "$LOG")
    bad=$(printf '%s\n' "$slice" | grep -E '^ERROR:|^; Warning: FASL unit failed|^WARNING:' | head -5)
    if [ -n "$bad" ]; then
        echo "compile-lib-fasls: FAIL $src" >&2
        printf '%s\n' "$bad" | sed 's/^/    /' >&2
        rm -f "$out"
        fail=1
    elif [ ! -s "$out" ]; then
        echo "compile-lib-fasls: FAIL $src -- no FASL produced" >&2
        printf '%s\n' "$slice" | grep -v '^; Loading' | tail -5 | sed 's/^/    /' >&2
        fail=1
    fi
done

if [ $rc -ne 0 ] && [ $fail -eq 0 ]; then
    echo "compile-lib-fasls: amilisp exited with status $rc" >&2
    grep -v '^; Loading\|^LIBFASL-' "$LOG" | tail -10 | sed 's/^/    /' >&2
    fail=1
fi

if [ $fail -ne 0 ]; then
    echo "compile-lib-fasls: FAILED (see above)" >&2
    exit 1
fi
echo "compile-lib-fasls: $n FASLs written under $OUTROOT/lib"
exit 0
