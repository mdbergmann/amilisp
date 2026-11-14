/* runtime.c -- C helpers invoked by JIT-emitted native code, the ones
 * every backend shares: their contract is CPU-neutral (arguments by value,
 * the result returned), and each mirrors the bytecode VM's slow path so
 * native code and the interpreter behave and fail alike.
 *
 * The walkers' own helpers live beside this file, split by frame design
 * rather than by CPU:
 *   runtime_m68k.c    - the m68k walker's: its operand stack is the m68k
 *                       stack (call sites, inline-setjmp NLX frames, the
 *                       &key prologue, OP_AMIGA_CALL);
 *   runtime_vmstack.c - a walker whose locals and operand stack live in
 *                       cl_vm.stack (AArch64 today; PPC or x86-64 built the
 *                       same way would reuse it).
 *
 * GC: see runtime.h.  These helpers may allocate, which may GC.  The m68k
 * walker's cached operand-stack values are reached by the conservative
 * m68k-stack scan (mem.c::gc_scan_jit_native_stack); a VM-stack walker
 * writes cl_vm.sp before every call, so its values are ordinary roots.
 */

#if defined(JIT_M68K) || defined(JIT_A64)

#include "jit/runtime.h"
#include "core/types.h"
#include "core/float.h"      /* CL_NUMBER_P, CL_REALP */
#include "core/error.h"
#include "core/symbol.h"     /* SYM_STAR_PACKAGE, SYM_TYPE_ERROR, KW_DATUM, KW_EXPECTED_TYPE */
#include "core/printer.h"    /* cl_prin1_to_string (OP_ASSERT_TYPE diagnostic) */
#include "core/package.h"    /* cl_sync_current_package_from_dynamic */
#include "core/thread.h"     /* cl_symbol_value / cl_set_symbol_value */
#include "core/opcodes.h"    /* CL_CMP_BR_*, CL_AREF_KIND_* */
#include "core/vm.h"         /* cl_dynbind_restore_to, CL_MAX_DYN_BINDINGS */
#include "core/mem.h"        /* cl_heap.arena_size */
#include "core/string_utils.h" /* cl_string_length, cl_string_set_char_at */
#include "core/builtins.h"   /* cl_vector_ref1 */
#include "platform/platform_thread.h" /* platform_memory_barrier */

/* Forward decls for the existing C-runtime arithmetic — same helpers
 * the bytecode VM falls through to from its OP_ADD / OP_LT slow paths.
 * Keeping the JIT slow path identical to the VM's keeps behaviour
 * (and error messages) consistent across the two execution modes. */
extern int32_t cl_bytevec_check_value(CL_Obj value, int is_signed,
                                      int elt_shift, const char *ctx);
extern CL_Obj cl_arith_add(CL_Obj a, CL_Obj b);
extern CL_Obj cl_arith_sub(CL_Obj a, CL_Obj b);
extern CL_Obj cl_arith_mul(CL_Obj a, CL_Obj b);
extern CL_Obj cl_arith_div(CL_Obj a, CL_Obj b);
extern int    cl_arith_compare(CL_Obj a, CL_Obj b);
extern int    cl_numeric_equal(CL_Obj a, CL_Obj b);
extern CL_Obj cl_car(CL_Obj obj);
extern CL_Obj cl_cdr(CL_Obj obj);
extern CL_Obj cl_cons(CL_Obj car, CL_Obj cdr);

/* From src/core/vm.c — the universal call entry point.  Handles C
 * builtins directly, sets up a stub frame for bytecode/closures, and
 * dispatches through cl_vm_run (which itself routes to native_code if
 * the callee carries one). */
extern CL_Obj cl_vm_apply(CL_Obj func, CL_Obj *args, int nargs);

/* Slow-path `+` (2 args).  Matches the VM's OP_ADD slow path: type-
 * check both operands as NUMBER, then call cl_arith_add which handles
 * fixnum, bignum, ratio, and float combinations. */
CL_Obj cl_jit_runtime_add(CL_Obj a, CL_Obj b)
{
    if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "+");
    if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "+");
    return cl_arith_add(a, b);
}

/* Slow-path `-` (2 args).  Matches the VM's OP_SUB slow path. */
CL_Obj cl_jit_runtime_sub(CL_Obj a, CL_Obj b)
{
    if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "-");
    if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "-");
    return cl_arith_sub(a, b);
}

/* Slow-path REAL comparisons (`<` `>` `<=` `>=`), each returning CL_T or
 * CL_NIL.  Matches the VM's OP_LT/GT/LE/GE slow path: type-check as REAL
 * (CLHS 12.1.4.1 rejects complex), then cl_arith_compare for the
 * cross-type compare.  The four entry points below stay distinct
 * exported JSR targets for the JIT; they differ only in the wanted sign
 * of cl_arith_compare's result and the operator name in the type-error
 * message, so they share this body. */
enum { CMP_LT, CMP_GT, CMP_LE, CMP_GE };
static CL_Obj real_cmp(CL_Obj a, CL_Obj b, int kind, const char *op)
{
    int c;
    if (!CL_REALP(a)) cl_signal_type_error(a, "REAL", op);
    if (!CL_REALP(b)) cl_signal_type_error(b, "REAL", op);
    c = cl_arith_compare(a, b);
    switch (kind) {
        case CMP_LT: return c <  0 ? CL_T : CL_NIL;
        case CMP_GT: return c >  0 ? CL_T : CL_NIL;
        case CMP_LE: return c <= 0 ? CL_T : CL_NIL;
        default:     return c >= 0 ? CL_T : CL_NIL;   /* CMP_GE */
    }
}

CL_Obj cl_jit_runtime_lt(CL_Obj a, CL_Obj b) { return real_cmp(a, b, CMP_LT, "<"); }
CL_Obj cl_jit_runtime_gt(CL_Obj a, CL_Obj b) { return real_cmp(a, b, CMP_GT, ">"); }
CL_Obj cl_jit_runtime_le(CL_Obj a, CL_Obj b) { return real_cmp(a, b, CMP_LE, "<="); }
CL_Obj cl_jit_runtime_ge(CL_Obj a, CL_Obj b) { return real_cmp(a, b, CMP_GE, ">="); }

/* Slow-path `=` (2 args).  Accepts NUMBER (not just REAL — `=` is
 * defined for complex per CLHS 12.1.4.1).  Falls through to
 * cl_numeric_equal which handles cross-type compares. */
CL_Obj cl_jit_runtime_numeq(CL_Obj a, CL_Obj b)
{
    if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "=");
    if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "=");
    return cl_numeric_equal(a, b) ? CL_T : CL_NIL;
}

/* Slow-path `*` (2 args).  No fixnum inline fast path on the JIT side
 * — `*` is rare in tight inner loops, and the MULS.L encoding would
 * roughly double the size of asm_m68k.c for marginal benefit.  All MUL
 * traffic from JIT'd code lands here; the helper itself preserves the
 * VM's inline fixnum fast path inside cl_arith_mul, so fixnum-only
 * MULs are still about as fast as bytecode (just one extra JSR per
 * call instead of inline). */
CL_Obj cl_jit_runtime_mul(CL_Obj a, CL_Obj b)
{
    if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "*");
    if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "*");
    return cl_arith_mul(a, b);
}

/* Slow-path `/` (2 args).  Mirrors the VM's OP_DIV slow path — NUMBER
 * type-check both operands and delegate to cl_arith_div, which performs
 * the inline fixnum-exact-division fast path and falls through to
 * bignum / ratio / float math for inexact or wide-result cases.  May
 * allocate (ratio or bignum result); the walker cache_flushes before
 * the JSR so cached operand-stack values are reachable to the
 * conservative scan. */
CL_Obj cl_jit_runtime_div(CL_Obj a, CL_Obj b)
{
    if (!CL_NUMBER_P(a)) cl_signal_type_error(a, "NUMBER", "/");
    if (!CL_NUMBER_P(b)) cl_signal_type_error(b, "NUMBER", "/");
    return cl_arith_div(a, b);
}

/* Backing for OP_CAR / OP_CDR — pure pass-through to cl_car / cl_cdr,
 * which already handle NIL→NIL, LIST type-errors with the same
 * diagnostic the bytecode VM prints, and the unbound-variable case.
 * Non-allocating, so GC-safe even without precise stack scanning. */
CL_Obj cl_jit_runtime_car(CL_Obj obj) { return cl_car(obj); }
CL_Obj cl_jit_runtime_cdr(CL_Obj obj) { return cl_cdr(obj); }

/* Backing for OP_FLOAD — mirror the VM's lookup: validate that the
 * baked-in constant really is a symbol (the JIT-time check in
 * walker_compile rejects non-symbols before we get here, but stay
 * defensive in case constants[] is mutated after compile), then
 * return s->function (or fall back to s->value for labels/flet
 * value bindings, same as OP_FLOAD).  Signals undefined-function
 * with the same diagnostic the VM emits.  Non-allocating, so this
 * step is GC-safe.  The follow-up OP_CALL is where allocation
 * lives. */
CL_Obj cl_jit_runtime_fload(CL_Obj sym)
{
    CL_Symbol *s;
    CL_Obj fval;

    if (!CL_SYMBOL_P(sym))
        cl_error(CL_ERR_TYPE,
                 "OP_FLOAD: JIT call site has non-symbol constant 0x%08x",
                 (unsigned)sym);

    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    fval = s->function;
    if (fval != CL_UNBOUND) return fval;

    /* labels / flet bind into the value cell, not the function cell. */
    fval = cl_symbol_value(sym);
    if (fval != CL_UNBOUND) return fval;

    cl_error_cell(CL_ERR_UNDEFINED, sym, "Undefined function: %s",
             cl_symbol_name(sym));
    return CL_NIL;   /* unreachable; cl_error longjmps */
}

/* Backing for OP_GLOAD: look up the symbol's dynamic value (thread-
 * local binding first, then the global value cell) and return it.
 * Mirrors the VM's OP_GLOAD: signals UNBOUND-VARIABLE with the same
 * diagnostic when the symbol has no value, otherwise returns the
 * value untouched.  Non-allocating, so always GC-safe. */
CL_Obj cl_jit_runtime_gload(CL_Obj sym)
{
    CL_Obj val = cl_symbol_value(sym);
    if (val == CL_UNBOUND)
        cl_error_cell(CL_ERR_UNBOUND, sym, "Unbound variable: %s",
                 cl_symbol_name(sym));
    return val;
}

/* Backing for OP_GSTORE: store `val` into the symbol's dynamic value
 * (thread-local binding if any, else the global value cell).  Returns
 * `val` so the JIT emitter can leave it as TOS without a separate
 * peek (matches the VM's "store does not pop" semantics for the
 * peek-then-helper path).  Mirrors OP_GSTORE's *PACKAGE* sync so
 * `(setq *package* ...)` updates cl_package_current the same way the
 * bytecode VM does.  Non-allocating, so always GC-safe. */
CL_Obj cl_jit_runtime_gstore(CL_Obj sym, CL_Obj val)
{
    cl_set_symbol_value(sym, val);
    if (sym == SYM_STAR_PACKAGE)
        cl_sync_current_package_from_dynamic();
    return val;
}

/* Backing for OP_FSTORE: write `val` into the symbol's function cell.
 * Mirrors the VM's OP_FSTORE byte-for-byte — peek semantics on the
 * caller side, the function-cell setter (it bumps cl_call_gen) here.  Non-allocating, so always
 * GC-safe; the JIT side reuses the OP_GSTORE peek pattern (flush
 * cache so TOS lives at (a7), push it as the C arg, leave it in
 * place after the JSR drops only the C args). */
CL_Obj cl_jit_runtime_fstore(CL_Obj sym, CL_Obj val)
{
    cl_symbol_set_function(sym, val);
    return val;
}

/* Backing for OP_DYNBIND: save current TLV of `sym` in the dyn-bind
 * stack, then install `new_val` as the new TLV.  Mirrors the VM's
 * OP_DYNBIND exactly: overflow check against CL_MAX_DYN_BINDINGS,
 * record (symbol, old_value) — old_value is CL_TLV_ABSENT if there
 * was no prior binding — install new TLV via cl_tlv_set, and sync
 * cl_package_current when the symbol is *PACKAGE*.  Non-allocating
 * (dyn_stack and tlv_table are preallocated), so always GC-safe.
 * The matching OP_DYNUNBIND undoes one entry per binding pushed. */
void cl_jit_runtime_dynbind(CL_Obj sym, CL_Obj new_val)
{
    CL_Thread *thr = CT;
    CL_Obj old_tlv = cl_tlv_get(thr, sym);
    if (cl_dyn_top >= CL_MAX_DYN_BINDINGS)
        cl_error(CL_ERR_OVERFLOW, "Dynamic binding stack overflow");
    cl_dyn_stack[cl_dyn_top].symbol = sym;
    cl_dyn_stack[cl_dyn_top].old_value = old_tlv;
    cl_dyn_top++;
    cl_tlv_set(thr, sym, new_val);
    if (sym == SYM_STAR_PACKAGE)
        cl_sync_current_package_from_dynamic();
}

/* Backing for OP_DYNUNBIND: restore the last `count` entries from the
 * dyn-bind stack.  Wrapper around cl_dynbind_restore_to so the JIT
 * doesn't have to compute (cl_dyn_top - count) in m68k code.  The
 * restore helper itself handles per-symbol TLV write-back and the
 * *PACKAGE* sync when a *PACKAGE* binding unwinds.  Non-allocating. */
void cl_jit_runtime_dynunbind(uint32_t count)
{
    cl_dynbind_restore_to(cl_dyn_top - (int)count);
}

/* Backing for OP_PROGV_BIND.  Mirrors vm.c::OP_PROGV_BIND exactly:
 * snapshot cl_dyn_top, then for each (sym, val) pair install a new TLV
 * via cl_tlv_set after saving the prior value in the dyn-bind stack.
 * Returns the saved mark as a CL_MAKE_FIXNUM so the JIT can push it as
 * an operand-stack value the matching OP_PROGV_UNBIND will consume.
 * Errors are byte-identical to the VM: type-error on non-symbol in
 * the symbols list, overflow on dyn-stack saturation.  Non-allocating
 * on the success path (dyn_stack and the TLV table are preallocated),
 * but cl_signal_type_error / cl_error allocate a condition object so
 * the walker flushes the cache before the JSR. */
CL_Obj cl_jit_runtime_progv_bind(CL_Obj symbols_list, CL_Obj values_list)
{
    int saved_mark = cl_dyn_top;
    CL_Thread *thr = CT;
    int touched_package = 0;

    while (!CL_NULL_P(symbols_list)) {
        CL_Obj sym_obj = cl_car(symbols_list);
        CL_Obj val;
        CL_Obj old_tlv;

        if (!CL_SYMBOL_P(sym_obj))
            cl_error(CL_ERR_TYPE,
                     "PROGV: expected symbol, got non-symbol");

        val = !CL_NULL_P(values_list) ? cl_car(values_list) : CL_UNBOUND;

        if (cl_dyn_top >= CL_MAX_DYN_BINDINGS)
            cl_error(CL_ERR_OVERFLOW, "Dynamic binding stack overflow");

        old_tlv = cl_tlv_get(thr, sym_obj);
        cl_dyn_stack[cl_dyn_top].symbol = sym_obj;
        cl_dyn_stack[cl_dyn_top].old_value = old_tlv;
        cl_dyn_top++;
        cl_tlv_set(thr, sym_obj, val);
        if (sym_obj == SYM_STAR_PACKAGE)
            touched_package = 1;

        symbols_list = cl_cdr(symbols_list);
        if (!CL_NULL_P(values_list))
            values_list = cl_cdr(values_list);
    }
    /* Mirrors OP_PROGV_BIND's *PACKAGE* sync. */
    if (touched_package)
        cl_sync_current_package_from_dynamic();
    return CL_MAKE_FIXNUM(saved_mark);
}

/* Backing for OP_PROGV_UNBIND.  Untag the saved dyn_top mark and
 * restore via cl_dynbind_restore_to (which also handles per-symbol
 * TLV write-back and the *PACKAGE* sync when a *PACKAGE* binding
 * unwinds).  Returns `result` so the walker can leave the body
 * result as the new operand-stack TOS without a separate push.
 * Non-allocating; same shape as cl_jit_runtime_dynunbind. */
CL_Obj cl_jit_runtime_progv_unbind(CL_Obj mark_obj, CL_Obj result)
{
    cl_dynbind_restore_to(CL_FIXNUM_VAL(mark_obj));
    return result;
}

/* Backing for OP_APPLY.  Flatten `arglist` into a stack-local buffer, resolve
 * a SYMBOL func through its function cell, then delegate to cl_vm_apply which
 * handles builtins, closures, and JIT-compiled callees uniformly.  The VM's
 * OP_APPLY inlines the dispatch to avoid C-stack growth on deep apply chains;
 * the JIT can't replicate that without an inline dispatch loop, so accept the
 * extra cl_vm_apply call cost — apply chains aren't a hot path in JIT'd code,
 * and going through cl_vm_apply keeps the implementation small and correct.
 * Builtins and bytecode/closure callees are both bounded to 255 by
 * cl_vm_apply's stub frame (it errors clearly past that).  We keep the flatten
 * buffer small and on the C stack — a CALL-ARGUMENTS-LIMIT-sized array here
 * would blow the Amiga 64K stack.  Code needing > 255 args runs through the
 * VM's inline OP_APPLY (cl_vm_run), which spreads onto the GC-rooted VM stack.
 * Allocates (cl_vm_apply may cons frames, invoke user code), so the walker
 * cache_flushes before the JSR. */
#define CL_JIT_APPLY_MAX 255
CL_Obj cl_jit_runtime_apply(CL_Obj func, CL_Obj arglist)
{
    CL_Obj args[CL_JIT_APPLY_MAX];
    int nargs = 0;

    while (!CL_NULL_P(arglist)) {
        if (nargs >= CL_JIT_APPLY_MAX)
            cl_error(CL_ERR_ARGS,
                     "APPLY: too many arguments (got > %d via the JIT apply "
                     "trampoline)", CL_JIT_APPLY_MAX);
        args[nargs++] = cl_car(arglist);
        arglist = cl_cdr(arglist);
    }

    if (CL_SYMBOL_P(func)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(func);
        CL_Obj fval = s->function;
        if (CL_NULL_P(fval) || fval == CL_UNBOUND)
            cl_error_cell(CL_ERR_UNDEFINED, func,
                          "APPLY: undefined function: %s",
                          cl_symbol_name(func));
        func = fval;
    }

    return cl_vm_apply(func, args, nargs);
}

/* Backing for OP_STRUCT_REF: read slot at `idx` from `obj`.  Mirrors
 * the VM's OP_STRUCT_REF exactly: validate STRUCTURE type then check
 * the index is in range, both signaling with the same messages.
 * Non-allocating — always GC-safe. */
CL_Obj cl_jit_runtime_struct_ref(CL_Obj obj, uint32_t idx)
{
    CL_Struct *st;
    if (!CL_STRUCT_P(obj))
        cl_signal_type_error(obj, "STRUCTURE", "%STRUCT-REF");
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    if (idx >= st->n_slots)
        cl_error(CL_ERR_ARGS,
                 "%%STRUCT-REF: index %u out of range (n_slots=%u)",
                 (unsigned)idx, (unsigned)st->n_slots);
    return st->slots[idx];
}

/* Backing for OP_STRUCT_SET.  Same shape as the VM: type-check,
 * bounds-check, write, and return the stored value (matching CL's
 * `setf` semantics where the assignment expression's value is the
 * new value).  Non-allocating — always GC-safe. */
CL_Obj cl_jit_runtime_struct_set(CL_Obj obj, uint32_t idx, CL_Obj val)
{
    CL_Struct *st;
    if (!CL_STRUCT_P(obj))
        cl_signal_type_error(obj, "STRUCTURE", "%STRUCT-SET");
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    if (idx >= st->n_slots)
        cl_error(CL_ERR_ARGS,
                 "%%STRUCT-SET: index %u out of range (n_slots=%u)",
                 (unsigned)idx, (unsigned)st->n_slots);
#ifndef JIT_M68K
    /* The VM's publication barrier (vm.c, OP_STRUCT_SET): on a weakly
     * ordered multi-core host the initializing stores of the object VAL
     * points to must be visible before the slot that publishes it. */
    if (CL_MT()) platform_memory_barrier();
#endif
    st->slots[idx] = val;
    return val;
}

/* Backing for OP_CONS: thin pass-through to cl_cons.  The bytecode
 * VM's OP_CONS is exactly two pops + cl_cons + push, so reproducing
 * that here keeps semantics identical.  Allocates one CL_Cons; the
 * conservative scan reaches the JIT'd caller's cached operand-stack
 * values across this call (see file banner). */
CL_Obj cl_jit_runtime_cons(CL_Obj car, CL_Obj cdr)
{
    return cl_cons(car, cdr);
}

/* Backing for OP_LIST: build a freshly-consed list from `n` operand-
 * stack values.  `operand_top[0]` is the TOS (last pushed), so it lands
 * at the tail of the list; `operand_top[n-1]` is the bottom (first
 * pushed), so it lands at the head.  Mirrors the VM's OP_LIST loop
 * (pop n times, cons each onto the accumulator).
 *
 * GC: each cl_cons may allocate and compact.  Two safety nets:
 *
 *   (a) `list` is a C local — CL_GC_PROTECT keeps it tracked across
 *       allocations so the partially-built list head isn't swept.
 *   (b) `operand_top` points at the JIT'd caller's flushed operand
 *       stack on the m68k stack.  The conservative scan reaches it
 *       (offset-validated) and the compactor's forwarding pass
 *       (gc_forward_jit_native_stack) rewrites those slots in place
 *       — so `operand_top[i]` stays valid across cl_cons even when a
 *       collection moves the referenced object. */
CL_Obj cl_jit_runtime_list(uint32_t n, CL_Obj *operand_top)
{
    CL_Obj list = CL_NIL;
    uint32_t i;
    CL_GC_PROTECT(list);
    for (i = 0; i < n; i++) {
        list = cl_cons(operand_top[i], list);
    }
    CL_GC_UNPROTECT(1);
    return list;
}

/* Backing for OP_RPLACA.  Type-check `cons_obj` (signal like the VM),
 * write `new_car`, return `new_car` so the JIT emitter pushes it as
 * the new TOS.  Non-allocating, so always GC-safe. */
CL_Obj cl_jit_runtime_rplaca(CL_Obj cons_obj, CL_Obj new_car)
{
    CL_Cons *cell;
    if (!CL_CONS_P(cons_obj))
        cl_error(CL_ERR_TYPE, "RPLACA: not a cons");
    cell = (CL_Cons *)CL_OBJ_TO_PTR(cons_obj);
    cell->car = new_car;
    cl_mv_count = 1;
    return new_car;
}

/* Mirror of cl_jit_runtime_rplaca for the cdr slot.  See vm.c::OP_RPLACD. */
CL_Obj cl_jit_runtime_rplacd(CL_Obj cons_obj, CL_Obj new_cdr)
{
    CL_Cons *cell;
    if (!CL_CONS_P(cons_obj))
        cl_error(CL_ERR_TYPE, "RPLACD: not a cons");
    cell = (CL_Cons *)CL_OBJ_TO_PTR(cons_obj);
    cell->cdr = new_cdr;
    cl_mv_count = 1;
    return new_cdr;
}

/* OP_MV_LOAD.  No mv_count reset (matches vm.c). */
CL_Obj cl_jit_runtime_mv_load(uint32_t index)
{
    return (int)index < cl_mv_count ? cl_mv_values[index] : CL_NIL;
}

/* OP_NTH_VALUE.  Argument order: idx_obj passed first (at 4(a7)
 * after JSR), primary second (at 8(a7)).  The walker emits the C-ABI
 * pushes in that order — see emit_op_nth_value. */
CL_Obj cl_jit_runtime_nth_value(CL_Obj idx_obj, CL_Obj primary)
{
    int idx;
    CL_Obj result;
    if (!CL_FIXNUM_P(idx_obj))
        cl_error(CL_ERR_TYPE, "NTH-VALUE: index must be a number");
    idx = CL_FIXNUM_VAL(idx_obj);
    if (idx == 0)
        result = primary;
    else
        result = (idx > 0 && idx < cl_mv_count) ? cl_mv_values[idx] : CL_NIL;
    cl_mv_count = 1;
    return result;
}

/* OP_ASSERT_TYPE.  Mirrors vm.c byte-for-byte: build a TYPE-ERROR
 * condition with :datum and :expected-type, signal it, and if the
 * handler returns (or no handler is bound) fall through to a
 * formatted cl_error so the user sees the offending value and the
 * expected type.  Allocating — caller must cache-flush before JSR. */
void cl_jit_runtime_assert_type(CL_Obj val, CL_Obj type_spec)
{
    CL_Obj slots = CL_NIL;
    CL_Obj cond;
    CL_Obj pair;
    char buf[128];
    char tbuf[64];
    int typep_result;
    /* val/type_spec are unprotected params, unlike vm.c's OP_ASSERT_TYPE
     * (which re-reads val/type_spec from the rooted VM stack/constants pool
     * after any allocating call). cl_typep itself can invoke cl_vm_apply
     * for SATISFIES/deftype specs and compact, so both must be protected
     * before calling it, not just before the conses that follow. */
    CL_GC_PROTECT(val);
    CL_GC_PROTECT(type_spec);
    typep_result = cl_typep(val, type_spec);
    CL_GC_UNPROTECT(2);
    if (typep_result) return;
    CL_GC_PROTECT(val);
    CL_GC_PROTECT(type_spec);
    CL_GC_PROTECT(slots);
    /* Build each pair THEN prepend — never nest as cl_cons(cl_cons(k,v), slots):
     * C's unspecified argument evaluation order (right-to-left on GCC/x86-64)
     * reads the outer `slots` operand before the inner cl_cons compacts, baking a
     * stale offset into the cdr and producing a cyclic slots alist that hangs the
     * slot readers.  Mirrors vm.c OP_ASSERT_TYPE / apply_condition_slot_initforms. */
    pair = cl_cons(KW_EXPECTED_TYPE, type_spec);
    slots = cl_cons(pair, slots);
    pair = cl_cons(KW_DATUM, val);
    slots = cl_cons(pair, slots);
    CL_GC_UNPROTECT(3);
    cond = cl_make_condition(SYM_TYPE_ERROR, slots, CL_NIL);
    cl_signal_condition(cond);
    cl_prin1_to_string(val, buf, sizeof(buf));
    cl_prin1_to_string(type_spec, tbuf, sizeof(tbuf));
    cl_error(CL_ERR_TYPE, "THE: value %s is not of type %s", buf, tbuf);
}

/* Backing for OP_ASET.  Mirrors the VM's OP_ASET dispatch byte-for-
 * byte: bit-vector, simple-string, and general-vector are all valid
 * destinations; the value type-check is destination-dependent
 * (FIXNUM 0/1 for bit-vector, CHARACTER for string, any object for
 * vector).  Returns `val` (CLHS 4.7 setf semantics — the assigned
 * value is the result of the form).  Non-allocating; cl_error
 * longjmps on type/bounds violations the same way the VM does. */
CL_Obj cl_jit_runtime_aset(CL_Obj vec_obj, CL_Obj idx_obj, CL_Obj val)
{
    int32_t idx;
    if (!CL_FIXNUM_P(idx_obj))
        cl_error(CL_ERR_TYPE, "ASET: index must be a number");
    idx = CL_FIXNUM_VAL(idx_obj);
    if (CL_BIT_VECTOR_P(vec_obj)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(vec_obj);
        int32_t v;
        if (idx < 0 || (uint32_t)idx >= cl_bv_active_length(bv))
            cl_error(CL_ERR_ARGS, "ASET: index %d out of range", (int)idx);
        if (!CL_FIXNUM_P(val))
            cl_error(CL_ERR_TYPE, "ASET: value must be 0 or 1 for bit vector");
        v = CL_FIXNUM_VAL(val);
        if (v != 0 && v != 1)
            cl_error(CL_ERR_TYPE, "ASET: value must be 0 or 1 for bit vector");
        cl_bv_set_bit(bv, (uint32_t)idx, v);
    } else if (CL_ANY_STRING_P(vec_obj)) {
        uint32_t slen = cl_string_length(vec_obj);
        if (idx < 0 || (uint32_t)idx >= slen)
            cl_error(CL_ERR_ARGS, "ASET: index %d out of range (0-%lu)",
                     (int)idx, (unsigned long)(slen - 1));
        if (!CL_CHAR_P(val))
            cl_error(CL_ERR_TYPE, "ASET: value must be a character for string");
        cl_string_set_char_at(vec_obj, (uint32_t)idx, CL_CHAR_VAL(val));
    } else if (CL_BYTE_VECTOR_P(vec_obj)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(vec_obj);
        if (idx < 0 || (uint32_t)idx >= bv->length)
            cl_error(CL_ERR_ARGS, "ASET: index %d out of range", (int)idx);
        cl_bytevec_set(bv, idx,
                       cl_bytevec_check_value(val, bv->is_signed,
                                              bv->elt_shift,
                                              "ASET on a byte vector"));
    } else if (CL_VECTOR_P(vec_obj)) {
        CL_Vector *vec = (CL_Vector *)CL_OBJ_TO_PTR(vec_obj);
        if (idx < 0 || (uint32_t)idx >= vec->length)
            cl_error(CL_ERR_ARGS, "ASET: index %d out of range (0-%lu)",
                     (int)idx, (unsigned long)(vec->length - 1));
        cl_vector_data(vec)[idx] = val;
    } else {
        cl_error(CL_ERR_TYPE, "ASET: not a vector");
    }
    return val;
}

/* Backings for OP_MAKE_CELL / OP_CELL_REF / OP_CELL_SET_LOCAL.  Match
 * the VM dispatch byte-for-byte.  cl_make_cell already CL_GC_PROTECTs
 * `val` across its allocation, so the JIT side only has to flush the
 * cache before the JSR (so any cached operand-stack values land where
 * the conservative scan can see them). */
CL_Obj cl_jit_runtime_make_cell(CL_Obj val)
{
    return cl_make_cell(val);
}

CL_Obj cl_jit_runtime_cell_ref(CL_Obj cell_obj)
{
    CL_Cell *cell = (CL_Cell *)CL_OBJ_TO_PTR(cell_obj);
    return cell->value;
}

/* --- String-scan fast path helpers (runtime.h) --- */

CL_Obj cl_jit_runtime_aref(CL_Obj vec_obj, CL_Obj idx_obj, uint32_t kind)
{
    return cl_vector_ref1(vec_obj, idx_obj, (int)kind);
}

CL_Obj cl_jit_runtime_chareq(CL_Obj a, CL_Obj b)
{
    if (!CL_CHAR_P(a) || !CL_CHAR_P(b))
        cl_error(CL_ERR_TYPE, "CHAR=: not a character");
    return a == b ? CL_T : CL_NIL;
}

CL_Obj cl_jit_runtime_cmp_kind(CL_Obj a, CL_Obj b, uint32_t cmp)
{
    return cl_vm_compare_kind(a, b, (int)cmp) ? CL_T : CL_NIL;
}

CL_Obj cl_jit_runtime_push_local(CL_Obj item, CL_Obj *slot)
{
    /* cl_cons roots both by-value arguments; the slot itself is a word of
     * the JIT'd frame, inside the conservatively scanned window. */
    CL_Obj cell = cl_cons(item, *slot);
    *slot = cell;
    return cell;
}

CL_Obj cl_jit_runtime_pop_local(CL_Obj *slot)
{
    CL_Obj list = *slot;
    CL_Obj car = cl_car(list);     /* signals on a non-list, like the macro's CAR */
    *slot = cl_cdr(list);
    return car;
}

CL_Obj cl_jit_runtime_cell_set(CL_Obj cell_obj, CL_Obj val)
{
    CL_Cell *cell = (CL_Cell *)CL_OBJ_TO_PTR(cell_obj);
    cell->value = val;
    return val;
}

/* OP_CLOSURE backing.  Allocates a CL_Closure sized to hold n_upvals
 * upvalue slots, populates bytecode + upvalues, returns the tagged obj.
 * Walker has already filtered out captures with is_local=0 (would
 * require parent-closure upvalues, impossible under the current n_upvalues==0
 * gate) and computed values[] from the parent's frame slots.
 *
 * GC: cl_alloc may compact.  Both `tmpl` (a CL_Bytecode pointer) and the
 * incoming values are reachable via the caller's flushed operand-stack
 * frame on the m68k stack — the caller stages values right above A7 so
 * the conservative scan finds them, and we receive `tmpl` as a raw C
 * pointer baked from the constants pool.  The tmpl pointer itself is
 * stable across GC because cl_alloc relocates the data but the
 * walker's call site re-derives `tmpl` from `constants[idx]` per call,
 * so even after compaction the next dispatch picks up the new address.
 * Inside this helper we don't dereference `tmpl` until after the
 * allocation has consumed any pre-existing slack — but we DO need
 * tmpl_bc->n_upvalues for the size, which we read BEFORE allocation.
 * That's the same access pattern the VM uses (see OP_CLOSURE in vm.c). */
CL_Obj cl_jit_runtime_make_closure(CL_Obj tmpl_obj, uint32_t n_upvals,
                                   CL_Obj *values)
{
    CL_Closure *cl;
    uint32_t i;

    cl = (CL_Closure *)cl_alloc(TYPE_CLOSURE,
        sizeof(CL_Closure) + n_upvals * sizeof(CL_Obj));
    if (!cl) return CL_NIL;
    cl->bytecode = tmpl_obj;
    for (i = 0; i < n_upvals; i++) {
        cl->upvalues[i] = values[i];
    }
    return CL_PTR_TO_OBJ(cl);
}

/* OP_UPVAL backing.  Reads func_obj's closure slot `index` if it's
 * a closure; CL_NIL otherwise (same fallback the VM uses — see
 * core/vm.c OP_UPVAL).  Non-allocating, so the JIT side doesn't
 * cache_flush before the JSR.  Index is u8 in the bytecode and
 * promoted to uint32_t at the C boundary. */
CL_Obj cl_jit_runtime_upval_ref(CL_Obj func_obj, uint32_t index)
{
    CL_Closure *cl;
    if (!CL_CLOSURE_P(func_obj)) return CL_NIL;
    cl = (CL_Closure *)CL_OBJ_TO_PTR(func_obj);
    return cl->upvalues[index];
}

/* OP_CELL_SET_UPVAL backing.  Reads the cell at func_obj's upvalue
 * slot `index` and writes `val` into it.  Returns `val` (matches
 * setf-style semantics, though the walker discards the result —
 * OP_CELL_SET_UPVAL is peek-only on the operand stack).  Non-
 * closure func_obj is a no-op, mirroring the VM's else-fall-through
 * for the "shouldn't happen" path. */
CL_Obj cl_jit_runtime_cell_set_upval(CL_Obj func_obj, uint32_t index,
                                     CL_Obj val)
{
    CL_Closure *cl;
    CL_Obj cell_obj;
    CL_Cell *cell;
    if (!CL_CLOSURE_P(func_obj)) return val;
    cl = (CL_Closure *)CL_OBJ_TO_PTR(func_obj);
    cell_obj = cl->upvalues[index];
    if (!CL_CELL_P(cell_obj)) {
        cl_error(CL_ERR_TYPE,
                 "OP_CELL_SET_UPVAL: upvalue[%u] is not a cell "
                 "(internal compiler error)", (unsigned)index);
    }
    cell = (CL_Cell *)CL_OBJ_TO_PTR(cell_obj);
    cell->value = val;
    return val;
}

/* OP_TAILCALL self-TCO guard.  Called from the walker-emitted
 * arity-matching tail-call site to decide whether the runtime func
 * value would dispatch back into this same bytecode — in which case
 * the emitter's bra-back-to-entry path is safe.  Three cases that
 * count as "self":
 *   - func IS self_bc directly (rare in practice: defun always
 *     wraps via OP_CLOSURE, but local function bindings may store
 *     bare bytecodes);
 *   - func is a CL_Closure whose `bytecode` field is self_bc — the
 *     dominant case for top-level defuns;
 *   - anything else (builtin, foreign, non-heap, different bytecode,
 *     redefined symbol pointing elsewhere): 0 → walker falls back to
 *     cl_jit_runtime_call, semantics match the bytecode VM.
 *
 * Non-allocating; safe under any GC state.  Returns plain int so
 * the m68k caller can `tst.l d0; beq fallback` after the JSR. */
int cl_jit_runtime_is_self_tco(CL_Obj func, CL_Obj self_bc)
{
    if (func == self_bc) return 1;
    if (!CL_HEAP_P(func)) return 0;
    if (func >= cl_heap.arena_size) return 0;
    {
        void *p = CL_OBJ_TO_PTR(func);
        if (CL_HDR_TYPE(p) == TYPE_CLOSURE) {
            CL_Closure *c = (CL_Closure *)p;
            return (c->bytecode == self_bc) ? 1 : 0;
        }
    }
    return 0;
}

void cl_jit_runtime_loop_poll(void)
{
    CL_Thread *thr = cl_get_current_thread();
    thr->jit_loop_ctr = (uint16_t)(CL_JIT_LOOP_POLL_EVERY - 1);
    if (thr->gc_requested) cl_gc_safepoint();
    if (thr->interrupt_pending) cl_thread_handle_interrupt(thr);
    cl_vm_poll_break();
}

/* Backing for OP_MV_RESET.  Bytecode VM does `cl_mv_count = 1` (= a
 * single store into the current thread's CL_Thread.mv_count field).
 * The walker doesn't have CT cached in an A-register and the broader
 * "reset on every value-producing opcode" approach previously broke
 * CLOS (see specs/native-backend.md + the jit-mv-count memory), so
 * we route only the *explicit* OP_MV_RESET — the one the compiler
 * emits between (and …)/(or …) arms — through this helper.  Matches
 * bytecode-VM semantics exactly without re-opening the broader
 * question.  Non-allocating; cache regs stay valid across the JSR. */
void cl_jit_runtime_mv_reset(void)
{
    cl_mv_count = 1;
}

#endif /* JIT_M68K || JIT_A64 */
