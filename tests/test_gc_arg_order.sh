#!/bin/sh
# No call in src/ may pass a heap value next to an allocating call in the
# same argument list -- see tests/gc_arg_order.pl for why, and mem.h
# (cl_list2) for what to write instead.
#
# The collector moves objects, and C does not say in which order a call's
# arguments are evaluated, so whether such a call stores a stale offset
# depends on the C compiler and on where the heap happens to be when the
# nested call collects: a DEFUN compiled as a function call on one host and
# heap size, and nowhere else.  That is not something a runtime test can
# pin down, so this check reads the sources instead.
#
# Run: sh tests/test_gc_arg_order.sh        (no amilisp binary needed)

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CHECK="$ROOT/tests/gc_arg_order.pl"
TMPDIR="${TMPDIR:-/tmp}"
WORK="$TMPDIR/clamiga_argorder_$$"
passed=0
failed=0
total=0
trap 'rm -rf "$WORK"' EXIT

ok()   { passed=$((passed + 1)); total=$((total + 1)); echo "  ok  $1"; }
fail() { failed=$((failed + 1)); total=$((total + 1)); echo "  FAIL  $1"; }

if ! command -v perl > /dev/null 2>&1; then
    echo "  skip  gc_arg_order (no perl)"
    echo "0 passed, 0 failed, 0 total"
    exit 0
fi

# 1. The checker still finds what it is for: both evaluation orders, a call
#    spread over two lines, a heap read (cl_car) as the early argument, and
#    a helper declared only in the fixture -- one finding per outer call, the
#    inner (cl_cons x CL_NIL) being safe.  And it leaves alone what is safe:
#    constants next to an allocation, one allocation per statement, a
#    pointer parameter, a comment or string that merely looks like a call.
mkdir -p "$WORK/src"
cat > "$WORK/src/fixture.c" <<'EOF'
CL_Obj cl_cons(CL_Obj car, CL_Obj cdr);
CL_Obj cl_intern(const char *name, uint32_t len);
void fixture_store(CL_Compiler *c, CL_Obj value);
static CL_Obj fixture_pair(CL_Obj a, CL_Obj b) { return a; }

void bad(CL_Obj x, CL_Obj tail)
{
    x = cl_cons(SYM_LAMBDA, cl_cons(x, CL_NIL));          /* BAD 1 */
    x = cl_cons(cl_cons(x, CL_NIL),
                tail);                                     /* BAD 2 */
    x = fixture_pair(x, cl_intern("Y", 1));                /* BAD 3 */
    x = cl_cons(cl_car(tail), cl_cons(SYM_T, CL_NIL));     /* BAD 4 */
}

void good(CL_Compiler *c, CL_Obj x, CL_Obj tail)
{
    x = cl_cons(CL_MAKE_FIXNUM(1), cl_cons(CL_NIL, CL_NIL));
    x = cl_cons(x, CL_NIL);
    x = cl_cons(SYM_LAMBDA, x);
    fixture_store(c, cl_cons(tail, CL_NIL));
    /* x = cl_cons(SYM_LAMBDA, cl_cons(x, CL_NIL)); */
    puts("cl_cons(SYM_LAMBDA, cl_cons(x, CL_NIL))");
}
EOF
out=$(perl "$CHECK" "$WORK" 2>&1); status=$?
lines=$(printf '%s\n' "$out" | sed -n 's/^src\/fixture\.c:\([0-9]*\):.*/\1/p' | tr '\n' ' ')
if [ "$status" -eq 1 ] && [ "$lines" = "8 9 11 12 " ]; then
    ok "checker_flags_both_orders"
else
    fail "checker_flags_both_orders (exit $status, lines '$lines')"
    printf '%s\n' "$out" | sed 's/^/        /'
fi

# 2. The runtime sources are clean.
out=$(perl "$CHECK" "$ROOT" 2>&1); status=$?
if [ "$status" -eq 0 ] && [ -z "$out" ]; then
    ok "src_has_no_allocation_next_to_heap_argument"
else
    fail "src_has_no_allocation_next_to_heap_argument"
    printf '%s\n' "$out" | sed 's/^/        /'
    echo "        -> build the list one allocation per statement, or use"
    echo "           cl_list2/3/4 / cl_list_star3 (src/core/mem.h)"
fi

echo ""
echo "$passed passed, $failed failed, $total total"
[ "$failed" -eq 0 ]
