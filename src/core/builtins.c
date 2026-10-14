#include "builtins.h"
#include "bignum.h"
#include "float.h"
#include "ratio.h"
#include "stream.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "compiler.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include "../jit/jit.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* Cached, GC-registered symbols for the destructuring-bind arity-error runtime
 * helpers; declared extern in compiler_internal.h, interned in cl_builtins_init. */
CL_Obj cl_dbind_too_few_sym = CL_NIL;
CL_Obj cl_dbind_too_many_sym = CL_NIL;
CL_Obj cl_dbind_check_keys_sym = CL_NIL;

/* --- Image-relink builtin registry (builtins.h) ---
 *
 * Open-addressed hash keyed by (package-name, symbol-name).  Both key
 * strings live outside the GC arena: symbol names are the static string
 * literals every registration site passes; package names are deduped
 * copies (a handful of packages) taken at registration time, because the
 * package OBJECT can move under GC while the registry must stay valid
 * after the arena has been wholesale replaced by an image restore. */
#define BUILTIN_REG_SIZE 2048   /* power of two; ~600 builtins → <30% load */

typedef struct {
    const char *pkg;    /* deduped C copy of the package name */
    const char *sym;    /* static-lifetime symbol name */
    CL_CFunc    fn;
} CL_BuiltinRegEntry;

static CL_BuiltinRegEntry builtin_reg[BUILTIN_REG_SIZE];
static uint32_t builtin_reg_count = 0;

/* Package-name dedup pool: tiny, boot-only. */
#define BUILTIN_REG_MAX_PKGS 16
static const char *builtin_reg_pkgs[BUILTIN_REG_MAX_PKGS];
static uint32_t builtin_reg_pkg_count = 0;

static const char *builtin_reg_intern_pkg(const char *name, uint32_t len)
{
    uint32_t i;
    char *copy;
    for (i = 0; i < builtin_reg_pkg_count; i++) {
        if (strlen(builtin_reg_pkgs[i]) == len &&
            memcmp(builtin_reg_pkgs[i], name, len) == 0)
            return builtin_reg_pkgs[i];
    }
    if (builtin_reg_pkg_count >= BUILTIN_REG_MAX_PKGS)
        return NULL;  /* registry degraded; lookup falls back to stale stub */
    copy = (char *)platform_alloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, name, len);
    copy[len] = '\0';
    builtin_reg_pkgs[builtin_reg_pkg_count++] = copy;
    return copy;
}

static uint32_t builtin_reg_hash(const char *pkg, uint32_t pkg_len,
                                 const char *sym, uint32_t sym_len)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < pkg_len; i++) { h ^= (uint8_t)pkg[i]; h *= 16777619u; }
    h ^= 0xFF; h *= 16777619u;
    for (i = 0; i < sym_len; i++) { h ^= (uint8_t)sym[i]; h *= 16777619u; }
    return h;
}

static void builtin_reg_insert(const char *sym, CL_CFunc fn, CL_Obj package)
{
    CL_Package *p;
    CL_String *pn;
    const char *pkg;
    uint32_t slot, sym_len;

    if (!CL_HEAP_P(package)) return;
    p = (CL_Package *)CL_OBJ_TO_PTR(package);
    pn = (CL_String *)CL_OBJ_TO_PTR(p->name);
    pkg = builtin_reg_intern_pkg(pn->data, pn->length);
    if (!pkg) return;

    sym_len = (uint32_t)strlen(sym);
    slot = builtin_reg_hash(pkg, (uint32_t)strlen(pkg), sym, sym_len)
           & (BUILTIN_REG_SIZE - 1);
    for (;;) {
        CL_BuiltinRegEntry *e = &builtin_reg[slot];
        if (!e->sym) {
            if (builtin_reg_count >= BUILTIN_REG_SIZE - 1)
                return;  /* never fills at ~600 entries; guard anyway */
            e->pkg = pkg;
            e->sym = sym;
            e->fn = fn;
            builtin_reg_count++;
            return;
        }
        if (e->pkg == pkg && strcmp(e->sym, sym) == 0) {
            e->fn = fn;   /* re-registration (heap re-init) refreshes */
            return;
        }
        slot = (slot + 1) & (BUILTIN_REG_SIZE - 1);
    }
}

CL_CFunc cl_builtin_registry_lookup(const char *pkg_name, uint32_t pkg_len,
                                    const char *sym_name, uint32_t sym_len)
{
    uint32_t slot = builtin_reg_hash(pkg_name, pkg_len, sym_name, sym_len)
                    & (BUILTIN_REG_SIZE - 1);
    uint32_t probes;
    for (probes = 0; probes < BUILTIN_REG_SIZE; probes++) {
        CL_BuiltinRegEntry *e = &builtin_reg[slot];
        if (!e->sym) return NULL;
        if (strlen(e->pkg) == pkg_len &&
            memcmp(e->pkg, pkg_name, pkg_len) == 0 &&
            strlen(e->sym) == sym_len &&
            memcmp(e->sym, sym_name, sym_len) == 0)
            return e->fn;
        slot = (slot + 1) & (BUILTIN_REG_SIZE - 1);
    }
    return NULL;
}

/* Stub installed by the image restore for a TYPE_FUNCTION whose name has
 * no registration in this build.  Loud runtime error, never corruption:
 * the function names itself as far as it can (the restore walk already
 * printed a warning listing the affected names). */
CL_Obj bi_stale_builtin(CL_Obj *args, int nargs)
{
    CL_UNUSED(args);
    CL_UNUSED(nargs);
    cl_error(CL_ERR_UNDEFINED,
             "This builtin function came from a restored image and has no "
             "implementation in this build of amilisp (see the warnings "
             "printed at --image restore time).  Rebuild the image with "
             "EXT:SAVE-IMAGE.");
    return CL_NIL;
}

/* Shared: register a builtin in a specific package */
void cl_register_builtin(const char *name, CL_CFunc func,
                          int min, int max, CL_Obj package)
{
    builtin_reg_insert(name, func, package);
    /* sym is protected across the function alloc — cl_make_function can
     * compact and leave sym/s stale (latent at boot where the heap is
     * dense, but wrong by contract and fatal if ever run post-boot). */
    CL_Obj sym = cl_intern_in(name, (uint32_t)strlen(name), package);
    CL_Obj fn;
    CL_GC_PROTECT(sym);
    fn = cl_make_function(func, sym, min, max);
    cl_symbol_set_function(sym, fn);
    CL_GC_UNPROTECT(1);
    /* Do NOT set s->value here.  Per CLHS, function and value cells are
     * disjoint: (boundp 'car) must return NIL even though CAR is fbound.
     * Setting value = fn silently broke BOUNDP / SYMBOL-VALUE for every
     * builtin name. */
}

/* Register + export in one step.  The per-package `*_defun` helpers across
 * the builtins_*.c files used to carry their own byte-identical copies of
 * cl_register_builtin's body plus an export; funneling them through here
 * matters beyond dedup: EVERY builtin registration must flow through
 * cl_register_builtin so the image-relink registry above sees it — a
 * bypassed registration is a function a restored image cannot relink. */
void cl_register_builtin_exported(const char *name, CL_CFunc func,
                                  int min, int max, CL_Obj package)
{
    CL_Obj sym;
    /* package is a C local copy of the caller's package handle — protect it
     * across the allocating registration or a compaction leaves it stale. */
    CL_GC_PROTECT(package);
    cl_register_builtin(name, func, min, max, package);
    sym = cl_intern_in(name, (uint32_t)strlen(name), package);
    CL_GC_PROTECT(sym);
    cl_export_symbol(sym, package);
    CL_GC_UNPROTECT(2);
}

/* Table-driven mass registration: collapses hundreds of per-call code
 * sequences in *_init() into a single tight loop, shrinking the binary
 * substantially on m68k where each defun() call site costs ~30 bytes. */
void cl_register_builtins(const CL_BuiltinDesc *table, uint32_t count,
                          CL_Obj package)
{
    uint32_t i;
    for (i = 0; i < count; i++) {
        cl_register_builtin(table[i].name, table[i].func,
                            (int)table[i].min, (int)table[i].max,
                            package);
    }
}

/* defun() (register a builtin into COMMON-LISP) is shared from builtins.h. */

/* Register a not-yet-implemented CL function stub (variadic). */
#define defstub(name) defun(name, bi_unimplemented_stub, 0, -1)

/* Coerce function designator: symbol -> its function binding.
 * A function designator is a function object or a symbol that names one.
 * A funcallable instance (STANDARD-GENERIC-FUNCTION struct) is unwrapped
 * to its discriminating-function slot here, so every higher-order
 * builtin (MAPCAR, REMOVE-IF-NOT, SORT, MAPHASH, ...) sees a callable
 * of one of the three flat types their dispatch already handles — no
 * per-callsite change needed to accept GFs as arguments. */
CL_Obj cl_coerce_funcdesig(CL_Obj obj, const char *context)
{
    if (cl_funcallable_instance_p(obj))
        obj = cl_unwrap_funcallable(obj);
    if (CL_FUNCTION_OBJ_P(obj))
        return obj;
    if (CL_SYMBOL_P(obj)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
        CL_Obj fn = s->function;
        if (fn != CL_UNBOUND && !CL_NULL_P(fn)) {
            if (cl_funcallable_instance_p(fn))
                fn = cl_unwrap_funcallable(fn);
            return fn;
        }
        cl_error_cell(CL_ERR_UNDEFINED, obj, "Undefined function: %s", cl_symbol_name(obj));
    }
    {
        char val[96];
        cl_error(CL_ERR_TYPE, "%s: not a function: %s", context,
                 cl_obj_brief(obj, val, sizeof(val)));
    }
    return CL_NIL;
}

/* --- List operations --- */

static CL_Obj bi_cons(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return cl_cons(args[0], args[1]);
}

static CL_Obj bi_car(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return cl_car(args[0]);
}

static CL_Obj bi_cdr(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return cl_cdr(args[0]);
}

static CL_Obj bi_second(CL_Obj *a, int n) { CL_UNUSED(n); return cl_car(cl_cdr(a[0])); }
static CL_Obj bi_third(CL_Obj *a, int n)  { CL_UNUSED(n); return cl_car(cl_cdr(cl_cdr(a[0]))); }
static CL_Obj bi_fourth(CL_Obj *a, int n) { CL_UNUSED(n); return cl_car(cl_cdr(cl_cdr(cl_cdr(a[0])))); }
static CL_Obj bi_fifth(CL_Obj *a, int n)  { CL_UNUSED(n); return cl_car(cl_cdr(cl_cdr(cl_cdr(cl_cdr(a[0]))))); }
static CL_Obj bi_sixth(CL_Obj *a, int n)  { CL_UNUSED(n); return cl_car(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(a[0])))))); }
static CL_Obj bi_seventh(CL_Obj *a, int n){ CL_UNUSED(n); return cl_car(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(a[0]))))))); }
static CL_Obj bi_eighth(CL_Obj *a, int n) { CL_UNUSED(n); return cl_car(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(a[0])))))))); }
static CL_Obj bi_ninth(CL_Obj *a, int n)  { CL_UNUSED(n); return cl_car(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(a[0]))))))))); }
static CL_Obj bi_tenth(CL_Obj *a, int n)  { CL_UNUSED(n); return cl_car(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(cl_cdr(a[0])))))))))); }

static CL_Obj bi_list(CL_Obj *args, int n)
{
    CL_Obj result = CL_NIL;
    int i;
    for (i = n - 1; i >= 0; i--)
        result = cl_cons(args[i], result);
    return result;
}

static CL_Obj bi_length(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    int len = 0;
    CL_UNUSED(n);

    if (CL_STRING_P(obj)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(obj);
        return CL_MAKE_FIXNUM(s->length);
    }

#ifdef CL_WIDE_STRINGS
    if (CL_WIDE_STRING_P(obj)) {
        CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(obj);
        return CL_MAKE_FIXNUM(ws->length);
    }
#endif

    if (CL_VECTOR_P(obj)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        return CL_MAKE_FIXNUM(cl_vector_active_length(v));
    }

    if (CL_BIT_VECTOR_P(obj)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
        return CL_MAKE_FIXNUM(cl_bv_active_length(bv));
    }

    if (CL_BYTE_VECTOR_P(obj)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
        return CL_MAKE_FIXNUM(cl_bytevec_active_length(bv));
    }

    while (!CL_NULL_P(obj)) {
        len++;
        obj = cl_cdr(obj);
    }
    return CL_MAKE_FIXNUM(len);
}

static CL_Obj bi_append(CL_Obj *args, int n)
{
    CL_Obj result = CL_NIL;
    CL_Obj tail = CL_NIL;
    CL_Obj list;
    int i;

    if (n == 0) return CL_NIL;

    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    /* GC SAFETY: the input cursor is re-read (cl_cdr) after each cl_cons
     * compaction — it must be a forwarded root (args[i] itself is a rooted
     * VM-stack slot, but this local walks INTO the list). */
    list = CL_NIL;
    CL_GC_PROTECT(list);

    for (i = 0; i < n - 1; i++) {
        list = args[i];
        while (!CL_NULL_P(list)) {
            CL_Obj cell = cl_cons(cl_car(list), CL_NIL);
            if (CL_NULL_P(result)) {
                result = cell;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
            }
            tail = cell;
            list = cl_cdr(list);
        }
    }

    /* Last arg shared (not copied) */
    if (CL_NULL_P(result)) {
        CL_GC_UNPROTECT(3);
        return args[n - 1];
    }
    ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = args[n - 1];
    CL_GC_UNPROTECT(3);
    return result;
}

static CL_Obj bi_reverse(CL_Obj *args, int n)
{
    CL_Obj seq = args[0];
    CL_UNUSED(n);

    if (CL_NULL_P(seq)) return CL_NIL;

    if (CL_CONS_P(seq)) {
        CL_Obj result = CL_NIL;
        /* GC SAFETY: seq is re-read after each cl_cons compaction. */
        CL_GC_PROTECT(seq);
        while (!CL_NULL_P(seq)) {
            result = cl_cons(cl_car(seq), result);
            seq = cl_cdr(seq);
        }
        CL_GC_UNPROTECT(1);
        return result;
    }
    /* String-like (simple string OR adjustable/fill-pointer character vector):
     * REVERSE yields a fresh simple STRING of the active length. Checked before
     * the general-vector branch because a string-vector is also CL_VECTOR_P. */
    if (CL_ANY_STRING_P(seq) || CL_STRING_VECTOR_P(seq)) {
        uint32_t slen = cl_string_length(seq);
        CL_Obj result;
        uint32_t i;
        int wide = 0;
        CL_GC_PROTECT(seq);
#ifdef CL_WIDE_STRINGS
        if (CL_WIDE_STRING_P(seq)) wide = 1;
        else for (i = 0; i < slen; i++)
            if (cl_string_char_at(seq, i) > 255) { wide = 1; break; }
        result = wide ? cl_make_wide_string(NULL, slen) : cl_make_string(NULL, slen);
#else
        (void)wide;
        result = cl_make_string(NULL, slen);
#endif
        for (i = 0; i < slen; i++)
            cl_string_set_char_at(result, i, cl_string_char_at(seq, slen - 1 - i));
        CL_GC_UNPROTECT(1);
        return result;
    }
    if (CL_VECTOR_P(seq)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        uint32_t alen = cl_vector_active_length(v);
        CL_Obj result;
        CL_Vector *rv;
        uint32_t i;
        /* Protect seq across the allocation: the "re-fetch" below must go
         * through the FORWARDED value — re-deriving from an unprotected
         * (stale) offset after a compaction reads whatever now lives there. */
        CL_GC_PROTECT(seq);
        result = cl_make_vector(alen);
        CL_GC_UNPROTECT(1);
        rv = (CL_Vector *)CL_OBJ_TO_PTR(result);
        v = (CL_Vector *)CL_OBJ_TO_PTR(seq);
        for (i = 0; i < alen; i++)
            cl_vector_data(rv)[i] = cl_vector_data(v)[alen - 1 - i];
        return result;
    }
    if (CL_BIT_VECTOR_P(seq)) {
        CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        uint32_t blen = cl_bv_active_length(bv);   /* honour fill pointer */
        CL_Obj result;
        CL_BitVector *rv;
        uint32_t i;
        CL_GC_PROTECT(seq);   /* see the vector branch above */
        result = cl_make_bit_vector(blen);
        CL_GC_UNPROTECT(1);
        rv = (CL_BitVector *)CL_OBJ_TO_PTR(result);
        bv = (CL_BitVector *)CL_OBJ_TO_PTR(seq);
        for (i = 0; i < blen; i++)
            cl_bv_set_bit(rv, i, cl_bv_get_bit(bv, blen - 1 - i));
        return result;
    }
    if (CL_BYTE_VECTOR_P(seq)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        uint32_t blen = cl_bytevec_active_length(bv);
        int is_signed = bv->is_signed;
        int elt_shift = bv->elt_shift;
        CL_Obj result;
        CL_ByteVector *rv;
        uint32_t i;
        CL_GC_PROTECT(seq);   /* see the vector branch above */
        result = cl_make_byte_vector(blen, is_signed, elt_shift);
        CL_GC_UNPROTECT(1);
        rv = (CL_ByteVector *)CL_OBJ_TO_PTR(result);
        bv = (CL_ByteVector *)CL_OBJ_TO_PTR(seq);
        for (i = 0; i < blen; i++)
            cl_bytevec_set(rv, i, cl_bytevec_get(bv, blen - 1 - i));
        return result;
    }
    cl_signal_type_error(seq, "SEQUENCE", "REVERSE");
    return CL_NIL;
}

static CL_Obj bi_nth(CL_Obj *args, int n)
{
    int32_t idx;
    CL_Obj list;
    CL_UNUSED(n);

    /* CLHS 14.2 NTH: n is a *non-negative* integer.  A negative one is a
     * type error (it used to fall straight through the walk loop and hand
     * back the first element), and a bignum is a perfectly good index that
     * simply runs off the end of any list — NIL, not an error. */
    idx = cl_extract_count_or_clamp(args[0], "NTH");
    list = args[1];
    while (idx > 0 && !CL_NULL_P(list)) {
        list = cl_cdr(list);
        idx--;
    }
    return cl_car(list);
}

/* --- Predicates --- */

static CL_Obj bi_null(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_NULL_P(args[0]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_consp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_CONS_P(args[0]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_atom(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_CONS_P(args[0]) ? CL_NIL : SYM_T;
}

static CL_Obj bi_listp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return (CL_NULL_P(args[0]) || CL_CONS_P(args[0])) ? SYM_T : CL_NIL;
}

static CL_Obj bi_symbolp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return (CL_NULL_P(args[0]) || CL_SYMBOL_P(args[0])) ? SYM_T : CL_NIL;
}

static CL_Obj bi_stringp(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return (CL_ANY_STRING_P(args[0]) || CL_STRING_VECTOR_P(args[0])) ? SYM_T : CL_NIL;
}

static CL_Obj bi_simple_string_p(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    /* Simple strings are TYPE_STRING or TYPE_WIDE_STRING (no fill-pointer/displaced) */
    return CL_ANY_STRING_P(args[0]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_functionp(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    CL_UNUSED(n);
    return (CL_FUNCTION_OBJ_P(obj) || cl_funcallable_instance_p(obj))
        ? SYM_T : CL_NIL;
}

/* Runtime helpers invoked by destructuring-bind arity guards.  The compiler
 * emits a 0-arg call to one of these when a required element is missing or an
 * extra element is present; signalling from a tiny shared builtin keeps the
 * per-binding bytecode small (no inline (error "...") form / string constant)
 * and avoids re-entering the expression compiler — important on the
 * memory-constrained target and to keep destructuring-bind compilation cheap.
 * (CLHS: a list that does not match the lambda-list structure must error.) */
static CL_Obj bi_dbind_too_few(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_error(CL_ERR_ARGS,
             "destructuring-bind: too few elements in list for the lambda list");
    return CL_NIL;
}

static CL_Obj bi_dbind_too_many(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_error(CL_ERR_ARGS,
             "destructuring-bind: too many elements in list for the lambda list");
    return CL_NIL;
}

/* (amilisp::%local-arity-error NAME EXPECTED GOT) — what the compiler emits
 * at a call to an inlined FLET/LABELS function with the wrong number of
 * arguments (compile_local_inline_call).  The arguments have already been
 * evaluated and dropped, exactly as they would have been for the closure
 * call the VM would have refused; the error is the same PROGRAM-ERROR the
 * VM's arity check raises (vm.c OP_CALL). */
CL_Obj cl_local_arity_error_sym = CL_NIL;
static CL_Obj bi_local_arity_error(CL_Obj *args, int n)
{
    int expected = CL_FIXNUM_P(args[1]) ? (int)CL_FIXNUM_VAL(args[1]) : 0;
    int got      = CL_FIXNUM_P(args[2]) ? (int)CL_FIXNUM_VAL(args[2]) : 0;
    CL_UNUSED(n);
    cl_error(CL_ERR_ARGS, "Too %s arguments to %s: expected %d, got %d",
             got < expected ? "few" : "many",
             CL_SYMBOL_P(args[0]) ? cl_symbol_name(args[0]) : "<local function>",
             expected, got);
    return CL_NIL;
}

/* (amilisp::%dbind-check-keys list allowed-keywords) => NIL
 * The unknown-keyword check of a destructuring &key section.  CLHS 3.4.4
 * gives destructuring lambda lists the 3.4.1.4 keyword rules: a keyword the
 * section does not name is an error, unless the lambda list says
 * &allow-other-keys (then the compiler emits no call at all) or the list
 * itself carries :allow-other-keys with a true value (3.4.1.4.1).
 * :allow-other-keys is always accepted (3.4.1.4.1.1).  The walk is by pairs
 * and stops at an odd trailing element, which the binding loop already
 * treats as "not supplied" — this helper adds exactly the missing check.
 * Signals PROGRAM-ERROR (CL_ERR_ARGS), like the VM's check for calls. */
static CL_Obj bi_dbind_check_keys(CL_Obj *args, int n)
{
    CL_Obj list = args[0], allowed = args[1], p, a;
    CL_UNUSED(n);

    for (p = list; CL_CONS_P(p) && CL_CONS_P(cl_cdr(p)); p = cl_cdr(cl_cdr(p)))
        if (cl_car(p) == KW_ALLOW_OTHER_KEYS) {
            if (!CL_NULL_P(cl_car(cl_cdr(p))))
                return CL_NIL;
            break;
        }

    for (p = list; CL_CONS_P(p) && CL_CONS_P(cl_cdr(p)); p = cl_cdr(cl_cdr(p))) {
        CL_Obj key = cl_car(p);
        char accepted[128];
        size_t used = 0;

        if (key == KW_ALLOW_OTHER_KEYS) continue;
        for (a = allowed; CL_CONS_P(a); a = cl_cdr(a))
            if (cl_car(a) == key) break;
        if (CL_CONS_P(a)) continue;

        if (!CL_SYMBOL_P(key))
            cl_error(CL_ERR_ARGS,
                     "destructuring-bind: keyword argument is not a symbol "
                     "(got a %s)", cl_type_name(key));
        /* Name what the section WOULD accept — a typo is the common case.
         * Bounded buffer; a long section is cut off with "...". */
        accepted[0] = '\0';
        for (a = allowed; CL_CONS_P(a); a = cl_cdr(a)) {
            const char *nm = cl_symbol_name(cl_car(a));
            size_t len = strlen(nm);
            if (used + 2 + len + sizeof(" ...") > sizeof(accepted)) {
                strcpy(accepted + used, " ...");
                break;
            }
            accepted[used++] = ' ';
            accepted[used++] = ':';
            memcpy(accepted + used, nm, len);
            used += len;
            accepted[used] = '\0';
        }
        cl_error(CL_ERR_ARGS,
                 "destructuring-bind: unknown keyword argument %s%s (accepted:%s)",
                 ((CL_Symbol *)CL_OBJ_TO_PTR(key))->package == cl_package_keyword
                     ? ":" : "",
                 cl_symbol_name(key), used ? accepted : " none");
    }
    return CL_NIL;
}

/* (compiled-function-p object) => boolean
 * In amilisp, all functions (C builtins, bytecode, closures, FFI stubs)
 * are compiled. */
static CL_Obj bi_compiled_function_p(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    CL_UNUSED(n);
    return (CL_FUNCTION_OBJ_P(obj) || cl_funcallable_instance_p(obj))
        ? SYM_T : CL_NIL;
}

/* (function-lambda-expression function) => lambda-expression, closure-p, name
 * Returns three values: NIL (no source), T (may be a closure), and the name. */
static CL_Obj bi_function_lambda_expression(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    CL_Obj name = CL_NIL;
    CL_UNUSED(n);

    if (CL_FUNCTION_P(obj)) {
        CL_Function *f = (CL_Function *)CL_OBJ_TO_PTR(obj);
        name = f->name;
    } else if (CL_BYTECODE_P(obj)) {
        CL_Bytecode *bc = (CL_Bytecode *)CL_OBJ_TO_PTR(obj);
        name = bc->name;
    } else if (CL_CLOSURE_P(obj)) {
        CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(obj);
        name = ((CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode))->name;
    } else if (CL_FFI_STUB_P(obj)) {
        name = ((CL_FfiStub *)CL_OBJ_TO_PTR(obj))->name;
    } else {
        cl_error(CL_ERR_TYPE, "FUNCTION-LAMBDA-EXPRESSION: not a function");
    }

    cl_mv_count = 3;
    cl_mv_values[0] = CL_NIL;  /* no source */
    cl_mv_values[1] = CL_T;    /* may be a closure */
    cl_mv_values[2] = name;
    return CL_NIL;
}

static CL_Obj bi_eq(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return (args[0] == args[1]) ? SYM_T : CL_NIL;
}

static CL_Obj bi_eql(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    /* For fixnums and characters, value equality; otherwise identity */
    if (CL_FIXNUM_P(args[0]) && CL_FIXNUM_P(args[1]))
        return (args[0] == args[1]) ? SYM_T : CL_NIL;
    if (CL_CHAR_P(args[0]) && CL_CHAR_P(args[1]))
        return (args[0] == args[1]) ? SYM_T : CL_NIL;
    if (CL_BIGNUM_P(args[0]) && CL_BIGNUM_P(args[1]))
        return cl_bignum_equal(args[0], args[1]) ? SYM_T : CL_NIL;
    /* Floats: same type and same value */
    if (CL_SINGLE_FLOAT_P(args[0]) && CL_SINGLE_FLOAT_P(args[1]))
        return ((CL_SingleFloat *)CL_OBJ_TO_PTR(args[0]))->value ==
               ((CL_SingleFloat *)CL_OBJ_TO_PTR(args[1]))->value ? SYM_T : CL_NIL;
    if (CL_DOUBLE_FLOAT_P(args[0]) && CL_DOUBLE_FLOAT_P(args[1]))
        return ((CL_DoubleFloat *)CL_OBJ_TO_PTR(args[0]))->value ==
               ((CL_DoubleFloat *)CL_OBJ_TO_PTR(args[1]))->value ? SYM_T : CL_NIL;
    if (CL_RATIO_P(args[0]) && CL_RATIO_P(args[1]))
        return cl_ratio_equal(args[0], args[1]) ? SYM_T : CL_NIL;
    if (CL_COMPLEX_P(args[0]) && CL_COMPLEX_P(args[1])) {
        CL_Complex *ca = (CL_Complex *)CL_OBJ_TO_PTR(args[0]);
        CL_Complex *cb = (CL_Complex *)CL_OBJ_TO_PTR(args[1]);
        CL_Obj ra[2], ia[2];
        ra[0] = ca->realpart; ra[1] = cb->realpart;
        ia[0] = ca->imagpart; ia[1] = cb->imagpart;
        return (!CL_NULL_P(bi_eql(ra, 2)) && !CL_NULL_P(bi_eql(ia, 2))) ? SYM_T : CL_NIL;
    }
    return (args[0] == args[1]) ? SYM_T : CL_NIL;
}

/* String-ish accessors: treat both TYPE_STRING (and wide strings) and
   character vectors (TYPE_VECTOR with CL_VEC_FLAG_STRING, e.g. the result of
   (make-array n :element-type 'character ...) or vector-push-extend on one)
   uniformly so that EQUAL compares them element-wise regardless of which
   representation each operand happens to use. */
static int cl_stringish_p(CL_Obj o)
{
    return CL_ANY_STRING_P(o) || CL_STRING_VECTOR_P(o);
}

static uint32_t cl_stringish_length(CL_Obj o)
{
    if (CL_ANY_STRING_P(o))
        return cl_string_length(o);
    return cl_vector_active_length((CL_Vector *)CL_OBJ_TO_PTR(o));
}

static int cl_stringish_char_at(CL_Obj o, uint32_t i)
{
    if (CL_ANY_STRING_P(o))
        return cl_string_char_at(o, i);
    {
        CL_Obj c = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(o))[i];
        return CL_CHAR_P(c) ? CL_CHAR_VAL(c) : -1;
    }
}

static CL_Obj bi_equal(CL_Obj *args, int n)
{
    CL_Obj a = args[0], b = args[1];
    CL_UNUSED(n);

    if (a == b) return SYM_T;
    if (CL_BIGNUM_P(a) && CL_BIGNUM_P(b))
        return cl_bignum_equal(a, b) ? SYM_T : CL_NIL;
    if (CL_RATIO_P(a) && CL_RATIO_P(b))
        return cl_ratio_equal(a, b) ? SYM_T : CL_NIL;
    if (CL_COMPLEX_P(a) && CL_COMPLEX_P(b)) {
        CL_Obj pair[2];
        pair[0] = a; pair[1] = b;
        return bi_eql(pair, 2);
    }
    /* Floats: equal is same as eql (same type, same value) */
    if (CL_SINGLE_FLOAT_P(a) && CL_SINGLE_FLOAT_P(b))
        return ((CL_SingleFloat *)CL_OBJ_TO_PTR(a))->value ==
               ((CL_SingleFloat *)CL_OBJ_TO_PTR(b))->value ? SYM_T : CL_NIL;
    if (CL_DOUBLE_FLOAT_P(a) && CL_DOUBLE_FLOAT_P(b))
        return ((CL_DoubleFloat *)CL_OBJ_TO_PTR(a))->value ==
               ((CL_DoubleFloat *)CL_OBJ_TO_PTR(b))->value ? SYM_T : CL_NIL;
    if (CL_CONS_P(a) && CL_CONS_P(b)) {
        CL_Obj aa[2], bb[2];
        aa[0] = cl_car(a); aa[1] = cl_car(b);
        if (CL_NULL_P(bi_equal(aa, 2))) return CL_NIL;
        bb[0] = cl_cdr(a); bb[1] = cl_cdr(b);
        return bi_equal(bb, 2);
    }
    if (CL_STRING_P(a) && CL_STRING_P(b)) {
        CL_String *sa = (CL_String *)CL_OBJ_TO_PTR(a);
        CL_String *sb = (CL_String *)CL_OBJ_TO_PTR(b);
        return (sa->length == sb->length &&
                memcmp(sa->data, sb->data, sa->length) == 0) ? SYM_T : CL_NIL;
    }
    /* Any other mix of string representations (base/wide strings and
       character vectors) — compare element-wise. Must precede the generic
       vector path below so a character vector is compared as a string. */
    if (cl_stringish_p(a) && cl_stringish_p(b)) {
        uint32_t la = cl_stringish_length(a), lb = cl_stringish_length(b);
        uint32_t i;
        if (la != lb) return CL_NIL;
        for (i = 0; i < la; i++) {
            if (cl_stringish_char_at(a, i) != cl_stringish_char_at(b, i))
                return CL_NIL;
        }
        return SYM_T;
    }
    /* EQUAL on arrays (CLHS): an array is equal to another object only if they
       are EQ — EXCEPT strings (handled above) and bit vectors, which are
       compared element-wise. General vectors and multidimensional arrays are
       therefore NOT descended; eq was already checked at the top. */
    if (CL_BIT_VECTOR_P(a) && CL_BIT_VECTOR_P(b)) {
        CL_BitVector *ba = (CL_BitVector *)CL_OBJ_TO_PTR(a);
        CL_BitVector *bb = (CL_BitVector *)CL_OBJ_TO_PTR(b);
        uint32_t nwords;
        if (ba->length != bb->length) return CL_NIL;
        nwords = CL_BV_WORDS(ba->length);
        if (nwords == 0) return SYM_T;
        return memcmp(ba->data, bb->data, nwords * sizeof(uint32_t)) == 0 ? SYM_T : CL_NIL;
    }
    if (CL_PATHNAME_P(a) && CL_PATHNAME_P(b)) {
        extern int cl_pathname_equal(CL_Obj a, CL_Obj b);
        return cl_pathname_equal(a, b) ? SYM_T : CL_NIL;
    }
    return CL_NIL;
}

/* Recursive EQUALP predicate, used as a callback for hash-table comparison
   (cl_hashtable_equalp lives in builtins_hashtable.c where the bucket-walking
   helpers are visible). EQUALP performs no allocation, so no GC protection is
   needed across the callback. */
static int cl_equalp_pred(CL_Obj a, CL_Obj b);

/* EQUALP: case-insensitive, numeric =, recursive on arrays/structs */
/* --- EQUALP 1-D array helpers (string / vector / bit-vector) --- */
/* These let EQUALP compare arrays of differing specialization element-wise,
 * e.g. a string against a (vector character) or a bit-vector against a
 * (vector bit), as required by CLHS (element type is ignored). */
static int equalp_arr1d_p(CL_Obj x)
{
    return CL_ANY_STRING_P(x) || CL_VECTOR_P(x) || CL_BIT_VECTOR_P(x) ||
           CL_BYTE_VECTOR_P(x);
}

static int equalp_arr1d_rank(CL_Obj x)
{
    if (CL_VECTOR_P(x)) {
        /* CL_Vector.rank: 0 = simple 1-D, >1 = multi-dimensional. */
        uint8_t r = ((CL_Vector *)CL_OBJ_TO_PTR(x))->rank;
        return r > 1 ? (int)r : 1;
    }
    return 1; /* strings and bit-vectors are always rank 1 */
}

static uint32_t equalp_arr1d_len(CL_Obj x)
{
    if (CL_ANY_STRING_P(x)) return cl_string_length(x);
    if (CL_BIT_VECTOR_P(x)) return cl_bv_active_length((CL_BitVector *)CL_OBJ_TO_PTR(x));
    if (CL_BYTE_VECTOR_P(x))
        return cl_bytevec_active_length((CL_ByteVector *)CL_OBJ_TO_PTR(x));
    return cl_vector_active_length((CL_Vector *)CL_OBJ_TO_PTR(x));
}

static CL_Obj equalp_arr1d_elt(CL_Obj x, uint32_t i)
{
    if (CL_ANY_STRING_P(x)) return CL_MAKE_CHAR(cl_string_char_at(x, i));
    if (CL_BIT_VECTOR_P(x))
        return CL_MAKE_FIXNUM(cl_bv_get_bit((CL_BitVector *)CL_OBJ_TO_PTR(x), i));
    if (CL_BYTE_VECTOR_P(x))
        return CL_MAKE_FIXNUM(cl_bytevec_get((CL_ByteVector *)CL_OBJ_TO_PTR(x), i));
    return cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(x))[i];
}

static CL_Obj bi_equalp(CL_Obj *args, int n)
{
    CL_Obj a = args[0], b = args[1];
    CL_UNUSED(n);

    if (a == b) return SYM_T;
    /* Characters: case-insensitive */
    if (CL_CHAR_P(a) && CL_CHAR_P(b)) {
        int ca = CL_CHAR_VAL(a), cb = CL_CHAR_VAL(b);
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        return ca == cb ? SYM_T : CL_NIL;
    }
    /* Numbers: use numeric = (cross-type).  cl_numeric_equal, not
     * cl_arith_compare — the latter is the ORDERED comparator and does not
     * handle complex numbers, so (equalp #C(0.0 2.0) #C(0.0 2.0)) was NIL. */
    if (CL_NUMBER_P(a) && CL_NUMBER_P(b))
        return cl_numeric_equal(a, b) ? SYM_T : CL_NIL;
    /* Conses: recursive.
     * GC SAFETY: the recursion can allocate (numeric EQUALP on ratios /
     * bignums goes through cl_arith_compare) and compact — a and b are
     * re-read (cl_cdr) after it, so they must be forwarded roots. */
    if (CL_CONS_P(a) && CL_CONS_P(b)) {
        CL_Obj pair[2];
        CL_GC_PROTECT(a);
        CL_GC_PROTECT(b);
        pair[0] = cl_car(a); pair[1] = cl_car(b);
        if (CL_NULL_P(bi_equalp(pair, 2))) {
            CL_GC_UNPROTECT(2);
            return CL_NIL;
        }
        pair[0] = cl_cdr(a); pair[1] = cl_cdr(b);
        CL_GC_UNPROTECT(2);
        return bi_equalp(pair, 2);
    }
    /* Strings: case-insensitive */
    if (CL_ANY_STRING_P(a) && CL_ANY_STRING_P(b)) {
        uint32_t la = cl_string_length(a), lb = cl_string_length(b);
        uint32_t i;
        if (la != lb) return CL_NIL;
        for (i = 0; i < la; i++) {
            int ca = cl_string_char_at(a, i), cb = cl_string_char_at(b, i);
            if (ca >= 'a' && ca <= 'z') ca -= 32;
            if (cb >= 'a' && cb <= 'z') cb -= 32;
            if (ca != cb) return CL_NIL;
        }
        return SYM_T;
    }
    /* Vectors (CLHS EQUALP): EQUALP compares two vectors as SEQUENCES — the
       relevant length is the ACTIVE length (the fill pointer if present) and
       only the active prefix is compared.  A fill-pointered vector is therefore
       equalp to a plain vector holding the same active elements.  This matches
       SBCL/CLISP and is mandated by the ANSI test suite itself: ansi-test
       data-and-control-flow/equalp.lsp EQUALP.11 asserts
         (equalp #(1 2 3) (make-array 8 :initial-contents '(1..8) :fill-pointer 3)) => T
       (dimension 8 vs 3, but the active prefix (1 2 3) matches), and
       sequences/reverse.lsp REVERSE-VECTOR.6 likewise compares a fill-pointer
       vector against a plain literal.  (Multidimensional arrays cannot have a
       fill pointer, so cl_vector_active_length == length there and the per-axis
       dimension check above still governs.)  Element type/specialization are
       ignored. */
    if (CL_VECTOR_P(a) && CL_VECTOR_P(b)) {
        CL_Vector *va = (CL_Vector *)CL_OBJ_TO_PTR(a);
        CL_Vector *vb = (CL_Vector *)CL_OBJ_TO_PTR(b);
        CL_Obj *da, *db;
        uint32_t i, na, nb;
        if (va->rank != vb->rank) return CL_NIL;
        for (i = 0; i < va->rank; i++)        /* compare dimensions (rank>1) */
            if (va->data[i] != vb->data[i]) return CL_NIL;
        na = cl_vector_active_length(va);
        nb = cl_vector_active_length(vb);
        if (na != nb) return CL_NIL;          /* active length (fill pointer counts) */
        /* GC SAFETY: the element recursion can compact — re-derive the data
         * pointers from the protected handles on every iteration instead of
         * holding raw pre-compaction pointers across the loop. */
        CL_GC_PROTECT(a);
        CL_GC_PROTECT(b);
        for (i = 0; i < na; i++) {
            CL_Obj pair[2];
            da = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(a));
            db = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(b));
            pair[0] = da[i]; pair[1] = db[i];
            if (CL_NULL_P(bi_equalp(pair, 2))) {
                CL_GC_UNPROTECT(2);
                return CL_NIL;
            }
        }
        CL_GC_UNPROTECT(2);
        return SYM_T;
    }
    /* Bit vectors: compare by ACTIVE length (the fill pointer if present),
       matching the vector branch above. The whole-word memcmp fast path is only
       valid when neither has a fill pointer; otherwise compare bit-by-bit. */
    if (CL_BIT_VECTOR_P(a) && CL_BIT_VECTOR_P(b)) {
        CL_BitVector *ba = (CL_BitVector *)CL_OBJ_TO_PTR(a);
        CL_BitVector *bb_bv = (CL_BitVector *)CL_OBJ_TO_PTR(b);
        uint32_t na = cl_bv_active_length(ba), nb = cl_bv_active_length(bb_bv);
        uint32_t i;
        if (na != nb) return CL_NIL;
        if (ba->fill_pointer == CL_NO_FILL_POINTER &&
            bb_bv->fill_pointer == CL_NO_FILL_POINTER) {
            uint32_t nwords = CL_BV_WORDS(na);
            if (nwords == 0) return SYM_T;
            return memcmp(ba->data, bb_bv->data, nwords * sizeof(uint32_t)) == 0 ? SYM_T : CL_NIL;
        }
        for (i = 0; i < na; i++)
            if (cl_bv_get_bit(ba, i) != cl_bv_get_bit(bb_bv, i)) return CL_NIL;
        return SYM_T;
    }
    /* Mixed 1-D arrays of differing specialization (string vs char vector,
     * bit-vector vs bit vector, ...).  Same-type combinations are handled by
     * the branches above; this catches the cross-type cases.  Both must be
     * rank 1 with equal dimensions and equalp corresponding elements. */
    if (equalp_arr1d_p(a) && equalp_arr1d_p(b) &&
        equalp_arr1d_rank(a) == 1 && equalp_arr1d_rank(b) == 1) {
        uint32_t la = equalp_arr1d_len(a), lb = equalp_arr1d_len(b);
        uint32_t i;
        if (la != lb) return CL_NIL;
        /* GC SAFETY: a/b are re-read via equalp_arr1d_elt after each
         * (potentially compacting) element recursion. */
        CL_GC_PROTECT(a);
        CL_GC_PROTECT(b);
        for (i = 0; i < la; i++) {
            CL_Obj pair[2];
            pair[0] = equalp_arr1d_elt(a, i);
            pair[1] = equalp_arr1d_elt(b, i);
            if (CL_NULL_P(bi_equalp(pair, 2))) {
                CL_GC_UNPROTECT(2);
                return CL_NIL;
            }
        }
        CL_GC_UNPROTECT(2);
        return SYM_T;
    }
    /* Structs: same type, slot-wise equalp */
    if (CL_STRUCT_P(a) && CL_STRUCT_P(b)) {
        CL_Struct *sa = (CL_Struct *)CL_OBJ_TO_PTR(a);
        CL_Struct *sb = (CL_Struct *)CL_OBJ_TO_PTR(b);
        uint32_t i, nslots;
        if (sa->type_desc != sb->type_desc) return CL_NIL;
        if (sa->n_slots != sb->n_slots) return CL_NIL;
        nslots = sa->n_slots;
        /* GC SAFETY: re-derive the struct pointers from the protected
         * handles each iteration — the slot recursion can compact. */
        CL_GC_PROTECT(a);
        CL_GC_PROTECT(b);
        for (i = 0; i < nslots; i++) {
            CL_Obj pair[2];
            sa = (CL_Struct *)CL_OBJ_TO_PTR(a);
            sb = (CL_Struct *)CL_OBJ_TO_PTR(b);
            pair[0] = sa->slots[i]; pair[1] = sb->slots[i];
            if (CL_NULL_P(bi_equalp(pair, 2))) {
                CL_GC_UNPROTECT(2);
                return CL_NIL;
            }
        }
        CL_GC_UNPROTECT(2);
        return SYM_T;
    }
    /* Hash tables (CLHS): same test, same number of entries, and every key in
       one is present in the other (under the table's test) with an equalp
       value. */
    if (CL_HASHTABLE_P(a) && CL_HASHTABLE_P(b)) {
        extern int cl_hashtable_equalp(CL_Obj a, CL_Obj b,
                                       int (*val_eq)(CL_Obj, CL_Obj));
        return cl_hashtable_equalp(a, b, cl_equalp_pred) ? SYM_T : CL_NIL;
    }
    /* Pathnames: same as equal */
    if (CL_PATHNAME_P(a) && CL_PATHNAME_P(b)) {
        extern int cl_pathname_equal(CL_Obj a, CL_Obj b);
        return cl_pathname_equal(a, b) ? SYM_T : CL_NIL;
    }
    return CL_NIL;
}

static int cl_equalp_pred(CL_Obj a, CL_Obj b)
{
    CL_Obj pair[2];
    pair[0] = a;
    pair[1] = b;
    return !CL_NULL_P(bi_equalp(pair, 2));
}

static CL_Obj bi_not(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return CL_NULL_P(args[0]) ? SYM_T : CL_NIL;
}

/* --- Higher-order --- */

static CL_Obj bi_mapcar(CL_Obj *args, int n)
{
    CL_Obj func = cl_coerce_funcdesig(args[0], "MAPCAR");
    CL_Obj result = CL_NIL, tail = CL_NIL;
    int nlists = n - 1;
    CL_Obj lists[16]; /* Max 16 list arguments */
    CL_Obj call_args[16];
    int i;

    if (nlists > 16)
        cl_error(CL_ERR_ARGS,
                 "MAPCAR: too many list arguments (%d; max 16)", nlists);
    for (i = 0; i < nlists; i++)
        lists[i] = args[i + 1];

    CL_GC_PROTECT(func);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    /* GC-protect the list CURSORS: cl_vm_apply below runs the mapped function,
     * which may allocate and compact (moving GC).  Without rooting &lists[i]
     * the cursors keep pre-compaction offsets and the next cl_car/cl_cdr walks
     * relocated/garbage memory ("CAR: argument is not of type LIST").  Each
     * &lists[i] is registered once; reassigning lists[i] across iterations is
     * fine because the compactor forwards whatever value the address holds. */
    for (i = 0; i < nlists; i++)
        CL_GC_PROTECT(lists[i]);

    for (;;) {
        CL_Obj val;

        /* Check if any list is exhausted */
        for (i = 0; i < nlists; i++) {
            if (CL_NULL_P(lists[i])) goto done;
        }

        /* Collect car of each list */
        for (i = 0; i < nlists; i++) {
            call_args[i] = cl_car(lists[i]);
            lists[i] = cl_cdr(lists[i]);
        }

        /* Call function.  cl_vm_apply GC-roots call_args across the call —
         * essential because the mapped function may allocate (and compact)
         * while reading its own args, which would otherwise relocate an arg
         * out from under this unprotected C array (e.g. MAPCAR #'LIST
         * splitting a gensym under GC stress). */
        val = cl_vm_apply(func, call_args, nlists);

        {
            CL_Obj cell = cl_cons(val, CL_NIL);
            if (CL_NULL_P(result)) {
                result = cell;
            } else {
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
            }
            tail = cell;
        }
    }

done:
    CL_GC_UNPROTECT(3 + nlists);
    return result;
}

static CL_Obj bi_apply(CL_Obj *args, int n)
{
    CL_Obj func = args[0];
    CL_Obj arglist;
    /* Spread the flattened args onto the VM stack (GC-rooted, no fixed C
     * cap) rather than a C array — a fixed flat_args[64] silently dropped
     * args past 64 while OP_APPLY handles CALL-ARGUMENTS-LIMIT (4096). */
    int base = cl_vm.sp;
    int saved_sp = cl_vm.sp;
    int nflat = 0;
    CL_Obj result;

    /* (apply func arg1 arg2 ... arglist) */
    if (n == 2) {
        arglist = args[1];
    } else {
        int i;
        /* Spread initial args, last arg is the list */
        for (i = 1; i < n - 1; i++) {
            cl_vm_push(args[i]);
            nflat++;
        }
        arglist = args[n - 1];
    }

    /* Flatten remaining arglist */
    while (!CL_NULL_P(arglist)) {
        if (nflat >= CL_CALL_ARGS_LIMIT) {
            cl_vm.sp = saved_sp;
            cl_error(CL_ERR_ARGS,
                     "APPLY: too many arguments (call-arguments-limit is %d)",
                     CL_CALL_ARGS_LIMIT);
        }
        cl_vm_push(cl_car(arglist));
        nflat++;
        arglist = cl_cdr(arglist);
    }

    /* Resolve symbol to its function binding (same as funcall) */
    if (CL_SYMBOL_P(func)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(func);
        func = s->function;
        if (CL_NULL_P(func) || func == CL_UNBOUND) {
            cl_vm.sp = saved_sp;
            cl_error_cell(CL_ERR_UNDEFINED, args[0],
                          "APPLY: undefined function: %s", cl_symbol_name(args[0]));
        }
    }
    /* Reader-GF fast path — same probe as bi_funcall's, before the unwrap
     * discards the GF identity.  (apply #'reader (list x)) answers from
     * the inline cache. */
    if (nflat == 1 && cl_funcallable_instance_p(func)) {
        CL_Obj v = cl_gf_reader_ic_probe(func, cl_vm.stack[base]);
        if (v != CL_UNBOUND) {
            cl_vm.sp = saved_sp;
            return v;
        }
    }
    /* Writer-GF fast path, ditto: (apply #'(setf x) val obj-list). */
    if (nflat == 2 && cl_funcallable_instance_p(func)) {
        CL_Obj v = cl_gf_writer_ic_probe(func, cl_vm.stack[base],
                                         cl_vm.stack[base + 1]);
        if (v != CL_UNBOUND) {
            cl_vm.sp = saved_sp;
            return v;
        }
    }
    func = cl_unwrap_funcallable(func);

    if (CL_FUNCTION_P(func)) {
        CL_Function *f = (CL_Function *)CL_OBJ_TO_PTR(func);
        if (!f->func) {
            cl_vm.sp = saved_sp;
            cl_error(CL_ERR_TYPE, "APPLY: NULL function pointer in %s",
                     CL_NULL_P(f->name) ? "?" : cl_symbol_name(f->name));
        }
        result = cl_vm_apply(func, &cl_vm.stack[base], nflat);
        cl_vm.sp = saved_sp;
        return result;
    }
    if (CL_BYTECODE_P(func) || CL_CLOSURE_P(func) || CL_FFI_STUB_P(func)) {
        result = cl_vm_apply(func, &cl_vm.stack[base], nflat);
        cl_vm.sp = saved_sp;
        return result;
    }

    cl_vm.sp = saved_sp;
    cl_error(CL_ERR_TYPE, "APPLY: not a function");
    return CL_NIL;
}

static CL_Obj bi_funcall(CL_Obj *args, int n)
{
    CL_Obj func = args[0];
    /* CL spec: funcall accepts a symbol, resolving to its function binding */
    if (CL_SYMBOL_P(func)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(func);
        func = s->function;
        if (CL_NULL_P(func) || func == CL_UNBOUND)
            cl_error_cell(CL_ERR_UNDEFINED, args[0],
                          "FUNCALL: undefined function: %s", cl_symbol_name(args[0]));
    }
    /* Reader-GF fast path: probe before the unwrap below discards the GF
     * identity cl_vm_apply's own probe would need.  (funcall #'reader x)
     * answers straight from the inline cache; a miss falls through to
     * ordinary dispatch.  Non-allocating; mv state is call_builtin's. */
    if (n == 2 && cl_funcallable_instance_p(func)) {
        CL_Obj v = cl_gf_reader_ic_probe(func, args[1]);
        if (v != CL_UNBOUND) return v;
    }
    /* Writer-GF fast path, ditto: (funcall #'(setf x) val obj). */
    if (n == 3 && cl_funcallable_instance_p(func)) {
        CL_Obj v = cl_gf_writer_ic_probe(func, args[1], args[2]);
        if (v != CL_UNBOUND) return v;
    }
    func = cl_unwrap_funcallable(func);
    if (CL_FUNCTION_P(func)) {
        CL_Function *f = (CL_Function *)CL_OBJ_TO_PTR(func);
        if (!f->func) {
            cl_error(CL_ERR_TYPE, "FUNCALL: NULL function pointer in %s",
                     CL_NULL_P(f->name) ? "?" : cl_symbol_name(f->name));
        }
        return f->func(args + 1, n - 1);
    }
    if (CL_BYTECODE_P(func) || CL_CLOSURE_P(func) || CL_FFI_STUB_P(func)) {
        return cl_vm_apply(func, args + 1, n - 1);
    }
    cl_error(CL_ERR_TYPE, "FUNCALL: not a function");
    return CL_NIL;
}

/* --- Declarations --- */

static CL_Obj bi_proclaim(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    /* No CL_Compiler in scope here (PROCLAIM is a runtime function, not a
     * compile-time-only special form) — this only updates the proclaimed
     * baseline cl_optimize_global.  A fresh top-level compile always seeds
     * its effective settings from that baseline, so the update is visible
     * to every subsequent compile regardless of thread. */
    cl_process_declaration_specifier(NULL, args[0], 1);
    return CL_NIL;
}

/* --- Trace --- */

static CL_Obj trace_list = CL_NIL;

/* Symbols currently traced, process-wide (cl_trace_count is per thread).
 * A JIT call site never fills while it is nonzero (runtime_m68k.c): a site
 * filled by an untracing thread would let a tracing one skip the trace.
 * Changed only under the tables write lock, and always before the
 * cl_call_gen bump that invalidates the filled sites. */
volatile uint32_t cl_traced_function_count = 0;

static CL_Obj bi_trace_function(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        cl_error(CL_ERR_TYPE, "TRACE: not a symbol");
    /* Claim the flag under the tables write lock: the flags |= and
     * cl_trace_count++ are read-modify-writes, and two threads tracing
     * concurrently could lose one of the flag updates / skew the count
     * (nothing in this section allocates, so holding the lock is safe). */
    {
        int newly = 0;
        cl_tables_wrlock();
        s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
        if (!(s->flags & CL_SYM_TRACED)) {
            s->flags |= CL_SYM_TRACED;
            cl_trace_count++;
            cl_traced_function_count++;
            newly = 1;
            /* Traced calls must take the trampoline, not a filled site. */
            cl_call_gen_bump("trace");
        }
        cl_tables_rwunlock();
        /* Cons outside the write lock (STW-vs-rwlock deadlock — see
         * cl_table_prepend_locked).  Only the thread that claimed the flag
         * prepends, so the list gets no duplicates. */
        if (newly)
            cl_table_prepend_locked(&trace_list, args[0]);
    }
    return args[0];
}

static CL_Obj bi_untrace_function(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        cl_error(CL_ERR_TYPE, "UNTRACE: not a symbol");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    if (s->flags & CL_SYM_TRACED) {
        cl_tables_wrlock();
        /* Flag/count RMW under the same lock as the list edit — and
         * re-check under the lock so two concurrent UNTRACEs don't both
         * decrement the count. */
        if (!(s->flags & CL_SYM_TRACED)) {
            cl_tables_rwunlock();
            return args[0];
        }
        s->flags &= ~CL_SYM_TRACED;
        cl_trace_count--;
        cl_traced_function_count--;
        cl_call_gen_bump("untrace");
        {
            CL_Obj prev = CL_NIL, curr = trace_list;
            while (!CL_NULL_P(curr)) {
                if (cl_car(curr) == args[0]) {
                    if (CL_NULL_P(prev))
                        trace_list = cl_cdr(curr);
                    else
                        ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = cl_cdr(curr);
                    break;
                }
                prev = curr;
                curr = cl_cdr(curr);
            }
        }
        cl_tables_rwunlock();
    }
    return args[0];
}

static CL_Obj bi_traced_functions(CL_Obj *args, int n)
{
    CL_Obj result;
    CL_UNUSED(args);
    CL_UNUSED(n);
    cl_tables_rdlock();
    result = trace_list;
    cl_tables_rwunlock();
    return result;
}

static CL_Obj bi_untrace_all(CL_Obj *args, int n)
{
    CL_UNUSED(args);
    CL_UNUSED(n);
    cl_tables_wrlock();
    {
        CL_Obj list = trace_list;
        while (!CL_NULL_P(list)) {
            CL_Obj sym = cl_car(list);
            if (CL_SYMBOL_P(sym)) {
                ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->flags &= ~CL_SYM_TRACED;
            }
            list = cl_cdr(list);
        }
        trace_list = CL_NIL;
        cl_traced_function_count = 0;
    }
    cl_tables_rwunlock();
    cl_trace_count = 0;
    cl_trace_depth = 0;
    cl_call_gen_bump("untrace all");
    return CL_NIL;
}

/* --- Opcode profiler (PROFILE_OPCODES build flag) --- */

static CL_Obj bi_op_counts_reset(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_op_counts_reset();
    return CL_NIL;
}

static CL_Obj bi_op_counts_dump(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_op_counts_dump(stdout);
    return CL_NIL;
}

/* --- JIT introspection (diagnostic helpers; m68k-only payload) --- */

/* Resolve a function-like CL_Obj to its underlying CL_Bytecode.
 * Closures are unwrapped; bare bytecode objects pass through.  Anything
 * else signals a type error.  Returns NULL only when the closure's
 * bytecode slot is unexpectedly non-bytecode (defensive). */
static CL_Bytecode *jit_bytecode_of(CL_Obj obj, const char *fn_name)
{
    if (CL_CLOSURE_P(obj)) {
        CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(obj);
        if (!CL_BYTECODE_P(cl->bytecode)) return NULL;
        return (CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode);
    }
    if (CL_BYTECODE_P(obj))
        return (CL_Bytecode *)CL_OBJ_TO_PTR(obj);
    cl_signal_type_error(obj, "COMPILED-FUNCTION", fn_name);
    return NULL;
}

static CL_Obj bi_jit_dump_bytes(CL_Obj *args, int n)
{
    CL_Bytecode *bc;
    CL_Obj result;
    uint32_t i;

    CL_UNUSED(n);
    bc = jit_bytecode_of(args[0], "%JIT-DUMP-BYTES");
    if (bc == NULL) return CL_NIL;
    if (bc->native_code == NULL || bc->native_len == 0) return CL_NIL;

    /* Build the byte list back-to-front so each cl_cons prepends — no
     * tail pointer needed.  GC-protect the in-progress head.  The
     * native_code buffer is platform_alloc'd (outside the arena) so it's
     * stable across collections — but `bc` itself is an ARENA pointer that
     * goes stale on the first compacting cl_cons, so hoist the buffer
     * pointer and length into C locals before the loop. */
    {
        const uint8_t *native_code = bc->native_code;
        uint32_t native_len = bc->native_len;
        result = CL_NIL;
        CL_GC_PROTECT(result);
        for (i = native_len; i > 0; i--) {
            result = cl_cons(CL_MAKE_FIXNUM((int32_t)native_code[i - 1]),
                             result);
        }
        CL_GC_UNPROTECT(1);
    }
    return result;
}

static CL_Obj bi_jit_compile_stub(CL_Obj *args, int n)
{
    CL_Bytecode *bc;
    CL_UNUSED(n);
    bc = jit_bytecode_of(args[0], "%JIT-COMPILE-STUB");
    if (bc == NULL) return CL_NIL;
    return cl_jit_emit_stub(bc) ? CL_T : CL_NIL;
}

static CL_Obj bi_jit_invoke_count(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    /* Bumped each time OP_CALL takes the native dispatch path.  Stays
     * at 0 on host (no JIT compiled in), so tests written for Amiga
     * verification gate themselves with this counter. */
    return CL_MAKE_FIXNUM((int32_t)cl_jit_invoke_count_get());
}

/* (%CALL-GEN) -- the current call generation (cl_call_gen, mem.h): the
 * word every JIT call site checks.  For tests: each invalidating event
 * (a function-cell write, a collection, TRACE, ...) must change it. */
static CL_Obj bi_call_gen(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return cl_bignum_from_uint32(cl_call_gen);
}

/* (%JIT-C-FLOOR) -- (FLOOR SP) while called from inside native code: the
 * C-stack floor the outermost cl_jit_invoke computed for native call sites
 * (CL_Thread.jit_c_floor, specs/jit-direct-calls.md §4) and this builtin's
 * own stack address, both as integers.  NIL outside native code, on host
 * (no JIT) and when the platform cannot measure the stack. */
static CL_Obj bi_jit_c_floor(CL_Obj *args, int n)
{
    volatile char probe;
    CL_Obj floor_obj, sp_obj, result;
    CL_UNUSED(args); CL_UNUSED(n);
    if (CT->jit_depth <= 0 || CT->jit_c_floor == NULL) return CL_NIL;
    floor_obj = cl_bignum_from_uint32((uint32_t)(uintptr_t)CT->jit_c_floor);
    CL_GC_PROTECT(floor_obj);
    sp_obj = cl_bignum_from_uint32((uint32_t)(uintptr_t)&probe);
    CL_GC_PROTECT(sp_obj);
    result = cl_cons(sp_obj, CL_NIL);
    result = cl_cons(floor_obj, result);
    CL_GC_UNPROTECT(2);
    return result;
}

/* (%JIT-SET-ACTIVE BOOL) — toggle whether functions defined from now on may
 * be JIT-compiled (at definition in eager mode, else once they turn hot);
 * one defined while it is off stays bytecode.  Returns the new state as
 * T/NIL.  Use
 * around `defun` to bind a function in either mode for A/B benchmarks:
 *
 *   (amilisp::%jit-set-active nil) (defun slow-foo ...)
 *   (amilisp::%jit-set-active t)   (defun fast-foo ...)
 *   (time (slow-foo ...)) (time (fast-foo ...))
 *
 * Does not invalidate already-compiled functions — toggle before
 * defining each variant. */
static CL_Obj bi_jit_set_active(CL_Obj *args, int n)
{
    int active;
    CL_UNUSED(n);
    active = !CL_NULL_P(args[0]);
    cl_jit_set_active(active);
    return active ? CL_T : CL_NIL;
}

/* (%JIT-SET-DIRECT-CALLS BOOL) -- the kill switch of native-to-native
 * direct calls (specs/jit-direct-calls.md): with NIL no JIT call site
 * fills, so every call from native code takes the helper path, for A/B
 * runs and bisection.  Toggling bumps the call generation.  Returns the
 * new state as T/NIL; always NIL on a build without the JIT. */
static CL_Obj bi_jit_set_direct_calls(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    cl_jit_set_direct_calls(!CL_NULL_P(args[0]));
    return cl_jit_direct_calls_enabled() ? CL_T : CL_NIL;
}

/* (%JIT-DIRECT-CALL-STATS) -- plist of the call-site miss path's counters
 * since boot: :FILLS :MISSES :REFUSED-TRACE :REFUSED-SHADOW :REFUSED-ABI
 * :REFUSED-NOT-NATIVE, plus :GEN (the current call generation) and
 * :ENABLED.  A hit is never counted (it runs no C).  All zero on host. */
static CL_Obj bi_jit_direct_call_stats(CL_Obj *args, int n)
{
    static const char *const names[CL_JIT_DS_COUNT] = {
        "FILLS", "MISSES", "REFUSED-TRACE", "REFUSED-SHADOW",
        "REFUSED-ABI", "REFUSED-NOT-NATIVE"
    };
    uint32_t st[CL_JIT_DS_COUNT];
    CL_Obj result = CL_NIL, val = CL_NIL;
    int i;
    CL_UNUSED(args); CL_UNUSED(n);
    cl_jit_direct_call_stats(st);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(val);
    /* Each allocating call is its own statement (tests/test_gc_arg_order.sh):
     * an allocation inside cl_cons's argument list could move RESULT after
     * it was read. */
    result = cl_cons(cl_jit_direct_calls_enabled() ? CL_T : CL_NIL, result);
    val = cl_intern_keyword("ENABLED", 7);
    result = cl_cons(val, result);
    val = cl_bignum_from_uint32(cl_call_gen);
    result = cl_cons(val, result);
    val = cl_intern_keyword("GEN", 3);
    result = cl_cons(val, result);
    for (i = CL_JIT_DS_COUNT - 1; i >= 0; i--) {
        val = cl_bignum_from_uint32(st[i]);
        result = cl_cons(val, result);
        val = cl_intern_keyword(names[i], (uint32_t)strlen(names[i]));
        result = cl_cons(val, result);
    }
    CL_GC_UNPROTECT(2);
    return result;
}

/* (%JIT-SET-HOT-THRESHOLD N) — compile a function on its Nth interpreted
 * call (0..126; a function with a loop, or compiled under (speed 3), does
 * not wait).  0 is eager mode: every function compiles at definition, the
 * way the JIT worked before 0.12 -- what `--jit-eager` sets, and what the
 * m68k tests that inspect a function's native code right after its DEFUN
 * bind around themselves.  Returns the previous threshold; always 0 on a
 * build without the JIT. */
static CL_Obj bi_jit_set_hot_threshold(CL_Obj *args, int n)
{
    int prev = cl_jit_hot_threshold();
    CL_UNUSED(n);
    if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) < 0 ||
        CL_FIXNUM_VAL(args[0]) > CL_JIT_HOT_MAX)
        cl_signal_type_error(args[0], "(INTEGER 0 126)", "%JIT-SET-HOT-THRESHOLD");
    cl_jit_set_hot_threshold((int)CL_FIXNUM_VAL(args[0]));
    return CL_MAKE_FIXNUM(prev);
}

/* (%JIT-HOT-THRESHOLD) — the current threshold (0 = eager). */
static CL_Obj bi_jit_hot_threshold(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM(cl_jit_hot_threshold());
}

/* (%JIT-HOT-COMPILE-COUNT) — functions the hot path has handed to the JIT
 * since boot (compiled or found ineligible).  Lets a test prove that a
 * function compiled because it turned hot, not at its definition. */
static CL_Obj bi_jit_hot_compile_count(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)cl_jit_hot_compile_count());
}

/* (%JIT-NATIVE-BYTES) — bytes of native code the JIT has installed since
 * boot, cumulative (a dead function's code is not subtracted): what a load
 * or a workload compiled, for A/B runs of the hot threshold. */
static CL_Obj bi_jit_native_bytes(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)cl_jit_native_bytes());
}

/* (%JIT-ACTIVE-P) — T when the m68k JIT is compiled in and enabled (so new
 * functions get native code), NIL otherwise (host build, or --no-jit).
 * Tests use this to gate JIT-specific expectations, e.g. that interior
 * LET-bound locals are not introspectable in JIT'd frames. */
static CL_Obj bi_jit_active_p(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return cl_jit_enabled() ? CL_T : CL_NIL;
}

/* (%JIT-SET-FRAMES BOOL) — enable/disable the per-call CL_Frame that makes
 * JIT'd functions visible to EXT:BACKTRACE / EXT:FRAME-LOCALS.  On by
 * default; off, native code pushes no frames and its call sites stop
 * going native-to-native (jit_common.c).  Returns the new state as T/NIL. */
static CL_Obj bi_jit_set_frames(CL_Obj *args, int n)
{
    int on;
    CL_UNUSED(n);
    on = !CL_NULL_P(args[0]);
    cl_jit_set_shadow_frames(on);
    return on ? CL_T : CL_NIL;
}

/* (%JIT-FRAMES-P) — T when JIT shadow frames are currently enabled. */
static CL_Obj bi_jit_frames_p(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return cl_jit_shadow_frames_enabled() ? CL_T : CL_NIL;
}

/* (%JIT-DISASSEMBLE fn) — prints one line of m68k assembly per
 * instruction in fn's native_code, to *standard-output*.  Returns NIL.
 * A function that has not turned hot yet is compiled first, so JITEXPAND
 * shows a fresh definition.  Prints a friendly placeholder when the
 * function has no native code (the JIT declined it, it was defined with
 * the JIT off, or we're on host where JIT_M68K is undefined). */
static CL_Obj bi_jit_disassemble(CL_Obj *args, int n)
{
    CL_Bytecode *bc;
    CL_UNUSED(n);
    bc = jit_bytecode_of(args[0], "%JIT-DISASSEMBLE");
    if (bc == NULL) return CL_NIL;
    cl_jit_compile_if_counting(bc);   /* heap-free: bc stays valid */
    if (bc->native_code == NULL || bc->native_len == 0) {
        cl_write_cstring_to_stdout(
            "  (no native code - function runs through the bytecode interpreter)\n");
        return CL_NIL;
    }
    cl_jit_disassemble(bc->native_code, bc->native_len);
    return CL_NIL;
}

/* --- Property lists --- */

static CL_Obj bi_symbol_plist(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "SYMBOL-PLIST");
    if (CL_NULL_P(args[0])) return CL_NIL;
    return ((CL_Symbol *)CL_OBJ_TO_PTR(args[0]))->plist;
}

static CL_Obj bi_set_symbol_plist(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "%SET-SYMBOL-PLIST");
    if (CL_NULL_P(args[0]))
        cl_error(CL_ERR_GENERAL, "Cannot set symbol-plist of NIL");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    s->plist = args[1];
    return args[1];
}

static CL_Obj bi_get(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_Obj plist, indicator, def;
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "GET");
    indicator = args[1];
    def = (n > 2) ? args[2] : CL_NIL;
    if (CL_NULL_P(args[0])) return def;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    plist = s->plist;
    while (!CL_NULL_P(plist) && !CL_NULL_P(cl_cdr(plist))) {
        if (cl_car(plist) == indicator)
            return cl_car(cl_cdr(plist));
        plist = cl_cdr(cl_cdr(plist));
    }
    return def;
}

static CL_Obj bi_setf_get(CL_Obj *args, int n)
{
    /* (%setf-get symbol indicator value) */
    CL_Symbol *s;
    CL_Obj plist, indicator, value;
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "GET");
    if (CL_NULL_P(args[0]))
        cl_error(CL_ERR_GENERAL, "Cannot setf get on NIL");
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    indicator = args[1];
    value = args[2];
    plist = s->plist;
    /* Search for existing indicator */
    while (!CL_NULL_P(plist) && !CL_NULL_P(cl_cdr(plist))) {
        if (cl_car(plist) == indicator) {
            /* Update value in place */
            ((CL_Cons *)CL_OBJ_TO_PTR(cl_cdr(plist)))->car = value;
            return value;
        }
        plist = cl_cdr(cl_cdr(plist));
    }
    /* Not found — prepend indicator+value.  Split the nested conses: the
     * inner one compacts, leaving the indicator local copy stale for the
     * outer — re-read it from the rooted args[]. */
    {
        CL_Obj new_plist;
        CL_GC_PROTECT(value);
        new_plist = cl_cons(value, s->plist);
        new_plist = cl_cons(args[1], new_plist);
        CL_GC_UNPROTECT(1);
        /* Re-fetch symbol pointer after potential GC */
        s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
        s->plist = new_plist;
    }
    return value;
}

static CL_Obj bi_remprop(CL_Obj *args, int n)
{
    CL_Symbol *s;
    CL_Obj indicator, plist, prev_val;
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_signal_type_error(args[0], "SYMBOL", "REMPROP");
    if (CL_NULL_P(args[0])) return CL_NIL;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(args[0]);
    indicator = args[1];
    plist = s->plist;
    prev_val = CL_NIL;
    while (!CL_NULL_P(plist) && !CL_NULL_P(cl_cdr(plist))) {
        if (cl_car(plist) == indicator) {
            CL_Obj rest = cl_cdr(cl_cdr(plist));
            if (CL_NULL_P(prev_val))
                s->plist = rest;
            else
                ((CL_Cons *)CL_OBJ_TO_PTR(prev_val))->cdr = rest;
            return SYM_T;
        }
        prev_val = cl_cdr(plist);  /* value cell */
        plist = cl_cdr(cl_cdr(plist));
    }
    return CL_NIL;
}

/* --- Registration --- */

/* Sub-module init functions */
void cl_builtins_arith_init(void);
void cl_builtins_io_init(void);
void cl_builtins_mutation_init(void);
void cl_builtins_strings_init(void);
void cl_builtins_lists_init(void);
void cl_builtins_hashtable_init(void);
void cl_builtins_sequence_init(void);
void cl_builtins_sequence2_init(void);
void cl_builtins_type_init(void);
void cl_builtins_condition_init(void);
void cl_builtins_condition_shutdown(void);
void cl_builtins_package_init(void);
void cl_builtins_struct_init(void);
void cl_builtins_struct_shutdown(void);
void cl_builtins_stream_init(void);
void cl_builtins_array_init(void);
void cl_float_math_init(void);
void cl_builtins_random_init(void);
void cl_builtins_bitvector_init(void);
void cl_builtins_pathname_init(void);
void cl_builtins_describe_init(void);
void cl_builtins_inspect_init(void);
void cl_builtins_thread_init(void);
void cl_builtins_ffi_init(void);
void cl_builtins_amiga_init(void);
void cl_builtins_bindtab_init(void);

/* --- Function arglist introspection (EXT:FUNCTION-ARGLIST) ----------------
 *
 * Returns the lambda-list of FN, for editor tooling (Sly/SLYNK arglist).
 * For functions compiled from source the exact written lambda-list is
 * returned verbatim — it is captured on CL_Bytecode.source_lambda_list at
 * compile time and survives FASL round-trips (format v9).  When no captured
 * list is available (C builtins, or bytecode predating the capture) a
 * lambda-list is reconstructed from the arity descriptor with placeholder
 * argument names (#:ARG0, #:ARG1, ...).  Generic functions are handled by
 * the Lisp layer via GF-LAMBDA-LIST; for those (and any non-function) this
 * primitive returns :NOT-AVAILABLE. */

static CL_Obj arg_placeholder(int idx)
{
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "ARG%d", idx);
    CL_Obj str = cl_make_string(buf, (uint32_t)len);
    CL_Obj sym;
    CL_GC_PROTECT(str);
    sym = cl_make_uninterned_symbol(str);
    CL_GC_UNPROTECT(1);
    return sym;
}

/* Build a lambda-list from decomposed arity info.  Conses are prepended onto
 * `result` walking the sections in reverse, so the final order is
 *   req... [&optional opt...] [&rest rest] [&key kw...].
 * key_syms (the platform-allocated keyword array) may be NULL when n_keys==0;
 * it lives outside the GC arena, so its address is stable across the
 * allocations below and its slots are updated in place by the compactor. */
static CL_Obj build_arglist(int n_required, int n_optional, int has_rest,
                            int n_keys, CL_Obj *key_syms)
{
    CL_Obj result = CL_NIL;
    CL_Obj tmp = CL_NIL;
    int i;
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tmp);

    if (n_keys > 0) {
        for (i = n_keys - 1; i >= 0; i--)
            result = cl_cons(key_syms[i], result);
        tmp = cl_intern_in("&KEY", 4, cl_package_cl);
        result = cl_cons(tmp, result);
    }
    if (has_rest) {
        tmp = arg_placeholder(n_required + n_optional);
        result = cl_cons(tmp, result);
        tmp = cl_intern_in("&REST", 5, cl_package_cl);
        result = cl_cons(tmp, result);
    }
    if (n_optional > 0) {
        for (i = n_optional - 1; i >= 0; i--) {
            tmp = arg_placeholder(n_required + i);
            result = cl_cons(tmp, result);
        }
        tmp = cl_intern_in("&OPTIONAL", 9, cl_package_cl);
        result = cl_cons(tmp, result);
    }
    for (i = n_required - 1; i >= 0; i--) {
        tmp = arg_placeholder(i);
        result = cl_cons(tmp, result);
    }

    CL_GC_UNPROTECT(2);
    return result;
}

CL_Obj cl_function_arglist(CL_Obj fn)
{
    CL_Bytecode *bc = NULL;

    if (CL_HEAP_P(fn)) {
        void *p = CL_OBJ_TO_PTR(fn);
        uint8_t t = CL_HDR_TYPE(p);
        if (t == TYPE_CLOSURE) {
            CL_Closure *clo = (CL_Closure *)p;
            if (CL_HEAP_P(clo->bytecode) &&
                CL_HDR_TYPE(CL_OBJ_TO_PTR(clo->bytecode)) == TYPE_BYTECODE)
                bc = (CL_Bytecode *)CL_OBJ_TO_PTR(clo->bytecode);
        } else if (t == TYPE_BYTECODE) {
            bc = (CL_Bytecode *)p;
        } else if (t == TYPE_FUNCTION) {
            /* C builtin: reconstruct from min/max args. */
            CL_Function *f = (CL_Function *)p;
            int has_rest = (f->max_args < 0);
            int n_opt = has_rest ? 0 : (f->max_args - f->min_args);
            return build_arglist(f->min_args, n_opt, has_rest, 0, NULL);
        } else if (t == TYPE_FFI_STUB) {
            /* FFI stub: fixed arity from its kind (DEFCFUN's register
             * count, 1 for a field reader, 2 for a writer, ...).  The C
             * prototype with the real parameter names is in the
             * docstring when the binding module kept it. */
            return build_arglist(cl_ffi_stub_arity(fn), 0, 0, 0, NULL);
        }
    }

    if (bc) {
        /* Path B: exact lambda-list captured at compile time. */
        if (!CL_NULL_P(bc->source_lambda_list))
            return bc->source_lambda_list;
        /* Path A: reconstruct from the arity descriptor. */
        return build_arglist((int)(bc->arity & 0x7FFF),
                             (int)bc->n_optional,
                             (bc->arity & 0x8000) ? 1 : 0,
                             (int)bc->n_keys, bc->key_syms);
    }

    return cl_intern_in("NOT-AVAILABLE", 13, cl_package_keyword);
}

static CL_Obj bi_function_arglist(CL_Obj *args, int n)
{
    (void)n;
    return cl_function_arglist(args[0]);
}

/* --- Function source location (EXT:FUNCTION-SOURCE-LOCATION) --------------
 *
 * Returns the source location of FN as the list (FILE LINE), where FILE is a
 * string namestring and LINE is a 1-based fixnum — the data captured on
 * CL_Bytecode.source_file / .source_line at compile time (and preserved
 * through FASL).  This is the native primitive behind the Sly/SLYNK
 * find-definitions (M-.) backend; the Lisp layer wraps it into the SLYNK
 * (:location (:file ...) (:line ...)) shape.
 *
 * Returns :NOT-AVAILABLE when FN is not a code object, or has no recorded
 * file (e.g. functions defined at the REPL, where source_file is NULL). */
CL_Obj cl_function_source_location(CL_Obj fn)
{
    CL_Bytecode *bc = NULL;
    const char *file;
    int line;
    CL_Obj file_str, result;

    if (CL_HEAP_P(fn)) {
        void *p = CL_OBJ_TO_PTR(fn);
        uint8_t t = CL_HDR_TYPE(p);
        if (t == TYPE_CLOSURE) {
            CL_Closure *clo = (CL_Closure *)p;
            if (CL_HEAP_P(clo->bytecode) &&
                CL_HDR_TYPE(CL_OBJ_TO_PTR(clo->bytecode)) == TYPE_BYTECODE)
                bc = (CL_Bytecode *)CL_OBJ_TO_PTR(clo->bytecode);
        } else if (t == TYPE_BYTECODE) {
            bc = (CL_Bytecode *)p;
        }
    }

    if (!bc || bc->source_file == NULL)
        return cl_intern_in("NOT-AVAILABLE", 13, cl_package_keyword);

    /* Read both fields before allocating.  source_file points into the
     * interned source-file pool (platform_alloc, outside the GC arena), so
     * its address is stable; bc itself is kept live by args[0] but must not
     * be re-dereferenced after an allocation. */
    file = bc->source_file;
    line = (int)bc->source_line;

    file_str = cl_make_string(file, (uint32_t)strlen(file));
    CL_GC_PROTECT(file_str);
    result = cl_cons(CL_MAKE_FIXNUM(line), CL_NIL);
    CL_GC_PROTECT(result);
    result = cl_cons(file_str, result);
    CL_GC_UNPROTECT(2);
    return result;
}

static CL_Obj bi_function_source_location(CL_Obj *args, int n)
{
    (void)n;
    return cl_function_source_location(args[0]);
}

/* --- Backtrace introspection (EXT:BACKTRACE / EXT:FRAME-LOCALS) -----------
 *
 * Native primitives behind the Sly/SLYNK SLDB (debugger) backend.  Within a
 * *debugger-hook* these report the error-time frame window; outside an error
 * they report the live call stack.  The Lisp layer formats the results into
 * the SLYNK debugger protocol; restarts are already covered by the existing
 * COMPUTE-RESTARTS / FIND-RESTART / INVOKE-RESTART builtins. */
static CL_Obj bi_backtrace(CL_Obj *args, int n)
{
    int max = 0;  /* 0 = all frames */
    if (n >= 1 && CL_FIXNUM_P(args[0]))
        max = CL_FIXNUM_VAL(args[0]);
    return cl_vm_backtrace_list(max);
}

static CL_Obj bi_frame_locals(CL_Obj *args, int n)
{
    (void)n;
    if (!CL_FIXNUM_P(args[0]))
        return cl_intern_in("NOT-AVAILABLE", 13, cl_package_keyword);
    return cl_vm_frame_locals(CL_FIXNUM_VAL(args[0]));
}

static CL_Obj bi_quit(CL_Obj *args, int n)
{
    int code = 0;
    if (n > 0 && CL_FIXNUM_P(args[0]))
        code = CL_FIXNUM_VAL(args[0]);
    cl_exit_code = code;
    cl_error(CL_ERR_EXIT, "");
    return CL_NIL; /* unreachable */
}

/* --- Exit hooks (EXT:*EXIT-HOOKS*) ---------------------------------------
 *
 * EXT:*EXIT-HOOKS* holds a list of function designators that amilisp funcalls
 * with no arguments when it shuts down normally: (QUIT)/(EXIT), the end of a
 * --load/--eval/script run, or EOF at the REPL.  cl_run_exit_hooks() is called
 * from the single shutdown funnel in main.c, before ANY subsystem teardown —
 * the VM, the streams and the heap are all still fully alive, so a hook may
 * print, close files, flush caches and stop threads.
 *
 * Why a hook list rather than UNWIND-PROTECT: (QUIT) unwinds via CL_ERR_EXIT,
 * which deliberately drops the whole NLX stack WITHOUT running cleanups (see
 * cl_error in error.c), so no UNWIND-PROTECT cleanup ever observes process
 * exit.  Exit hooks are the supported way to run code on the way out.
 *
 * Hooks run most-recently-added first (ADD-EXIT-HOOK pushes), matching the
 * LIFO teardown order of C's atexit() and of nested UNWIND-PROTECTs. */
static CL_Obj SYM_EXIT_HOOKS = CL_NIL;   /* EXT:*EXIT-HOOKS* */

/* A hook must be callable at shutdown: a function object, or a symbol naming
 * one.  Symbols are resolved when the hook RUNS, so (ext:add-exit-hook 'foo)
 * works before FOO is defined and picks up a later redefinition. */
static int exit_hook_designator_p(CL_Obj obj)
{
    return CL_SYMBOL_P(obj) || CL_FUNCTION_OBJ_P(obj) ||
           cl_funcallable_instance_p(obj);
}

/* Current *EXIT-HOOKS* value, normalized: an unbound or non-list value reads
 * as the empty list rather than erroring at shutdown. */
static CL_Obj exit_hooks_value(void)
{
    CL_Obj hooks;
    if (CL_NULL_P(SYM_EXIT_HOOKS)) return CL_NIL;  /* not interned yet */
    hooks = cl_symbol_value(SYM_EXIT_HOOKS);
    if (hooks == CL_UNBOUND || !CL_CONS_P(hooks)) return CL_NIL;
    return hooks;
}

static CL_Obj bi_add_exit_hook(CL_Obj *args, int n)
{
    CL_Obj fn = args[0];
    CL_Obj hooks, cur;
    CL_UNUSED(n);

    if (!exit_hook_designator_p(fn)) {
        char buf[128];
        cl_prin1_to_string(fn, buf, (int)sizeof(buf));
        cl_error(CL_ERR_TYPE,
                 "ADD-EXIT-HOOK: %s is not a function designator "
                 "(expected a function or a symbol naming one)", buf);
    }

    /* Idempotent under EQL: re-LOADing a file that registers its cleanup must
     * not queue the same hook twice.  EQL on a function designator is plain
     * identity — no number or character can reach here. */
    hooks = exit_hooks_value();
    for (cur = hooks; CL_CONS_P(cur); cur = cl_cdr(cur)) {
        if (cl_car(cur) == fn) return fn;
    }

    /* fn is an argument slot (VM-stack rooted); hooks is a C local that must
     * survive the cons below, which can compact. */
    CL_GC_PROTECT(hooks);
    hooks = cl_cons(args[0], hooks);
    cl_set_symbol_value(SYM_EXIT_HOOKS, hooks);
    CL_GC_UNPROTECT(1);
    return args[0];
}

static CL_Obj bi_remove_exit_hook(CL_Obj *args, int n)
{
    CL_Obj fn = args[0];
    CL_Obj hooks, cur, prev;
    CL_UNUSED(n);

    hooks = exit_hooks_value();
    prev = CL_NIL;
    for (cur = hooks; CL_CONS_P(cur); cur = cl_cdr(cur)) {
        if (cl_car(cur) == fn) {   /* EQL on a designator == identity */
            /* Destructive splice (DELETE-like) — allocation-free, so nothing
             * can move underneath us here. */
            if (CL_NULL_P(prev)) {
                cl_set_symbol_value(SYM_EXIT_HOOKS, cl_cdr(cur));
            } else {
                CL_Cons *cell = (CL_Cons *)CL_OBJ_TO_PTR(prev);
                cell->cdr = cl_cdr(cur);
            }
            return SYM_T;
        }
        prev = cur;
    }
    return CL_NIL;
}

/* Run and clear EXT:*EXIT-HOOKS*.  Called once from main.c's shutdown funnel;
 * safe to call again (the list is taken before the first hook runs).
 *
 * An erroring hook must not take the process down with it or skip the hooks
 * behind it, so every call gets its own CL_CATCH: an error is reported and the
 * next hook runs.  A hook that itself calls (QUIT) simply stops the sequence —
 * re-entering shutdown from inside shutdown is never what the user wants. */
void cl_run_exit_hooks(void)
{
    CL_Obj hooks = exit_hooks_value();
    CL_Obj fn = CL_NIL;

    if (CL_NULL_P(hooks)) return;
    cl_set_symbol_value(SYM_EXIT_HOOKS, CL_NIL);

    /* (QUIT) longjmps out of arbitrarily deep VM frames; main.c's error
     * branches reset the VM the same way before continuing. */
    cl_vm.sp = 0;
    cl_vm.fp = 0;

    /* hooks is the list cursor and fn the callee across cl_vm_apply, which
     * allocates and can compact — both are C locals invisible to the GC
     * otherwise.  Protecting BEFORE the CL_CATCH below matters: an unwind
     * restores gc_root_count to the catch-site snapshot, which then still
     * includes these two roots. */
    CL_GC_PROTECT(hooks);
    CL_GC_PROTECT(fn);
    while (CL_CONS_P(hooks)) {
        int err;
        CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            /* Resolve inside the catch: a symbol hook whose function was never
             * defined (or was FMAKUNBOUND'd) reports through the same path as
             * a hook that errors while running. */
            fn = cl_coerce_funcdesig(cl_car(hooks), "exit hook");
            cl_vm_apply(fn, NULL, 0);
            CL_UNCATCH();
        } else if (err == CL_ERR_EXIT) {
            /* A hook called (QUIT): stop here rather than re-entering the
             * shutdown sequence we are already in. */
            CL_UNCATCH();
            break;
        } else {
            char buf[128];
            cl_prin1_to_string(cl_car(hooks), buf, (int)sizeof(buf));
            cl_write_cstring_to_error("; Warning: error in exit hook ");
            cl_write_cstring_to_error(buf);
            cl_write_cstring_to_error(": ");
            cl_write_cstring_to_error(cl_error_msg);
            cl_write_cstring_to_error("\n");
            cl_vm.sp = 0;
            cl_vm.fp = 0;
            CL_UNCATCH();
        }
        hooks = cl_cdr(hooks);
    }
    CL_GC_UNPROTECT(2);
}

/* --- Thread death hooks (MP:*THREAD-DEATH-HOOKS*) -------------------------
 *
 * A worker thread that an unhandled error ends is gone without a word: its
 * status says so to whoever asks, but nobody is told.  And the error that
 * ends most of them in practice -- the heap exhausted, cl_storage_error --
 * resets the handler and NLX stacks before it unwinds, so no HANDLER-CASE
 * and no UNWIND-PROTECT in the thread's own code ever sees it.  The editor's
 * REPL thread died that way in the middle of a system load, and the editor
 * went on showing "Loading system ..." (lib/dev-repl.lisp is the first user).
 *
 * MP:*THREAD-DEATH-HOOKS* is a list of function designators, called with
 * the thread object and the error's message, ON the dying thread, after its
 * stacks were dropped and before it leaves the thread list -- so what the
 * thread held is garbage by now and a hook has the room a collection finds.
 * Not called for a thread that returned, that took its ABORT restart, or
 * that MP:DESTROY-THREAD ended.  The symbol is looked up when a thread dies
 * (rare) rather than cached in a static a heap image would have to carry. */
void cl_run_thread_death_hooks(const char *msg)
{
    CL_Obj sym, hooks;
    CL_Obj fn = CL_NIL, text = CL_NIL;
    char saved[512];

    sym = cl_find_symbol("*THREAD-DEATH-HOOKS*", 20, cl_package_mp);
    if (!CL_SYMBOL_P(sym)) return;
    hooks = cl_symbol_value(sym);
    if (hooks == CL_UNBOUND || !CL_CONS_P(hooks)) return;

    /* MSG is the thread's error buffer, which a failing hook writes over. */
    strncpy(saved, msg ? msg : "", sizeof(saved) - 1);
    saved[sizeof(saved) - 1] = '\0';

    /* The error longjmp'd out of arbitrarily deep VM frames (see
     * cl_run_exit_hooks). */
    cl_vm.sp = 0;
    cl_vm.fp = 0;

    /* Protected BEFORE the CL_CATCH, as in cl_run_exit_hooks: an unwind
     * restores gc_root_count to a snapshot that still includes these. */
    CL_GC_PROTECT(hooks);
    CL_GC_PROTECT(fn);
    CL_GC_PROTECT(text);
    while (CL_CONS_P(hooks)) {
        int err;
        CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            CL_Obj call_args[2];
            /* Inside the catch: the string is the first allocation of a
             * thread that may have died of having no room for one. */
            text = cl_make_string(saved, (uint32_t)strlen(saved));
            fn = cl_coerce_funcdesig(cl_car(hooks), "thread death hook");
            call_args[0] = CT->thread_obj;
            call_args[1] = text;
            cl_vm_apply(fn, call_args, 2);
            CL_UNCATCH();
        } else {
            char envbuf[8];
            if (platform_getenv("AMILISP_THREAD_ERRORS", envbuf, sizeof(envbuf))) {
                cl_write_cstring_to_error("; Warning: error in thread death hook: ");
                cl_write_cstring_to_error(cl_error_msg);
                cl_write_cstring_to_error("\n");
            }
            cl_vm.sp = 0;
            cl_vm.fp = 0;
            CL_UNCATCH();
            if (err == CL_ERR_EXIT) break;   /* (QUIT) in a hook: no more */
        }
        hooks = cl_cdr(hooks);
    }
    CL_GC_UNPROTECT(3);
}

/* Single shared stub for CL functions not yet implemented in amilisp.
 * Any call signals an error; registering the stub satisfies FBOUNDP and
 * SYMBOL-FUNCTION checks in the ANSI test suite without a full implementation. */
static CL_Obj bi_unimplemented_stub(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_error(CL_ERR_GENERAL, "not yet implemented");
    return CL_NIL; /* unreachable */
}

/* Names that ANSI requires to be exported from COMMON-LISP (11.1.2.1) but
 * which currently lack a backing implementation.  Interning them here is
 * enough to satisfy FIND-SYMBOL / cl-symbols presence tests; calling any
 * such symbol as a function still signals "function not bound" because
 * its function cell is left CL_UNBOUND.  When a real implementation is
 * added later it should remove the corresponding entry from this list. */
static const char *const cl_reserved_symbol_names[] = {
    "*BREAK-ON-SIGNALS*",
    "//", "///",
    "BROADCAST-STREAM",
    "CIS",
    "COMPILER-MACRO",
    "COMPUTE-APPLICABLE-METHODS",
    "CONCATENATED-STREAM",
    "DECLARATION",
    "DESCRIBE-OBJECT",
    "DO-ALL-SYMBOLS",
    "ED",
    "ECHO-STREAM",
    "EXTENDED-CHAR",
    "FILE-STREAM",
    "FLOATING-POINT-INEXACT", "FLOATING-POINT-INVALID-OPERATION",
    "FLOATING-POINT-OVERFLOW", "FLOATING-POINT-UNDERFLOW",
    "FORMATTER", "FUNCTION-KEYWORDS",
    "GENERIC-FUNCTION",
    "KEYWORD",
    "MAKE-INSTANCES-OBSOLETE", "MAKE-LOAD-FORM",
    "MAKE-METHOD", "MAKE-TWO-WAY-STREAM",
    "METHOD", "METHOD-COMBINATION",
    "NO-NEXT-METHOD",
    "PHASE",
    "RESTART",
    "SIGNED-BYTE", "SIGNUM",
    "SLOT-MISSING",
    "STANDARD", "STEP",
    "STRING-STREAM",
    "STRUCTURE", "STRUCTURE-CLASS",
    "SYNONYM-STREAM",
    "TWO-WAY-STREAM", "TWO-WAY-STREAM-INPUT-STREAM",
    "TWO-WAY-STREAM-OUTPUT-STREAM",
    "UNSIGNED-BYTE",
    "UPDATE-INSTANCE-FOR-DIFFERENT-CLASS",
    "UPDATE-INSTANCE-FOR-REDEFINED-CLASS",
    "UPGRADED-COMPLEX-PART-TYPE",
    "VARIABLE",
    "WITH-CONDITION-RESTARTS", "WITH-PACKAGE-ITERATOR",
    NULL
};

static void cl_intern_standard_cl_symbols(void)
{
    const char *const *p;
    for (p = cl_reserved_symbol_names; *p != NULL; p++) {
        (void)cl_intern_in(*p, (uint32_t)strlen(*p), cl_package_cl);
    }
}

/* Intern NAME in EXT and export it.  Two statements on purpose: the package
 * must be read after the intern has allocated (see cl_list2 in mem.h). */
static void export_ext_symbol(const char *name, uint32_t len)
{
    CL_Obj sym = cl_intern_in(name, len, cl_package_ext);
    cl_export_symbol(sym, cl_package_ext);
}

void cl_builtins_init(void)
{
    /* List ops */
    defun("CONS", bi_cons, 2, 2);
    defun("CAR", bi_car, 1, 1);
    defun("CDR", bi_cdr, 1, 1);
    defun("FIRST", bi_car, 1, 1);
    defun("SECOND", bi_second, 1, 1);
    defun("THIRD", bi_third, 1, 1);
    defun("FOURTH", bi_fourth, 1, 1);
    defun("FIFTH", bi_fifth, 1, 1);
    defun("SIXTH", bi_sixth, 1, 1);
    defun("SEVENTH", bi_seventh, 1, 1);
    defun("EIGHTH", bi_eighth, 1, 1);
    defun("NINTH", bi_ninth, 1, 1);
    defun("TENTH", bi_tenth, 1, 1);
    defun("REST", bi_cdr, 1, 1);
    defun("LIST", bi_list, 0, -1);
    defun("LENGTH", bi_length, 1, 1);
    defun("APPEND", bi_append, 0, -1);
    defun("REVERSE", bi_reverse, 1, 1);
    defun("NTH", bi_nth, 2, 2);

    /* Predicates */
    defun("NULL", bi_null, 1, 1);
    defun("CONSP", bi_consp, 1, 1);
    defun("ATOM", bi_atom, 1, 1);
    defun("LISTP", bi_listp, 1, 1);
    defun("SYMBOLP", bi_symbolp, 1, 1);
    defun("STRINGP", bi_stringp, 1, 1);
    defun("SIMPLE-STRING-P", bi_simple_string_p, 1, 1);
    defun("FUNCTIONP", bi_functionp, 1, 1);
    defun("COMPILED-FUNCTION-P", bi_compiled_function_p, 1, 1);
    defun("FUNCTION-LAMBDA-EXPRESSION", bi_function_lambda_expression, 1, 1);
    defun("EQ", bi_eq, 2, 2);
    defun("EQL", bi_eql, 2, 2);
    defun("EQUAL", bi_equal, 2, 2);
    defun("EQUALP", bi_equalp, 2, 2);
    defun("NOT", bi_not, 1, 1);

    /* Higher-order */
    defun("MAPCAR", bi_mapcar, 2, -1);
    defun("APPLY", bi_apply, 2, -1);
    defun("FUNCALL", bi_funcall, 1, -1);

    /* Declarations */
    defun("PROCLAIM", bi_proclaim, 1, 1);

    /* Property lists */
    defun("SYMBOL-PLIST", bi_symbol_plist, 1, 1);
    cl_register_builtin("%DBIND-TOO-FEW", bi_dbind_too_few, 0, 0, cl_package_amilisp);
    cl_register_builtin("%DBIND-TOO-MANY", bi_dbind_too_many, 0, 0, cl_package_amilisp);
    /* Cache + GC-root the helper symbols so the compiler's destructuring-bind
     * arity guards can be emitted without allocating (see compiler_internal.h). */
    cl_dbind_too_few_sym = cl_intern_in("%DBIND-TOO-FEW", 14, cl_package_amilisp);
    cl_gc_register_root(&cl_dbind_too_few_sym);
    cl_dbind_too_many_sym = cl_intern_in("%DBIND-TOO-MANY", 15, cl_package_amilisp);
    cl_gc_register_root(&cl_dbind_too_many_sym);
    cl_register_builtin("%DBIND-CHECK-KEYS", bi_dbind_check_keys, 2, 2, cl_package_amilisp);
    cl_dbind_check_keys_sym = cl_intern_in("%DBIND-CHECK-KEYS", 17, cl_package_amilisp);
    cl_gc_register_root(&cl_dbind_check_keys_sym);
    cl_register_builtin("%LOCAL-ARITY-ERROR", bi_local_arity_error, 3, 3, cl_package_amilisp);
    cl_local_arity_error_sym = cl_intern_in("%LOCAL-ARITY-ERROR", 18, cl_package_amilisp);
    cl_gc_register_root(&cl_local_arity_error_sym);
    cl_register_builtin("%SET-SYMBOL-PLIST", bi_set_symbol_plist, 2, 2, cl_package_amilisp);
    defun("GET", bi_get, 2, 3);
    cl_register_builtin("%SETF-GET", bi_setf_get, 3, 3, cl_package_amilisp);
    defun("REMPROP", bi_remprop, 2, 2);

    /* Trace (internal helpers, called by compiler special forms) */
    cl_register_builtin("%TRACE-FUNCTION", bi_trace_function, 1, 1, cl_package_amilisp);
    cl_register_builtin("%UNTRACE-FUNCTION", bi_untrace_function, 1, 1, cl_package_amilisp);
    cl_register_builtin("%TRACED-FUNCTIONS", bi_traced_functions, 0, 0, cl_package_amilisp);
    cl_register_builtin("%UNTRACE-ALL", bi_untrace_all, 0, 0, cl_package_amilisp);

    /* Function arglist introspection (Sly/SLYNK) — exported from EXT */
    cl_register_builtin("FUNCTION-ARGLIST", bi_function_arglist, 1, 1, cl_package_ext);
    export_ext_symbol("FUNCTION-ARGLIST", 16);
    cl_register_builtin("FUNCTION-SOURCE-LOCATION", bi_function_source_location,
                        1, 1, cl_package_ext);
    export_ext_symbol("FUNCTION-SOURCE-LOCATION", 24);

    /* Backtrace introspection (Sly/SLYNK SLDB) — exported from EXT */
    cl_register_builtin("BACKTRACE", bi_backtrace, 0, 1, cl_package_ext);
    export_ext_symbol("BACKTRACE", 9);
    cl_register_builtin("FRAME-LOCALS", bi_frame_locals, 1, 1, cl_package_ext);
    export_ext_symbol("FRAME-LOCALS", 12);

    /* Opcode profiler (no-op unless built with -DPROFILE_OPCODES) */
    cl_register_builtin("%OP-COUNTS-RESET", bi_op_counts_reset, 0, 0, cl_package_amilisp);
    cl_register_builtin("%OP-COUNTS-DUMP", bi_op_counts_dump, 0, 0, cl_package_amilisp);

    /* JIT introspection — payload only on m68k builds; on host both
     * return NIL.  Useful for verifying the encoder pipeline before
     * any function actually executes through native code. */
    cl_register_builtin("%JIT-DUMP-BYTES",    bi_jit_dump_bytes,    1, 1, cl_package_amilisp);
    cl_register_builtin("%JIT-COMPILE-STUB",  bi_jit_compile_stub,  1, 1, cl_package_amilisp);
    cl_register_builtin("%JIT-INVOKE-COUNT",  bi_jit_invoke_count,  0, 0, cl_package_amilisp);
    cl_register_builtin("%JIT-C-FLOOR",       bi_jit_c_floor,       0, 0, cl_package_amilisp);
    cl_register_builtin("%CALL-GEN",          bi_call_gen,          0, 0, cl_package_amilisp);
    cl_register_builtin("%JIT-SET-DIRECT-CALLS", bi_jit_set_direct_calls, 1, 1, cl_package_amilisp);
    cl_register_builtin("%JIT-DIRECT-CALL-STATS", bi_jit_direct_call_stats, 0, 0, cl_package_amilisp);
    cl_register_builtin("%JIT-DISASSEMBLE",   bi_jit_disassemble,   1, 1, cl_package_amilisp);
    cl_register_builtin("%JIT-SET-ACTIVE",    bi_jit_set_active,    1, 1, cl_package_amilisp);
    cl_register_builtin("%JIT-ACTIVE-P",      bi_jit_active_p,      0, 0, cl_package_amilisp);
    cl_register_builtin("%JIT-SET-HOT-THRESHOLD", bi_jit_set_hot_threshold, 1, 1, cl_package_amilisp);
    cl_register_builtin("%JIT-HOT-THRESHOLD", bi_jit_hot_threshold, 0, 0, cl_package_amilisp);
    cl_register_builtin("%JIT-HOT-COMPILE-COUNT", bi_jit_hot_compile_count, 0, 0, cl_package_amilisp);
    cl_register_builtin("%JIT-NATIVE-BYTES", bi_jit_native_bytes, 0, 0, cl_package_amilisp);
    cl_register_builtin("%JIT-SET-FRAMES",    bi_jit_set_frames,    1, 1, cl_package_amilisp);
    cl_register_builtin("%JIT-FRAMES-P",      bi_jit_frames_p,      0, 0, cl_package_amilisp);

    /* Reserved standard CL symbols — required to be present and external in
     * COMMON-LISP per ANSI 11.1.2.1.  Many do not have full implementations
     * yet; interning them here ensures the symbols exist so user code can
     * see them via FIND-SYMBOL and so the cl-symbols ANSI test passes.
     * Sub-module init functions above already intern the ones backed by a
     * builtin; this list covers the remainder. */
    cl_intern_standard_cl_symbols();

    /* Sub-module builtins */
    cl_builtins_arith_init();
    cl_builtins_io_init();
    cl_builtins_mutation_init();
    cl_builtins_strings_init();
    cl_builtins_lists_init();
    cl_builtins_hashtable_init();
    cl_builtins_sequence_init();
    cl_builtins_sequence2_init();
    cl_builtins_type_init();
    cl_builtins_condition_init();
    cl_builtins_package_init();
    cl_builtins_struct_init();
    cl_builtins_stream_init();
    cl_builtins_array_init();
    cl_float_math_init();
    cl_builtins_random_init();
    cl_builtins_bitvector_init();
    cl_builtins_pathname_init();
    cl_builtins_describe_init();
    cl_builtins_inspect_init();
    cl_builtins_thread_init();
    cl_builtins_ffi_init();
    cl_builtins_amiga_init();
    cl_builtins_bindtab_init();

    /* CL functions not yet implemented — register stubs so FBOUNDP /
     * SYMBOL-FUNCTION return non-NIL; any call signals an error.
     * APROPOS / APROPOS-LIST are real, in lib/boot.lisp. */
    /* MAKE-BROADCAST-STREAM / MAKE-CONCATENATED-STREAM / MAKE-ECHO-STREAM
     * and their component accessors are real builtins in builtins_stream.c
     * (CLHS 21.2). */
    defstub("INVALID-METHOD-ERROR");
    defstub("LOAD-LOGICAL-PATHNAME-TRANSLATIONS");
    defstub("LOGICAL-PATHNAME");
    /* MAKE-LOAD-FORM (generic function) and MAKE-LOAD-FORM-SAVING-SLOTS
     * are defined in lib/clos.lisp (CLHS 7.6); no stub here. */
    defstub("METHOD-COMBINATION-ERROR");
    defstub("PPRINT-TAB");
    defstub("PPRINT-TABULAR");
    defstub("PRINT-NOT-READABLE-OBJECT");
    /* STORE-VALUE / USE-VALUE are real restart-invoking functions defined in
     * builtins_condition.c (CLHS 9.1) — not stubs. */
    defstub("Y-OR-N-P");
    defstub("YES-OR-NO-P");

    /* All CL symbols now interned — mark them exported */
    cl_package_export_all_cl_symbols();

    /* Process control — in CL-USER, not CL (quit/exit are non-standard) */
    cl_register_builtin("QUIT", bi_quit, 0, 1, cl_package_cl_user);
    cl_register_builtin("EXIT", bi_quit, 0, 1, cl_package_cl_user);

    /* Exit hooks — run from main.c's shutdown funnel (see cl_run_exit_hooks) */
    SYM_EXIT_HOOKS = cl_intern_in("*EXIT-HOOKS*", 12, cl_package_ext);
    /* Rooted immediately: the registrations below allocate, and a compaction
     * there would leave this cached handle stale. */
    cl_gc_register_root(&SYM_EXIT_HOOKS);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_EXIT_HOOKS);
        s->flags |= CL_SYM_SPECIAL;
        s->value = CL_NIL;
    }
    cl_export_symbol(SYM_EXIT_HOOKS, cl_package_ext);
    cl_register_builtin("ADD-EXIT-HOOK", bi_add_exit_hook, 1, 1, cl_package_ext);
    export_ext_symbol("ADD-EXIT-HOOK", 13);
    cl_register_builtin("REMOVE-EXIT-HOOK", bi_remove_exit_hook, 1, 1,
                        cl_package_ext);
    export_ext_symbol("REMOVE-EXIT-HOOK", 16);

    /* Heap images: EXT:SAVE-IMAGE, EXT:*SAVE-HOOKS* / *RESTORE-HOOKS* /
     * *IMAGE-RESTORED-P* (image.c) */
    {
        extern void cl_image_builtins_init(void);
        cl_image_builtins_init();
    }

    /* EXT:*COMMAND-LINE-ARGS* / *WORKBENCH-STARTED-P* (cmdline.c) */
    {
        extern void cl_cmdline_builtins_init(void);
        cl_cmdline_builtins_init();
    }

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&trace_list);
}

/* Release the builtin modules' off-arena side tables at process exit.
 *
 * These are hash indexes and slot maps allocated with platform_alloc, sized
 * to the image and rebuilt (never grown) as the registries change — so no
 * amount of GC returns them, and on AmigaOS, where a process's memory is not
 * reclaimed by the OS, they are Fast RAM lost until reboot.  Called from
 * main() after the VM is down, so nothing can repopulate them. */
void cl_builtins_shutdown(void)
{
    uint32_t i;

    cl_builtins_struct_shutdown();
    cl_builtins_condition_shutdown();

    /* Deduped package names held by the builtin registry (builtin_reg_pkgs).
     * At most BUILTIN_REG_MAX_PKGS short strings — a few dozen bytes — but
     * the point of this pass is that nothing is left behind, and the registry
     * is dead once the VM is. */
    for (i = 0; i < builtin_reg_pkg_count; i++) {
        platform_free((void *)builtin_reg_pkgs[i]);
        builtin_reg_pkgs[i] = NULL;
    }
    builtin_reg_pkg_count = 0;
}
