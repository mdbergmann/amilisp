#!/bin/sh
# Every write of a live symbol's function cell must go through
# cl_symbol_set_function (mem.c), because that is what bumps cl_call_gen,
# the word every m68k JIT call site checks before jumping straight into a
# cached callee (specs/jit-direct-calls.md §1).  A raw `->function =` that
# slips past the setter leaves the sites valid, so a redefined function
# keeps calling its old code -- on the Amiga only, and only once the
# caller has turned hot.  No runtime test can catch every such site, so
# this check reads the sources.
#
# A line that must stay raw (a fresh symbol nothing can cache yet, a
# non-symbol struct with a `function` slot, the setter itself) carries a
# `symfn-raw: <reason>` comment on the same line.
#
# Run: sh tests/test_symfn_funnel.sh        (no amilisp binary needed)

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/clamiga_symfn_$$"
passed=0
failed=0
total=0
trap 'rm -rf "$WORK"' EXIT

ok()   { passed=$((passed + 1)); total=$((total + 1)); echo "  ok  $1"; }
fail() { failed=$((failed + 1)); total=$((total + 1)); echo "  FAIL  $1"; }

# Print file:line:text of each raw function-cell store under $1/src.
# Comment lines and `==` comparisons are not stores.  The $1 prefix is
# stripped first: on Windows TMPDIR is `C:/...`, and the drive colon
# would throw off the file:line: match of the comment filter.
raw_writes() {
    grep -rnE --include='*.c' --include='*.h' \
         '(->|\.)function[[:space:]]*=([^=]|$)' "$1/src" 2>/dev/null \
        | sed "s|^$1/||" \
        | grep -v 'symfn-raw:' \
        | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(\*|/\*|//)'
}

# 1. The check still finds what it is for: the struct-pointer and the
#    cast forms, a store with the value on the next line, a `.function`
#    member store -- and leaves alone a comparison, a comment, a marked
#    line and a read.
mkdir -p "$WORK/src/core"
cat > "$WORK/src/core/fixture.c" <<'EOF'
void bad(CL_Symbol *s, CL_Obj sym, CL_Obj fn, CL_Symbol copy)
{
    s->function = fn;                                         /* BAD 3 */
    ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->function = fn;         /* BAD 4 */
    s->function =
        fn;                                                   /* BAD 5 */
    copy.function = fn;                                       /* BAD 7 */
}

void good(CL_Symbol *s, CL_Obj sym, CL_Obj fn)
{
    if (s->function == CL_UNBOUND) return;
    /* s->function = fn; */
     * s->function = fn, in a block comment
    s->function = CL_UNBOUND;   /* symfn-raw: fresh symbol */
    fn = s->function;
    cl_symbol_set_function(sym, fn);
}
EOF
lines=$(raw_writes "$WORK" | sed -n 's/^src\/core\/fixture\.c:\([0-9]*\):.*/\1/p' | tr '\n' ' ')
if [ "$lines" = "3 4 5 7 " ]; then
    ok "checker_flags_raw_function_cell_stores"
else
    fail "checker_flags_raw_function_cell_stores (lines '$lines', want '3 4 5 7 ')"
fi

# 2. The runtime sources are clean.
out=$(raw_writes "$ROOT")
if [ -z "$out" ]; then
    ok "src_writes_function_cells_only_through_the_setter"
else
    fail "src_writes_function_cells_only_through_the_setter"
    echo "        raw function-cell store(s) -- use cl_symbol_set_function(sym, fn)"
    echo "        (mem.h), or mark a store that must stay raw with 'symfn-raw: <why>':"
    printf '%s\n' "$out" | sed 's/^/          /'
fi

# 3. The setter exists and bumps: a refactor that drops the bump from it
#    would leave this lint green and every call site stale.
if awk '/^void cl_symbol_set_function\(/ { inside = 1 }
        inside && /cl_call_gen_bump\(/ { found = 1 }
        inside && /^}/ { exit }
        END { exit !found }' "$ROOT/src/core/mem.c"; then
    ok "setter_bumps_the_call_generation"
else
    fail "setter_bumps_the_call_generation (cl_symbol_set_function in src/core/mem.c)"
fi

echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
