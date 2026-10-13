/*
 * builtins_describe.c — DESCRIBE function
 *
 * (describe object &optional stream) — prints descriptive information
 * about an object to a stream (default *standard-output*), returns no values.
 */

#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "gc_backend.h"
#include "error.h"
#include "printer.h"
#include "stream.h"
#include "string_utils.h"
#include "vm.h"
#include "float.h"
#include "compiler.h"
#include "../platform/platform.h"
#include <string.h>
#include <stdio.h>

/* --- Helpers --- */

static void write_str(CL_Obj stream, const char *str)
{
    cl_stream_write_string(stream, str, (uint32_t)strlen(str));
}

static void write_nl(CL_Obj stream)
{
    cl_stream_write_char(stream, '\n');
}

static void write_line(CL_Obj stream, const char *str)
{
    write_str(stream, str);
    write_nl(stream);
}

/* Print obj representation (prin1 style) to stream */
static void write_obj(CL_Obj stream, CL_Obj obj)
{
    char buf[256];
    cl_prin1_to_string(obj, buf, sizeof(buf));
    write_str(stream, buf);
}


/* Apply the Lisp function named NAME in PACKAGE to ARGS, or return CL_NIL
 * when it is not fbound -- DESCRIBE also runs during boot, before
 * DOCUMENTATION (boot.lisp) and GENERIC-FUNCTION-LAMBDA-LIST (clos.lisp)
 * exist.  A generic function is unwrapped to its discriminating function,
 * which is what cl_vm_apply can run.  The caller protects ARGS. */
static CL_Obj call_lisp(const char *name, CL_Obj package, CL_Obj *args, int n)
{
    CL_Obj sym = cl_find_symbol(name, (uint32_t)strlen(name), package);
    CL_Obj fn;
    if (CL_NULL_P(sym) || !CL_SYMBOL_P(sym)) return CL_NIL;
    fn = ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->function;
    if (fn == CL_UNBOUND || CL_NULL_P(fn)) return CL_NIL;
    fn = cl_unwrap_funcallable(fn);
    return cl_vm_apply(fn, args, n);
}

/* "  LABEL: <doc>" when (DOCUMENTATION NAME 'DOC-TYPE) is a string. */
static void describe_documentation(CL_Obj stream, CL_Obj name,
                                   const char *doc_type, const char *label)
{
    CL_Obj args[2];
    CL_Obj doc;
    args[0] = name;
    args[1] = cl_intern_in(doc_type, (uint32_t)strlen(doc_type), cl_package_cl);
    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(args[0]);
    CL_GC_PROTECT(args[1]);
    doc = call_lisp("DOCUMENTATION", cl_package_cl, args, 2);
    if (CL_ANY_STRING_P(doc)) {
        CL_GC_PROTECT(doc);
        write_str(stream, label);
        cl_princ_to_stream(doc, stream);
        write_nl(stream);
        CL_GC_UNPROTECT(1);
    }
    CL_GC_UNPROTECT(3);
}

/* "  Lambda-list: (...)" for a function object -- the list as written for
 * compiled code (EXT:FUNCTION-ARGLIST), the GF's own for a generic
 * function -- and "  Source: file:line" when the compiler recorded one. */
static void describe_lambda_list(CL_Obj stream, CL_Obj fn)
{
    CL_Obj ll, loc;
    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(fn);
    if (cl_funcallable_instance_p(fn)) {
        CL_Obj args[1];
        args[0] = fn;
        CL_GC_PROTECT(args[0]);
        ll = call_lisp("GENERIC-FUNCTION-LAMBDA-LIST", cl_package_mop, args, 1);
        CL_GC_UNPROTECT(1);
    } else {
        ll = cl_function_arglist(fn);
    }
    /* :NOT-AVAILABLE is a symbol; a real lambda list is NIL or a cons. */
    if (CL_NULL_P(ll) || CL_CONS_P(ll)) {
        CL_GC_PROTECT(ll);
        write_str(stream, "  Lambda-list: ");
        if (CL_NULL_P(ll))
            write_str(stream, "()");
        else
            cl_prin1_to_stream(ll, stream);
        write_nl(stream);
        CL_GC_UNPROTECT(1);
    }
    loc = cl_function_source_location(fn);
    if (CL_CONS_P(loc)) {
        CL_GC_PROTECT(loc);
        write_str(stream, "  Source: ");
        cl_princ_to_stream(cl_car(loc), stream);
        /* Lines are 1-based; 0 means the file is known but not the line
         * (a body compiled at load time from a FASL). */
        if (CL_CONS_P(cl_cdr(loc)) && CL_FIXNUM_P(cl_car(cl_cdr(loc))) &&
            CL_FIXNUM_VAL(cl_car(cl_cdr(loc))) > 0) {
            write_str(stream, ":");
            cl_princ_to_stream(cl_car(cl_cdr(loc)), stream);
        }
        write_nl(stream);
        CL_GC_UNPROTECT(1);
    }
    CL_GC_UNPROTECT(2);
}

static CL_Obj resolve_stream(CL_Obj *args, int n)
{
    if (n >= 2 && !CL_NULL_P(args[1])) {
        if (args[1] == CL_T)
            return cl_symbol_value(SYM_STANDARD_OUTPUT);
        return args[1];
    }
    return cl_symbol_value(SYM_STANDARD_OUTPUT);
}

/* Count length of a proper list. Returns -1 if dotted/circular. */
static int list_length(CL_Obj obj)
{
    int len = 0;
    CL_Obj slow = obj, fast = obj;
    while (!CL_NULL_P(fast)) {
        if (!CL_CONS_P(fast)) return -1;
        fast = cl_cdr(fast);
        len++;
        if (CL_NULL_P(fast)) break;
        if (!CL_CONS_P(fast)) return -1;
        fast = cl_cdr(fast);
        len++;
        slow = cl_cdr(slow);
        if (slow == fast) return -1; /* circular */
    }
    return len;
}

/* --- Type-specific describers ---
 *
 * GC discipline (audit 2026-07 tier 3): every write_* helper can allocate —
 * printing an element may dispatch a PRINT-OBJECT hook via cl_vm_apply, and
 * the stream itself may be a Gray stream applying Lisp methods.  So each
 * describer (a) protects its stream and obj params for the duration (the
 * local copies go stale after the first allocating write otherwise), (b)
 * snapshots scalar fields into C locals BEFORE the first write, and (c)
 * keeps heap-object fields in protected CL_Obj locals rather than re-reading
 * them through a raw pointer captured up front. */

static void describe_nil(CL_Obj stream)
{
    CL_GC_PROTECT(stream);
    write_line(stream, "NIL is the symbol NIL");
    write_line(stream, "  It is also the empty list and the boolean false.");
    write_line(stream, "  Value: NIL");
    write_line(stream, "  Type: NULL");
    CL_GC_UNPROTECT(1);
}

static void describe_fixnum(CL_Obj obj, CL_Obj stream)
{
    char buf[64];
    int32_t val = CL_FIXNUM_VAL(obj);
    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a FIXNUM");
    snprintf(buf, sizeof(buf), "  Value: %d", (int)val);
    write_line(stream, buf);
    CL_GC_UNPROTECT(1);
}

static void describe_character(CL_Obj obj, CL_Obj stream)
{
    char buf[64];
    int ch = CL_CHAR_VAL(obj);
    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a CHARACTER");
    if (ch >= 32 && ch < 127) {
        snprintf(buf, sizeof(buf), "  Char: '%c'", (char)ch);
        write_line(stream, buf);
    }
    snprintf(buf, sizeof(buf), "  Char-code: %d", ch);
    write_line(stream, buf);
    CL_GC_UNPROTECT(1);
}

static void describe_symbol(CL_Obj obj, CL_Obj stream)
{
    CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
    CL_Obj name = sym->name;
    CL_Obj pkgname = CL_NULL_P(sym->package)
                     ? CL_NIL
                     : ((CL_Package *)CL_OBJ_TO_PTR(sym->package))->name;
    CL_Obj fn = sym->function;
    CL_Obj plist = sym->plist;
    uint32_t flags = sym->flags;
    int have_pkg = !CL_NULL_P(sym->package);

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(obj);
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(pkgname);
    CL_GC_PROTECT(fn);
    CL_GC_PROTECT(plist);

    write_obj(stream, obj);
    write_line(stream, " is a SYMBOL");

    write_str(stream, "  Name: \"");
    if (cl_string_is_base(name)) {
        /* Arena-safe chunked write (see cl_stream_write_lisp_string). */
        cl_stream_write_lisp_string(stream, name, 0,
            ((CL_String *)CL_OBJ_TO_PTR(name))->length);
    } else {
        write_obj(stream, name);
    }
    write_line(stream, "\"");

    if (have_pkg) {
        write_str(stream, "  Package: ");
        write_obj(stream, pkgname);
        write_nl(stream);
    } else {
        write_line(stream, "  Package: NIL (uninterned)");
    }

    {
        CL_Obj sv = cl_symbol_value(obj);
        if (sv != CL_UNBOUND) {
            write_str(stream, "  Value: ");
            write_obj(stream, sv);
            write_nl(stream);
        } else {
            write_line(stream, "  Value: <unbound>");
        }
    }

    if (fn != CL_UNBOUND) {
        write_str(stream, "  Function: ");
        write_obj(stream, fn);
        write_nl(stream);
        describe_lambda_list(stream, fn);
    }

    /* A macro's expander lives in the macro table, not the function cell. */
    {
        CL_Obj macro = cl_get_macro(obj);
        if (!CL_NULL_P(macro)) {
            CL_GC_PROTECT(macro);
            write_str(stream, "  Macro: ");
            write_obj(stream, macro);
            write_nl(stream);
            describe_lambda_list(stream, macro);
            CL_GC_UNPROTECT(1);
        }
    }

    describe_documentation(stream, obj, "FUNCTION", "  Documentation: ");
    describe_documentation(stream, obj, "VARIABLE", "  Variable documentation: ");
    describe_documentation(stream, obj, "TYPE", "  Type documentation: ");
    describe_documentation(stream, obj, "STRUCTURE", "  Structure documentation: ");

    if (!CL_NULL_P(plist)) {
        write_str(stream, "  Plist: ");
        write_obj(stream, plist);
        write_nl(stream);
    }

    /* Flags */
    if (flags) {
        write_str(stream, "  Flags:");
        if (flags & CL_SYM_SPECIAL)  write_str(stream, " SPECIAL");
        if (flags & CL_SYM_CONSTANT) write_str(stream, " CONSTANT");
        if (flags & CL_SYM_EXPORTED) write_str(stream, " EXPORTED");
        if (flags & CL_SYM_TRACED)   write_str(stream, " TRACED");
        if (flags & CL_SYM_INLINE)   write_str(stream, " INLINE");
        write_nl(stream);
    }
    CL_GC_UNPROTECT(6);
}

static void describe_cons(CL_Obj obj, CL_Obj stream)
{
    int len;

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(obj);

    write_obj(stream, obj);
    write_line(stream, " is a CONS");

    len = list_length(obj);
    if (len >= 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "  Length: %d", len);
        write_line(stream, buf);
    } else {
        write_line(stream, "  (dotted or circular list)");
    }

    write_str(stream, "  Car: ");
    write_obj(stream, cl_car(obj));
    write_nl(stream);
    write_str(stream, "  Cdr: ");
    write_obj(stream, cl_cdr(obj));
    write_nl(stream);
    CL_GC_UNPROTECT(2);
}

static void describe_string(CL_Obj obj, CL_Obj stream)
{
    uint32_t len = ((CL_String *)CL_OBJ_TO_PTR(obj))->length;
    char buf[64];

    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a STRING");
    snprintf(buf, sizeof(buf), "  Length: %u", (unsigned)len);
    write_line(stream, buf);
    CL_GC_UNPROTECT(1);
}

static void describe_function(CL_Obj obj, CL_Obj stream)
{
    CL_Function *fn = (CL_Function *)CL_OBJ_TO_PTR(obj);
    CL_Obj fname = fn->name;
    int min_args = fn->min_args;
    int max_args = fn->max_args;
    char buf[64];

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(fname);

    write_obj(stream, obj);
    write_line(stream, " is a FUNCTION");
    write_line(stream, "  Type: built-in");

    write_str(stream, "  Name: ");
    write_obj(stream, fname);
    write_nl(stream);

    snprintf(buf, sizeof(buf), "  Min-args: %d", min_args);
    write_line(stream, buf);
    if (max_args < 0) {
        write_line(stream, "  Max-args: variable");
    } else {
        snprintf(buf, sizeof(buf), "  Max-args: %d", max_args);
        write_line(stream, buf);
    }
    CL_GC_UNPROTECT(2);
}

/* FFI binding descriptor (types.h CL_FfiStub): print what it binds.
 * Fields are copied out before the first write (the stream writes can
 * allocate and move the stub). */
static void describe_ffi_stub(CL_Obj obj, CL_Obj stream)
{
    static const char *const ct_names[CL_STUB_CT_MAX + 1] = {
        ":U8", ":I8", ":U16", ":I16", ":U32", ":I32",
        ":POINTER", ":FPTR", ":SINGLE", ":DOUBLE" };
    static const char *const res_names[CL_AMIGA_RES_KIND_MAX + 1] = {
        ":UNSIGNED", ":VOID", ":POINTER", ":SIGNED", ":BOOL",
        ":U16", ":I16", ":U8", ":I8" };
    CL_FfiStub *fs = (CL_FfiStub *)CL_OBJ_TO_PTR(obj);
    CL_Obj fname = fs->name, aux = fs->aux;
    uint32_t a = fs->a;
    int16_t b = fs->b;
    uint8_t kind = fs->kind, ctype = fs->ctype;
    char buf[96];
    int i;

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(fname);
    CL_GC_PROTECT(aux);

    write_obj(stream, obj);
    write_line(stream, " is a FUNCTION");
    write_line(stream, "  Type: FFI stub (binding descriptor)");
    write_str(stream, "  Name: ");
    write_obj(stream, fname);
    write_nl(stream);

    if (kind == CL_STUB_LIBCALL) {
        int rk = CL_AMIGA_RES_KIND(a);
        write_line(stream, "  Kind: AmigaOS library call");
        write_str(stream, "  Library base: ");
        write_obj(stream, aux);
        write_nl(stream);
        snprintf(buf, sizeof(buf), "  LVO: %d", (int)b);
        write_line(stream, buf);
        snprintf(buf, sizeof(buf), "  Arguments: %u (registers:", (unsigned)ctype);
        write_str(stream, buf);
        for (i = 0; i < (int)ctype && i < 7; i++) {
            int reg = (int)((a >> (i * 4)) & 0xF);
            snprintf(buf, sizeof(buf), " %c%d", reg < 8 ? 'D' : 'A',
                     reg < 8 ? reg : reg - 8);
            write_str(stream, buf);
        }
        write_line(stream, ")");
        snprintf(buf, sizeof(buf), "  Result: %s",
                 rk <= CL_AMIGA_RES_KIND_MAX ? res_names[rk] : "?");
        write_line(stream, buf);
    } else {
        const char *ct = ctype <= CL_STUB_CT_MAX ? ct_names[ctype] : "?";
        switch (kind) {
        case CL_STUB_PEEK:
            snprintf(buf, sizeof(buf), "  Kind: struct field reader, %s at offset %u",
                     ct, (unsigned)a);
            break;
        case CL_STUB_POKE:
            snprintf(buf, sizeof(buf), "  Kind: struct field writer, %s at offset %u",
                     ct, (unsigned)a);
            break;
        case CL_STUB_PEEK_IDX:
            snprintf(buf, sizeof(buf),
                     "  Kind: array field reader, %s at offset %u, %d-byte elements, count %u",
                     ct, (unsigned)(a & 0xFFFFu), (int)b, (unsigned)(a >> 16));
            break;
        case CL_STUB_POKE_IDX:
            snprintf(buf, sizeof(buf),
                     "  Kind: array field writer, %s at offset %u, %d-byte elements, count %u",
                     ct, (unsigned)(a & 0xFFFFu), (int)b, (unsigned)(a >> 16));
            break;
        case CL_STUB_FIELD_PTR:
            snprintf(buf, sizeof(buf), "  Kind: embedded struct pointer at offset %u",
                     (unsigned)a);
            break;
        default:
            snprintf(buf, sizeof(buf), "  Kind: ? (%u)", (unsigned)kind);
            break;
        }
        write_line(stream, buf);
    }
    CL_GC_UNPROTECT(3);
}

static void describe_closure(CL_Obj obj, CL_Obj stream)
{
    CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(obj);
    CL_Obj bc_name = CL_NIL;
    uint16_t arity = 0;
    uint16_t n_upvalues = 0;
    int have_bc = 0;
    int have_ll = 0;
    char buf[64];

    if (CL_BYTECODE_P(cl->bytecode)) {
        CL_Bytecode *bc = (CL_Bytecode *)CL_OBJ_TO_PTR(cl->bytecode);
        bc_name = bc->name;
        arity = bc->arity;
        n_upvalues = bc->n_upvalues;
        have_ll = !CL_NULL_P(bc->source_lambda_list);
        have_bc = 1;
    }

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(bc_name);
    CL_GC_PROTECT(obj);

    write_obj(stream, obj);
    write_line(stream, " is a COMPILED-FUNCTION");

    if (have_bc) {
        if (!CL_NULL_P(bc_name)) {
            write_str(stream, "  Name: ");
            write_obj(stream, bc_name);
            write_nl(stream);
        }
        /* The lambda list as written, when the compiler captured it (and
         * the source file:line); the bare arity otherwise. */
        if (have_ll) {
            describe_lambda_list(stream, obj);
        } else {
            snprintf(buf, sizeof(buf), "  Arity: %d", (int)(arity & 0x7FFF));
            write_line(stream, buf);
            if (arity & 0x8000)
                write_line(stream, "  Has &rest parameter");
        }
        if (n_upvalues > 0) {
            snprintf(buf, sizeof(buf), "  Upvalues: %u", (unsigned)n_upvalues);
            write_line(stream, buf);
        }
        if (CL_SYMBOL_P(bc_name))
            describe_documentation(stream, bc_name, "FUNCTION", "  Documentation: ");
    }
    CL_GC_UNPROTECT(3);
}

static void describe_vector(CL_Obj obj, CL_Obj stream)
{
    CL_Vector *vec = (CL_Vector *)CL_OBJ_TO_PTR(obj);
    char buf[64];
    uint32_t length = vec->length;
    uint32_t fill_pointer = vec->fill_pointer;
    uint32_t vflags = vec->flags;
    uint32_t active_len = cl_vector_active_length(vec);
    uint32_t show, i;

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(obj);

    write_obj(stream, obj);
    write_line(stream, " is a VECTOR");
    snprintf(buf, sizeof(buf), "  Length: %u", (unsigned)length);
    write_line(stream, buf);

    if (fill_pointer != CL_NO_FILL_POINTER) {
        snprintf(buf, sizeof(buf), "  Fill-pointer: %u", (unsigned)fill_pointer);
        write_line(stream, buf);
    }
    if (vflags & CL_VEC_FLAG_ADJUSTABLE)
        write_line(stream, "  Adjustable: T");

    show = active_len > 5 ? 5 : active_len;
    if (show > 0) {
        write_str(stream, "  Elements:");
        for (i = 0; i < show; i++) {
            /* re-derive the data pointer per element — each write above
             * can compact */
            CL_Obj elt =
                cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(obj))[i];
            write_str(stream, " ");
            write_obj(stream, elt);
        }
        if (active_len > 5)
            write_str(stream, " ...");
        write_nl(stream);
    }
    CL_GC_UNPROTECT(2);
}

static void describe_package(CL_Obj obj, CL_Obj stream)
{
    CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(obj);
    CL_Obj name = pkg->name;
    CL_Obj nicknames = pkg->nicknames;
    CL_Obj use_list = pkg->use_list;
    uint32_t sym_count = pkg->sym_count;
    char buf[64];

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(nicknames);
    CL_GC_PROTECT(use_list);

    write_obj(stream, obj);
    write_line(stream, " is a PACKAGE");

    write_str(stream, "  Name: ");
    write_obj(stream, name);
    write_nl(stream);

    if (!CL_NULL_P(nicknames)) {
        write_str(stream, "  Nicknames: ");
        write_obj(stream, nicknames);
        write_nl(stream);
    }

    if (!CL_NULL_P(use_list)) {
        write_str(stream, "  Use-list: ");
        write_obj(stream, use_list);
        write_nl(stream);
    }

    snprintf(buf, sizeof(buf), "  Symbol-count: %u", (unsigned)sym_count);
    write_line(stream, buf);
    CL_GC_UNPROTECT(4);
}

static void describe_hashtable(CL_Obj obj, CL_Obj stream)
{
    CL_Hashtable *ht = (CL_Hashtable *)CL_OBJ_TO_PTR(obj);
    uint32_t count = ht->count;
    uint32_t bucket_count = ht->bucket_count;
    uint8_t test = ht->test;
    char buf[64];
    const char *test_name;

    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a HASH-TABLE");

    switch (test) {
        case CL_HT_TEST_EQ:     test_name = "EQ";     break;
        case CL_HT_TEST_EQL:    test_name = "EQL";    break;
        case CL_HT_TEST_EQUAL:  test_name = "EQUAL";  break;
        case CL_HT_TEST_EQUALP: test_name = "EQUALP"; break;
        default:                test_name = "?";     break;
    }
    snprintf(buf, sizeof(buf), "  Test: %s", test_name);
    write_line(stream, buf);
    snprintf(buf, sizeof(buf), "  Count: %u", (unsigned)count);
    write_line(stream, buf);
    snprintf(buf, sizeof(buf), "  Size: %u", (unsigned)bucket_count);
    write_line(stream, buf);
    CL_GC_UNPROTECT(1);
}

static void describe_struct(CL_Obj obj, CL_Obj stream)
{
    CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    CL_Obj type_desc = st->type_desc;
    uint32_t n_slots = st->n_slots;
    char buf[64];

    extern CL_Obj cl_struct_slot_names(CL_Obj type_name);

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(obj);
    CL_GC_PROTECT(type_desc);

    /* Printing the struct itself consults the PRINT-OBJECT hook — st is
     * stale after this first write already. */
    write_obj(stream, obj);
    write_line(stream, " is a STRUCTURE");

    write_str(stream, "  Type: ");
    write_obj(stream, type_desc);
    write_nl(stream);

    snprintf(buf, sizeof(buf), "  Slots: %u", (unsigned)n_slots);
    write_line(stream, buf);

    /* Show slot names and values — cl_struct_slot_names allocates, and
     * every element print can too: keep the name cursor rooted and read
     * each slot through a freshly derived pointer. */
    {
        CL_Obj names = cl_struct_slot_names(type_desc);
        uint32_t i = 0;
        CL_GC_PROTECT(names);
        while (!CL_NULL_P(names) && i < n_slots) {
            write_str(stream, "  ");
            write_obj(stream, cl_car(names));
            write_str(stream, ": ");
            write_obj(stream, ((CL_Struct *)CL_OBJ_TO_PTR(obj))->slots[i]);
            write_nl(stream);
            names = cl_cdr(names);
            i++;
        }
        CL_GC_UNPROTECT(1);
    }
    CL_GC_UNPROTECT(3);
}

static void describe_condition(CL_Obj obj, CL_Obj stream)
{
    CL_Condition *cond = (CL_Condition *)CL_OBJ_TO_PTR(obj);
    CL_Obj type_name = cond->type_name;
    CL_Obj report_string = cond->report_string;
    CL_Obj slots = cond->slots;

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(type_name);
    CL_GC_PROTECT(report_string);
    CL_GC_PROTECT(slots);

    write_obj(stream, obj);
    write_line(stream, " is a CONDITION");

    write_str(stream, "  Type: ");
    write_obj(stream, type_name);
    write_nl(stream);

    if (!CL_NULL_P(report_string)) {
        write_str(stream, "  Report: ");
        write_obj(stream, report_string);
        write_nl(stream);
    }

    if (!CL_NULL_P(slots)) {
        write_str(stream, "  Slots: ");
        write_obj(stream, slots);
        write_nl(stream);
    }
    CL_GC_UNPROTECT(4);
}

static void describe_bignum(CL_Obj obj, CL_Obj stream)
{
    CL_Bignum *bn = (CL_Bignum *)CL_OBJ_TO_PTR(obj);
    uint16_t sign = bn->sign;
    uint16_t length = bn->length;
    char buf[64];

    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a BIGNUM");

    snprintf(buf, sizeof(buf), "  Sign: %s", sign ? "negative" : "positive");
    write_line(stream, buf);
    snprintf(buf, sizeof(buf), "  Limbs: %u", (unsigned)length);
    write_line(stream, buf);
    CL_GC_UNPROTECT(1);
}

static void describe_single_float(CL_Obj obj, CL_Obj stream)
{
    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a SINGLE-FLOAT");
    CL_GC_UNPROTECT(1);
}

static void describe_double_float(CL_Obj obj, CL_Obj stream)
{
    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a DOUBLE-FLOAT");
    CL_GC_UNPROTECT(1);
}

static void describe_ratio(CL_Obj obj, CL_Obj stream)
{
    CL_Ratio *r = (CL_Ratio *)CL_OBJ_TO_PTR(obj);
    CL_Obj num = r->numerator;
    CL_Obj den = r->denominator;

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(num);
    CL_GC_PROTECT(den);

    write_obj(stream, obj);
    write_line(stream, " is a RATIO");

    write_str(stream, "  Numerator: ");
    write_obj(stream, num);
    write_nl(stream);
    write_str(stream, "  Denominator: ");
    write_obj(stream, den);
    write_nl(stream);
    CL_GC_UNPROTECT(3);
}

static void describe_stream(CL_Obj obj, CL_Obj stream)
{
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(obj);
    uint8_t direction = st->direction;
    uint8_t stream_type = st->stream_type;
    uint32_t sflags = st->flags;
    const char *dir;
    const char *stype;

    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a STREAM");

    switch (direction) {
        case CL_STREAM_INPUT:  dir = "INPUT";  break;
        case CL_STREAM_OUTPUT: dir = "OUTPUT"; break;
        case CL_STREAM_IO:     dir = "IO";     break;
        default:               dir = "?";      break;
    }
    write_str(stream, "  Direction: ");
    write_line(stream, dir);

    switch (stream_type) {
        case CL_STREAM_CONSOLE: stype = "CONSOLE"; break;
        case CL_STREAM_FILE:    stype = "FILE";    break;
        case CL_STREAM_STRING:  stype = "STRING";  break;
        default:                stype = "?";       break;
    }
    write_str(stream, "  Type: ");
    write_line(stream, stype);

    write_str(stream, "  Open: ");
    write_line(stream, (sflags & CL_STREAM_FLAG_OPEN) ? "T" : "NIL");
    CL_GC_UNPROTECT(1);
}

static void describe_random_state(CL_Obj obj, CL_Obj stream)
{
    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a RANDOM-STATE");
    CL_GC_UNPROTECT(1);
}

static void describe_bit_vector(CL_Obj obj, CL_Obj stream)
{
    uint32_t length = ((CL_BitVector *)CL_OBJ_TO_PTR(obj))->length;
    char buf[64];

    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is a BIT-VECTOR");

    snprintf(buf, sizeof(buf), "  Length: %u", (unsigned)length);
    write_line(stream, buf);
    CL_GC_UNPROTECT(1);
}

static void describe_pathname(CL_Obj obj, CL_Obj stream)
{
    CL_Pathname *pn = (CL_Pathname *)CL_OBJ_TO_PTR(obj);
    CL_Obj host = pn->host;
    CL_Obj device = pn->device;
    CL_Obj directory = pn->directory;
    CL_Obj name = pn->name;
    CL_Obj type = pn->type;
    CL_Obj version = pn->version;

    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(host);
    CL_GC_PROTECT(device);
    CL_GC_PROTECT(directory);
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(type);
    CL_GC_PROTECT(version);

    write_obj(stream, obj);
    write_line(stream, " is a PATHNAME");

    write_str(stream, "  Host: ");
    write_obj(stream, host);
    write_nl(stream);
    write_str(stream, "  Device: ");
    write_obj(stream, device);
    write_nl(stream);
    write_str(stream, "  Directory: ");
    write_obj(stream, directory);
    write_nl(stream);
    write_str(stream, "  Name: ");
    write_obj(stream, name);
    write_nl(stream);
    write_str(stream, "  Type: ");
    write_obj(stream, type);
    write_nl(stream);
    write_str(stream, "  Version: ");
    write_obj(stream, version);
    write_nl(stream);
    CL_GC_UNPROTECT(7);
}

/* --- Main dispatch --- */

void cl_describe_to_stream(CL_Obj obj, CL_Obj stream)
{
    /* NIL */
    if (CL_NULL_P(obj)) {
        describe_nil(stream);
        return;
    }

    /* Fixnum */
    if (CL_FIXNUM_P(obj)) {
        describe_fixnum(obj, stream);
        return;
    }

    /* Character */
    if (CL_CHAR_P(obj)) {
        describe_character(obj, stream);
        return;
    }

    /* Heap objects — use if-else to avoid m68k-amigaos-gcc LTO
     * jump table bug (undefined .L labels during link) */
    if (CL_SYMBOL_P(obj))       { describe_symbol(obj, stream);       return; }
    if (CL_CONS_P(obj))         { describe_cons(obj, stream);         return; }
    if (CL_STRING_P(obj))       { describe_string(obj, stream);       return; }
    if (CL_FUNCTION_P(obj))     { describe_function(obj, stream);     return; }
    if (CL_FFI_STUB_P(obj))     { describe_ffi_stub(obj, stream);     return; }
    if (CL_CLOSURE_P(obj))      { describe_closure(obj, stream);      return; }
    if (CL_VECTOR_P(obj))       { describe_vector(obj, stream);       return; }
    if (CL_PACKAGE_P(obj))      { describe_package(obj, stream);      return; }
    if (CL_HASHTABLE_P(obj))    { describe_hashtable(obj, stream);    return; }
    if (CL_STRUCT_P(obj))       { describe_struct(obj, stream);       return; }
    if (CL_CONDITION_P(obj))    { describe_condition(obj, stream);    return; }
    if (CL_BIGNUM_P(obj))       { describe_bignum(obj, stream);       return; }
    if (CL_SINGLE_FLOAT_P(obj)) { describe_single_float(obj, stream); return; }
    if (CL_DOUBLE_FLOAT_P(obj)) { describe_double_float(obj, stream); return; }
    if (CL_RATIO_P(obj))        { describe_ratio(obj, stream);        return; }
    if (CL_STREAM_P(obj))       { describe_stream(obj, stream);       return; }
    if (CL_RANDOM_STATE_P(obj)) { describe_random_state(obj, stream); return; }
    if (CL_BIT_VECTOR_P(obj))   { describe_bit_vector(obj, stream);   return; }
    if (CL_PATHNAME_P(obj))     { describe_pathname(obj, stream);     return; }

    /* Fallback */
    CL_GC_PROTECT(stream);
    write_obj(stream, obj);
    write_line(stream, " is an unknown object");
    CL_GC_UNPROTECT(1);
}

/* (describe object &optional stream) */
static CL_Obj bi_describe(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    CL_Obj stream = resolve_stream(args, n);

    cl_describe_to_stream(obj, stream);

    /* describe returns no values */
    cl_mv_count = 0;
    return CL_NIL;
}

/* (room &optional verbosity)
   verbosity: :default (or omitted), :minimal, :full
   Prints heap usage, GC stats, and allocation info. */
static CL_Obj bi_room(CL_Obj *args, int n)
{
    char buf[256];
    uint32_t used = cl_heap.total_allocated;
    uint32_t total = cl_heap.arena_size;
    uint32_t free_bytes = total - used;
    uint32_t pct = (used * 100) / total;

    (void)args; (void)n;

    cl_write_cstring_to_stdout("Heap:\n");
    sprintf(buf, "  %lu / %lu bytes used (%lu%%)\n",
            (unsigned long)used, (unsigned long)total, (unsigned long)pct);
    cl_write_cstring_to_stdout(buf);
    sprintf(buf, "  %lu bytes free\n", (unsigned long)free_bytes);
    cl_write_cstring_to_stdout(buf);
    sprintf(buf, "  %lu bytes consed (total ever allocated)\n",
            (unsigned long)cl_heap.total_consed);
    cl_write_cstring_to_stdout(buf);
    sprintf(buf, "GC:\n  %lu collections\n  %lu compactions\n",
            (unsigned long)cl_heap.gc_count,
            (unsigned long)cl_heap.compact_count);
    cl_write_cstring_to_stdout(buf);
    {
        uint32_t ms_cap, ms_grows, ms_rescans;
        cl_gc_mark_stack_stats(&ms_cap, &ms_grows, &ms_rescans);
        sprintf(buf, "  mark stack: %lu entries, %lu grows, "
                "%lu overflow re-scan passes\n",
                (unsigned long)ms_cap, (unsigned long)ms_grows,
                (unsigned long)ms_rescans);
        cl_write_cstring_to_stdout(buf);
    }
    /* The collector's own lines (gc_backend.h): the classic free list's
     * shape and probe count, or the Immix line map and hole statistics. */
    gcb_stats_report(cl_write_cstring_to_stdout);

    cl_mv_count = 0;
    return CL_NIL;
}

/* --- Registration --- */

void cl_builtins_describe_init(void)
{
    defun("DESCRIBE", bi_describe, 1, 2);
    defun("ROOM", bi_room, 0, 1);
}
