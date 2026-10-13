/*
 * image.c — heap image save/restore (EXT:SAVE-IMAGE / --image).
 *
 * Design: specs/image-save-load.md.  The load-bearing invariant is
 * "restore set == GC root set": the compacting GC already forces every
 * C location that holds a CL_Obj across a collection to be registered
 * (global root registry, the named shared tables in gc_mark, the
 * readtable pool, per-thread state), so the image writer dumps exactly
 * that enumeration plus the raw arena payload, and the loader overwrites
 * the same enumeration in a freshly-initialized process of the SAME
 * build.  Heap references are arena-relative offsets, so the payload
 * needs no relocation; the only per-process fixups are C pointers
 * (builtin CFuncs, bytecode side buffers, OS handles), handled by one
 * linear relink walk.
 */

#include "image.h"
#include "mem.h"
#include "gc_backend.h"   /* gcb_before_image_save */
#include "error.h"
#include "symbol.h"
#include "package.h"
#include "bindtab.h"      /* :SHAKE-BINDINGS sheds the binding tables */
#include "stream.h"
#include "reader.h"
#include "readtable.h"
#include "compiler.h"     /* cl_intern_source_file */
#include "vm.h"
#include "builtins.h"
#include "fasl.h"         /* CL_FASL_VERSION keys the bytecode encoding */
#include "string_utils.h"
#include "thread.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>   /* _exit — see image_fatal */

/* thread.h maps gc_root_count to (CT->gc_root_count) for legacy call
 * sites; this file walks explicit CL_Thread pointers, so the raw struct
 * member must stay addressable (same idiom as mem.c). */
#undef gc_root_count

/* The named shared globals gc_mark marks directly (mem.c keeps the same
 * extern list).  Dumped/restored in THIS order — it is part of the image
 * format, so a new shared global means a CL_IMAGE_VERSION bump. */
extern CL_Obj cl_package_registry;
extern CL_Obj macro_table, setf_table, setf_fn_table, setf_expander_table,
              type_table, compiler_macro_table;
extern CL_Obj cl_clos_class_table;
extern CL_Obj struct_table;
extern CL_Obj condition_hierarchy;
extern CL_Obj condition_slot_table;
extern CL_Obj condition_default_initargs;
extern CL_Obj condition_slot_initforms;

extern CL_Obj cl_main_thread_lisp_obj(void);
extern CL_Obj *cl_main_thread_lisp_obj_ptr(void);
extern CL_Thread *cl_main_thread_ptr;

#define N_IMAGE_NAMED_ROOTS 13

static CL_Obj *image_named_root(int i)
{
    switch (i) {
    case 0:  return &cl_package_registry;
    case 1:  return &macro_table;
    case 2:  return &setf_table;
    case 3:  return &setf_fn_table;
    case 4:  return &setf_expander_table;
    case 5:  return &type_table;
    case 6:  return &compiler_macro_table;
    case 7:  return &cl_clos_class_table;
    case 8:  return &struct_table;
    case 9:  return &condition_hierarchy;
    case 10: return &condition_slot_table;
    case 11: return &condition_default_initargs;
    case 12: return &condition_slot_initforms;
    default: return NULL;
    }
}

/* ================================================================
 * Build fingerprint
 * ================================================================ */

static uint64_t fnv1a64(uint64_t h, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t fnv1a64_u32(uint64_t h, uint32_t v)
{
    uint8_t b[4];
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
    return fnv1a64(h, b, 4);
}

static int image_little_endian(void)
{
    uint16_t v = 1;
    return *(const uint8_t *)&v;
}

static uint16_t image_flags(void)
{
    uint16_t f = 0;
#ifdef CL_WIDE_STRINGS
    f |= CL_IMAGE_FLAG_WIDE;
#endif
#ifdef __HAVE_68881__
    f |= CL_IMAGE_FLAG_FPU;
#endif
    if (image_little_endian())
        f |= CL_IMAGE_FLAG_LE;
    return f;
}

static void image_fingerprint(uint8_t out[CL_IMAGE_FPRINT_LEN])
{
    /* Four FNV-1a-64 lanes over different aspects of the build.  Any
     * difference in version, bytecode/FASL format, alignment, struct
     * layout, type-tag set or feature flags flips at least one lane. */
    uint64_t h1 = 1469598103934665603ull;   /* versions + formats */
    uint64_t h2 = 1469598103934665603ull;   /* heap struct sizes */
    uint64_t h3 = 1469598103934665603ull;   /* ABI + features */
    uint64_t h4;
    int i;

    h1 = fnv1a64(h1, CL_VERSION_STRING_FULL, CL_VERSION_LEN_FULL);
    h1 = fnv1a64_u32(h1, CL_FASL_VERSION);
    h1 = fnv1a64_u32(h1, CL_IMAGE_VERSION);

    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Cons));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Symbol));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_String));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Function));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Bytecode));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Closure));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Vector));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Package));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Hashtable));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Condition));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Struct));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Bignum));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Ratio));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Complex));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Stream));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_RandomState));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_BitVector));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Pathname));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Cell));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_ThreadObj));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Lock));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_CondVar));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_ForeignPtr));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_Restart));
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_ByteVector));
#ifdef CL_WIDE_STRINGS
    h2 = fnv1a64_u32(h2, (uint32_t)sizeof(CL_WideString));
#endif

    h3 = fnv1a64_u32(h3, CL_ALIGN);
    h3 = fnv1a64_u32(h3, (uint32_t)sizeof(void *));
    h3 = fnv1a64_u32(h3, (uint32_t)image_little_endian());
    h3 = fnv1a64_u32(h3, (uint32_t)CL_TYPE_MAX);
    h3 = fnv1a64_u32(h3, (uint32_t)image_flags());
#ifdef CL_GENGC
    h3 = fnv1a64_u32(h3, 1u);
#else
    h3 = fnv1a64_u32(h3, 0u);
#endif
#ifdef CL_TLAB
    h3 = fnv1a64_u32(h3, 1u);
#else
    h3 = fnv1a64_u32(h3, 0u);
#endif
#if defined(PLATFORM_MORPHOS)
    h3 = fnv1a64(h3, "morphos", 7);
#elif defined(PLATFORM_AMIGA)
    h3 = fnv1a64(h3, "amiga", 5);
#else
    h3 = fnv1a64(h3, "posix", 5);
#endif

    h4 = fnv1a64_u32(fnv1a64_u32(h1 ^ h2 ^ h3, (uint32_t)(h1 >> 32)),
                     (uint32_t)(h2 >> 32));

    for (i = 0; i < 8; i++) {
        out[i]      = (uint8_t)(h1 >> (i * 8));
        out[8 + i]  = (uint8_t)(h2 >> (i * 8));
        out[16 + i] = (uint8_t)(h3 >> (i * 8));
        out[24 + i] = (uint8_t)(h4 >> (i * 8));
    }
}

/* ================================================================
 * Module state
 * ================================================================ */

static int image_boot_roots = -1;          /* snapshot at end of C init */

/* Pending save (armed by EXT:SAVE-IMAGE, executed at a top-level safe
 * point where the main thread is at rest). */
static char image_pending_path[1024];
static int  image_pending = 0;
static int  image_pending_quit = 0;
static int  image_pending_shake = 0;
static int  image_pending_exe = 0;         /* :EXECUTABLE T */
static uint32_t image_pending_heap = 0;    /* :HEAP-SIZE (executables) */

/* Staged image awaiting restore.  Staging (before cl_mem_init) reads only
 * the header, which is all the arena sizing needs; the payload is read by
 * cl_image_restore_staged, AFTER the arena exists.  Reading the whole file
 * first parked a payload-sized block in the middle of free memory right
 * before the arena asked for its contiguous one: on a 24 MB 68020 the
 * 1.6 MB editor image left no 8 MB block where 11.8 MB had been free. */
static char          *image_staged_path = NULL;   /* platform_alloc'd */
static char          *image_staged_buf = NULL;    /* payload, once read */
static unsigned long  image_staged_size = 0;      /* image bytes at staging */
static uint32_t       image_staged_offset = 0;    /* where they start in the file */
static unsigned long  image_staged_file_len = 0;  /* the whole file's length */
/* The staged image is the one appended to the running executable (a
 * delivered executable, image.h), and the :HEAP-SIZE its trailer names. */
static int            image_embedded = 0;
static uint32_t       image_embedded_heap = 0;
static uint8_t        image_staged_head[CL_IMAGE_HEADER_BYTES];
static CL_ImageHeader image_staged_hdr;

static int image_restored = 0;

/* Lisp surface symbols (registered as GC roots during C init, so they are
 * part of the boot-root restore set and survive into images). */
static CL_Obj SYM_SAVE_HOOKS = CL_NIL;        /* EXT:*SAVE-HOOKS* */
static CL_Obj SYM_RESTORE_HOOKS = CL_NIL;     /* EXT:*RESTORE-HOOKS* */
/* EXT::*SYSTEM-RESTORE-HOOKS* (internal): the runtime's own library modules
 * re-derive the OS state they captured at load time -- the Amiga library
 * bases of AMIGA.FFI:DEFINE-LIBRARY-VARIABLE -- before ~/.clamigarc runs,
 * so no user code ever sees the zeroed foreign pointers of the image. */
static CL_Obj SYM_SYSTEM_RESTORE_HOOKS = CL_NIL;
static CL_Obj SYM_IMAGE_RESTORED_P = CL_NIL;  /* EXT:*IMAGE-RESTORED-P* */
/* EXT::*IMAGE-TOPLEVEL* (internal): the :TOPLEVEL function designator of
 * a delivered executable.  A symbol value, so it travels in the heap. */
static CL_Obj SYM_IMAGE_TOPLEVEL = CL_NIL;
static CL_Obj KW_QUIT_IMG = CL_NIL;
static CL_Obj KW_SHAKE_BINDINGS = CL_NIL;
static CL_Obj KW_EXECUTABLE = CL_NIL;
static CL_Obj KW_TOPLEVEL = CL_NIL;
static CL_Obj KW_HEAP_SIZE = CL_NIL;

void cl_image_note_boot_roots(void)
{
    image_boot_roots = cl_gc_global_root_count();
}

int cl_image_save_pending_p(void)
{
    return image_pending;
}

int cl_image_restored_p(void)
{
    return image_restored;
}

/* ================================================================
 * Preconditions (shared by the builtin and the deferred dump)
 * ================================================================ */

/* Returns NULL when saving is allowed, else a static message describing
 * the first violated precondition. */
static const char *image_save_preconditions(void)
{
    static char msg[512];
    GcWalk w;
    uint32_t off, size;
    uint8_t type;

    if (image_boot_roots < 0)
        return "SAVE-IMAGE: internal error - boot root count was never "
               "snapshotted (cl_image_note_boot_roots)";

    /* 1. No live worker threads (SBCL save-lisp-and-die doctrine).  Name
     * them: "1 worker thread" tells the user nothing when the thread is a
     * service a module started for them (the ARexx port from .clamigarc,
     * a TCP dev port, a REPL thread) -- the name says what to stop. */
    if (cl_thread_count > 1) {
        CL_Thread *t;
        size_t n;
        snprintf(msg, sizeof(msg),
                 "SAVE-IMAGE: %u worker thread(s) are still running (",
                 (unsigned)(cl_thread_count - 1));
        n = strlen(msg);
        platform_mutex_lock(cl_thread_list_lock);
        for (t = cl_thread_list; t; t = t->next) {
            char one[72];
            const char *sep = msg[n - 1] == '(' ? "" : ", ";
            int w;
            if (t == cl_main_thread_ptr) continue;
            if (CL_STRING_P(t->name)) {
                CL_String *s = (CL_String *)CL_OBJ_TO_PTR(t->name);
                w = snprintf(one, sizeof(one), "%s\"%.*s\"", sep,
                             (int)(s->length < 60 ? s->length : 60), s->data);
            } else {
                w = snprintf(one, sizeof(one), "%sunnamed thread %u", sep,
                             (unsigned)t->id);
            }
            if (w < 0 || n + (size_t)w >= sizeof(msg) - 200) {
                snprintf(msg + n, sizeof(msg) - n, ", ...");
                n += 5;
                break;
            }
            memcpy(msg + n, one, (size_t)w);
            n += (size_t)w;
            msg[n] = '\0';
        }
        platform_mutex_unlock(cl_thread_list_lock);
        snprintf(msg + n, sizeof(msg) - n,
                 ") - threads cannot survive into an image: JOIN-THREAD or "
                 "MP:DESTROY-THREAD them, or stop the service that started "
                 "them (the ARexx port from .clamigarc is one: save from a "
                 "clamiga started with --no-userinit)");
        return msg;
    }

    /* 4. No active FASL readers/writers or MAKE-LOAD-FORM pre-pass.
     * Guaranteed at a top-level safe point; asserted for defense. */
    if (cl_fasl_reader_save_count() != 0 || cl_fasl_writer_save_count() != 0 ||
        cl_fasl_mlf_active_p())
        return "SAVE-IMAGE: a FASL load/compile is still active";

    /* 2 + 3. One arena walk: open file/socket streams and owned foreign
     * pointers.  Console, string, synonym, two-way, broadcast,
     * concatenated, echo, cbuf and closed streams are fine. */
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        uint8_t *ptr = cl_heap.arena + off;
        if (type == TYPE_STREAM) {
            CL_Stream *st = (CL_Stream *)ptr;
            if ((st->flags & CL_STREAM_FLAG_OPEN) &&
                (st->stream_type == CL_STREAM_FILE ||
                 st->stream_type == CL_STREAM_SOCKET)) {
                if (st->stream_type == CL_STREAM_FILE &&
                    CL_STRING_P(st->string_buf)) {
                    CL_String *s = (CL_String *)CL_OBJ_TO_PTR(st->string_buf);
                    snprintf(msg, sizeof(msg),
                             "SAVE-IMAGE: file stream \"%.300s\" is still "
                             "open - CLOSE it first (OS handles cannot "
                             "survive into an image; use EXT:*SAVE-HOOKS* "
                             "to close and EXT:*RESTORE-HOOKS* to reopen)",
                             s->data);
                } else {
                    snprintf(msg, sizeof(msg),
                             "SAVE-IMAGE: an open %s stream is still live - "
                             "close it first (OS handles cannot survive "
                             "into an image; use EXT:*SAVE-HOOKS* to close "
                             "and EXT:*RESTORE-HOOKS* to reopen)",
                             st->stream_type == CL_STREAM_SOCKET ? "socket"
                                                                 : "file");
                }
                return msg;
            }
        } else if (type == TYPE_FOREIGN_POINTER) {
            CL_ForeignPtr *fp = (CL_ForeignPtr *)ptr;
            if ((fp->flags & CL_FPTR_FLAG_OWNED) && fp->address != 0)
                return "SAVE-IMAGE: an owned foreign pointer (FFI "
                       "allocation) is still live - free it first, or tear "
                       "it down in EXT:*SAVE-HOOKS* and rebuild it in "
                       "EXT:*RESTORE-HOOKS*";
        }
    }

    return NULL;
}

/* Main thread completely at rest: nothing transient holds heap state, so
 * the persistent enumeration below IS the whole session state. */
static int image_main_thread_at_rest(void)
{
    CL_Thread *t = cl_main_thread_ptr;
    return t->vm.sp == 0 && t->vm.fp == 0 && t->dyn_top == 0 &&
           t->nlx_top == 0 && t->handler_top == 0 && t->restart_top == 0 &&
           t->gc_root_count == 0 && t->saved_pending_top == 0 &&
           !t->pending_throw;
}

/* ================================================================
 * Hook lists (EXT:*SAVE-HOOKS* / EXT:*RESTORE-HOOKS*)
 * ================================================================ */

/* Funcall every designator on the list bound to SYM (most recent first,
 * i.e. list order), each in its own CL_CATCH so one erroring hook cannot
 * abort the rest.  Unlike exit hooks the list is NOT cleared: save hooks
 * serve every save of the session, restore hooks live in the image. */
static void image_run_hook_list(CL_Obj sym, const char *what)
{
    CL_Obj hooks, fn = CL_NIL;

    if (CL_NULL_P(sym)) return;
    hooks = cl_symbol_value(sym);
    if (hooks == CL_UNBOUND || !CL_CONS_P(hooks)) return;

    cl_vm.sp = 0;
    cl_vm.fp = 0;

    CL_GC_PROTECT(hooks);
    CL_GC_PROTECT(fn);
    while (CL_CONS_P(hooks)) {
        int err;
        CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            fn = cl_coerce_funcdesig(cl_car(hooks), what);
            cl_vm_apply(fn, NULL, 0);
            CL_UNCATCH();
        } else if (err == CL_ERR_EXIT) {
            CL_UNCATCH();
            break;
        } else {
            char buf[256];
            CL_UNCATCH();
            snprintf(buf, sizeof(buf), "; Error in %s: %.180s\n",
                     what, cl_error_msg);
            platform_write_string(buf);
            cl_vm.sp = 0;
            cl_vm.fp = 0;
        }
        hooks = cl_cdr(hooks);
    }
    CL_GC_UNPROTECT(2);
}

void cl_image_run_restore_hooks(void)
{
    image_run_hook_list(SYM_RESTORE_HOOKS, "restore hook");
}

void cl_image_run_system_restore_hooks(void)
{
    image_run_hook_list(SYM_SYSTEM_RESTORE_HOOKS, "system restore hook");
}

/* ================================================================
 * File writer
 * ================================================================ */

typedef struct {
    PlatformFile fh;
    int error;
    uint32_t written;   /* bytes so far (the executable trailer needs them) */
} ImgWriter;

static void iw_bytes(ImgWriter *w, const void *data, uint32_t len)
{
    if (w->error || len == 0) return;
    if (platform_file_write_buf(w->fh, (const char *)data, len) < 0)
        w->error = 1;
    w->written += len;
}

static void iw_u8(ImgWriter *w, uint8_t v)  { iw_bytes(w, &v, 1); }
static void iw_u16(ImgWriter *w, uint16_t v) { iw_bytes(w, &v, 2); }
static void iw_u32(ImgWriter *w, uint32_t v) { iw_bytes(w, &v, 4); }

/* The SOURCES section: every distinct source-file name once, which a blob
 * then names by a 1-based u16 index (0 = no file).  A file's functions all
 * point at ONE interned string (cl_intern_source_file), so the table is
 * keyed by pointer; a blob used to carry its own copy of the path instead
 * -- 2,019 copies of 46 paths, 130 KB, in the editor's image.  The arena
 * walk meets a file's bytecodes mostly back to back, so the last hit is
 * tried before the scan. */
#define IMG_MAX_SOURCES 0xFFFFu

typedef struct {
    const char **paths;
    uint32_t count;
    uint32_t cap;
    uint32_t last;          /* index of the last hit */
} ImgSources;

/* PATH's 1-based index, entered first when ADD.  0 for no path, and for
 * one the table cannot take (full, out of memory, longer than a u16 length
 * says): that function then comes back without a source file. */
static uint16_t img_source_index(ImgSources *t, const char *path, int add)
{
    uint32_t i;

    if (!path) return 0;
    if (t->last < t->count && t->paths[t->last] == path)
        return (uint16_t)(t->last + 1);
    for (i = 0; i < t->count; i++) {
        if (t->paths[i] == path) {
            t->last = i;
            return (uint16_t)(i + 1);
        }
    }
    if (!add || t->count >= IMG_MAX_SOURCES || strlen(path) > 0xFFFFu)
        return 0;
    if (t->count == t->cap) {
        uint32_t ncap = t->cap ? t->cap * 2 : 64;
        const char **np =
            (const char **)platform_alloc(ncap * (uint32_t)sizeof(*np));
        if (!np) return 0;
        if (t->count)
            memcpy(np, t->paths, t->count * (uint32_t)sizeof(*np));
        if (t->paths) platform_free((void *)t->paths);
        t->paths = np;
        t->cap = ncap;
    }
    t->paths[t->count] = path;
    t->last = t->count;
    return (uint16_t)(++t->count);
}

/* Count TYPE_BYTECODE objects in the (compacted, hole-free) arena and enter
 * their source files into SRCS.  Returns the count; *LOST is how many of
 * them have a source file the table could not take. */
static uint32_t image_collect_bytecodes(ImgSources *srcs, uint32_t *lost)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;
    uint32_t n = 0;
    *lost = 0;
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        if (type == TYPE_BYTECODE) {
            const CL_Bytecode *bc = (const CL_Bytecode *)(cl_heap.arena + off);
            n++;
            if (bc->source_file &&
                img_source_index(srcs, bc->source_file, 1) == 0)
                (*lost)++;
        }
    }
    return n;
}

static void image_write_bytecode_blob(ImgWriter *w, uint32_t off,
                                      const CL_Bytecode *bc, uint16_t sf_idx)
{
    uint32_t code_len = bc->code ? bc->code_len : 0;
    uint16_t n_const = bc->constants ? bc->n_constants : 0;
    uint8_t n_keys = (bc->key_syms && bc->key_slots) ? bc->n_keys : 0;
    uint16_t lm_count = bc->line_map ? bc->line_map_count : 0;

    iw_u32(w, off);
    iw_u32(w, code_len);
    iw_bytes(w, bc->code, code_len);
    iw_u16(w, n_const);
    iw_bytes(w, bc->constants, (uint32_t)n_const * 4u);
    iw_u8(w, n_keys);
    iw_bytes(w, bc->key_syms, (uint32_t)n_keys * 4u);
    iw_bytes(w, bc->key_slots, n_keys);
    if (n_keys) {
        if (bc->key_suppliedp_slots) {
            iw_bytes(w, bc->key_suppliedp_slots, n_keys);
        } else {
            uint8_t none = 0xFF;
            uint8_t i;
            for (i = 0; i < n_keys; i++) iw_bytes(w, &none, 1);
        }
    }
    iw_u16(w, lm_count);
    iw_bytes(w, bc->line_map, (uint32_t)lm_count * (uint32_t)sizeof(CL_LineEntry));
    iw_u16(w, sf_idx);
}

/* What a delivered executable puts between the runtime and the image.
 * ELF, Mach-O and PE loaders never look behind the last section; AmigaOS
 * LoadSeg reads hunk after hunk until the file ends and refuses anything
 * it does not know (ERROR_BAD_HUNK, seen in FS-UAE).  So on the hunk
 * build the image and the trailer are the body of one HUNK_DEBUG, the
 * hunk type LoadSeg skips: its id and its length in longwords. */
#if defined(PLATFORM_AMIGA) && !defined(PLATFORM_MORPHOS)
#define IMAGE_EXE_WRAP_BYTES 8u
#define IMAGE_HUNK_DEBUG     0x000003F1u
#else
#define IMAGE_EXE_WRAP_BYTES 0u
#endif

/* Read the delivered-executable trailer at the end of PATH (image.h).
 * Returns 1 with the three fields and the file's length when there is one,
 * 0 when there is none, -1 when PATH cannot be opened. */
static int image_probe_trailer(const char *path, uint32_t *off, uint32_t *len,
                               uint32_t *heap, unsigned long *file_len)
{
    uint8_t t[CL_IMAGE_EXE_TRAILER_BYTES];
    PlatformFile fh;
    long flen;
    int got = 0;

    *file_len = 0;
    fh = platform_file_open(path, PLATFORM_FILE_READ);
    if (fh == PLATFORM_FILE_INVALID)
        return -1;
    flen = platform_file_length(fh);
    if (flen >= (long)CL_IMAGE_EXE_TRAILER_BYTES &&
        platform_file_set_position(
            fh, flen - (long)CL_IMAGE_EXE_TRAILER_BYTES) == 0)
        got = platform_file_read_buf(fh, (char *)t,
                                     CL_IMAGE_EXE_TRAILER_BYTES);
    platform_file_close(fh);
    if (flen > 0) *file_len = (unsigned long)flen;
    if (got != (int)CL_IMAGE_EXE_TRAILER_BYTES ||
        memcmp(t + 12, CL_IMAGE_EXE_MAGIC, CL_IMAGE_EXE_MAGIC_LEN) != 0)
        return 0;
    memcpy(off, t, 4);
    memcpy(len, t + 4, 4);
    memcpy(heap, t + 8, 4);
    return 1;
}

/* Copy the runtime -- the running executable without an image it may carry
 * itself -- to the writer.  Returns its length, 0 on failure (said). */
static uint32_t image_copy_runtime(ImgWriter *w)
{
    char path[1024];
    char msg[400];
    uint32_t off = 0, len = 0, heap = 0, left, total;
    unsigned long flen = 0;
    PlatformFile in;
    char *buf;
    int found;

    if (!platform_executable_path(path, (int)sizeof(path))) {
        platform_write_string("; SAVE-IMAGE: cannot locate the running "
                              "clamiga executable\n");
        return 0;
    }
    found = image_probe_trailer(path, &off, &len, &heap, &flen);
    total = (found == 1 && off >= IMAGE_EXE_WRAP_BYTES && off < flen)
                ? off - IMAGE_EXE_WRAP_BYTES : (uint32_t)flen;
    buf = (char *)platform_alloc(16384);
    in = (found >= 0 && total > 0 && buf)
             ? platform_file_open(path, PLATFORM_FILE_READ)
             : PLATFORM_FILE_INVALID;
    if (in == PLATFORM_FILE_INVALID) {
        if (buf) platform_free(buf);
        snprintf(msg, sizeof(msg),
                 "; SAVE-IMAGE: cannot read the running executable "
                 "\"%.300s\"\n", path);
        platform_write_string(msg);
        return 0;
    }
    for (left = total; left > 0 && !w->error; ) {
        uint32_t want = left < 16384u ? left : 16384u;
        int got = platform_file_read_buf(in, buf, want);
        if (got <= 0) break;
        iw_bytes(w, buf, (uint32_t)got);
        left -= (uint32_t)got;
    }
    platform_file_close(in);
    platform_free(buf);
    if (left > 0) {
        snprintf(msg, sizeof(msg),
                 "; SAVE-IMAGE: short read copying the running executable "
                 "\"%.300s\"\n", path);
        platform_write_string(msg);
        return 0;
    }
    return total;
}

/* Write the image to PATH; with EXE, as a delivered executable: the
 * runtime first, the image behind it, the trailer last (image.h). */
static int image_write_file(const char *path, int exe, uint32_t exe_heap)
{
    char tmp_path[1100];
    uint32_t runtime_len = 0, image_off = 0;
    ImgWriter w;
    uint8_t fprint[CL_IMAGE_FPRINT_LEN];
    uint32_t n_blobs, n_roots, n_outbufs, h, lost;
    ImgSources srcs;
    int i;
    CL_Thread *mt = cl_main_thread_ptr;

    if (strlen(path) > sizeof(tmp_path) - 8) {
        platform_write_string("; SAVE-IMAGE: path too long\n");
        return -1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    n_roots = (uint32_t)image_boot_roots + N_IMAGE_NAMED_ROOTS;
    n_outbufs = 0;
    for (h = cl_stream_outbuf_next_used(0); h != 0;
         h = cl_stream_outbuf_next_used(h))
        n_outbufs++;

    w.fh = platform_file_open(tmp_path, PLATFORM_FILE_WRITE);
    w.error = 0;
    w.written = 0;
    if (w.fh == PLATFORM_FILE_INVALID) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "; SAVE-IMAGE: cannot create \"%.180s\"\n", tmp_path);
        platform_write_string(buf);
        return -1;
    }
    if (exe) {
        runtime_len = image_copy_runtime(&w);
        if (runtime_len == 0) {
            platform_file_close(w.fh);
            platform_file_delete(tmp_path);
            return -1;
        }
#if IMAGE_EXE_WRAP_BYTES
        iw_u32(&w, IMAGE_HUNK_DEBUG);
        iw_u32(&w, 0);                  /* longwords: patched below */
#endif
        image_off = w.written;
    }

    memset(&srcs, 0, sizeof(srcs));
    n_blobs = image_collect_bytecodes(&srcs, &lost);
    if (lost) {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "; SAVE-IMAGE: note: %u function(s) lose their source file "
                 "name - it did not fit the image's source-file table\n",
                 (unsigned)lost);
        platform_write_string(buf);
    }

    /* --- Header --- */
    image_fingerprint(fprint);
    iw_u32(&w, CL_IMAGE_MAGIC);
    iw_u16(&w, CL_IMAGE_VERSION);
    iw_u16(&w, image_flags());
    iw_bytes(&w, fprint, CL_IMAGE_FPRINT_LEN);
    iw_u32(&w, cl_heap.arena_size);
    iw_u32(&w, cl_heap.bump);
    iw_u32(&w, n_roots);
    iw_u32(&w, n_blobs);
    iw_u32(&w, (uint32_t)image_boot_roots);

    /* --- Section 1: ROOTS --- */
    for (i = 0; i < image_boot_roots; i++) {
        uint32_t v = cl_gc_global_root_get(i);
        iw_bytes(&w, &v, 4);
    }
    for (i = 0; i < N_IMAGE_NAMED_ROOTS; i++) {
        uint32_t v = *image_named_root(i);
        iw_bytes(&w, &v, 4);
    }

    /* --- Section 2: READTABLES --- */
    iw_u32(&w, cl_readtable_alloc_mask);
    iw_u32(&w, (uint32_t)sizeof(cl_readtable_pool));
    iw_bytes(&w, cl_readtable_pool, (uint32_t)sizeof(cl_readtable_pool));

    /* --- Section 3: THREAD0 --- */
    {
        uint32_t v = mt->current_package;
        uint32_t wobj = cl_main_thread_lisp_obj();
        uint32_t n_tlv = 0;
        int ti;
        iw_bytes(&w, &v, 4);
        iw_bytes(&w, &wobj, 4);
        for (ti = 0; ti < CL_TLV_TABLE_SIZE; ti++) {
            CL_Obj s = mt->tlv_table[ti].symbol;
            if (s != CL_NIL && s != CL_UNBOUND) n_tlv++;
        }
        iw_u32(&w, n_tlv);
        for (ti = 0; ti < CL_TLV_TABLE_SIZE; ti++) {
            CL_Obj s = mt->tlv_table[ti].symbol;
            if (s != CL_NIL && s != CL_UNBOUND) {
                uint32_t sv = s, vv = mt->tlv_table[ti].value;
                iw_bytes(&w, &sv, 4);
                iw_bytes(&w, &vv, 4);
            }
        }
    }

    /* --- Section 4: SOURCES (each source-file name once) --- */
    iw_u32(&w, srcs.count);
    for (h = 0; h < srcs.count; h++) {
        uint16_t len = (uint16_t)strlen(srcs.paths[h]);
        iw_u16(&w, len);
        iw_bytes(&w, srcs.paths[h], len);
    }

    /* --- Section 5: BLOBS (bytecode side buffers, then outbufs) --- */
    {
        GcWalk gw;
        uint32_t off, size;
        uint8_t type;
        gc_walk_begin(&gw);
        while (gc_walk_next(&gw, &off, &size, &type)) {
            if (type == TYPE_BYTECODE) {
                const CL_Bytecode *bc =
                    (const CL_Bytecode *)(cl_heap.arena + off);
                image_write_bytecode_blob(&w, off, bc,
                    img_source_index(&srcs, bc->source_file, 0));
            }
        }
    }
    if (srcs.paths) platform_free((void *)srcs.paths);
    iw_u32(&w, n_outbufs);
    for (h = cl_stream_outbuf_next_used(0); h != 0;
         h = cl_stream_outbuf_next_used(h)) {
        uint32_t len = cl_stream_outbuf_len(h);
        iw_u32(&w, h);
        iw_u32(&w, len);
        iw_bytes(&w, cl_stream_outbuf_data(h), len);
    }

    /* --- Section 6: ARENA --- */
    iw_bytes(&w, cl_heap.arena, cl_heap.bump);

    if (exe) {
        static const uint8_t zeros[4] = { 0, 0, 0, 0 };
        uint32_t image_len = w.written - image_off;
        /* The trailer starts on a longword (a hunk body is longwords). */
        iw_bytes(&w, zeros, (4u - (image_len & 3u)) & 3u);
        iw_u32(&w, image_off);
        iw_u32(&w, image_len);
        iw_u32(&w, exe_heap);
        iw_bytes(&w, CL_IMAGE_EXE_MAGIC, CL_IMAGE_EXE_MAGIC_LEN);
#if IMAGE_EXE_WRAP_BYTES
        {
            uint32_t longs = (w.written - image_off) / 4u;
            if (platform_file_set_position(w.fh, (long)(runtime_len + 4u)) != 0)
                w.error = 1;
            iw_u32(&w, longs);
        }
#endif
    }

    if (platform_file_flush(w.fh) < 0) w.error = 1;
    platform_file_close(w.fh);

    if (w.error) {
        platform_file_delete(tmp_path);
        platform_write_string("; SAVE-IMAGE: write failed (disk full?) - "
                              "image not saved, session unchanged\n");
        return -1;
    }

    /* Delete-before-rename: an in-place overwrite keeps the old datestamp
     * under FS-UAE's .uaem metadata handling (documented stale-FASL trap),
     * and AmigaDOS Rename() refuses to replace an existing object. */
    platform_file_delete(path);
    if (platform_file_rename(tmp_path, path) != 0) {
        platform_file_delete(tmp_path);
        platform_write_string("; SAVE-IMAGE: could not rename temp file "
                              "into place - image not saved\n");
        return -1;
    }
    if (exe && platform_file_make_executable(path) != 0)
        platform_write_string("; SAVE-IMAGE: note: could not mark the file "
                              "executable - set the permission by hand\n");
    return 0;
}

/* ================================================================
 * Save request + deferred execution
 * ================================================================ */

static int image_have_save_hooks(void)
{
    CL_Obj hooks;
    if (CL_NULL_P(SYM_SAVE_HOOKS)) return 0;
    hooks = cl_symbol_value(SYM_SAVE_HOOKS);
    return hooks != CL_UNBOUND && CL_CONS_P(hooks);
}

void cl_image_save_request(const char *path, int quit, int shake)
{
    if (!path || !path[0])
        cl_error(CL_ERR_FILE, "SAVE-IMAGE: empty pathname");
    if (strlen(path) >= sizeof(image_pending_path))
        cl_error(CL_ERR_FILE, "SAVE-IMAGE: pathname too long");

    /* With no save hooks registered, check the preconditions NOW so the
     * caller gets a synchronous, catchable error.  With hooks the check
     * must wait for the dump point: hooks exist precisely to close the
     * streams / tear down the FFI state that would otherwise abort the
     * save, and they only run once the main thread is at rest. */
    if (!image_have_save_hooks()) {
        const char *pre = image_save_preconditions();
        if (pre)
            cl_error(CL_ERR_GENERAL, "%s", pre);
    }

    strcpy(image_pending_path, path);
    image_pending_quit = quit;
    image_pending_shake = shake;
    image_pending_exe = 0;
    image_pending_heap = 0;
    image_pending = 1;
}

void cl_image_save_executable_request(const char *path, int quit, int shake,
                                      uint32_t heap_size)
{
    char self[1024];
    PlatformFile fh;

    /* Synchronously, so a build script gets a catchable error: without the
     * runtime's own file there is nothing to put the image behind. */
    if (!platform_executable_path(self, (int)sizeof(self)))
        cl_error(CL_ERR_FILE,
                 "SAVE-IMAGE: :EXECUTABLE T needs the running clamiga's own "
                 "file, and this process cannot tell where it was started "
                 "from");
    fh = platform_file_open(self, PLATFORM_FILE_READ);
    if (fh == PLATFORM_FILE_INVALID)
        cl_error(CL_ERR_FILE,
                 "SAVE-IMAGE: :EXECUTABLE T cannot read the running "
                 "executable \"%s\"", self);
    platform_file_close(fh);

    cl_image_save_request(path, quit, shake);
    image_pending_exe = 1;
    image_pending_heap = heap_size;
}

int cl_image_save_run_if_pending(void)
{
    const char *pre;
    int quit, shake, exe;
    uint32_t exe_heap;

    if (!image_pending)
        return 0;
    if (!image_main_thread_at_rest())
        return 0;   /* stays pending until an outer safe point is at rest */

    image_pending = 0;
    quit = image_pending_quit;
    shake = image_pending_shake;
    exe = image_pending_exe;
    exe_heap = image_pending_heap;
    image_pending_quit = 0;
    image_pending_shake = 0;
    image_pending_exe = 0;
    image_pending_heap = 0;

    /* 1. Save hooks first — a hook may close the very streams that would
     * otherwise abort the save. */
    image_run_hook_list(SYM_SAVE_HOOKS, "save hook");

    /* 2. Re-verify (hooks ran user code) — including at-rest, which a
     * misbehaving hook could theoretically have broken. */
    pre = image_save_preconditions();
    if (!pre && !image_main_thread_at_rest())
        pre = "SAVE-IMAGE: a save hook left transient state behind - "
              "image not saved";
    if (pre) {
        char buf[600];
        snprintf(buf, sizeof(buf), "; %.560s\n", pre);
        platform_write_string(buf);
        return 0;
    }

    /* Late-registered roots (index >= boot_roots) are deliberately dropped
     * from the image: in the restoring process the corresponding static is
     * still NIL/unregistered and its lazy-init pattern re-derives the value
     * (cached interned symbols).  That is safe ONLY for re-derivable
     * caches, so warn whenever any exist — a STATEFUL late root is a bug
     * (register it during init instead; see specs/image-save-load.md). */
    if (cl_gc_global_root_count() > image_boot_roots) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "; SAVE-IMAGE: note: %d GC root(s) were registered after "
                 "boot and are not saved (re-derived lazily at restore)\n",
                 cl_gc_global_root_count() - image_boot_roots);
        platform_write_string(buf);
#ifdef DEBUG_GC
        {
            int li;
            for (li = image_boot_roots; li < cl_gc_global_root_count(); li++) {
                snprintf(buf, sizeof(buf), ";   late root #%d: %s:%d\n", li,
                         cl_gc_global_root_file(li) ? cl_gc_global_root_file(li)
                                                    : "?",
                         cl_gc_global_root_line(li));
                platform_write_string(buf);
            }
        }
#endif
    }

    /* 2b. :SHAKE-BINDINGS — shed the demand-interned binding tables
     * (specs/raw-bindings-footprint.md, Phase 3).  After the save hooks, so
     * a hook that REQUIREd another raw module is covered too, and before
     * the dump GC below, which is what actually reclaims the blobs.
     *
     * This closes those packages permanently — in the image and in this
     * session (a failed write afterwards does not put them back; re-loading
     * the module's FASL does).  Say so: silently losing names would be the
     * worst possible failure mode to debug from a restored image. */
    if (shake) {
        uint32_t bytes = 0;
        uint32_t count = cl_bindtab_shed_all(&bytes);
        if (count) {
            char buf[320];
            snprintf(buf, sizeof(buf),
                     "; SAVE-IMAGE: shed %u binding table%s (%u KB) - the image "
                     "and this session now resolve only the names already "
                     "referenced in those packages\n",
                     (unsigned)count, count == 1 ? "" : "s",
                     (unsigned)((bytes + 1023) >> 10));
            platform_write_string(buf);
        }
    }

    /* 3. Dump the minimal, hole-free heap: the backend makes the arena
     * dense and normal (free_list == 0, a dense [CL_ALIGN, bump) payload,
     * marks as the loader expects). */
    gcb_before_image_save();

    /* 4. Offset-keyed reader diagnostics are stale by definition in the
     * next process (also cleared at restore — belt and braces). */
    {
        int i;
        for (i = 0; i < CL_SRCLOC_SIZE; i++)
            cl_srcloc_table[i].cons_obj = CL_NIL;
    }

    /* 5. Write temp + rename. */
    if (image_write_file(image_pending_path, exe, exe_heap) == 0) {
        char buf[400];
        snprintf(buf, sizeof(buf),
                 "; %s saved to \"%.300s\" (%u KB heap)\n",
                 exe ? "Executable" : "Image",
                 image_pending_path,
                 (unsigned)(cl_heap.bump >> 10));
        platform_write_string(buf);
        return quit;
    }
    return 0;
}

/* ================================================================
 * Restore: staging (before cl_mem_init)
 * ================================================================ */

typedef struct {
    const uint8_t *data;
    uint32_t size;
    uint32_t pos;
    int error;
} ImgReader;

static int ir_need(ImgReader *r, uint32_t n)
{
    if (r->error || r->size - r->pos < n || r->pos + n < r->pos) {
        r->error = 1;
        return 0;
    }
    return 1;
}

static void ir_bytes(ImgReader *r, void *out, uint32_t n)
{
    if (!ir_need(r, n)) { if (out) memset(out, 0, n); return; }
    if (out) memcpy(out, r->data + r->pos, n);
    r->pos += n;
}

static uint8_t  ir_u8(ImgReader *r)  { uint8_t v = 0;  ir_bytes(r, &v, 1); return v; }
static uint16_t ir_u16(ImgReader *r) { uint16_t v = 0; ir_bytes(r, &v, 2); return v; }
static uint32_t ir_u32(ImgReader *r) { uint32_t v = 0; ir_bytes(r, &v, 4); return v; }

static const uint8_t *ir_ptr(ImgReader *r, uint32_t n)
{
    const uint8_t *p;
    if (!ir_need(r, n)) return NULL;
    p = r->data + r->pos;
    r->pos += n;
    return p;
}

static int image_parse_header(ImgReader *r, CL_ImageHeader *h)
{
    h->magic = ir_u32(r);
    h->version = ir_u16(r);
    h->flags = ir_u16(r);
    ir_bytes(r, h->fingerprint, CL_IMAGE_FPRINT_LEN);
    h->arena_size = ir_u32(r);
    h->bump = ir_u32(r);
    h->n_roots = ir_u32(r);
    h->n_blobs = ir_u32(r);
    h->boot_roots = ir_u32(r);
    return !r->error;
}

static void image_stage_fail(int quiet, const char *fmt, const char *arg)
{
    if (!quiet) {
        char buf[512];
        snprintf(buf, sizeof(buf), fmt, arg ? arg : "");
        platform_write_string(buf);
    }
}

/* Stage the image that starts OFF bytes into PATH and is IMG_LEN bytes
 * long (0 = it runs to the end of the file: a plain image file). */
static int image_stage_at(const char *path, uint32_t off, uint32_t img_len,
                          int quiet)
{
    ImgReader r;
    uint8_t fprint[CL_IMAGE_FPRINT_LEN];
    PlatformFile fh;
    long len;
    int got = -1;
    size_t plen;

    cl_image_discard_staged();

    /* The header only (see image_staged_path): the payload is read by the
     * restore, once the arena has its block. */
    fh = platform_file_open(path, PLATFORM_FILE_READ);
    if (fh == PLATFORM_FILE_INVALID) {
        image_stage_fail(quiet, "; --image: cannot read \"%.300s\"\n", path);
        return -1;
    }
    len = platform_file_length(fh);
    if (off == 0 || platform_file_set_position(fh, (long)off) == 0)
        got = platform_file_read_buf(fh, (char *)image_staged_head,
                                     CL_IMAGE_HEADER_BYTES);
    platform_file_close(fh);
    plen = strlen(path) + 1;
    image_staged_path = (char *)platform_alloc((unsigned long)plen);
    if (!image_staged_path) {
        image_stage_fail(quiet, "; --image: out of memory staging \"%.300s\"\n",
                         path);
        return -1;
    }
    memcpy(image_staged_path, path, plen);
    image_staged_file_len = len > 0 ? (unsigned long)len : 0;
    image_staged_offset = off;
    image_staged_size = img_len ? (unsigned long)img_len
                      : image_staged_file_len > off ? image_staged_file_len - off
                      : 0;

    r.data = image_staged_head;
    r.size = got > 0 ? (uint32_t)got : 0;
    r.pos = 0;
    r.error = 0;

    if (got != (int)CL_IMAGE_HEADER_BYTES ||
        image_staged_size < CL_IMAGE_HEADER_BYTES ||
        !image_parse_header(&r, &image_staged_hdr) ||
        image_staged_hdr.magic != CL_IMAGE_MAGIC) {
        image_stage_fail(quiet,
            "; --image: \"%.300s\" is not a clamiga heap image (or was "
            "written with a different byte order)\n", path);
        cl_image_discard_staged();
        return -1;
    }
    if (image_staged_hdr.version != CL_IMAGE_VERSION) {
        image_stage_fail(quiet,
            "; --image: \"%.300s\" uses a different image format version - "
            "it was written by a different build of clamiga; delete it or "
            "rebuild it with EXT:SAVE-IMAGE\n", path);
        cl_image_discard_staged();
        return -1;
    }
    image_fingerprint(fprint);
    if (memcmp(fprint, image_staged_hdr.fingerprint,
               CL_IMAGE_FPRINT_LEN) != 0) {
        image_stage_fail(quiet,
            "; --image: \"%.300s\" was written by a different build of "
            "clamiga (version/feature/layout fingerprint mismatch) - "
            "delete it or rebuild it with EXT:SAVE-IMAGE\n", path);
        cl_image_discard_staged();
        return -1;
    }
    if (image_staged_hdr.bump < CL_ALIGN ||
        (image_staged_hdr.bump & (CL_ALIGN - 1)) != 0 ||
        image_staged_hdr.n_roots !=
            image_staged_hdr.boot_roots + N_IMAGE_NAMED_ROOTS) {
        image_stage_fail(quiet,
            "; --image: \"%.300s\" is structurally corrupt\n", path);
        cl_image_discard_staged();
        return -1;
    }
    return 0;
}

int cl_image_stage(const char *path, int quiet)
{
    return image_stage_at(path, 0, 0, quiet);
}

int cl_image_stage_embedded(void)
{
    char path[1024];
    char msg[512];
    uint32_t off = 0, len = 0, heap = 0;
    unsigned long flen = 0;

    if (!platform_executable_path(path, (int)sizeof(path)))
        return 0;
    if (image_probe_trailer(path, &off, &len, &heap, &flen) != 1)
        return 0;
    if (len < CL_IMAGE_HEADER_BYTES ||
        flen < CL_IMAGE_EXE_TRAILER_BYTES ||
        len > flen - CL_IMAGE_EXE_TRAILER_BYTES ||
        off > flen - CL_IMAGE_EXE_TRAILER_BYTES - len ||
        flen - CL_IMAGE_EXE_TRAILER_BYTES - len - off > 3 || /* padding */
        image_stage_at(path, off, len, 0) != 0) {
        snprintf(msg, sizeof(msg),
                 "; \"%.300s\": the program image appended to this "
                 "executable cannot be used - the file is damaged or was "
                 "altered after it was saved\n", path);
        platform_write_string(msg);
        return -1;
    }
    image_embedded = 1;
    image_embedded_heap = heap;
    return 1;
}

int cl_image_embedded_p(void)
{
    return image_embedded;
}

uint32_t cl_image_embedded_heap_size(void)
{
    return image_embedded_heap;
}

uint32_t cl_image_staged_bump(void)
{
    return image_staged_path ? image_staged_hdr.bump : 0;
}

int cl_image_staged_p(void)
{
    return image_staged_path != NULL;
}

void cl_image_discard_staged(void)
{
    if (image_staged_buf) {
        platform_free(image_staged_buf);
        image_staged_buf = NULL;
    }
    if (image_staged_path) {
        platform_free(image_staged_path);
        image_staged_path = NULL;
    }
    image_staged_size = 0;
    image_staged_offset = 0;
    image_staged_file_len = 0;
}

/* Read the staged file's payload (restore time, after cl_mem_init) and
 * check it is still the file that was staged: same size, same header
 * bytes.  Pre-arena, so a failure lets the caller fall back.  0 on
 * success. */
static int image_load_staged_payload(void)
{
    PlatformFile fh;
    char buf[400];
    int changed = 0, got = -1;

    if (image_staged_buf) return 0;
    fh = platform_file_open(image_staged_path, PLATFORM_FILE_READ);
    if (fh != PLATFORM_FILE_INVALID) {
        long flen = platform_file_length(fh);
        if (flen < 0 || (unsigned long)flen != image_staged_file_len ||
            (image_staged_offset != 0 &&
             platform_file_set_position(fh, (long)image_staged_offset) != 0)) {
            changed = 1;
        } else {
            image_staged_buf = (char *)platform_alloc(image_staged_size);
            if (image_staged_buf)
                got = platform_file_read_buf(fh, image_staged_buf,
                                             (uint32_t)image_staged_size);
        }
        platform_file_close(fh);
    }
    if (!changed && image_staged_buf &&
        (got < 0 || (unsigned long)got != image_staged_size ||
         memcmp(image_staged_buf, image_staged_head,
                CL_IMAGE_HEADER_BYTES) != 0))
        changed = 1;
    if (changed) {
        snprintf(buf, sizeof(buf),
                 "; --image: \"%.300s\" changed while clamiga was starting - "
                 "start it again\n", image_staged_path);
        platform_write_string(buf);
        if (image_staged_buf) {
            platform_free(image_staged_buf);
            image_staged_buf = NULL;
        }
        return -1;
    }
    if (!image_staged_buf) {
        snprintf(buf, sizeof(buf),
                 "; --image: cannot read \"%.300s\" (%lu bytes) after the "
                 "heap was allocated - too little memory left for the image "
                 "file? try a smaller --heap\n",
                 image_staged_path, image_staged_size);
        platform_write_string(buf);
        return -1;
    }
    return 0;
}

/* ================================================================
 * Restore: the relink walk
 * ================================================================ */

/* Fatal-but-clean abort once the arena has been overwritten: there is no
 * heap to fall back to — print exactly what went wrong and exit. */
static void image_fatal(const char *fmt, unsigned a, unsigned b)
{
    char buf[512];
    platform_write_string(
        "FATAL: --image restore failed after the heap was replaced "
        "(corrupt image): ");
    snprintf(buf, sizeof(buf), fmt, a, b);
    platform_write_string(buf);
    platform_write_string("\nDelete the image file and boot normally.\n");
    fflush(NULL);
#if defined(PLATFORM_AMIGA) && !defined(PLATFORM_MORPHOS)
    /* m68k AmigaOS: exit()'s post-main stdio teardown hangs the Shell —
     * same reason main.c ends there through platform_process_exit, which
     * also hands back the swapped main stack and a Workbench start's
     * console and directory lock (a bare _exit would leak all three). */
    platform_process_exit(1);
#else
    /* MorphOS (and host): route through cl_fatal_exit() so
     * cl_thread_restore_main_tls() runs first — cl_thread_init() has
     * already overwritten tc_UserData by the time this path can be
     * reached, and a plain exit() here would hit the same -noixemul
     * crt0 post-main-teardown freeze cl_thread_restore_main_tls()
     * exists to prevent. */
    cl_fatal_exit(1);
#endif
}

/* Relink one restored CL_Function through the builtin registry.  Returns
 * 0 on success, -1 when the stale stub had to be installed. */
static int image_relink_function(CL_Function *fn, char *namebuf,
                                 uint32_t namebuf_len)
{
    CL_Symbol *sym;
    CL_Package *pkg;
    CL_String *sname, *pname;
    CL_CFunc cf;

    namebuf[0] = '\0';
    if (!CL_SYMBOL_P(fn->name)) goto stale;
    sym = (CL_Symbol *)CL_OBJ_TO_PTR(fn->name);
    if (!CL_STRING_P(sym->name)) goto stale;
    sname = (CL_String *)CL_OBJ_TO_PTR(sym->name);
    snprintf(namebuf, namebuf_len, "%.*s",
             (int)(sname->length > 200 ? 200 : sname->length), sname->data);
    if (!CL_PACKAGE_P(sym->package)) goto stale;
    pkg = (CL_Package *)CL_OBJ_TO_PTR(sym->package);
    if (!CL_STRING_P(pkg->name)) goto stale;
    pname = (CL_String *)CL_OBJ_TO_PTR(pkg->name);

    cf = cl_builtin_registry_lookup(pname->data, pname->length,
                                    sname->data, sname->length);
    if (!cf) goto stale;
    fn->func = cf;
    return 0;

stale:
    fn->func = bi_stale_builtin;
    return -1;
}

int cl_image_restore_staged(void)
{
    ImgReader r;
    CL_ImageHeader *h = &image_staged_hdr;
    const uint8_t *roots_sec, *arena_sec;
    uint32_t rt_mask, rt_bytes;
    const uint8_t *rt_pool;
    CL_Obj th_package, th_wrapper;
    uint32_t th_ntlv;
    const uint8_t *tlv_sec;
    uint32_t src_sec_pos, n_sources, blob_sec_pos, outbuf_sec_pos;
    const char **src_tab = NULL;
    uint32_t i;
    CL_Thread *mt = cl_main_thread_ptr;
    int stale_count = 0;
    char stale_first[256];

    if (!image_staged_path) return -1;

    /* ---- Pre-arena verification: everything checked BEFORE any byte of
     * the current heap is touched, so the caller can still fall back. ---- */

    if ((uint32_t)cl_gc_global_root_count() != h->boot_roots) {
        char buf[300];
        snprintf(buf, sizeof(buf),
                 "; --image: boot-root count mismatch (image %u, this "
                 "process %d) - the image was written by a different build "
                 "of clamiga; delete it or rebuild it with EXT:SAVE-IMAGE\n",
                 (unsigned)h->boot_roots, cl_gc_global_root_count());
        platform_write_string(buf);
        return -1;
    }
    if (h->bump > cl_heap.arena_size) {
        char buf[300];
        snprintf(buf, sizeof(buf),
                 "; --image: image heap (%u KB) exceeds the arena (%u KB) - "
                 "start clamiga with a larger --heap\n",
                 (unsigned)(h->bump >> 10),
                 (unsigned)(cl_heap.arena_size >> 10));
        platform_write_string(buf);
        return -1;
    }
    if (image_load_staged_payload() != 0)
        return -1;

    r.data = (const uint8_t *)image_staged_buf;
    r.size = (uint32_t)image_staged_size;
    r.pos = CL_IMAGE_HEADER_BYTES;
    r.error = 0;

    /* Section 1: ROOTS */
    roots_sec = ir_ptr(&r, h->n_roots * 4u);

    /* Section 2: READTABLES */
    rt_mask = ir_u32(&r);
    rt_bytes = ir_u32(&r);
    rt_pool = ir_ptr(&r, rt_bytes);
    if (r.error) {
        char buf[400];
        snprintf(buf, sizeof(buf),
                 "; --image: \"%.300s\" is truncated (%lu bytes) - an "
                 "interrupted save or copy; rebuild it with EXT:SAVE-IMAGE\n",
                 image_staged_path, image_staged_size);
        platform_write_string(buf);
        return -1;
    }
    if (rt_bytes != (uint32_t)sizeof(cl_readtable_pool)) {
        platform_write_string(
            "; --image: readtable section mismatch - the image was written "
            "by a different build of clamiga\n");
        return -1;
    }

    /* Section 3: THREAD0 */
    ir_bytes(&r, &th_package, 4);
    ir_bytes(&r, &th_wrapper, 4);
    th_ntlv = ir_u32(&r);
    if (th_ntlv > CL_TLV_TABLE_SIZE) {
        platform_write_string("; --image: corrupt THREAD0 section\n");
        return -1;
    }
    tlv_sec = ir_ptr(&r, th_ntlv * 8u);

    /* Section 4: SOURCES — bounds only; interned once the heap is ours. */
    n_sources = ir_u32(&r);
    src_sec_pos = r.pos;
    if (n_sources > IMG_MAX_SOURCES)
        r.error = 1;
    for (i = 0; i < n_sources && !r.error; i++)
        (void)ir_ptr(&r, ir_u16(&r));

    /* Section 5: BLOBS — structural scan (bounds and source indices; attach
     * comes after the arena is in place). */
    blob_sec_pos = r.pos;
    for (i = 0; i < h->n_blobs && !r.error; i++) {
        uint32_t code_len;
        uint16_t n_const, lm_count;
        uint8_t n_keys;
        (void)ir_u32(&r);               /* bc_offset */
        code_len = ir_u32(&r);
        (void)ir_ptr(&r, code_len);
        n_const = ir_u16(&r);
        (void)ir_ptr(&r, (uint32_t)n_const * 4u);
        n_keys = ir_u8(&r);
        (void)ir_ptr(&r, (uint32_t)n_keys * 6u);
        lm_count = ir_u16(&r);
        (void)ir_ptr(&r, (uint32_t)lm_count * (uint32_t)sizeof(CL_LineEntry));
        if (ir_u16(&r) > n_sources)
            r.error = 1;                /* source index past the table */
    }
    outbuf_sec_pos = r.pos;
    {
        uint32_t n_outbufs = ir_u32(&r);
        for (i = 0; i < n_outbufs && !r.error; i++) {
            uint32_t len;
            (void)ir_u32(&r);           /* handle */
            len = ir_u32(&r);
            (void)ir_ptr(&r, len);
        }
    }

    /* Section 6: ARENA — must be exactly the trailing h->bump bytes. */
    arena_sec = ir_ptr(&r, h->bump);
    if (r.error || !roots_sec || !arena_sec || r.pos != r.size) {
        platform_write_string(
            "; --image: file is truncated or structurally corrupt - "
            "delete it or rebuild it with EXT:SAVE-IMAGE\n");
        return -1;
    }

    /* ---- Point of no return: replace the heap. ---- */

    memcpy(cl_heap.arena, arena_sec, h->bump);
    cl_mem_adopt_image_begin(h->bump);

    /* The virgin boot's per-thread Lisp references (mv_values, reader/
     * printer state, ...) are stale offsets into the heap just replaced;
     * gc_visit_thread_roots walks them unconditionally. */
    cl_thread_reset_lisp_state(mt);

    /* Roots: registered globals by index, then the named shared tables. */
    for (i = 0; i < h->boot_roots; i++) {
        CL_Obj v;
        memcpy(&v, roots_sec + i * 4u, 4);
        cl_gc_global_root_set((int)i, v);
    }
    for (i = 0; i < N_IMAGE_NAMED_ROOTS; i++) {
        CL_Obj v;
        memcpy(&v, roots_sec + (h->boot_roots + i) * 4u, 4);
        *image_named_root((int)i) = v;
    }

    /* Readtable pool: syntax bytes, case modes and the CL_Obj macro/
     * dispatch arrays (arena offsets — valid as-is). */
    memcpy(cl_readtable_pool, rt_pool, sizeof(cl_readtable_pool));
    cl_readtable_alloc_mask = rt_mask;

    /* THREAD0: main thread's persistent slots. */
    mt->current_package = th_package;
    for (i = 0; i < th_ntlv; i++) {
        CL_Obj s, v;
        memcpy(&s, tlv_sec + i * 8u, 4);
        memcpy(&v, tlv_sec + i * 8u + 4, 4);
        cl_tlv_set(mt, s, v);
    }
    *cl_main_thread_lisp_obj_ptr() = th_wrapper;

    /* Outbufs: re-materialize BEFORE the walk so stream relinking can
     * verify each live string-output-stream's handle resolves. */
    {
        ImgReader ob;
        uint32_t n_outbufs;
        ob.data = r.data;
        ob.size = r.size;
        ob.pos = outbuf_sec_pos;
        ob.error = 0;
        n_outbufs = ir_u32(&ob);
        for (i = 0; i < n_outbufs; i++) {
            uint32_t hd = ir_u32(&ob);
            uint32_t len = ir_u32(&ob);
            const uint8_t *data = ir_ptr(&ob, len);
            if (ob.error ||
                cl_stream_outbuf_install_at(hd, (const char *)data, len) != 0)
                image_fatal("outbuf slot %u could not be re-materialized "
                            "(len %u)", (unsigned)hd, (unsigned)len);
        }
    }

    /* Source files: each name interned once, before the walk hands the
     * pointers out by index. */
    if (n_sources) {
        ImgReader sr;
        sr.data = r.data;
        sr.size = r.size;
        sr.pos = src_sec_pos;
        sr.error = 0;
        src_tab = (const char **)platform_alloc(
            n_sources * (uint32_t)sizeof(*src_tab));
        if (!src_tab)
            image_fatal("out of memory attaching %u source-file names",
                        (unsigned)n_sources, 0);
        for (i = 0; i < n_sources; i++) {
            uint16_t len = ir_u16(&sr);
            const uint8_t *p = ir_ptr(&sr, len);
            char *name = (char *)platform_alloc((uint32_t)len + 1);
            if (!name)
                image_fatal("out of memory attaching source-file name %u "
                            "(%u bytes)", (unsigned)i, (unsigned)len);
            memcpy(name, p, len);
            name[len] = '\0';
            src_tab[i] = cl_intern_source_file(name);
            platform_free(name);
        }
    }

    /* ---- The relink walk: one linear pass over the arena iterator
     * (cl_mem_adopt_image_begin set the bump front to the payload's).
     * Also the integrity check — any malformed header aborts cleanly. ---- */
    {
        ImgReader br;
        uint32_t blob_idx = 0;
        GcWalk gw;
        uint32_t off, size;
        uint8_t type;
        char namebuf[256];

        br.data = r.data;
        br.size = r.size;
        br.pos = blob_sec_pos;
        br.error = 0;
        stale_first[0] = '\0';

        gc_walk_begin(&gw);
        while (gc_walk_next(&gw, &off, &size, &type)) {
            uint8_t *ptr = cl_heap.arena + off;

            if ((size & (CL_ALIGN - 1)) != 0 || off + size > h->bump ||
                type > CL_TYPE_MAX)
                image_fatal("bad object header 0x%08x at offset 0x%08x",
                            (unsigned)CL_HDR(ptr), (unsigned)off);

            switch (type) {
            case TYPE_FUNCTION:
                if (image_relink_function((CL_Function *)ptr, namebuf,
                                          sizeof(namebuf)) != 0) {
                    stale_count++;
                    if (stale_first[0] == '\0')
                        snprintf(stale_first, sizeof(stale_first), "%.200s",
                                 namebuf[0] ? namebuf : "<anonymous>");
                }
                break;

            case TYPE_BYTECODE: {
                CL_Bytecode *bc = (CL_Bytecode *)ptr;
                uint32_t b_off, code_len;
                uint16_t n_const, lm_count, sf_idx;
                uint8_t n_keys;
                const uint8_t *p;

                if (blob_idx >= h->n_blobs)
                    image_fatal("bytecode at offset 0x%08x has no side-"
                                "buffer blob (%u blobs consumed)",
                                (unsigned)off, (unsigned)blob_idx);
                b_off = ir_u32(&br);
                if (b_off != off)
                    image_fatal("blob/bytecode offset mismatch: blob "
                                "0x%08x vs object 0x%08x",
                                (unsigned)b_off, (unsigned)off);
                blob_idx++;

                code_len = ir_u32(&br);
                p = ir_ptr(&br, code_len);
                if (code_len) {
                    bc->code = (uint8_t *)platform_alloc(code_len);
                    if (!bc->code)
                        image_fatal("out of memory attaching %u bytecode "
                                    "bytes at 0x%08x", (unsigned)code_len,
                                    (unsigned)off);
                    memcpy(bc->code, p, code_len);
                } else {
                    bc->code = NULL;
                }
                bc->code_len = code_len;

                n_const = ir_u16(&br);
                p = ir_ptr(&br, (uint32_t)n_const * 4u);
                if (n_const) {
                    bc->constants =
                        (CL_Obj *)platform_alloc((uint32_t)n_const * 4u);
                    if (!bc->constants)
                        image_fatal("out of memory attaching %u constants "
                                    "at 0x%08x", (unsigned)n_const,
                                    (unsigned)off);
                    memcpy(bc->constants, p, (uint32_t)n_const * 4u);
                } else {
                    bc->constants = NULL;
                }
                bc->n_constants = n_const;

                n_keys = ir_u8(&br);
                p = ir_ptr(&br, (uint32_t)n_keys * 6u);
                if (n_keys) {
                    bc->key_syms =
                        (CL_Obj *)platform_alloc((uint32_t)n_keys * 4u);
                    bc->key_slots = (uint8_t *)platform_alloc(n_keys);
                    bc->key_suppliedp_slots =
                        (uint8_t *)platform_alloc(n_keys);
                    if (!bc->key_syms || !bc->key_slots ||
                        !bc->key_suppliedp_slots)
                        image_fatal("out of memory attaching %u key params "
                                    "at 0x%08x", (unsigned)n_keys,
                                    (unsigned)off);
                    memcpy(bc->key_syms, p, (uint32_t)n_keys * 4u);
                    memcpy(bc->key_slots, p + (uint32_t)n_keys * 4u, n_keys);
                    memcpy(bc->key_suppliedp_slots,
                           p + (uint32_t)n_keys * 5u, n_keys);
                } else {
                    bc->key_syms = NULL;
                    bc->key_slots = NULL;
                    bc->key_suppliedp_slots = NULL;
                }
                bc->n_keys = n_keys;

                lm_count = ir_u16(&br);
                p = ir_ptr(&br, (uint32_t)lm_count *
                                 (uint32_t)sizeof(CL_LineEntry));
                if (lm_count) {
                    bc->line_map = (CL_LineEntry *)platform_alloc(
                        (uint32_t)lm_count * (uint32_t)sizeof(CL_LineEntry));
                    if (!bc->line_map)
                        image_fatal("out of memory attaching line map at "
                                    "0x%08x", (unsigned)off, 0);
                    memcpy(bc->line_map, p,
                           (uint32_t)lm_count * (uint32_t)sizeof(CL_LineEntry));
                } else {
                    bc->line_map = NULL;
                }
                bc->line_map_count = lm_count;

                sf_idx = ir_u16(&br);   /* <= n_sources: checked by the scan */
                bc->source_file = sf_idx ? src_tab[sf_idx - 1] : NULL;

                /* Native code does not survive the image: the JIT compiles
                 * the function again once it turns hot (jit.h), as after a
                 * FASL load.  The saved call count -- mostly "settled",
                 * because the saving session compiled or rejected the
                 * function -- starts over; the (speed 3) hint stays. */
                bc->native_code = NULL;
                bc->native_len = 0;
                bc->native_relocs = NULL;
                bc->native_reloc_count = 0;
                bc->jit_hot &= CL_BC_JIT_SPEED;

                if (br.error)
                    image_fatal("blob section truncated at bytecode "
                                "0x%08x", (unsigned)off, 0);
                break;
            }

            case TYPE_STREAM: {
                CL_Stream *st = (CL_Stream *)ptr;
                switch (st->stream_type) {
                case CL_STREAM_CONSOLE:
                case CL_STREAM_SYNONYM:
                case CL_STREAM_TWO_WAY:
                case CL_STREAM_BROADCAST:
                case CL_STREAM_CONCATENATED:
                case CL_STREAM_ECHO:
                    break;   /* no OS handle involved / children are arena refs */
                case CL_STREAM_STRING:
                    if (st->out_buf_handle != 0 &&
                        !cl_stream_outbuf_data(st->out_buf_handle)) {
                        if (st->flags & CL_STREAM_FLAG_OPEN)
                            image_fatal("open string stream at 0x%08x "
                                        "references missing outbuf %u",
                                        (unsigned)off,
                                        (unsigned)st->out_buf_handle);
                        st->out_buf_handle = 0;   /* closed: drop stale handle */
                    }
                    break;
                case CL_STREAM_FILE:
                case CL_STREAM_SOCKET:
                    /* Cannot exist (save precondition).  Defensive: dead. */
                    st->flags &= ~(uint32_t)CL_STREAM_FLAG_OPEN;
                    st->flags |= CL_STREAM_FLAG_EOF;
                    st->handle_id = 0;
                    break;
                case CL_STREAM_CBUF:
                    /* Load-time C-buffer stream: the buffer died with the
                     * saving process. */
                    st->flags &= ~(uint32_t)CL_STREAM_FLAG_OPEN;
                    st->flags |= CL_STREAM_FLAG_EOF;
                    st->handle_id = 0;
                    break;
                default:
                    image_fatal("stream at 0x%08x has unknown stream_type "
                                "%u", (unsigned)off,
                                (unsigned)st->stream_type);
                }
                break;
            }

            case TYPE_LOCK: {
                /* A lock is plain heap data; only its dynamic state needs
                 * resetting.  The saving process's owner serial and depth
                 * mean nothing here (a saved image cannot meaningfully
                 * contain a held lock), so every restored lock is free. */
                CL_Lock *lk = (CL_Lock *)ptr;
                lk->state = 0;
                lk->depth = 0;
                break;
            }

            case TYPE_CONDVAR: {
                /* Same: no thread of this process is registered on it. */
                CL_CondVar *cv = (CL_CondVar *)ptr;
                cv->waiters = 0;
                break;
            }

            case TYPE_THREAD: {
                CL_ThreadObj *to = (CL_ThreadObj *)ptr;
                if (CL_PTR_TO_OBJ(ptr) == th_wrapper) {
                    /* The saved main-thread wrapper: re-bind to the live
                     * main thread. */
                    to->thread_id = 0;
                    to->table_gen = cl_thread_table_gen[0];
                } else {
                    /* Any other wrapper's thread died with the saving
                     * process: force the table_gen mismatch so JOIN/
                     * INTERRUPT report "thread no longer exists" — the
                     * existing stale-wrapper semantics. */
                    uint32_t id = to->thread_id;
                    if (id >= CL_MAX_THREADS) id = 0;
                    to->thread_id = id;
                    to->table_gen = cl_thread_table_gen[id] - 1u;
                }
                break;
            }

            case TYPE_FOREIGN_POINTER: {
                /* Raw machine addresses / dlopen handles from the saving
                 * process are meaningless here.  FFI deref/call paths
                 * reject a null address with a clean error; rebuild such
                 * state via EXT:*RESTORE-HOOKS*. */
                CL_ForeignPtr *fp = (CL_ForeignPtr *)ptr;
                fp->address = 0;
                fp->size = 0;
                fp->flags = 0;
                break;
            }

            default:
                /* Fully arena-contained — nothing to do. */
                break;
            }
        }
        if (!gc_walk_complete(&gw))      /* a size-0 header ended the walk */
            image_fatal("bad object header 0x%08x at offset 0x%08x",
                        (unsigned)CL_HDR(cl_heap.arena + gw.off),
                        (unsigned)gw.off);

        if (blob_idx != h->n_blobs)
            image_fatal("blob count mismatch: %u blobs in file, %u "
                        "bytecodes in heap", (unsigned)h->n_blobs,
                        (unsigned)blob_idx);
    }
    if (src_tab) platform_free((void *)src_tab);

    if (stale_count > 0) {
        char buf[400];
        snprintf(buf, sizeof(buf),
                 "; Warning: %d builtin function(s) in the image have no "
                 "implementation in this build (first: %.200s) - calling "
                 "one will signal an error.  Rebuild the image with "
                 "EXT:SAVE-IMAGE.\n", stale_count, stale_first);
        platform_write_string(buf);
    }

    /* Re-sync the C-level main-thread record with the restored wrapper. */
    if (CL_HEAP_P(th_wrapper) &&
        CL_HDR_TYPE(CL_OBJ_TO_PTR(th_wrapper)) == TYPE_THREAD) {
        CL_ThreadObj *to = (CL_ThreadObj *)CL_OBJ_TO_PTR(th_wrapper);
        mt->name = to->name;
    }

    /* Offset-keyed reader diagnostics: stale by definition. */
    for (i = 0; i < CL_SRCLOC_SIZE; i++)
        cl_srcloc_table[i].cons_obj = CL_NIL;

    /* Normalize collector state for the adopted payload (mark bits,
     * GenGC watermark/crossing map/protection). */
    cl_mem_adopt_image_finish();

    /* Process-derived Lisp state came back from the SAVING process and
     * must be re-derived from this one, exactly as a fresh boot derives it
     * (the heap is a normal heap again here, so these may allocate):
     *   *DEFAULT-PATHNAME-DEFAULTS* — the saver's cwd; for a shipped
     *     clamiga.img a directory on the build machine.  REQUIRE/LOAD
     *     check existence against the real cwd but merge the path against
     *     this, so a relative load "found" a file and then failed to open it.
     *   *RANDOM-STATE* — the saver's state, seeded from its clock: every
     *     restore would replay the same (random) sequence. */
    cl_pathname_default_from_cwd();
    if (CL_SYMBOL_P(SYM_RANDOM_STATE)) {
        CL_Obj rs = cl_make_random_state(platform_time_ms() ^
                                         platform_universal_time());
        ((CL_Symbol *)CL_OBJ_TO_PTR(SYM_RANDOM_STATE))->value = rs;
    }

    /* EXT:*IMAGE-RESTORED-P* — set before .clamigarc runs so an rc file
     * can skip redundant loads. */
    if (CL_SYMBOL_P(SYM_IMAGE_RESTORED_P))
        ((CL_Symbol *)CL_OBJ_TO_PTR(SYM_IMAGE_RESTORED_P))->value = SYM_T;

    image_restored = 1;
    cl_image_discard_staged();
    return 0;
}

int cl_image_run_toplevel(void)
{
    CL_Obj fn;
    int err;

    if (!image_embedded || CL_NULL_P(SYM_IMAGE_TOPLEVEL))
        return 0;
    fn = cl_symbol_value(SYM_IMAGE_TOPLEVEL);
    if (fn == CL_UNBOUND || CL_NULL_P(fn))
        return 0;

    cl_vm.sp = 0;
    cl_vm.fp = 0;
    CL_GC_PROTECT(fn);
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        fn = cl_coerce_funcdesig(fn, "the executable's :TOPLEVEL");
        cl_vm_apply(fn, NULL, 0);
        CL_UNCATCH();
    } else if (err == CL_ERR_EXIT) {
        CL_UNCATCH();                   /* (QUIT n): cl_exit_code is set */
    } else {
        /* An error nothing handled ends the program, and says so in the
         * exit status: this is a program now, not a session to recover. */
        cl_error_print();
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        CL_UNCATCH();
        cl_exit_code = 1;
    }
    CL_GC_UNPROTECT(1);
    return 1;
}

/* ================================================================
 * Lisp surface: EXT:SAVE-IMAGE + hook variables
 * ================================================================ */

/* Coerce a pathname designator (string / wide string / pathname) to a C
 * path in BUF.  Returns BUF or NULL. */
static const char *image_coerce_path(CL_Obj obj, char *buf, uint32_t buflen)
{
    if (CL_PATHNAME_P(obj)) {
        extern const char *cl_coerce_to_namestring(CL_Obj, char *, uint32_t);
        cl_coerce_to_namestring(obj, buf, buflen);
        return buf;
    }
#ifdef CL_WIDE_STRINGS
    if (CL_WIDE_STRING_P(obj)) {
        uint32_t nb = cl_wide_string_to_utf8(obj, buf, buflen - 1);
        buf[nb] = '\0';
        return buf;
    }
#endif
    if (CL_STRING_P(obj)) {
        CL_String *s = (CL_String *)CL_OBJ_TO_PTR(obj);
        if (s->length >= buflen) return NULL;
        memcpy(buf, s->data, s->length);
        buf[s->length] = '\0';
        return buf;
    }
    return NULL;
}

static CL_Obj bi_save_image(CL_Obj *args, int nargs)
{
    char path[1024];
    int quit = 0, shake = 0, exe = 0;
    int toplevel_at = 0, heap_given = 0;   /* args[] index / flag */
    uint32_t heap = 0;
    int i;

    if (!image_coerce_path(args[0], path, sizeof(path)))
        cl_error(CL_ERR_TYPE,
                 "SAVE-IMAGE: pathname designator expected (string or "
                 "pathname)");

    for (i = 1; i + 1 < nargs; i += 2) {
        if (args[i] == KW_QUIT_IMG)
            quit = !CL_NULL_P(args[i + 1]);
        else if (args[i] == KW_SHAKE_BINDINGS)
            shake = !CL_NULL_P(args[i + 1]);
        else if (args[i] == KW_EXECUTABLE)
            exe = !CL_NULL_P(args[i + 1]);
        else if (args[i] == KW_TOPLEVEL)
            toplevel_at = CL_NULL_P(args[i + 1]) ? 0 : i + 1;
        else if (args[i] == KW_HEAP_SIZE) {
            if (!CL_NULL_P(args[i + 1])) {
                if (!CL_FIXNUM_P(args[i + 1]) ||
                    CL_FIXNUM_VAL(args[i + 1]) <= 0 ||
                    (int64_t)CL_FIXNUM_VAL(args[i + 1]) > (int64_t)UINT32_MAX)
                    cl_error(CL_ERR_TYPE,
                             "SAVE-IMAGE: :HEAP-SIZE must be a positive "
                             "number of bytes, e.g. (* 16 1024 1024)");
                heap = (uint32_t)CL_FIXNUM_VAL(args[i + 1]);
                heap_given = 1;
            }
        } else
            cl_error(CL_ERR_ARGS,
                     "SAVE-IMAGE: unknown keyword argument (:QUIT, "
                     ":SHAKE-BINDINGS, :EXECUTABLE, :TOPLEVEL and :HEAP-SIZE "
                     "are accepted)");
    }
    if (i != nargs)
        cl_error(CL_ERR_ARGS, "SAVE-IMAGE: odd number of keyword arguments");
    if (!exe && (toplevel_at || heap_given))
        cl_error(CL_ERR_ARGS,
                 "SAVE-IMAGE: :TOPLEVEL and :HEAP-SIZE describe a delivered "
                 "executable - add :EXECUTABLE T");
    /* A typo in the entry point must fail the build, not the first start
     * of the shipped program. */
    if (toplevel_at) {
        CL_Obj tl = args[toplevel_at];
        if (CL_SYMBOL_P(tl)) {
            CL_Obj fn = ((CL_Symbol *)CL_OBJ_TO_PTR(tl))->function;
            if (fn == CL_UNBOUND || CL_NULL_P(fn))
                cl_error(CL_ERR_UNDEFINED,
                         "SAVE-IMAGE: the :TOPLEVEL function %s is not "
                         "defined - load the program before saving it",
                         cl_symbol_name(tl));
        } else
            cl_coerce_funcdesig(tl, "SAVE-IMAGE :TOPLEVEL");
    }

    if (exe)
        cl_image_save_executable_request(path, quit, shake, heap);
    else
        cl_image_save_request(path, quit, shake);
    /* The designator as given (a symbol stays a symbol), or NIL: a plain
     * image saved after an executable must not inherit its entry point. */
    ((CL_Symbol *)CL_OBJ_TO_PTR(SYM_IMAGE_TOPLEVEL))->value =
        toplevel_at ? args[toplevel_at] : CL_NIL;

    /* The dump itself runs after this top-level form finishes, at the
     * next safe point where the main thread is at rest (spec).  Return
     * the pathname so scripts can report it. */
    return cl_make_string(path, (uint32_t)strlen(path));
}

/* Intern an EXT special variable with initial value NIL, export it unless
 * it is one of the runtime's internals, and root the C handle. */
static CL_Obj image_defvar_ext(const char *name, CL_Obj *slot, int exported)
{
    CL_Obj sym = cl_intern_in(name, (uint32_t)strlen(name), cl_package_ext);
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    s->flags |= CL_SYM_SPECIAL;
    s->value = CL_NIL;
    *slot = sym;
    cl_gc_register_root(slot);
    if (exported)
        cl_export_symbol(sym, cl_package_ext);
    return *slot;   /* the export can collect: *slot is the rooted copy */
}

void cl_image_builtins_init(void)
{
    image_defvar_ext("*SAVE-HOOKS*", &SYM_SAVE_HOOKS, 1);
    image_defvar_ext("*RESTORE-HOOKS*", &SYM_RESTORE_HOOKS, 1);
    image_defvar_ext("*SYSTEM-RESTORE-HOOKS*", &SYM_SYSTEM_RESTORE_HOOKS, 0);
    image_defvar_ext("*IMAGE-RESTORED-P*", &SYM_IMAGE_RESTORED_P, 1);
    image_defvar_ext("*IMAGE-TOPLEVEL*", &SYM_IMAGE_TOPLEVEL, 0);

    KW_QUIT_IMG = cl_intern_keyword("QUIT", 4);
    cl_gc_register_root(&KW_QUIT_IMG);
    KW_SHAKE_BINDINGS = cl_intern_keyword("SHAKE-BINDINGS", 14);
    cl_gc_register_root(&KW_SHAKE_BINDINGS);
    KW_EXECUTABLE = cl_intern_keyword("EXECUTABLE", 10);
    cl_gc_register_root(&KW_EXECUTABLE);
    KW_TOPLEVEL = cl_intern_keyword("TOPLEVEL", 8);
    cl_gc_register_root(&KW_TOPLEVEL);
    KW_HEAP_SIZE = cl_intern_keyword("HEAP-SIZE", 9);
    cl_gc_register_root(&KW_HEAP_SIZE);

    cl_register_builtin_exported("SAVE-IMAGE", bi_save_image, 1, 11,
                                 cl_package_ext);
}
