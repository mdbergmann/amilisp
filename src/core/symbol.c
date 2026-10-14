#include "symbol.h"
#include "package.h"
#include "bindtab.h"
#include "mem.h"
#include "thread.h"
#include "error.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>

/* Well-known symbols */
CL_Obj SYM_T = CL_NIL;
CL_Obj SYM_NIL = CL_NIL;
CL_Obj SYM_QUOTE = CL_NIL;
CL_Obj SYM_IF = CL_NIL;
CL_Obj SYM_PROGN = CL_NIL;
CL_Obj SYM_LAMBDA = CL_NIL;
CL_Obj SYM_LET = CL_NIL;
CL_Obj SYM_LETSTAR = CL_NIL;
CL_Obj SYM_SETQ = CL_NIL;
CL_Obj SYM_DEFUN = CL_NIL;
CL_Obj SYM_DEFMACRO = CL_NIL;
CL_Obj SYM_FUNCTION = CL_NIL;
CL_Obj SYM_BLOCK = CL_NIL;
CL_Obj SYM_RETURN_FROM = CL_NIL;
CL_Obj SYM_QUASIQUOTE = CL_NIL;
CL_Obj SYM_UNQUOTE = CL_NIL;
CL_Obj SYM_UNQUOTE_SPLICING = CL_NIL;
CL_Obj SYM_AMP_REST = CL_NIL;
CL_Obj SYM_AMP_OPTIONAL = CL_NIL;
CL_Obj SYM_AMP_BODY = CL_NIL;
CL_Obj SYM_AND = CL_NIL;
CL_Obj SYM_OR = CL_NIL;
CL_Obj SYM_COND = CL_NIL;
CL_Obj SYM_DO = CL_NIL;
CL_Obj SYM_DO_STAR = CL_NIL;
CL_Obj SYM_DOLIST = CL_NIL;
CL_Obj SYM_DOTIMES = CL_NIL;
CL_Obj SYM_RETURN = CL_NIL;
CL_Obj SYM_CASE = CL_NIL;
CL_Obj SYM_ECASE = CL_NIL;
CL_Obj SYM_TYPECASE = CL_NIL;
CL_Obj SYM_ETYPECASE = CL_NIL;
CL_Obj SYM_OTHERWISE = CL_NIL;
CL_Obj SYM_PUSH = CL_NIL;
CL_Obj SYM_POP = CL_NIL;
CL_Obj SYM_FLET = CL_NIL;
CL_Obj SYM_LABELS = CL_NIL;
CL_Obj SYM_AMP_KEY = CL_NIL;
CL_Obj SYM_AMP_ALLOW_OTHER_KEYS = CL_NIL;
CL_Obj SYM_AMP_AUX = CL_NIL;
CL_Obj SYM_AMP_ENVIRONMENT = CL_NIL;
CL_Obj SYM_AMP_WHOLE = CL_NIL;
CL_Obj KW_ALLOW_OTHER_KEYS = CL_NIL;
CL_Obj SYM_TAGBODY = CL_NIL;
CL_Obj SYM_GO = CL_NIL;
CL_Obj SYM_CATCH = CL_NIL;
CL_Obj SYM_THROW = CL_NIL;
CL_Obj SYM_UNWIND_PROTECT = CL_NIL;
CL_Obj SYM_MULTIPLE_VALUE_BIND = CL_NIL;
CL_Obj SYM_MULTIPLE_VALUE_CALL = CL_NIL;
CL_Obj SYM_MULTIPLE_VALUE_LIST = CL_NIL;
CL_Obj SYM_MULTIPLE_VALUE_PROG1 = CL_NIL;
CL_Obj SYM_NTH_VALUE = CL_NIL;
CL_Obj SYM_DEFVAR = CL_NIL;
CL_Obj SYM_DEFPARAMETER = CL_NIL;
CL_Obj SYM_DEFCONSTANT = CL_NIL;
CL_Obj SYM_NAMED_LAMBDA = CL_NIL;
CL_Obj SYM_SETF = CL_NIL;
CL_Obj SYM_EVAL_WHEN = CL_NIL;
CL_Obj SYM_LOAD_TIME_VALUE = CL_NIL;
CL_Obj SYM_DESTRUCTURING_BIND = CL_NIL;
CL_Obj SYM_DEFSETF = CL_NIL;
CL_Obj SYM_DEFTYPE = CL_NIL;
CL_Obj SYM_DECLARE = CL_NIL;
CL_Obj SYM_DECLAIM = CL_NIL;
CL_Obj SYM_LOCALLY = CL_NIL;
CL_Obj SYM_TRACE = CL_NIL;
CL_Obj SYM_UNTRACE = CL_NIL;
CL_Obj SYM_TIME = CL_NIL;
CL_Obj SYM_IN_PACKAGE = CL_NIL;
CL_Obj SYM_STAR_PACKAGE = CL_NIL;
CL_Obj SYM_STAR_LOAD_PATHNAME = CL_NIL;
CL_Obj SYM_STAR_LOAD_TRUENAME = CL_NIL;
CL_Obj SYM_STAR_LOAD_VERBOSE = CL_NIL;
CL_Obj SYM_STAR_GENSYM_COUNTER = CL_NIL;
CL_Obj SYM_STAR_COMPILE_FILE_PATHNAME = CL_NIL;
CL_Obj SYM_STAR_COMPILE_FILE_TRUENAME = CL_NIL;
CL_Obj SYM_STAR_COMPILE_VERBOSE = CL_NIL;
CL_Obj SYM_MACROLET = CL_NIL;
CL_Obj SYM_SYMBOL_MACROLET = CL_NIL;
CL_Obj SYM_THE = CL_NIL;
CL_Obj SYM_PROGV = CL_NIL;
CL_Obj SYM_FUNCALL = CL_NIL;
CL_Obj SYM_APPLY = CL_NIL;

/* Declaration specifier symbols */
CL_Obj SYM_SPECIAL_DECL = CL_NIL;
CL_Obj SYM_TYPE_DECL = CL_NIL;
CL_Obj SYM_FTYPE_DECL = CL_NIL;
CL_Obj SYM_INLINE_DECL = CL_NIL;
CL_Obj SYM_NOTINLINE_DECL = CL_NIL;
CL_Obj SYM_OPTIMIZE_DECL = CL_NIL;
CL_Obj SYM_IGNORE_DECL = CL_NIL;
CL_Obj SYM_IGNORABLE_DECL = CL_NIL;
CL_Obj SYM_DYNAMIC_EXTENT_DECL = CL_NIL;

/* Optimize quality symbols */
CL_Obj SYM_SPEED = CL_NIL;
CL_Obj SYM_SAFETY = CL_NIL;
CL_Obj SYM_DEBUG = CL_NIL;
CL_Obj SYM_SPACE = CL_NIL;

/* Condition type name symbols */
CL_Obj SYM_CONDITION = CL_NIL;
CL_Obj SYM_WARNING = CL_NIL;
CL_Obj SYM_SERIOUS_CONDITION = CL_NIL;
CL_Obj SYM_ERROR_COND = CL_NIL;
CL_Obj SYM_SIMPLE_CONDITION = CL_NIL;
CL_Obj SYM_SIMPLE_ERROR = CL_NIL;
CL_Obj SYM_SIMPLE_WARNING = CL_NIL;
CL_Obj SYM_TYPE_ERROR = CL_NIL;
CL_Obj SYM_SIMPLE_TYPE_ERROR = CL_NIL;
CL_Obj SYM_UNBOUND_VARIABLE_COND = CL_NIL;
CL_Obj SYM_UNDEFINED_FUNCTION_COND = CL_NIL;
CL_Obj SYM_PROGRAM_ERROR = CL_NIL;
CL_Obj SYM_CONTROL_ERROR = CL_NIL;
CL_Obj SYM_ARITHMETIC_ERROR = CL_NIL;
CL_Obj SYM_DIVISION_BY_ZERO = CL_NIL;
CL_Obj SYM_FLOATING_POINT_OVERFLOW = CL_NIL;
CL_Obj SYM_FLOATING_POINT_UNDERFLOW = CL_NIL;
CL_Obj SYM_FLOATING_POINT_INEXACT = CL_NIL;
CL_Obj SYM_FLOATING_POINT_INVALID = CL_NIL;
CL_Obj SYM_STYLE_WARNING = CL_NIL;
CL_Obj SYM_CELL_ERROR = CL_NIL;
CL_Obj SYM_UNBOUND_SLOT = CL_NIL;
CL_Obj SYM_STREAM_ERROR = CL_NIL;
CL_Obj SYM_END_OF_FILE = CL_NIL;
CL_Obj SYM_SOCKET_TIMEOUT = CL_NIL;
CL_Obj SYM_FILE_ERROR = CL_NIL;
CL_Obj SYM_PACKAGE_ERROR = CL_NIL;
CL_Obj SYM_PARSE_ERROR = CL_NIL;
CL_Obj SYM_READER_ERROR = CL_NIL;
CL_Obj SYM_PRINT_NOT_READABLE = CL_NIL;
CL_Obj SYM_STORAGE_CONDITION = CL_NIL;

/* Signaling symbols */
CL_Obj SYM_SIGNAL = CL_NIL;
CL_Obj SYM_WARN = CL_NIL;
CL_Obj SYM_HANDLER_BIND = CL_NIL;
CL_Obj SYM_RESTART_CASE = CL_NIL;

/* Restart name symbols */
CL_Obj SYM_ABORT = CL_NIL;
CL_Obj SYM_CONTINUE = CL_NIL;
CL_Obj SYM_MUFFLE_WARNING = CL_NIL;
CL_Obj SYM_USE_VALUE = CL_NIL;
CL_Obj SYM_STORE_VALUE = CL_NIL;

/* Condition keyword symbols */
CL_Obj KW_FORMAT_CONTROL = CL_NIL;
CL_Obj KW_FORMAT_ARGUMENTS = CL_NIL;
CL_Obj KW_DATUM = CL_NIL;
CL_Obj KW_EXPECTED_TYPE = CL_NIL;
CL_Obj KW_STREAM = CL_NIL;
CL_Obj KW_PACKAGE = CL_NIL;
CL_Obj KW_OPERANDS = CL_NIL;
CL_Obj KW_OPERATION = CL_NIL;
CL_Obj KW_PATHNAME = CL_NIL;
CL_Obj KW_INSTANCE = CL_NIL;

/* REPL history symbols */
CL_Obj SYM_STAR = CL_NIL;
CL_Obj SYM_STARSTAR = CL_NIL;
CL_Obj SYM_STARSTARSTAR = CL_NIL;
CL_Obj SYM_PLUS = CL_NIL;
CL_Obj SYM_PLUSPLUS = CL_NIL;
CL_Obj SYM_PLUSPLUSPLUS = CL_NIL;
CL_Obj SYM_MINUS = CL_NIL;
CL_Obj SYM_SLASH = CL_NIL;
CL_Obj SYM_SLASHSLASH = CL_NIL;
CL_Obj SYM_SLASHSLASHSLASH = CL_NIL;

/* Standard stream variable symbols */
CL_Obj SYM_STANDARD_INPUT = CL_NIL;
CL_Obj SYM_STANDARD_OUTPUT = CL_NIL;
CL_Obj SYM_ERROR_OUTPUT = CL_NIL;
CL_Obj SYM_TRACE_OUTPUT = CL_NIL;
CL_Obj SYM_DEBUG_IO = CL_NIL;
CL_Obj SYM_QUERY_IO = CL_NIL;
CL_Obj SYM_TERMINAL_IO = CL_NIL;
CL_Obj SYM_STAR_FEATURES = CL_NIL;
uint32_t cl_cpu_lost_stores = 0;
CL_Obj SYM_STAR_READTABLE = CL_NIL;

/* Printer control variable symbols */
CL_Obj SYM_PRINT_ESCAPE = CL_NIL;
CL_Obj SYM_PRINT_READABLY = CL_NIL;
CL_Obj SYM_PRINT_BASE = CL_NIL;
CL_Obj SYM_PRINT_RADIX = CL_NIL;
CL_Obj SYM_PRINT_LEVEL = CL_NIL;
CL_Obj SYM_PRINT_LENGTH = CL_NIL;
CL_Obj SYM_PRINT_CASE = CL_NIL;
CL_Obj SYM_PRINT_GENSYM = CL_NIL;
CL_Obj SYM_PRINT_ARRAY = CL_NIL;
CL_Obj SYM_PRINT_CIRCLE = CL_NIL;
CL_Obj SYM_PRINT_PRETTY = CL_NIL;
CL_Obj SYM_PRINT_RIGHT_MARGIN = CL_NIL;
CL_Obj SYM_PRINT_PPRINT_DISPATCH = CL_NIL;
CL_Obj SYM_PRINT_OBJECT_HOOK = CL_NIL;
CL_Obj SYM_READ_DEFAULT_FLOAT_FORMAT = CL_NIL;
CL_Obj SYM_READ_SUPPRESS = CL_NIL;
CL_Obj KW_UPCASE = CL_NIL;
CL_Obj KW_DOWNCASE = CL_NIL;
CL_Obj KW_CAPITALIZE = CL_NIL;

/* Boole operation constants */
CL_Obj SYM_BOOLE_CLR = CL_NIL;
CL_Obj SYM_BOOLE_SET = CL_NIL;
CL_Obj SYM_BOOLE_1 = CL_NIL;
CL_Obj SYM_BOOLE_2 = CL_NIL;
CL_Obj SYM_BOOLE_C1 = CL_NIL;
CL_Obj SYM_BOOLE_C2 = CL_NIL;
CL_Obj SYM_BOOLE_AND = CL_NIL;
CL_Obj SYM_BOOLE_IOR = CL_NIL;
CL_Obj SYM_BOOLE_XOR = CL_NIL;
CL_Obj SYM_BOOLE_EQV = CL_NIL;
CL_Obj SYM_BOOLE_NAND = CL_NIL;
CL_Obj SYM_BOOLE_NOR = CL_NIL;
CL_Obj SYM_BOOLE_ANDC1 = CL_NIL;
CL_Obj SYM_BOOLE_ANDC2 = CL_NIL;
CL_Obj SYM_BOOLE_ORC1 = CL_NIL;
CL_Obj SYM_BOOLE_ORC2 = CL_NIL;

CL_Obj SYM_RANDOM_STATE = CL_NIL;

/* Pathname */
CL_Obj SYM_PATHNAME = CL_NIL;
CL_Obj KW_ABSOLUTE = CL_NIL;
CL_Obj KW_RELATIVE = CL_NIL;
CL_Obj KW_NEWEST = CL_NIL;
CL_Obj KW_HOST = CL_NIL;
CL_Obj KW_DEVICE = CL_NIL;
CL_Obj KW_DIRECTORY = CL_NIL;
CL_Obj KW_NAME = CL_NIL;
CL_Obj KW_TYPE = CL_NIL;
CL_Obj KW_VERSION = CL_NIL;
CL_Obj KW_DEFAULTS = CL_NIL;
CL_Obj KW_UNSPECIFIC = CL_NIL;
CL_Obj KW_WILD = CL_NIL;
CL_Obj SYM_STAR_DEFAULT_PATHNAME_DEFAULTS = CL_NIL;

CL_Obj SYM_STAR_READ_EVAL = CL_NIL;
CL_Obj SYM_STAR_MODULES = CL_NIL;

CL_Obj KW_CL_AMIGA = CL_NIL;
CL_Obj KW_COMMON_LISP = CL_NIL;
CL_Obj KW_POSIX = CL_NIL;
CL_Obj KW_UNIX = CL_NIL;
CL_Obj KW_AMIGAOS = CL_NIL;
CL_Obj KW_M68K = CL_NIL;

/* Rotate-XOR hash — fast on 68020 (no multiply, just shift/or/xor) */
uint32_t cl_hash_string(const char *str, uint32_t len)
{
    uint32_t hash = 0;
    uint32_t i;
    for (i = 0; i < len; i++) {
        hash = ((hash << 5) | (hash >> 27)) ^ (uint8_t)str[i];
    }
    return hash;
}

CL_Obj cl_intern_in(const char *name, uint32_t len, CL_Obj package)
{
    CL_Obj existing;
    CL_Obj name_str, sym, cell;
    CL_Symbol *s;
    char namebuf[256];
    char *heapname = NULL;

    /* GC safety: callers may pass `name` pointing INTO the moving heap
     * (e.g. a Lisp string's character data).  The allocations below
     * (cl_make_string, cl_make_symbol) can compact and relocate that
     * string, dangling `name` and interning a garbage symbol name.  Copy
     * it into a GC-immune C buffer up-front and use that everywhere. */
    if (len < sizeof(namebuf)) {
        memcpy(namebuf, name, len);
        name = namebuf;
    } else {
        heapname = (char *)platform_alloc(len);
        if (heapname) {
            memcpy(heapname, name, len);
            name = heapname;
        }
        /* If allocation fails, fall back to the original pointer — no worse
         * than before; OOM will surface elsewhere. */
    }

    /* Fast path: present or inherited — including a name that lives in a
     * package's demand-interned binding table (bindtab.c), which
     * cl_find_symbol_with_status materialises before returning, so an
     * INTERN of such a name yields the binding, never a fresh symbol. */
    {
        int status = 0;
        existing = cl_find_symbol_with_status(name, len, package, &status);
        if (status != 0) {
            if (heapname) platform_free(heapname);
            return existing;   /* already normalized */
        }
    }

    /* Slow path: create the symbol AND its bucket cell outside the lock.
     * Protect package — allocations below may trigger GC/compaction.
     * The bucket cell must be pre-consed too: allocating while holding
     * cl_package_rwlock can trigger a stop-the-world GC that waits for a
     * peer blocked on the rdlock (any intern fast path), which can never
     * park — a circular STW-vs-rwlock hang. */
    CL_GC_PROTECT(package);
    name_str = cl_make_string(name, len);
    CL_GC_PROTECT(name_str);
    sym = cl_make_symbol(name_str);
    CL_GC_PROTECT(sym);
    cell = cl_cons(sym, CL_NIL);
    CL_GC_UNPROTECT(3);

    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    s->hash = cl_hash_string(name, len);

    /* Write-lock with re-check; the link inside the lock is plain stores
     * only (cl_package_add_symbol_cell).  On the lost-race path our fresh
     * symbol and cell simply become garbage.  The re-check includes the
     * binding tables: a table registered since the fast path must win over
     * our fresh symbol, or that symbol would shadow its own definition. */
    {
        int status = 0, name_idx = 0, def_idx = -1, want_setter = 0;
        CL_Obj lazy_pkg = CL_NIL;
        if (cl_package_rwlock) platform_rwlock_wrlock(cl_package_rwlock);
        existing = cl_package_lookup_nolock(name, len, package, &status,
                                            &lazy_pkg, &name_idx, &def_idx,
                                            &want_setter);
        if (existing != CL_UNBOUND) {
            if (cl_package_rwlock) platform_rwlock_unlock(cl_package_rwlock);
            if (heapname) platform_free(heapname);
            return cl_normalize_nil_symbol(existing);
        }
        if (!CL_NULL_P(lazy_pkg)) {
            if (cl_package_rwlock) platform_rwlock_unlock(cl_package_rwlock);
            if (heapname) platform_free(heapname);
            return cl_bindtab_materialize(lazy_pkg, name, len, name_idx,
                                          def_idx, want_setter, 0);
        }
        cl_package_add_symbol_cell(package, sym, cell);
        if (cl_package_rwlock) platform_rwlock_unlock(cl_package_rwlock);
    }
    if (heapname) platform_free(heapname);
    /* Newly-interned KEYWORD-package symbols are self-evaluating constants
     * and automatically external (CLHS 11.1.2.3.1).  Done here so any path
     * that creates a fresh keyword (cl_intern_keyword, bi_intern, the
     * reader's :foo syntax) sets these uniformly. */
    if (package == cl_package_keyword) {
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->value = sym;
        s->flags |= CL_SYM_CONSTANT;
        /* GC SAFETY: cl_export_symbol conses its import/export cells and
         * can collect.  It roots its OWN copy of the argument, not this
         * local, so an unprotected `sym` came back as the pre-move offset
         * of the symbol it just interned -- the reader then consed that
         * stale offset into the form it was building (a generational
         * minor slides the fresh symbol down; the host crashed in the
         * next minor's survivor slide, 2026-09-22). */
        CL_GC_PROTECT(sym);
        cl_export_symbol(sym, cl_package_keyword);
        CL_GC_UNPROTECT(1);
    }
    return sym;
}

CL_Obj cl_intern(const char *name, uint32_t len)
{
    return cl_intern_in(name, len, cl_current_package);
}

CL_Obj cl_find_symbol(const char *name, uint32_t len, CL_Obj package)
{
    return cl_package_find_symbol(name, len, package);
}

CL_Obj cl_intern_keyword(const char *name, uint32_t len)
{
    /* cl_intern_in handles the KEYWORD-specific bookkeeping
     * (self-evaluating constant flag + export). */
    return cl_intern_in(name, len, cl_package_keyword);
}

CL_Obj cl_make_uninterned_symbol(CL_Obj name_str)
{
    CL_Obj sym;
    CL_Symbol *s;
    CL_String *name;

    CL_GC_PROTECT(name_str);
    sym = cl_make_symbol(name_str);
    CL_GC_UNPROTECT(1);

    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    name = (CL_String *)CL_OBJ_TO_PTR(name_str);
    s->hash = cl_hash_string(name->data, name->length);
    s->package = CL_NIL;  /* No home package */
    return sym;
}

int cl_symbol_specialp(CL_Obj sym)
{
    CL_Symbol *s;
    if (CL_NULL_P(sym)) return 0;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    return (s->flags & CL_SYM_SPECIAL) != 0;
}

const char *cl_symbol_name(CL_Obj sym)
{
    CL_Symbol *s;
    CL_String *name;
    if (CL_NULL_P(sym)) return "NIL";
    /* Defensive: callers occasionally hand us a non-symbol (e.g. a non-symbol
     * value in keyword-argument position).  Dereferencing it as a CL_Symbol
     * would read a garbage offset and, when the result is fed to a "%s"
     * cl_error format, crash inside vsnprintf.  Return a safe marker instead so
     * the caller's diagnostic prints rather than SEGVs. */
    if (!CL_SYMBOL_P(sym)) return "(non-symbol)";
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    name = (CL_String *)CL_OBJ_TO_PTR(s->name);
    return name->data;
}

/* Make *FEATURES* say what THIS process's CPU self-test found (main.c):
 * :CPU-LOST-STORES on it when cl_cpu_lost_stores is nonzero, off it
 * otherwise.  cl_symbol_init calls it at boot; main.c calls it again after a
 * heap-image restore, because the restore replaces *FEATURES* with the
 * SAVING process's list -- an image saved on an affected Vampire would
 * otherwise tell a sound machine its CPU loses stores, and the shipped
 * amilisp.img (saved in FS-UAE) would hide the flag from an affected one. */
void cl_features_sync_cpu_lost_stores(void)
{
    CL_Obj kw, cur, prev = CL_NIL;
    CL_Symbol *s;

    /* Sound CPU: only look the keyword up -- a boot that never had it must
     * not grow the KEYWORD package, and an image that had it finds it. */
    if (cl_cpu_lost_stores)
        kw = cl_intern_keyword("CPU-LOST-STORES", 15);
    else
        kw = cl_find_symbol("CPU-LOST-STORES", 15, cl_package_keyword);
    if (CL_NULL_P(kw))
        return;                 /* never interned: on no list */

    /* No allocation in this walk, so the raw pointers stay valid.  Removal
     * splices in place; a duplicate entry goes too. */
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_FEATURES);
    for (cur = s->value; CL_CONS_P(cur); ) {
        CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(cur);
        if (c->car == kw) {
            if (cl_cpu_lost_stores)
                return;         /* already on the list */
            if (CL_NULL_P(prev))
                s->value = c->cdr;
            else
                ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = c->cdr;
        } else {
            prev = cur;
        }
        cur = c->cdr;
    }

    if (cl_cpu_lost_stores) {
        /* cl_cons roots both operands; re-derive s after the allocation. */
        CL_Obj features = s->value;
        features = cl_cons(kw, features);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_FEATURES);
        s->value = features;
    }
}

void cl_symbol_init(void)
{
    /* Intern well-known symbols in CL package */
    SYM_T             = cl_intern_in("T", 1, cl_package_cl);
    SYM_NIL           = cl_intern_in("NIL", 3, cl_package_cl);
    /* T and NIL are constants per CL spec */
    {
        CL_Symbol *s;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_T);
        s->flags |= CL_SYM_CONSTANT;
        s->value = SYM_T;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_NIL);
        s->flags |= CL_SYM_CONSTANT;
        /* NIL's value is already 0 = CL_NIL */
    }
    SYM_QUOTE         = cl_intern_in("QUOTE", 5, cl_package_cl);
    SYM_IF            = cl_intern_in("IF", 2, cl_package_cl);
    SYM_PROGN         = cl_intern_in("PROGN", 5, cl_package_cl);
    SYM_LAMBDA        = cl_intern_in("LAMBDA", 6, cl_package_cl);
    SYM_LET           = cl_intern_in("LET", 3, cl_package_cl);
    SYM_LETSTAR       = cl_intern_in("LET*", 4, cl_package_cl);
    SYM_SETQ          = cl_intern_in("SETQ", 4, cl_package_cl);
    SYM_DEFUN         = cl_intern_in("DEFUN", 5, cl_package_cl);
    SYM_DEFMACRO      = cl_intern_in("DEFMACRO", 8, cl_package_cl);
    SYM_FUNCTION      = cl_intern_in("FUNCTION", 8, cl_package_cl);
    SYM_BLOCK         = cl_intern_in("BLOCK", 5, cl_package_cl);
    SYM_RETURN_FROM   = cl_intern_in("RETURN-FROM", 11, cl_package_cl);
    /* Backquote-related markers — interned in CLAMIGA so the reader's
     * synthetic forms don't pollute COMMON-LISP exports.  CL :use CLAMIGA
     * so unqualified references continue to resolve. */
    SYM_QUASIQUOTE    = cl_intern_in("QUASIQUOTE", 10, cl_package_clamiga);
    SYM_UNQUOTE       = cl_intern_in("UNQUOTE", 7, cl_package_clamiga);
    SYM_UNQUOTE_SPLICING = cl_intern_in("UNQUOTE-SPLICING", 16, cl_package_clamiga);
    SYM_AMP_REST      = cl_intern_in("&REST", 5, cl_package_cl);
    SYM_AMP_OPTIONAL  = cl_intern_in("&OPTIONAL", 9, cl_package_cl);
    SYM_AMP_BODY      = cl_intern_in("&BODY", 5, cl_package_cl);
    SYM_AND           = cl_intern_in("AND", 3, cl_package_cl);
    SYM_OR            = cl_intern_in("OR", 2, cl_package_cl);
    SYM_COND          = cl_intern_in("COND", 4, cl_package_cl);
    SYM_DO            = cl_intern_in("DO", 2, cl_package_cl);
    SYM_DO_STAR       = cl_intern_in("DO*", 3, cl_package_cl);
    SYM_DOLIST        = cl_intern_in("DOLIST", 6, cl_package_cl);
    SYM_DOTIMES       = cl_intern_in("DOTIMES", 7, cl_package_cl);
    SYM_RETURN        = cl_intern_in("RETURN", 6, cl_package_cl);
    SYM_CASE          = cl_intern_in("CASE", 4, cl_package_cl);
    SYM_ECASE         = cl_intern_in("ECASE", 5, cl_package_cl);
    SYM_TYPECASE      = cl_intern_in("TYPECASE", 8, cl_package_cl);
    SYM_ETYPECASE     = cl_intern_in("ETYPECASE", 9, cl_package_cl);
    SYM_OTHERWISE     = cl_intern_in("OTHERWISE", 9, cl_package_cl);
    SYM_PUSH          = cl_intern_in("PUSH", 4, cl_package_cl);
    SYM_POP           = cl_intern_in("POP", 3, cl_package_cl);
    SYM_FLET          = cl_intern_in("FLET", 4, cl_package_cl);
    SYM_LABELS        = cl_intern_in("LABELS", 6, cl_package_cl);
    SYM_AMP_KEY       = cl_intern_in("&KEY", 4, cl_package_cl);
    SYM_AMP_ALLOW_OTHER_KEYS = cl_intern_in("&ALLOW-OTHER-KEYS", 17, cl_package_cl);
    SYM_AMP_AUX       = cl_intern_in("&AUX", 4, cl_package_cl);
    SYM_AMP_ENVIRONMENT = cl_intern_in("&ENVIRONMENT", 12, cl_package_cl);
    SYM_AMP_WHOLE     = cl_intern_in("&WHOLE", 6, cl_package_cl);
    KW_ALLOW_OTHER_KEYS = cl_intern_keyword("ALLOW-OTHER-KEYS", 16);
    SYM_TAGBODY        = cl_intern_in("TAGBODY", 7, cl_package_cl);
    SYM_GO             = cl_intern_in("GO", 2, cl_package_cl);
    SYM_CATCH          = cl_intern_in("CATCH", 5, cl_package_cl);
    SYM_THROW          = cl_intern_in("THROW", 5, cl_package_cl);
    SYM_UNWIND_PROTECT = cl_intern_in("UNWIND-PROTECT", 14, cl_package_cl);
    SYM_MULTIPLE_VALUE_BIND  = cl_intern_in("MULTIPLE-VALUE-BIND", 19, cl_package_cl);
    SYM_MULTIPLE_VALUE_CALL  = cl_intern_in("MULTIPLE-VALUE-CALL", 19, cl_package_cl);
    SYM_MULTIPLE_VALUE_LIST  = cl_intern_in("MULTIPLE-VALUE-LIST", 19, cl_package_cl);
    SYM_MULTIPLE_VALUE_PROG1 = cl_intern_in("MULTIPLE-VALUE-PROG1", 20, cl_package_cl);
    SYM_NTH_VALUE            = cl_intern_in("NTH-VALUE", 9, cl_package_cl);
    SYM_DEFVAR               = cl_intern_in("DEFVAR", 6, cl_package_cl);
    SYM_DEFPARAMETER         = cl_intern_in("DEFPARAMETER", 12, cl_package_cl);
    SYM_DEFCONSTANT          = cl_intern_in("DEFCONSTANT", 11, cl_package_cl);
    /* NAMED-LAMBDA is a compiler internal, not part of ANSI CL —
     * keep it in CLAMIGA. */
    SYM_NAMED_LAMBDA         = cl_intern_in("NAMED-LAMBDA", 12, cl_package_clamiga);
    SYM_SETF                 = cl_intern_in("SETF", 4, cl_package_cl);
    SYM_EVAL_WHEN            = cl_intern_in("EVAL-WHEN", 9, cl_package_cl);
    SYM_LOAD_TIME_VALUE      = cl_intern_in("LOAD-TIME-VALUE", 15, cl_package_cl);
    SYM_DESTRUCTURING_BIND   = cl_intern_in("DESTRUCTURING-BIND", 18, cl_package_cl);
    SYM_DEFSETF              = cl_intern_in("DEFSETF", 7, cl_package_cl);
    SYM_DEFTYPE              = cl_intern_in("DEFTYPE", 7, cl_package_cl);
    SYM_DECLARE              = cl_intern_in("DECLARE", 7, cl_package_cl);
    SYM_DECLAIM              = cl_intern_in("DECLAIM", 7, cl_package_cl);
    SYM_LOCALLY              = cl_intern_in("LOCALLY", 7, cl_package_cl);
    SYM_TRACE                = cl_intern_in("TRACE", 5, cl_package_cl);
    SYM_UNTRACE              = cl_intern_in("UNTRACE", 7, cl_package_cl);
    SYM_TIME                 = cl_intern_in("TIME", 4, cl_package_cl);
    SYM_IN_PACKAGE           = cl_intern_in("IN-PACKAGE", 10, cl_package_cl);
    SYM_STAR_PACKAGE         = cl_intern_in("*PACKAGE*", 9, cl_package_cl);
    SYM_STAR_LOAD_PATHNAME   = cl_intern_in("*LOAD-PATHNAME*", 15, cl_package_cl);
    SYM_STAR_LOAD_TRUENAME   = cl_intern_in("*LOAD-TRUENAME*", 15, cl_package_cl);
    SYM_STAR_LOAD_VERBOSE    = cl_intern_in("*LOAD-VERBOSE*", 14, cl_package_cl);
    SYM_STAR_COMPILE_FILE_PATHNAME = cl_intern_in("*COMPILE-FILE-PATHNAME*", 23, cl_package_cl);
    SYM_STAR_COMPILE_FILE_TRUENAME = cl_intern_in("*COMPILE-FILE-TRUENAME*", 23, cl_package_cl);
    SYM_STAR_COMPILE_VERBOSE = cl_intern_in("*COMPILE-VERBOSE*", 17, cl_package_cl);
    {
        CL_Symbol *s;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_LOAD_PATHNAME);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_LOAD_TRUENAME);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_LOAD_VERBOSE);
        s->flags |= CL_SYM_SPECIAL; s->value = SYM_T;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_COMPILE_FILE_PATHNAME);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_COMPILE_FILE_TRUENAME);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
    }
    /* CLHS: *GENSYM-COUNTER* — initial value is implementation-dependent
     * but must be a non-negative integer.  ANSI tests rely on this being
     * a CL-visible special variable that GENSYM reads and increments. */
    SYM_STAR_GENSYM_COUNTER  = cl_intern_in("*GENSYM-COUNTER*", 16, cl_package_cl);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_GENSYM_COUNTER);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_MAKE_FIXNUM(1);
    }
    /* *macroexpand-hook* — set to NIL here, boot.lisp sets to #'funcall */
    /* *compile-print*, *compile-verbose* — standard CL variables */
    {
        CL_Obj sym;
        CL_Symbol *s;
        sym = cl_intern_in("*MACROEXPAND-HOOK*", 18, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        sym = cl_intern_in("*COMPILE-PRINT*", 15, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        /* *compile-verbose* now uses SYM_STAR_COMPILE_VERBOSE, initialized above */
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_COMPILE_VERBOSE);
        s->flags |= CL_SYM_SPECIAL; s->value = SYM_T;
    }
    SYM_MACROLET             = cl_intern_in("MACROLET", 8, cl_package_cl);
    SYM_SYMBOL_MACROLET      = cl_intern_in("SYMBOL-MACROLET", 15, cl_package_cl);
    SYM_THE                  = cl_intern_in("THE", 3, cl_package_cl);
    SYM_PROGV                = cl_intern_in("PROGV", 5, cl_package_cl);
    SYM_FUNCALL              = cl_intern_in("FUNCALL", 7, cl_package_cl);
    SYM_APPLY                = cl_intern_in("APPLY", 5, cl_package_cl);

    /* Declaration specifier symbols */
    SYM_SPECIAL_DECL         = cl_intern_in("SPECIAL", 7, cl_package_cl);
    SYM_TYPE_DECL            = cl_intern_in("TYPE", 4, cl_package_cl);
    SYM_FTYPE_DECL           = cl_intern_in("FTYPE", 5, cl_package_cl);
    SYM_INLINE_DECL          = cl_intern_in("INLINE", 6, cl_package_cl);
    SYM_NOTINLINE_DECL       = cl_intern_in("NOTINLINE", 9, cl_package_cl);
    SYM_OPTIMIZE_DECL        = cl_intern_in("OPTIMIZE", 8, cl_package_cl);
    SYM_IGNORE_DECL          = cl_intern_in("IGNORE", 6, cl_package_cl);
    SYM_IGNORABLE_DECL       = cl_intern_in("IGNORABLE", 9, cl_package_cl);
    SYM_DYNAMIC_EXTENT_DECL  = cl_intern_in("DYNAMIC-EXTENT", 14, cl_package_cl);

    /* Optimize quality symbols.  COMPILATION-SPEED is the fifth standard
       OPTIMIZE quality per CLHS; although CL does not strictly export it,
       SBCL/CCL/most implementations treat it as a CL symbol so portable
       libraries (serapeum macro-tools, etc.) read it as CL:COMPILATION-SPEED.
       We intern it here so the bulk exporter picks it up. */
    SYM_SPEED                = cl_intern_in("SPEED", 5, cl_package_cl);
    SYM_SAFETY               = cl_intern_in("SAFETY", 6, cl_package_cl);
    SYM_DEBUG                = cl_intern_in("DEBUG", 5, cl_package_cl);
    SYM_SPACE                = cl_intern_in("SPACE", 5, cl_package_cl);
    (void)cl_intern_in("COMPILATION-SPEED", 17, cl_package_cl);

    /* Condition type name symbols */
    SYM_CONDITION                = cl_intern_in("CONDITION", 9, cl_package_cl);
    SYM_WARNING                  = cl_intern_in("WARNING", 7, cl_package_cl);
    SYM_SERIOUS_CONDITION        = cl_intern_in("SERIOUS-CONDITION", 17, cl_package_cl);
    SYM_ERROR_COND               = cl_intern_in("ERROR", 5, cl_package_cl);
    SYM_SIMPLE_CONDITION         = cl_intern_in("SIMPLE-CONDITION", 16, cl_package_cl);
    SYM_SIMPLE_ERROR             = cl_intern_in("SIMPLE-ERROR", 12, cl_package_cl);
    SYM_SIMPLE_WARNING           = cl_intern_in("SIMPLE-WARNING", 14, cl_package_cl);
    SYM_TYPE_ERROR               = cl_intern_in("TYPE-ERROR", 10, cl_package_cl);
    SYM_SIMPLE_TYPE_ERROR        = cl_intern_in("SIMPLE-TYPE-ERROR", 17, cl_package_cl);
    SYM_UNBOUND_VARIABLE_COND    = cl_intern_in("UNBOUND-VARIABLE", 16, cl_package_cl);
    SYM_UNDEFINED_FUNCTION_COND  = cl_intern_in("UNDEFINED-FUNCTION", 18, cl_package_cl);
    SYM_PROGRAM_ERROR            = cl_intern_in("PROGRAM-ERROR", 13, cl_package_cl);
    SYM_CONTROL_ERROR            = cl_intern_in("CONTROL-ERROR", 13, cl_package_cl);
    SYM_ARITHMETIC_ERROR         = cl_intern_in("ARITHMETIC-ERROR", 16, cl_package_cl);
    SYM_DIVISION_BY_ZERO         = cl_intern_in("DIVISION-BY-ZERO", 16, cl_package_cl);
    SYM_FLOATING_POINT_OVERFLOW  = cl_intern_in("FLOATING-POINT-OVERFLOW", 23, cl_package_cl);
    SYM_FLOATING_POINT_UNDERFLOW = cl_intern_in("FLOATING-POINT-UNDERFLOW", 24, cl_package_cl);
    SYM_FLOATING_POINT_INEXACT   = cl_intern_in("FLOATING-POINT-INEXACT", 22, cl_package_cl);
    SYM_FLOATING_POINT_INVALID   = cl_intern_in("FLOATING-POINT-INVALID-OPERATION", 32, cl_package_cl);
    SYM_STYLE_WARNING            = cl_intern_in("STYLE-WARNING", 13, cl_package_cl);
    SYM_CELL_ERROR               = cl_intern_in("CELL-ERROR", 10, cl_package_cl);
    SYM_UNBOUND_SLOT             = cl_intern_in("UNBOUND-SLOT", 12, cl_package_cl);
    SYM_STREAM_ERROR             = cl_intern_in("STREAM-ERROR", 12, cl_package_cl);
    SYM_END_OF_FILE              = cl_intern_in("END-OF-FILE", 11, cl_package_cl);
    /* EXT:SOCKET-TIMEOUT — implementation extension (not a CLHS condition);
     * lives in EXT and is exported so user code can name it in handler clauses. */
    SYM_SOCKET_TIMEOUT           = cl_intern_in("SOCKET-TIMEOUT", 14, cl_package_ext);
    cl_export_symbol(SYM_SOCKET_TIMEOUT, cl_package_ext);
    SYM_FILE_ERROR               = cl_intern_in("FILE-ERROR", 10, cl_package_cl);
    SYM_PACKAGE_ERROR            = cl_intern_in("PACKAGE-ERROR", 13, cl_package_cl);
    SYM_PARSE_ERROR              = cl_intern_in("PARSE-ERROR", 11, cl_package_cl);
    SYM_READER_ERROR             = cl_intern_in("READER-ERROR", 12, cl_package_cl);
    SYM_PRINT_NOT_READABLE       = cl_intern_in("PRINT-NOT-READABLE", 18, cl_package_cl);
    SYM_STORAGE_CONDITION        = cl_intern_in("STORAGE-CONDITION", 17, cl_package_cl);

    /* Signaling symbols */
    SYM_SIGNAL                   = cl_intern_in("SIGNAL", 6, cl_package_cl);
    SYM_WARN                     = cl_intern_in("WARN", 4, cl_package_cl);
    SYM_HANDLER_BIND             = cl_intern_in("HANDLER-BIND", 12, cl_package_cl);
    SYM_RESTART_CASE             = cl_intern_in("RESTART-CASE", 12, cl_package_cl);

    /* Restart name symbols */
    SYM_ABORT                    = cl_intern_in("ABORT", 5, cl_package_cl);
    SYM_CONTINUE                 = cl_intern_in("CONTINUE", 8, cl_package_cl);
    SYM_MUFFLE_WARNING           = cl_intern_in("MUFFLE-WARNING", 14, cl_package_cl);
    SYM_USE_VALUE                = cl_intern_in("USE-VALUE", 9, cl_package_cl);
    SYM_STORE_VALUE              = cl_intern_in("STORE-VALUE", 11, cl_package_cl);

    /* Condition keyword symbols */
    KW_FORMAT_CONTROL            = cl_intern_keyword("FORMAT-CONTROL", 14);
    KW_FORMAT_ARGUMENTS          = cl_intern_keyword("FORMAT-ARGUMENTS", 16);
    KW_DATUM                     = cl_intern_keyword("DATUM", 5);
    KW_EXPECTED_TYPE             = cl_intern_keyword("EXPECTED-TYPE", 13);
    KW_STREAM                    = cl_intern_keyword("STREAM", 6);
    KW_PACKAGE                   = cl_intern_keyword("PACKAGE", 7);
    KW_OPERANDS                  = cl_intern_keyword("OPERANDS", 8);
    KW_OPERATION                 = cl_intern_keyword("OPERATION", 9);
    KW_PATHNAME                  = cl_intern_keyword("PATHNAME", 8);
    KW_INSTANCE                  = cl_intern_keyword("INSTANCE", 8);

    /* REPL history symbols — intern multi-char ones now;
       *, +, - will be looked up in cl_repl_init() after builtins register them */
    SYM_STARSTAR       = cl_intern_in("**", 2, cl_package_cl);
    SYM_STARSTARSTAR   = cl_intern_in("***", 3, cl_package_cl);
    SYM_PLUSPLUS        = cl_intern_in("++", 2, cl_package_cl);
    SYM_PLUSPLUSPLUS   = cl_intern_in("+++", 3, cl_package_cl);
    SYM_SLASHSLASH     = cl_intern_in("//", 2, cl_package_cl);
    SYM_SLASHSLASHSLASH = cl_intern_in("///", 3, cl_package_cl);

    /* Standard stream variable symbols — mark as special */
    SYM_STANDARD_INPUT   = cl_intern_in("*STANDARD-INPUT*", 16, cl_package_cl);
    SYM_STANDARD_OUTPUT  = cl_intern_in("*STANDARD-OUTPUT*", 17, cl_package_cl);
    SYM_ERROR_OUTPUT     = cl_intern_in("*ERROR-OUTPUT*", 14, cl_package_cl);
    SYM_TRACE_OUTPUT     = cl_intern_in("*TRACE-OUTPUT*", 14, cl_package_cl);
    SYM_DEBUG_IO         = cl_intern_in("*DEBUG-IO*", 10, cl_package_cl);
    SYM_QUERY_IO         = cl_intern_in("*QUERY-IO*", 10, cl_package_cl);
    SYM_TERMINAL_IO      = cl_intern_in("*TERMINAL-IO*", 13, cl_package_cl);
    {
        CL_Symbol *s;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STANDARD_INPUT);
        s->flags |= CL_SYM_SPECIAL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STANDARD_OUTPUT);
        s->flags |= CL_SYM_SPECIAL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_ERROR_OUTPUT);
        s->flags |= CL_SYM_SPECIAL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_TRACE_OUTPUT);
        s->flags |= CL_SYM_SPECIAL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_DEBUG_IO);
        s->flags |= CL_SYM_SPECIAL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_QUERY_IO);
        s->flags |= CL_SYM_SPECIAL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_TERMINAL_IO);
        s->flags |= CL_SYM_SPECIAL;
    }

    /* Printer control variables — CL spec defaults */
    SYM_PRINT_ESCAPE   = cl_intern_in("*PRINT-ESCAPE*", 14, cl_package_cl);
    SYM_PRINT_READABLY = cl_intern_in("*PRINT-READABLY*", 16, cl_package_cl);
    SYM_PRINT_BASE     = cl_intern_in("*PRINT-BASE*", 12, cl_package_cl);
    SYM_PRINT_RADIX    = cl_intern_in("*PRINT-RADIX*", 13, cl_package_cl);
    SYM_PRINT_LEVEL    = cl_intern_in("*PRINT-LEVEL*", 13, cl_package_cl);
    SYM_PRINT_LENGTH   = cl_intern_in("*PRINT-LENGTH*", 14, cl_package_cl);
    SYM_PRINT_CASE     = cl_intern_in("*PRINT-CASE*", 12, cl_package_cl);
    SYM_PRINT_GENSYM   = cl_intern_in("*PRINT-GENSYM*", 14, cl_package_cl);
    SYM_PRINT_ARRAY    = cl_intern_in("*PRINT-ARRAY*", 13, cl_package_cl);
    SYM_PRINT_CIRCLE   = cl_intern_in("*PRINT-CIRCLE*", 14, cl_package_cl);
    SYM_PRINT_PRETTY   = cl_intern_in("*PRINT-PRETTY*", 14, cl_package_cl);
    SYM_PRINT_RIGHT_MARGIN = cl_intern_in("*PRINT-RIGHT-MARGIN*", 20, cl_package_cl);
    KW_UPCASE          = cl_intern_keyword("UPCASE", 6);
    KW_DOWNCASE        = cl_intern_keyword("DOWNCASE", 8);
    KW_CAPITALIZE      = cl_intern_keyword("CAPITALIZE", 10);
    {
        CL_Symbol *s;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_ESCAPE);
        s->flags |= CL_SYM_SPECIAL; s->value = SYM_T;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_READABLY);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_BASE);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_MAKE_FIXNUM(10);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_RADIX);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_LEVEL);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_LENGTH);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_CASE);
        s->flags |= CL_SYM_SPECIAL; s->value = KW_UPCASE;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_GENSYM);
        s->flags |= CL_SYM_SPECIAL; s->value = SYM_T;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_ARRAY);
        s->flags |= CL_SYM_SPECIAL; s->value = SYM_T;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_CIRCLE);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_PRETTY);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_RIGHT_MARGIN);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
    }

    /* *PRINT-LINES* — CL spec, default NIL (no line limit) */
    {
        CL_Obj sym = cl_intern_in("*PRINT-LINES*", 13, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
    }

    /* *PRINT-MISER-WIDTH* — CL spec, default NIL */
    {
        CL_Obj sym = cl_intern_in("*PRINT-MISER-WIDTH*", 19, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
    }

    /* *PRINT-PPRINT-DISPATCH* — default is NIL (no custom dispatch) */
    SYM_PRINT_PPRINT_DISPATCH = cl_intern_in("*PRINT-PPRINT-DISPATCH*", 23, cl_package_cl);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_PPRINT_DISPATCH);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
    }

    /* *PRINT-OBJECT-HOOK* — called for struct printing, returns string or NIL.
     * Implementation-specific, intern in CLAMIGA. */
    SYM_PRINT_OBJECT_HOOK = cl_intern_in("*PRINT-OBJECT-HOOK*", 19, cl_package_clamiga);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_OBJECT_HOOK);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
    }

    /* *RANDOM-STATE* — default random state */
    SYM_RANDOM_STATE = cl_intern_in("*RANDOM-STATE*", 14, cl_package_cl);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_RANDOM_STATE);
        s->flags |= CL_SYM_SPECIAL;
        s->value = cl_make_random_state(platform_time_ms() ^ platform_universal_time());
    }

    /* Pathname symbols */
    SYM_PATHNAME = cl_intern_in("PATHNAME", 8, cl_package_cl);
    KW_ABSOLUTE  = cl_intern_keyword("ABSOLUTE", 8);
    KW_RELATIVE  = cl_intern_keyword("RELATIVE", 8);
    KW_NEWEST    = cl_intern_keyword("NEWEST", 6);
    KW_HOST      = cl_intern_keyword("HOST", 4);
    KW_DEVICE    = cl_intern_keyword("DEVICE", 6);
    KW_DIRECTORY = cl_intern_keyword("DIRECTORY", 9);
    KW_NAME      = cl_intern_keyword("NAME", 4);
    KW_TYPE      = cl_intern_keyword("TYPE", 4);
    KW_VERSION   = cl_intern_keyword("VERSION", 7);
    KW_DEFAULTS    = cl_intern_keyword("DEFAULTS", 8);
    KW_UNSPECIFIC  = cl_intern_keyword("UNSPECIFIC", 10);
    KW_WILD        = cl_intern_keyword("WILD", 4);
    SYM_STAR_DEFAULT_PATHNAME_DEFAULTS = cl_intern_in("*DEFAULT-PATHNAME-DEFAULTS*", 27, cl_package_cl);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_DEFAULT_PATHNAME_DEFAULTS);
        s->flags |= CL_SYM_SPECIAL;
        /* Value set later in cl_builtins_pathname_init after pathname type is available */
    }

    /* *READ-EVAL* — controls whether #. is allowed (default T) */
    SYM_STAR_READ_EVAL = cl_intern_in("*READ-EVAL*", 11, cl_package_cl);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_READ_EVAL);
        s->flags |= CL_SYM_SPECIAL;
        s->value = SYM_T;
    }
    /* Reader dereferences this handle on every #. — forward it across
     * compaction (see SYM_STAR_READTABLE note below). */
    cl_gc_register_root(&SYM_STAR_READ_EVAL);

    /* *READ-BASE* — CL spec, default 10 */
    {
        CL_Obj sym = cl_intern_in("*READ-BASE*", 11, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_MAKE_FIXNUM(10);
    }

    /* *READ-DEFAULT-FLOAT-FORMAT* — CL spec, default SINGLE-FLOAT */
    {
        CL_Symbol *s;
        SYM_READ_DEFAULT_FLOAT_FORMAT = cl_intern_in("*READ-DEFAULT-FLOAT-FORMAT*", 27, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_READ_DEFAULT_FLOAT_FORMAT);
        s->flags |= CL_SYM_SPECIAL;
        s->value = cl_intern("SINGLE-FLOAT", 12);
    }

    /* *READ-SUPPRESS* — CL spec, default NIL.  The reader consults this on
     * every path that would construct/intern an object or signal a parse
     * error (see read_suppress_active() in reader.c). */
    {
        CL_Symbol *s;
        SYM_READ_SUPPRESS = cl_intern_in("*READ-SUPPRESS*", 15, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_READ_SUPPRESS);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
    }

    /* *MODULES* — list of provided module names */
    SYM_STAR_MODULES = cl_intern_in("*MODULES*", 9, cl_package_cl);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_MODULES);
        s->flags |= CL_SYM_SPECIAL;
        s->value = CL_NIL;
    }

    /* *FEATURES* — feature list for #+ / #- */
    SYM_STAR_FEATURES = cl_intern_in("*FEATURES*", 10, cl_package_cl);
    KW_CL_AMIGA    = cl_intern_keyword("CL-AMIGA", 8);
    KW_COMMON_LISP = cl_intern_keyword("COMMON-LISP", 11);
    KW_POSIX       = cl_intern_keyword("POSIX", 5);
    KW_UNIX        = cl_intern_keyword("UNIX", 4);
    KW_AMIGAOS     = cl_intern_keyword("AMIGAOS", 7);
    KW_M68K        = cl_intern_keyword("M68K", 4);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_FEATURES);
        CL_Obj features = CL_NIL, kw;
        s->flags |= CL_SYM_SPECIAL;
        /* Each keyword is interned in its own statement, before the cons
         * reads `features` (mem.h, cl_list2). */
        CL_GC_PROTECT(features);
#ifdef PLATFORM_AMIGA
#ifdef PLATFORM_MORPHOS
        /* MorphOS is the AmigaOS API on PPC: keep :AMIGAOS (the OS-API
         * feature the library code keys on) but NOT :M68K — the CPU
         * feature gates m68k-only machinery (the template JIT, the
         * register-based library-call FFI dispatcher) that this build
         * does not have.  :MORPHOS lets code target this port. */
        kw = cl_intern_keyword("PPC", 3);
        features = cl_cons(kw, features);
        kw = cl_intern_keyword("MORPHOS", 7);
        features = cl_cons(kw, features);
#else
        features = cl_cons(KW_M68K, features);
#endif
        features = cl_cons(KW_AMIGAOS, features);
#else
        features = cl_cons(KW_POSIX, features);
        features = cl_cons(KW_UNIX, features);
        /* Host-OS feature so foreign-library loaders (CFFI / cl+ssl)
         * pick the correct platform naming convention (.dylib vs .so)
         * and probe the right install locations. */
#if defined(__APPLE__)
        kw = cl_intern_keyword("DARWIN", 6);
        features = cl_cons(kw, features);
#elif defined(__linux__)
        kw = cl_intern_keyword("LINUX", 5);
        features = cl_cons(kw, features);
#elif defined(PLATFORM_WIN32)
        /* The Windows build keeps :UNIX (paths are spelled with `/`), so
         * :WINDOWS is what tells it apart -- a list of directories in an
         * environment variable is separated by `;` there, since every
         * entry carries a `:` after its drive letter (lib/asdf.lisp,
         * INTER-DIRECTORY-SEPARATOR). */
        kw = cl_intern_keyword("WINDOWS", 7);
        features = cl_cons(kw, features);
#endif
        /* Host CPU architecture — cl+ssl keys homebrew detection on :ARM64 */
#if defined(__aarch64__) || defined(__arm64__)
        kw = cl_intern_keyword("ARM64", 5);
        features = cl_cons(kw, features);
#elif defined(__x86_64__) || defined(_M_X64)
        kw = cl_intern_keyword("X86-64", 6);
        features = cl_cons(kw, features);
#endif
#endif
        /* Word size and endianness of the RUNNING binary.  Portability
         * layers (trivial-features) and FFI users (CFFI sizes :SIZE/:SSIZE
         * via #+64-bit/#+32-bit, babel picks codecs via endianness) expect
         * the implementation to provide these — SBCL/CCL do.  Deriving them
         * here from the compiler makes feature-conditional code work even
         * with an unpatched trivial-features. */
        if (sizeof(void *) == 8)
            kw = cl_intern_keyword("64-BIT", 6);
        else
            kw = cl_intern_keyword("32-BIT", 6);
        features = cl_cons(kw, features);
        {
            union { uint32_t u; uint8_t b[4]; } ec;
            ec.u = 1;
            if (ec.b[0] == 1)
                kw = cl_intern_keyword("LITTLE-ENDIAN", 13);
            else
                kw = cl_intern_keyword("BIG-ENDIAN", 10);
            features = cl_cons(kw, features);
        }
        features = cl_cons(KW_COMMON_LISP, features);
        /* The implementation's own features: :AMILISP and :AMICL are the
         * names since the rename to AmiLisp (0.12); :CL-AMIGA and :CLAMIGA
         * are the names before it and stay so existing #+ conditionals in
         * user code and the library forks keep working. */
        features = cl_cons(KW_CL_AMIGA, features);
        {
            CL_Obj kw = cl_intern_keyword("CLAMIGA", 7);
            features = cl_cons(kw, features);
            kw = cl_intern_keyword("AMICL", 5);
            features = cl_cons(kw, features);
            kw = cl_intern_keyword("AMILISP", 7);
            features = cl_cons(kw, features);
        }
#ifdef CL_WIDE_STRINGS
        {
            CL_Obj kw_unicode = cl_intern_keyword("UNICODE", 7);
            features = cl_cons(kw_unicode, features);
        }
#endif
        /* Re-derive s: the cl_cons/cl_intern_keyword calls above can
         * trigger a compacting GC, which would leave a pointer fetched
         * before the loop stale. */
        s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_FEATURES);
        s->value = features;
        CL_GC_UNPROTECT(1);
    }
    /* Reader dereferences this handle on every #+ / #- — forward it across
     * compaction (see SYM_STAR_READTABLE note below). */
    cl_gc_register_root(&SYM_STAR_FEATURES);
    /* The CPU of this machine loses memory stores (the startup self-test,
     * main.c): let #+cpu-lost-stores and test suites know.  After the root
     * registration, since it allocates; main.c repeats it after an image
     * restore. */
    cl_features_sync_cpu_lost_stores();

    /* LAMBDA-LIST-KEYWORDS — CL constant */
    {
        CL_Obj sym = cl_intern_in("LAMBDA-LIST-KEYWORDS", 20, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        CL_Obj kws = CL_NIL;
        kws = cl_cons(SYM_AMP_WHOLE, kws);
        kws = cl_cons(SYM_AMP_ENVIRONMENT, kws);
        kws = cl_cons(SYM_AMP_AUX, kws);
        kws = cl_cons(SYM_AMP_ALLOW_OTHER_KEYS, kws);
        kws = cl_cons(SYM_AMP_KEY, kws);
        kws = cl_cons(SYM_AMP_BODY, kws);
        kws = cl_cons(SYM_AMP_REST, kws);
        kws = cl_cons(SYM_AMP_OPTIONAL, kws);
        s->value = kws;
        s->flags |= CL_SYM_CONSTANT;
    }

    /* *LOAD-VERBOSE*, *LOAD-PRINT*, *LOAD-PATHNAME*, *LOAD-TRUENAME* */
    {
        CL_Obj sym;
        CL_Symbol *s;
        sym = cl_intern_in("*LOAD-VERBOSE*", 14, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = SYM_T;
        sym = cl_intern_in("*LOAD-PRINT*", 12, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        sym = cl_intern_in("*LOAD-PATHNAME*", 15, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        sym = cl_intern_in("*LOAD-TRUENAME*", 15, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        sym = cl_intern_in("*COMPILE-VERBOSE*", 17, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
        sym = cl_intern_in("*COMPILE-PRINT*", 15, cl_package_cl);
        s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->flags |= CL_SYM_SPECIAL; s->value = CL_NIL;
    }

    /* T is self-evaluating */
    {
        CL_Symbol *t = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_T);
        t->value = SYM_T;
    }

    /* NIL's value is NIL */
    {
        CL_Symbol *n = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_NIL);
        n->value = CL_NIL;
    }

    /* Set the global CL_T for use by type predicates.  CL_T is a SEPARATE
     * global from SYM_T (types.c), so it needs its OWN root registration:
     * the compactor forwards registered addresses, not values — an
     * unregistered CL_T silently went stale the first time a compaction
     * moved the T symbol.  Latent for years because boot's allocation
     * layout happened to leave no garbage below T (so it never moved);
     * exposed the moment early-boot allocation patterns changed (tier-4
     * batch 6: pre-consed package cells), when every builtin returning
     * CL_T started returning whatever object landed at T's old offset. */
    CL_T = SYM_T;
    cl_gc_register_root(&CL_T);

    /* *PACKAGE* is a special variable initialized to CL-USER */
    {
        CL_Symbol *pkg_sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_PACKAGE);
        pkg_sym->value = cl_current_package;
        pkg_sym->flags |= CL_SYM_SPECIAL;
    }

    /* *READTABLE* — readtable pool index, initial = 1 (user-modifiable copy) */
    SYM_STAR_READTABLE = cl_intern_in("*READTABLE*", 11, cl_package_cl);
    {
        CL_Symbol *rt_sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STAR_READTABLE);
        rt_sym->value = CL_MAKE_FIXNUM(1);
        rt_sym->flags |= CL_SYM_SPECIAL;
    }
    /* The C reader/runtime dereferences SYM_STAR_READTABLE by this global
     * handle (resolve_readtable_idx, cl_readtable_current).  The *READTABLE*
     * symbol is interned late enough that a moving compaction can relocate it;
     * register the handle so the compactor forwards it, otherwise the reader
     * reads a stale symbol and loses user dispatch macros (e.g. ironclad's
     * #n@ array reader) after the first compaction. */
    cl_gc_register_root(&SYM_STAR_READTABLE);

    /* Standard CL symbols that have no global binding but must be exported.
       These are used as macrolet names in boot.lisp macros (loop, pprint-logical-block)
       and must be visible to user code for the macrolets to match. */
    cl_intern_in("LOOP-FINISH", 11, cl_package_cl);
    cl_intern_in("PPRINT-POP", 10, cl_package_cl);
    cl_intern_in("PPRINT-EXIT-IF-LIST-EXHAUSTED", 29, cl_package_cl);


    /* Register EVERY cached symbol handle above as a GC root.  These C
     * globals hold raw arena offsets; the moving compactor only rewrites
     * offsets reachable through registered roots, so an unregistered
     * handle silently goes stale the first time a compaction relocates
     * its symbol — identity comparisons (pathname keywords, print-hook
     * dispatch) then match the wrong object.  Registration is idempotent,
     * so the three handles registered individually above are unaffected.
     * Generated from the CL_Obj SYM_ / KW_ definitions at the top of
     * this file — keep in sync when adding a handle. */
    cl_gc_register_root(&SYM_T);
    cl_gc_register_root(&SYM_NIL);
    cl_gc_register_root(&SYM_QUOTE);
    cl_gc_register_root(&SYM_IF);
    cl_gc_register_root(&SYM_PROGN);
    cl_gc_register_root(&SYM_LAMBDA);
    cl_gc_register_root(&SYM_LET);
    cl_gc_register_root(&SYM_LETSTAR);
    cl_gc_register_root(&SYM_SETQ);
    cl_gc_register_root(&SYM_DEFUN);
    cl_gc_register_root(&SYM_DEFMACRO);
    cl_gc_register_root(&SYM_FUNCTION);
    cl_gc_register_root(&SYM_BLOCK);
    cl_gc_register_root(&SYM_RETURN_FROM);
    cl_gc_register_root(&SYM_QUASIQUOTE);
    cl_gc_register_root(&SYM_UNQUOTE);
    cl_gc_register_root(&SYM_UNQUOTE_SPLICING);
    cl_gc_register_root(&SYM_AMP_REST);
    cl_gc_register_root(&SYM_AMP_OPTIONAL);
    cl_gc_register_root(&SYM_AMP_BODY);
    cl_gc_register_root(&SYM_AND);
    cl_gc_register_root(&SYM_OR);
    cl_gc_register_root(&SYM_COND);
    cl_gc_register_root(&SYM_DO);
    cl_gc_register_root(&SYM_DO_STAR);
    cl_gc_register_root(&SYM_DOLIST);
    cl_gc_register_root(&SYM_DOTIMES);
    cl_gc_register_root(&SYM_RETURN);
    cl_gc_register_root(&SYM_CASE);
    cl_gc_register_root(&SYM_ECASE);
    cl_gc_register_root(&SYM_TYPECASE);
    cl_gc_register_root(&SYM_ETYPECASE);
    cl_gc_register_root(&SYM_OTHERWISE);
    cl_gc_register_root(&SYM_PUSH);
    cl_gc_register_root(&SYM_POP);
    cl_gc_register_root(&SYM_FLET);
    cl_gc_register_root(&SYM_LABELS);
    cl_gc_register_root(&SYM_AMP_KEY);
    cl_gc_register_root(&SYM_AMP_ALLOW_OTHER_KEYS);
    cl_gc_register_root(&SYM_AMP_AUX);
    cl_gc_register_root(&SYM_AMP_ENVIRONMENT);
    cl_gc_register_root(&SYM_AMP_WHOLE);
    cl_gc_register_root(&KW_ALLOW_OTHER_KEYS);
    cl_gc_register_root(&SYM_TAGBODY);
    cl_gc_register_root(&SYM_GO);
    cl_gc_register_root(&SYM_CATCH);
    cl_gc_register_root(&SYM_THROW);
    cl_gc_register_root(&SYM_UNWIND_PROTECT);
    cl_gc_register_root(&SYM_MULTIPLE_VALUE_BIND);
    cl_gc_register_root(&SYM_MULTIPLE_VALUE_CALL);
    cl_gc_register_root(&SYM_MULTIPLE_VALUE_LIST);
    cl_gc_register_root(&SYM_MULTIPLE_VALUE_PROG1);
    cl_gc_register_root(&SYM_NTH_VALUE);
    cl_gc_register_root(&SYM_DEFVAR);
    cl_gc_register_root(&SYM_DEFPARAMETER);
    cl_gc_register_root(&SYM_DEFCONSTANT);
    cl_gc_register_root(&SYM_NAMED_LAMBDA);
    cl_gc_register_root(&SYM_SETF);
    cl_gc_register_root(&SYM_EVAL_WHEN);
    cl_gc_register_root(&SYM_LOAD_TIME_VALUE);
    cl_gc_register_root(&SYM_DESTRUCTURING_BIND);
    cl_gc_register_root(&SYM_DEFSETF);
    cl_gc_register_root(&SYM_DEFTYPE);
    cl_gc_register_root(&SYM_DECLARE);
    cl_gc_register_root(&SYM_DECLAIM);
    cl_gc_register_root(&SYM_LOCALLY);
    cl_gc_register_root(&SYM_TRACE);
    cl_gc_register_root(&SYM_UNTRACE);
    cl_gc_register_root(&SYM_TIME);
    cl_gc_register_root(&SYM_IN_PACKAGE);
    cl_gc_register_root(&SYM_STAR_PACKAGE);
    cl_gc_register_root(&SYM_STAR_LOAD_PATHNAME);
    cl_gc_register_root(&SYM_STAR_LOAD_TRUENAME);
    cl_gc_register_root(&SYM_STAR_LOAD_VERBOSE);
    cl_gc_register_root(&SYM_STAR_GENSYM_COUNTER);
    cl_gc_register_root(&SYM_STAR_COMPILE_FILE_PATHNAME);
    cl_gc_register_root(&SYM_STAR_COMPILE_FILE_TRUENAME);
    cl_gc_register_root(&SYM_STAR_COMPILE_VERBOSE);
    cl_gc_register_root(&SYM_MACROLET);
    cl_gc_register_root(&SYM_SYMBOL_MACROLET);
    cl_gc_register_root(&SYM_THE);
    cl_gc_register_root(&SYM_PROGV);
    cl_gc_register_root(&SYM_FUNCALL);
    cl_gc_register_root(&SYM_APPLY);
    cl_gc_register_root(&SYM_SPECIAL_DECL);
    cl_gc_register_root(&SYM_TYPE_DECL);
    cl_gc_register_root(&SYM_FTYPE_DECL);
    cl_gc_register_root(&SYM_INLINE_DECL);
    cl_gc_register_root(&SYM_NOTINLINE_DECL);
    cl_gc_register_root(&SYM_OPTIMIZE_DECL);
    cl_gc_register_root(&SYM_IGNORE_DECL);
    cl_gc_register_root(&SYM_IGNORABLE_DECL);
    cl_gc_register_root(&SYM_DYNAMIC_EXTENT_DECL);
    cl_gc_register_root(&SYM_SPEED);
    cl_gc_register_root(&SYM_SAFETY);
    cl_gc_register_root(&SYM_DEBUG);
    cl_gc_register_root(&SYM_SPACE);
    cl_gc_register_root(&SYM_CONDITION);
    cl_gc_register_root(&SYM_WARNING);
    cl_gc_register_root(&SYM_SERIOUS_CONDITION);
    cl_gc_register_root(&SYM_ERROR_COND);
    cl_gc_register_root(&SYM_SIMPLE_CONDITION);
    cl_gc_register_root(&SYM_SIMPLE_ERROR);
    cl_gc_register_root(&SYM_SIMPLE_WARNING);
    cl_gc_register_root(&SYM_TYPE_ERROR);
    cl_gc_register_root(&SYM_SIMPLE_TYPE_ERROR);
    cl_gc_register_root(&SYM_UNBOUND_VARIABLE_COND);
    cl_gc_register_root(&SYM_UNDEFINED_FUNCTION_COND);
    cl_gc_register_root(&SYM_PROGRAM_ERROR);
    cl_gc_register_root(&SYM_CONTROL_ERROR);
    cl_gc_register_root(&SYM_ARITHMETIC_ERROR);
    cl_gc_register_root(&SYM_DIVISION_BY_ZERO);
    cl_gc_register_root(&SYM_FLOATING_POINT_OVERFLOW);
    cl_gc_register_root(&SYM_FLOATING_POINT_UNDERFLOW);
    cl_gc_register_root(&SYM_FLOATING_POINT_INEXACT);
    cl_gc_register_root(&SYM_FLOATING_POINT_INVALID);
    cl_gc_register_root(&SYM_STYLE_WARNING);
    cl_gc_register_root(&SYM_CELL_ERROR);
    cl_gc_register_root(&SYM_UNBOUND_SLOT);
    cl_gc_register_root(&SYM_STREAM_ERROR);
    cl_gc_register_root(&SYM_END_OF_FILE);
    cl_gc_register_root(&SYM_SOCKET_TIMEOUT);
    cl_gc_register_root(&SYM_FILE_ERROR);
    cl_gc_register_root(&SYM_PACKAGE_ERROR);
    cl_gc_register_root(&SYM_PARSE_ERROR);
    cl_gc_register_root(&SYM_READER_ERROR);
    cl_gc_register_root(&SYM_PRINT_NOT_READABLE);
    cl_gc_register_root(&SYM_STORAGE_CONDITION);
    cl_gc_register_root(&SYM_SIGNAL);
    cl_gc_register_root(&SYM_WARN);
    cl_gc_register_root(&SYM_HANDLER_BIND);
    cl_gc_register_root(&SYM_RESTART_CASE);
    cl_gc_register_root(&SYM_ABORT);
    cl_gc_register_root(&SYM_CONTINUE);
    cl_gc_register_root(&SYM_MUFFLE_WARNING);
    cl_gc_register_root(&SYM_USE_VALUE);
    cl_gc_register_root(&SYM_STORE_VALUE);
    cl_gc_register_root(&KW_FORMAT_CONTROL);
    cl_gc_register_root(&KW_FORMAT_ARGUMENTS);
    cl_gc_register_root(&KW_DATUM);
    cl_gc_register_root(&KW_EXPECTED_TYPE);
    cl_gc_register_root(&KW_STREAM);
    cl_gc_register_root(&KW_PACKAGE);
    cl_gc_register_root(&KW_OPERANDS);
    cl_gc_register_root(&KW_OPERATION);
    cl_gc_register_root(&KW_PATHNAME);
    cl_gc_register_root(&KW_INSTANCE);
    cl_gc_register_root(&SYM_STAR);
    cl_gc_register_root(&SYM_STARSTAR);
    cl_gc_register_root(&SYM_STARSTARSTAR);
    cl_gc_register_root(&SYM_PLUS);
    cl_gc_register_root(&SYM_PLUSPLUS);
    cl_gc_register_root(&SYM_PLUSPLUSPLUS);
    cl_gc_register_root(&SYM_MINUS);
    cl_gc_register_root(&SYM_SLASH);
    cl_gc_register_root(&SYM_SLASHSLASH);
    cl_gc_register_root(&SYM_SLASHSLASHSLASH);
    cl_gc_register_root(&SYM_STANDARD_INPUT);
    cl_gc_register_root(&SYM_STANDARD_OUTPUT);
    cl_gc_register_root(&SYM_ERROR_OUTPUT);
    cl_gc_register_root(&SYM_TRACE_OUTPUT);
    cl_gc_register_root(&SYM_DEBUG_IO);
    cl_gc_register_root(&SYM_QUERY_IO);
    cl_gc_register_root(&SYM_TERMINAL_IO);
    cl_gc_register_root(&SYM_STAR_FEATURES);
    cl_gc_register_root(&SYM_STAR_READTABLE);
    cl_gc_register_root(&SYM_PRINT_ESCAPE);
    cl_gc_register_root(&SYM_PRINT_READABLY);
    cl_gc_register_root(&SYM_PRINT_BASE);
    cl_gc_register_root(&SYM_PRINT_RADIX);
    cl_gc_register_root(&SYM_PRINT_LEVEL);
    cl_gc_register_root(&SYM_PRINT_LENGTH);
    cl_gc_register_root(&SYM_PRINT_CASE);
    cl_gc_register_root(&SYM_PRINT_GENSYM);
    cl_gc_register_root(&SYM_PRINT_ARRAY);
    cl_gc_register_root(&SYM_PRINT_CIRCLE);
    cl_gc_register_root(&SYM_PRINT_PRETTY);
    cl_gc_register_root(&SYM_PRINT_RIGHT_MARGIN);
    cl_gc_register_root(&SYM_PRINT_PPRINT_DISPATCH);
    cl_gc_register_root(&SYM_PRINT_OBJECT_HOOK);
    cl_gc_register_root(&SYM_READ_DEFAULT_FLOAT_FORMAT);
    cl_gc_register_root(&SYM_READ_SUPPRESS);
    cl_gc_register_root(&KW_UPCASE);
    cl_gc_register_root(&KW_DOWNCASE);
    cl_gc_register_root(&KW_CAPITALIZE);
    cl_gc_register_root(&SYM_BOOLE_CLR);
    cl_gc_register_root(&SYM_BOOLE_SET);
    cl_gc_register_root(&SYM_BOOLE_1);
    cl_gc_register_root(&SYM_BOOLE_2);
    cl_gc_register_root(&SYM_BOOLE_C1);
    cl_gc_register_root(&SYM_BOOLE_C2);
    cl_gc_register_root(&SYM_BOOLE_AND);
    cl_gc_register_root(&SYM_BOOLE_IOR);
    cl_gc_register_root(&SYM_BOOLE_XOR);
    cl_gc_register_root(&SYM_BOOLE_EQV);
    cl_gc_register_root(&SYM_BOOLE_NAND);
    cl_gc_register_root(&SYM_BOOLE_NOR);
    cl_gc_register_root(&SYM_BOOLE_ANDC1);
    cl_gc_register_root(&SYM_BOOLE_ANDC2);
    cl_gc_register_root(&SYM_BOOLE_ORC1);
    cl_gc_register_root(&SYM_BOOLE_ORC2);
    cl_gc_register_root(&SYM_RANDOM_STATE);
    cl_gc_register_root(&SYM_PATHNAME);
    cl_gc_register_root(&KW_ABSOLUTE);
    cl_gc_register_root(&KW_RELATIVE);
    cl_gc_register_root(&KW_NEWEST);
    cl_gc_register_root(&KW_HOST);
    cl_gc_register_root(&KW_DEVICE);
    cl_gc_register_root(&KW_DIRECTORY);
    cl_gc_register_root(&KW_NAME);
    cl_gc_register_root(&KW_TYPE);
    cl_gc_register_root(&KW_VERSION);
    cl_gc_register_root(&KW_DEFAULTS);
    cl_gc_register_root(&KW_UNSPECIFIC);
    cl_gc_register_root(&KW_WILD);
    cl_gc_register_root(&SYM_STAR_DEFAULT_PATHNAME_DEFAULTS);
    cl_gc_register_root(&SYM_STAR_READ_EVAL);
    cl_gc_register_root(&SYM_STAR_MODULES);
    cl_gc_register_root(&KW_CL_AMIGA);
    cl_gc_register_root(&KW_COMMON_LISP);
    cl_gc_register_root(&KW_POSIX);
    cl_gc_register_root(&KW_UNIX);
    cl_gc_register_root(&KW_AMIGAOS);
    cl_gc_register_root(&KW_M68K);

    /* CLAMIGA::%EXPANDING-FORM / CLAMIGA::%ERROR-EXPANDING-FORM (see
     * error.h) — hidden TLV keys for the macroexpansion error context.
     * Interned here (not in cl_error_init, which runs before packages
     * exist) and never reassigned afterward, so registering them as GC
     * roots is a one-time, race-free write, same as every other symbol
     * above. */
    cl_expanding_form_sym = cl_intern_in("%EXPANDING-FORM",
                                          (uint32_t)(sizeof("%EXPANDING-FORM") - 1),
                                          cl_package_clamiga);
    cl_error_expanding_form_sym = cl_intern_in("%ERROR-EXPANDING-FORM",
                                                (uint32_t)(sizeof("%ERROR-EXPANDING-FORM") - 1),
                                                cl_package_clamiga);
    cl_gc_register_root(&cl_expanding_form_sym);
    cl_gc_register_root(&cl_error_expanding_form_sym);

    /* Export all CL symbols so CL-USER inherits them.
       Also called again after cl_builtins_init() for builtin names. */
    cl_package_export_all_cl_symbols();
}
