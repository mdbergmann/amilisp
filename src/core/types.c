#include "types.h"
#include "mem.h"
#include "error.h"
#include "package.h"
#include "../platform/platform.h"
#include <stdio.h>

#ifdef DEBUG_GC
#include "vm.h"
#endif

CL_Obj CL_T = CL_NIL;  /* Set properly during init by symbol/package setup */

/*
 * AmigaOS version cookie — makes `Version amilisp` work from a Shell.
 *
 * The OS finds it by scanning the executable's bytes for "$VER: ", so it need
 * only be PRESENT, never referenced.  That is exactly what makes it fragile:
 * nothing in the program uses it, so the host build's -flto happily discards
 * it.  CL_USED forces the compiler to emit it anyway.  It lives here rather
 * than in main.c because main.o is not linked into the unit-test binaries.
 *
 * The build uses neither --gc-sections nor strip, so once emitted it survives
 * into the final executable — verified by test_version.c and, end to end, by
 * `strings build/host/amilisp | grep '$VER'`.
 */
CL_USED const char cl_version_cookie[] = CL_VERSION_TAG;

CL_Obj cl_car(CL_Obj obj)
{
    if (CL_NULL_P(obj)) return CL_NIL;
    if (!CL_HEAP_P(obj)) {
        if (obj == CL_UNBOUND)
            cl_error(CL_ERR_TYPE, "CAR: value is unbound (did you reference an uninitialized variable?)");
        cl_signal_type_error(obj, "LIST", "CAR");
    }
    if (obj >= cl_heap.arena_size)
        cl_storage_error("CAR: corrupted pointer 0x%08x (arena size 0x%08x)",
                         (unsigned)obj, (unsigned)cl_heap.arena_size);
    /* Heap object must actually be a cons.  Without this check (heap)
     * symbols, strings, vectors, conditions etc. were silently treated
     * as conses — (car 'a) would dereference the symbol struct as if
     * it were a cons and return whatever happened to live at offset 0.
     * CLHS requires car/cdr to signal type-error on non-list args. */
    if (CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) != TYPE_CONS)
        cl_signal_type_error(obj, "LIST", "CAR");
    return ((CL_Cons *)CL_OBJ_TO_PTR(obj))->car;
}

CL_Obj cl_cdr(CL_Obj obj)
{
    if (CL_NULL_P(obj)) return CL_NIL;
    if (!CL_HEAP_P(obj)) {
        if (obj == CL_UNBOUND)
            cl_error(CL_ERR_TYPE, "CDR: value is unbound (did you reference an uninitialized variable?)");
        cl_signal_type_error(obj, "LIST", "CDR");
    }
    if (obj >= cl_heap.arena_size)
        cl_storage_error("CDR: corrupted pointer 0x%08x (arena size 0x%08x)",
                         (unsigned)obj, (unsigned)cl_heap.arena_size);
    if (CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) != TYPE_CONS)
        cl_signal_type_error(obj, "LIST", "CDR");
    {
        CL_Obj result = ((CL_Cons *)CL_OBJ_TO_PTR(obj))->cdr;
#ifdef DEBUG_GC
        /* Detect use-after-free: if cdr is poison pattern, the cons was freed */
        if (result == 0xDEDEDEDEu) {
            char buf[512];
            CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(obj);
            int i;
            snprintf(buf, sizeof(buf),
                     "CDR-USE-AFTER-FREE: cons @0x%08x car=0x%08x cdr=POISON hdr=0x%08x\n"
                     "  VM sp=%d fp=%d\n",
                     (unsigned)obj, (unsigned)c->car, (unsigned)c->hdr.header,
                     cl_vm.sp, cl_vm.fp);
            platform_write_string(buf);
            for (i = 0; i < cl_vm.sp; i++) {
                if (cl_vm.stack[i] == obj) {
                    snprintf(buf, sizeof(buf), "  FOUND on VM stack[%d]\n", i);
                    platform_write_string(buf);
                }
            }
            for (i = 0; i < cl_vm.fp; i++) {
                snprintf(buf, sizeof(buf),
                         "  frame[%d] bp=%u n_locals=%d bytecode=0x%08x\n",
                         i, cl_vm.frames[i].bp, cl_vm.frames[i].n_locals,
                         (unsigned)cl_vm.frames[i].bytecode);
                platform_write_string(buf);
            }
            cl_capture_backtrace();
            platform_write_string(cl_backtrace_buf);
            platform_write_string("\n");
        }
#endif
        return result;
    }
}

/* cl_cons is implemented in mem.c (needs allocator) */

const char *cl_type_name(CL_Obj obj)
{
    if (CL_NULL_P(obj))    return "NULL";
    if (CL_FIXNUM_P(obj))  return "FIXNUM";
    if (CL_CHAR_P(obj))    return "CHARACTER";
    if (CL_HEAP_P(obj)) {
        switch (CL_HDR_TYPE(CL_OBJ_TO_PTR(obj))) {
        case TYPE_CONS:     return "CONS";
        case TYPE_SYMBOL:   return "SYMBOL";
        case TYPE_STRING:   return "STRING";
        case TYPE_FUNCTION: return "FUNCTION";
        case TYPE_CLOSURE:  return "FUNCTION";
        case TYPE_BYTECODE: return "COMPILED-FUNCTION";
        case TYPE_VECTOR:   return "VECTOR";
        case TYPE_PACKAGE:  return "PACKAGE";
        case TYPE_HASHTABLE: return "HASH-TABLE";
        case TYPE_CONDITION: return "CONDITION";
        case TYPE_STRUCT:   return "STRUCTURE";
        case TYPE_BIGNUM:   return "BIGNUM";
        case TYPE_SINGLE_FLOAT: return "SINGLE-FLOAT";
        case TYPE_DOUBLE_FLOAT: return "DOUBLE-FLOAT";
        case TYPE_RATIO:    return "RATIO";
        case TYPE_COMPLEX:  return "COMPLEX";
        case TYPE_STREAM:   return "STREAM";
        case TYPE_RANDOM_STATE: return "RANDOM-STATE";
        case TYPE_BIT_VECTOR: return "BIT-VECTOR";
        case TYPE_BYTE_VECTOR: return "BYTE-VECTOR";
        case TYPE_PATHNAME: return "PATHNAME";
        case TYPE_CELL:     return "CELL";
        case TYPE_THREAD:   return "THREAD";
        case TYPE_LOCK:     return "LOCK";
        case TYPE_CONDVAR:  return "CONDITION-VARIABLE";
        case TYPE_FOREIGN_POINTER: return "FOREIGN-POINTER";
        case TYPE_RESTART:  return "RESTART";
        case TYPE_FFI_STUB: return "FUNCTION";   /* a function for every CL predicate */
#ifdef CL_WIDE_STRINGS
        case TYPE_WIDE_STRING: return "STRING";
#endif
        default:            return "UNKNOWN";
        }
    }
    return "UNKNOWN";
}

/*
 * A short description of ANY 32-bit value for a diagnostic: the printed
 * form of a symbol, fixnum, character or short string, the type and raw
 * value of every other object, and an explicit marker for a word that is
 * not an object at all (an offset past the arena, the UNBOUND sentinel, a
 * raw word).  Allocates nothing and dereferences no offset it has not
 * bounds-checked against the arena, so it can be pointed at the very
 * garbage a corruption diagnostic is trying to describe -- the printer
 * cannot (it recurses into whatever it finds).  Answers buf.
 */
const char *cl_obj_brief(CL_Obj obj, char *buf, int bufsize)
{
    if (bufsize < 8) { if (bufsize > 0) buf[0] = '\0'; return buf; }
    if (CL_NULL_P(obj)) { strcpy(buf, "NIL"); return buf; }
    if (obj == CL_UNBOUND) { strcpy(buf, "#<unbound>"); return buf; }
    if (CL_FIXNUM_P(obj)) {
        snprintf(buf, bufsize, "%ld", (long)CL_FIXNUM_VAL(obj));
        return buf;
    }
    if (CL_CHAR_P(obj)) {
        int c = CL_CHAR_VAL(obj);
        if (c > 32 && c < 127) snprintf(buf, bufsize, "#\\%c", c);
        else                   snprintf(buf, bufsize, "#\\(code %d)", c);
        return buf;
    }
    if (!CL_HEAP_P(obj)) {
        snprintf(buf, bufsize, "#<raw 0x%08lx>", (unsigned long)obj);
        return buf;
    }
    /* limit - size, not obj + size: size_t is 32 bits on the Amiga, and a
     * raw word near 0xFFFFFFFF would wrap the sum past the check */
    if (obj > cl_heap.arena_size - sizeof(CL_Header)) {
        snprintf(buf, bufsize, "#<out of arena 0x%08lx>", (unsigned long)obj);
        return buf;
    }
    switch (CL_HDR_TYPE(CL_OBJ_TO_PTR(obj))) {
    case TYPE_SYMBOL: {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
        CL_Obj name = s->name;
        if (CL_HEAP_P(name) && name <= cl_heap.arena_size - sizeof(CL_String) &&
            CL_HDR_TYPE(CL_OBJ_TO_PTR(name)) == TYPE_STRING) {
            CL_String *str = (CL_String *)CL_OBJ_TO_PTR(name);
            int len = str->length > 64 ? 64 : (int)str->length;
            snprintf(buf, bufsize, "%s%.*s",
                     s->package == cl_package_keyword ? ":" : "", len, str->data);
        } else {
            snprintf(buf, bufsize, "#<SYMBOL 0x%08lx, name 0x%08lx>",
                     (unsigned long)obj, (unsigned long)name);
        }
        return buf;
    }
    case TYPE_STRING: {
        CL_String *str = (CL_String *)CL_OBJ_TO_PTR(obj);
        int len = str->length > 32 ? 32 : (int)str->length;
        snprintf(buf, bufsize, "\"%.*s%s\"", len, str->data,
                 str->length > 32 ? "..." : "");
        return buf;
    }
    default:
        break;
    }
    if (strcmp(cl_type_name(obj), "UNKNOWN") == 0)
        snprintf(buf, bufsize, "#<type %u 0x%08lx>",
                 (unsigned)CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)), (unsigned long)obj);
    else
        snprintf(buf, bufsize, "#<%s 0x%08lx>", cl_type_name(obj), (unsigned long)obj);
    return buf;
}

/* The arguments of a call, each through cl_obj_brief, space-separated and
 * cut off with "..." once the buffer is nearly full.  For the messages of
 * an arity / keyword / callee check: seeing the whole argument list is what
 * tells a shifted stack (an argument missing at the front, the callee
 * itself among the arguments) from a single bad value. */
const char *cl_args_brief(CL_Obj *args, int nargs, char *buf, int bufsize)
{
    int i, pos = 0;
    if (bufsize < 8) { if (bufsize > 0) buf[0] = '\0'; return buf; }
    buf[0] = '\0';
    for (i = 0; i < nargs; i++) {
        char one[96];
        int n;
        cl_obj_brief(args[i], one, sizeof(one));
        n = snprintf(buf + pos, bufsize - pos, "%s%s", i ? " " : "", one);
        if (n < 0 || pos + n >= bufsize - 4) {
            /* out of room: end with "..." */
            if (pos > bufsize - 5) pos = bufsize - 5;
            strcpy(buf + pos, " ...");
            break;
        }
        pos += n;
    }
    return buf;
}
