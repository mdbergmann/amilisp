#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "printer.h"
#include "bignum.h"
#include "compiler.h"
#include "reader.h"
#include "stream.h"
#include "repl.h"
#include "vm.h"
#include "opcodes.h"
#include "float.h"
#include "fasl.h"
#include "thread.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

/* Per-unit FASL serialization buffer cap.  When a top-level form's
 * serialized bytecode exceeds this, the unit (and the whole file's
 * FASL) is dropped from the cache and the file is recompiled from
 * source on the next session.  Default is generous (64 MB) — the
 * pathological case is a cyclic constant graph rather than honestly
 * large output.  Override at build time with
 * -DCL_FASL_UNIT_CAP_BYTES=<bytes> to stress-test the dedup. */
#ifndef CL_FASL_UNIT_CAP_BYTES
#define CL_FASL_UNIT_CAP_BYTES (64u * 1024u * 1024u)
#endif

/* Forward declarations for helpers used in bi_load before definition */
static int make_fasl_cache_path(const char *input, char *output, uint32_t outsize);
static void path_directory(const char *path, char *dir, uint32_t dirsz);
static void mkdir_p(const char *path);


/* Helper to register an extension builtin in the EXT package (exported).
 * Routed through cl_register_builtin_exported so the image-relink registry
 * sees the registration (builtins.h). */
static void extfun(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin_exported(name, func, min, max, cl_package_ext);
}

/* Output-stream resolution is shared with builtins_stream.c
 * (cl_resolve_output_stream, declared in stream.h). */

/* --- Keywords for WRITE --- */

static CL_Obj KW_WR_STREAM;
static CL_Obj KW_WR_ESCAPE;
static CL_Obj KW_WR_READABLY;
static CL_Obj KW_WR_BASE;
static CL_Obj KW_WR_RADIX;
static CL_Obj KW_WR_LEVEL;
static CL_Obj KW_WR_LENGTH;
static CL_Obj KW_WR_CASE;
static CL_Obj KW_WR_GENSYM;
static CL_Obj KW_WR_ARRAY;
static CL_Obj KW_WR_CIRCLE;
static CL_Obj KW_WR_PRETTY;
static CL_Obj KW_WR_RIGHT_MARGIN;
static CL_Obj KW_WR_PPRINT_DISPATCH;

/* --- EVAL-WHEN situation keywords (CLHS 3.2.3.1, Table 3-9) --- */
/* Modern keyword forms: :compile-toplevel, :load-toplevel, :execute */
static CL_Obj KW_EW_CT;         /* :compile-toplevel */
static CL_Obj KW_EW_LT;         /* :load-toplevel */
static CL_Obj KW_EW_EX;         /* :execute */
/* Legacy CL-package synonyms: CL:COMPILE, CL:LOAD, CL:EVAL (same symbols as
 * the functions — valid per CLHS as eval-when situation names) */
static CL_Obj SYM_EW_COMPILE;   /* CL:COMPILE */
static CL_Obj SYM_EW_LOAD;      /* CL:LOAD */
static CL_Obj SYM_EW_EVAL_SYM;  /* CL:EVAL (distinct from SYM_EVAL_WHEN) */

/* --- I/O --- */

/*
 * (write object &key :stream :escape :readably :base :radix :level
 *        :length :case :gensym :array :circle :pretty)
 * Outputs object honoring all *print-* variable overrides.
 * Returns object.
 */
static CL_Obj bi_write(CL_Obj *args, int n)
{
    CL_Obj stream;
    int i;
    /* The keyword overrides are THREAD-LOCAL dynamic binds (tier-4 FS16):
     * the old save/set-global/restore pattern raced every peer thread's
     * printer between the set and the restore.  cl_dynbind_c mirrors
     * OP_DYNBIND (the dyn stack is GC-marked and forwarded, so no manual
     * rooting of saved values is needed), and cl_dynbind_restore_to pops
     * the binds — removing the transient TLV entries entirely. */
    int dyn_mark = cl_dyn_top;

    /* Default: *standard-output* */
    stream = cl_symbol_value(SYM_STANDARD_OUTPUT);

    /* Parse keyword arguments (start at index 1, pairs) */
    for (i = 1; i + 1 < n; i += 2) {
        CL_Obj kw = args[i];
        CL_Obj val = args[i + 1];
        if (kw == KW_WR_STREAM) {
            if (val == CL_T) {
                stream = cl_symbol_value(SYM_TERMINAL_IO);
            } else if (!CL_NULL_P(val)) {
                stream = val;
            }
        } else if (kw == KW_WR_ESCAPE) {
            cl_dynbind_c(SYM_PRINT_ESCAPE, val);
        } else if (kw == KW_WR_READABLY) {
            cl_dynbind_c(SYM_PRINT_READABLY, val);
        } else if (kw == KW_WR_BASE) {
            cl_dynbind_c(SYM_PRINT_BASE, val);
        } else if (kw == KW_WR_RADIX) {
            cl_dynbind_c(SYM_PRINT_RADIX, val);
        } else if (kw == KW_WR_LEVEL) {
            cl_dynbind_c(SYM_PRINT_LEVEL, val);
        } else if (kw == KW_WR_LENGTH) {
            cl_dynbind_c(SYM_PRINT_LENGTH, val);
        } else if (kw == KW_WR_CASE) {
            cl_dynbind_c(SYM_PRINT_CASE, val);
        } else if (kw == KW_WR_GENSYM) {
            cl_dynbind_c(SYM_PRINT_GENSYM, val);
        } else if (kw == KW_WR_ARRAY) {
            cl_dynbind_c(SYM_PRINT_ARRAY, val);
        } else if (kw == KW_WR_CIRCLE) {
            cl_dynbind_c(SYM_PRINT_CIRCLE, val);
        } else if (kw == KW_WR_PRETTY) {
            cl_dynbind_c(SYM_PRINT_PRETTY, val);
        } else if (kw == KW_WR_RIGHT_MARGIN) {
            cl_dynbind_c(SYM_PRINT_RIGHT_MARGIN, val);
        } else if (kw == KW_WR_PPRINT_DISPATCH) {
            cl_dynbind_c(SYM_PRINT_PPRINT_DISPATCH, val);
        }
    }

    /* Print args[0] (rooted VM slot), not a stale entry copy — and return
     * it, for the same reason.  `stream` is current here: nothing between
     * its assignment and this call allocates (cl_dynbind_c is
     * non-allocating). */
    cl_write_to_stream(args[0], stream);

    cl_dynbind_restore_to(dyn_mark);

    return args[0];
}

static CL_Obj bi_print(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_output_stream(args, n, 1);
    cl_print_to_stream(args[0], stream);
    return args[0];
}

static CL_Obj bi_prin1(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_output_stream(args, n, 1);
    cl_prin1_to_stream(args[0], stream);
    return args[0];
}

static CL_Obj bi_princ(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_output_stream(args, n, 1);
    cl_princ_to_stream(args[0], stream);
    return args[0];
}

static CL_Obj bi_pprint(CL_Obj *args, int n)
{
    CL_Obj stream = cl_resolve_output_stream(args, n, 1);
    /* Thread-local dynamic binds (FS16) — see bi_write.  The dyn stack is
     * GC-marked, so the saved values need no manual roots; `stream` still
     * does (the newline write can grow a string stream and compact). */
    int dyn_mark = cl_dyn_top;
    CL_GC_PROTECT(stream);
    cl_dynbind_c(SYM_PRINT_ESCAPE, SYM_T);
    cl_dynbind_c(SYM_PRINT_PRETTY, SYM_T);
    cl_stream_write_char(stream, '\n');
    cl_write_to_stream(args[0], stream);
    cl_dynbind_restore_to(dyn_mark);
    CL_GC_UNPROTECT(1);
    return CL_NIL;
}

/* Defined in builtins_format.c */
extern void cl_format_to_stream(CL_Obj stream, CL_Obj *args, int n);
extern void cl_format_builtins_init(void);

static CL_Obj bi_format(CL_Obj *args, int n)
{
    CL_Obj dest = (n >= 1) ? args[0] : CL_NIL;

    if (CL_NULL_P(dest)) {
        /* (format nil ...) => return string */
        CL_Obj sstream = cl_make_string_output_stream();
        CL_GC_PROTECT(sstream);
        cl_format_to_stream(sstream, args, n);
        {
            CL_Obj result = cl_finish_string_output_stream(sstream);
            CL_GC_UNPROTECT(1);
            return result;
        }
    } else {
        /* T => *standard-output*, stream => that stream */
        CL_Obj stream;
        if (dest == CL_T) {
            stream = cl_symbol_value(SYM_STANDARD_OUTPUT);
        } else if (CL_STREAM_P(dest)) {
            stream = dest;
        } else {
            /* Treat anything else as T for backward compat */
            stream = cl_symbol_value(SYM_STANDARD_OUTPUT);
        }
        cl_format_to_stream(stream, args, n);
        return CL_NIL;
    }
}

/* --- Shared FASL emit helper ---------------------------------------------
 *
 * Serializes one bytecode into a per-unit buffer and appends it (length
 * prefix + bytes) to a file-level CL_FaslWriter.  Used by both bi_load's
 * source-load auto-cache and bi_compile_file's deferred-serialize loop —
 * before this helper existed, both call sites had near-identical (but
 * subtly drifting) copies of the overflow/retry/grow logic.  The CLOS
 * FASL truncation bug fixed in commit f52cb57 surfaced because the two
 * fragmented compile paths were both vulnerable; sharing the writer
 * means future similar fixes only need to land in one place.
 *
 * Manages:
 *   - per-unit serialize-with-retry: doubles unit_buf on FASL_ERR_OVERFLOW
 *     up to unit_cap_max (UINT32_MAX = unlimited), freeing shared_objs
 *     between retries so they don't leak;
 *   - file-level append: grows fasl_buf on demand, then writes
 *     u32(unit_pos) + unit bytes via fw.
 *
 * On failure returns -1 with no partial state written to fw; *err_out
 * (if non-NULL) receives the underlying FASL error so the caller can
 * emit a context-rich warning ("graph too deep" vs "exceeded unit cap"
 * etc.).  On success returns 0; uw's gensym table is synced back into
 * fw so subsequent units share the dedup state.  *unit_buf_p,
 * *unit_cap_p, *fasl_buf_p, *fasl_cap_p are updated if grow happened. */
static int cf_emit_fasl_unit(CL_FaslWriter *fw,
                             uint8_t **fasl_buf_p, uint32_t *fasl_cap_p,
                             uint8_t **unit_buf_p, uint32_t *unit_cap_p,
                             uint32_t unit_cap_max,
                             CL_Obj bytecode,
                             int *err_out)
{
    CL_FaslWriter uw;

    cl_fasl_reset_serialize_count();
    cl_fasl_writer_init(&uw, *unit_buf_p, *unit_cap_p);
    memcpy(uw.gensym_objs, fw->gensym_objs, fw->gensym_count * sizeof(CL_Obj));
    uw.gensym_count = fw->gensym_count;
#ifdef DEBUG_FASL
    cl_fasl_hist_begin();
#endif
    cl_fasl_serialize_bytecode(&uw, bytecode);
#ifdef DEBUG_FASL
    cl_fasl_hist_end();
#endif

    while (uw.error == FASL_ERR_OVERFLOW && *unit_cap_p < unit_cap_max) {
        uint32_t new_cap = *unit_cap_p * 2;
        uint8_t *new_buf;
        if (new_cap > unit_cap_max) new_cap = unit_cap_max;
        platform_free(*unit_buf_p);
        *unit_buf_p = NULL;
        /* Keep the registry's abandonment copy of unit_buf (if fw is
         * registered) from ever pointing at freed memory — see the writer
         * registry note in fasl.h/fasl.c. */
        cl_fasl_writer_note_scratch(fw, NULL);
        new_buf = (uint8_t *)platform_alloc(new_cap);
        if (!new_buf) {
            cl_fasl_writer_release(&uw);
            if (err_out) *err_out = FASL_ERR_OVERFLOW;
            return -1;
        }
        *unit_buf_p = new_buf;
        *unit_cap_p = new_cap;
        cl_fasl_writer_note_scratch(fw, *unit_buf_p);
        cl_fasl_writer_release(&uw);
        cl_fasl_writer_init(&uw, *unit_buf_p, *unit_cap_p);
        memcpy(uw.gensym_objs, fw->gensym_objs, fw->gensym_count * sizeof(CL_Obj));
        uw.gensym_count = fw->gensym_count;
#ifdef DEBUG_FASL
        cl_fasl_hist_begin();
#endif
        cl_fasl_serialize_bytecode(&uw, bytecode);
#ifdef DEBUG_FASL
        cl_fasl_hist_end();
#endif
    }

    if (uw.error != FASL_OK) {
        if (err_out) *err_out = uw.error;
        /* uw is a stack-local scratch writer; the caller diagnoses the
         * failure via fw (cl_fasl_nonportable_detail(fw)), so the refusal
         * detail set on uw must be copied onto fw before uw goes away. */
        memcpy(fw->nonportable_detail, uw.nonportable_detail,
               sizeof(fw->nonportable_detail));
        cl_fasl_writer_release(&uw);
        return -1;
    }

    /* Sync gensym table back so subsequent units see deduped IDs. */
    memcpy(fw->gensym_objs, uw.gensym_objs, uw.gensym_count * sizeof(CL_Obj));
    fw->gensym_count = uw.gensym_count;

    while (fw->pos + 4 + uw.pos > *fasl_cap_p) {
        uint32_t new_cap = *fasl_cap_p * 2;
        uint8_t *new_buf = (uint8_t *)platform_alloc(new_cap);
        if (!new_buf) {
            if (err_out) *err_out = FASL_ERR_OVERFLOW;
            cl_fasl_writer_release(&uw);
            return -1;
        }
        memcpy(new_buf, *fasl_buf_p, fw->pos);
        platform_free(*fasl_buf_p);
        *fasl_buf_p = new_buf;
        *fasl_cap_p = new_cap;
        fw->data = *fasl_buf_p;
        fw->capacity = *fasl_cap_p;
    }

    cl_fasl_write_u32(fw, uw.pos);
    cl_fasl_write_bytes(fw, *unit_buf_p, uw.pos);

    cl_fasl_writer_release(&uw);
    if (err_out) *err_out = FASL_OK;
    return 0;
}

/* Append the DEPS trailer (fasl.c) for the structure names DEPS to the
 * file-level writer, growing the buffer like cf_emit_fasl_unit does.
 * Returns 0 on success (including "nothing to write"), -1 when the buffer
 * could not grow — the caller must then drop the FASL: without its trailer
 * it would load unchecked. */
static int cf_emit_fasl_deps(CL_FaslWriter *fw,
                             uint8_t **fasl_buf_p, uint32_t *fasl_cap_p,
                             CL_Obj deps)
{
    uint32_t need = cl_fasl_deps_size(deps);

    if (need == 0) return 0;
    if (fw->pos + need > *fasl_cap_p) {
        uint32_t new_cap = fw->pos + need;
        uint8_t *new_buf = (uint8_t *)platform_alloc(new_cap);
        if (!new_buf) return -1;
        memcpy(new_buf, *fasl_buf_p, fw->pos);
        platform_free(*fasl_buf_p);
        *fasl_buf_p = new_buf;
        *fasl_cap_p = new_cap;
        fw->data = new_buf;
        fw->capacity = new_cap;
    }
    cl_fasl_write_deps(fw, deps);
    return fw->error == FASL_OK ? 0 : -1;
}

/* LOAD's implicit FASL cache can be switched off: --no-fasl-cache, or
 * CLAMIGA_FASL_CACHE=0 in the environment.  Every LOAD of a source file
 * then compiles it, and nothing is read from or written to the cache.  The
 * DEPS trailer catches a changed DEFSTRUCT; a changed macro or inline
 * function in another file it cannot see, and this is the answer to that.
 * COMPILE-FILE's default output path is not affected. */
int cl_load_fasl_cache_off = 0;

static int load_fasl_cache_enabled(void)
{
    char envbuf[16];
    const char *v;

    if (cl_load_fasl_cache_off) return 0;
    v = platform_getenv("CLAMIGA_FASL_CACHE", envbuf, sizeof(envbuf));
    return !(v && v[0] == '0' && v[1] == '\0');
}

/* --- Load --- */

/* Resolve a LOAD / COMPILE-FILE input designator (string or pathname) to a
 * C namestring in out[0..outsz-1]: parse → merge with
 * *default-pathname-defaults* (CL spec) → coerce → expand a leading ~.
 * `who` names the calling operator for error messages.  Signals on a
 * non-string/non-pathname input or an over-long resolved path.
 *
 * Consolidates the identical parse/merge/coerce/expand block that used to be
 * copy-pasted in bi_load, bi_compile_file, and bi_compile_file_pathname (the
 * fragmentation behind the f52cb57 truncation bug — see the note above). */
static void cl_resolve_input_namestring(CL_Obj input, char *out, size_t outsz,
                                        const char *who)
{
    extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
    extern const char *cl_coerce_to_namestring(CL_Obj arg, char *buf,
                                               uint32_t bufsz);
    extern CL_Obj bi_merge_pathnames(CL_Obj *margs, int mn);
    CL_Obj path_pn;
    CL_Obj merge_args[1];
    char ns_buf[1024];
    const char *expanded;

    if (CL_PATHNAME_P(input)) {
        path_pn = input;
    } else if (CL_STRING_P(input)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(input);
        path_pn = cl_parse_namestring(s->data, s->length);
    } else {
        cl_error(CL_ERR_TYPE, "%s: argument must be a string or pathname", who);
        return;  /* unreachable */
    }
    /* GC SAFETY (audit tier 4, IO12): merge_args[0]/path_pn are C-local slots
     * (not VM-rooted) and bi_merge_pathnames / coerce allocate — root them
     * across the calls or a compaction leaves a stale pre-move offset and
     * corrupts the resolved namestring. */
    merge_args[0] = path_pn;
    CL_GC_PROTECT(merge_args[0]);
    path_pn = bi_merge_pathnames(merge_args, 1);
    CL_GC_UNPROTECT(1);
    CL_GC_PROTECT(path_pn);
    cl_coerce_to_namestring(path_pn, ns_buf, sizeof(ns_buf));
    CL_GC_UNPROTECT(1);

    expanded = platform_expand_home(ns_buf, out, (int)outsz);
    if (expanded != out) {
        /* No ~ expansion happened — copy the merged namestring as-is. */
        size_t nslen = strlen(ns_buf);
        if (nslen >= outsz)
            cl_error(CL_ERR_GENERAL, "%s: path too long", who);
        memcpy(out, ns_buf, nslen);
        out[nslen] = '\0';
    }
}

static CL_Obj bi_load(CL_Obj *args, int n)
{
    /* The load path as a C copy: the input CL_String moves under compaction
     * and this function allocates all over (parse-namestring, compile, eval) —
     * a namestring read after any of those would be stale, so we resolve into
     * this fixed buffer up front. */
    char path_buf[1024];
    char *buf;
    unsigned long size;
    CL_Obj stream, expr, bytecode;
    const char *prev_file;
    const char *own_file;
    uint16_t own_file_id;
    uint16_t prev_file_id;
    int prev_line;

    CL_Obj load_pathname_obj, load_truename_obj;
    int verbose_explicit = 0, verbose = 0;
    CL_Obj if_dne = CL_UNBOUND;
    int i;
    /* CLHS 23.2.7: LOAD binds *PACKAGE* and *READTABLE* (and *LOAD-PATHNAME*
     * / *LOAD-TRUENAME*) around the load.  These are REAL dynamic bindings on
     * the dyn-bind stack — not C-local save/restore — because a condition
     * signaled mid-load and handled by the CALLER (handler-case around the
     * (load ...) call) unwinds via NLX straight past this C frame: no code
     * here runs.  Only the dyn stack is restored on that path (NLX landings
     * and the C error frames both pop it to their creation-time mark), so
     * only dyn bindings give LOAD the CLHS abort semantics.  The old locals
     * missed exactly that path: a file that did IN-PACKAGE and then errored
     * left *PACKAGE* switched in the caller (real-Amiga suite: one failed
     * load cascaded into 156 "Undefined function" errors because the test
     * package never came back).  The dyn stack is GC-marked and forwarded,
     * so the bound values need no manual rooting; every normal return pops
     * back to dyn_mark. */
    int dyn_mark;

    /* Reap a leaked MAKE-LOAD-FORM cache: if a prior COMPILE-FILE unwound
     * between its prepass and cleanup, g_mlf stayed active and the
     * auto-cache serialization below would consult its stale entries.
     * LOAD never runs while a compile-file's cache is legitimately live
     * (compile-file evaluates forms BEFORE its prepass), so active here
     * always means leaked. */
    if (cl_fasl_mlf_active_p())
        cl_fasl_mlf_cleanup();

    /* Parse keyword args: :verbose, :print, :if-does-not-exist, :external-format */
    for (i = 1; i + 1 < n; i += 2) {
        if (args[i] == cl_intern_keyword("VERBOSE", 7)) {
            verbose_explicit = 1;
            verbose = !CL_NULL_P(args[i + 1]);
        } else if (args[i] == cl_intern_keyword("IF-DOES-NOT-EXIST", 17)) {
            if_dne = args[i + 1];
        }
        /* :print, :external-format, and unknown kwargs: allow-other-keys behavior */
    }

    /* Sync cl_current_package from *package* special variable,
       so Lisp-level let-bindings of *package* are respected by the reader */
    {
        CL_Obj pkg_val = cl_symbol_value(SYM_STAR_PACKAGE);
        if (!CL_NULL_P(pkg_val))
            cl_current_package = pkg_val;
    }

    /* Establish the LOAD bindings (see the dyn_mark note above).  Both are
     * bound to their current values: IN-PACKAGE / readtable mutation in the
     * loaded file writes through the binding and is undone when it pops. */
    dyn_mark = cl_dyn_top;
    cl_dynbind_c(SYM_STAR_PACKAGE, cl_current_package);
    cl_dynbind_c(SYM_STAR_READTABLE, cl_symbol_value(SYM_STAR_READTABLE));

    /* CLHS: LOAD's filespec may be an (already open) stream — read and
     * evaluate forms from it until end of file, with *load-pathname* and
     * *load-truename* bound to NIL.  This is how SLYNK and ICL inject code:
     * (with-input-from-string (s ...) (load s)).  Gray streams are CLOS
     * instances, not CL_Streams, and fall through to the type error in
     * cl_resolve_input_namestring. */
    if (CL_STREAM_P(args[0])) {
        stream = args[0];
        CL_GC_PROTECT(stream);

        /* Forms from a stream have no backing file (CLHS). */
        cl_dynbind_c(SYM_STAR_LOAD_PATHNAME, CL_NIL);
        cl_dynbind_c(SYM_STAR_LOAD_TRUENAME, CL_NIL);

        /* Source-file context: forms from a stream have no backing file. */
        prev_file = cl_current_source_file;
        prev_file_id = cl_current_file_id;
        prev_line = cl_reader_get_line();
        cl_current_source_file = cl_intern_source_file("<stream>");
        cl_current_file_id++;
        cl_reader_reset_line();
        own_file = cl_current_source_file;
        own_file_id = cl_current_file_id;

        for (;;) {
            int err;
            int saved_fp, saved_sp, saved_nlx;

            /* see the file-load loop below */
            cl_current_source_file = own_file;
            cl_current_file_id = own_file_id;
            cl_debug_base_fp = 0;
            expr = cl_read_from_stream(stream);
            if (cl_reader_eof()) break;

            /* Save VM state so we can restore on error (cl_error longjmps
               past cl_vm_eval's OP_HALT, leaving fp/sp/nlx leaked) —
               mirrors the file-load loop below. */
            saved_fp = cl_vm.fp;
            saved_sp = cl_vm.sp;
            saved_nlx = cl_nlx_top;
            {
            int saved_gc_roots = gc_root_count;

            CL_CATCH(err);
            if (err == CL_ERR_NONE) {
                CL_GC_PROTECT(expr);
                bytecode = cl_compile(expr);
                CL_GC_UNPROTECT(1);
                if (!CL_NULL_P(bytecode)) {
                    CL_GC_PROTECT(bytecode);
                    cl_vm_eval(bytecode);
                    /* Safe point: run pending compaction between forms */
                    cl_gc_compact_if_pending();
                    CL_GC_UNPROTECT(1); /* bytecode */
                }
                CL_UNCATCH();
            } else {
                cl_vm.fp = saved_fp;
                cl_vm.sp = saved_sp;
                cl_nlx_top = saved_nlx;
                gc_root_count = saved_gc_roots;
                CL_UNCATCH();
                /* Propagate exit request — don't swallow (quit).
                 * cl_error(CL_ERR_EXIT) restores ALL dynamic bindings and
                 * the GC-root stack wholesale, so no manual pops here. */
                if (err == CL_ERR_EXIT) {
                    cl_current_source_file = prev_file;
                    cl_current_file_id = prev_file_id;
                    cl_reader_set_line(prev_line);
                    cl_error(CL_ERR_EXIT, "");
                }
                cl_error_print();
            }
            }
        }

        cl_current_source_file = prev_file;
        cl_current_file_id = prev_file_id;
        cl_reader_set_line(prev_line);

        /* Pop *load-truename*, *load-pathname*, *readtable*, *package* —
         * restoring the caller's values (and re-syncing cl_current_package). */
        cl_dynbind_restore_to(dyn_mark);

        CL_GC_UNPROTECT(1); /* stream */
        return SYM_T;
    }

    /* Resolve the filespec into path_buf: parse → merge with
       *default-pathname-defaults* (CL spec, so relative paths resolve against
       the caller's bound default, not just cwd) → coerce → expand a leading ~.
       See cl_resolve_input_namestring. */
    cl_resolve_input_namestring(args[0], path_buf, sizeof(path_buf), "LOAD");

    /* :if-does-not-exist NIL → return NIL without erroring.
       Check before the FASL cache block so we never substitute a stale cached
       FASL for a source file the caller explicitly said is OK to be missing. */
    if (if_dne == CL_NIL && !platform_file_exists(path_buf)) {
        cl_dynbind_restore_to(dyn_mark);
        return CL_NIL;
    }

    /* Check for cached FASL before reading source.  Skipped when:
       - .asd (shares base names with .lisp source files; must not be
         FASL-substituted via the cache).
       - .fasl input (the caller is already pointing at a precompiled
         artifact — typically a shipped lib/*.fasl or a hand-compiled
         output.  The cache exists to short-circuit a .lisp->fasl
         recompile; for .fasl inputs there is no recompile to skip,
         and an older stale cache here would silently shadow the file
         the caller asked for). */
    {
        char cache_path[1024];
        size_t plen2 = strlen(path_buf);
        int skip_cache =
            (plen2 >= 4 && strcmp(path_buf + plen2 - 4, ".asd") == 0) ||
            (plen2 >= 5 && strcmp(path_buf + plen2 - 5, ".fasl") == 0);
        if (!skip_cache && load_fasl_cache_enabled() &&
            make_fasl_cache_path(path_buf, cache_path, sizeof(cache_path)) &&
            platform_file_exists(cache_path))
        {
            uint32_t src_mtime = platform_file_mtime(path_buf);
            uint32_t fasl_mtime = platform_file_mtime(cache_path);
            if (fasl_mtime > 0 && fasl_mtime >= src_mtime) {
                /* Load cached FASL instead of source */
                buf = platform_file_read(cache_path, &size);
                if (buf) {
                    /* Verify it's actually a FASL with a compatible version.
                       Pre-validating the header avoids the cl_error/longjmp
                       path through cl_fasl_load when a cached FASL is stale
                       (e.g. left over from a different CL_FASL_VERSION). */
                    if (size >= 6) {
                        uint32_t magic = ((uint32_t)(uint8_t)buf[0] << 24) |
                                         ((uint32_t)(uint8_t)buf[1] << 16) |
                                         ((uint32_t)(uint8_t)buf[2] << 8) |
                                         ((uint32_t)(uint8_t)buf[3]);
                        uint32_t fver  = ((uint32_t)(uint8_t)buf[4] << 8) |
                                         ((uint32_t)(uint8_t)buf[5]);
                        char stale_dep[160];
                        if (magic == CL_FASL_MAGIC && fver != CL_FASL_VERSION) {
                            /* Stale FASL — drop the cache entry and fall
                               through to compile from source. */
                            platform_free(buf);
                            buf = NULL;
                            platform_file_delete(cache_path);
                        } else if (magic == CL_FASL_MAGIC &&
                                   !cl_fasl_check_deps((const uint8_t *)buf,
                                                       (uint32_t)size, stale_dep,
                                                       sizeof(stale_dep))) {
                            /* Compiled against another layout of a structure
                               (its DEFSTRUCT, in some other file, changed):
                               the inlined slot indices are wrong.  Same
                               treatment — recompile from source. */
                            int do_verbose = verbose_explicit ? verbose
                                : !CL_NULL_P(cl_symbol_value(SYM_STAR_LOAD_VERBOSE));
                            if (do_verbose) {
                                cl_write_cstring_to_stdout("; Recompiling ");
                                cl_write_cstring_to_stdout(path_buf);
                                cl_write_cstring_to_stdout(": structure ");
                                cl_write_cstring_to_stdout(stale_dep);
                                cl_write_cstring_to_stdout(" changed since it was cached\n");
                            }
                            platform_free(buf);
                            buf = NULL;
                            platform_file_delete(cache_path);
                        } else if (magic == CL_FASL_MAGIC) {
                            /* Try loading from FASL; fall back to source on error */
                            int fasl_err;
                            int lplt_mark;
                            /* Bind load-pathname and load-truename.
                             * GC SAFETY (audit tier 4, IO1): protect
                             * load_pathname_obj BEFORE the second
                             * cl_parse_namestring — it allocates, and the
                             * old protect-after left a stale offset bound
                             * into *LOAD-PATHNAME* (ASDF reads it
                             * constantly).  Mirrors bi_compile_file. */
                            {
                                extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
                                load_pathname_obj = cl_parse_namestring(path_buf,
                                    (uint32_t)strlen(path_buf));
                            }
                            CL_GC_PROTECT(load_pathname_obj);
                            {
                                char resolved[512];
                                if (platform_realpath(path_buf, resolved, (int)sizeof(resolved))) {
                                    extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
                                    load_truename_obj = cl_parse_namestring(resolved,
                                        (uint32_t)strlen(resolved));
                                } else {
                                    load_truename_obj = load_pathname_obj;
                                }
                            }
                            CL_GC_PROTECT(load_truename_obj);
                            /* Dynamic binds (see dyn_mark note at the top);
                             * the dyn stack roots the values, so the two C
                             * protects above can drop once both are in. */
                            lplt_mark = cl_dyn_top;
                            cl_dynbind_c(SYM_STAR_LOAD_PATHNAME, load_pathname_obj);
                            cl_dynbind_c(SYM_STAR_LOAD_TRUENAME, load_truename_obj);
                            CL_GC_UNPROTECT(2); /* lt_obj, lp_obj */

                            {
                                int do_verbose = verbose_explicit ? verbose
                                    : !CL_NULL_P(cl_symbol_value(SYM_STAR_LOAD_VERBOSE));
                                if (do_verbose) {
                                    cl_write_cstring_to_stdout("; Loading ");
                                    cl_write_cstring_to_stdout(cache_path);
                                    cl_write_cstring_to_stdout("\n");
                                }
                            }

                            {
                                int sf = cl_vm.fp, ss = cl_vm.sp, sn = cl_nlx_top;
                                int saved_gc_roots_fasl = gc_root_count;
                                CL_CATCH(fasl_err);
                                if (fasl_err == CL_ERR_NONE) {
                                    cl_fasl_load_prechecked((const uint8_t *)buf, (uint32_t)size);
                                    platform_free(buf);
                                    CL_UNCATCH();

                                    /* Pop lt, lp, readtable, package binds. */
                                    cl_dynbind_restore_to(dyn_mark);
                                    return SYM_T;
                                } else {
                                    /* Restore VM state leaked by aborted cl_vm_eval */
                                    cl_vm.fp = sf;
                                    cl_vm.sp = ss;
                                    cl_nlx_top = sn;
                                    gc_root_count = saved_gc_roots_fasl;  /* Restore leaked GC roots */
                                    CL_UNCATCH();
                                    /* Propagate exit request — don't swallow (quit).
                                     * cl_error(CL_ERR_EXIT) restores all dyn
                                     * bindings + GC roots wholesale. */
                                    if (fasl_err == CL_ERR_EXIT) {
                                        platform_free(buf);
                                        cl_error(CL_ERR_EXIT, "");
                                    }
                                    /* FASL load failed — pop the lp/lt binds
                                     * (the package/readtable binds stay for
                                     * the source-load fallthrough, which
                                     * re-binds lp/lt itself) */
                                platform_free(buf);
                                buf = NULL;
                                cl_dynbind_restore_to(lplt_mark);
                                /* Delete broken FASL so we don't retry it */
                                platform_file_delete(cache_path);
                            }
                            }
                        }
                    }
                    if (buf) platform_free(buf);
                }
            }
        }
    }

    buf = platform_file_read(path_buf, &size);
    if (!buf) {
        if (!platform_file_exists(path_buf)) {
            /* File disappeared between the earlier existence check and now (TOCTOU),
               or was never there.  Honour :if-does-not-exist nil. */
            if (if_dne == CL_NIL) {
                cl_dynbind_restore_to(dyn_mark);
                return CL_NIL;
            }
            cl_error(CL_ERR_GENERAL, "LOAD: file does not exist: %s", path_buf);
        } else {
            cl_error(CL_ERR_GENERAL, "LOAD: cannot read file: %s", path_buf);
        }
    }

    /* Bind *load-pathname* and *load-truename* per CL spec.
     * GC SAFETY (audit tier 4, IO1): protect load_pathname_obj BEFORE the
     * second cl_parse_namestring (it allocates) — see the FASL-cache twin
     * above. */
    {
        extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
        load_pathname_obj = cl_parse_namestring(path_buf,
                                                (uint32_t)strlen(path_buf));
    }
    CL_GC_PROTECT(load_pathname_obj);
    /* Resolve truename to absolute path */
    {
        char resolved[512];
        if (platform_realpath(path_buf, resolved, (int)sizeof(resolved))) {
            extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);
            load_truename_obj = cl_parse_namestring(resolved,
                                                    (uint32_t)strlen(resolved));
        } else {
            load_truename_obj = load_pathname_obj;
        }
    }
    CL_GC_PROTECT(load_truename_obj);
    /* Dynamic binds (see dyn_mark note at the top); the dyn stack roots the
     * values, so the two C protects above can drop once both are in. */
    cl_dynbind_c(SYM_STAR_LOAD_PATHNAME, load_pathname_obj);
    cl_dynbind_c(SYM_STAR_LOAD_TRUENAME, load_truename_obj);
    CL_GC_UNPROTECT(2); /* lt_obj, lp_obj */

    /* Print loading message respecting :verbose kwarg then *load-verbose* */
    {
        int do_verbose = verbose_explicit ? verbose
            : !CL_NULL_P(cl_symbol_value(SYM_STAR_LOAD_VERBOSE));
        if (do_verbose) {
            cl_write_cstring_to_stdout("; Loading ");
            cl_write_cstring_to_stdout(path_buf);
            cl_write_cstring_to_stdout("\n");
        }
    }

    /* Check if this is a FASL file (by magic bytes or extension) */
    {
        int is_fasl = 0;

        /* Check magic bytes first (most reliable) */
        if (size >= 4) {
            uint32_t magic = ((uint32_t)(uint8_t)buf[0] << 24) |
                             ((uint32_t)(uint8_t)buf[1] << 16) |
                             ((uint32_t)(uint8_t)buf[2] << 8) |
                             ((uint32_t)(uint8_t)buf[3]);
            if (magic == CL_FASL_MAGIC)
                is_fasl = 1;
        }

        if (is_fasl) {
            /* FASL binary loading — no source parsing needed */
            cl_fasl_load((const uint8_t *)buf, (uint32_t)size);
            platform_free(buf);

            /* Pop lt, lp, readtable, package binds. */
            cl_dynbind_restore_to(dyn_mark);
            return SYM_T;
        }
    }

    /* Source loading — also auto-cache to FASL for faster subsequent loads */

    /* Set up FASL auto-cache if cache path is available */
    {
        char auto_cache_path[1024];
        uint8_t *fasl_buf = NULL;
        uint8_t *unit_buf = NULL;
        /* Start big enough to hold the largest auto-cached FASL (lib/clos.lisp
         * is ~145 KB) without ever growing.  AmigaOS AllocVec of 512 KB
         * succeeds at startup but later doublings (256→512→1024) can fail
         * due to system pool fragmentation, which silently disables caching
         * for that load. */
        uint32_t fasl_capacity = 512 * 1024;
        uint32_t unit_capacity = 32 * 1024;
        uint32_t n_units = 0;
        /* CL_FaslWriter is ~4 KB (gensym_objs[1024]).  Keeping it on the
         * stack overflows the 64 KB AmigaOS stack when REQUIRE chains
         * nest 4-5 bi_load frames deep (e.g. test-gui → intuition →
         * amiga/ffi → ffi), silently corrupting adjacent stack memory
         * and producing wild CL_Obj values that surface much later as
         * "func_obj out of arena" or GC-ROOT-BUG aborts.  Heap-allocate
         * to keep each bi_load frame small — same mitigation that
         * bi_compile_file already uses. */
        CL_FaslWriter *fw = NULL;
        int do_cache = 0;

        if (load_fasl_cache_enabled() &&
            make_fasl_cache_path(path_buf, auto_cache_path, sizeof(auto_cache_path))) {
            /* Skip auto-caching for .asd files — they are ASDF system definitions
             * and would collide with .lisp files of the same name in the cache
             * (e.g. mt19937.asd and mt19937.lisp both map to mt19937.fasl) */
            size_t plen = strlen(path_buf);
            int is_asd = (plen >= 4 && strcmp(path_buf + plen - 4, ".asd") == 0);
            int is_fasl = (plen >= 5 && strcmp(path_buf + plen - 5, ".fasl") == 0);
            if (!is_asd && !is_fasl) {
            fasl_buf = (uint8_t *)platform_alloc(fasl_capacity);
            unit_buf = (uint8_t *)platform_alloc(unit_capacity);
            fw       = (CL_FaslWriter *)platform_alloc(sizeof(CL_FaslWriter));
            if (fasl_buf && unit_buf && fw) {
                do_cache = 1;
                cl_fasl_writer_init(fw, fasl_buf, fasl_capacity);
                cl_fasl_write_header(fw, 0); /* patch n_units later */
                /* GC-root the writer's gensym dedup table: units are
                 * serialized interleaved with compiling/evaluating later
                 * forms, which compact — unrooted entries went stale and
                 * could EQ-collide with a different gensym's new offset
                 * (silently wrong GENSYM_REF in the cached FASL).
                 *
                 * fw (and its scratch/output buffers) is heap-allocated, so
                 * an abandoning NLX past this whole function does not
                 * dangle-pointer-crash the way a stack-local reader would —
                 * but nothing else frees fw/fasl_buf/unit_buf for that path,
                 * so the registry still has to be told: register with a
                 * stack anchor (this frame) so cl_fasl_writer_unwind_to can
                 * recognize an abandoned registration, and note unit_buf so
                 * it can hand that buffer back too. */
                cl_fasl_writer_register(fw, CL_CAPTURE_SP());
                cl_fasl_writer_note_scratch(fw, unit_buf);
            } else {
                if (fasl_buf) platform_free(fasl_buf);
                if (unit_buf) platform_free(unit_buf);
                if (fw) platform_free(fw);
                fasl_buf = NULL;
                unit_buf = NULL;
                fw = NULL;
            }
            } /* !is_asd */
        }

    /* Collect the structure layouts this file's code gets compiled against
     * (builtins_struct.c); T = don't record, so a file that is not being
     * cached does not leak its names into an enclosing LOAD's list.  Popped
     * with the other binds by cl_dynbind_restore_to(dyn_mark). */
    cl_dynbind_c(cl_struct_deps_sym, do_cache ? CL_NIL : SYM_T);

    /* Save and set source file context.  The path is interned into the
     * source-file pool so any bytecode compiled here can hold a stable
     * pointer that outlives the CL_String (which may be GC-collected). */
    prev_file = cl_current_source_file;
    prev_file_id = cl_current_file_id;
    prev_line = cl_reader_get_line();
    cl_current_source_file = cl_intern_source_file(path_buf);
    cl_current_file_id++;
    cl_reader_reset_line();
    own_file = cl_current_source_file;
    own_file_id = cl_current_file_id;

    /* Use C-buffer stream — file content stays outside GC arena */
    stream = cl_make_cbuf_input_stream(buf, (uint32_t)size);
    if (CL_NULL_P(stream)) {
        /* Reading from NIL would hit EOF at once and "load" nothing, quietly.
         * The cache writer registered above goes with the error unwind. */
        platform_free(buf);
        cl_error(CL_ERR_GENERAL,
                 "LOAD: cannot open a source stream for %s - out of memory, "
                 "or over 250 source files being read at once", path_buf);
    }
    CL_GC_PROTECT(stream);

    for (;;) {
        int err;
        int saved_fp, saved_sp, saved_nlx;

        /* A nested LOAD that the previous form left by a non-local exit
         * (HANDLER-CASE around it) never ran its own restore below, so the
         * context still names that file: every function compiled from here
         * on would carry it into its backtraces.  Line numbers live in the
         * stream and need no such care. */
        cl_current_source_file = own_file;
        cl_current_file_id = own_file_id;
        /* The previous form's handled error left its frame snapshot behind
         * (see the --load loop in repl.c): a stale one truncates a later
         * EXT:BACKTRACE. */
        cl_debug_base_fp = 0;
        expr = cl_read_from_stream(stream);
        if (cl_reader_eof()) break;

        /* Save VM state so we can restore on error (cl_error longjmps
           past cl_vm_eval's OP_HALT, leaving fp/sp/nlx leaked) */
        saved_fp = cl_vm.fp;
        saved_sp = cl_vm.sp;
        saved_nlx = cl_nlx_top;
        {
        int saved_gc_roots = gc_root_count;

        CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            CL_GC_PROTECT(expr);
            bytecode = cl_compile(expr);
            CL_GC_UNPROTECT(1);
            if (!CL_NULL_P(bytecode)) {
                /* GC-protect bytecode across eval + serialization */
                CL_GC_PROTECT(bytecode);
                cl_vm_eval(bytecode);

                /* Safe point: run pending compaction between top-level forms */
                cl_gc_compact_if_pending();

                /* Append this form's bytecode to the FASL cache.  Any
                 * unit that fails to serialize (alloc fail, too-deep
                 * graph, etc.) invalidates the whole cache: writing a
                 * partial FASL would silently drop top-level forms (and
                 * their DEFUN side effects), making the cached load
                 * behave differently from the source load. */
                if (do_cache) {
                    if (cf_emit_fasl_unit(fw, &fasl_buf, &fasl_capacity,
                                          &unit_buf, &unit_capacity,
                                          UINT32_MAX, bytecode, NULL) == 0) {
                        n_units++;
                    } else {
                        cl_write_cstring_to_stdout("; Warning: FASL serialization failed - skipping cache for this file\n");
                        do_cache = 0;
                    }
                }
                CL_GC_UNPROTECT(1); /* bytecode */
            }
            CL_UNCATCH();
        } else {
            /* Restore VM state leaked by aborted cl_vm_eval */
            cl_vm.fp = saved_fp;
            cl_vm.sp = saved_sp;
            cl_nlx_top = saved_nlx;
            gc_root_count = saved_gc_roots;  /* Restore leaked GC roots */
            CL_UNCATCH();
            /* Propagate exit request — don't swallow (quit).
             * cl_error(CL_ERR_EXIT) restores all dyn bindings + GC roots
             * wholesale, so no manual pops here. */
            if (err == CL_ERR_EXIT) {
                CL_GC_UNPROTECT(1); /* stream */
                cl_stream_close(stream);
                platform_free(buf);
                if (unit_buf) platform_free(unit_buf);
                if (fasl_buf) platform_free(fasl_buf);
                if (fw) {
                    cl_fasl_writer_unregister(fw);
                    /* Same release the normal path does: the writer's
                     * shared-object table and hash are platform_alloc'd and
                     * are NOT freed by platform_free(fw).  They are normally
                     * NULL on the file-level writer (the per-unit scratch
                     * writer does the dedup), but the two paths must not
                     * disagree — this one is (QUIT) inside LOAD, i.e. exactly
                     * the exit path where a leak is permanent on AmigaOS. */
                    cl_fasl_writer_release(fw);
                    platform_free(fw);
                }
                cl_error(CL_ERR_EXIT, "");
            }
            cl_error_print();
            do_cache = 0; /* Don't cache files with errors */
        }
        }
    }

    CL_GC_UNPROTECT(1); /* stream */
    cl_stream_close(stream);
    platform_free(buf);

    /* Write FASL cache file if we collected units */
    if (do_cache && n_units > 0 &&
        cf_emit_fasl_deps(fw, &fasl_buf, &fasl_capacity,
                          cl_symbol_value(cl_struct_deps_sym)) != 0)
        do_cache = 0;   /* no room for the trailer: an unchecked FASL is worse than none */
    if (do_cache && n_units > 0) {
        char dir[1024];
        /* Patch n_units in header */
        fasl_buf[8]  = (uint8_t)(n_units >> 24);
        fasl_buf[9]  = (uint8_t)(n_units >> 16);
        fasl_buf[10] = (uint8_t)(n_units >> 8);
        fasl_buf[11] = (uint8_t)(n_units);
        /* Ensure directory exists */
        path_directory(auto_cache_path, dir, sizeof(dir));
        if (dir[0]) mkdir_p(dir);
        /* Write file.  Delete-then-create rather than overwrite: FS-UAE
         * directory hard drives keep a replaced file's ORIGINAL datestamp
         * (the .uaem sidecar is not refreshed on overwrite), so a cache
         * entry rewritten in place still Examine()s as older than the
         * edited source — stale forever, recompiled on every launch.
         * Recreating the file gets a fresh stamp; on real filesystems the
         * delete is redundant but harmless. */
        {
            PlatformFile fh;
            platform_file_delete(auto_cache_path);
            fh = platform_file_open(auto_cache_path, PLATFORM_FILE_WRITE);
            if (fh != PLATFORM_FILE_INVALID) {
                platform_file_write_buf(fh, (const char *)fasl_buf, fw->pos);
                platform_file_close(fh);
            }
        }
    }
    if (fasl_buf) platform_free(fasl_buf);
    if (unit_buf) platform_free(unit_buf);
    if (fw) {
        cl_fasl_writer_unregister(fw);
        cl_fasl_writer_release(fw);
        platform_free(fw);
    }
    } /* end auto-cache scope */

    /* Pop *load-truename*, *load-pathname*, *readtable*, *package* —
     * restoring the caller's values (and re-syncing cl_current_package). */
    cl_dynbind_restore_to(dyn_mark);

    /* Restore source file context */
    cl_current_source_file = prev_file;
    cl_current_file_id = prev_file_id;
    cl_reader_set_line(prev_line);

    return SYM_T;
}

/* --- Helper: recursive mkdir (like mkdir -p) --- */

static void mkdir_p(const char *path)
{
    char buf[1024];
    int i, len;
    len = (int)strlen(path);
    if (len >= (int)sizeof(buf)) return;
    memcpy(buf, path, (size_t)len + 1);
    for (i = 1; i < len; i++) {
#ifdef PLATFORM_AMIGA
        if (buf[i] == '/') {
            /* Don't mkdir before the colon (volume name) */
            int has_colon = 0;
            int j;
            for (j = 0; j < i; j++) {
                if (buf[j] == ':') { has_colon = 1; break; }
            }
            if (!has_colon) continue;
            buf[i] = '\0';
            platform_mkdir(buf);
            buf[i] = '/';
        }
#else
        if (buf[i] == '/') {
            buf[i] = '\0';
            platform_mkdir(buf);
            buf[i] = '/';
        }
#endif
    }
    platform_mkdir(buf);
}

/* --- Helper: construct FASL cache path from source path --- */

static int make_fasl_cache_path(const char *input, char *output, uint32_t outsize)
{
    char resolved[1024];
    char root[512];
    const char *src;
    const char *dot;
    size_t root_len, src_len, base_len, total;

    /* Resolve to absolute path — try realpath first, fall back to
       resolving directory + filename for non-existent files */
    if (!platform_realpath(input, resolved, (int)sizeof(resolved))) {
        /* File doesn't exist yet — try resolving directory */
        const char *last_sep = strrchr(input, '/');
#ifdef PLATFORM_AMIGA
        const char *colon = strrchr(input, ':');
        if (colon && (!last_sep || colon > last_sep))
            last_sep = colon;
#endif
        if (last_sep) {
            char dir_buf[1024];
            size_t dir_len = (size_t)(last_sep - input + 1);
            if (dir_len >= sizeof(dir_buf)) return 0;
            memcpy(dir_buf, input, dir_len);
            dir_buf[dir_len] = '\0';
            if (platform_realpath(dir_buf, resolved, (int)sizeof(resolved))) {
                size_t rlen = strlen(resolved);
                const char *fname = last_sep + 1;
                size_t flen = strlen(fname);
                if (rlen + 1 + flen >= sizeof(resolved)) return 0;
                if (rlen > 0 && resolved[rlen - 1] != '/') {
                    resolved[rlen] = '/';
                    rlen++;
                }
                memcpy(resolved + rlen, fname, flen + 1);
            } else {
                return 0;
            }
        } else if (input[0] == '/') {
            /* Already absolute but doesn't exist */
            if (strlen(input) >= sizeof(resolved)) return 0;
            strcpy(resolved, input);
        } else {
            /* Relative path, resolve from cwd */
            char cwd[512];
            size_t clen, ilen;
            if (!platform_getcwd(cwd, (int)sizeof(cwd))) return 0;
            clen = strlen(cwd);
            ilen = strlen(input);
            if (clen + 1 + ilen >= sizeof(resolved)) return 0;
            memcpy(resolved, cwd, clen);
            resolved[clen] = '/';
            memcpy(resolved + clen + 1, input, ilen + 1);
        }
    }

    /* Build cache root — include FASL format version so bumping
       CL_FASL_VERSION automatically invalidates stale caches. */
#ifdef PLATFORM_AMIGA
    snprintf(root, sizeof(root), "S:cl-amiga/faslcache/%s-fasl%d/",
             CL_VERSION_STRING, CL_FASL_VERSION);
    /* Transform "DH0:path" -> "DH0/path" by replacing ':' with '/' */
    {
        char *colon = strchr(resolved, ':');
        if (colon) *colon = '/';
    }
    src = resolved;
#else
    {
        char envbuf[400];
        /* CLAMIGA_FASL_CACHE_DIR overrides the default ~/.cache/common-lisp
           location. The load-and-test runner sets this per script so each
           script gets its own cache: scripts that compile the same shared
           library under different *features* (e.g. :hunchentoot-no-ssl) would
           otherwise poison one another's FASLs, since ASDF's staleness check
           keys only on source mtime, not on *features*. Still namespaced by
           CL_FASL_VERSION so a version bump invalidates it. */
        const char *override =
            platform_getenv("CLAMIGA_FASL_CACHE_DIR", envbuf, sizeof(envbuf));
        if (override && override[0]) {
            snprintf(root, sizeof(root), "%s/cl-amiga-%s-fasl%d/",
                     override, CL_VERSION_STRING, CL_FASL_VERSION);
        } else {
            const char *home = platform_getenv("HOME", envbuf, sizeof(envbuf));
#ifdef PLATFORM_WIN32
            /* A cmd.exe or PowerShell session has no $HOME; USERPROFILE is
               where Windows keeps the same thing. */
            if (!home || !home[0])
                home = platform_getenv("USERPROFILE", envbuf, sizeof(envbuf));
#endif
            if (!home) return 0;
            snprintf(root, sizeof(root), "%s/.cache/common-lisp/cl-amiga-%s-fasl%d/",
                     home, CL_VERSION_STRING, CL_FASL_VERSION);
        }
    }
#ifdef PLATFORM_WIN32
    /* The cache path mirrors the source path underneath the cache root, so
       the source's drive letter has to stop being a drive letter:
       "C:/tmp/x.lisp" becomes "C/tmp/x.fasl".  Left as "C:" it would name an
       alternate data stream on the cache directory and the create fails.
       The Amiga branch above does the same thing to a volume name.  The root
       itself is normalised to '/' so the whole path has one separator. */
    {
        char *p, *colon;
        for (p = root; *p; p++)
            if (*p == '\\') *p = '/';
        colon = strchr(resolved, ':');
        if (colon) memmove(colon, colon + 1, strlen(colon + 1) + 1);
    }
#endif
    /* Strip leading '/' */
    src = resolved;
    if (src[0] == '/') src++;
#endif

    root_len = strlen(root);
    src_len = strlen(src);

    /* Find last dot for extension replacement */
    dot = strrchr(src, '.');
    if (dot && dot > strrchr(src, '/')) {
        base_len = (size_t)(dot - src);
    } else {
        base_len = src_len;
    }

    total = root_len + base_len + 5; /* ".fasl" */
    if (total + 1 > outsize) return 0;

    memcpy(output, root, root_len);
    memcpy(output + root_len, src, base_len);
    memcpy(output + root_len + base_len, ".fasl", 5);
    output[root_len + base_len + 5] = '\0';
    return 1;
}

/* --- Helper: get directory portion of path --- */

static void path_directory(const char *path, char *dir, uint32_t dirsize)
{
    const char *last_sep = strrchr(path, '/');
#ifdef PLATFORM_AMIGA
    const char *colon = strrchr(path, ':');
    if (colon && (!last_sep || colon > last_sep))
        last_sep = colon;
#endif
    if (last_sep) {
        size_t len = (size_t)(last_sep - path + 1);
        if (len >= dirsize) len = dirsize - 1;
        memcpy(dir, path, len);
        dir[len] = '\0';
    } else {
        dir[0] = '\0';
    }
}

/* --- Helper: replace extension with .fasl --- */

static void make_fasl_path(const char *input, char *output, uint32_t outsize)
{
    const char *dot;
    size_t base_len;

    /* Find last dot */
    dot = strrchr(input, '.');
    if (dot) {
        base_len = (size_t)(dot - input);
    } else {
        base_len = strlen(input);
    }

    if (base_len + 5 >= outsize) {
        /* Truncate if needed */
        base_len = outsize - 6;
    }
    memcpy(output, input, base_len);
    memcpy(output + base_len, ".fasl", 5);
    output[base_len + 5] = '\0';
}

/* Returns 1 if this top-level form (after macroexpansion) must be evaluated
 * at compile time so that later forms in the same file compile correctly.
 * CLHS 3.2.3.1.2: only a small set of defining forms require this.
 * Notably absent: DEFVAR, DEFPARAMETER (proclaim-special is done inside
 * cl_compile itself), DEFUN (compiler records name/arglist; body must not
 * run at compile time), and user-level macros like DEFPACKAGE/DEFCLASS
 * (those expand to eval-when wrappers, handled via cf_process_toplevel_eval_when). */
static int cf_form_needs_compile_time_eval(CL_Obj expr)
{
    CL_Obj head;
    if (!CL_CONS_P(expr)) return 0;
    head = cl_car(expr);
    return head == SYM_DEFMACRO
        || head == SYM_DEFTYPE
        || head == SYM_DEFCONSTANT
        || head == SYM_DEFSETF
        || head == SYM_IN_PACKAGE;
}

/* Forward declarations — helpers reference each other. */
static void cf_bc_vec_append(CL_Obj *bc_vec_p, int *bc_count_p, CL_Obj bytecode);
/* Returns 1 if processing this form (re)set the reader package via an
 * IN-PACKAGE (at any nesting), so the caller must NOT restore the
 * pre-form package; 0 otherwise.  See the bi_compile_file read loop. */
static int cf_process_toplevel_form(CL_Obj expr,
                                    CL_Obj lex_env,
                                    CL_Obj *bc_vec_p,
                                    int *bc_count_p);

/* Process a top-level (eval-when (situations...) body...) per CLHS Table 3-9.
 * Called by cf_process_toplevel_form after the macroexpand-1 loop, when the
 * (already macroexpanded) head is SYM_EVAL_WHEN.
 *
 * CLHS Table 3-9 (compile-file top-level context):
 *   CT+LT → each body form is a fresh top-level form (compile+eval+collect)
 *   CT    → compile+eval each body; do NOT emit to FASL
 *   LT    → compile each body; emit to FASL; do NOT eval at compile time
 *   EX    → "not at top-level" semantics: compile+eval; do NOT emit
 *   none  → do nothing */
static int cf_process_toplevel_eval_when(CL_Obj form,
                                         CL_Obj lex_env,
                                         CL_Obj *bc_vec_p,
                                         int *bc_count_p)
{
    CL_Obj situations, body, sit, rest;
    int has_ct = 0, has_lt = 0, has_ex = 0;
    int saw_in_package = 0;   /* an IN-PACKAGE body form changed the reader pkg */

    /* lex_env is consulted on every loop iteration below, across cl_compile_lex
     * calls that can trigger moving GC; protect it so compaction forwards our
     * copy (the caller's protected copy does not cover this stack frame). */
    CL_GC_PROTECT(lex_env);

    situations = cl_car(cl_cdr(form));
    body       = cl_cdr(cl_cdr(form));

    /* Parse situation list; accept both modern keywords and legacy CL names */
    for (sit = situations; !CL_NULL_P(sit); sit = cl_cdr(sit)) {
        CL_Obj s = cl_car(sit);
        if      (s == KW_EW_CT || s == SYM_EW_COMPILE) has_ct = 1;
        else if (s == KW_EW_LT || s == SYM_EW_LOAD)    has_lt = 1;
        else if (s == KW_EW_EX || s == SYM_EW_EVAL_SYM) has_ex = 1;
    }

    /* CLHS Table 3-9.  Each active situation combination reduces to two
     * booleans over the same per-form loop:
     *   - do_eval    : evaluate the compiled form at compile time
     *   - do_collect : emit the compiled form into the FASL
     * CT+LT evaluates AND collects (so e.g. DEFPACKAGE — which expands to
     * (eval-when (:ct :lt :ex) ...) — creates the package at compile time,
     * letting later (in-package ...) forms in the same unit use the fast
     * compile_in_package path).  CT / EX evaluate only; LT collects only.
     * (EX at a non-top-level position behaves like CT here.) */
    {
        int do_eval = 0, do_collect = 0, active = 1;
        if (has_ct && has_lt)      { do_eval = 1; do_collect = 1; }
        else if (has_ct)           { do_eval = 1; }
        else if (has_lt)           { do_collect = 1; }
        else if (has_ex)           { do_eval = 1; }
        else                       { active = 0; }  /* no active situation */

        if (active) {
            rest = body;
            CL_GC_PROTECT(rest);
            while (!CL_NULL_P(rest)) {
                CL_Obj sub = cl_car(rest);
                CL_Obj bc;
                if (CL_CONS_P(sub) && cl_car(sub) == SYM_IN_PACKAGE)
                    saw_in_package = 1;
                cl_ltv_init_count = 0;
                CL_GC_PROTECT(sub);
                bc = CL_CONS_P(lex_env) ? cl_compile_lex(sub, lex_env)
                                        : cl_compile(sub);
                CL_GC_UNPROTECT(1);
                cl_ltv_init_count = 0;  /* discard unless collected into FASL */
                if (!CL_NULL_P(bc)) {
                    CL_GC_PROTECT(bc);
                    if (do_eval) cl_vm_eval(bc);
                    cl_gc_compact_if_pending();
                    if (do_collect)
                        cf_bc_vec_append(bc_vec_p, bc_count_p, bc);
                    CL_GC_UNPROTECT(1);
                }
                rest = cl_cdr(rest);
            }
            CL_GC_UNPROTECT(1);
        }
    }

    CL_GC_UNPROTECT(1); /* lex_env */
    return saw_in_package;
}

/* Process a single top-level form for compile-file.
 * Implements CLHS 3.2.3.1: macroexpand-1 loop, then PROGN recursion,
 * EVAL-WHEN dispatch (Table 3-9), or compile-only-eval-when-needed.
 *
 * Top-level atoms (bare symbols / numbers / strings) produce no effect. */
/* Append `bytecode` into the collected-bytecodes vector, growing if needed.
 * The vector is a Lisp object (not a host alloc) so its slots are GC roots
 * via *bc_vec_p — bytecodes accumulated here survive any GC triggered by
 * later top-level forms in the same compile-file run.  Without this, a
 * top-level form whose bytecode is not anchored by a defun (e.g. a bare
 * (format t ...)) gets swept once arena pressure rises, and the FASL
 * serializer then dereferences a stale CL_Obj — observed as a SIGSEGV in
 * cl_fasl_serialize_bytecode under heavy quicklisp loads. */
static void cf_bc_vec_append(CL_Obj *bc_vec_p, int *bc_count_p, CL_Obj bytecode)
{
    CL_Vector *vec = (CL_Vector *)CL_OBJ_TO_PTR(*bc_vec_p);
    if ((uint32_t)*bc_count_p >= vec->length) {
        CL_Obj new_vec_obj;
        CL_Vector *new_vec;
        uint32_t i;
        uint32_t new_len = vec->length ? vec->length * 2 : 4096;
        /* GC-protect bytecode across the allocation — it is not yet rooted
         * via the vector. */
        CL_GC_PROTECT(bytecode);
        new_vec_obj = cl_make_vector(new_len);
        CL_GC_UNPROTECT(1);
        if (CL_NULL_P(new_vec_obj)) return;  /* OOM: drop this bytecode */
        /* Re-fetch old vec — cl_make_vector may have triggered GC compaction. */
        vec = (CL_Vector *)CL_OBJ_TO_PTR(*bc_vec_p);
        new_vec = (CL_Vector *)CL_OBJ_TO_PTR(new_vec_obj);
        for (i = 0; i < (uint32_t)*bc_count_p; i++)
            new_vec->data[i] = vec->data[i];
        *bc_vec_p = new_vec_obj;
        vec = new_vec;
    }
    vec->data[(*bc_count_p)++] = bytecode;
}

static int cf_process_toplevel_form(CL_Obj expr,
                                    CL_Obj lex_env,
                                    CL_Obj *bc_vec_p,
                                    int *bc_count_p)
{
    CL_Obj bytecode;
    CL_Obj head;
    int changed_pkg = 0;   /* OR of nested IN-PACKAGE results */

    /* LEX_ENV (CL_NIL or an alist from cl_build_lex_env) carries the local
     * macros/symbol-macros of any enclosing top-level MACROLET/SYMBOL-MACROLET
     * so the macroexpand below and any sub-compilation see them.  Protected for
     * the whole call since allocations happen throughout. */
    CL_GC_PROTECT(lex_env);     /* prot #1 — released at every return */
    CL_GC_PROTECT(expr);        /* prot #2 */

    /* CLHS 3.2.3.1: top-level macroexpand-1 loop.  User-level macros such as
     * DEFPACKAGE and DEFCLASS expand to (eval-when ...) wrappers that we then
     * dispatch below.  Special operators (PROGN, EVAL-WHEN, MACROLET, ...) are
     * not macros and return unchanged on the first iteration. */
    for (;;) {
        CL_Obj exp;
        if (!CL_CONS_P(expr)) { CL_GC_UNPROTECT(2); return 0; }
        exp = cl_macroexpand_1_env(expr, lex_env);
        if (exp == expr) break;
        expr = exp;
    }

    head = cl_car(expr);

    /* (QUOTE x) at top level has no effect and LOAD discards the value
     * (CLHS 3.2.3.1 leaves nothing to do for it) — emit no unit.  Every
     * definer macro that ends in `',name` (DEFCFUN, DEFCSTRUCT, DEFSTRUCT,
     * DEFCLASS ...) otherwise costs ~200 bytes of FASL per definition for
     * a unit whose only job is to push a constant; in the generated OS
     * binding modules that was a fifth of the file.  Non-cons atoms are
     * already skipped above for the same reason. */
    if (head == SYM_QUOTE) {
        CL_GC_UNPROTECT(2);     /* expr, lex_env */
        return 0;
    }

    /* PROGN: each subform is a fresh top-level form (CLHS 3.2.3.1). */
    if (head == SYM_PROGN) {
        CL_Obj subs = cl_cdr(expr);
        CL_GC_PROTECT(subs);
        while (!CL_NULL_P(subs)) {
            changed_pkg |= cf_process_toplevel_form(cl_car(subs), lex_env,
                                                    bc_vec_p, bc_count_p);
            subs = cl_cdr(subs);
        }
        CL_GC_UNPROTECT(1);     /* subs */
        CL_GC_UNPROTECT(2);     /* expr, lex_env */
        return changed_pkg;
    }

    /* LOCALLY: body forms (after any declarations) are fresh top-level forms
     * in the same lexical environment (CLHS 3.2.3.1). */
    if (head == SYM_LOCALLY) {
        CL_Obj subs = cl_cdr(expr);
        CL_GC_PROTECT(subs);
        while (!CL_NULL_P(subs)) {
            CL_Obj sub = cl_car(subs);
            if (!(CL_CONS_P(sub) && cl_car(sub) == SYM_DECLARE))
                changed_pkg |= cf_process_toplevel_form(sub, lex_env,
                                                        bc_vec_p, bc_count_p);
            subs = cl_cdr(subs);
        }
        CL_GC_UNPROTECT(1);     /* subs */
        CL_GC_UNPROTECT(2);     /* expr, lex_env */
        return changed_pkg;
    }

    /* MACROLET / SYMBOL-MACROLET: process the body as top-level forms with the
     * local macros/symbol-macros in scope (CLHS 3.2.3.1).  Without this, a
     * compile-time-effecting body form (e.g. DEFPACKAGE that expands to a local
     * macro call) would only run at load time, so later same-file forms could
     * not reference the package/macro it defines. */
    if (head == SYM_MACROLET || head == SYM_SYMBOL_MACROLET) {
        CL_Obj bindings = cl_car(cl_cdr(expr));
        CL_Obj body;
        CL_Obj new_env;
        if (head == SYM_MACROLET)
            new_env = cl_macrolet_lex_env(bindings, lex_env);
        else
            new_env = cl_symbol_macrolet_lex_env(bindings, lex_env);
        CL_GC_PROTECT(new_env);
        /* cl_*_lex_env compiles the expanders, which can trigger GC compaction;
         * `expr` is GC-protected and thus forwarded, but a bare `body` computed
         * before the call would be left holding a stale offset.  Recompute it
         * from the (forwarded) expr now. */
        body = cl_cdr(cl_cdr(expr));
        CL_GC_PROTECT(body);
        while (!CL_NULL_P(body)) {
            changed_pkg |= cf_process_toplevel_form(cl_car(body), new_env,
                                                    bc_vec_p, bc_count_p);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(2);     /* body, new_env */
        CL_GC_UNPROTECT(2);     /* expr, lex_env */
        return changed_pkg;
    }

    /* EVAL-WHEN: dispatch per CLHS Table 3-9. */
    if (head == SYM_EVAL_WHEN) {
        changed_pkg = cf_process_toplevel_eval_when(expr, lex_env,
                                                    bc_vec_p, bc_count_p);
        CL_GC_UNPROTECT(2);     /* expr, lex_env */
        return changed_pkg;
    }

    /* All other forms: compile to bytecode.
     * Per CLHS 3.2.3.1.2, compile-time side effects of defining forms
     * (proclaim-special for DEFVAR/DEFPARAMETER, macro installation for
     * DEFMACRO, etc.) are produced inside cl_compile itself.  We only need
     * to run the resulting bytecode at compile time for the small set of
     * forms whose effect requires the VM to execute (cf_form_needs_compile_time_eval). */
    cl_ltv_init_count = 0;
    bytecode = CL_CONS_P(lex_env) ? cl_compile_lex(expr, lex_env)
                                  : cl_compile(expr);

    if (CL_NULL_P(bytecode)) { cl_ltv_init_count = 0; CL_GC_UNPROTECT(2); return 0; }
    cl_ltv_init_count = 0;

    CL_GC_PROTECT(bytecode);
    if (cf_form_needs_compile_time_eval(expr))
        cl_vm_eval(bytecode);
    cl_gc_compact_if_pending();
    cf_bc_vec_append(bc_vec_p, bc_count_p, bytecode);
    CL_GC_UNPROTECT(1);         /* bytecode */
    CL_GC_UNPROTECT(2);         /* expr, lex_env */
    /* IN-PACKAGE (compile_in_package fast path or the deferred
     * %SET-CURRENT-PACKAGE runtime path) is the only top-level form that
     * legitimately changes the file reader's package; report it so the
     * caller keeps the new package instead of restoring. */
    return (head == SYM_IN_PACKAGE) ? 1 : 0;
}

/* --- compile-file ---
 *
 * (compile-file input-file &key output-file verbose)
 * Reads source, compiles+evals each top-level form, serializes bytecode to FASL.
 * Returns (values output-truename nil nil) per CL spec.
 */

static CL_Obj bi_compile_file(CL_Obj *args, int n)
{
    char in_path[1024];
    char out_path[1024];
    char *src_buf;
    unsigned long src_size;
    CL_Obj stream, expr;
    const char *prev_file;
    const char *own_file;
    uint16_t own_file_id;
    uint16_t prev_file_id;
    int prev_line;
    CL_Obj output_pathname;
    int verbose = 0;
    int i;
    int prev_compiling_to_file;
    /* CLHS 24.2.2: COMPILE-FILE binds *PACKAGE* and *READTABLE* (and
     * *COMPILE-FILE-PATHNAME* / *COMPILE-FILE-TRUENAME*) around the compile.
     * Dynamic bindings, not C-local save/restore, for the same reason as
     * bi_load (see the dyn_mark note there): a condition handled by the
     * caller's handler-case NLXes past this C frame, and only the dyn stack
     * is restored on that path.  The dyn stack is GC-marked, so the bound
     * values need no manual rooting. */
    int dyn_mark;
#ifdef DEBUG_COMPILER
    int cf_form_count = 0;
#endif

    /* Dynamic serialization buffers.  The file-level CL_FaslWriter `w`
     * is heap-allocated to keep this function's stack small (the writer
     * is ~4 KB due to gensym_objs[1024]).  Per-unit serialization is
     * delegated to cf_emit_fasl_unit which uses a stack-local writer. */
    uint8_t *fasl_buf = NULL;
    uint32_t fasl_capacity = 64 * 1024;  /* Start with 64KB */
    CL_FaslWriter *w = NULL;

    /* Per-unit scratch buffer — passed to cf_emit_fasl_unit which may
     * grow it via realloc-style swap. */
    uint8_t *unit_buf = NULL;
    uint32_t unit_capacity = 32 * 1024;  /* 32KB per unit */

    /* Collected bytecodes for deferred FASL serialization.  Stored in a
     * Lisp vector (not a host array) so the bytecodes are GC roots —
     * required because top-level forms accumulate across many GCs and
     * un-anchored bytecodes (no defun target) would otherwise be swept. */
    CL_Obj bc_vec = CL_NIL;
    int bc_collect_count = 0;

    uint32_t n_units = 0;
    int fasl_incomplete = 0;  /* set if any unit fails to serialize */

    extern const char *cl_coerce_to_namestring(CL_Obj arg, char *buf, uint32_t bufsz);
    extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);

    /* Sync *package* */
    {
        CL_Obj pkg_val2 = cl_symbol_value(SYM_STAR_PACKAGE);
        if (!CL_NULL_P(pkg_val2))
            cl_current_package = pkg_val2;
    }

    /* Establish the COMPILE-FILE bindings (see the dyn_mark note above). */
    dyn_mark = cl_dyn_top;
    cl_dynbind_c(SYM_STAR_PACKAGE, cl_current_package);
    cl_dynbind_c(SYM_STAR_READTABLE, cl_symbol_value(SYM_STAR_READTABLE));

    /* Resolve input path into in_path: parse → merge with
       *default-pathname-defaults* (CL spec) → coerce → expand ~. */
    cl_resolve_input_namestring(args[0], in_path, sizeof(in_path), "COMPILE-FILE");

    /* Default output: cache path, fallback to next-to-source */
    if (!make_fasl_cache_path(in_path, out_path, sizeof(out_path)))
        make_fasl_path(in_path, out_path, sizeof(out_path));
    /* Parse keyword args: :output-file, :verbose */
    for (i = 1; i + 1 < n; i += 2) {
        if (args[i] == cl_intern_keyword("OUTPUT-FILE", 11)) {
            if (CL_PATHNAME_P(args[i + 1])) {
                cl_coerce_to_namestring(args[i + 1], out_path, sizeof(out_path));
            } else if (CL_STRING_P(args[i + 1])) {
                CL_String *s = (CL_String *)CL_OBJ_TO_PTR(args[i + 1]);
                if (s->length < sizeof(out_path)) {
                    memcpy(out_path, s->data, s->length);
                    out_path[s->length] = '\0';
                }
            }
        } else if (args[i] == cl_intern_keyword("VERBOSE", 7)) {
            verbose = !CL_NULL_P(args[i + 1]);
        }
        /* Ignore unknown keywords (allow-other-keys behavior) */
    }

    /* Check *compile-verbose* if :verbose not explicitly given */
    if (!verbose) {
        if (!CL_NULL_P(cl_symbol_value(SYM_STAR_COMPILE_VERBOSE)))
            verbose = 1;
    }

    /* Read source file */
    src_buf = platform_file_read(in_path, &src_size);
    if (!src_buf) {
        cl_error(CL_ERR_GENERAL, "COMPILE-FILE: cannot open file \"%s\"", in_path);
        return CL_NIL;
    }

    if (verbose) {
        cl_write_cstring_to_stdout("; Compiling ");
        cl_write_cstring_to_stdout(in_path);
        cl_write_cstring_to_stdout("\n");
    }

    /* Bind *compile-file-pathname* and *compile-file-truename* — dynamic
     * binds (see dyn_mark note above); the dyn stack roots the values, so
     * the C protects drop once both binds are in. */
    {
        CL_Obj cfp_path = cl_parse_namestring(in_path, (uint32_t)strlen(in_path));
        CL_GC_PROTECT(cfp_path);
        CL_Obj cft_path;
        char resolved[512];
        if (platform_realpath(in_path, resolved, (int)sizeof(resolved)))
            cft_path = cl_parse_namestring(resolved, (uint32_t)strlen(resolved));
        else
            cft_path = cfp_path;
        CL_GC_PROTECT(cft_path);

        cl_dynbind_c(SYM_STAR_COMPILE_FILE_PATHNAME, cfp_path);
        cl_dynbind_c(SYM_STAR_COMPILE_FILE_TRUENAME, cft_path);
        CL_GC_UNPROTECT(2);  /* cft_path, cfp_path */
    }

    /* Collect the structure layouts this file's code gets compiled against
     * (builtins_struct.c); they become the FASL's DEPS trailer.  Popped
     * with the binds above by cl_dynbind_restore_to(dyn_mark). */
    cl_dynbind_c(cl_struct_deps_sym, CL_NIL);

    /* Save source file context.  in_path is a stack-local buffer; intern
     * it into the process-lifetime source-file pool so that bytecodes
     * compiled below can keep referencing the path string after this
     * function returns (CL_Bytecode.source_file is read by the FATAL
     * crash trace, debugger, and FASL serialize paths). */
    prev_file = cl_current_source_file;
    prev_file_id = cl_current_file_id;
    prev_line = cl_reader_get_line();
    cl_current_source_file = cl_intern_source_file(
        cl_fasl_portable_source_name(in_path));
    cl_current_file_id++;
    cl_reader_reset_line();
    own_file = cl_current_source_file;
    own_file_id = cl_current_file_id;

    /* Allocate FASL and unit buffers + FASL writers (heap-allocated to
     * keep bi_compile_file's stack frame small — CL_FaslWriter is ~4KB
     * due to gensym_objs[1024], and two on the stack would interact
     * badly with setjmp/longjmp error recovery) */
    fasl_buf = (uint8_t *)platform_alloc(fasl_capacity);
    unit_buf = (uint8_t *)platform_alloc(unit_capacity);
    w = (CL_FaslWriter *)platform_alloc(sizeof(CL_FaslWriter));
    if (!fasl_buf || !unit_buf || !w) {
        if (fasl_buf) platform_free(fasl_buf);
        if (unit_buf) platform_free(unit_buf);
        if (w) platform_free(w);
        platform_free(src_buf);
        cl_error(CL_ERR_GENERAL, "COMPILE-FILE: out of memory");
        return CL_NIL;
    }
    bc_vec = cl_make_vector(4096);
    if (CL_NULL_P(bc_vec)) {
        platform_free(fasl_buf);
        platform_free(unit_buf);
        platform_free(w);
        platform_free(src_buf);
        cl_error(CL_ERR_GENERAL, "COMPILE-FILE: out of memory (bytecode vector)");
        return CL_NIL;
    }
    CL_GC_PROTECT(bc_vec);

    /* Write header placeholder (n_units=0, patched later) */
    cl_fasl_writer_init(w, fasl_buf, fasl_capacity);
    cl_fasl_write_header(w, 0);


    /* Create source stream */
    stream = cl_make_cbuf_input_stream(src_buf, (uint32_t)src_size);
    if (CL_NULL_P(stream)) {
        /* Reading from NIL would compile nothing and write no file, quietly. */
        CL_GC_UNPROTECT(1); /* bc_vec */
        platform_free(fasl_buf);
        platform_free(unit_buf);
        platform_free(w);
        platform_free(src_buf);
        cl_error(CL_ERR_GENERAL,
                 "COMPILE-FILE: cannot open a source stream - out of memory, "
                 "or over 250 source files being read at once");
        return CL_NIL;
    }
    CL_GC_PROTECT(stream);

    /* Flag COMPILE-FILE active so load-time-value defers evaluation to load time */
    prev_compiling_to_file = cl_compiling_to_file;
    cl_compiling_to_file = 1;

    /* Read-compile-eval-serialize loop */
    for (;;) {
        int err;
        int saved_fp, saved_sp, saved_nlx;
        int saved_handler_top, saved_restart_top;

        /* Save VM state so we can restore on error (cl_error longjmps
           past cl_vm_eval's OP_HALT, leaving fp/sp/nlx leaked) */
        saved_fp = cl_vm.fp;
        saved_sp = cl_vm.sp;
        saved_nlx = cl_nlx_top;
        /* Hide outer Lisp handlers so compile-file errors don't escape
         * to handler-case frames installed by the caller (e.g. ASDF).
         * Use floor instead of zeroing top — GC must still mark the
         * full handler/restart stacks to avoid collecting referenced objects. */
        saved_handler_top = cl_handler_floor;
        saved_restart_top = cl_restart_floor;
        cl_handler_floor = cl_handler_top;
        cl_restart_floor = cl_restart_top;
        {
        int saved_gc_roots_cf = gc_root_count;

        CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            /* see bi_load's file loop: an abandoned nested LOAD (from an
             * EVAL-WHEN or a macro expander) leaves its file behind */
            cl_current_source_file = own_file;
            cl_current_file_id = own_file_id;
            expr = cl_read_from_stream(stream);
            if (cl_reader_eof()) {
                CL_UNCATCH();
                cl_handler_floor = saved_handler_top;
                cl_restart_floor = saved_restart_top;
                break;
            }

#ifdef DEBUG_COMPILER
            cf_form_count++;
            fprintf(stderr, "[compile-file %s] form %d read\n",
                    in_path, cf_form_count);
            fflush(stderr);
#endif
            CL_GC_PROTECT(expr);
            {
                /* Preserve the file reader's package across this form.  The
                 * reader interns symbols using cl_current_package, set by the
                 * file's IN-PACKAGE forms (compile_in_package).  Compiling or
                 * compile-time-evaluating a form can transiently rebind
                 * *PACKAGE* — e.g. ASDF's session/visiting machinery unwinds
                 * dynamic bindings during the operate that drives compile-file
                 * — and each *PACKAGE* dynbind/unbind syncs cl_current_package
                 * (cl_sync_current_package_from_dynamic).  Left unchecked, the
                 * reader switches to the wrong package mid-file, so a later
                 * form's symbols intern in COMMON-LISP-USER instead of the
                 * file's package; that yields a *different* symbol object than
                 * the file's earlier forms registered, so e.g. a macro defined
                 * earlier looks "undefined" and the call compiles as a bare
                 * function call.  Restore cl_current_package unless this form
                 * was an IN-PACKAGE (whose change must persist for the rest of
                 * the file).  See tests/test_compile_file_package.c. */
                CL_Obj reader_pkg = cl_current_package;
                int form_set_package;
                CL_GC_PROTECT(reader_pkg);
                form_set_package = cf_process_toplevel_form(
                    expr, CL_NIL, &bc_vec, &bc_collect_count);
                if (!form_set_package)
                    cl_current_package = reader_pkg;
                CL_GC_UNPROTECT(1); /* reader_pkg */
            }
            CL_GC_UNPROTECT(1);
            CL_UNCATCH();
        } else {
            /* Restore VM state leaked by aborted cl_vm_eval */
            cl_vm.fp = saved_fp;
            cl_vm.sp = saved_sp;
            cl_nlx_top = saved_nlx;
            gc_root_count = saved_gc_roots_cf;
            CL_UNCATCH();
            /* Propagate exit request — don't swallow (quit) */
            if (err == CL_ERR_EXIT) {
                cl_compiling_to_file = prev_compiling_to_file;
                CL_GC_UNPROTECT(2); /* stream, bc_vec */
                cl_stream_close(stream);
                platform_free(src_buf);
                cl_error(CL_ERR_EXIT, "");
            }
            cl_error_print();
        }
        }
        /* Restore outer Lisp handler/restart floor */
        cl_handler_floor = saved_handler_top;
        cl_restart_floor = saved_restart_top;
    }

    CL_GC_UNPROTECT(1); /* stream */
    cl_stream_close(stream);
    platform_free(src_buf);

    /* --- MAKE-LOAD-FORM pre-pass ---
     * Walk the collected units' constant graph and record load forms for
     * any literal object whose class has a user MAKE-LOAD-FORM method, so
     * the serializer below can emit FASL_TAG_LOAD_FORM for them.  This is
     * a no-op (skips the walk) when no such method exists.  It runs here,
     * before any byte serialization, because the user methods it calls can
     * allocate/compact and the serializer's offset-keyed tables cannot
     * tolerate that mid-serialization. */
    cl_fasl_mlf_prepass(bc_vec, bc_collect_count);

    /* --- Deferred FASL serialization --- */
    {
        int bci;
        CL_Vector *bc_vec_ptr = (CL_Vector *)CL_OBJ_TO_PTR(bc_vec);

        for (bci = 0; bci < bc_collect_count && !fasl_incomplete; bci++) {
            CL_Obj bc = bc_vec_ptr->data[bci];
            int unit_err = FASL_OK;

#ifdef DEBUG_COMPILER
            {
                CL_Bytecode *bcobj = (CL_Bytecode *)CL_OBJ_TO_PTR(bc);
                CL_String *bname = CL_NULL_P(bcobj->name) ? NULL :
                    (CL_String *)CL_OBJ_TO_PTR(((CL_Symbol *)CL_OBJ_TO_PTR(bcobj->name))->name);
                fprintf(stderr, "[compile-file] serializing unit %d/%d name=%s src-line=%u nconst=%u codelen=%u\n",
                        bci + 1, bc_collect_count,
                        bname ? bname->data : "<anon>",
                        (unsigned)bcobj->source_line,
                        (unsigned)bcobj->n_constants,
                        (unsigned)bcobj->code_len);
                fflush(stderr);
            }
#endif

            /* compile-file caps unit_capacity at CL_FASL_UNIT_CAP_BYTES;
             * source-load via bi_load passes UINT32_MAX (uncapped).
             * Either way, a unit failure invalidates the whole cache —
             * a partial FASL would silently drop top-level DEFUN side
             * effects and load-time behavior would diverge from source. */
            if (cf_emit_fasl_unit(w, &fasl_buf, &fasl_capacity,
                                  &unit_buf, &unit_capacity,
                                  CL_FASL_UNIT_CAP_BYTES, bc,
                                  &unit_err) == 0) {
                n_units++;
            } else {
                CL_Bytecode *bcobj = (CL_Bytecode *)CL_OBJ_TO_PTR(bc);
                CL_String *bname = CL_NULL_P(bcobj->name) ? NULL :
                    (CL_String *)CL_OBJ_TO_PTR(((CL_Symbol *)CL_OBJ_TO_PTR(bcobj->name))->name);
                const char *reason = (unit_err == FASL_ERR_TOO_DEEP)
                                       ? "graph too deep"
                                   : (unit_err == FASL_ERR_OVERFLOW)
                                       ? "buffer overflow > unit cap"
                                   : (unit_err == FASL_ERR_NONPORTABLE)
                                       ? cl_fasl_nonportable_detail(w)
                                       : "serialize failed";
                char msg[1024];
                snprintf(msg, sizeof(msg),
                         "; Warning: FASL unit failed in %s "
                         "(unit %d/%d, name=%s, src-line=%u, reason=%s, error=%d) - skipping FASL cache for this file\n",
                         in_path, bci + 1, bc_collect_count,
                         bname ? bname->data : "<anon>",
                         (unsigned)bcobj->source_line,
                         reason, unit_err);
                cl_write_cstring_to_stdout(msg);
                fasl_incomplete = 1;
                break;
            }
        }

    }

    /* Release the MAKE-LOAD-FORM pre-pass cache now that serialization is
     * done (also clears the GC roots it held).  The remaining steps
     * (file write) don't reference it. */
    cl_fasl_mlf_cleanup();

#ifdef DEBUG_COMPILER
    fprintf(stderr, "[compile-file %s] all forms done, n_units=%u, writing FASL\n",
            in_path, n_units);
    fflush(stderr);
#endif

    /* The DEPS trailer goes after the last unit.  Without room for it the
     * FASL would load unchecked, so it counts as incomplete. */
    if (n_units > 0 && !fasl_incomplete &&
        cf_emit_fasl_deps(w, &fasl_buf, &fasl_capacity,
                          cl_symbol_value(cl_struct_deps_sym)) != 0)
        fasl_incomplete = 1;

    /* Write FASL file to disk (skip if any units failed to serialize). */
    if (n_units > 0 && !fasl_incomplete) {
        /* Patch n_units in the header (bytes 8-11, big-endian) */
        fasl_buf[8]  = (uint8_t)(n_units >> 24);
        fasl_buf[9]  = (uint8_t)(n_units >> 16);
        fasl_buf[10] = (uint8_t)(n_units >> 8);
        fasl_buf[11] = (uint8_t)(n_units);

        /* Ensure output directory exists */
        {
            char dir[1024];
            path_directory(out_path, dir, sizeof(dir));
            if (dir[0]) mkdir_p(dir);
        }

        /* Write FASL file.  Delete-then-create so the output carries a
         * fresh datestamp even on FS-UAE directory drives, which keep a
         * replaced file's original .uaem stamp on in-place overwrite
         * (see the auto-cache write in bi_load). */
        {
            PlatformFile fh;
            platform_file_delete(out_path);
            fh = platform_file_open(out_path, PLATFORM_FILE_WRITE);
            if (fh == PLATFORM_FILE_INVALID) {
                cl_compiling_to_file = prev_compiling_to_file;
                platform_free(fasl_buf);
                platform_free(unit_buf);
                if (w) cl_fasl_writer_release(w);
                platform_free(w);
                CL_GC_UNPROTECT(1); /* bc_vec */
                cl_error(CL_ERR_GENERAL, "COMPILE-FILE: cannot create output file: %s", out_path);
                return CL_NIL;
            }
            platform_file_write_buf(fh, (const char *)fasl_buf, w->pos);
            platform_file_close(fh);
        }
    }

    platform_free(fasl_buf);
    platform_free(unit_buf);
    if (w) cl_fasl_writer_release(w);
    platform_free(w);
    CL_GC_UNPROTECT(1); /* bc_vec */
    cl_compiling_to_file = prev_compiling_to_file;

    /* Pop *compile-file-truename*, *compile-file-pathname*, *readtable*,
     * *package* — restoring the caller's values (and re-syncing
     * cl_current_package). */
    cl_dynbind_restore_to(dyn_mark);

    /* Restore source file context */
    cl_current_source_file = prev_file;
    cl_current_file_id = prev_file_id;
    cl_reader_set_line(prev_line);

    /* Return (values output-truename nil nil) per CL spec */
    if (fasl_incomplete) {
        /* FASL was not written — return NIL so caller falls back to source */
        output_pathname = CL_NIL;
    } else {
        output_pathname = cl_parse_namestring(out_path, (uint32_t)strlen(out_path));
    }
    /* Set multiple values: truename, warnings-p, failure-p */
    cl_mv_values[0] = output_pathname;
    cl_mv_values[1] = CL_NIL;  /* warnings-p */
    cl_mv_values[2] = CL_NIL;  /* failure-p */
    cl_mv_count = 3;
    return output_pathname;
}

/* --- compile-file-pathname ---
 *
 * (compile-file-pathname input-file &key output-file)
 * Returns the pathname of the FASL that compile-file would produce.
 */

static CL_Obj bi_compile_file_pathname(CL_Obj *args, int n)
{
    char in_path[1024], out_path[1024];
    int i;
    extern const char *cl_coerce_to_namestring(CL_Obj arg, char *buf, uint32_t bufsz);
    extern CL_Obj cl_parse_namestring(const char *str, uint32_t len);

    /* Resolve input path into in_path: parse → merge with
       *default-pathname-defaults* (CL spec) → coerce → expand ~. */
    cl_resolve_input_namestring(args[0], in_path, sizeof(in_path),
                                "COMPILE-FILE-PATHNAME");

    /* Default: cache path, fallback to next-to-source */
    if (!make_fasl_cache_path(in_path, out_path, sizeof(out_path)))
        make_fasl_path(in_path, out_path, sizeof(out_path));

    /* Check for :output-file keyword override */
    for (i = 1; i + 1 < n; i += 2) {
        if (args[i] == cl_intern_keyword("OUTPUT-FILE", 11)) {
            if (CL_PATHNAME_P(args[i + 1])) {
                cl_coerce_to_namestring(args[i + 1], out_path, sizeof(out_path));
            } else if (CL_STRING_P(args[i + 1])) {
                CL_String *s = (CL_String *)CL_OBJ_TO_PTR(args[i + 1]);
                if (s->length < sizeof(out_path)) {
                    memcpy(out_path, s->data, s->length);
                    out_path[s->length] = '\0';
                }
            }
            break;
        }
    }

    return cl_parse_namestring(out_path, (uint32_t)strlen(out_path));
}

/* --- Read --- */

static CL_Obj bi_read(CL_Obj *args, int n)
{
    CL_Obj stream;
    int eof_error_p;
    CL_Obj eof_value;
    CL_Obj result;

    /* Resolve stream argument (nil->*standard-input*, t->*terminal-io*) */
    if (n < 1 || CL_NULL_P(args[0])) {
        stream = cl_symbol_value(SYM_STANDARD_INPUT);
    } else if (args[0] == CL_T) {
        stream = cl_symbol_value(SYM_TERMINAL_IO);
    } else {
        stream = args[0];
    }

    eof_error_p = (n < 2 || !CL_NULL_P(args[1]));  /* default T */
    eof_value = (n >= 3) ? args[2] : CL_NIL;

    result = cl_read_from_stream(stream);
    if (cl_reader_eof()) {
        if (eof_error_p)
            cl_error(CL_ERR_EOF, "READ: end of file");
        return eof_value;
    }
    return result;
}

/* (read-delimited-list char &optional stream recursive-p) */
static CL_Obj bi_read_delimited_list(CL_Obj *args, int n)
{
    CL_Obj stream;
    int delim_char;
    int ch;
    CL_Obj result = CL_NIL, tail = CL_NIL, obj, cell;

    if (!CL_CHAR_P(args[0]))
        cl_error(CL_ERR_TYPE, "READ-DELIMITED-LIST: first argument must be a character");
    delim_char = CL_CHAR_VAL(args[0]);

    /* Resolve stream */
    if (n < 2 || CL_NULL_P(args[1])) {
        stream = cl_symbol_value(SYM_STANDARD_INPUT);
    } else if (args[1] == CL_T) {
        stream = cl_symbol_value(SYM_TERMINAL_IO);
    } else {
        stream = args[1];
    }

    /* GC SAFETY (audit tier 4, IO4): the nested reads and the cons loop
     * compact — `stream` is a C local (possibly from a *standard-input*
     * lookup, not a rooted args[] slot) and must stay forwarded across
     * iterations. */
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);
    CL_GC_PROTECT(stream);

    for (;;) {
        /* Skip whitespace */
        for (;;) {
            ch = cl_stream_read_char(stream);
            if (ch == -1)
                cl_error(CL_ERR_EOF, "READ-DELIMITED-LIST: unexpected end of file");
            if (ch == delim_char) {
                CL_GC_UNPROTECT(3);
                return result;
            }
            if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
                break;
        }
        /* Unread the non-whitespace char and read an object */
        cl_stream_unread_char(stream, ch);
        obj = cl_read_from_stream(stream);
        if (cl_reader_eof()) {
            CL_GC_UNPROTECT(3);
            cl_error(CL_ERR_EOF, "READ-DELIMITED-LIST: unexpected end of file");
            return CL_NIL;
        }
        /* Append to list */
        cell = cl_cons(obj, CL_NIL);
        if (CL_NULL_P(result)) {
            result = cell;
        } else {
            ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
        }
        tail = cell;
    }
}

/* --- Eval / Macroexpand --- */

static CL_Obj bi_eval(CL_Obj *args, int n)
{
    CL_Obj bytecode;
    CL_UNUSED(n);
    /* args[0] is a VM-stack slot — already GC-rooted and forwarded on
     * compaction.  Do NOT CL_GC_PROTECT it: registering an already-rooted
     * slot forwards it twice (gc_forward is not idempotent). */
    bytecode = cl_compile(args[0]);
    if (CL_NULL_P(bytecode)) return CL_NIL;
    return cl_vm_eval(bytecode);
}

/* Look up a symbol-macro binding in an ENV alist (as built by
 * cl_build_lex_env).  Returns 1 iff sym has a binding; writes the
 * expansion to *out.  Skips local-macro entries (whose car is the
 * SYM_LEX_LOCAL_MACRO sentinel). */
static int lex_env_lookup_symbol_macro(CL_Obj env, CL_Obj sym, CL_Obj *out)
{
    while (CL_CONS_P(env)) {
        CL_Obj pair = cl_car(env);
        if (CL_CONS_P(pair)) {
            CL_Obj k = cl_car(pair);
            if (k != SYM_LEX_LOCAL_MACRO && k == sym) {
                *out = cl_cdr(pair);
                return 1;
            }
        }
        env = cl_cdr(env);
    }
    return 0;
}

/* CLHS: macroexpand-1 form &optional env => expansion, expanded-p
 *
 * ENV is a Lisp alist of (SYMBOL . EXPANSION) pairs captured by the
 * compiler at a macro call site (see cl_build_lex_env).  A macro
 * expander receives ENV via its &environment parameter and can pass
 * it back here to look up symbol-macros visible at its call site. */
static CL_Obj bi_macroexpand_1(CL_Obj *args, int n)
{
    CL_Obj form = args[0];
    CL_Obj env = (n >= 2) ? args[1] : CL_NIL;
    CL_Obj expansion;
    if (CL_SYMBOL_P(form) && !CL_NULL_P(form)) {
        CL_Obj sm;
        /* Lexical env wins over global symbol-macro table. */
        if (!CL_NULL_P(env) && lex_env_lookup_symbol_macro(env, form, &sm)) {
            cl_mv_values[0] = sm;
            cl_mv_values[1] = SYM_T;
            cl_mv_count = 2;
            return sm;
        }
        if (cl_lookup_global_symbol_macro_p(form, &sm)) {
            cl_mv_values[0] = sm;
            cl_mv_values[1] = SYM_T;
            cl_mv_count = 2;
            return sm;
        }
    }
    if (!CL_CONS_P(form)) {
        cl_mv_values[0] = form;
        cl_mv_values[1] = CL_NIL;
        cl_mv_count = 2;
        return form;
    }
    expansion = cl_macroexpand_1_env(form, env);
    /* GC SAFETY (audit tier 4, IO9): the expander call compacts — compare
     * against the rooted (forwarded) args[0] slot, not the stale `form`
     * copy, or an unchanged form reports expanded-p = T. */
    cl_mv_values[0] = expansion;
    cl_mv_values[1] = (expansion == args[0]) ? CL_NIL : SYM_T;
    cl_mv_count = 2;
    return expansion;
}

/* CLHS: macroexpand form &optional env => expansion, expanded-p */
static CL_Obj bi_macroexpand(CL_Obj *args, int n)
{
    CL_Obj form = args[0];
    CL_Obj env = (n >= 2) ? args[1] : CL_NIL;
    int any_expansion = 0;
    /* Symbol input: apply symbol-macro once (chased below). */
    if (CL_SYMBOL_P(form) && !CL_NULL_P(form)) {
        CL_Obj sm;
        if (!CL_NULL_P(env) && lex_env_lookup_symbol_macro(env, form, &sm)) {
            any_expansion = 1;
            form = sm;
        } else if (cl_lookup_global_symbol_macro_p(form, &sm)) {
            any_expansion = 1;
            form = sm;
        }
    }
    if (!CL_CONS_P(form)) {
        cl_mv_values[0] = form;
        cl_mv_values[1] = any_expansion ? SYM_T : CL_NIL;
        cl_mv_count = 2;
        return form;
    }
    /* GC SAFETY (audit tier 4, IO9): every cl_macroexpand_1_env call can run
     * an expander (cl_vm_apply) and compact.  `form` must stay forwarded for
     * the unchanged-form EQ test, and `env` — a heap alist walked again on
     * the next iteration — must stay forwarded too.  expanded-p is tracked
     * as a C int so no stale SYM_T copy is ever written to mv_values. */
    CL_GC_PROTECT(form);
    CL_GC_PROTECT(env);
    for (;;) {
        CL_Obj expanded = cl_macroexpand_1_env(form, env);
        if (expanded == form) break;
        any_expansion = 1;
        form = expanded;
        if (!CL_CONS_P(form)) break;
    }
    CL_GC_UNPROTECT(2);
    cl_mv_values[0] = form;
    cl_mv_values[1] = any_expansion ? SYM_T : CL_NIL;
    cl_mv_count = 2;
    return form;
}

/* CLAMIGA::%MACROEXPAND-ENV — read the current lexical environment
 * installed by cl_macroexpand_1_env before calling an expander.
 * Called from the body of a DEFMACRO / MACROLET expander to bind the
 * user's &environment parameter. */
static CL_Obj bi_macroexpand_env(CL_Obj *args, int n)
{
    CL_UNUSED(args);
    CL_UNUSED(n);
    return cl_current_lex_env;
}

/* CLAMIGA::%CALL-MACRO-EXPANDER inner-fn whole-form env
 *
 * Internal trampoline produced by compile_defmacro / compile_macrolet.
 * Wraps an N-arg destructuring expander in the CLHS-mandated 2-arg
 * (form environment) shape so that (macro-function 'foo) returns a
 * function callable as (funcall (macro-function 'foo) form env).
 * Calls with any other arity must signal PROGRAM-ERROR — the wrapper
 * itself is a 2-required-arg function, so cl_vm_apply will signal
 * CL_ERR_ARGS (mapped to PROGRAM-ERROR) for 0/1/3+ arg calls.
 *
 * Threads env through cl_current_lex_env so that the inner expander's
 * &environment binding (compiled as (CLAMIGA::%MACROEXPAND-ENV)) sees
 * the right env.  Unpacks (cdr whole-form) into positional args for
 * the inner expander, with whole-form prepended (for &whole). */
static CL_Obj bi_call_macro_expander(CL_Obj *args, int n)
{
    CL_Obj inner = args[0];
    CL_Obj form  = args[1];
    CL_Obj env   = args[2];
    CL_Obj saved_env;
    CL_Obj arg_array[255];
    int nargs = 0;
    CL_Obj rest;
    CL_Obj result;

    CL_UNUSED(n);

    arg_array[nargs++] = form;
    rest = CL_CONS_P(form) ? cl_cdr(form) : CL_NIL;
    while (!CL_NULL_P(rest) && nargs < 255) {
        arg_array[nargs++] = cl_car(rest);
        rest = cl_cdr(rest);
    }

    CL_GC_PROTECT(inner);
    CL_GC_PROTECT(form);
    CL_GC_PROTECT(env);

    /* GC SAFETY: saved_env is written back into the thread's
     * current_lex_env slot AFTER the expander ran -- which allocates and can
     * compact.  The slot is a GC root (marked and forwarded), the C local is
     * not: for a nested expansion under a non-NIL lexical environment (a
     * global macro inside a MACROLET body) the restore put a stale offset
     * into a root, for the next collection to mark and forward. */
    saved_env = cl_current_lex_env;
    CL_GC_PROTECT(saved_env);
    cl_current_lex_env = env;
    if (CL_NULL_P(rest)) {
        result = cl_vm_apply(inner, arg_array, nargs);
    } else {
        /* More than 254 arguments after the head: the array path above
         * (and cl_vm_apply's one-byte OP_CALL stub) cannot carry them — it
         * used to drop the tail SILENTLY, so a DEFMACRO with &body over a
         * thousand-row form (a generated binding module is exactly that)
         * expanded with most of its rows missing.  Call through OP_APPLY,
         * which spreads up to CALL-ARGUMENTS-LIMIT: (apply inner
         * whole-form . (cdr whole-form)) — one cons. */
        CL_Obj arglist = cl_cons(form, cl_cdr(form));
        result = cl_vm_apply_list(inner, arglist);
    }
    cl_current_lex_env = saved_env;

    CL_GC_UNPROTECT(4);
    return result;
}

/* --- macro-function / (setf macro-function) --- */

static CL_Obj bi_macro_function(CL_Obj *args, int n)
{
    /* (macro-function symbol &optional env)
     * NIL is a symbol (with no macro binding), so it must be accepted and
     * return NIL — exactly like (macro-function t). CL_SYMBOL_P alone is
     * false for NIL because NIL is the tagged value 0, not a heap symbol. */
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_error(CL_ERR_TYPE, "MACRO-FUNCTION: not a symbol");
    return cl_get_macro(args[0]);
}

static CL_Obj bi_set_macro_function(CL_Obj *args, int n)
{
    /* (setf (macro-function symbol) function) — args: new-value, symbol */
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[1]))
        cl_error(CL_ERR_TYPE, "(SETF MACRO-FUNCTION): not a symbol");
    cl_register_macro(args[1], args[0]);
    return args[0];
}

/* --- compiler-macro-function / (setf compiler-macro-function) --- */

static CL_Obj bi_compiler_macro_function(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[0]))
        cl_error(CL_ERR_TYPE, "COMPILER-MACRO-FUNCTION: not a symbol");
    return cl_get_compiler_macro(args[0]);
}

static CL_Obj bi_set_compiler_macro_function(CL_Obj *args, int n)
{
    /* (setf (compiler-macro-function name) function) — args: new-value, name */
    CL_UNUSED(n);
    if (!CL_SYMBOL_OR_NIL_P(args[1]))
        cl_error(CL_ERR_TYPE, "(SETF COMPILER-MACRO-FUNCTION): not a symbol");
    cl_register_compiler_macro(args[1], args[0]);
    return args[0];
}

/* --- Throw --- */

#ifdef DEBUG_NLX
extern void cl_nlx_debug_dump(const char *where, unsigned tag);
#endif

static CL_Obj bi_throw(CL_Obj *args, int n)
{
    CL_Obj tag = args[0];
    CL_Obj value = (n > 1) ? args[1] : CL_NIL;
    int i;

#ifdef DEBUG_NLX
    cl_nlx_debug_dump("THROW enter", (unsigned)tag);
#endif
    /* Scan NLX stack for matching catch — down to cl_nlx_floor: a catch
     * established outside the foreign callback we may be running in is
     * not a target (thread.h). */
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
                    /* Set pending throw, longjmp to UWPROT.
                     * Use cl_pre_call_mv_count: call_builtin resets
                     * cl_mv_count=1 before invoking bi_throw, so cl_mv_count
                     * is always 1 here; the correct mv state from the value
                     * argument is preserved in cl_pre_call_mv_*. */
                    cl_pending_throw = 1;
                    cl_pending_tag = tag;
                    cl_pending_value = value;
                    cl_pending_mv_count = cl_pre_call_mv_count;
                    { int mi; for (mi = 0; mi < cl_pre_call_mv_count && mi < CL_MAX_MV; mi++)
                        cl_pending_mv_values[mi] = cl_pre_call_mv_values[mi]; }
                    cl_nlx_top = j;
                    cl_nlx_jump(&cl_nlx_stack[j]);
                }
            }
            /* No interposing UWPROT — go directly to catch.
             * Same reasoning: use cl_pre_call_mv_count, not cl_mv_count. */
            cl_nlx_stack[i].result = value;
            cl_nlx_stack[i].mv_count = cl_pre_call_mv_count;
            { int mi; for (mi = 0; mi < cl_pre_call_mv_count && mi < CL_MAX_MV; mi++)
                cl_nlx_stack[i].mv_values[mi] = cl_pre_call_mv_values[mi]; }
            cl_nlx_top = i;
            cl_nlx_jump(&cl_nlx_stack[i]);
        }
    }

#ifdef DEBUG_NLX
    cl_nlx_debug_dump("THROW no-catch", (unsigned)tag);
#endif
    if (CL_SYMBOL_P(tag))
        cl_error(CL_ERR_GENERAL, "No catch for tag %s%s", cl_symbol_name(tag),
                 cl_nlx_boundary_hint(tag, CL_NLX_CATCH));
    cl_error(CL_ERR_GENERAL, "No catch for tag%s",
             cl_nlx_boundary_hint(tag, CL_NLX_CATCH));
    return CL_NIL;
}

/* --- Multiple Values --- */

static CL_Obj bi_values(CL_Obj *args, int n)
{
    int i;
    int count = n < CL_MAX_MV ? n : CL_MAX_MV;
    for (i = 0; i < count; i++)
        cl_mv_values[i] = args[i];
    cl_mv_count = count;
    return n > 0 ? args[0] : CL_NIL;
}

static CL_Obj bi_values_list(CL_Obj *args, int n)
{
    CL_Obj list = args[0];
    int count = 0;
    CL_UNUSED(n);
    while (!CL_NULL_P(list) && count < CL_MAX_MV) {
        cl_mv_values[count++] = cl_car(list);
        list = cl_cdr(list);
    }
    cl_mv_count = count;
    return count > 0 ? cl_mv_values[0] : CL_NIL;
}

/* --- Gensym --- */

/* Compiler-internal counter — distinct from CL-visible *GENSYM-COUNTER*.
 * Code generation (setf expansion, destructuring, lambda rewriting)
 * makes many gensyms per compile; threading those through *GENSYM-COUNTER*
 * would surprise user code that binds it (e.g. (let ((*gensym-counter* 1))
 * (compile-file ...) *gensym-counter*) would no longer be 1). */
static volatile uint32_t internal_gensym_counter = 0;

/* C-callable GENSYM with a C-string prefix.  Used by compiler code
 * that needs fresh symbols when rewriting a form (e.g. SETF expansion
 * for GETF).  Uses a private counter, NOT *GENSYM-COUNTER*. */
CL_Obj cl_gensym_with_name(const char *prefix)
{
    char buf[64];
    CL_Obj name_str, sym;
    int len;
    uint32_t cnt;

    if (prefix == NULL) prefix = "G";
    cnt = platform_atomic_inc(&internal_gensym_counter) - 1;
    len = snprintf(buf, sizeof(buf), "%s%lu", prefix, (unsigned long)cnt);
    name_str = cl_make_string(buf, (uint32_t)len);
    CL_GC_PROTECT(name_str);
    sym = cl_make_symbol(name_str);  /* Uninterned — not in any package */
    CL_GC_UNPROTECT(1);
    return sym;
}

/* CLHS: GENSYM &optional x => new-symbol
 *   x ::= a string (prefix) or a non-negative integer (suffix override).
 *   With a string (or no arg), names are <prefix><*GENSYM-COUNTER*> and
 *   the counter is incremented after the symbol is allocated.
 *   With an integer, names are G<integer> and *GENSYM-COUNTER* is NOT
 *   touched.  *GENSYM-COUNTER* must be a non-negative integer; otherwise
 *   GENSYM signals TYPE-ERROR.  An x of any other type is a TYPE-ERROR. */
static CL_Obj bi_gensym(CL_Obj *args, int n)
{
    /* Buffer big enough for a 50-digit *GENSYM-COUNTER* (ansi gensym.10/.11)
     * plus a generous prefix.  Bignum suffix > 200 digits is exotic enough
     * to truncate safely. */
    char buf[256];
    CL_Obj name_str, sym;
    int prefix_len = 0;
    int suffix_len;
    int use_counter; /* 1 = read & bump *GENSYM-COUNTER*; 0 = integer arg */
    CL_Obj suffix_int = CL_NIL;

    if (n == 0) {
        buf[0] = 'G';
        prefix_len = 1;
        use_counter = 1;
    } else {
        CL_Obj arg = args[0];
        if (CL_STRING_P(arg)) {
            CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
            uint32_t slen = s->length;
            if (slen > sizeof(buf) - 64) slen = sizeof(buf) - 64;
            memcpy(buf, s->data, slen);
            prefix_len = (int)slen;
            use_counter = 1;
        } else if (CL_INTEGER_P(arg)) {
            buf[0] = 'G';
            prefix_len = 1;
            use_counter = 0;
            suffix_int = arg;
        } else {
            cl_signal_type_error(arg, "STRING", "GENSYM");
            return CL_NIL; /* unreachable */
        }
    }

    if (use_counter) {
        suffix_int = cl_symbol_value(SYM_STAR_GENSYM_COUNTER);
        if (!CL_INTEGER_P(suffix_int))
            cl_signal_type_error(suffix_int, "INTEGER", "GENSYM");
        if (cl_arith_minusp(suffix_int))
            cl_signal_type_error(suffix_int, "UNSIGNED-BYTE", "GENSYM");
    }

    suffix_len = cl_princ_to_string(suffix_int, buf + prefix_len,
                                    (int)sizeof(buf) - prefix_len);
    if (suffix_len < 0) suffix_len = 0;

    name_str = cl_make_string(buf, (uint32_t)(prefix_len + suffix_len));
    CL_GC_PROTECT(name_str);
    sym = cl_make_symbol(name_str);
    CL_GC_UNPROTECT(1);

    /* Increment *GENSYM-COUNTER* AFTER allocating the symbol (CLHS:
     * "the counter is incremented after the symbol has been created"). */
    if (use_counter) {
        CL_Obj cur = cl_symbol_value(SYM_STAR_GENSYM_COUNTER);
        CL_Obj next = cl_arith_add(cur, CL_MAKE_FIXNUM(1));
        cl_set_symbol_value(SYM_STAR_GENSYM_COUNTER, next);
    }

    return sym;
}

/* GENTEMP &optional prefix package => new-symbol
 * Like GENSYM but interns the result in PACKAGE (default *PACKAGE*).
 * Keeps bumping its counter until it finds a name not already present
 * in the package, so every call returns a fresh interned symbol. */
static volatile uint32_t gentemp_counter = 0;

static CL_Obj bi_gentemp(CL_Obj *args, int n)
{
    const char *prefix = "T";
    CL_Obj package;
    char buf[64];
    int plen;
    char char_pkg_name[2]; /* for character package designator */

    if (n > 0) {
        /* CLHS: prefix must be a STRING; anything else (including NIL)
         * is a TYPE-ERROR.  The ansi-test gentemp.error.1 walks
         * *mini-universe* and expects every non-string value to error. */
        if (!CL_STRING_P(args[0]))
            cl_signal_type_error(args[0], "STRING", "GENTEMP");
        {
            CL_String *s = (CL_String *)CL_OBJ_TO_PTR(args[0]);
            prefix = s->data;
        }
    }

    if (n > 1 && !CL_NULL_P(args[1])) {
        /* Package designator: package, string, symbol, or character. */
        CL_Obj arg = args[1];
        if (CL_HEAP_P(arg) && CL_HDR_TYPE(CL_OBJ_TO_PTR(arg)) == TYPE_PACKAGE) {
            package = arg;
        } else if (CL_STRING_P(arg)) {
            CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
            package = cl_find_package(s->data, s->length);
            if (CL_NULL_P(package))
                cl_error(CL_ERR_GENERAL, "GENTEMP: package not found: %s", s->data);
        } else if (CL_SYMBOL_P(arg)) {
            const char *name = cl_symbol_name(arg);
            package = cl_find_package(name, (uint32_t)strlen(name));
            if (CL_NULL_P(package))
                cl_error(CL_ERR_GENERAL, "GENTEMP: package not found: %s", name);
        } else if (CL_CHAR_P(arg)) {
            /* Character is a string-designator (CLHS glossary): treat as
             * a one-character package name. */
            char_pkg_name[0] = (char)CL_CHAR_VAL(arg);
            char_pkg_name[1] = '\0';
            package = cl_find_package(char_pkg_name, 1);
            if (CL_NULL_P(package))
                cl_error(CL_ERR_GENERAL, "GENTEMP: package not found: %c",
                         char_pkg_name[0]);
        } else {
            cl_signal_type_error(arg, "PACKAGE", "GENTEMP");
        }
    } else {
        package = cl_current_package;
    }

    for (;;) {
        uint32_t cnt = platform_atomic_inc(&gentemp_counter) - 1;
        plen = snprintf(buf, sizeof(buf), "%s%lu", prefix, (unsigned long)cnt);
        /* Skip names already present to match CLHS guarantee of a new symbol */
        if (CL_NULL_P(cl_package_find_symbol(buf, (uint32_t)plen, package)))
            return cl_intern_in(buf, (uint32_t)plen, package);
    }
}

/* --- Disassemble --- */

/* Opcode names and operand shapes come from the shared CL_OPCODE_LIST table
 * (opcodes.h / cl_opcode_info) — the same source the peephole decoder uses,
 * so the disassembler can never drift out of sync with the instruction set
 * again (its previous private table was missing ~10 newer opcodes, which
 * desynchronized all output after the first TAGBODY/PROGV/DEFSETF/DEFVAR/
 * STRUCT_REF instruction). */

/* Does this u16-operand opcode index the constant pool? (for annotation) */
static int disasm_u16_is_const(uint8_t op)
{
    switch (op) {
    case OP_CONST: case OP_GLOAD: case OP_GSTORE: case OP_FLOAD:
    case OP_FSTORE: case OP_DEFMACRO: case OP_DEFTYPE: case OP_DYNBIND:
    case OP_CLOSURE: case OP_HANDLER_PUSH: case OP_RESTART_PUSH:
    case OP_ASSERT_TYPE: case OP_BLOCK_PUSH: case OP_BLOCK_RETURN:
    case OP_TAGBODY_PUSH: case OP_TAGBODY_GO: case OP_DEFVAR:
    case OP_GLOAD_JNIL: case OP_GLOAD_EQ_JNIL:
        return 1;
    default:
        return 0;
    }
}

static void disasm_bytecode(CL_Obj bcobj)
{
    /* GC SAFETY (audit tier 4, IO10): the output stream may be a string or
     * Gray stream (Sly) — every cl_write_cstring_to_stdout / prin1 below can
     * allocate and compact, moving the CL_Bytecode object.  code/constants
     * are platform_alloc'd (stable addresses; the constants SLOTS are
     * forwarded in place by the compactor), so hoist them plus all scalar
     * fields ONCE before the first write, keep bcobj rooted, and never read
     * through a pre-write `bc` pointer again. */
    CL_Bytecode *bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bcobj);
    uint8_t *code = bc->code;
    CL_Obj *constants = bc->constants;
    uint32_t len = bc->code_len;
    uint16_t n_constants = bc->n_constants;
    uint16_t arity = bc->arity;
    uint8_t n_optional = bc->n_optional;
    uint8_t n_keys = bc->n_keys;
    uint8_t flags = bc->flags;
    uint16_t n_locals = bc->n_locals;
    uint16_t n_upvalues = bc->n_upvalues;
    uint32_t ip = 0;
    char line[256];
    char annot[128];

    CL_GC_PROTECT(bcobj);

    /* Header */
    cl_write_cstring_to_stdout("Disassembly of ");
    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bcobj);
    if (!CL_NULL_P(bc->name)) {
        cl_prin1_to_string(bc->name, annot, sizeof(annot));
        cl_write_cstring_to_stdout(annot);
    } else {
        cl_write_cstring_to_stdout("#<anonymous>");
    }
    cl_write_cstring_to_stdout(":\n");

    /* Metadata */
    {
        int req = arity & 0x7FFF;
        int has_rest = (arity >> 15) & 1;
        snprintf(line, sizeof(line), "  %d required, %d optional, %d key%s%s\n",
                req, (int)n_optional, (int)n_keys,
                has_rest ? ", &rest" : "",
                (flags & 2) ? ", &allow-other-keys" : "");
        cl_write_cstring_to_stdout(line);
        snprintf(line, sizeof(line), "  %lu locals, %lu upvalues\n",
                (unsigned long)n_locals, (unsigned long)n_upvalues);
        cl_write_cstring_to_stdout(line);
        snprintf(line, sizeof(line), "  %lu bytes, %lu constants\n\n",
                (unsigned long)len, (unsigned long)n_constants);
        cl_write_cstring_to_stdout(line);
    }

    /* Instructions */
    while (ip < len) {
        uint32_t start_ip = ip;
        uint8_t op = code[ip++];
        const CL_OpcodeInfo *info = cl_opcode_info(op);

        if (!info) {
            snprintf(line, sizeof(line), "  %04lu: ???          0x%02X\n",
                    (unsigned long)start_ip, (unsigned int)op);
            cl_write_cstring_to_stdout(line);
            continue;
        }

        switch ((CL_OperandKind)info->operands) {
        case CL_OPND_NONE:
            snprintf(line, sizeof(line), "  %04lu: %s\n",
                    (unsigned long)start_ip, info->name);
            cl_write_cstring_to_stdout(line);
            break;

        case CL_OPND_U8: {
            uint8_t val = code[ip++];
            snprintf(line, sizeof(line), "  %04lu: %-12s %u\n",
                    (unsigned long)start_ip, info->name, (unsigned int)val);
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_U16:
        case CL_OPND_U16_JREL:
        case CL_OPND_CLOSURE: {
            uint16_t val = (uint16_t)((code[ip] << 8) | code[ip + 1]);
            ip += 2;
            annot[0] = '\0';
            if (val < n_constants && disasm_u16_is_const(op)) {
                cl_prin1_to_string(constants[val], annot, sizeof(annot));
            }
            if (annot[0]) {
                snprintf(line, sizeof(line), "  %04lu: %-12s %-4u ; %s\n",
                        (unsigned long)start_ip, info->name,
                        (unsigned int)val, annot);
            } else {
                snprintf(line, sizeof(line), "  %04lu: %-12s %u\n",
                        (unsigned long)start_ip, info->name, (unsigned int)val);
            }
            cl_write_cstring_to_stdout(line);

            /* BLOCK_PUSH/TAGBODY_PUSH: i32 landing-pad offset after the u16 */
            if (info->operands == CL_OPND_U16_JREL) {
                int32_t boff = (int32_t)(((uint32_t)code[ip] << 24) |
                                         ((uint32_t)code[ip + 1] << 16) |
                                         ((uint32_t)code[ip + 2] << 8) |
                                         (uint32_t)code[ip + 3]);
                ip += 4;
                snprintf(line, sizeof(line),
                        "          offset %+d -> %04lu\n",
                        (int)boff, (unsigned long)((int32_t)ip + boff));
                cl_write_cstring_to_stdout(line);
            }

            /* OP_CLOSURE: read and print capture descriptors */
            if (info->operands == CL_OPND_CLOSURE && val < n_constants) {
                CL_Obj tmpl = constants[val];
                if (CL_BYTECODE_P(tmpl)) {
                    /* hoist n_upvalues: tmpl_bc goes stale after the first
                     * allocating write below */
                    int tmpl_nup =
                        ((CL_Bytecode *)CL_OBJ_TO_PTR(tmpl))->n_upvalues;
                    int ci;
                    for (ci = 0; ci < tmpl_nup; ci++) {
                        uint8_t is_local = code[ip++];
                        uint8_t cap_idx = code[ip++];
                        snprintf(line, sizeof(line),
                                "          capture %d: %s slot %u\n",
                                ci, is_local ? "local" : "upval",
                                (unsigned int)cap_idx);
                        cl_write_cstring_to_stdout(line);
                    }
                }
            }
            break;
        }

        case CL_OPND_U16_U16: {
            uint16_t a = (uint16_t)((code[ip] << 8) | code[ip + 1]);
            uint16_t b = (uint16_t)((code[ip + 2] << 8) | code[ip + 3]);
            ip += 4;
            snprintf(line, sizeof(line), "  %04lu: %-12s %u %u\n",
                    (unsigned long)start_ip, info->name,
                    (unsigned int)a, (unsigned int)b);
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_U16_U8: {
            /* CALL_GLOBAL / TAILCALL_GLOBAL: symbol const index + nargs */
            uint16_t a = (uint16_t)((code[ip] << 8) | code[ip + 1]);
            uint8_t b = code[ip + 2];
            ip += 3;
            annot[0] = '\0';
            if (a < n_constants)
                cl_prin1_to_string(constants[a], annot, sizeof(annot));
            snprintf(line, sizeof(line), "  %04lu: %-12s %-4u %u ; %s\n",
                    (unsigned long)start_ip, info->name,
                    (unsigned int)a, (unsigned int)b, annot);
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_U8_U8: {
            /* LOAD_LOAD a b / LOAD_STRUCT_REF slot idx */
            uint8_t a = code[ip], b = code[ip + 1];
            ip += 2;
            snprintf(line, sizeof(line), "  %04lu: %-12s %u %u\n",
                    (unsigned long)start_ip, info->name,
                    (unsigned int)a, (unsigned int)b);
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_U8_U16_U8: {
            /* LOAD_CALL_GLOBAL: slot, symbol const index, nargs */
            uint8_t s = code[ip];
            uint16_t a = (uint16_t)((code[ip + 1] << 8) | code[ip + 2]);
            uint8_t b = code[ip + 3];
            ip += 4;
            annot[0] = '\0';
            if (a < n_constants)
                cl_prin1_to_string(constants[a], annot, sizeof(annot));
            snprintf(line, sizeof(line), "  %04lu: %-12s %u %-4u %u ; %s\n",
                    (unsigned long)start_ip, info->name, (unsigned int)s,
                    (unsigned int)a, (unsigned int)b, annot);
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_U8_U16: {
            /* LOAD_CONST: slot, then the constant */
            uint8_t s = code[ip];
            uint16_t a = (uint16_t)((code[ip + 1] << 8) | code[ip + 2]);
            ip += 3;
            annot[0] = '\0';
            if (a < n_constants)
                cl_prin1_to_string(constants[a], annot, sizeof(annot));
            snprintf(line, sizeof(line), "  %04lu: %-12s %u %-4u ; %s\n",
                    (unsigned long)start_ip, info->name, (unsigned int)s,
                    (unsigned int)a, annot);
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_U16_U16_U8: {
            /* GLOAD_CALL_GLOBAL: the special, the callee, nargs */
            uint16_t a = (uint16_t)((code[ip] << 8) | code[ip + 1]);
            uint16_t b = (uint16_t)((code[ip + 2] << 8) | code[ip + 3]);
            uint8_t n = code[ip + 4];
            char annot2[128];
            ip += 5;
            annot[0] = '\0';
            annot2[0] = '\0';
            if (a < n_constants)
                cl_prin1_to_string(constants[a], annot, sizeof(annot));
            if (b < n_constants)
                cl_prin1_to_string(constants[b], annot2, sizeof(annot2));
            snprintf(line, sizeof(line), "  %04lu: %-12s %-4u %-4u %u ; %s %s\n",
                    (unsigned long)start_ip, info->name,
                    (unsigned int)a, (unsigned int)b, (unsigned int)n,
                    annot, annot2);
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_U8_JREL: {
            /* LOAD_JNIL: slot, then the i32 branch offset */
            uint8_t s = code[ip++];
            int32_t val = (int32_t)(((uint32_t)code[ip] << 24) |
                                    ((uint32_t)code[ip + 1] << 16) |
                                    ((uint32_t)code[ip + 2] << 8) |
                                    (uint32_t)code[ip + 3]);
            ip += 4;
            snprintf(line, sizeof(line), "  %04lu: %-12s %u %+d    ; -> %04lu\n",
                    (unsigned long)start_ip, info->name, (unsigned int)s,
                    (int)val, (unsigned long)((int32_t)ip + val));
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_JREL: {
            int32_t val = (int32_t)(((uint32_t)code[ip] << 24) |
                                    ((uint32_t)code[ip + 1] << 16) |
                                    ((uint32_t)code[ip + 2] << 8) |
                                    (uint32_t)code[ip + 3]);
            ip += 4;
            snprintf(line, sizeof(line), "  %04lu: %-12s %+d    ; -> %04lu\n",
                    (unsigned long)start_ip, info->name, (int)val,
                    (unsigned long)((int32_t)ip + val));
            cl_write_cstring_to_stdout(line);
            break;
        }

        case CL_OPND_AMIGA: {
            /* u16 base-sym-idx, i16 offset, u32 regspec, u8 nargs */
            uint16_t sidx = (uint16_t)((code[ip] << 8) | code[ip + 1]);
            int16_t loff = (int16_t)((code[ip + 2] << 8) | code[ip + 3]);
            uint32_t regspec = ((uint32_t)code[ip + 4] << 24) |
                               ((uint32_t)code[ip + 5] << 16) |
                               ((uint32_t)code[ip + 6] << 8) |
                               (uint32_t)code[ip + 7];
            uint8_t nargs = code[ip + 8];
            ip += 9;
            snprintf(line, sizeof(line),
                    "  %04lu: %-12s sym %u off %d regspec 0x%08lx nargs %u\n",
                    (unsigned long)start_ip, info->name, (unsigned int)sidx,
                    (int)loff, (unsigned long)regspec, (unsigned int)nargs);
            cl_write_cstring_to_stdout(line);
            break;
        }
        }
    }

    /* Constants pool — constants[ci] is re-read per iteration from the
     * stable platform array; the compactor forwards those slots in place. */
    if (n_constants > 0) {
        uint16_t ci;
        cl_write_cstring_to_stdout("\nConstants:\n");
        for (ci = 0; ci < n_constants; ci++) {
            cl_prin1_to_string(constants[ci], annot, sizeof(annot));
            snprintf(line, sizeof(line), "  %u: %s\n",
                    (unsigned int)ci, annot);
            cl_write_cstring_to_stdout(line);
        }
    }
    cl_write_cstring_to_stdout("\n");
    CL_GC_UNPROTECT(1); /* bcobj */
}

static CL_Obj bi_disassemble(CL_Obj *args, int n)
{
    CL_Obj arg = args[0];
    CL_Obj bcobj = CL_NIL;
    CL_UNUSED(n);

    /* Accept symbol — resolve to function binding (same fallback as OP_FLOAD) */
    if (CL_SYMBOL_P(arg)) {
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(arg);
        if (sym->function != CL_UNBOUND && !CL_NULL_P(sym->function))
            arg = sym->function;
        else {
            CL_Obj sv = cl_symbol_value(arg);
            if (sv != CL_UNBOUND && !CL_NULL_P(sv))
                arg = sv;
            else
                cl_error_cell(CL_ERR_UNDEFINED, arg, "DISASSEMBLE: no function binding");
        }
    }

    if (CL_BYTECODE_P(arg)) {
        bcobj = arg;
    } else if (CL_CLOSURE_P(arg)) {
        CL_Closure *cl = (CL_Closure *)CL_OBJ_TO_PTR(arg);
        if (CL_BYTECODE_P(cl->bytecode))
            bcobj = cl->bytecode;
    } else if (CL_FUNCTION_P(arg)) {
        /* GC SAFETY (audit tier 4, IO10): the "Built-in function: " write can
         * allocate (string/Gray stream) and move the function object — root
         * the name before the write instead of reading fn->name after it. */
        CL_Obj fname = ((CL_Function *)CL_OBJ_TO_PTR(arg))->name;
        char nbuf[128];
        CL_GC_PROTECT(fname);
        cl_write_cstring_to_stdout("Built-in function: ");
        cl_prin1_to_string(fname, nbuf, sizeof(nbuf));
        cl_write_cstring_to_stdout(nbuf);
        cl_write_cstring_to_stdout("\n  (no bytecode to disassemble)\n");
        CL_GC_UNPROTECT(1);
        return CL_NIL;
    } else if (CL_FFI_STUB_P(arg)) {
        /* An FFI binding descriptor has no bytecode either: print the
         * descriptor itself (#<FFI-STUB NAME LVO n (k args)> / PEEK ...)
         * — the printer shows every field. */
        char nbuf[160];
        CL_GC_PROTECT(arg);
        cl_write_cstring_to_stdout("FFI stub: ");
        cl_prin1_to_string(arg, nbuf, sizeof(nbuf));
        cl_write_cstring_to_stdout(nbuf);
        cl_write_cstring_to_stdout("\n  (a binding descriptor, no bytecode to "
                                   "disassemble; see FFI::%FFI-STUB-INFO)\n");
        CL_GC_UNPROTECT(1);
        return CL_NIL;
    }

    if (CL_NULL_P(bcobj))
        cl_error(CL_ERR_TYPE, "DISASSEMBLE: argument must be a function or symbol");

    disasm_bytecode(bcobj);
    return CL_NIL;
}

/* --- Timing --- */

/* The TIME start-value helpers mask with CL_FIXNUM_MAX (30 bits): anything
 * wider no longer fits the tagged-fixnum encoding and reads back negative.
 * %TIME-REPORT's wraparound arithmetic uses the same modulus. */
static CL_Obj bi_get_internal_time(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)(platform_time_ms() & CL_FIXNUM_MAX));
}

static CL_Obj bi_get_run_time(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)(platform_run_time_ms() & CL_FIXNUM_MAX));
}

/* (%BOOT-QUIET-P) -> T unless --boot-log was given.  Used by
 * lib/clos.lisp's %clos-trace so the "; [clos] ..." progress lines only
 * appear when boot logging was requested. */
static CL_Obj bi_boot_quiet_p(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return cl_quiet_boot ? SYM_T : CL_NIL;
}

/* (%BOOT-TRACE-CLOS elapsed-ms delta-ms phase-string)
 * Print "; [clos] <elapsed> ms (+<delta>): <phase>" via cl_write_cstring_to_stdout.
 * Same path as the C-side BOOT_TIME() macro; the helper falls back to
 * platform_write_string when *standard-output* isn't a stream yet, so this
 * is safe even when cl_stream_init has been skipped (e.g. some unit tests).
 * No-op unless --boot-log is in effect. */
static CL_Obj bi_boot_trace_clos(CL_Obj *args, int n)
{
    if (n != 3 || cl_quiet_boot) return CL_NIL;
    if (!CL_FIXNUM_P(args[0]) || !CL_FIXNUM_P(args[1])) return CL_NIL;
    if (!CL_STRING_P(args[2])) return CL_NIL;
    {
        int32_t elapsed = (int32_t)CL_FIXNUM_VAL(args[0]);
        int32_t delta   = (int32_t)CL_FIXNUM_VAL(args[1]);
        uint32_t plen   = cl_string_length(args[2]);
        const char *pdata = cl_string_base_data(args[2]);
        char phase_buf[80];
        char out[160];
        if (plen >= sizeof(phase_buf)) plen = sizeof(phase_buf) - 1;
        memcpy(phase_buf, pdata, plen);
        phase_buf[plen] = '\0';
        snprintf(out, sizeof(out), "; [clos] %5d ms (+%5d): %s\n",
                 (int)elapsed, (int)delta, phase_buf);
        cl_write_cstring_to_stdout(out);
    }
    return CL_NIL;
}

static CL_Obj bi_get_bytes_consed(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)(cl_heap.total_consed & CL_FIXNUM_MAX));
}

static CL_Obj bi_get_gc_count(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)(cl_heap.gc_count & CL_FIXNUM_MAX));
}

/* ext:gc — trigger garbage collection + pending compaction */
static CL_Obj bi_ext_gc(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_gc();
    cl_gc_compact_if_pending();
    return CL_NIL;
}

/* (ext:gc-compact) — force a full moving (compacting) GC now.
 * Unlike (ext:gc), which is a mark-sweep that only compacts when the allocator
 * has flagged fragmentation, this always relocates live objects.  Useful for
 * reclaiming a contiguous run of the arena, and for deterministically
 * exercising the moving collector (e.g. multi-thread GC stress tests that must
 * relocate a peer thread's roots).  Returns NIL. */
static CL_Obj bi_ext_gc_compact(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_gc_compact();
    return CL_NIL;
}

/* (ext:%gc-mark-stats) — GC mark-stack diagnostics as a 3-element list:
 * (capacity-entries grow-count rescan-pass-count).  A nonzero rescan-pass
 * count means the mark stack could not grow (cap or OOM) and the GC fell
 * back to the quadratic full-arena overflow re-scan — the pathology that
 * turns GC cycles on large live heaps from milliseconds into seconds. */
static CL_Obj bi_ext_gc_mark_stats(CL_Obj *args, int n)
{
    uint32_t cap, grows, rescans;
    CL_UNUSED(args); CL_UNUSED(n);
    cl_gc_mark_stack_stats(&cap, &grows, &rescans);
    return cl_cons(CL_MAKE_FIXNUM((int32_t)cap),
                   cl_cons(CL_MAKE_FIXNUM((int32_t)grows),
                           cl_cons(CL_MAKE_FIXNUM((int32_t)rescans),
                                   CL_NIL)));
}

/* (ext:%gc-fwd-bytes) — the off-heap bytes a compaction would allocate
 * right now for its forwarding structures (live-granule bitmap + per-page
 * slide cursor + pin gaps): about 1/32 + 1/64 of the used heap span on
 * 32-bit.  Transient — taken after the mark phase, released before the
 * compaction returns.  A compaction that cannot get this block sweeps
 * instead and says so once; the suite pins the bound so the block stays
 * small enough for an 8 MB Amiga (tests/amiga/run-tests.lisp,
 * tests/test_gc_fwd_bitmap.c). */
static CL_Obj bi_ext_gc_fwd_bytes(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)cl_gc_fwd_bytes());
}

/* (ext:%fasl-registry-stats) — the calling thread's active FASL readers and
 * writers as a 2-element list: (readers writers).  Both are GC roots that
 * point at C-side state (a reader is a stack local of fasl_load), so both
 * must be 0 whenever no LOAD / COMPILE-FILE is in flight on this thread.  A
 * nonzero reader count at top level means a non-local exit out of a load left
 * the registry pointing into dead stack — the next GC would crash on it. */
static CL_Obj bi_ext_fasl_registry_stats(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return cl_list2(CL_MAKE_FIXNUM((int32_t)cl_fasl_reader_save_count()),
                    CL_MAKE_FIXNUM((int32_t)cl_fasl_writer_save_count()));
}

/* (ext:%bytecode-offheap-stats) — off-heap bytecode payload returned to the
 * allocator since heap init, as a 2-element list: (bytes objects).
 *
 * A compiled function is a small arena object with up to eight
 * platform_alloc'd buffers hanging off it (bytecode body, constants pool,
 * &key arrays, pc→line map, JIT native code and its reloc table).  Those
 * live outside the GC arena, so nothing in ROOM or the arena statistics says
 * whether they are being handed back.  Redefining a function, LOADing a file
 * twice or any COMPILE churn orphans a bytecode; this counter must grow
 * after the next GC.  If it stays flat while functions are being replaced,
 * the runtime is leaking compiled-code memory — which on AmigaOS is Fast RAM
 * lost to the system until reboot. */
static CL_Obj bi_ext_bytecode_offheap_stats(CL_Obj *args, int n)
{
    uint32_t bytes, count;
    CL_UNUSED(args); CL_UNUSED(n);
    cl_gc_offheap_stats(&bytes, &count);
    return cl_cons(CL_MAKE_FIXNUM((int32_t)bytes),
                   cl_cons(CL_MAKE_FIXNUM((int32_t)count), CL_NIL));
}

/* (ext:%compiler-pool-stats) — the compiler's working memory, also outside
 * the arena, as a 6-element list: (parked-blocks block-bytes buffer-bytes
 * largest-parked-buffer largest-code-bytes largest-constants).  The pool
 * keeps its blocks for the whole run (compiler.c says why), so parked-blocks
 * x block-bytes + buffer-bytes is what compiling costs a process between
 * compiles; largest-parked-buffer stays at or under the pool's keep size
 * (16 KB) however large a function was compiled; the last two say how large
 * the biggest compile so far had to grow its buffers. */
static CL_Obj bi_ext_compiler_pool_stats(CL_Obj *args, int n)
{
    CL_CompilerPoolStats st;
    CL_Obj result = CL_NIL;
    CL_UNUSED(args); CL_UNUSED(n);
    cl_compiler_pool_stats(&st);
    CL_GC_PROTECT(result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)st.const_max), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)st.code_max), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)st.largest_parked), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)st.buffer_bytes), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)st.block_bytes), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)st.parked), result);
    CL_GC_UNPROTECT(1);
    return result;
}

/* (ext:%gc-time-stats) — cumulative GC phase timers as a 9-element list:
 * (gc-count compact-count stw-seconds mark-seconds sweep-seconds
 *  compact-seconds stw-stops stw-max-seconds epoch-skips).  Counts are
 * fixnums; the phase timers are double-floats of seconds since heap init.
 * stw = waiting for other threads to park (stw-stops events; stw-max is the
 * worst single stop); mark/sweep = sweep-GC phases; compact = whole
 * compaction cycles (their internal mark/sweep counts toward compact only);
 * epoch-skips = allocation-triggered collections deduped as redundant
 * because a peer thread collected first.  Together they show how much wall
 * time the runtime spends collecting — and in which phase — which is the
 * primary telemetry for allocation-heavy workload tuning. */
static CL_Obj bi_ext_gc_time_stats(CL_Obj *args, int n)
{
    uint64_t stw, mark, sweep, compact, stw_max;
    uint32_t stops, skips;
    CL_Obj result = CL_NIL, f;
    CL_UNUSED(args); CL_UNUSED(n);
    cl_gc_time_stats(&stw, &mark, &sweep, &compact);
    cl_gc_stw_stats(&stops, &stw_max, &skips);
    CL_GC_PROTECT(result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(skips & CL_FIXNUM_MAX)), result);
    f = cl_make_double_float((double)stw_max / 1e6);
    result = cl_cons(f, result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(stops & CL_FIXNUM_MAX)), result);
    f = cl_make_double_float((double)compact / 1e6);
    result = cl_cons(f, result);
    f = cl_make_double_float((double)sweep / 1e6);
    result = cl_cons(f, result);
    f = cl_make_double_float((double)mark / 1e6);
    result = cl_cons(f, result);
    f = cl_make_double_float((double)stw / 1e6);
    result = cl_cons(f, result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(cl_heap.compact_count & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(cl_heap.gc_count & CL_FIXNUM_MAX)), result);
    CL_GC_UNPROTECT(1);
    return result;
}

/* (ext:%heap-stats) — allocator-side counters as a 9-element list:
 * (arena-size bump total-allocated total-consed freelist-steps tlab-refills
 *  free-blocks free-bytes largest-free).  freelist-steps is the monotonic
 * count of free-list blocks the allocator has walked (every probe step is
 * one): together with the free-list shape it says how much of the time
 * between collections goes into the classic collector's first-fit search —
 * the allocation-side cost the GC phase timers do not see. */
static CL_Obj bi_ext_heap_stats(CL_Obj *args, int n)
{
    uint32_t blocks, bytes, largest;
    CL_Obj result = CL_NIL;
    CL_UNUSED(args); CL_UNUSED(n);
    cl_heap_free_list_stats(&blocks, &bytes, &largest);
    CL_GC_PROTECT(result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(largest & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(bytes & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(blocks & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(cl_heap.tlab_refills & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(cl_heap.freelist_steps & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(cl_heap.total_consed & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(cl_heap.total_allocated & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(cl_heap.bump & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(cl_heap.arena_size & CL_FIXNUM_MAX)), result);
    CL_GC_UNPROTECT(1);
    return result;
}

#ifdef CL_GENGC
/* (ext:%gengc-stats) — generational collector diagnostics as a 6-element
 * list: (enabled-p minor-count minor-seconds promoted-bytes old-top
 * dirty-pages-last-minor).  enabled-p is T/NIL (NIL = classic collector:
 * CLAMIGA_GENGC=0, setup failure, or a fallback demotion); the rest are
 * fixnums except minor-seconds (double-float).  promoted-bytes is
 * cumulative; old-top is the current old-space watermark in bytes. */
static CL_Obj bi_ext_gengc_stats(CL_Obj *args, int n)
{
    uint32_t minors, old_top, dirty_last;
    uint64_t minor_us, promoted;
    CL_Obj result = CL_NIL, f;
    CL_UNUSED(args); CL_UNUSED(n);
    cl_gengc_stats(&minors, &minor_us, &promoted, &old_top, &dirty_last);
    CL_GC_PROTECT(result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(dirty_last & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(old_top & CL_FIXNUM_MAX)), result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(promoted & CL_FIXNUM_MAX)), result);
    f = cl_make_double_float((double)minor_us / 1e6);
    result = cl_cons(f, result);
    result = cl_cons(CL_MAKE_FIXNUM((int32_t)(minors & CL_FIXNUM_MAX)), result);
    result = cl_cons(cl_gengc_enabled() ? CL_T : CL_NIL, result);
    CL_GC_UNPROTECT(1);
    return result;
}
/* (ext:%gc-minor) — trigger a minor (nursery) collection now.  Returns T
 * if a minor cycle ran, NIL if it was refused (gen mode disabled or the
 * cycle escalated — callers wanting a guaranteed collection use ext:gc).
 * Primarily a test/diagnostic hook: real minors are allocation-triggered. */
static CL_Obj bi_ext_gc_minor(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return cl_gc_minor(cl_heap.gc_count) ? CL_T : CL_NIL;
}
#endif /* CL_GENGC */

/* (ext:%stream-outbuf-stats) — string-output-stream buffer table diagnostics
 * as a 2-element list: (used-slots total-slots).  used-slots is the number of
 * allocated platform buffers (live streams + dead-but-uncollected ones);
 * total-slots grows on demand in 256-slot blocks.  A used count that keeps
 * climbing across GCs indicates streams that are written to but never CLOSEd
 * (their buffers are only reclaimed when the GC proves the stream dead). */
static CL_Obj bi_ext_stream_outbuf_stats(CL_Obj *args, int n)
{
    uint32_t used, capacity;
    CL_UNUSED(args); CL_UNUSED(n);
    cl_stream_outbuf_table_stats(&used, &capacity);
    return cl_cons(CL_MAKE_FIXNUM((int32_t)used),
                   cl_cons(CL_MAKE_FIXNUM((int32_t)capacity), CL_NIL));
}

/* (ext:%gc-audit-roots) — count double-registered GC roots (global vs.
 * global, global vs. any thread root stack).  gc_forward is not
 * idempotent, so any double-registered root silently corrupts on
 * compaction; tests assert this returns 0. */
static CL_Obj bi_ext_gc_audit_roots(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM(cl_gc_audit_roots());
}

/* (ext:%gc-audit-hdr-index) — check the block-start index the JIT
 * native-stack scan validates candidates against (0 = clean, -1 = no
 * index: generational mode or CLAMIGA_HDR_INDEX=0).  The Amiga suite
 * runs it after the JIT GC-stress tests; a violation means a header was
 * written without the index being told, which the scan would turn into
 * a phantom mark or a missed pin. */
static CL_Obj bi_ext_gc_audit_hdr_index(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM(cl_gc_audit_hdr_index());
}

/* (ext:%heap-verify) => faults, report-string.  Collects, then checks
 * every live object's header and references, the registered static
 * roots and the thread stacks (mem.c cl_gc_verify_heap).  For the field:
 * call it after each LOAD of a suspect file, or before the form that
 * fails, to find where a corruption entered -- on the Amiga the report
 * goes into the script's log, where stderr never does. */
static CL_Obj bi_ext_heap_verify(CL_Obj *args, int n)
{
    int faults;
    const char *rep;
    CL_UNUSED(args); CL_UNUSED(n);
    faults = cl_gc_verify_heap();
    rep = cl_gc_verify_heap_report();
    cl_mv_values[0] = CL_MAKE_FIXNUM(faults);
    cl_mv_values[1] = cl_make_string(rep, (uint32_t)strlen(rep));
    cl_mv_count = 2;
    return cl_mv_values[0];
}

/* (ext:%cpu-store-selftest &optional (rounds 4096)) => lost-stores.
 * Repeats the startup CPU store self-test (platform_cpu_store_selftest,
 * main.c cpu_store_check) for ROUNDS replays and answers how many left a
 * word unwritten: 0 on a sound CPU and on every non-m68k build.  Lets a
 * script measure a suspect machine, or confirm that :CPU-LOST-STORES on
 * *FEATURES* was earned. */
static CL_Obj bi_ext_cpu_store_selftest(CL_Obj *args, int n)
{
    uint32_t rounds = 4096;
    if (n > 0) {
        if (!CL_FIXNUM_P(args[0]) || CL_FIXNUM_VAL(args[0]) < 1) {
            char ob[64];
            cl_error(CL_ERR_TYPE, "%%CPU-STORE-SELFTEST: ROUNDS must be a positive fixnum, got %s",
                     cl_obj_brief(args[0], ob, (int)sizeof(ob)));
        }
        rounds = (uint32_t)CL_FIXNUM_VAL(args[0]);
    }
    return CL_MAKE_FIXNUM(platform_cpu_store_selftest(rounds) & CL_FIXNUM_MAX);
}

static CL_Obj bi_time_report(CL_Obj *args, int n)
{
    uint32_t start_time, end_time, elapsed;
    uint32_t start_consed, end_consed, bytes_consed;
    uint32_t start_gc, end_gc, gc_cycles;
    uint32_t start_run, end_run, cpu_elapsed;
    char buf[256];

    start_time = (uint32_t)CL_FIXNUM_VAL(args[0]);
    start_consed = (uint32_t)CL_FIXNUM_VAL(args[1]);
    start_gc = (uint32_t)CL_FIXNUM_VAL(args[2]);

    end_time = platform_time_ms() & CL_FIXNUM_MAX;
    end_consed = cl_heap.total_consed & CL_FIXNUM_MAX;
    end_gc = cl_heap.gc_count & CL_FIXNUM_MAX;

    elapsed = (end_time >= start_time)
        ? (end_time - start_time)
        : ((CL_FIXNUM_MAX - start_time) + end_time + 1);
    bytes_consed = (end_consed >= start_consed)
        ? (end_consed - start_consed)
        : ((CL_FIXNUM_MAX - start_consed) + end_consed + 1);
    gc_cycles = (end_gc >= start_gc)
        ? (end_gc - start_gc)
        : ((CL_FIXNUM_MAX - start_gc) + end_gc + 1);

    /* 4th arg (start run/CPU time) is optional: bytecode compiled before it
     * existed (stale user .fasl) still calls with 3 args. */
    if (n >= 4) {
        start_run = (uint32_t)CL_FIXNUM_VAL(args[3]);
        end_run = platform_run_time_ms() & CL_FIXNUM_MAX;
        cpu_elapsed = (end_run >= start_run)
            ? (end_run - start_run)
            : ((CL_FIXNUM_MAX - start_run) + end_run + 1);
        snprintf(buf, sizeof(buf),
                 "Evaluation took %lu ms real, %lu ms cpu; %lu bytes consed; %lu GC cycles; %lu/%lu heap bytes used.\n",
                 (unsigned long)elapsed,
                 (unsigned long)cpu_elapsed,
                 (unsigned long)bytes_consed,
                 (unsigned long)gc_cycles,
                 (unsigned long)cl_heap.total_allocated,
                 (unsigned long)cl_heap.arena_size);
    } else {
        snprintf(buf, sizeof(buf),
                 "Evaluation took %lu ms; %lu bytes consed; %lu GC cycles; %lu/%lu heap bytes used.\n",
                 (unsigned long)elapsed,
                 (unsigned long)bytes_consed,
                 (unsigned long)gc_cycles,
                 (unsigned long)cl_heap.total_allocated,
                 (unsigned long)cl_heap.arena_size);
    }
    cl_write_cstring_to_trace(buf);
    return CL_NIL;
}

/* Internal-time epoch: captured on first call so values stay small enough for
 * fixnum range (CLHS 25.1.4.1 only requires epoch no later than Lisp start). */
static uint32_t cl_internal_time_epoch_ms = 0;
static int cl_internal_time_epoch_set = 0;

void cl_internal_time_init(void)
{
    if (!cl_internal_time_epoch_set) {
        cl_internal_time_epoch_ms = platform_time_ms();
        cl_internal_time_epoch_set = 1;
    }
}

static uint32_t cl_internal_time_now_ms(void)
{
    uint32_t now = platform_time_ms();
    if (!cl_internal_time_epoch_set) {
        cl_internal_time_epoch_ms = now;
        cl_internal_time_epoch_set = 1;
    }
    return now - cl_internal_time_epoch_ms;
}

static CL_Obj bi_get_internal_real_time(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)(cl_internal_time_now_ms() & CL_FIXNUM_MAX));
}

static CL_Obj bi_get_internal_run_time(CL_Obj *args, int n)
{
    /* CLHS 25.1.4.2: run time = "execution time", implementation-defined.
     * We report process CPU time (user+system); on AmigaOS the platform
     * layer falls back to wall-clock time (no per-task CPU accounting).
     * No epoch shift needed: CPU time starts near 0 at process start. */
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_MAKE_FIXNUM((int32_t)(platform_run_time_ms() & CL_FIXNUM_MAX));
}

static CL_Obj bi_sleep(CL_Obj *args, int n)
{
    uint32_t ms;
    uint32_t remaining;
    CL_UNUSED(n);
    if (CL_FIXNUM_P(args[0])) {
        int32_t sec = CL_FIXNUM_VAL(args[0]);
        if (sec < 0)
            cl_error(CL_ERR_TYPE, "SLEEP: argument must be non-negative");
        ms = (uint32_t)sec * 1000;
    } else if (CL_HEAP_P(args[0]) &&
               (CL_HDR_TYPE(CL_OBJ_TO_PTR(args[0])) == TYPE_SINGLE_FLOAT ||
                CL_HDR_TYPE(CL_OBJ_TO_PTR(args[0])) == TYPE_DOUBLE_FLOAT)) {
        double val = cl_to_double(args[0]);
        if (val < 0.0)
            cl_error(CL_ERR_TYPE, "SLEEP: argument must be non-negative");
        ms = (uint32_t)(val * 1000.0);
    } else if (CL_HEAP_P(args[0]) &&
               CL_HDR_TYPE(CL_OBJ_TO_PTR(args[0])) == TYPE_RATIO) {
        double val = cl_to_double(args[0]);
        if (val < 0.0)
            cl_error(CL_ERR_TYPE, "SLEEP: argument must be non-negative");
        ms = (uint32_t)(val * 1000.0);
    } else {
        cl_error(CL_ERR_TYPE, "SLEEP: argument must be a non-negative real number");
        return CL_NIL;
    }
    /* Sleep in 100ms chunks so a concurrent MP:DESTROY-THREAD or
     * MP:INTERRUPT-THREAD aimed at this thread is honored within ~100ms,
     * rather than after the full nominal duration.  CL_SAFEPOINT() is the
     * supported way to surface the pending interrupt — it longjmps via
     * cl_error("Thread destroyed") for destroy, or invokes the pending
     * interrupt thunk otherwise. */
    remaining = ms;
    while (remaining > 0) {
        uint32_t chunk = remaining > 100 ? 100 : remaining;
        platform_sleep_ms(chunk);
        remaining -= chunk;
        CL_SAFEPOINT();
    }
    return CL_NIL;
}

/* (compile name &optional definition) */
static CL_Obj bi_compile(CL_Obj *args, int n)
{
    CL_Obj name = args[0];

    if (n > 1 && !CL_NULL_P(args[1])) {
        /* (compile nil '(lambda (x) x)) — compile lambda form, return function.
         * Detect warnings/errors signaled during compilation (e.g. a macro that
         * calls WARN at expansion time) so we can report warnings-p/failure-p
         * per CLHS COMPILE.  Save/restore the detection state so nested COMPILE
         * calls don't clobber an outer one's tally.
         *
         * CL_CATCH wraps the compile+eval so that a compile-time error (e.g. a
         * macro that calls ERROR at expansion time) propagates to the caller but
         * still decrements cl_compile_detect_depth.  Without this, any NLX out
         * of cl_compile/cl_vm_eval leaves the depth elevated, causing all later
         * unrelated conditions to spuriously set warnings-p/failure-p. */
        CL_Obj bc, fn;
        int saved_w = cl_compile_warnings_p;
        int saved_f = cl_compile_failure_p;
        int compile_err;
        fn = CL_NIL;
        cl_compile_warnings_p = 0;
        cl_compile_failure_p = 0;
        cl_compile_detect_depth++;
        CL_CATCH(compile_err);
        if (compile_err != CL_ERR_NONE) {
            char saved_msg[512];
            int  saved_code = compile_err;
            snprintf(saved_msg, sizeof(saved_msg), "%s", cl_error_msg);
            CL_UNCATCH();
            cl_compile_detect_depth--;
            cl_compile_warnings_p = saved_w;
            cl_compile_failure_p  = saved_f;
            cl_error(saved_code, "%s", saved_msg);
        }
        bc = cl_compile(args[1]);
        fn = cl_vm_eval(bc);
        CL_UNCATCH();
        cl_compile_detect_depth--;
        /* Return 3 values per CL spec: function, warnings-p, failure-p */
        cl_mv_count = 3;
        cl_mv_values[0] = fn;
        cl_mv_values[1] = cl_compile_warnings_p ? CL_T : CL_NIL;
        cl_mv_values[2] = cl_compile_failure_p ? CL_T : CL_NIL;
        cl_compile_warnings_p = saved_w;
        cl_compile_failure_p  = saved_f;
        return fn;
    }

    if (!CL_NULL_P(name) && CL_SYMBOL_P(name)) {
        /* (compile 'foo) — return existing function binding */
        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(name);
        CL_Obj fn = sym->function;
        cl_mv_count = 3;
        cl_mv_values[0] = fn;
        cl_mv_values[1] = CL_NIL;
        cl_mv_values[2] = CL_NIL;
        return fn;
    }

    return CL_NIL;
}

/* --- Pretty-printing builtins --- */

static CL_Obj KW_LINEAR;
static CL_Obj KW_FILL;
static CL_Obj KW_MISER;
static CL_Obj KW_MANDATORY;
static CL_Obj KW_BLOCK;
static CL_Obj KW_CURRENT;

/*
 * (pprint-newline kind &optional stream)
 * kind: :linear :fill :miser :mandatory
 */
static CL_Obj bi_pprint_newline(CL_Obj *args, int n)
{
    CL_Obj kind = args[0];
    CL_UNUSED(n);

    if (kind == KW_MANDATORY) {
        cl_pp_newline_indent();
    } else if (kind == KW_FILL || kind == KW_LINEAR) {
        /* Greedy: break when past right margin */
        if (cl_pp_get_column() >= cl_pp_get_right_margin())
            cl_pp_newline_indent();
    } else if (kind == KW_MISER) {
        /* Break when close to right margin */
        if (cl_pp_get_column() >= cl_pp_get_right_margin() - 4)
            cl_pp_newline_indent();
    } else {
        cl_error(CL_ERR_TYPE, "PPRINT-NEWLINE: invalid kind");
    }
    return CL_NIL;
}

/*
 * (pprint-indent relative-to n &optional stream)
 * relative-to: :block or :current
 */
static CL_Obj bi_pprint_indent(CL_Obj *args, int n)
{
    CL_Obj rel = args[0];
    int32_t delta;
    CL_UNUSED(n);

    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "PPRINT-INDENT: n must be an integer");
    delta = CL_FIXNUM_VAL(args[1]);

    if (rel == KW_CURRENT) {
        cl_pp_set_indent(cl_pp_get_column() + delta);
    } else {
        /* :block — would need block start column; use current indent as approximation */
        cl_pp_set_indent(delta);
    }
    return CL_NIL;
}

/* Internal builtins for pprint-logical-block */
static CL_Obj bi_pp_push_block(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_pp_push_block(cl_pp_get_column());
    return CL_NIL;
}

static CL_Obj bi_pp_pop_block(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    cl_pp_pop_block();
    return CL_NIL;
}

/* --- Pprint-dispatch table --- */

/* Table is an alist: ((type-spec priority . function) ...) stored in *print-pprint-dispatch* */

/*
 * (set-pprint-dispatch type-spec function &optional priority table)
 * If function is nil, removes the entry.
 */
static CL_Obj bi_set_pprint_dispatch(CL_Obj *args, int n)
{
    CL_Obj type_spec = args[0];
    CL_Obj function = args[1];
    int32_t priority = 0;
    CL_Symbol *sym;
    CL_Obj table, cur, entry, new_entry;

    if (n >= 3 && CL_FIXNUM_P(args[2]))
        priority = CL_FIXNUM_VAL(args[2]);

    /* Get the dispatch table (or use the 4th arg if provided) */
    sym = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_PRINT_PPRINT_DISPATCH);
    if (n >= 4 && !CL_NULL_P(args[3]))
        table = args[3];
    else
        table = cl_symbol_value(SYM_PRINT_PPRINT_DISPATCH);

    /* GC SAFETY (audit tier 4, IO5): both rebuild loops below cons per
     * iteration — the cursor, the accumulating result and the table must be
     * rooted or every entry after the first compaction is consed from stale
     * offsets (corrupt table → CAR-type-error/dead-closure crashes on the
     * next pretty print).  type_spec/function are re-read from the rooted
     * args[] slots after allocs instead of trusting the C locals. */

    /* If function is nil, remove matching entry (rebuild without it) */
    if (CL_NULL_P(function)) {
        CL_Obj result = CL_NIL;
        CL_GC_PROTECT(result);
        cur = table;
        CL_GC_PROTECT(cur);
        while (!CL_NULL_P(cur)) {
            type_spec = args[0];
            entry = cl_car(cur);
            if (cl_car(entry) != type_spec)
                result = cl_cons(entry, result);
            cur = cl_cdr(cur);
        }
        CL_GC_UNPROTECT(2);
        cl_set_symbol_value(SYM_PRINT_PPRINT_DISPATCH, result);
        return CL_NIL;
    }

    /* Remove existing entry for this type-spec first */
    {
        CL_Obj result = CL_NIL;
        CL_GC_PROTECT(result);
        cur = table;
        CL_GC_PROTECT(cur);
        while (!CL_NULL_P(cur)) {
            type_spec = args[0];
            entry = cl_car(cur);
            if (cl_car(entry) != type_spec)
                result = cl_cons(entry, result);
            cur = cl_cdr(cur);
        }
        CL_GC_UNPROTECT(2);
        table = result;
    }

    /* Build new entry (type-spec priority . function) and push it onto the
     * table.  Each cons can compact: keep table rooted and read
     * type-spec/function from the rooted args[] slots. */
    CL_GC_PROTECT(table);
    new_entry = cl_cons(CL_MAKE_FIXNUM(priority), args[1]);
    CL_GC_PROTECT(new_entry);
    new_entry = cl_cons(args[0], new_entry);
    table = cl_cons(new_entry, table);
    CL_GC_UNPROTECT(2);

    cl_set_symbol_value(SYM_PRINT_PPRINT_DISPATCH, table);
    return CL_NIL;
}

/*
 * (pprint-dispatch object &optional table)
 * Returns 2 values: function, found-p
 */
static CL_Obj bi_pprint_dispatch(CL_Obj *args, int n)
{
    CL_Obj table = (n >= 2 && !CL_NULL_P(args[1])) ? args[1] : cl_symbol_value(SYM_PRINT_PPRINT_DISPATCH);
    CL_Obj best_fn = CL_NIL;
    int32_t best_priority = -999999;
    CL_Obj cur;

    /* GC SAFETY (audit tier 4, IO7): cl_typep can run a deftype expander
     * (cl_vm_apply — allocates/compacts).  Keep the cursor and the winning
     * function rooted; read the object from the rooted args[0] slot and
     * re-derive the entry from the forwarded cursor AFTER each typep call. */
    cur = table;
    CL_GC_PROTECT(cur);
    CL_GC_PROTECT(best_fn);
    while (!CL_NULL_P(cur)) {
        CL_Obj entry, prio_fn;
        int32_t prio;
        int matches;

        entry = cl_car(cur);
        prio_fn = cl_cdr(entry);
        prio = CL_FIXNUM_P(cl_car(prio_fn)) ? CL_FIXNUM_VAL(cl_car(prio_fn)) : 0;

        matches = cl_typep(args[0], cl_car(entry));

        if (matches && prio > best_priority) {
            best_priority = prio;
            /* re-derive through the forwarded cursor — the pre-typep
             * entry/fn locals may be stale after a compaction */
            best_fn = cl_cdr(cl_cdr(cl_car(cur)));
        }

        cur = cl_cdr(cur);
    }
    CL_GC_UNPROTECT(2);

    cl_mv_count = 2;
    cl_mv_values[0] = best_fn;
    cl_mv_values[1] = CL_NULL_P(best_fn) ? CL_NIL : SYM_T;
    return best_fn;
}

/*
 * (copy-pprint-dispatch &optional table)
 * Shallow copy the alist.
 */
static CL_Obj bi_copy_pprint_dispatch(CL_Obj *args, int n)
{
    CL_Obj table = (n >= 1 && !CL_NULL_P(args[0])) ? args[0] : cl_symbol_value(SYM_PRINT_PPRINT_DISPATCH);
    CL_Obj result = CL_NIL;
    CL_Obj cur;

    /* Rebuild list (reverses order, but that's fine for alists).
     * GC SAFETY (audit tier 4, IO6): each cl_cons can compact — the cursor
     * must stay forwarded alongside the accumulating result. */
    cur = table;
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(cur);
    while (!CL_NULL_P(cur)) {
        result = cl_cons(cl_car(cur), result);
        cur = cl_cdr(cur);
    }
    CL_GC_UNPROTECT(2);
    return result;
}

/* --- Modules (provide / require) --- */

/* Get module name as a C string from a string or symbol argument.
   charbuf must be a caller-supplied buffer of at least 8 bytes; it is used
   when arg is a character (the only case that cannot return a pointer into
   heap-managed storage directly). */
static const char *module_name_cstr(CL_Obj arg, uint32_t *len_out, char *charbuf)
{
    if (CL_STRING_P(arg)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(arg);
        *len_out = s->length;
        return s->data;
    }
    if (CL_SYMBOL_P(arg)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(arg);
        CL_String *name = (CL_String *)CL_OBJ_TO_PTR(s->name);
        *len_out = name->length;
        return name->data;
    }
    if (CL_CHAR_P(arg)) {
        /* A character is a valid string designator (CLHS glossary): it
           denotes the singleton string containing just that character.
           Use caller-supplied stack buffer so concurrent calls from different
           threads don't race on shared state. */
        int nb = cl_utf8_encode(CL_CHAR_VAL(arg), charbuf);
        if (nb < 0) nb = 0;
        charbuf[nb] = '\0';
        *len_out = (uint32_t)nb;
        return charbuf;
    }
    cl_error(CL_ERR_TYPE,
             "module name must be a string designator (string, symbol, or character)");
    *len_out = 0;
    return NULL;
}

/* ASCII case-insensitive equality of two byte strings of equal length. */
static int module_name_equalp(const char *a, const char *b, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return 1;
}

/* Check if module name is in *modules* list.
   fold_case=0: exact (string=) match — PROVIDE's duplicate check, so a
   module may register both spellings, e.g. asdf.lisp's (provide "asdf")
   (provide "ASDF"), exactly as SBCL does.
   fold_case=1: string-equal match — REQUIRE's "already loaded" test.  The
   CLHS leaves the comparison implementation-dependent; folding case is what
   makes (require :gray-streams) after (require "gray-streams") a no-op
   instead of a second load (the reader upcases the symbol, the module
   provides its lowercase file name). */
static int module_provided_p(const char *name, uint32_t len, int fold_case)
{
    CL_Obj list = cl_symbol_value(SYM_STAR_MODULES);
    while (!CL_NULL_P(list)) {
        CL_Obj entry = cl_car(list);
        if (CL_STRING_P(entry)) {
            CL_String *s = (CL_String *)CL_OBJ_TO_PTR(entry);
            if (s->length == len &&
                (fold_case ? module_name_equalp(s->data, name, len)
                           : memcmp(s->data, name, len) == 0))
                return 1;
        }
        list = cl_cdr(list);
    }
    return 0;
}

/* Downcase an ASCII module name into buf.  Returns 1 if the name had any
   uppercase ASCII letter (i.e. the downcased spelling is a new candidate)
   and fits, 0 otherwise. */
static int module_name_downcase(const char *name, uint32_t len,
                                char *buf, uint32_t bufsz)
{
    uint32_t i;
    int changed = 0;
    if (len + 1 > bufsz) return 0;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c >= 'A' && c <= 'Z') { c = (unsigned char)(c + ('a' - 'A')); changed = 1; }
        buf[i] = (char)c;
    }
    buf[len] = '\0';
    return changed;
}

#define MODULE_PATH_MAX 512   /* REQUIRE's candidate path buffers */

/* Locate lib/<name>.fasl and lib/<name>.lisp for REQUIRE's
   implementation-defined search.  Fills fasl_path/lisp_path (each
   MODULE_PATH_MAX bytes) and sets *have_fasl / *have_lisp to whether each exists.  The
   search order is: cwd-relative lib/, then platform fallbacks (Amiga:
   PROGDIR: and the in-repo ancestor; host: $CLAMIGA_HOME and the
   executable-relative cl_lib_rel_dirs table). */
static void module_locate(const char *name, uint32_t len,
                          char *fasl_path, char *lisp_path,
                          int *have_fasl, int *have_lisp)
{
    snprintf(fasl_path, MODULE_PATH_MAX, "lib/%.*s.fasl", (int)len, name);
    snprintf(lisp_path, MODULE_PATH_MAX, "lib/%.*s.lisp", (int)len, name);
    *have_fasl = platform_file_exists(fasl_path);
    *have_lisp = platform_file_exists(lisp_path);

#ifdef PLATFORM_AMIGA
    if (!*have_fasl && !*have_lisp) {
        snprintf(fasl_path, MODULE_PATH_MAX, "PROGDIR:lib/%.*s.fasl", (int)len, name);
        snprintf(lisp_path, MODULE_PATH_MAX, "PROGDIR:lib/%.*s.lisp", (int)len, name);
        *have_fasl = platform_file_exists(fasl_path);
        *have_lisp = platform_file_exists(lisp_path);
    }
    /* Executable-relative in-repo layout (build/amiga/clamiga with
       lib/ two levels up) — mirrors the boot.lisp search in repl.c
       and the host's "../../" fallback, so REQUIRE works when
       clamiga is launched from any cwd (e.g. an example project
       directory).  "PROGDIR://" cannot express this (parent-climb
       slashes only apply to cwd-relative paths), so the ancestor is
       resolved through dos.library ParentDir. */
    if (!*have_fasl && !*have_lisp) {
        char prefix[300];
        if (platform_executable_ancestor_prefix(2, prefix,
                                                (int)sizeof(prefix))) {
            snprintf(fasl_path, MODULE_PATH_MAX, "%slib/%.*s.fasl",
                     prefix, (int)len, name);
            snprintf(lisp_path, MODULE_PATH_MAX, "%slib/%.*s.lisp",
                     prefix, (int)len, name);
            *have_fasl = platform_file_exists(fasl_path);
            *have_lisp = platform_file_exists(lisp_path);
        }
    }
#else
    /* Host fallback: when clamiga is launched from a directory other
       than its source root (e.g. an editor/Sly session whose cwd
       follows the file buffer), cwd-relative "lib/" won't exist.
       Honour $CLAMIGA_HOME so the bundled lib/ is still found without
       forcing the process working directory — on Amiga PROGDIR: above
       already serves this role. */
    if (!*have_fasl && !*have_lisp) {
        char homebuf[256];
        const char *home = platform_getenv("CLAMIGA_HOME", homebuf, sizeof(homebuf));
        if (home && home[0]) {
            size_t hlen = strlen(home);
            /* tolerate a trailing slash on CLAMIGA_HOME */
            int hcut = (hlen > 0 && home[hlen - 1] == '/') ? (int)(hlen - 1) : (int)hlen;
            snprintf(fasl_path, MODULE_PATH_MAX, "%.*s/lib/%.*s.fasl",
                     hcut, home, (int)len, name);
            snprintf(lisp_path, MODULE_PATH_MAX, "%.*s/lib/%.*s.lisp",
                     hcut, home, (int)len, name);
            *have_fasl = platform_file_exists(fasl_path);
            *have_lisp = platform_file_exists(lisp_path);
        }
    }

    /* Executable-relative fallback (mirrors the boot.lisp search in
       repl.c, sharing its cl_lib_rel_dirs table): a deployed layout
       has lib/ next to the binary, an install prefix has
       ../lib/clamiga/, the in-repo build has lib/ two levels above
       build/host/clamiga — so a plain `clamiga` on $PATH works from
       any directory without CLAMIGA_HOME. */
    if (!*have_fasl && !*have_lisp) {
        char prefix[300];
        if (platform_executable_prefix(prefix, (int)sizeof(prefix))) {
            int ri;
            for (ri = 0; ri < CL_LIB_REL_COUNT && !*have_fasl && !*have_lisp; ri++) {
                snprintf(fasl_path, MODULE_PATH_MAX, "%s%s%.*s.fasl",
                         prefix, cl_lib_rel_dirs[ri], (int)len, name);
                snprintf(lisp_path, MODULE_PATH_MAX, "%s%s%.*s.lisp",
                         prefix, cl_lib_rel_dirs[ri], (int)len, name);
                *have_fasl = platform_file_exists(fasl_path);
                *have_lisp = platform_file_exists(lisp_path);
            }
        }
    }
#endif
}

/*
 * (provide module-name)
 * Add module-name to *modules* if not already present.
 */
static CL_Obj bi_provide(CL_Obj *args, int n)
{
    uint32_t len;
    const char *name;
    char charbuf[8];
    CL_Obj name_str, modules;

    CL_UNUSED(n);
    name = module_name_cstr(args[0], &len, charbuf);

    if (module_provided_p(name, len, 0))
        return SYM_T;

    /* Push string onto *modules* */
    name_str = cl_make_string(name, len);
    CL_GC_PROTECT(name_str);
    /* Cons first, then read the symbol for the store (mem.h, cl_list2). */
    modules = cl_cons(name_str, cl_symbol_value(SYM_STAR_MODULES));
    cl_set_symbol_value(SYM_STAR_MODULES, modules);
    CL_GC_UNPROTECT(1);
    return SYM_T;
}

/*
 * (require module-name &optional pathnames)
 * Load module if not already provided.
 */
static CL_Obj bi_require(CL_Obj *args, int n)
{
    uint32_t len;
    const char *name;
    char charbuf[8];

    name = module_name_cstr(args[0], &len, charbuf);

    /* Already provided?  Case-folded: (require :asdf) after (require "asdf")
       must not load twice. */
    if (module_provided_p(name, len, 1))
        return CL_NIL;

    if (n >= 2 && !CL_NULL_P(args[1])) {
        /* Pathnames provided.
         * GC SAFETY (audit tier 4, IO8): bi_load compacts freely — the
         * load_args[0] C slot must be a registered root (bi_load treats
         * args[0] as a rooted, forwarded slot and re-reads it after its own
         * allocs), and the pathnames cursor must stay forwarded across each
         * load. */
        CL_Obj pathnames = args[1];
        if (CL_STRING_P(pathnames) || CL_PATHNAME_P(pathnames)) {
            /* Single pathname */
            CL_Obj load_args[1];
            load_args[0] = pathnames;
            CL_GC_PROTECT(load_args[0]);
            bi_load(load_args, 1);
            CL_GC_UNPROTECT(1);
        } else if (CL_CONS_P(pathnames)) {
            /* List of pathnames */
            CL_Obj load_args[1];
            CL_GC_PROTECT(pathnames);
            load_args[0] = CL_NIL;
            CL_GC_PROTECT(load_args[0]);
            while (!CL_NULL_P(pathnames)) {
                load_args[0] = cl_car(pathnames);
                bi_load(load_args, 1);
                pathnames = cl_cdr(pathnames);
            }
            CL_GC_UNPROTECT(2);
        } else {
            cl_error(CL_ERR_TYPE, "REQUIRE: pathnames must be a string, pathname, or list");
        }
    } else {
        /* Implementation-defined search: try .fasl first, then .lisp */
        char path[512];
        CL_Obj load_args[1];
        CL_Obj path_obj;
        int found = 0;

        /* Try .fasl and .lisp; prefer .fasl only if newer than .lisp */
        {
            char fasl_path[MODULE_PATH_MAX], lisp_path[MODULE_PATH_MAX];
            int have_fasl = 0, have_lisp = 0;

            module_locate(name, len, fasl_path, lisp_path, &have_fasl, &have_lisp);

            /* Case fallback (issue #18): the module name is a string
               designator, and a symbol's name is upcased by the reader —
               (require :asdf) / (require 'asdf) arrive here as "ASDF" while
               the file is lib/asdf.lisp.  Amiga and macOS file systems are
               case-insensitive and hide this; a case-sensitive host (Linux)
               needs the downcased spelling tried explicitly. */
            if (!have_fasl && !have_lisp) {
                char lcname[256];
                if (module_name_downcase(name, len, lcname, sizeof(lcname)))
                    module_locate(lcname, len, fasl_path, lisp_path,
                                  &have_fasl, &have_lisp);
            }

            /* Pre-validate FASL header — if magic or version mismatches the
               current runtime, treat the FASL as absent so we recompile from
               source (mirrors the boot.fasl recovery in repl.c).  Without
               this, bumping CL_FASL_VERSION strands lib/*.fasl until a manual
               `make fasl`. */
            if (have_fasl) {
                unsigned long hsize = 0;
                char *hbuf = platform_file_read(fasl_path, &hsize);
                int header_ok = 0;
                if (hbuf) {
                    if (hsize >= 6) {
                        uint32_t magic = ((uint32_t)(uint8_t)hbuf[0] << 24) |
                                         ((uint32_t)(uint8_t)hbuf[1] << 16) |
                                         ((uint32_t)(uint8_t)hbuf[2] << 8) |
                                         ((uint32_t)(uint8_t)hbuf[3]);
                        uint32_t fver  = ((uint32_t)(uint8_t)hbuf[4] << 8) |
                                         ((uint32_t)(uint8_t)hbuf[5]);
                        if (magic == CL_FASL_MAGIC && fver == CL_FASL_VERSION)
                            header_ok = 1;
                    }
                    platform_free(hbuf);
                }
                if (!header_ok)
                    have_fasl = 0;
            }

            if (have_fasl && have_lisp) {
                /* Both exist: prefer FASL only if at least as new as source */
                uint32_t fasl_mt = platform_file_mtime(fasl_path);
                uint32_t lisp_mt = platform_file_mtime(lisp_path);
                if (fasl_mt > 0 && fasl_mt >= lisp_mt) {
                    snprintf(path, sizeof(path), "%s", fasl_path);
                    found = 1;
                } else {
                    snprintf(path, sizeof(path), "%s", lisp_path);
                    found = 1;
                }
            } else if (have_fasl) {
                snprintf(path, sizeof(path), "%s", fasl_path);
                found = 1;
            } else if (have_lisp) {
                snprintf(path, sizeof(path), "%s", lisp_path);
                found = 1;
            }
        }

        if (!found) {
            char msg[640];   /* fixed text ~330 chars + 3x the module name */
            snprintf(msg, sizeof(msg),
                     "REQUIRE: cannot find module \"%.*s\" - looked for "
                     "lib/%.*s.fasl and lib/%.*s.lisp (and the lowercase "
                     "spelling) under the current "
#ifdef PLATFORM_AMIGA
                     "directory, PROGDIR: and two levels above the binary "
                     "(in-repo layout).",
#else
                     "directory, $CLAMIGA_HOME, and - relative to the clamiga "
                     "executable - lib/, ../lib/clamiga/ (installed layout) "
                     "and ../../lib/ (in-repo build). If clamiga runs outside "
                     "its installation, set CLAMIGA_HOME to the cl-amiga "
                     "directory.",
#endif
                     (int)len, name, (int)len, name, (int)len, name);
            cl_error(CL_ERR_GENERAL, "%s", msg);
        }

        path_obj = cl_make_string(path, (uint32_t)strlen(path));
        /* Root the load_args slot itself (not just path_obj) — bi_load
         * re-reads args[0] after its own compactions. */
        load_args[0] = path_obj;
        CL_GC_PROTECT(load_args[0]);
        bi_load(load_args, 1);
        CL_GC_UNPROTECT(1);
    }

    return SYM_T;
}

/* --- Environment Info --- */

static CL_Obj bi_lisp_implementation_type(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return cl_make_string("CL-Amiga", 8);
}

static CL_Obj bi_lisp_implementation_version(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    /* Derived from CL_VERSION_* in types.h — never spell the version here. */
    return cl_make_string(CL_VERSION_STRING_FULL, CL_VERSION_LEN_FULL);
}

static CL_Obj bi_software_type(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
#if defined(PLATFORM_MORPHOS)
    return cl_make_string("MorphOS", 7);
#elif defined(PLATFORM_AMIGA)
    return cl_make_string("AmigaOS", 7);
#elif defined(__APPLE__)
    return cl_make_string("Darwin", 6);
#else
    return cl_make_string("Linux", 5);
#endif
}

static CL_Obj bi_software_version(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

static CL_Obj bi_machine_type(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
#if defined(PLATFORM_MORPHOS)
    return cl_make_string("ppc", 3);
#elif defined(PLATFORM_AMIGA)
    return cl_make_string("m68k", 4);
#elif defined(__aarch64__)
    return cl_make_string("aarch64", 7);
#elif defined(__x86_64__)
    return cl_make_string("x86-64", 6);
#else
    return cl_make_string("unknown", 7);
#endif
}

static CL_Obj bi_machine_version(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

static CL_Obj bi_machine_instance(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return CL_NIL;
}

/* (ext:unpack-byterun1 src pos end dst dst-len &optional (dst-start 0))
 *   => new source position
 *
 * Decode ByteRun1/PackBits-compressed bytes (the RLE shared by IFF ILBM
 * BODYs, TIFF and MacPaint: literal runs for codes 0..127, repeat runs for
 * 129..255, 128 a no-op) from SRC[POS,END) until DST-LEN bytes have been
 * produced, writing them to DST from DST-START.  SRC and DST must be
 * (unsigned-byte 8) vectors.  Signals a clear error on truncated input or
 * a run that overflows the requested output — corrupt data must not
 * silently misalign everything that follows.
 *
 * A C builtin because decoding is per-byte work: one VM round-trip per
 * output byte made unpacking an ILBM plane row cost seconds on a 68020.
 * No allocation happens inside, so the raw payload pointers stay valid. */
static CL_Obj bi_unpack_byterun1(CL_Obj *args, int n)
{
    CL_ByteVector *src, *dst;
    uint32_t pos, end, out, limit;
    int32_t dst_len, dst_start = 0;
    const uint8_t *sp;
    uint8_t *dp;

    if (!CL_BYTE_VECTOR_P(args[0]) ||
        ((CL_ByteVector *)CL_OBJ_TO_PTR(args[0]))->elt_shift != 0 ||
        ((CL_ByteVector *)CL_OBJ_TO_PTR(args[0]))->is_signed)
        cl_error(CL_ERR_TYPE,
                 "EXT:UNPACK-BYTERUN1: source must be an (UNSIGNED-BYTE 8) vector");
    if (!CL_BYTE_VECTOR_P(args[3]) ||
        ((CL_ByteVector *)CL_OBJ_TO_PTR(args[3]))->elt_shift != 0 ||
        ((CL_ByteVector *)CL_OBJ_TO_PTR(args[3]))->is_signed)
        cl_error(CL_ERR_TYPE,
                 "EXT:UNPACK-BYTERUN1: destination must be an (UNSIGNED-BYTE 8) vector");
    if (!CL_FIXNUM_P(args[1]) || !CL_FIXNUM_P(args[2]) || !CL_FIXNUM_P(args[4]) ||
        (n >= 6 && !CL_FIXNUM_P(args[5])))
        cl_error(CL_ERR_TYPE, "EXT:UNPACK-BYTERUN1: indices must be fixnums");

    src = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
    dst = (CL_ByteVector *)CL_OBJ_TO_PTR(args[3]);
    dst_len = CL_FIXNUM_VAL(args[4]);
    if (n >= 6) dst_start = CL_FIXNUM_VAL(args[5]);

    {
        int32_t p = CL_FIXNUM_VAL(args[1]);
        int32_t e = CL_FIXNUM_VAL(args[2]);
        int32_t slen = (int32_t)cl_bytevec_active_length(src);
        int32_t dlen = (int32_t)cl_bytevec_active_length(dst);
        if (p < 0 || e > slen || p > e)
            cl_error(CL_ERR_ARGS,
                     "EXT:UNPACK-BYTERUN1: source range %d..%d is bad for a vector of length %d",
                     (int)p, (int)e, (int)slen);
        if (dst_len < 0 || dst_start < 0 || dst_start + dst_len > dlen)
            cl_error(CL_ERR_ARGS,
                     "EXT:UNPACK-BYTERUN1: output range %d..%d is bad for a vector of length %d",
                     (int)dst_start, (int)(dst_start + dst_len), (int)dlen);
        pos = (uint32_t)p;
        end = (uint32_t)e;
        out = (uint32_t)dst_start;
        limit = (uint32_t)(dst_start + dst_len);
    }

    /* volatile: m68k-amigaos-gcc miscompiles flexible-array `->data +
     * offset` pointer arithmetic (off-by-one pointers) — route the base
     * pointers through volatile locals so the fold cannot happen (same
     * workaround as cl_stream_write_lisp_string / bi_replace). */
    {
        const uint8_t *volatile spv = src->data;
        uint8_t *volatile dpv = dst->data;
        sp = (const uint8_t *)spv;
        dp = (uint8_t *)dpv;
    }
    while (out < limit) {
        uint32_t code, cnt;
        if (pos >= end)
            cl_error(CL_ERR_GENERAL,
                     "EXT:UNPACK-BYTERUN1: truncated ByteRun1 data (need %u more output bytes)",
                     (unsigned)(limit - out));
        code = sp[pos++];
        if (code < 128) {               /* literal run of code+1 bytes */
            cnt = code + 1;
            if (out + cnt > limit || pos + cnt > end)
                cl_error(CL_ERR_GENERAL,
                         "EXT:UNPACK-BYTERUN1: literal run of %u overflows its row",
                         (unsigned)cnt);
            memcpy(dp + out, sp + pos, cnt);
            out += cnt;
            pos += cnt;
        } else if (code > 128) {        /* repeat next byte 257-code times */
            cnt = 257 - code;
            if (out + cnt > limit)
                cl_error(CL_ERR_GENERAL,
                         "EXT:UNPACK-BYTERUN1: repeat run of %u overflows its row",
                         (unsigned)cnt);
            if (pos >= end)
                cl_error(CL_ERR_GENERAL,
                         "EXT:UNPACK-BYTERUN1: truncated ByteRun1 data (repeat byte missing)");
            memset(dp + out, sp[pos++], cnt);
            out += cnt;
        }                               /* 128: no-op */
    }
    return CL_MAKE_FIXNUM((int32_t)pos);
}

/* (ext:copy-rows dst src count chunk dst-start dst-stride src-start src-stride)
 *   => dst
 *
 * Copy COUNT rows of CHUNK bytes each between (unsigned-byte 8) vectors:
 * row I goes from SRC[src-start + I*src-stride ...) to
 * DST[dst-start + I*dst-stride ...).  This is the gather/scatter step for
 * interleaved byte formats — an IFF ILBM BODY decoded in one piece holds
 * each bitplane's rows CHUNK bytes wide every SRC-STRIDE bytes, and one
 * call here extracts a whole plane where a Lisp loop would cost a VM
 * round-trip per row (hundreds per image on a 14MHz 68020).
 *
 * Rows are copied front to back with memmove, so overlap WITHIN a row is
 * safe; when SRC and DST are the same vector, rows already written are
 * not re-read only if the regions don't cross (the REPLACE rule, per
 * row).  No allocation happens inside, so the raw payload pointers stay
 * valid. */
static CL_Obj bi_copy_rows(CL_Obj *args, int n)
{
    CL_ByteVector *dst, *src;
    int32_t count, chunk, dst_start, dst_stride, src_start, src_stride;
    int32_t dlen, slen, i;
    const uint8_t *sp;
    uint8_t *dp;
    CL_UNUSED(n);

    if (!CL_BYTE_VECTOR_P(args[0]) ||
        ((CL_ByteVector *)CL_OBJ_TO_PTR(args[0]))->elt_shift != 0 ||
        ((CL_ByteVector *)CL_OBJ_TO_PTR(args[0]))->is_signed)
        cl_error(CL_ERR_TYPE,
                 "EXT:COPY-ROWS: destination must be an (UNSIGNED-BYTE 8) vector");
    if (!CL_BYTE_VECTOR_P(args[1]) ||
        ((CL_ByteVector *)CL_OBJ_TO_PTR(args[1]))->elt_shift != 0 ||
        ((CL_ByteVector *)CL_OBJ_TO_PTR(args[1]))->is_signed)
        cl_error(CL_ERR_TYPE,
                 "EXT:COPY-ROWS: source must be an (UNSIGNED-BYTE 8) vector");
    for (i = 2; i <= 7; i++)
        if (!CL_FIXNUM_P(args[i]))
            cl_error(CL_ERR_TYPE, "EXT:COPY-ROWS: counts, starts and strides "
                     "must be fixnums (argument %d of %d is 0x%08x)",
                     (int)i, n, (unsigned)args[i]);

    dst = (CL_ByteVector *)CL_OBJ_TO_PTR(args[0]);
    src = (CL_ByteVector *)CL_OBJ_TO_PTR(args[1]);
    count      = CL_FIXNUM_VAL(args[2]);
    chunk      = CL_FIXNUM_VAL(args[3]);
    dst_start  = CL_FIXNUM_VAL(args[4]);
    dst_stride = CL_FIXNUM_VAL(args[5]);
    src_start  = CL_FIXNUM_VAL(args[6]);
    src_stride = CL_FIXNUM_VAL(args[7]);
    dlen = (int32_t)cl_bytevec_active_length(dst);
    slen = (int32_t)cl_bytevec_active_length(src);

    if (count < 0 || chunk < 0 || dst_start < 0 || dst_stride < 0 ||
        src_start < 0 || src_stride < 0)
        cl_error(CL_ERR_ARGS,
                 "EXT:COPY-ROWS: counts, starts and strides must be non-negative");
    if (count > 0 && chunk > 0) {
        /* the widest access on each side is the last row's end */
        int64_t dend = (int64_t)dst_start + (int64_t)(count - 1) * dst_stride + chunk;
        int64_t send = (int64_t)src_start + (int64_t)(count - 1) * src_stride + chunk;
        if (dend > (int64_t)dlen)
            cl_error(CL_ERR_ARGS,
                     "EXT:COPY-ROWS: %d rows of %d bytes at stride %d from %d "
                     "run past the destination's %d bytes",
                     (int)count, (int)chunk, (int)dst_stride, (int)dst_start,
                     (int)dlen);
        if (send > (int64_t)slen)
            cl_error(CL_ERR_ARGS,
                     "EXT:COPY-ROWS: %d rows of %d bytes at stride %d from %d "
                     "run past the source's %d bytes",
                     (int)count, (int)chunk, (int)src_stride, (int)src_start,
                     (int)slen);
        /* volatile: m68k-amigaos-gcc miscompiles flexible-array `->data +
         * offset` pointer arithmetic — same workaround as
         * bi_unpack_byterun1 / bi_replace above. */
        {
            const uint8_t *volatile spv = src->data;
            uint8_t *volatile dpv = dst->data;
            sp = (const uint8_t *)spv;
            dp = (uint8_t *)dpv;
        }
        for (i = 0; i < count; i++)
            memmove(dp + (uint32_t)dst_start + (uint32_t)i * (uint32_t)dst_stride,
                    sp + (uint32_t)src_start + (uint32_t)i * (uint32_t)src_stride,
                    (size_t)chunk);
    }
    return args[0];
}

/* (ext:executable-path) => string or NIL
 * The path of the running clamiga as another process can start it:
 * "/usr/local/bin/clamiga" (symlinks resolved), "C:/.../clamiga.exe",
 * "PROGDIR:clamiga".  NIL when the platform cannot tell.  What the host
 * Clamacs starts a second clamiga with. */
static CL_Obj bi_executable_path(CL_Obj *args, int n)
{
    char buf[1024];
    CL_UNUSED(args);
    CL_UNUSED(n);
    if (!platform_executable_path(buf, (int)sizeof(buf)))
        return CL_NIL;
    return cl_make_string(buf, (uint32_t)strlen(buf));
}

static CL_Obj bi_getenv(CL_Obj *args, int n)
{
    CL_Obj name_obj = args[0];
    const char *result;
    CL_UNUSED(n);
    if (!CL_STRING_P(name_obj))
        cl_error(CL_ERR_TYPE, "EXT:GETENV: argument must be a string");
    {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(name_obj);
        char buf[256];
        result = platform_getenv(s->data, buf, sizeof(buf));
        if (!result) return CL_NIL;
        return cl_make_string(result, (uint32_t)strlen(result));
    }
}

/* (ext:tty-p) => T iff stdin is an interactive terminal (a real console).
 * Lets TUI code decide up front whether raw mode can work at all. */
static CL_Obj bi_ext_tty_p(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return platform_stdin_is_interactive() ? CL_T : CL_NIL;
}

/* (ext:tty-raw-mode enable) => T on success, NIL on failure.
 * ENABLE non-NIL puts the console in raw mode (no echo, no line buffering,
 * characters delivered as typed); NIL restores cooked mode.  Fails (NIL)
 * when stdin is not an interactive terminal.  While raw mode is active,
 * LISTEN and READ-CHAR-NO-HANG on console streams report availability
 * exactly.  The runtime restores cooked mode on exit as crash insurance,
 * but callers should pair enable/disable (e.g. via unwind-protect). */
static CL_Obj bi_ext_tty_raw_mode(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return (platform_tty_raw(!CL_NULL_P(args[0])) == 0) ? CL_T : CL_NIL;
}

/* (ext:tty-size) => (cols . rows) or NIL when the size is unknown
 * (stdin/stdout not a terminal, or the query failed). */
static CL_Obj bi_ext_tty_size(CL_Obj *args, int n)
{
    int cols, rows;
    CL_UNUSED(args); CL_UNUSED(n);
    if (platform_tty_size(&cols, &rows) != 0)
        return CL_NIL;
    return cl_cons(CL_MAKE_FIXNUM(cols), CL_MAKE_FIXNUM(rows));
}

static CL_Obj bi_getcwd(CL_Obj *args, int n)
{
    char buf[1024];
    int len;
    CL_UNUSED(args); CL_UNUSED(n);
    len = platform_getcwd(buf, sizeof(buf));
    if (len == 0)
        cl_error(CL_ERR_GENERAL, "EXT:GETCWD: could not determine current directory");
    return cl_make_string(buf, (uint32_t)len);
}

static CL_Obj bi_system_command(CL_Obj *args, int n)
{
    CL_Obj cmd_obj = args[0];
    CL_String *s;
    int exit_code;
    CL_UNUSED(n);
    if (!CL_STRING_P(cmd_obj))
        cl_error(CL_ERR_TYPE, "EXT:SYSTEM-COMMAND: argument must be a string");
    s = (CL_String *)CL_OBJ_TO_PTR(cmd_obj);
    exit_code = platform_system(s->data);
    return CL_MAKE_FIXNUM(exit_code);
}

/* (ext:open-tcp-stream host port &optional connect-timeout) => stream or NIL
 * Like LispWorks comm:open-tcp-stream / CLISP socket:socket-connect.
 * Returns a bidirectional binary stream connected to host:port.
 * CONNECT-TIMEOUT, when supplied as a non-negative real, bounds the connect
 * handshake to that many seconds (fractions allowed) so an unreachable host
 * fails fast instead of hanging on the OS connect timeout; NIL or omitted
 * blocks until the OS gives up. */
static CL_Obj bi_open_tcp_stream(CL_Obj *args, int n)
{
    CL_Obj host_obj = args[0];
    CL_Obj port_obj = args[1];
    CL_String *host_str;
    int port;
    int connect_ms = 0;
    CL_Obj stream;
    if (!CL_STRING_P(host_obj))
        cl_error(CL_ERR_TYPE, "EXT:OPEN-TCP-STREAM: host must be a string");
    if (!CL_FIXNUM_P(port_obj))
        cl_error(CL_ERR_TYPE, "EXT:OPEN-TCP-STREAM: port must be an integer");
    if (n >= 3 && !CL_NULL_P(args[2])) {
        double secs;
        if (!CL_REALP(args[2]))
            cl_error(CL_ERR_TYPE,
                     "EXT:OPEN-TCP-STREAM: connect-timeout must be a non-negative real or NIL");
        secs = cl_to_double(args[2]);
        if (secs < 0.0)
            cl_error(CL_ERR_GENERAL,
                     "EXT:OPEN-TCP-STREAM: connect-timeout must be non-negative");
        connect_ms = (secs * 1000.0 >= (double)INT_MAX) ? INT_MAX : (int)(secs * 1000.0 + 0.5);
    }
    host_str = (CL_String *)CL_OBJ_TO_PTR(host_obj);
    port = CL_FIXNUM_VAL(port_obj);
    if (port < 1 || port > 65535)
        cl_error(CL_ERR_GENERAL, "EXT:OPEN-TCP-STREAM: port must be 1-65535");
    /* Copy the hostname out of the arena: cl_make_socket_stream allocates
     * (cl_make_stream), so host_str->data is stale in the error message
     * below — printing a garbage hostname. */
    {
        char host_buf[256];
        uint32_t hl = host_str->length;
        if (hl >= sizeof(host_buf))
            cl_error(CL_ERR_GENERAL,
                     "EXT:OPEN-TCP-STREAM: hostname too long (%u bytes, max %u)",
                     (unsigned)hl, (unsigned)sizeof(host_buf) - 1);
        memcpy(host_buf, host_str->data, hl);
        host_buf[hl] = '\0';
        stream = cl_make_socket_stream(host_buf, port, connect_ms);
        if (CL_NULL_P(stream))
            cl_error(CL_ERR_GENERAL,
                     "EXT:OPEN-TCP-STREAM: failed to connect to %s:%d",
                     host_buf, port);
    }
    return stream;
}

/* (ext:socket-listen port &optional address) => listening socket stream
 * Binds a TCP server socket to `port` and starts listening.  ADDRESS says
 * where: T binds 127.0.0.1 only, NIL (or omitted) every interface, and a
 * string names ONE dotted-quad IPv4 address of this machine ("192.168.1.5";
 * "0.0.0.0" is every interface again) -- what a development port that must
 * be reachable from another machine, and from nowhere else, binds.  A port
 * of 0 lets the OS pick an ephemeral port.  Use ext:socket-accept to wait
 * for and accept client connections.  Close it with CLOSE. */
static CL_Obj bi_socket_listen(CL_Obj *args, int n)
{
    CL_Obj port_obj = args[0];
    int port;
    CL_Obj stream;
    char addr_buf[16];
    const char *addr = NULL;
    if (!CL_FIXNUM_P(port_obj))
        cl_error(CL_ERR_TYPE, "EXT:SOCKET-LISTEN: port must be an integer");
    port = CL_FIXNUM_VAL(port_obj);
    if (port < 0 || port > 65535)
        cl_error(CL_ERR_GENERAL, "EXT:SOCKET-LISTEN: port must be 0-65535");
    if (n >= 2 && CL_STRING_P(args[1])) {
        /* Copied out of the arena: cl_make_listen_stream_addr allocates. */
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(args[1]);
        if (s->length == 0 || s->length >= sizeof(addr_buf))
            cl_error(CL_ERR_GENERAL,
                     "EXT:SOCKET-LISTEN: the address must be a dotted-quad IPv4 "
                     "address such as \"192.168.1.5\" (T for 127.0.0.1, NIL for every interface)");
        memcpy(addr_buf, s->data, s->length);
        addr_buf[s->length] = '\0';
        addr = addr_buf;
    } else if (n >= 2 && !CL_NULL_P(args[1])) {
        addr = "127.0.0.1";
    }
    stream = cl_make_listen_stream_addr(port, addr, NULL);
    if (CL_NULL_P(stream)) {
        if (addr && addr == addr_buf)
            cl_error(CL_ERR_GENERAL,
                     "EXT:SOCKET-LISTEN: failed to bind/listen on %s:%d (is it a "
                     "dotted-quad address of an interface of this machine?)",
                     addr_buf, port);
        cl_error(CL_ERR_GENERAL,
                 "EXT:SOCKET-LISTEN: failed to bind/listen on port %d", port);
    }
    return stream;
}

/* (ext:socket-accept listener) => connection stream
 * Blocks until a client connects to the listening socket stream `listener`,
 * then returns a bidirectional binary socket stream for that connection. */
static CL_Obj bi_socket_accept(CL_Obj *args, int n)
{
    CL_Obj listener = args[0];
    CL_Obj stream;
    CL_UNUSED(n);
    if (!CL_STREAM_P(listener))
        cl_error(CL_ERR_TYPE,
                 "EXT:SOCKET-ACCEPT: argument must be a listening socket stream");
    stream = cl_socket_stream_accept(listener);
    if (CL_NULL_P(stream))
        cl_error(CL_ERR_GENERAL,
                 "EXT:SOCKET-ACCEPT: accept failed (argument is not an open "
                 "listening socket stream)");
    return stream;
}

/* (ext:socket-local-port listener) => port integer
 * Returns the local TCP port a listening socket stream is bound to.  This is
 * the only way to learn the port after (ext:socket-listen 0), which lets the
 * OS assign an ephemeral port.  Signals an error if the argument is not an
 * open listening socket stream. */
static CL_Obj bi_socket_local_port(CL_Obj *args, int n)
{
    int port;
    CL_UNUSED(n);
    port = cl_listen_stream_local_port(args[0]);
    if (port < 0)
        cl_error(CL_ERR_TYPE,
                 "EXT:SOCKET-LOCAL-PORT: argument must be an open listening "
                 "socket stream");
    return CL_MAKE_FIXNUM(port);
}

/* --- UDP datagram socket streams ---
 *
 * Message-oriented I/O for KNXnet/IP-style protocols (usocket's
 * datagram-usocket maps onto these).  The stream is a CL_STREAM_SOCKET with
 * CL_STREAM_FLAG_DGRAM; CLOSE and the socket timeout accessors work on it,
 * while character/byte stream I/O is rejected by the type checks below. */

static CL_Stream *check_udp_stream(CL_Obj obj, const char *who)
{
    CL_Stream *st;
    if (!CL_STREAM_P(obj))
        cl_error(CL_ERR_TYPE, "%s: argument must be a UDP socket stream", who);
    st = (CL_Stream *)CL_OBJ_TO_PTR(obj);
    if (st->stream_type != CL_STREAM_SOCKET || !(st->flags & CL_STREAM_FLAG_DGRAM))
        cl_error(CL_ERR_TYPE, "%s: argument must be a UDP socket stream "
                 "(see EXT:OPEN-UDP-STREAM)", who);
    if (!(st->flags & CL_STREAM_FLAG_OPEN))
        cl_error(CL_ERR_GENERAL, "%s: stream is closed", who);
    return st;
}

/* Validate an octet buffer argument — a simple general vector OR a packed
 * (unsigned-byte 8)/(signed-byte 8) byte vector — and return its usable
 * length (fill pointer if active, else total length) plus its total
 * capacity.  Exactly one of *V_OUT / *BV_OUT is set.  Displaced/multi-dim
 * general vectors are rejected — the send/receive paths index the element
 * storage directly. */
static void check_octet_vector(CL_Obj obj, const char *who, uint32_t *len_out,
                               uint32_t *cap_out,
                               CL_Vector **v_out, CL_ByteVector **bv_out)
{
    *v_out = NULL;
    *bv_out = NULL;
    if (CL_BYTE_VECTOR_P(obj)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
        if (bv->elt_shift)
            cl_error(CL_ERR_TYPE,
                     "%s: buffer must be an octet vector - a packed "
                     "(UNSIGNED-BYTE 16)/(SIGNED-BYTE 16) vector holds "
                     "2-byte elements, not octets", who);
        *bv_out = bv;
        *len_out = cl_bytevec_active_length(bv);
        *cap_out = bv->length;
        return;
    }
    if (!CL_VECTOR_P(obj))
        cl_error(CL_ERR_TYPE, "%s: buffer must be a vector", who);
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        if (v->flags & (CL_VEC_FLAG_DISPLACED | CL_VEC_FLAG_MULTIDIM | CL_VEC_FLAG_STRING))
            cl_error(CL_ERR_TYPE,
                     "%s: buffer must be a simple (non-displaced) octet vector", who);
        *len_out = (v->fill_pointer != CL_NO_FILL_POINTER) ? v->fill_pointer : v->length;
        *cap_out = v->length;
        *v_out = v;
    }
}

/* (ext:open-udp-stream host port) => stream
 * Opens a connected UDP (datagram) socket to host:port and returns a socket
 * stream for it.  I/O goes through EXT:UDP-STREAM-SEND / -RECEIVE (one call
 * per datagram); close with CL:CLOSE.  Read/write timeouts set via
 * (SETF EXT:SOCKET-STREAM-TIMEOUT) apply to the datagram operations. */
static CL_Obj bi_open_udp_stream(CL_Obj *args, int n)
{
    CL_String *host_str;
    int port;
    CL_Obj stream;
    char host_buf[256];
    uint32_t hl;
    CL_UNUSED(n);
    if (!CL_STRING_P(args[0]))
        cl_error(CL_ERR_TYPE, "EXT:OPEN-UDP-STREAM: host must be a string");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "EXT:OPEN-UDP-STREAM: port must be an integer");
    host_str = (CL_String *)CL_OBJ_TO_PTR(args[0]);
    port = CL_FIXNUM_VAL(args[1]);
    if (port < 1 || port > 65535)
        cl_error(CL_ERR_GENERAL, "EXT:OPEN-UDP-STREAM: port must be 1-65535");
    /* Copy the hostname off the arena — cl_make_udp_socket_stream allocates
     * (and DNS parks in a safe region), so host_str->data can move. */
    hl = host_str->length;
    if (hl >= sizeof(host_buf))
        cl_error(CL_ERR_GENERAL,
                 "EXT:OPEN-UDP-STREAM: hostname too long (%u bytes, max %u)",
                 (unsigned)hl, (unsigned)sizeof(host_buf) - 1);
    memcpy(host_buf, host_str->data, hl);
    host_buf[hl] = '\0';
    stream = cl_make_udp_socket_stream(host_buf, port);
    if (CL_NULL_P(stream))
        cl_error(CL_ERR_GENERAL,
                 "EXT:OPEN-UDP-STREAM: failed to open UDP socket to %s:%d "
                 "(host unresolvable or no route)", host_buf, port);
    return stream;
}

#define UDP_STACK_BUF 2048

/* (ext:udp-stream-send stream buffer &optional length) => t
 * Sends the first LENGTH octets of BUFFER (defaults to its fill pointer /
 * length) as ONE datagram on the connected UDP stream. */
static CL_Obj bi_udp_stream_send(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_Vector *v;
    CL_ByteVector *bv;
    uint32_t len, cap, i;
    uint32_t handle, wtmo;
    uint8_t sbuf[UDP_STACK_BUF];
    uint8_t *buf;
    int rc;
    st = check_udp_stream(args[0], "EXT:UDP-STREAM-SEND");
    check_octet_vector(args[1], "EXT:UDP-STREAM-SEND", &len, &cap, &v, &bv);
    if (n >= 3 && !CL_NULL_P(args[2])) {
        if (!CL_FIXNUM_P(args[2]) || CL_FIXNUM_VAL(args[2]) < 0)
            cl_error(CL_ERR_TYPE,
                     "EXT:UDP-STREAM-SEND: length must be a non-negative integer");
        if ((uint32_t)CL_FIXNUM_VAL(args[2]) > cap)
            cl_error(CL_ERR_GENERAL,
                     "EXT:UDP-STREAM-SEND: length %ld exceeds buffer size %u",
                     (long)CL_FIXNUM_VAL(args[2]), (unsigned)cap);
        len = (uint32_t)CL_FIXNUM_VAL(args[2]);
    }
    if (len > 65507)
        cl_error(CL_ERR_GENERAL,
                 "EXT:UDP-STREAM-SEND: %u octets exceed the maximum UDP "
                 "datagram size (65507)", (unsigned)len);
    buf = (len <= UDP_STACK_BUF) ? sbuf : (uint8_t *)malloc(len);
    if (!buf)
        cl_error(CL_ERR_GENERAL, "EXT:UDP-STREAM-SEND: out of memory");
    if (bv) {
        /* Packed byte vector: the datagram payload is the raw bytes. */
        memcpy(buf, bv->data, len);
    } else {
        for (i = 0; i < len; i++) {
            CL_Obj e = v->data[i];
            if (!CL_FIXNUM_P(e) || CL_FIXNUM_VAL(e) < 0 || CL_FIXNUM_VAL(e) > 255) {
                if (buf != sbuf) free(buf);
                cl_error(CL_ERR_TYPE,
                         "EXT:UDP-STREAM-SEND: buffer element %u is not an octet (0-255)",
                         (unsigned)i);
            }
            buf[i] = (uint8_t)CL_FIXNUM_VAL(e);
        }
    }
    /* Capture the handle + timeout before the platform call: the send parks
     * in a GC safe region, and compaction can move the stream object. */
    handle = st->handle_id;
    wtmo = st->write_timeout_ms;
    rc = platform_udp_send((PlatformSocket)handle, buf, len);
    if (buf != sbuf) free(buf);
    if (rc == PLATFORM_SOCKET_TIMEOUT)
        cl_error(CL_ERR_TIMEOUT,
                 "EXT:UDP-STREAM-SEND: send timed out after %u ms", (unsigned)wtmo);
    if (rc != 0)
        cl_error(CL_ERR_GENERAL, "EXT:UDP-STREAM-SEND: send failed");
    return CL_T;
}

/* (ext:udp-stream-receive stream buffer &optional max-length) => length
 * Blocks until one datagram arrives on the connected UDP stream, stores its
 * octets into BUFFER (up to MAX-LENGTH, defaulting to the buffer size) and
 * returns the datagram length.  A read timeout set via
 * (SETF EXT:SOCKET-STREAM-TIMEOUT) raises EXT:SOCKET-TIMEOUT. */
static CL_Obj bi_udp_stream_receive(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_Vector *v;
    CL_ByteVector *bv;
    uint32_t maxlen, cap, i;
    uint32_t handle, rtmo;
    uint8_t sbuf[UDP_STACK_BUF];
    uint8_t *buf;
    int rc;
    st = check_udp_stream(args[0], "EXT:UDP-STREAM-RECEIVE");
    check_octet_vector(args[1], "EXT:UDP-STREAM-RECEIVE", &maxlen, &cap, &v, &bv);
    maxlen = cap;   /* capacity, not fill pointer, bounds a receive */
    if (n >= 3 && !CL_NULL_P(args[2])) {
        if (!CL_FIXNUM_P(args[2]) || CL_FIXNUM_VAL(args[2]) < 0)
            cl_error(CL_ERR_TYPE,
                     "EXT:UDP-STREAM-RECEIVE: max-length must be a non-negative integer");
        if ((uint32_t)CL_FIXNUM_VAL(args[2]) < maxlen)
            maxlen = (uint32_t)CL_FIXNUM_VAL(args[2]);
    }
    buf = (maxlen <= UDP_STACK_BUF) ? sbuf : (uint8_t *)malloc(maxlen ? maxlen : 1);
    if (!buf)
        cl_error(CL_ERR_GENERAL, "EXT:UDP-STREAM-RECEIVE: out of memory");
    /* Capture handle + timeout first: the blocking recv parks in a GC safe
     * region and compaction can move the stream object meanwhile. */
    handle = st->handle_id;
    rtmo = st->read_timeout_ms;
    rc = platform_udp_recv((PlatformSocket)handle, buf, maxlen);
    if (rc == PLATFORM_SOCKET_TIMEOUT) {
        if (buf != sbuf) free(buf);
        cl_error(CL_ERR_TIMEOUT,
                 "EXT:UDP-STREAM-RECEIVE: no datagram within %u ms", (unsigned)rtmo);
    }
    if (rc < 0) {
        if (buf != sbuf) free(buf);
        cl_error(CL_ERR_GENERAL,
                 "EXT:UDP-STREAM-RECEIVE: receive failed (socket closed?)");
    }
    /* GC SAFETY: re-derive the vector pointer — the safe region above may
     * have let a compaction move it (args[] itself is GC-rooted). */
    if (bv) {
        bv = (CL_ByteVector *)CL_OBJ_TO_PTR(args[1]);
        memcpy(bv->data, buf, (size_t)rc);
    } else {
        v = (CL_Vector *)CL_OBJ_TO_PTR(args[1]);
        for (i = 0; i < (uint32_t)rc; i++)
            v->data[i] = CL_MAKE_FIXNUM(buf[i]);
    }
    if (buf != sbuf) free(buf);
    return CL_MAKE_FIXNUM(rc);
}

/* (ext:socket-stream-local-endpoint stream) => ip-string, port
 * Local address/port of a connected TCP or UDP socket stream (getsockname).
 * usocket's get-local-name/get-local-port build on this — KNXnet/IP needs
 * the real local endpoint for its HPAI structures. */
static CL_Obj bi_socket_stream_local_endpoint(CL_Obj *args, int n)
{
    CL_Stream *st;
    char ip[16];
    int port = 0;
    uint32_t handle;
    CL_Obj ip_obj;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "EXT:SOCKET-STREAM-LOCAL-ENDPOINT: argument must be a socket stream");
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->stream_type != CL_STREAM_SOCKET || !(st->flags & CL_STREAM_FLAG_OPEN))
        cl_error(CL_ERR_TYPE,
                 "EXT:SOCKET-STREAM-LOCAL-ENDPOINT: argument must be an open socket stream");
    handle = st->handle_id;
    if (platform_socket_local_endpoint((PlatformSocket)handle, ip, &port) != 0)
        cl_error(CL_ERR_GENERAL,
                 "EXT:SOCKET-STREAM-LOCAL-ENDPOINT: getsockname failed");
    ip_obj = cl_make_string(ip, (uint32_t)strlen(ip));
    cl_mv_count = 2;
    cl_mv_values[0] = ip_obj;
    cl_mv_values[1] = CL_MAKE_FIXNUM(port);
    return ip_obj;
}

/* Decode a :INPUT / :OUTPUT / :IO direction keyword into which-timeout flags.
 * *do_read and *do_write are set accordingly; signals a type error otherwise. */
static void socket_timeout_dir(CL_Obj dir, const char *fn, int *do_read, int *do_write)
{
    const char *name = NULL;
    *do_read = 0;
    *do_write = 0;
    if (CL_SYMBOL_P(dir)) {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(dir);
        if (CL_STRING_P(s->name))
            name = ((CL_String *)CL_OBJ_TO_PTR(s->name))->data;
    }
    if (name && strcmp(name, "INPUT") == 0)       { *do_read = 1; }
    else if (name && strcmp(name, "OUTPUT") == 0) { *do_write = 1; }
    else if (name && strcmp(name, "IO") == 0)     { *do_read = 1; *do_write = 1; }
    else
        cl_error(CL_ERR_TYPE, "%s: direction must be :INPUT, :OUTPUT or :IO", fn);
}

/* (ext:socket-stream-timeout stream direction) => seconds-or-NIL
 * Returns the read (:INPUT) or write (:OUTPUT) timeout of a socket stream in
 * seconds, or NIL when no timeout is set.  :IO returns the read timeout.
 * Settable via (setf (ext:socket-stream-timeout stream direction) seconds). */
static CL_Obj bi_socket_stream_timeout(CL_Obj *args, int n)
{
    int do_read, do_write;
    uint32_t ms;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "EXT:SOCKET-STREAM-TIMEOUT: not a stream");
    socket_timeout_dir(args[1], "EXT:SOCKET-STREAM-TIMEOUT", &do_read, &do_write);
    ms = cl_socket_stream_get_timeout(args[0], do_write && !do_read ? 1 : 0);
    if (ms == 0) return CL_NIL;
    return cl_make_double_float((double)ms / 1000.0);
}

/* (ext:%set-socket-stream-timeout stream direction seconds) => seconds
 * Setter behind (setf socket-stream-timeout).  `seconds` is a non-negative real
 * (fractional allowed) or NIL to clear.  Mirrors the value into the platform
 * socket layer so a blocked read/write gives up and raises EXT:SOCKET-TIMEOUT. */
static CL_Obj bi_set_socket_stream_timeout(CL_Obj *args, int n)
{
    int do_read, do_write;
    uint32_t ms;
    CL_Obj val = args[2];
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        cl_error(CL_ERR_TYPE, "(SETF EXT:SOCKET-STREAM-TIMEOUT): not a stream");
    socket_timeout_dir(args[1], "(SETF EXT:SOCKET-STREAM-TIMEOUT)", &do_read, &do_write);
    if (CL_NULL_P(val)) {
        ms = 0;
    } else {
        double secs;
        if (!CL_REALP(val))
            cl_error(CL_ERR_TYPE,
                     "(SETF EXT:SOCKET-STREAM-TIMEOUT): timeout must be a non-negative real or NIL");
        secs = cl_to_double(val);
        if (secs < 0.0)
            cl_error(CL_ERR_GENERAL,
                     "(SETF EXT:SOCKET-STREAM-TIMEOUT): timeout must be non-negative");
        ms = (uint32_t)(secs * 1000.0 + 0.5);
    }
    if (do_read)  cl_socket_stream_set_timeout(args[0], 0, ms);
    if (do_write) cl_socket_stream_set_timeout(args[0], 1, ms);
    return val;
}

/* --- TLS-upgraded socket streams ---
 *
 * TLS upgrades an existing TCP socket stream IN PLACE: platform_tls_start
 * hangs the TLS connection off the platform socket slot, after which the
 * ordinary byte/char stream paths (and CL:LISTEN) transparently carry
 * ciphertext.  The same stream object is returned, so wrappers holding it
 * (chunga, flexi-streams, usocket) keep working untouched.  The keyword
 * front-end is EXT:SOCKET-START-TLS (boot.lisp); the cl+ssl-compatible
 * facade for drakma/hunchentoot lives in lib/shims/cl+ssl. */

/* Validate a connected TCP socket stream argument. */
static CL_Stream *check_tls_stream(CL_Obj obj, const char *who)
{
    CL_Stream *st;
    if (!CL_STREAM_P(obj))
        cl_error(CL_ERR_TYPE, "%s: argument must be a socket stream", who);
    st = (CL_Stream *)CL_OBJ_TO_PTR(obj);
    if (st->stream_type != CL_STREAM_SOCKET || (st->flags & CL_STREAM_FLAG_DGRAM))
        cl_error(CL_ERR_TYPE, "%s: argument must be a TCP socket stream "
                 "(see EXT:OPEN-TCP-STREAM / EXT:SOCKET-ACCEPT)", who);
    if (!(st->flags & CL_STREAM_FLAG_OPEN))
        cl_error(CL_ERR_GENERAL, "%s: stream is closed", who);
    return st;
}

/* Copy a string argument into a fixed C buffer, or return NULL for NIL.
 * TLS parameter strings must not point into the movable Lisp arena: the
 * handshake below parks in a GC safe region, and a peer thread's compaction
 * would leave arena pointers stale (same discipline as bi_open_tcp_stream's
 * hostname copy). */
static const char *tls_string_arg(CL_Obj obj, char *buf, uint32_t bufsize,
                                  const char *who, const char *what)
{
    CL_String *s;
    if (CL_NULL_P(obj)) return NULL;
    if (!CL_STRING_P(obj))
        cl_error(CL_ERR_TYPE, "%s: %s must be a string or NIL", who, what);
    s = (CL_String *)CL_OBJ_TO_PTR(obj);
    if (s->length >= bufsize)
        cl_error(CL_ERR_GENERAL, "%s: %s too long (%u bytes, max %u)",
                 who, what, (unsigned)s->length, (unsigned)(bufsize - 1));
    memcpy(buf, s->data, s->length);
    buf[s->length] = '\0';
    return buf;
}

/* (ext:tls-available-p) => generalized boolean
 * True when a TLS provider is present (OpenSSL on the host, AmiSSL on
 * AmigaOS, openssl3.library on MorphOS).  The provider is loaded lazily on
 * the first call; a NIL answer means EXT:SOCKET-START-TLS cannot work. */
static CL_Obj bi_tls_available_p(CL_Obj *args, int n)
{
    CL_UNUSED(args); CL_UNUSED(n);
    return platform_tls_available() ? CL_T : CL_NIL;
}

/* (ext:tls-version) => string or NIL
 * Provider identification ("OpenSSL 3.2.1 ...", "AmiSSL 5.21"), or NIL when
 * no TLS provider is available. */
static CL_Obj bi_tls_version(CL_Obj *args, int n)
{
    const char *v = platform_tls_version();
    CL_UNUSED(args); CL_UNUSED(n);
    if (!v) return CL_NIL;
    return cl_make_string(v, (uint32_t)strlen(v));
}

/* (ext:socket-tls-p stream) => generalized boolean
 * True when `stream` is a socket stream already upgraded to TLS. */
static CL_Obj bi_socket_tls_p(CL_Obj *args, int n)
{
    CL_Stream *st;
    CL_UNUSED(n);
    if (!CL_STREAM_P(args[0]))
        return CL_NIL;
    st = (CL_Stream *)CL_OBJ_TO_PTR(args[0]);
    if (st->stream_type != CL_STREAM_SOCKET)
        return CL_NIL;
    return platform_tls_active((PlatformSocket)st->handle_id) ? CL_T : CL_NIL;
}

/* (ext:%socket-start-tls stream server-p hostname verify certificate key
 *                        key-password ca-file ca-path timeout-seconds)
 * => stream
 * Positional core behind the EXT:SOCKET-START-TLS keyword wrapper
 * (boot.lisp).  Upgrades the connected TCP socket stream to TLS in place
 * and returns it; signals an error with the provider's diagnostic when the
 * handshake (or certificate verification) fails. */
static CL_Obj bi_socket_start_tls(CL_Obj *args, int n)
{
    static const char *who = "EXT:SOCKET-START-TLS";
    CL_Stream *st = check_tls_stream(args[0], "EXT:SOCKET-START-TLS");
    PlatformSocket sh = (PlatformSocket)st->handle_id;
    PlatformTLSParams p;
    char host_buf[256], cert_buf[1024], key_buf[1024], pw_buf[256];
    char ca_file_buf[1024], ca_path_buf[1024];
    char err[512];
#define TLS_ARG(i) ((i) < n ? args[(i)] : CL_NIL)
    memset(&p, 0, sizeof(p));
    p.server       = CL_NULL_P(TLS_ARG(1)) ? 0 : 1;
    p.hostname     = tls_string_arg(TLS_ARG(2), host_buf, sizeof(host_buf),
                                    who, "hostname");
    p.verify       = CL_NULL_P(TLS_ARG(3)) ? 0 : 1;
    p.cert_file    = tls_string_arg(TLS_ARG(4), cert_buf, sizeof(cert_buf),
                                    who, "certificate path");
    p.key_file     = tls_string_arg(TLS_ARG(5), key_buf, sizeof(key_buf),
                                    who, "key path");
    p.key_password = tls_string_arg(TLS_ARG(6), pw_buf, sizeof(pw_buf),
                                    who, "key password");
    p.ca_file      = tls_string_arg(TLS_ARG(7), ca_file_buf, sizeof(ca_file_buf),
                                    who, "ca-file path");
    p.ca_path      = tls_string_arg(TLS_ARG(8), ca_path_buf, sizeof(ca_path_buf),
                                    who, "ca-path");
    if (!CL_NULL_P(TLS_ARG(9))) {
        double secs;
        if (!CL_REALP(args[9]))
            cl_error(CL_ERR_TYPE,
                     "%s: timeout must be a non-negative real or NIL", who);
        secs = cl_to_double(args[9]);
        if (secs < 0.0)
            cl_error(CL_ERR_GENERAL, "%s: timeout must be non-negative", who);
        p.timeout_ms = (secs * 1000.0 >= (double)INT_MAX)
                       ? INT_MAX : (int)(secs * 1000.0 + 0.5);
    }
#undef TLS_ARG
    if (platform_tls_start(sh, &p, err, sizeof(err)) != 0)
        cl_error(CL_ERR_GENERAL, "%s: %s", who, err);
    return args[0];
}

/* (ext:tls-peer-certificate stream) => plist or NIL
 * Peer certificate of a TLS-upgraded socket stream as a plist
 * (:subject "..." :issuer "..." :not-before "..." :not-after "..."), with
 * absent fields omitted.  NIL when the stream is not TLS or the peer
 * presented no certificate (e.g. a server looking at a plain client). */
static CL_Obj bi_tls_peer_certificate(CL_Obj *args, int n)
{
    /* Reverse order: consing prepends, so the result reads
     * subject, issuer, not-before, not-after. */
    static const struct { int field; const char *kw; uint32_t kwlen; } fields[4] = {
        { PLATFORM_TLS_CERT_NOT_AFTER,  "NOT-AFTER",  9  },
        { PLATFORM_TLS_CERT_NOT_BEFORE, "NOT-BEFORE", 10 },
        { PLATFORM_TLS_CERT_ISSUER,     "ISSUER",     6  },
        { PLATFORM_TLS_CERT_SUBJECT,    "SUBJECT",    7  },
    };
    CL_Stream *st = check_tls_stream(args[0], "EXT:TLS-PEER-CERTIFICATE");
    PlatformSocket sh = (PlatformSocket)st->handle_id;
    CL_Obj result = CL_NIL, item;
    char buf[512];
    int i;
    CL_UNUSED(n);
    if (!platform_tls_active(sh))
        return CL_NIL;
    /* GC SAFETY: each cl_cons can compact — the accumulating list must stay
     * forwarded across iterations. */
    CL_GC_PROTECT(result);
    for (i = 0; i < 4; i++) {
        if (platform_tls_peer_cert_field(sh, fields[i].field,
                                         buf, sizeof(buf)) == 0) {
            item = cl_make_string(buf, (uint32_t)strlen(buf));
            result = cl_cons(item, result);
            item = cl_intern_keyword(fields[i].kw, fields[i].kwlen);
            result = cl_cons(item, result);
        }
    }
    CL_GC_UNPROTECT(1);
    return result;
}

/* --- Registration --- */

void cl_builtins_io_init(void)
{
    /* Intern EVAL-WHEN situation keywords (CLHS 3.2.3.1 / Table 3-9) */
    KW_EW_CT       = cl_intern_keyword("COMPILE-TOPLEVEL", 16);
    KW_EW_LT       = cl_intern_keyword("LOAD-TOPLEVEL", 13);
    KW_EW_EX       = cl_intern_keyword("EXECUTE", 7);
    SYM_EW_COMPILE = cl_intern_in("COMPILE", 7, cl_package_cl);
    SYM_EW_LOAD    = cl_intern_in("LOAD", 4, cl_package_cl);
    SYM_EW_EVAL_SYM = cl_intern_in("EVAL", 4, cl_package_cl);

    /* Intern keywords for WRITE */
    KW_WR_STREAM   = cl_intern_keyword("STREAM", 6);
    KW_WR_ESCAPE   = cl_intern_keyword("ESCAPE", 6);
    KW_WR_READABLY = cl_intern_keyword("READABLY", 8);
    KW_WR_BASE     = cl_intern_keyword("BASE", 4);
    KW_WR_RADIX    = cl_intern_keyword("RADIX", 5);
    KW_WR_LEVEL    = cl_intern_keyword("LEVEL", 5);
    KW_WR_LENGTH   = cl_intern_keyword("LENGTH", 6);
    KW_WR_CASE     = cl_intern_keyword("CASE", 4);
    KW_WR_GENSYM   = cl_intern_keyword("GENSYM", 6);
    KW_WR_ARRAY    = cl_intern_keyword("ARRAY", 5);
    KW_WR_CIRCLE   = cl_intern_keyword("CIRCLE", 6);
    KW_WR_PRETTY   = cl_intern_keyword("PRETTY", 6);
    KW_WR_RIGHT_MARGIN = cl_intern_keyword("RIGHT-MARGIN", 12);
    KW_WR_PPRINT_DISPATCH = cl_intern_keyword("PPRINT-DISPATCH", 15);

    /* I/O */
    defun("WRITE", bi_write, 1, -1);
    defun("PRINT", bi_print, 1, 2);
    defun("PRIN1", bi_prin1, 1, 2);
    defun("PRINC", bi_princ, 1, 2);
    defun("PPRINT", bi_pprint, 1, 2);
    defun("FORMAT", bi_format, 1, -1);
    cl_format_builtins_init();  /* registers CLAMIGA::%FORMATTER-INNER */

    /* Read / Load / Eval */
    defun("READ", bi_read, 0, -1);
    defun("READ-DELIMITED-LIST", bi_read_delimited_list, 1, 3);
    defun("LOAD", bi_load, 1, -1);  /* &key :verbose :print :if-does-not-exist :external-format */
    defun("EVAL", bi_eval, 1, 1);
    defun("MACROEXPAND-1", bi_macroexpand_1, 1, 2);
    defun("MACROEXPAND", bi_macroexpand, 1, 2);
    defun("MACRO-FUNCTION", bi_macro_function, 1, 2);
    cl_register_builtin("%SETF-MACRO-FUNCTION", bi_set_macro_function, 2, 2, cl_package_clamiga);
    defun("COMPILER-MACRO-FUNCTION", bi_compiler_macro_function, 1, 2);
    cl_register_builtin("%SETF-COMPILER-MACRO-FUNCTION",
                        bi_set_compiler_macro_function, 2, 2, cl_package_clamiga);
    cl_register_builtin("%MACROEXPAND-ENV", bi_macroexpand_env, 0, 0, cl_package_clamiga);
    cl_register_builtin("%CALL-MACRO-EXPANDER", bi_call_macro_expander, 3, 3, cl_package_clamiga);

    /* Throw (ERROR is now in builtins_condition.c) */
    defun("THROW", bi_throw, 1, 2);

    /* Multiple values */
    defun("VALUES", bi_values, 0, -1);
    defun("VALUES-LIST", bi_values_list, 1, 1);

    /* Gensym / gentemp */
    defun("GENSYM", bi_gensym, 0, 1);
    defun("GENTEMP", bi_gentemp, 0, 2);

    /* Debugging */
    defun("DISASSEMBLE", bi_disassemble, 1, 1);

    /* Timing (internal helpers for TIME special form) */
    cl_register_builtin("%GET-INTERNAL-TIME", bi_get_internal_time, 0, 0, cl_package_clamiga);
    cl_register_builtin("%GET-RUN-TIME",      bi_get_run_time,      0, 0, cl_package_clamiga);
    cl_register_builtin("%BOOT-QUIET-P",      bi_boot_quiet_p,      0, 0, cl_package_clamiga);
    cl_register_builtin("%BOOT-TRACE-CLOS",   bi_boot_trace_clos,   3, 3, cl_package_clamiga);
    cl_register_builtin("%GET-BYTES-CONSED", bi_get_bytes_consed, 0, 0, cl_package_clamiga);
    cl_register_builtin("%GET-GC-COUNT", bi_get_gc_count, 0, 0, cl_package_clamiga);
    cl_register_builtin("%TIME-REPORT", bi_time_report, 3, 4, cl_package_clamiga);
    defun("GET-INTERNAL-REAL-TIME", bi_get_internal_real_time, 0, 0);
    defun("GET-INTERNAL-RUN-TIME", bi_get_internal_run_time, 0, 0);
    {
        /* INTERNAL-TIME-UNITS-PER-SECOND — milliseconds.  Per CLHS this is
         * a constant, so set CL_SYM_CONSTANT for boundp/constantp/setf to
         * behave correctly. */
        CL_Obj sym = cl_intern_in("INTERNAL-TIME-UNITS-PER-SECOND", 30, cl_package_cl);
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        s->value = CL_MAKE_FIXNUM(1000);
        s->flags |= CL_SYM_CONSTANT;
    }

    /* Sleep */
    defun("SLEEP", bi_sleep, 1, 1);

    /* Compile */
    defun("COMPILE", bi_compile, 1, 2);
    defun("COMPILE-FILE", bi_compile_file, 1, -1);
    defun("COMPILE-FILE-PATHNAME", bi_compile_file_pathname, 1, -1);

    /* Modules */
    defun("PROVIDE", bi_provide, 1, 1);
    defun("REQUIRE", bi_require, 1, 2);

    /* Environment */
    defun("LISP-IMPLEMENTATION-TYPE", bi_lisp_implementation_type, 0, 0);
    defun("LISP-IMPLEMENTATION-VERSION", bi_lisp_implementation_version, 0, 0);
    defun("SOFTWARE-TYPE", bi_software_type, 0, 0);
    defun("SOFTWARE-VERSION", bi_software_version, 0, 0);
    defun("MACHINE-TYPE", bi_machine_type, 0, 0);
    defun("MACHINE-VERSION", bi_machine_version, 0, 0);
    defun("MACHINE-INSTANCE", bi_machine_instance, 0, 0);
    /* Extension functions (EXT package) */
    extfun("GC", bi_ext_gc, 0, 0);
    extfun("GC-COMPACT", bi_ext_gc_compact, 0, 0);
    extfun("%GC-MARK-STATS", bi_ext_gc_mark_stats, 0, 0);
    extfun("%GC-FWD-BYTES", bi_ext_gc_fwd_bytes, 0, 0);
    extfun("%BYTECODE-OFFHEAP-STATS", bi_ext_bytecode_offheap_stats, 0, 0);
    extfun("%FASL-REGISTRY-STATS", bi_ext_fasl_registry_stats, 0, 0);
    extfun("%COMPILER-POOL-STATS", bi_ext_compiler_pool_stats, 0, 0);
    extfun("%GC-TIME-STATS", bi_ext_gc_time_stats, 0, 0);
    extfun("%HEAP-STATS", bi_ext_heap_stats, 0, 0);
#ifdef CL_GENGC
    extfun("%GENGC-STATS", bi_ext_gengc_stats, 0, 0);
    extfun("%GC-MINOR", bi_ext_gc_minor, 0, 0);
#endif
    extfun("%STREAM-OUTBUF-STATS", bi_ext_stream_outbuf_stats, 0, 0);
    extfun("%GC-AUDIT-ROOTS", bi_ext_gc_audit_roots, 0, 0);
    extfun("%GC-AUDIT-HDR-INDEX", bi_ext_gc_audit_hdr_index, 0, 0);
    extfun("%HEAP-VERIFY", bi_ext_heap_verify, 0, 0);
    extfun("%CPU-STORE-SELFTEST", bi_ext_cpu_store_selftest, 0, 1);
    extfun("GETENV", bi_getenv, 1, 1);
    extfun("EXECUTABLE-PATH", bi_executable_path, 0, 0);
    extfun("UNPACK-BYTERUN1", bi_unpack_byterun1, 5, 6);
    extfun("COPY-ROWS", bi_copy_rows, 8, 8);
    extfun("GETCWD", bi_getcwd, 0, 0);
    extfun("TTY-P", bi_ext_tty_p, 0, 0);
    extfun("TTY-RAW-MODE", bi_ext_tty_raw_mode, 1, 1);
    extfun("TTY-SIZE", bi_ext_tty_size, 0, 0);
    extfun("SYSTEM-COMMAND", bi_system_command, 1, 1);
    extfun("OPEN-TCP-STREAM", bi_open_tcp_stream, 2, 3);
    extfun("SOCKET-LISTEN", bi_socket_listen, 1, 2);
    extfun("SOCKET-ACCEPT", bi_socket_accept, 1, 1);
    extfun("SOCKET-LOCAL-PORT", bi_socket_local_port, 1, 1);
    extfun("OPEN-UDP-STREAM", bi_open_udp_stream, 2, 2);
    extfun("UDP-STREAM-SEND", bi_udp_stream_send, 2, 3);
    extfun("UDP-STREAM-RECEIVE", bi_udp_stream_receive, 2, 3);
    extfun("SOCKET-STREAM-LOCAL-ENDPOINT", bi_socket_stream_local_endpoint, 1, 1);
    extfun("SOCKET-STREAM-TIMEOUT", bi_socket_stream_timeout, 2, 2);
    extfun("%SET-SOCKET-STREAM-TIMEOUT", bi_set_socket_stream_timeout, 3, 3);
    extfun("TLS-AVAILABLE-P", bi_tls_available_p, 0, 0);
    extfun("TLS-VERSION", bi_tls_version, 0, 0);
    extfun("SOCKET-TLS-P", bi_socket_tls_p, 1, 1);
    extfun("%SOCKET-START-TLS", bi_socket_start_tls, 1, 10);
    extfun("TLS-PEER-CERTIFICATE", bi_tls_peer_certificate, 1, 1);

    /* Pretty-printing keywords */
    KW_LINEAR    = cl_intern_keyword("LINEAR", 6);
    KW_FILL      = cl_intern_keyword("FILL", 4);
    KW_MISER     = cl_intern_keyword("MISER", 5);
    KW_MANDATORY = cl_intern_keyword("MANDATORY", 9);
    KW_BLOCK     = cl_intern_keyword("BLOCK", 5);
    KW_CURRENT   = cl_intern_keyword("CURRENT", 7);

    /* Pretty-printing builtins */
    defun("PPRINT-NEWLINE", bi_pprint_newline, 1, 2);
    defun("PPRINT-INDENT", bi_pprint_indent, 2, 3);

    /* Internal builtins for pprint-logical-block */
    cl_register_builtin("%PP-PUSH-BLOCK", bi_pp_push_block, 0, 0, cl_package_clamiga);
    cl_register_builtin("%PP-POP-BLOCK", bi_pp_pop_block, 0, 0, cl_package_clamiga);

    /* Pprint-dispatch table */
    defun("SET-PPRINT-DISPATCH", bi_set_pprint_dispatch, 2, 4);
    defun("PPRINT-DISPATCH", bi_pprint_dispatch, 1, 2);
    defun("COPY-PPRINT-DISPATCH", bi_copy_pprint_dispatch, 0, 1);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&KW_EW_CT);
    cl_gc_register_root(&KW_EW_LT);
    cl_gc_register_root(&KW_EW_EX);
    cl_gc_register_root(&SYM_EW_COMPILE);
    cl_gc_register_root(&SYM_EW_LOAD);
    cl_gc_register_root(&SYM_EW_EVAL_SYM);
    cl_gc_register_root(&KW_WR_STREAM);
    cl_gc_register_root(&KW_WR_ESCAPE);
    cl_gc_register_root(&KW_WR_READABLY);
    cl_gc_register_root(&KW_WR_BASE);
    cl_gc_register_root(&KW_WR_RADIX);
    cl_gc_register_root(&KW_WR_LEVEL);
    cl_gc_register_root(&KW_WR_LENGTH);
    cl_gc_register_root(&KW_WR_CASE);
    cl_gc_register_root(&KW_WR_GENSYM);
    cl_gc_register_root(&KW_WR_ARRAY);
    cl_gc_register_root(&KW_WR_CIRCLE);
    cl_gc_register_root(&KW_WR_PRETTY);
    cl_gc_register_root(&KW_WR_RIGHT_MARGIN);
    cl_gc_register_root(&KW_WR_PPRINT_DISPATCH);
    cl_gc_register_root(&KW_LINEAR);
    cl_gc_register_root(&KW_FILL);
    cl_gc_register_root(&KW_MISER);
    cl_gc_register_root(&KW_MANDATORY);
    cl_gc_register_root(&KW_BLOCK);
    cl_gc_register_root(&KW_CURRENT);
}
