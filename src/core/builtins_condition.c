/*
 * builtins_condition.c — Condition type infrastructure
 *
 * Provides condition type hierarchy, constructors, accessors,
 * and type predicates for the CL condition system.
 */

#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include <stdio.h>
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "compiler.h"
#include "fasl.h"
#include "reader.h"
#include "printer.h"
#include "color.h"
#include "stream.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>

/* Defined in builtins_format.c */
extern void cl_format_to_stream(CL_Obj stream, CL_Obj *args, int n);

/*
 * Condition type hierarchy (CL spec):
 *
 * condition
 *   simple-condition
 *   warning
 *     simple-warning        (also simple-condition)
 *     style-warning
 *   serious-condition
 *     storage-condition
 *     error
 *       simple-error        (also simple-condition)
 *       type-error
 *       cell-error
 *         unbound-slot
 *       unbound-variable
 *       undefined-function
 *       program-error
 *       control-error
 *       arithmetic-error
 *         division-by-zero
 *       stream-error
 *         end-of-file
 *       file-error
 *       package-error
 *       parse-error
 *       print-not-readable
 *
 * Stored as alist: ((type-sym parent-sym ...) ...)
 * Multiple parents for types with multiple inheritance.
 */
CL_Obj condition_hierarchy = CL_NIL;

/* Hash index over condition_hierarchy (see CL_AlistIndex in compiler.h).
 * TYPEP calls cl_is_condition_type for EVERY symbol type spec that is not
 * a struct type — for non-condition symbols that was a full O(conditions)
 * walk per call.  With eta-hab's ~hundreds of registered conditions and a
 * print-object hook doing one TYPEP per printed node, a single log4cl log
 * line took ~60s while holding the appender lock, stalling every logging
 * thread (diagnosed 2026-07-14 via AMILISP_LOCK_DIAG + in-container gdb). */
static CL_AlistIndex cond_index = { &condition_hierarchy, NULL, 0, 0, 0 };

/* Compaction moved the index's key symbols and entry conses — mark it
 * stale (called from the compaction update phase, world stopped). */
void cl_condition_index_gc_invalidate(void)
{
    cl_alist_index_invalidate(&cond_index);
}

/* Slot table for user-defined condition types:
 * ((type-name . ((slot-name . :initarg) ...)) ...) */
CL_Obj condition_slot_table = CL_NIL;

/* Default-initargs for user condition types (define-condition's
 * :default-initargs option):
 *   ((type-name initarg1 val1 initarg2 val2 ...) ...)
 * The values are stored as literal objects — the define-condition macro
 * passes them quoted — so an init *form* like (:x (foo)) would be taken
 * verbatim rather than re-evaluated per instance.  This covers the literal
 * values (strings/keywords/numbers) that conditions use in practice; full
 * per-instance form evaluation is intentionally not implemented. */
CL_Obj condition_default_initargs = CL_NIL;

/* Slot :initform thunks for user condition types (define-condition slot
 * :initform option):
 *   ((type-name (slot-name initarg-or-nil thunk) ...) ...)
 * Each thunk is a zero-argument closure that evaluates the slot's initform in
 * the lexical environment of the define-condition.  Unlike :default-initargs,
 * initforms are re-evaluated per instance at make-condition time, which is
 * required for forms that read dynamic state (e.g. fiasco's CONTEXT slot whose
 * initform is *CONTEXT*).  A slot whose value was supplied via its initarg (or
 * already present by slot name) is left untouched.  Slots that have no initarg
 * are keyed in the instance's slots alist by their SLOT NAME. */
CL_Obj condition_slot_initforms = CL_NIL;

/* Fill in any initarg the caller of make-condition / error / signal omitted
 * from TYPE_SYM's (and its ancestors') registered :default-initargs.
 * Defined after the hierarchy walker below; forward-declared here because
 * bi_make_condition (above it) also calls it. */
static CL_Obj merge_default_initargs(CL_Obj type_sym, CL_Obj slots,
                                     CL_Obj *report_string);
/* Likewise forward-declared: evaluates registered slot :initform thunks for
 * any slot the caller left unsupplied (defined after the hierarchy walker). */
static CL_Obj apply_condition_slot_initforms(CL_Obj type_sym, CL_Obj slots);

/* Push one (type parent1 parent2 ...) entry.  The entry is built by the
 * caller before this runs, so the global is read only after that
 * allocation (see cl_list2 in mem.h). */
static void hierarchy_add(CL_Obj entry)
{
    condition_hierarchy = cl_cons(entry, condition_hierarchy);
}

/* Build the hierarchy alist during init, bottom-up so it is a proper alist */
static void build_hierarchy(void)
{
    /* print-not-readable -> error */
    hierarchy_add(cl_list2(SYM_PRINT_NOT_READABLE, SYM_ERROR_COND));

    /* reader-error -> parse-error, stream-error */
    hierarchy_add(cl_list3(SYM_READER_ERROR, SYM_PARSE_ERROR, SYM_STREAM_ERROR));

    /* parse-error -> error */
    hierarchy_add(cl_list2(SYM_PARSE_ERROR, SYM_ERROR_COND));

    /* package-error -> error */
    hierarchy_add(cl_list2(SYM_PACKAGE_ERROR, SYM_ERROR_COND));

    /* file-error -> error */
    hierarchy_add(cl_list2(SYM_FILE_ERROR, SYM_ERROR_COND));

    /* end-of-file -> stream-error */
    hierarchy_add(cl_list2(SYM_END_OF_FILE, SYM_STREAM_ERROR));

    /* socket-timeout (EXT) -> stream-error: a socket read/write deadline
     * elapsed.  A subtype of STREAM-ERROR so generic stream handlers catch it. */
    hierarchy_add(cl_list2(SYM_SOCKET_TIMEOUT, SYM_STREAM_ERROR));

    /* stream-error -> error */
    hierarchy_add(cl_list2(SYM_STREAM_ERROR, SYM_ERROR_COND));

    /* unbound-slot -> cell-error */
    hierarchy_add(cl_list2(SYM_UNBOUND_SLOT, SYM_CELL_ERROR));

    /* undefined-function -> cell-error (CLHS) */
    hierarchy_add(cl_list2(SYM_UNDEFINED_FUNCTION_COND, SYM_CELL_ERROR));

    /* unbound-variable -> cell-error (CLHS) */
    hierarchy_add(cl_list2(SYM_UNBOUND_VARIABLE_COND, SYM_CELL_ERROR));

    /* cell-error -> error */
    hierarchy_add(cl_list2(SYM_CELL_ERROR, SYM_ERROR_COND));

    /* storage-condition -> serious-condition */
    hierarchy_add(cl_list2(SYM_STORAGE_CONDITION, SYM_SERIOUS_CONDITION));

    /* division-by-zero -> arithmetic-error */
    hierarchy_add(cl_list2(SYM_DIVISION_BY_ZERO, SYM_ARITHMETIC_ERROR));

    /* floating-point-overflow -> arithmetic-error */
    hierarchy_add(cl_list2(SYM_FLOATING_POINT_OVERFLOW, SYM_ARITHMETIC_ERROR));

    /* floating-point-underflow -> arithmetic-error */
    hierarchy_add(cl_list2(SYM_FLOATING_POINT_UNDERFLOW, SYM_ARITHMETIC_ERROR));

    /* floating-point-inexact -> arithmetic-error */
    hierarchy_add(cl_list2(SYM_FLOATING_POINT_INEXACT, SYM_ARITHMETIC_ERROR));

    /* floating-point-invalid-operation -> arithmetic-error */
    hierarchy_add(cl_list2(SYM_FLOATING_POINT_INVALID, SYM_ARITHMETIC_ERROR));

    /* arithmetic-error -> error */
    hierarchy_add(cl_list2(SYM_ARITHMETIC_ERROR, SYM_ERROR_COND));

    /* control-error -> error */
    hierarchy_add(cl_list2(SYM_CONTROL_ERROR, SYM_ERROR_COND));

    /* program-error -> error */
    hierarchy_add(cl_list2(SYM_PROGRAM_ERROR, SYM_ERROR_COND));

    /* type-error -> error */
    hierarchy_add(cl_list2(SYM_TYPE_ERROR, SYM_ERROR_COND));

    /* simple-type-error -> type-error, simple-condition */
    hierarchy_add(cl_list3(SYM_SIMPLE_TYPE_ERROR, SYM_TYPE_ERROR, SYM_SIMPLE_CONDITION));

    /* simple-error -> error, simple-condition */
    hierarchy_add(cl_list3(SYM_SIMPLE_ERROR, SYM_ERROR_COND, SYM_SIMPLE_CONDITION));

    /* style-warning -> warning */
    hierarchy_add(cl_list2(SYM_STYLE_WARNING, SYM_WARNING));

    /* simple-warning -> warning, simple-condition */
    hierarchy_add(cl_list3(SYM_SIMPLE_WARNING, SYM_WARNING, SYM_SIMPLE_CONDITION));

    /* error -> serious-condition */
    hierarchy_add(cl_list2(SYM_ERROR_COND, SYM_SERIOUS_CONDITION));

    /* serious-condition -> condition */
    hierarchy_add(cl_list2(SYM_SERIOUS_CONDITION, SYM_CONDITION));

    /* simple-condition -> condition */
    hierarchy_add(cl_list2(SYM_SIMPLE_CONDITION, SYM_CONDITION));

    /* warning -> condition */
    hierarchy_add(cl_list2(SYM_WARNING, SYM_CONDITION));

    /* condition -> (no parent) */
    hierarchy_add(cl_cons(SYM_CONDITION, CL_NIL));
}

/* Look up parent list for a type in the hierarchy alist.
 * Returns CDR of the matching entry (list of parents), or NIL.
 * O(1) via the hash index (linear-walk fallback inside _find). */
static CL_Obj find_parents(CL_Obj type_sym)
{
    CL_Obj entry = cl_alist_index_find(&cond_index, type_sym);
    return CL_NULL_P(entry) ? CL_NIL : cl_cdr(entry);
}

/* Check if cond_type is a subtype of (or equal to) handler_type.
 * Walks the condition hierarchy and, for a handler_type that is a user
 * deftype alias (or a compound OR/AND of type names), expands it so that
 * e.g. (handler-case ... (my-error-alias () ...)) — where my-error-alias is
 * (deftype my-error-alias () 'simple-error) — still matches a SIMPLE-ERROR.
 * DEPTH bounds the deftype-expansion recursion. */
static int cond_type_matches_depth(CL_Obj cond_type, CL_Obj handler_type, int depth)
{
    CL_Obj parents;

    /* T is the universal supertype — every condition is (typep cond T).
     * Frameworks rely on `(handler-case ... (t (c) ...))` as a catch-all. */
    if (handler_type == SYM_T)
        return 1;

    /* Identity check */
    if (cond_type == handler_type)
        return 1;

    /* Compound type specifier on the handler side: (or ...) / (and ...).
     * A deftype alias may expand to one of these (e.g. portability's
     * no-applicable-method-error type when an implementation signals a
     * compound condition type).
     *
     * GC safety: the recursive cond_type_matches_depth call can reach the
     * deftype expansion path and invoke cl_vm_apply, which allocates and may
     * compact the heap.  Protect `cond_type` and the list cursor `rest` so the
     * compactor forwards them; without this, `rest = cl_cdr(rest)` after the
     * recursive call dereferences a stale arena offset. */
    if (CL_CONS_P(handler_type)) {
        CL_Obj head = cl_car(handler_type);
        CL_Obj rest = cl_cdr(handler_type);
        if (head == SYM_OR) {
            int match = 0;
            CL_GC_PROTECT(cond_type);
            CL_GC_PROTECT(rest);
            while (!CL_NULL_P(rest)) {
                if (cond_type_matches_depth(cond_type, cl_car(rest), depth)) {
                    match = 1;
                    break;
                }
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(2);
            return match;
        }
        if (head == SYM_AND) {
            if (CL_NULL_P(rest))
                return 1; /* (and) ≡ T */
            {
                int match = 1;
                CL_GC_PROTECT(cond_type);
                CL_GC_PROTECT(rest);
                while (!CL_NULL_P(rest)) {
                    if (!cond_type_matches_depth(cond_type, cl_car(rest), depth)) {
                        match = 0;
                        break;
                    }
                    rest = cl_cdr(rest);
                }
                CL_GC_UNPROTECT(2);
                return match;
            }
        }
        /* Other compounds (satisfies, not, member, …) aren't expressible
         * through the condition hierarchy — decline. */
        return 0;
    }

    /* Walk parent chain.
     * GC safety: the recursive call can compact the heap (deftype expansion
     * path via cl_vm_apply); protect both `parents` (cursor) and `handler_type`
     * (used in the deftype expansion check below after the loop). */
    parents = find_parents(cond_type);
    {
        int found = 0;
        CL_GC_PROTECT(parents);
        CL_GC_PROTECT(handler_type);
        while (!CL_NULL_P(parents)) {
            if (cond_type_matches_depth(cl_car(parents), handler_type, depth)) {
                found = 1;
                break;
            }
            parents = cl_cdr(parents);
        }
        CL_GC_UNPROTECT(2);
        if (found) return 1;
    }

    /* No match through the class hierarchy.  If handler_type names a user
     * deftype, expand it once and retry — this is what makes a deftype that
     * aliases a condition class usable as a handler/handler-case clause. */
    if (depth < 8 && CL_SYMBOL_P(handler_type)) {
        CL_Obj expander = cl_get_type_expander(handler_type);
        if (!CL_NULL_P(expander)) {
            CL_Obj expanded;
            /* handler_type must be forwarded too: the identity guard below
             * compares it against expanded, and a stale offset would make
             * the comparison misfire (one wrong expansion decision). */
            CL_GC_PROTECT(cond_type);
            CL_GC_PROTECT(handler_type);
            expanded = cl_vm_apply(expander, NULL, 0);
            CL_GC_UNPROTECT(2);
            if (!CL_NULL_P(expanded) && expanded != handler_type)
                return cond_type_matches_depth(cond_type, expanded, depth + 1);
        }
    }

    return 0;
}

int cl_condition_type_matches(CL_Obj cond_type, CL_Obj handler_type)
{
    return cond_type_matches_depth(cond_type, handler_type, 0);
}

/* --- Slot lookup helper --- */

static CL_Obj slot_lookup(CL_Obj slots, CL_Obj key)
{
    while (!CL_NULL_P(slots)) {
        CL_Obj pair = cl_car(slots);
        if (cl_car(pair) == key)
            return cl_cdr(pair);
        slots = cl_cdr(slots);
    }
    return CL_NIL;
}

/* True if KEY appears as the car of any pair in the SLOTS alist.
 * Unlike slot_lookup this distinguishes "present with value NIL" from
 * "absent" — needed so a merged default-initarg never clobbers an initarg
 * the caller explicitly supplied as NIL. */
static int slot_present_p(CL_Obj slots, CL_Obj key)
{
    while (!CL_NULL_P(slots)) {
        if (cl_car(cl_car(slots)) == key)
            return 1;
        slots = cl_cdr(slots);
    }
    return 0;
}

/* FORMAT a C control string with up to three condition-slot arguments into
 * a fresh Lisp string.  Same rooting idiom as format_condition_report:
 * everything is copied into args_buf and rooted BEFORE the first
 * allocation, since the string stream and the formatter both compact. */
static CL_Obj format_default_report(const char *ctl, const CL_Obj *argv, int argc)
{
    CL_Obj args_buf[5];  /* [dest, ctl, up to 3 args] */
    CL_Obj sstream, result;
    int i;
    int dyn_mark = cl_dyn_top;

    args_buf[0] = CL_NIL;
    args_buf[1] = CL_NIL;
    for (i = 0; i < argc; i++)
        args_buf[2 + i] = argv[i];
    for (i = 0; i < 2 + argc; i++)
        cl_gc_push_root(&args_buf[i]);

    args_buf[1] = cl_make_string(ctl, (uint32_t)strlen(ctl));
    sstream = cl_make_string_output_stream();
    CL_GC_PROTECT(sstream);
    args_buf[0] = sstream;
    /* The arguments are slot values -- any object the program put there.
     * ERROR formats this report before it signals, so a circular datum
     * ((error 'type-error :datum (circular-list 1 2 3) ...), alexandria's
     * LASTCAR) would print forever and never reach the handler. */
    cl_dynbind_c(SYM_PRINT_CIRCLE, SYM_T);
    cl_format_to_stream(sstream, args_buf, 2 + argc);
    cl_dynbind_restore_to(dyn_mark);
    result = cl_finish_string_output_stream(sstream);
    CL_GC_UNPROTECT(1 + 2 + argc);
    return result;
}

/* The report of a standard condition made WITHOUT a :format-control —
 * (make-condition 'type-error :datum 5 :expected-type 'list),
 * (error 'type-error ...), CHECK-TYPE, THE, ECASE/ETYPECASE fallthrough —
 * derived from the type's standard slots (CLHS 9.2: TYPE-ERROR has
 * :datum/:expected-type, CELL-ERROR :name, ...).  Without this the
 * condition printed as "#<CONDITION TYPE-ERROR>" under ~A and the
 * unhandled-error line said just "TYPE-ERROR", the datum and expected
 * type invisible although both were right there in the slots.
 *
 * The condition is not pinned by any table here: COND must be rooted by
 * the caller (the helper allocates), and the slot values are read out of
 * the alist BEFORE the first allocation.  Returns CL_NIL for a type with
 * no standard report, leaving the "#<CONDITION ...>" default to the
 * printer. */
static CL_Obj cl_condition_default_report(CL_Obj cond)
{
    /* Per standard type: the supertype to test, the control string, and up
     * to three slot keys whose values become the format arguments (a NULL
     * key slot means "the condition's type name" — the arithmetic-error
     * family names its concrete type).  The order matters: a subtype must
     * precede its supertype (UNBOUND-VARIABLE before CELL-ERROR). */
    struct default_report {
        CL_Obj *type;
        const char *ctl;
        CL_Obj *keys[3];
        int argc;
    };
    /* Interned first: the lookup can create the keyword (allocating), so
     * the condition pointer is derived only afterwards. */
    CL_Obj kw_object = cl_intern_keyword("OBJECT", 6);
    CL_Obj type;
    CL_Obj result = CL_NIL;
    struct default_report table[] = {
        { &SYM_TYPE_ERROR, "The value ~S is not of type ~S",
          { &KW_DATUM, &KW_EXPECTED_TYPE, NULL }, 2 },
        { &SYM_UNBOUND_VARIABLE_COND, "The variable ~S is unbound",
          { &KW_NAME, NULL, NULL }, 1 },
        { &SYM_UNDEFINED_FUNCTION_COND, "The function ~S is undefined",
          { &KW_NAME, NULL, NULL }, 1 },
        { &SYM_UNBOUND_SLOT, "The slot ~S is unbound in ~S",
          { &KW_NAME, &KW_INSTANCE, NULL }, 2 },
        { &SYM_CELL_ERROR, "Cell error on ~S",
          { &KW_NAME, NULL, NULL }, 1 },
        { &SYM_FILE_ERROR, "File error on ~S",
          { &KW_PATHNAME, NULL, NULL }, 1 },
        { &SYM_PACKAGE_ERROR, "Package error on ~S",
          { &KW_PACKAGE, NULL, NULL }, 1 },
        { &SYM_STREAM_ERROR, "Stream error on ~S",
          { &KW_STREAM, NULL, NULL }, 1 },
        { &SYM_ARITHMETIC_ERROR, "~A signalled by ~S with operands ~S",
          { NULL, &KW_OPERATION, &KW_OPERANDS }, 3 },
        { &SYM_PRINT_NOT_READABLE, "~S cannot be printed readably",
          { &kw_object, NULL, NULL }, 1 },
    };
    CL_Condition *c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
    CL_Obj argv[3];
    unsigned ti;

    type = c->type_name;

    if (CL_NULL_P(type) || !CL_SYMBOL_P(type))
        return CL_NIL;

    /* COND, TYPE and KW_OBJECT are all live across cl_condition_type_matches,
     * which can macroexpand a deftype alias (cl_vm_apply, allocating) on any
     * table entry — matched or not.  Without rooting, a compaction there
     * would leave COND/TYPE as stale offsets for the next iteration, and
     * KW_OBJECT (referenced via table[9].keys[0]) stale for the final one.
     * C is re-derived from the now-always-valid COND after every match. */
    CL_GC_PROTECT(cond);
    CL_GC_PROTECT(type);
    CL_GC_PROTECT(kw_object);

    for (ti = 0; ti < sizeof(table) / sizeof(table[0]); ti++) {
        const struct default_report *d = &table[ti];
        int i;
        if (!cl_condition_type_matches(type, *d->type))
            continue;
        c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
        for (i = 0; i < d->argc; i++)
            argv[i] = d->keys[i] ? slot_lookup(c->slots, *d->keys[i])
                                 : c->type_name;
        result = format_default_report(d->ctl, argv, d->argc);
        break;
    }

    CL_GC_UNPROTECT(3);
    return result;
}

/* Format a condition's report string, applying :format-arguments if present.
 * Returns a CL string object, or CL_NIL if no format-control and no
 * report_string — unless the type has a standard report (see
 * cl_condition_default_report), which is then returned instead.
 *
 * The condition object must be GC-rooted by the caller; C is re-derived
 * from it after the (allocating) default-report path. */
static CL_Obj format_condition_report(CL_Condition *c)
{
    CL_Obj fmt_ctrl = slot_lookup(c->slots, KW_FORMAT_CONTROL);
    CL_Obj fmt_args;
    if (CL_NULL_P(fmt_ctrl) || !CL_ANY_STRING_P(fmt_ctrl)) {
        if (!CL_NULL_P(c->report_string))
            return c->report_string;  /* fallback to raw report_string */
        return cl_condition_default_report(CL_PTR_TO_OBJ(c));
    }

    fmt_args = slot_lookup(c->slots, KW_FORMAT_ARGUMENTS);
    if (CL_NULL_P(fmt_args))
        return fmt_ctrl;  /* no args to substitute */

    /* Build args array for cl_format_to_stream: [dest, fmt, arg1, arg2, ...] */
    {
        CL_Obj sstream, result;
        int list_len = 0;
        CL_Obj tmp;
        CL_Obj args_buf[32];  /* [dest, fmt, up to 30 format args] */
        int i;
        int dyn_mark = cl_dyn_top;

        /* Collect the control string and arguments into args_buf BEFORE
         * any allocation, then GC-root every slot: both
         * cl_make_string_output_stream and cl_format_to_stream allocate
         * and can compact, which would leave the un-rooted CL_Obj copies
         * (and the fmt_ctrl local) as stale arena offsets — the report
         * then formats relocated garbage ("CAR: corrupted pointer" under
         * GC stress on every (error "..." args) whose report is printed).
         * Same idiom as %FORMATTER-INNER in builtins_format.c. */
        args_buf[0] = CL_NIL;  /* destination, filled in below */
        args_buf[1] = fmt_ctrl;
        for (tmp = fmt_args; !CL_NULL_P(tmp) && list_len < 30; tmp = cl_cdr(tmp))
            args_buf[2 + list_len++] = cl_car(tmp);
        for (i = 0; i < 2 + list_len; i++)
            cl_gc_push_root(&args_buf[i]);

        sstream = cl_make_string_output_stream();
        CL_GC_PROTECT(sstream);
        args_buf[0] = sstream;  /* destination (unused by cl_format_to_stream) */
        /* The arguments are whatever the caller passed to ERROR/WARN --
         * same "would print forever" risk as format_default_report's
         * slot-derived args (e.g. (error "~a" (circular-list 1 2 3))). */
        cl_dynbind_c(SYM_PRINT_CIRCLE, SYM_T);
        cl_format_to_stream(sstream, args_buf, 2 + list_len);
        cl_dynbind_restore_to(dyn_mark);
        result = cl_finish_string_output_stream(sstream);
        CL_GC_UNPROTECT(1 + 2 + list_len);
        return result;
    }
}

/* The report text of COND for ~A / the unhandled-error line: the
 * :format-control applied to its :format-arguments, else the raw
 * report_string, else the type's standard slot-derived report, else
 * CL_NIL.  Allocates; COND must be rooted by the caller.  (A
 * make-condition'd SIMPLE-ERROR used to print its control string with the
 * ~a directives unfilled, since only ERROR formatted it.) */
CL_Obj cl_condition_report(CL_Obj cond)
{
    return format_condition_report((CL_Condition *)CL_OBJ_TO_PTR(cond));
}

/* Check if a symbol is a known condition type in the hierarchy.
 * O(1) via the hash index — this is on TYPEP's path for every symbol
 * type spec, where the answer is usually "no" (a full walk pre-index). */
int cl_is_condition_type(CL_Obj type_sym)
{
    return !CL_NULL_P(cl_alist_index_find(&cond_index, type_sym));
}

/* --- Builtins --- */

/* (make-condition type &key ...) */
static CL_Obj bi_make_condition(CL_Obj *args, int n)
{
    CL_Obj type_sym = args[0];
    CL_Obj slots = CL_NIL;
    CL_Obj report_string = CL_NIL;
    int i;

    if (!CL_SYMBOL_P(type_sym) && !CL_NULL_P(type_sym))
        cl_error(CL_ERR_TYPE, "MAKE-CONDITION: type must be a symbol");

    /* Parse keyword initargs */
    CL_GC_PROTECT(slots);
    CL_GC_PROTECT(report_string);

    for (i = 1; i + 1 < n; i += 2) {
        CL_Obj pair = cl_cons(args[i], args[i + 1]);
        slots = cl_cons(pair, slots);

        /* Extract :format-control as report_string if it's a string.
         * Read the key/value from the GC-rooted args[] AFTER the cl_cons calls
         * above (which can trigger a compacting GC) — caching them in C locals
         * before the cons'es would leave stale arena offsets, so the eq-compare
         * against KW_FORMAT_CONTROL would miss under GC stress and the report
         * string would be dropped (condition then prints as "#<CONDITION ...>"
         * with no message). args[] is rooted, so its slots are forwarded. */
        if (args[i] == KW_FORMAT_CONTROL && CL_ANY_STRING_P(args[i + 1]))
            report_string = args[i + 1];
    }

    /* Fill in any :default-initargs the caller omitted (CLHS make-condition).
     * type_sym (a local copy) is stale after the cons loop above — read the
     * type from the rooted args[0] for every remaining use, or the stale
     * offset gets baked into the condition's type_name. */
    slots = merge_default_initargs(args[0], slots, &report_string);
    /* Evaluate slot :initforms for any slot still unsupplied (CLHS 7.1.3). */
    slots = apply_condition_slot_initforms(args[0], slots);

    {
        CL_Obj result = cl_make_condition(args[0], slots, report_string);
        CL_GC_UNPROTECT(2);
        return result;
    }
}

/* (conditionp obj) */
DEFINE_TYPE_PREDICATE(bi_conditionp, CL_CONDITION_P)

/* Type-check O is a condition and return its CL_Condition*; signals a
 * TYPE-ERROR tagged with WHO otherwise.  Backs both CONDITION-TYPE-NAME and
 * every slot reader below. */
static CL_Condition *require_condition(CL_Obj o, const char *who)
{
    if (!CL_CONDITION_P(o))
        cl_error(CL_ERR_TYPE, "%s: not a condition", who);
    return (CL_Condition *)CL_OBJ_TO_PTR(o);
}

/* Define a condition slot reader `(who condition) => (slot-value condition KW)`.
 * All the standard-condition accessors below share this exact body — only the
 * accessor name, its diagnostic tag, and the keyword differ.  Kept as a
 * body-generating macro so each stays a distinct registrable function. */
#define DEFINE_COND_SLOT_READER(fn, who, KW)                        \
    static CL_Obj fn(CL_Obj *args, int n)                           \
    {                                                               \
        CL_UNUSED(n);                                               \
        return slot_lookup(require_condition(args[0], who)->slots, KW); \
    }

/* (condition-type-name condition) */
static CL_Obj bi_condition_type_name(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return require_condition(args[0], "CONDITION-TYPE-NAME")->type_name;
}

DEFINE_COND_SLOT_READER(bi_simple_condition_format_control,
                        "SIMPLE-CONDITION-FORMAT-CONTROL",   KW_FORMAT_CONTROL)
DEFINE_COND_SLOT_READER(bi_simple_condition_format_arguments,
                        "SIMPLE-CONDITION-FORMAT-ARGUMENTS", KW_FORMAT_ARGUMENTS)
DEFINE_COND_SLOT_READER(bi_type_error_datum,
                        "TYPE-ERROR-DATUM",                  KW_DATUM)
DEFINE_COND_SLOT_READER(bi_type_error_expected_type,
                        "TYPE-ERROR-EXPECTED-TYPE",          KW_EXPECTED_TYPE)
DEFINE_COND_SLOT_READER(bi_stream_error_stream,
                        "STREAM-ERROR-STREAM",               KW_STREAM)
DEFINE_COND_SLOT_READER(bi_package_error_package,
                        "PACKAGE-ERROR-PACKAGE",             KW_PACKAGE)
DEFINE_COND_SLOT_READER(bi_cell_error_name,
                        "CELL-ERROR-NAME",                   KW_NAME)
DEFINE_COND_SLOT_READER(bi_arithmetic_error_operands,
                        "ARITHMETIC-ERROR-OPERANDS",         KW_OPERANDS)
DEFINE_COND_SLOT_READER(bi_arithmetic_error_operation,
                        "ARITHMETIC-ERROR-OPERATION",        KW_OPERATION)
DEFINE_COND_SLOT_READER(bi_file_error_pathname,
                        "FILE-ERROR-PATHNAME",               KW_PATHNAME)
DEFINE_COND_SLOT_READER(bi_unbound_slot_instance,
                        "UNBOUND-SLOT-INSTANCE",             KW_INSTANCE)

/* --- User-defined condition types --- */

/* (%register-condition-type name parent slot-pairs)
 * Adds to condition_hierarchy and condition_slot_table. */
static CL_Obj bi_register_condition_type(CL_Obj *args, int n)
{
    CL_Obj name = args[0];
    CL_Obj parents = args[1];   /* list of parent symbols (legacy: a lone symbol) */
    CL_Obj slot_pairs = args[2];
    CL_Obj entry = CL_NIL;
    CL_UNUSED(n);

    if (!CL_SYMBOL_P(name))
        cl_error(CL_ERR_TYPE, "%%REGISTER-CONDITION-TYPE: name must be a symbol");
    if (!CL_SYMBOL_P(parents) && !CL_NULL_P(parents) && !CL_CONS_P(parents))
        cl_error(CL_ERR_TYPE,
                 "%%REGISTER-CONDITION-TYPE: parents must be a symbol or list");

    /* GC-protect ALL the CL_Obj locals across the cl_cons calls below.
     * args[] entries are VM-rooted, but the cached `name`/`parents`/`slot_pairs`
     * locals are not — the nested cl_cons() allocations can trigger a
     * compacting GC that relocates the symbols, leaving the locals holding
     * stale offsets.  Storing a stale `name` here produced cons cells with
     * out-of-bounds car/cdr offsets (caught by the GC verifier) when a
     * define-condition was loaded under heap pressure.
     * `entry` must also be protected: it holds a freshly-consed pair and is
     * passed as CAR to a second cl_cons() which can itself trigger compaction,
     * leaving `entry` stale before it is stored in the global tables. */
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(parents);
    CL_GC_PROTECT(slot_pairs);
    CL_GC_PROTECT(entry);

    /* Add (name parent1 parent2 ...) to condition_hierarchy.  ALL parents must
     * be recorded, not just the first: a condition whose SECOND (or later)
     * superclass is ERROR — e.g. usocket's SOCKET-ERROR is
     * (define-condition socket-error (socket-condition error)) — must still be
     * recognised as a subtype of ERROR by cl_condition_type_matches() (and
     * hence TYPEP / SUBTYPEP / handler-case).  Previously only (car parents)
     * was stored, so such conditions escaped (error ...) handlers.
     *
     * Accept a lone symbol for backward compatibility by wrapping it in a
     * one-element list.  Each cl_cons() can trigger a compacting GC, so the
     * locals stay protected and `name` is re-read from its protected slot
     * AFTER the inner allocation (see the historical note above on the stale
     * mid-object offset that clobbered a symbol's plist with the mark bit). */
    if (CL_SYMBOL_P(parents)) {
        entry = cl_cons(parents, CL_NIL);   /* (parent) */
        parents = entry;
    }
    /* parents is now a (possibly empty) proper list of parent symbols.
     * Both table conses run outside the write lock (STW-vs-rwlock
     * deadlock — see cl_table_prepend_locked).  The hierarchy prepend
     * goes through the indexed variant so the hash index is dirty-marked
     * in the same wrlock critical section. */
    entry = cl_cons(name, parents);   /* (name . parents) — name re-read post-alloc */
    cl_alist_index_prepend(&cond_index, entry);

    /* Add (name . slot-pairs) to condition_slot_table */
    entry = cl_cons(name, slot_pairs);
    cl_table_prepend_locked(&condition_slot_table, entry);
    CL_GC_UNPROTECT(4);

    return name;
}

/* Find the parent type of a condition type in a snapshot of the
 * condition hierarchy.  Caller is responsible for capturing the
 * snapshot head under cl_tables_rdlock. */
static CL_Obj condition_parent_in(CL_Obj hierarchy, CL_Obj type_name)
{
    CL_Obj entry = hierarchy;
    while (!CL_NULL_P(entry)) {
        CL_Obj pair = cl_car(entry);
        if (cl_car(pair) == type_name)
            return cl_car(cl_cdr(pair));
        entry = cl_cdr(entry);
    }
    return CL_NIL;
}

/* Merge any registered :default-initargs for TYPE_SYM (and its ancestors)
 * into SLOTS, for every initarg the caller did not already supply.  Returns
 * the (possibly extended) slots alist.  A more-specific class's
 * default-initarg shadows a less-specific one — we walk most-specific first
 * and add only absent initargs (CLHS 7.1.4).
 *
 * If a merged-in :format-control is a string and *REPORT_STRING is still
 * NIL, it is also published as the condition's report string so the default
 * message prints.  Parent inheritance follows the first-parent chain, which
 * covers the single-inheritance conditions that carry default-initargs in
 * practice. */
static CL_Obj merge_default_initargs(CL_Obj type_sym, CL_Obj slots,
                                     CL_Obj *report_string)
{
    CL_Obj hierarchy, di_table, type, plist;

    if (!CL_SYMBOL_P(type_sym))
        return slots;

    cl_tables_rdlock();
    hierarchy = condition_hierarchy;
    di_table = condition_default_initargs;
    cl_tables_rwunlock();

    /* Fast path: no user condition declared any default-initargs.  Avoids
     * all the work for the built-in error/signal paths. */
    if (CL_NULL_P(di_table))
        return slots;

    CL_GC_PROTECT(type_sym);
    CL_GC_PROTECT(slots);
    CL_GC_PROTECT(hierarchy);
    CL_GC_PROTECT(di_table);
    type = type_sym;
    CL_GC_PROTECT(type);
    plist = CL_NIL;
    CL_GC_PROTECT(plist);          /* cursor spans cl_cons() below */

    while (!CL_NULL_P(type) && CL_SYMBOL_P(type)) {
        /* Locate this type's default-initargs plist (no allocation). */
        CL_Obj t = di_table;
        plist = CL_NIL;
        while (!CL_NULL_P(t)) {
            CL_Obj entry = cl_car(t);
            if (cl_car(entry) == type) { plist = cl_cdr(entry); break; }
            t = cl_cdr(t);
        }
        /* Add each (key val) the caller omitted.  cl_cons can compact, so
         * only GC-protected locals (plist, slots, *report_string's target)
         * may be relied on afterward — re-read the stored value rather than
         * caching `val` across the cons. */
        while (!CL_NULL_P(plist) && !CL_NULL_P(cl_cdr(plist))) {
            CL_Obj key = cl_car(plist);
            CL_Obj val = cl_car(cl_cdr(plist));
            if (!slot_present_p(slots, key)) {
                int is_fmt = (key == KW_FORMAT_CONTROL && CL_ANY_STRING_P(val)
                              && CL_NULL_P(*report_string));
                /* Build the pair THEN prepend it — never nest as
                 * cl_cons(cl_cons(key,val), slots): C's unspecified argument
                 * evaluation order (right-to-left on GCC/x86-64) reads the outer
                 * `slots` operand before the inner cl_cons compacts, baking a
                 * stale offset into the new cell's cdr and producing a cyclic
                 * slots alist that hangs the slot readers.  See the detailed note
                 * in apply_condition_slot_initforms. */
                CL_Obj pair = cl_cons(key, val);
                slots = cl_cons(pair, slots);
                if (is_fmt)
                    *report_string = cl_cdr(cl_car(slots));  /* post-compaction */
            }
            plist = cl_cdr(cl_cdr(plist));
        }
        type = condition_parent_in(hierarchy, type);
    }

    CL_GC_UNPROTECT(6);
    return slots;
}

/* Evaluate the registered slot :initform thunks for TYPE_SYM (and its
 * ancestors) for every slot whose value the caller did not supply, prepending
 * (key . value) to SLOTS.  A slot with an initarg is keyed by that initarg; a
 * slot without one is keyed by its slot name.  Most-specific class wins (we
 * walk most-specific first and skip slots already present), matching CLHS slot
 * shadowing.  Returns the (possibly extended) slots alist.
 *
 * Fast path: a no-op when no user condition registered any slot initform,
 * which is the case for every built-in error/signal path. */
static CL_Obj apply_condition_slot_initforms(CL_Obj type_sym, CL_Obj slots)
{
    CL_Obj hierarchy, if_table, type;

    if (!CL_SYMBOL_P(type_sym))
        return slots;

    cl_tables_rdlock();
    hierarchy = condition_hierarchy;
    if_table = condition_slot_initforms;
    cl_tables_rwunlock();

    if (CL_NULL_P(if_table))
        return slots;

    CL_GC_PROTECT(type_sym);
    CL_GC_PROTECT(slots);
    CL_GC_PROTECT(hierarchy);
    CL_GC_PROTECT(if_table);
    type = type_sym;
    CL_GC_PROTECT(type);

    while (!CL_NULL_P(type) && CL_SYMBOL_P(type)) {
        /* Locate this type's initform spec list (no allocation). */
        CL_Obj t = if_table;
        CL_Obj specs = CL_NIL;
        CL_GC_PROTECT(specs);            /* cursor spans cl_vm_apply below */
        while (!CL_NULL_P(t)) {
            CL_Obj entry = cl_car(t);
            if (cl_car(entry) == type) { specs = cl_cdr(entry); break; }
            t = cl_cdr(t);
        }
        while (!CL_NULL_P(specs)) {
            CL_Obj spec    = cl_car(specs);          /* (slot-name initarg thunk) */
            CL_Obj slot_nm = cl_car(spec);
            CL_Obj initarg = cl_car(cl_cdr(spec));
            CL_Obj thunk   = cl_car(cl_cdr(cl_cdr(spec)));
            int supplied   = (!CL_NULL_P(initarg) && slot_present_p(slots, initarg))
                             || slot_present_p(slots, slot_nm);
            if (!supplied) {
                CL_Obj pair;
                CL_Obj val = cl_vm_apply(thunk, NULL, 0);
                CL_GC_PROTECT(val);
                /* cl_vm_apply may have compacted: re-read slot_nm from the
                 * still-protected `specs` chain rather than trusting the stale
                 * offset captured before the call. */
                slot_nm = cl_car(cl_car(specs));
                /* Build the pair, THEN prepend it — do NOT nest the two conses
                 * as cl_cons(cl_cons(slot_nm,val), slots).  C leaves the outer
                 * call's argument evaluation order unspecified (GCC/x86-64
                 * evaluates right-to-left), so the outer `slots` operand is read
                 * into a temporary BEFORE the inner cl_cons runs; that inner
                 * allocation can compact and relocate the list, leaving the outer
                 * cons a stale `slots` offset.  The result is a self-referential
                 * (cyclic) slots alist that hangs the slot readers (slot_present_p)
                 * in an infinite walk.  CL_GC_PROTECT(slots) cannot help — the
                 * stale value lives in an un-rooted compiler temporary, not the
                 * protected variable.  Sequencing re-reads slots after the inner
                 * cons.  Same fix as bi_set_condition_slot_value. */
                pair = cl_cons(slot_nm, val);
                slots = cl_cons(pair, slots);
                CL_GC_UNPROTECT(1);
            }
            specs = cl_cdr(specs);
        }
        CL_GC_UNPROTECT(1);             /* specs */
        type = condition_parent_in(hierarchy, type);
    }

    CL_GC_UNPROTECT(5);
    return slots;
}

/* (%set-condition-default-initargs name initargs)
 * Records NAME's default-initargs plist (from define-condition's
 * :default-initargs option) so condition creation can fill in any initarg
 * the caller omitted. */
static CL_Obj bi_set_condition_default_initargs(CL_Obj *args, int n)
{
    CL_Obj name = args[0];
    CL_Obj initargs = args[1];
    CL_Obj entry;
    CL_UNUSED(n);

    if (!CL_SYMBOL_P(name))
        cl_error(CL_ERR_TYPE,
                 "%%SET-CONDITION-DEFAULT-INITARGS: name must be a symbol");

    /* GC-protect both locals across the cl_cons calls (see the matching
     * note in bi_register_condition_type). */
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(initargs);
    entry = cl_cons(name, initargs);   /* (name . plist) */
    /* Cons outside the write lock (STW-vs-rwlock deadlock — see
     * cl_table_prepend_locked). */
    cl_table_prepend_locked(&condition_default_initargs, entry);
    CL_GC_UNPROTECT(2);
    return name;
}

/* (%register-condition-slot-initforms name specs)
 * Records NAME's slot :initform thunks (from define-condition).  SPECS is a
 * list of (slot-name initarg-or-nil thunk) entries (proper list). */
static CL_Obj bi_register_condition_slot_initforms(CL_Obj *args, int n)
{
    CL_Obj name = args[0];
    CL_Obj specs = args[1];
    CL_Obj entry;
    CL_UNUSED(n);

    if (!CL_SYMBOL_P(name))
        cl_error(CL_ERR_TYPE,
                 "%%REGISTER-CONDITION-SLOT-INITFORMS: name must be a symbol");

    CL_GC_PROTECT(name);
    CL_GC_PROTECT(specs);
    entry = cl_cons(name, specs);   /* (name . specs) */
    /* Cons outside the write lock (STW-vs-rwlock deadlock — see
     * cl_table_prepend_locked). */
    cl_table_prepend_locked(&condition_slot_initforms, entry);
    CL_GC_UNPROTECT(2);
    return name;
}

/* Find initarg keyword for a slot-name in a snapshot of the condition
 * slot table.  Caller is responsible for the snapshot. */
static CL_Obj find_slot_initarg_in(CL_Obj slot_table, CL_Obj type_name, CL_Obj slot_name)
{
    CL_Obj table_entry = slot_table;
    while (!CL_NULL_P(table_entry)) {
        CL_Obj entry = cl_car(table_entry);
        if (cl_car(entry) == type_name) {
            CL_Obj slot_pairs = cl_cdr(entry);
            while (!CL_NULL_P(slot_pairs)) {
                CL_Obj pair = cl_car(slot_pairs);
                if (cl_car(pair) == slot_name)
                    return cl_cdr(pair);
                slot_pairs = cl_cdr(slot_pairs);
            }
            break;
        }
        table_entry = cl_cdr(table_entry);
    }
    return CL_NIL;
}

/* (condition-slot-value condition slot-name)
 * Look up slot-name in condition_slot_table to find the initarg keyword,
 * then look up that keyword in the condition's slots alist.
 * Walks up the type hierarchy to find inherited slots. */
static CL_Obj bi_condition_slot_value(CL_Obj *args, int n)
{
    CL_Obj cond_obj = args[0];
    CL_Obj slot_name = args[1];
    CL_Condition *cond;
    CL_Obj cur_type, initarg;
    CL_UNUSED(n);

    if (!CL_CONDITION_P(cond_obj))
        cl_error(CL_ERR_TYPE, "CONDITION-SLOT-VALUE: not a condition");

    cond = (CL_Condition *)CL_OBJ_TO_PTR(cond_obj);
    cur_type = cond->type_name;

    /* Search this type and all ancestors for the slot.  Both tables
     * are only ever prepended to, so snapshot under the lock and walk
     * without it — avoids leaking the rdlock on cl_car-corrupt-cell
     * longjmps and the resulting condition-wait safety abort. */
    initarg = CL_NIL;
    {
        CL_Obj hierarchy_snap, slot_table_snap;
        cl_tables_rdlock();
        hierarchy_snap = condition_hierarchy;
        slot_table_snap = condition_slot_table;
        cl_tables_rwunlock();
        while (!CL_NULL_P(cur_type) && CL_NULL_P(initarg)) {
            initarg = find_slot_initarg_in(slot_table_snap, cur_type, slot_name);
            if (CL_NULL_P(initarg))
                cur_type = condition_parent_in(hierarchy_snap, cur_type);
        }
    }

    /* Prefer the initarg-keyed value (caller-supplied or a default-initarg);
     * fall back to a value keyed by the slot name itself, which is how an
     * evaluated :initform for a slot is stored (see
     * apply_condition_slot_initforms — covers slots with no :initarg). */
    if (!CL_NULL_P(initarg) && slot_present_p(cond->slots, initarg))
        return slot_lookup(cond->slots, initarg);
    if (slot_present_p(cond->slots, slot_name))
        return slot_lookup(cond->slots, slot_name);
    return CL_NIL;
}

/* (%set-condition-slot-value condition slot-name value) */
static CL_Obj bi_set_condition_slot_value(CL_Obj *args, int n)
{
    CL_Obj cond_obj = args[0];
    CL_Obj slot_name = args[1];
    CL_Obj value = args[2];
    CL_Condition *cond;
    CL_Obj cur_type, initarg;
    CL_UNUSED(n);

    if (!CL_CONDITION_P(cond_obj))
        cl_error(CL_ERR_TYPE, "%SET-CONDITION-SLOT-VALUE: not a condition");

    cond = (CL_Condition *)CL_OBJ_TO_PTR(cond_obj);
    cur_type = cond->type_name;

    /* Search this type and all ancestors for the slot.  Both tables
     * are only ever prepended to, so snapshot under the lock and walk
     * without it — avoids leaking the rdlock on cl_car-corrupt-cell
     * longjmps and the resulting condition-wait safety abort. */
    initarg = CL_NIL;
    {
        CL_Obj hierarchy_snap, slot_table_snap;
        cl_tables_rdlock();
        hierarchy_snap = condition_hierarchy;
        slot_table_snap = condition_slot_table;
        cl_tables_rwunlock();
        while (!CL_NULL_P(cur_type) && CL_NULL_P(initarg)) {
            initarg = find_slot_initarg_in(slot_table_snap, cur_type, slot_name);
            if (CL_NULL_P(initarg))
                cur_type = condition_parent_in(hierarchy_snap, cur_type);
        }
    }

    /* A slot with no :initarg is stored/looked up by its slot name (this is
     * how an evaluated :initform value lands in the alist); a slot with an
     * initarg uses that keyword as the key. */
    {
        CL_Obj key = CL_NULL_P(initarg) ? slot_name : initarg;

        /* Update existing slot value */
        {
            CL_Obj slots = cond->slots;
            while (!CL_NULL_P(slots)) {
                CL_Obj sp = cl_car(slots);
                if (cl_car(sp) == key) {
                    ((CL_Cons *)CL_OBJ_TO_PTR(sp))->cdr = value;
                    return value;
                }
                slots = cl_cdr(slots);
            }
        }
        /* Slot not found in alist — add it.  Each cons can compact: split
         * them and re-derive cond before the store (the old code's store
         * went through a cond captured before the SECOND cons, writing
         * into the condition's old location), and keep value rooted for
         * the return. */
        {
            CL_Obj new_pair;
            CL_GC_PROTECT(cond_obj);
            CL_GC_PROTECT(key);
            CL_GC_PROTECT(value);
            new_pair = cl_cons(key, value);
            new_pair = cl_cons(new_pair,
                               ((CL_Condition *)CL_OBJ_TO_PTR(cond_obj))->slots);
            cond = (CL_Condition *)CL_OBJ_TO_PTR(cond_obj);
            cond->slots = new_pair;
            CL_GC_UNPROTECT(3);
        }
    }
    return value;
}

/* Resolve the alist key (initarg keyword or slot name) used to store
 * SLOT-NAME's value in COND's slots alist, walking the type hierarchy. */
static CL_Obj condition_slot_key(CL_Condition *cond, CL_Obj slot_name)
{
    CL_Obj cur_type = cond->type_name;
    CL_Obj initarg = CL_NIL;
    CL_Obj hierarchy_snap, slot_table_snap;
    cl_tables_rdlock();
    hierarchy_snap = condition_hierarchy;
    slot_table_snap = condition_slot_table;
    cl_tables_rwunlock();
    while (!CL_NULL_P(cur_type) && CL_NULL_P(initarg)) {
        initarg = find_slot_initarg_in(slot_table_snap, cur_type, slot_name);
        if (CL_NULL_P(initarg))
            cur_type = condition_parent_in(hierarchy_snap, cur_type);
    }
    return CL_NULL_P(initarg) ? slot_name : initarg;
}

/* (condition-slot-boundp condition slot-name)
 * T when the slot has a value in the condition's slots alist (a supplied
 * initarg or an evaluated :initform), NIL when it is genuinely unbound. */
static CL_Obj bi_condition_slot_boundp(CL_Obj *args, int n)
{
    CL_Obj cond_obj = args[0];
    CL_Obj slot_name = args[1];
    CL_Condition *cond;
    CL_Obj initarg;
    CL_UNUSED(n);

    if (!CL_CONDITION_P(cond_obj))
        cl_error(CL_ERR_TYPE, "CONDITION-SLOT-BOUNDP: not a condition");

    cond = (CL_Condition *)CL_OBJ_TO_PTR(cond_obj);
    initarg = condition_slot_key(cond, slot_name);
    if (slot_present_p(cond->slots, initarg))
        return CL_T;
    /* A slot with no :initarg is keyed by its name; check that too. */
    if (slot_present_p(cond->slots, slot_name))
        return CL_T;
    return CL_NIL;
}

/* (%condition-slot-makunbound condition slot-name)
 * Remove the slot's (key . value) pair from the condition's slots alist so
 * it reads as unbound.  Returns the condition. */
static CL_Obj bi_condition_slot_makunbound(CL_Obj *args, int n)
{
    CL_Obj cond_obj = args[0];
    CL_Obj slot_name = args[1];
    CL_Condition *cond;
    CL_Obj key, slots, prev;
    CL_UNUSED(n);

    if (!CL_CONDITION_P(cond_obj))
        cl_error(CL_ERR_TYPE, "%CONDITION-SLOT-MAKUNBOUND: not a condition");

    cond = (CL_Condition *)CL_OBJ_TO_PTR(cond_obj);
    key = condition_slot_key(cond, slot_name);

    /* Unlink any pair keyed by the initarg key or the bare slot name. */
    prev = CL_NIL;
    slots = cond->slots;
    while (!CL_NULL_P(slots)) {
        CL_Obj sp = cl_car(slots);
        CL_Obj k = cl_car(sp);
        if (k == key || k == slot_name) {
            CL_Obj rest = cl_cdr(slots);
            if (CL_NULL_P(prev))
                cond->slots = rest;
            else
                ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = rest;
            slots = rest;
            continue;
        }
        prev = slots;
        slots = cl_cdr(slots);
    }
    return cond_obj;
}

/* --- Signaling --- */

/* Build and signal a TYPE-ERROR with :datum and :expected-type slots
 * populated, then unwind through the standard error path.
 *
 * cl_error / cl_create_condition_from_error build a condition with only
 * :format-control set, so handler-case clauses calling type-error-datum
 * or type-error-expected-type get NIL.  The ANSI conformance tests
 * (signals-type-error, check-type-error) compare datum against the
 * original argument and fail when datum is NIL.  Use this helper from
 * call sites that already know both values (car, cdr, rplaca/rplacd,
 * etc.) so the slots survive into Lisp-land. */
/* Core implementation: expected_type is an already-built Lisp type specifier.
 * type_name_for_msg is a human-readable string for the report (may be NULL). */
static CL_NORETURN void signal_type_error_core(CL_Obj datum, CL_Obj expected_type,
                                               const char *type_name_for_msg,
                                               const char *fn_name)
{
    CL_Obj report = CL_NIL;
    CL_Obj slots = CL_NIL;
    CL_Obj cond;
    char msgbuf[128];

    CL_GC_PROTECT(datum);
    CL_GC_PROTECT(expected_type);

    /* Name the datum's observed type in the report — "not of type
     * STRUCTURE" alone is undiagnosable in the field (was it NIL? a
     * fixnum? a wild offset from corruption?).  cl_type_name reads the
     * heap header, so guard against an out-of-arena offset (corruption)
     * and report it as such instead of dereferencing it. */
    {
        const char *got;
        if (CL_HEAP_P(datum) && datum >= cl_heap.arena_size)
            got = "an out-of-arena object reference";
        else
            got = cl_type_name(datum);
        snprintf(msgbuf, sizeof(msgbuf),
                 "%s: argument is not of type %s (got %s)", fn_name,
                 type_name_for_msg ? type_name_for_msg : "(see expected-type)",
                 got);
    }
    report = cl_make_string(msgbuf, (uint32_t)strlen(msgbuf));
    CL_GC_PROTECT(report);

    /* slots = ((:datum . datum)
     *          (:expected-type . expected_type)
     *          (:format-control . report))
     * Built tail-first so each cl_cons sees its predecessor protected via
     * the head of `slots`. */
    {
        CL_Obj pair;
        pair = cl_cons(KW_FORMAT_CONTROL, report);
        slots = cl_cons(pair, CL_NIL);
        CL_GC_PROTECT(slots);
        pair = cl_cons(KW_EXPECTED_TYPE, expected_type);
        slots = cl_cons(pair, slots);
        pair = cl_cons(KW_DATUM, datum);
        slots = cl_cons(pair, slots);
    }

    cond = cl_make_condition(SYM_TYPE_ERROR, slots, report);

    CL_GC_UNPROTECT(4); /* datum, expected_type, report, slots */

    /* cond stays rooted across the signal: a handler that runs and
     * declines allocates freely, and a stale cond would reach the
     * debugger.  Both calls unwind via longjmp, restoring the roots. */
    CL_GC_PROTECT(cond);
    cl_signal_condition(cond);
    cl_error_from_condition(cond);
}

void cl_signal_type_error(CL_Obj datum, const char *expected_type_name,
                          const char *fn_name)
{
    CL_Obj expected_type;

    CL_GC_PROTECT(datum);
    /* Compound type specs like "(INTEGER 1 *)" must be read as Lisp
     * expressions, not interned as symbols, so that (typep datum
     * expected-type) in handler code works correctly. */
    if (expected_type_name[0] == '(') {
        CL_ReadStream rs;
        rs.buf = expected_type_name;
        rs.pos = 0;
        rs.len = (int)strlen(expected_type_name);
        rs.line = 1;
        expected_type = cl_read_from_string(&rs);
    } else {
        expected_type = cl_intern_in(expected_type_name,
                                     (uint32_t)strlen(expected_type_name),
                                     cl_package_cl);
    }
    CL_GC_UNPROTECT(1);
    signal_type_error_core(datum, expected_type, expected_type_name, fn_name);
}

void cl_signal_type_error_obj(CL_Obj datum, CL_Obj expected_type,
                              const char *fn_name)
{
    signal_type_error_core(datum, expected_type, NULL, fn_name);
}

/* Signal an ARITHMETIC-ERROR subtype (floating-point-overflow/-underflow,
 * division-by-zero, ...) with :operation and :operands slots populated,
 * then unwind. operands is a Lisp list of the arguments (or CL_NIL when
 * none are meaningful — e.g. (exp x) returning +inf carries (x) as
 * the operand list). */
void cl_signal_arith_error(CL_Obj type_sym, CL_Obj operation, CL_Obj operands,
                           const char *fn_name)
{
    CL_Obj report = CL_NIL;
    CL_Obj slots = CL_NIL;
    CL_Obj cond;
    char msgbuf[128];

    CL_GC_PROTECT(type_sym);
    CL_GC_PROTECT(operation);
    CL_GC_PROTECT(operands);

    snprintf(msgbuf, sizeof(msgbuf), "%s: arithmetic error", fn_name);
    report = cl_make_string(msgbuf, (uint32_t)strlen(msgbuf));
    CL_GC_PROTECT(report);

    {
        CL_Obj pair;
        pair = cl_cons(KW_FORMAT_CONTROL, report);
        slots = cl_cons(pair, CL_NIL);
        CL_GC_PROTECT(slots);
        pair = cl_cons(KW_OPERANDS, operands);
        slots = cl_cons(pair, slots);
        pair = cl_cons(KW_OPERATION, operation);
        slots = cl_cons(pair, slots);
    }

    cond = cl_make_condition(type_sym, slots, report);

    CL_GC_UNPROTECT(5); /* type_sym, operation, operands, report, slots */

    /* Root cond across the signal — see signal_type_error_core. */
    CL_GC_PROTECT(cond);
    cl_signal_condition(cond);
    cl_error_from_condition(cond);
}

/* Build and signal a CELL-ERROR subtype (unbound-variable or
 * undefined-function) carrying the :name slot, then unwind.  The plain
 * cl_error(CL_ERR_UNBOUND/UNDEFINED, ...) path leaves :name unset, so
 * (cell-error-name c) returns NIL — the ANSI conformance tests assert
 * (eq (cell-error-name c) sym) and fail without the slot. */
static void signal_cell_error(CL_Obj type_sym, CL_Obj name,
                              const char *report_prefix)
{
    CL_Obj report = CL_NIL;
    CL_Obj slots = CL_NIL;
    CL_Obj cond;
    char msgbuf[160];
    char namebuf[96];
    const char *sym_name;

    if (CL_NULL_P(name))
        sym_name = "NIL";
    else if (CL_SYMBOL_P(name))
        sym_name = cl_symbol_name(name);
    else if (CL_CONS_P(name) && CL_SYMBOL_P(cl_car(name))
             && CL_CONS_P(cl_cdr(name)) && CL_SYMBOL_P(cl_car(cl_cdr(name)))
             && CL_NULL_P(cl_cdr(cl_cdr(name)))) {
        /* a (SETF ACCESSOR) function name -- FDEFINITION / FBOUNDP /
         * FUNCALL on one that has only a DEFSETF land here */
        snprintf(namebuf, sizeof(namebuf), "(%s %s)",
                 cl_symbol_name(cl_car(name)), cl_symbol_name(cl_car(cl_cdr(name))));
        sym_name = namebuf;
    } else
        sym_name = "?";

    CL_GC_PROTECT(name);

    snprintf(msgbuf, sizeof(msgbuf), "%s: %s", report_prefix, sym_name);
    report = cl_make_string(msgbuf, (uint32_t)strlen(msgbuf));
    CL_GC_PROTECT(report);

    {
        CL_Obj pair;
        pair = cl_cons(KW_FORMAT_CONTROL, report);
        slots = cl_cons(pair, CL_NIL);
        CL_GC_PROTECT(slots);
        pair = cl_cons(KW_NAME, name);
        slots = cl_cons(pair, slots);
    }

    cond = cl_make_condition(type_sym, slots, report);

    CL_GC_UNPROTECT(3); /* name, report, slots */

    /* Root cond across the signal — see signal_type_error_core. */
    CL_GC_PROTECT(cond);
    cl_signal_condition(cond);
    cl_error_from_condition(cond);
}

void cl_signal_unbound_variable(CL_Obj name)
{
    signal_cell_error(SYM_UNBOUND_VARIABLE_COND, name, "unbound variable");
}

void cl_signal_undefined_function(CL_Obj name)
{
    signal_cell_error(SYM_UNDEFINED_FUNCTION_COND, name, "undefined function");
}

/* Create a condition object from a C error code and message string */
CL_Obj cl_create_condition_from_error(int code, const char *msg)
{
    CL_Obj type_sym;
    CL_Obj slots = CL_NIL;
    CL_Obj report = CL_NIL;

    switch (code) {
    case CL_ERR_TYPE:      type_sym = SYM_TYPE_ERROR; break;
    case CL_ERR_UNBOUND:   type_sym = SYM_UNBOUND_VARIABLE_COND; break;
    case CL_ERR_UNDEFINED: type_sym = SYM_UNDEFINED_FUNCTION_COND; break;
    case CL_ERR_DIVZERO:   type_sym = SYM_DIVISION_BY_ZERO; break;
    case CL_ERR_OVERFLOW:  type_sym = SYM_ARITHMETIC_ERROR; break;
    case CL_ERR_ARGS:      type_sym = SYM_PROGRAM_ERROR; break;
    case CL_ERR_PARSE:     type_sym = SYM_PARSE_ERROR; break;
    case CL_ERR_FILE:      type_sym = SYM_FILE_ERROR; break;
    case CL_ERR_EOF:       type_sym = SYM_END_OF_FILE; break;
    case CL_ERR_TIMEOUT:   type_sym = SYM_SOCKET_TIMEOUT; break;
    default:               type_sym = SYM_SIMPLE_ERROR; break;
    }

    if (msg && msg[0]) {
        /* type_sym (a boot symbol picked above) is passed to
         * cl_make_condition after these allocs — root it too. */
        CL_GC_PROTECT(type_sym);
        report = cl_make_string(msg, (uint32_t)strlen(msg));
        CL_GC_PROTECT(report);
        {
            CL_Obj pair = cl_cons(KW_FORMAT_CONTROL, report);
            slots = cl_cons(pair, CL_NIL);
        }
        CL_GC_UNPROTECT(2);
    }

    return cl_make_condition(type_sym, slots, report);
}

/* COMPILE warning/failure detection (CLHS COMPILE return values 2 and 3).
 * While cl_compile_detect_depth > 0, every signaled WARNING/ERROR is recorded
 * so COMPILE can report warnings-p (any warning or error) and failure-p (any
 * error, or any warning other than style-warning).  Counting happens before
 * the handler search so it reflects what the compiler *detected*, regardless
 * of whether a handler later muffles the condition. */
int cl_compile_detect_depth = 0;
int cl_compile_warnings_p = 0;
int cl_compile_failure_p = 0;

/* Walk handler stack top-down, calling matching handlers.
 * Handlers run in the signaler's dynamic context without unwinding.
 * Returns NIL if no handler transferred control. */
CL_Obj cl_signal_condition(CL_Obj condition)
{
    CL_Condition *cond;
    int i;

    if (!CL_CONDITION_P(condition))
        return CL_NIL;

    cond = (CL_Condition *)CL_OBJ_TO_PTR(condition);

    /* cl_condition_type_matches can now compact the heap: for a handler whose
     * clause type is a user deftype, it calls the deftype's expander via
     * cl_vm_apply (allocates).  That moves the condition object, so the bare
     * C local `condition`/`cond` would go stale across the loop below.  Root
     * `condition` for the duration so the compactor forwards it, and re-derive
     * `cond` from it after every potentially-allocating call.  (Without this,
     * a later loop iteration dereferenced a relocated condition — observed as
     * heap corruption / a shutdown hang on the Amiga, where the heap layout
     * triggers compaction the host run did not.) */
    CL_GC_PROTECT(condition);

    if (cl_compile_detect_depth > 0) {
        int is_warning = cl_condition_type_matches(cond->type_name, SYM_WARNING);
        int is_error   = cl_condition_type_matches(cond->type_name, SYM_ERROR_COND);
        cond = (CL_Condition *)CL_OBJ_TO_PTR(condition);
        /* Per CLHS COMPILE: warnings-p is true if any condition of type ERROR
         * *or* WARNING was detected; failure-p is true for the same set minus
         * STYLE-WARNING.  So an ERROR sets both; a STYLE-WARNING sets only
         * warnings-p; a non-style WARNING sets both. */
        if (is_warning || is_error) {
            cl_compile_warnings_p = 1;
            if (!cl_condition_type_matches(cond->type_name, SYM_STYLE_WARNING))
                cl_compile_failure_p = 1;
            cond = (CL_Condition *)CL_OBJ_TO_PTR(condition);
        }
    }

#ifdef DEBUG_CONDITION
    {
        CL_String *tns = (CL_String *)CL_OBJ_TO_PTR(
            ((CL_Symbol *)CL_OBJ_TO_PTR(cond->type_name))->name);
        fprintf(stderr, "[signal_condition] type=%s handler_top=%d\n",
                tns->data, cl_handler_top);
        for (i = 0; i < cl_handler_top; i++) {
            CL_String *htn = (CL_String *)CL_OBJ_TO_PTR(
                ((CL_Symbol *)CL_OBJ_TO_PTR(
                    cl_handler_stack[i].type_name))->name);
            fprintf(stderr, "  [%d] type=%s mark=%d\n", i,
                    htn->data, cl_handler_stack[i].handler_mark);
        }
    }
#endif

    for (i = cl_handler_top - 1; i >= cl_handler_floor; i--) {
        /* Re-derive cond each iteration: a prior iteration's matcher (deftype
         * expander) or handler call may have compacted the heap.  `condition`
         * is GC-rooted above, so this always yields the current location. */
        cond = (CL_Condition *)CL_OBJ_TO_PTR(condition);
        if (!((cl_handler_active_mask >> i) & 1))
            continue;
        if (cl_condition_type_matches(cond->type_name,
                                      cl_handler_stack[i].type_name)) {
            /* A HANDLER-CASE clause binding: no handler function runs — the
             * transfer itself IS the handling (CLHS 9.1.3: handler-case's
             * clause is entered after the dynamic extent of the form is
             * unwound).  Never returns. */
            if (CL_FIXNUM_P(cl_handler_stack[i].handler))
                cl_handler_case_transfer(i, condition);
            /* CLHS 9.1.4: while a handler runs, that handler and every handler
             * established more recently than it are disabled, so a condition it
             * signals is handled by the *outer* handlers.  Disable the band
             * [i, cl_handler_top) by clearing those bits in the active mask
             * rather than by truncating cl_handler_top.  Truncating made a
             * HANDLER-BIND/IGNORE-ERRORS established inside the handler push
             * onto slot i, clobbering this handler's own binding; once
             * cl_handler_top was restored, that stale binding was invoked for a
             * later, unrelated condition and threw into a handler-case CATCH tag
             * that no longer existed ("No catch for tag").  Keeping
             * cl_handler_top intact makes the handler's pushes land above the
             * band instead.
             *
             * The mask is restored on BOTH exit paths: normally below, and —
             * crucially — on a NON-LOCAL exit (the handler INVOKE-RESTARTs a
             * restart in its own dynamic extent, e.g. fiveam's process-failure),
             * via the snapshot stored in the NLX/error frame the throw unwinds
             * to.  The old per-frame `active` save lived only in this C frame's
             * locals and was lost to the longjmp, leaving the band permanently
             * disabled so the *next* signal escaped uncaught. */
            uint64_t saved_mask = cl_handler_active_mask;
            cl_handler_active_mask &= ~CL_HANDLER_BAND_MASK(i, cl_handler_top);
            cl_vm_apply(cl_handler_stack[i].handler, &condition, 1);
            /* GC may have compacted during the handler call; re-derive cond
             * from the GC-tracked CL_Obj so the next iteration's type check
             * uses a valid pointer. */
            cond = (CL_Condition *)CL_OBJ_TO_PTR(condition);
            /* Handler returned normally (declined) — re-enable the band and
             * keep searching the outer (lower-index) handlers. */
            cl_handler_active_mask = saved_mask;
        }
    }

    CL_GC_UNPROTECT(1);  /* condition */
    return CL_NIL;
}

/* Helper: coerce signal/warn/error argument into a condition object.
 * If arg is already a condition, return it.
 * If arg is a symbol, make-condition with that type.
 * If arg is a string, make simple-condition with :format-control.
 * default_type is used when arg is a string (e.g. SIMPLE-ERROR, SIMPLE-WARNING). */
static CL_Obj coerce_to_condition(CL_Obj *args, int n, CL_Obj default_type)
{
    CL_Obj arg = args[0];

    if (CL_CONDITION_P(arg))
        return arg;

    if (CL_SYMBOL_P(arg)) {
        /* (signal 'type-error :datum x :expected-type y) */
        CL_Obj slots = CL_NIL;
        CL_Obj report = CL_NIL;
        int i;

        CL_GC_PROTECT(slots);
        CL_GC_PROTECT(report);

        for (i = 1; i + 1 < n; i += 2) {
            CL_Obj key = args[i];
            CL_Obj val = args[i + 1];
            CL_Obj pair = cl_cons(key, val);
            slots = cl_cons(pair, slots);
            if (key == KW_FORMAT_CONTROL && CL_ANY_STRING_P(val))
                report = val;
        }

        /* Fill in any :default-initargs not supplied at the signal site
         * (CLHS define-condition :default-initargs).  This is what makes
         * (error 'my-error) carry my-error's default :format-control etc.
         * The arg local copy is stale after the cons loop above — read the
         * type from the rooted args[0] for every remaining use. */
        slots = merge_default_initargs(args[0], slots, &report);
        slots = apply_condition_slot_initforms(args[0], slots);

        {
            CL_Obj result = cl_make_condition(args[0], slots, report);
            CL_GC_UNPROTECT(2);
            return result;
        }
    }

    if (CL_ANY_STRING_P(arg)) {
        /* (error "msg" a b) → simple-condition with :format-control +
         * :format-arguments.  A wide control (any non-ASCII character in
         * the message) is a string too — rejecting it here turned
         * (error "Fehler: ü") into "Expected condition, symbol, or
         * string". */
        CL_Obj pair, slots, fmt_args;
        int i;
        /* default_type is a boot symbol (SIMPLE-ERROR/-WARNING/-CONDITION)
         * read after all the conses below — boot symbols rarely move, but
         * they DO relocate under heap-scale fragmentation (cf. the
         * *READTABLE* root fix), so root it like everything else. */
        CL_GC_PROTECT(default_type);
        CL_GC_PROTECT(arg);
        /* Collect format arguments (args[1..n-1]) into a list */
        fmt_args = CL_NIL;
        for (i = n - 1; i > 0; i--)
            fmt_args = cl_cons(args[i], fmt_args);
        CL_GC_PROTECT(fmt_args);
        slots = CL_NIL;
        if (!CL_NULL_P(fmt_args)) {
            pair = cl_cons(KW_FORMAT_ARGUMENTS, fmt_args);
            slots = cl_cons(pair, slots);
        }
        CL_GC_PROTECT(slots);
        pair = cl_cons(KW_FORMAT_CONTROL, arg);
        slots = cl_cons(pair, slots);
        {
            CL_Obj result = cl_make_condition(default_type, slots, arg);
            CL_GC_UNPROTECT(4);
            return result;
        }
    }

    cl_error(CL_ERR_TYPE, "Expected condition, symbol, or string");
    return CL_NIL;
}

/* (signal condition-or-type &rest args) */
static CL_Obj bi_signal(CL_Obj *args, int n)
{
    CL_Obj cond = coerce_to_condition(args, n, SYM_SIMPLE_CONDITION);
    cl_signal_condition(cond);
    return CL_NIL;
}

/* Cached NIL-returning closure for muffle-warning restart handler */
static CL_Obj muffle_handler = CL_NIL;

static CL_Obj get_muffle_handler(void)
{
    /* muffle_handler is registered as a GLOBAL root at init (see
     * cl_builtins_condition_init).  The old lazy CL_GC_PROTECT here was a
     * root-discipline bug: pushed mid-WARN it sat between bi_warn's own
     * cond/tag roots, so the caller's CL_GC_UNPROTECT(2) popped the wrong
     * entries and left a permanent root pointing at a dead C-stack slot —
     * later compactions wrote forwarded values through it. */
    if (CL_NULL_P(muffle_handler)) {
        extern CL_Obj cl_eval_string(const char *str);
        muffle_handler = cl_eval_string("(lambda () nil)");
    }
    return muffle_handler;
}

/* (warn condition-or-type &rest args) */
static CL_Obj bi_warn(CL_Obj *args, int n)
{
    CL_Obj cond = coerce_to_condition(args, n, SYM_SIMPLE_WARNING);
    CL_Obj tag;
    int saved_restart_top = cl_restart_top;
    int saved_handler_top = cl_handler_top;
    int muffled = 0;

    /* Create unique catch tag for muffle-warning.  cond is protected
     * BEFORE the cons — protecting after would root an already-stale
     * offset when the cons compacts. */
    CL_GC_PROTECT(cond);
    tag = cl_cons(SYM_MUFFLE_WARNING, CL_NIL);
    CL_GC_PROTECT(tag);

    /* Push NLX catch frame for the muffle-warning restart */
    if (cl_nlx_top < cl_nlx_max) {
        CL_NLXFrame *frame = &cl_nlx_stack[cl_nlx_top];
        frame->type = CL_NLX_CATCH;
        frame->tag = tag;
        frame->vm_sp = cl_vm.sp;
        frame->vm_fp = cl_vm.fp;
        frame->result = CL_NIL;
        /* The collector marks tag, result AND bytecode of every frame below
         * nlx_top; a C-pushed frame has no bytecode, and the slot's previous
         * occupant (a VM frame of some since-collected top-level form) left
         * a stale offset here.  Marking it corrupted the arena walk
         * ([GC-BADMARK] nlx_stack.bytecode under AMILISP_GC_STRESS, found by
         * tests/test_tier4_phase3.sh's HANDLER-CASE + WARN case). */
        frame->bytecode = CL_NIL;
        frame->dyn_mark = cl_dyn_top;
        frame->handler_mark = cl_handler_top;
        frame->restart_mark = cl_restart_top;
        /* Save the remaining unwind marks so the muffle landing below can
         * restore the full VM/GC/error state — see the comment there. */
        frame->error_mark = cl_error_frame_top;
        frame->gc_root_mark = gc_root_count;
        frame->compiler_mark = cl_compiler_mark();
        frame->printer_mark = cl_printer_state_save();
        frame->saved_pending_mark = cl_saved_pending_top;
        frame->mv_save_mark = CT->mv_save_top;
        frame->saved_jit_depth = CT->jit_depth;
        frame->saved_jit_c_floor = CT->jit_c_floor;
        frame->landing = NULL;   /* C-owned frame: cl_nlx_jump uses buf */
        /* Snapshot the handler ACTIVE mask too: cl_signal_condition
         * disables the running handler's band while it executes, and the
         * muffle longjmp skips cl_signal_condition's own restore.  Without
         * this the WARNING handler stayed band-disabled after the first
         * muffle, so only the FIRST of several warns under one
         * handler-bind was caught (pre-existing, found in tier 3). */
        frame->handler_active_mask = cl_handler_active_mask;
        cl_nlx_top++;

        /* Push muffle-warning restart */
        if (cl_restart_top < CL_MAX_RESTART_BINDINGS) {
            CL_Obj mw_handler = get_muffle_handler();
            CL_Obj mw_restart = cl_make_restart(SYM_MUFFLE_WARNING, mw_handler,
                                                CL_NIL, CL_NIL, CL_NIL, tag);
            {
                CL_Restart *rp = (CL_Restart *)CL_OBJ_TO_PTR(mw_restart);
                cl_restart_stack[cl_restart_top].name    = rp->name;
                cl_restart_stack[cl_restart_top].handler = rp->function;
                cl_restart_stack[cl_restart_top].tag     = rp->tag;
                cl_restart_stack[cl_restart_top].restart = mw_restart;
            }
            cl_restart_top++;
        }

        if (setjmp(frame->buf) != 0) {
            /* muffle-warning was invoked.  The throw may have unwound through
             * one or more bytecode VM frames — this happens whenever the
             * WARNING handler is a Lisp closure that *calls* (muffle-warning)
             * (e.g. fiasco's `(lambda (a) ... (muffle-warning))`), as opposed
             * to #'muffle-warning being installed as the handler directly.  In
             * that case cl_vm.sp/fp (and any dynamic bindings, GC roots or
             * C-level error frames the handler established) are left dangling
             * by the longjmp.  Restore them from the saved NLX frame exactly as
             * the canonical CATCH landing in vm.c does, otherwise the VM stack
             * stays misaligned and later code misbehaves (observed: a spurious
             * "restart MUFFLE-WARNING not found" / VM re-entry).
             *
             * Recompute the frame pointer from the global stack: locals are
             * indeterminate after longjmp (C99 7.13.2.1) and cl_throw_to_tag
             * set cl_nlx_top to this frame's index. */
            CL_NLXFrame *f = &cl_nlx_stack[cl_nlx_top];
            cl_dynbind_restore_to(f->dyn_mark);
            cl_handler_top = f->handler_mark;
            cl_handler_active_mask = f->handler_active_mask;
            cl_restart_top = f->restart_mark;
            cl_error_frame_top = f->error_mark;
            gc_root_count = f->gc_root_mark;
            cl_jit_restore_depth(f->saved_jit_depth, f->saved_jit_c_floor);
            cl_compiler_unwind_to(f->compiler_mark, CL_CAPTURE_SP());
            cl_fasl_reader_unwind_to(CL_CAPTURE_SP());
            cl_fasl_writer_unwind_to(CL_CAPTURE_SP());
            cl_printer_state_restore(f->printer_mark);
            cl_saved_pending_top = f->saved_pending_mark;
            CT->mv_save_top = f->mv_save_mark;
            cl_vm.sp = f->vm_sp;
            cl_vm.fp = f->vm_fp;
            muffled = 1;
        }
    }

    if (!muffled) {
        cl_signal_condition(cond);

        /* No handler transferred control — print warning and return NIL */
        {
            CL_Condition *c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
            CL_Obj report = format_condition_report(c);
            /* format_condition_report allocates — re-derive c from the
             * rooted cond before touching it again. */
            c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
            cl_color_set(CL_COLOR_YELLOW);
            cl_write_cstring_to_error("WARNING: ");
            if (!CL_NULL_P(report) && CL_ANY_STRING_P(report)) {
                /* The report is wide whenever the formatted message holds
                 * any non-ASCII character — bounded UTF-8 conversion
                 * instead of reading CL_String bytes. */
                char rbuf[512];
                cl_string_to_utf8(report, rbuf, sizeof(rbuf));
                cl_write_cstring_to_error(rbuf);
            } else {
                char buf[128];
                cl_prin1_to_string(c->type_name, buf, sizeof(buf));
                cl_write_cstring_to_error(buf);
            }
            cl_color_reset();
            cl_write_cstring_to_error("\n");
        }
    }

    /* Clean up restart, handler, and NLX frames.
     * Handler top must be restored because when muffle-warning longjmps
     * back here, cl_signal_condition's handler_top restore is skipped. */
    cl_restart_top = saved_restart_top;
    cl_handler_top = saved_handler_top;
    /* Pop our NLX catch frame if still there */
    if (cl_nlx_top > 0 && cl_nlx_stack[cl_nlx_top - 1].tag == tag)
        cl_nlx_top--;

    CL_GC_UNPROTECT(2);
    return CL_NIL;
}

/* (error condition-or-type &rest args) — new version with condition system */
static CL_Obj bi_error(CL_Obj *args, int n)
{
    CL_Obj cond = coerce_to_condition(args, n, SYM_SIMPLE_ERROR);
    CL_Condition *c;

    /* Replace report_string with the FORMATTED report so debuggers and
     * observers see the interpolated message rather than the raw
     * format-control (e.g. "foo ~S" → "foo <value>"). format_condition_report
     * returns the existing report_string unchanged when there is no
     * :format-control, so conditions signaled by name keep a NIL report
     * and let PRINT-OBJECT dispatch supply the text. */
    {
        CL_Obj rpt;
        /* cond must be rooted: format_condition_report allocates (string
         * stream + formatting), and a compaction would leave the local a
         * stale offset — the re-derive below would then write
         * report_string into the object's OLD location and the stale
         * cond would be signaled.  The protect extends across
         * cl_signal_condition too: a handler that runs and DECLINES
         * allocates freely, and the stale cond would then be handed to
         * the debugger.  Both calls below unwind via longjmp, which
         * restores the root count — the protect needs no explicit pop. */
        CL_GC_PROTECT(cond);
        c = (CL_Condition *)CL_OBJ_TO_PTR(cond);
        rpt = format_condition_report(c);
        if (!CL_NULL_P(rpt) && CL_ANY_STRING_P(rpt)) {
            c = (CL_Condition *)CL_OBJ_TO_PTR(cond);  /* GC may have moved it */
            c->report_string = rpt;
        }
    }

    cl_signal_condition(cond);

    /* No handler transferred control — invoke debugger with the
     * original condition so its type and slots (and PRINT-OBJECT
     * method, if any) are visible. */
    cl_error_from_condition(cond);
    return CL_NIL;  /* unreachable */
}

/* --- Restart builtins --- */

/* Throw to a catch tag — used by restart invocation and debugger */
void cl_throw_to_tag(CL_Obj tag, CL_Obj value)
{
    int i;
    /* cl_nlx_floor: no target below a foreign-callback boundary (thread.h) */
    for (i = cl_nlx_top - 1; i >= cl_nlx_floor; i--) {
        if (cl_nlx_stack[i].type == CL_NLX_CATCH &&
            cl_nlx_stack[i].tag == tag) {
            int j;
            /* Check for interposing UWPROT frames (skip stale ones) */
            for (j = cl_nlx_top - 1; j > i; j--) {
                if (cl_nlx_stack[j].type == CL_NLX_UWPROT) {
                    /* Skip stale NLX frames (frame was reused by tail call) */
                    CL_Frame *tf = &cl_vm.frames[cl_nlx_stack[j].vm_fp - 1];
                    if (tf->code != cl_nlx_stack[j].code) continue;
                    cl_pending_throw = 1;
                    cl_pending_tag = tag;
                    cl_pending_value = value;
                    cl_pending_mv_count = cl_mv_count;
                    { int mi; for (mi = 0; mi < cl_mv_count && mi < CL_MAX_MV; mi++)
                        cl_pending_mv_values[mi] = cl_mv_values[mi]; }
                    cl_nlx_top = j;
                    cl_nlx_jump(&cl_nlx_stack[j]);
                }
            }
            cl_nlx_stack[i].result = value;
            /* Preserve multiple values across NLX */
            cl_nlx_stack[i].mv_count = cl_mv_count;
            { int mi; for (mi = 0; mi < cl_mv_count && mi < CL_MAX_MV; mi++)
                cl_nlx_stack[i].mv_values[mi] = cl_mv_values[mi]; }
            cl_nlx_top = i;
            cl_nlx_jump(&cl_nlx_stack[i]);
        }
    }
    cl_error(CL_ERR_GENERAL, "INVOKE-RESTART: no catch for restart tag");
}

/* Locate the binding for a restart designator (a restart object — matched by
 * identity — or a restart name symbol — innermost match wins).  Searches
 * top-down respecting the floor.  Returns the binding index or -1. */
static int find_restart_binding(CL_Obj designator)
{
    int i;
    int by_object = CL_RESTART_P(designator);
    for (i = cl_restart_top - 1; i >= cl_restart_floor; i--) {
        if (by_object) {
            if (cl_restart_stack[i].restart == designator)
                return i;
        } else if (cl_restart_stack[i].name == designator) {
            return i;
        }
    }
    return -1;
}

/* Is the restart at binding i applicable to condition?  Consults the
 * restart's :test function (called with the condition, possibly NIL). */
static int restart_applicable(int i, CL_Obj condition)
{
    CL_Obj robj = cl_restart_stack[i].restart;
    CL_Obj test, targs[1], r;
    if (!CL_RESTART_P(robj))
        return 1;
    test = ((CL_Restart *)CL_OBJ_TO_PTR(robj))->test;
    if (CL_NULL_P(test))
        return 1;
    targs[0] = condition;
    r = cl_vm_apply(test, targs, 1);
    return !CL_NULL_P(r);
}

/* Invoke the restart at binding i with call_args, transferring control to
 * its catch tag.  Does not return.
 *
 * Per CLHS INVOKE-RESTART, the non-local transfer completes (running every
 * interposing UNWIND-PROTECT cleanup) BEFORE the handler is called.  We
 * achieve this by throwing a dispatch cons (handler . args-list) to the catch
 * tag; the catch landing in compile_restart_case applies the handler after the
 * unwind has already run. */
/* Invoke the restart at binding i with an already-built argument LIST.
 * Used directly by the interactive path, where the arguments come back
 * from Lisp as a list — copying them through a C array would leave the
 * later elements stale once the first cl_cons compacts. */
static CL_Obj invoke_restart_with_list(int i, CL_Obj args_list)
{
    CL_Obj dispatch_cons;
    /* Build dispatch cons (handler . args-list); the handler slot lives in
     * the GC-rooted restart stack, read at call time. */
    dispatch_cons = cl_cons(cl_restart_stack[i].handler, args_list);
    cl_throw_to_tag(cl_restart_stack[i].tag, dispatch_cons);
    return CL_NIL; /* unreachable */
}

static CL_Obj invoke_restart_at_binding(int i, CL_Obj *call_args, int n_call)
{
    CL_Obj args_list = CL_NIL;
    int j;

    /* Build argument list; protect args_list across each allocating cl_cons.
     * call_args points into the rooted VM stack at every call site (or is a
     * single-element array read before the first cons), so re-reading
     * call_args[j] after a compaction is safe. */
    CL_GC_PROTECT(args_list);
    for (j = n_call - 1; j >= 0; j--)
        args_list = cl_cons(call_args[j], args_list);
    CL_GC_UNPROTECT(1);

    return invoke_restart_with_list(i, args_list);
}

/* Debugger entry (numeric restart selection): same dispatch protocol as
 * INVOKE-RESTART, so the handler runs at the catch landing after the
 * unwind.  See error.h. */
void cl_invoke_restart_by_stack_index(int stack_idx)
{
    (void)invoke_restart_at_binding(stack_idx, NULL, 0);
}

/* (invoke-restart restart-designator &rest args) */
static CL_Obj bi_invoke_restart(CL_Obj *args, int n)
{
    int i = find_restart_binding(args[0]);
    if (i < 0) {
        char buf[64];
        cl_prin1_to_string(args[0], buf, sizeof(buf));
        cl_error(CL_ERR_GENERAL, "INVOKE-RESTART: restart %s not found", buf);
    }
    return invoke_restart_at_binding(i, args + 1, n - 1);
}

/* (invoke-restart-interactively restart-designator) — call the restart's
 * :interactive function (if any) to obtain the argument list, then invoke. */
static CL_Obj bi_invoke_restart_interactively(CL_Obj *args, int n)
{
    int i = find_restart_binding(args[0]);
    CL_Obj interactive, arglist = CL_NIL;
    CL_UNUSED(n);

    if (i < 0) {
        char buf[64];
        cl_prin1_to_string(args[0], buf, sizeof(buf));
        cl_error(CL_ERR_GENERAL,
                 "INVOKE-RESTART-INTERACTIVELY: restart %s not found", buf);
    }

    interactive = ((CL_Restart *)CL_OBJ_TO_PTR(cl_restart_stack[i].restart))
                      ->interactive;
    if (!CL_NULL_P(interactive)) {
        arglist = cl_vm_apply(interactive, NULL, 0);
        /* Pass the returned list straight through — snapshotting it into a
         * C array left elements 2..n stale after the dispatch cons
         * compacted (the array is invisible to GC).  Indices into the
         * GC-rooted restart_stack stay valid; nothing was popped. */
    }
    return invoke_restart_with_list(i, arglist);
}

/* (find-restart identifier &optional condition) — return the innermost
 * applicable restart object matching identifier (a restart object or a
 * restart name), or NIL. */
static CL_Obj bi_find_restart(CL_Obj *args, int n)
{
    CL_Obj condition = (n >= 2) ? args[1] : CL_NIL;
    int by_object = CL_RESTART_P(args[0]);
    CL_Obj found = CL_NIL;
    int i;

    CL_GC_PROTECT(condition);
    for (i = cl_restart_top - 1; i >= cl_restart_floor; i--) {
        if (by_object) {
            /* CLHS: condition filter applies even when identifier is a restart
             * object, so consult restart_applicable. */
            if (cl_restart_stack[i].restart == args[0] &&
                restart_applicable(i, condition)) {
                found = cl_restart_stack[i].restart;
                break;
            }
        } else if (cl_restart_stack[i].name == args[0] &&
                   restart_applicable(i, condition)) {
            found = cl_restart_stack[i].restart;
            break;
        }
    }
    CL_GC_UNPROTECT(1);
    return found;
}

/* (compute-restarts &optional condition) — return list of applicable restart
 * objects, innermost first. */
static CL_Obj bi_compute_restarts(CL_Obj *args, int n)
{
    CL_Obj condition = (n >= 1) ? args[0] : CL_NIL;
    CL_Obj result = CL_NIL;
    int i;

    CL_GC_PROTECT(condition);
    CL_GC_PROTECT(result);
    /* Iterate floor..top so that consing produces innermost-first order. */
    for (i = cl_restart_floor; i < cl_restart_top; i++) {
        if (restart_applicable(i, condition))
            result = cl_cons(cl_restart_stack[i].restart, result);
    }
    CL_GC_UNPROTECT(2);
    return result;
}

/* (restart-name restart) — return the restart's name symbol (or NIL). */
static CL_Obj bi_restart_name(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_RESTART_P(args[0]))
        cl_error(CL_ERR_TYPE, "RESTART-NAME: argument is not a restart");
    return ((CL_Restart *)CL_OBJ_TO_PTR(args[0]))->name;
}

/* (abort &optional condition) */
static CL_Obj bi_abort(CL_Obj *args, int n)
{
    CL_Obj abort_args[1];
    CL_UNUSED(args);
    CL_UNUSED(n);
    abort_args[0] = SYM_ABORT;
    return bi_invoke_restart(abort_args, 1);
}

/* (continue &optional condition) — invoke CONTINUE restart if available */
static CL_Obj bi_continue_restart(CL_Obj *args, int n)
{
    int i;
    CL_UNUSED(args);
    CL_UNUSED(n);

    /* Only invoke if CONTINUE restart is available */
    for (i = cl_restart_top - 1; i >= cl_restart_floor; i--) {
        if (cl_restart_stack[i].name == SYM_CONTINUE) {
            CL_Obj cont_args[1];
            cont_args[0] = SYM_CONTINUE;
            return bi_invoke_restart(cont_args, 1);
        }
    }
    return CL_NIL;
}

/* (muffle-warning &optional condition) */
static CL_Obj bi_muffle_warning(CL_Obj *args, int n)
{
    CL_Obj mw_args[1];
    CL_UNUSED(args);
    CL_UNUSED(n);
    mw_args[0] = SYM_MUFFLE_WARNING;
    return bi_invoke_restart(mw_args, 1);
}

/* Shared body for STORE-VALUE / USE-VALUE (CLHS 9.1): invoke the innermost
 * restart named NAME that is applicable to the optional condition, passing
 * VALUE as its single argument.  If no such restart is established, return NIL
 * (these functions never error on a missing restart). */
static CL_Obj invoke_value_restart(CL_Obj name, CL_Obj *args, int n)
{
    CL_Obj value = (n >= 1) ? args[0] : CL_NIL;
    CL_Obj condition = (n >= 2) ? args[1] : CL_NIL;
    CL_Obj call_args[1];
    int i;
    /* restart_applicable can call cl_vm_apply (for :test fns), which may
     * compact.  Protect all three locals so subsequent iterations see
     * current arena offsets after any compaction. */
    CL_GC_PROTECT(value);
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(condition);
    for (i = cl_restart_top - 1; i >= cl_restart_floor; i--) {
        if (cl_restart_stack[i].name == name && restart_applicable(i, condition)) {
            call_args[0] = value;
            CL_GC_UNPROTECT(3);
            return invoke_restart_at_binding(i, call_args, 1);
        }
    }
    CL_GC_UNPROTECT(3);
    return CL_NIL;
}

/* (store-value value &optional condition) */
static CL_Obj bi_store_value(CL_Obj *args, int n)
{
    return invoke_value_restart(SYM_STORE_VALUE, args, n);
}

/* (use-value value &optional condition) */
static CL_Obj bi_use_value(CL_Obj *args, int n)
{
    return invoke_value_restart(SYM_USE_VALUE, args, n);
}

/* --- Registration --- */

/* Release the condition-hierarchy hash index at process exit — a
 * platform_alloc'd table sized to twice the number of registered condition
 * types, rebuilt rather than grown, so nothing else hands it back. */
void cl_builtins_condition_shutdown(void)
{
    cl_alist_index_reset(&cond_index);
}

/* The HANDLER-CASE special form's operator, AMILISP::%HANDLER-CASE (what the
 * boot.lisp HANDLER-CASE macro expands to); recognized by identity in
 * compile_expr.  Interned in AMILISP at init and GC-rooted, like
 * cl_struct_ref_sym. */
CL_Obj cl_handler_case_sym = CL_NIL;

void cl_builtins_condition_init(void)
{
    cl_handler_case_sym = cl_intern_in("%HANDLER-CASE", 13, cl_package_amilisp);
    cl_gc_register_root(&cl_handler_case_sym);
    /* After a shutdown/re-init cycle (test harnesses) these statics
     * still hold PREVIOUS-arena offsets, and gc_mark marks them —
     * setting "mark bits" at interior positions of unrelated
     * fresh-arena objects, i.e. silent corruption whose victim moves
     * with heap layout (see cl_mem_init's n_global_roots reset).
     * No-op in a normal once-per-process boot. */
    condition_hierarchy = CL_NIL;
    cl_alist_index_reset(&cond_index);
    condition_slot_table = CL_NIL;
    condition_default_initargs = CL_NIL;
    condition_slot_initforms = CL_NIL;

    /* Build condition type hierarchy */
    build_hierarchy();
    /* build_hierarchy prepends the builtin entries with raw assignments
     * (single-threaded init, no lock needed) — mark the index stale so
     * the first lookup rebuilds over them. */
    cl_alist_index_invalidate(&cond_index);

    /* Register builtins */
    defun("MAKE-CONDITION", bi_make_condition, 1, -1);
    /* Implementation-specific predicates / accessors live in AMILISP.
     * CL :uses AMILISP, so boot.lisp / clos.lisp / test code can see
     * them as bare symbols.  User packages that only (:use :common-lisp)
     * must add (:use :amilisp) or qualify with the CLAMIGA: prefix. */
    cl_register_builtin("CONDITIONP", bi_conditionp, 1, 1, cl_package_amilisp);
    cl_register_builtin("CONDITION-TYPE-NAME", bi_condition_type_name, 1, 1, cl_package_amilisp);
    defun("SIMPLE-CONDITION-FORMAT-CONTROL", bi_simple_condition_format_control, 1, 1);
    defun("SIMPLE-CONDITION-FORMAT-ARGUMENTS", bi_simple_condition_format_arguments, 1, 1);
    defun("TYPE-ERROR-DATUM", bi_type_error_datum, 1, 1);
    defun("TYPE-ERROR-EXPECTED-TYPE", bi_type_error_expected_type, 1, 1);
    defun("STREAM-ERROR-STREAM", bi_stream_error_stream, 1, 1);
    defun("PACKAGE-ERROR-PACKAGE", bi_package_error_package, 1, 1);
    defun("CELL-ERROR-NAME", bi_cell_error_name, 1, 1);
    defun("ARITHMETIC-ERROR-OPERANDS", bi_arithmetic_error_operands, 1, 1);
    defun("ARITHMETIC-ERROR-OPERATION", bi_arithmetic_error_operation, 1, 1);
    defun("FILE-ERROR-PATHNAME", bi_file_error_pathname, 1, 1);
    defun("UNBOUND-SLOT-INSTANCE", bi_unbound_slot_instance, 1, 1);

    /* User-defined condition types */
    cl_register_builtin("%REGISTER-CONDITION-TYPE", bi_register_condition_type, 3, 3, cl_package_amilisp);
    cl_register_builtin("CONDITION-SLOT-VALUE", bi_condition_slot_value, 2, 2, cl_package_amilisp);
    cl_register_builtin("%SET-CONDITION-SLOT-VALUE", bi_set_condition_slot_value, 3, 3, cl_package_amilisp);
    cl_register_builtin("CONDITION-SLOT-BOUNDP", bi_condition_slot_boundp, 2, 2, cl_package_amilisp);
    cl_register_builtin("%CONDITION-SLOT-MAKUNBOUND", bi_condition_slot_makunbound, 2, 2, cl_package_amilisp);
    cl_register_builtin("%SET-CONDITION-DEFAULT-INITARGS", bi_set_condition_default_initargs, 2, 2, cl_package_amilisp);
    cl_register_builtin("%REGISTER-CONDITION-SLOT-INITFORMS", bi_register_condition_slot_initforms, 2, 2, cl_package_amilisp);

    /* Signaling */
    defun("SIGNAL", bi_signal, 1, -1);
    defun("WARN", bi_warn, 1, -1);
    defun("ERROR", bi_error, 1, -1);

    /* Restarts */
    defun("INVOKE-RESTART", bi_invoke_restart, 1, -1);
    defun("INVOKE-RESTART-INTERACTIVELY", bi_invoke_restart_interactively, 1, 1);
    defun("FIND-RESTART", bi_find_restart, 1, 2);
    defun("COMPUTE-RESTARTS", bi_compute_restarts, 0, 1);
    defun("RESTART-NAME", bi_restart_name, 1, 1);
    defun("ABORT", bi_abort, 0, 1);
    defun("CONTINUE", bi_continue_restart, 0, 1);
    defun("MUFFLE-WARNING", bi_muffle_warning, 0, 1);
    defun("STORE-VALUE", bi_store_value, 1, 2);
    defun("USE-VALUE", bi_use_value, 1, 2);
    /* Register restart-name globals as GC roots so compaction forwards them;
     * invoke_value_restart compares cl_restart_stack[i].name (GC-forwarded)
     * against these, so both sides must track through compaction. */
    cl_gc_register_root(&SYM_STORE_VALUE);
    cl_gc_register_root(&SYM_USE_VALUE);
    cl_gc_register_root(&muffle_handler);
}
