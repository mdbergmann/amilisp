#include "compiler_internal.h"
#include "peephole.h"
#include "thread.h"
#include "../platform/platform_thread.h"
#include "../jit/jit.h"
#include <stdio.h>

/* --- Shared globals --- */
/* cl_active_compiler and pending_lambda_name are now in CL_Thread */

CL_Obj macro_table = CL_NIL;
CL_Obj setf_table = CL_NIL;
CL_Obj setf_fn_table = CL_NIL;  /* (setf name) functions: ((accessor . setf-fn-sym) ...) val-first calling */
CL_Obj setf_expander_table = CL_NIL;  /* define-setf-expander: ((name . expander-fn) ...) */
CL_Obj type_table = CL_NIL;
CL_Obj compiler_macro_table = CL_NIL;  /* define-compiler-macro: ((name . expander-fn) ...) */

void *cl_tables_rwlock = NULL;

/* Forward declarations for the CL_Compiler pool helpers — defined near
 * cl_compile.  Declared here so compile_lambda (above the definitions)
 * can use them. */
static CL_Compiler *cl_compiler_pool_acquire(void);
static void cl_compiler_pool_release(CL_Compiler *c);
/* The CL_Compiler pool's mutex (lazily initialized by
 * cl_compiler_pool_init, near the pool below). Declared this early so
 * compiler_grow_code/compiler_grow_constants can guard the shared
 * high-water-mark globals with it — those run on whichever thread is
 * compiling, with no other synchronization of their own. */
static void *cl_compiler_pool_lock;

/* Per-thread-tracked rwlock helpers (see compiler.h).
 * cl_tables_rdlock is a macro at every call site that tags the call
 * with __FILE__ ":" __LINE__ and forwards to cl_tables_rdlock_at. */
/* Lock state tracking is keyed off whether the rwlock has been
 * initialized, NOT off CL_MT() — the latter can flip from true → false
 * while a thread holds an rdlock (e.g. when the last sento worker exits
 * after a test fixture's actor-system shutdown), which would skip the
 * unlock and leave the held counter — and the underlying platform
 * rwlock — permanently leaked.  As long as cl_tables_rwlock is non-NULL
 * (initialized in cl_compiler_init), pair every rdlock with a rwunlock
 * regardless of current thread count. */
void cl_tables_rdlock_at(const char *site)
{
    if (!cl_tables_rwlock) return;
    platform_rwlock_rdlock(cl_tables_rwlock);
    if (CT->rdlock_tables_sites_top < CL_RDLOCK_SITES_MAX)
        CT->rdlock_tables_sites[CT->rdlock_tables_sites_top] = site;
    CT->rdlock_tables_sites_top++;
    CT->rdlock_tables_held++;
}

void cl_tables_wrlock(void)
{
    if (!cl_tables_rwlock) return;
    platform_rwlock_wrlock(cl_tables_rwlock);
}

void cl_tables_rwunlock(void)
{
    if (!cl_tables_rwlock) return;
    if (CT->rdlock_tables_held > 0) {
        CT->rdlock_tables_held--;
        if (CT->rdlock_tables_sites_top > 0)
            CT->rdlock_tables_sites_top--;
    }
    platform_rwlock_unlock(cl_tables_rwlock);
}

/* See compiler.h.  The cons happens OUTSIDE the write lock (cl_cons roots
 * its own arguments); the raw cell pointer derived after it stays valid
 * through the lock acquisition because this thread never parks while
 * blocked on the rwlock — a peer's stop-the-world compaction must wait
 * for us, so no relocation can happen between the cons and the store. */
void cl_table_prepend_locked(CL_Obj *table_p, CL_Obj value)
{
    CL_Obj cell = cl_cons(value, CL_NIL);
    cl_tables_wrlock();
    ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = *table_p;
    *table_p = cell;
    cl_tables_rwunlock();
}

/* --- Generic symbol->entry alist hash index (see compiler.h) --- */

static uint32_t alist_index_hash(CL_Obj obj)
{
    /* Heap offsets are CL_ALIGN-aligned, so shift the always-zero low
     * bits out before Knuth's golden-ratio multiplier (same hash as the
     * struct registry index / the FASL writer's MLF index). */
#if CL_ALIGN == 8
    return ((uint32_t)obj >> 3) * 2654435769u;
#else
    return ((uint32_t)obj >> 2) * 2654435769u;
#endif
}

/* (Re)build IX from its current alist.  Caller holds the tables wrlock.
 * Returns 1 when the index is usable; 0 disables it.  Raw cons access
 * throughout — cl_car can cl_error→longjmp out from under the held
 * wrlock (leaked-reader lesson, see the fallback in _find). */
static int alist_index_rebuild(CL_AlistIndex *ix)
{
    uint32_t count = 0, cap, mask;
    CL_AlistIndexSlot *slots;
    CL_Obj list;

    for (list = *ix->table_p; !CL_NULL_P(list); ) {
        CL_Cons *c;
        if (!CL_CONS_P(list)) goto disable;
        c = (CL_Cons *)CL_OBJ_TO_PTR(list);
        if (!CL_CONS_P(c->car)) goto disable;
        if (!CL_SYMBOL_P(((CL_Cons *)CL_OBJ_TO_PTR(c->car))->car)) goto disable;
        count++;
        list = c->cdr;
    }

    cap = 64;
    while (cap < count * 2 + 1) cap <<= 1;
    slots = (CL_AlistIndexSlot *)platform_alloc(cap * sizeof(CL_AlistIndexSlot));
    if (!slots) goto disable;
    memset(slots, 0, cap * sizeof(CL_AlistIndexSlot));   /* CL_NIL == 0 */
    mask = cap - 1;

    /* Head-first insert, first key wins — matches the linear walk's
     * first-match semantics when a key has been re-registered. */
    for (list = *ix->table_p; !CL_NULL_P(list); ) {
        CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(list);
        CL_Obj entry = c->car;
        CL_Obj name = ((CL_Cons *)CL_OBJ_TO_PTR(entry))->car;
        uint32_t slot = alist_index_hash(name) & mask;
        while (!CL_NULL_P(slots[slot].name) && slots[slot].name != name)
            slot = (slot + 1) & mask;
        if (CL_NULL_P(slots[slot].name)) {
            slots[slot].name = name;
            slots[slot].entry = entry;
        }
        list = c->cdr;
    }

    if (ix->slots) platform_free(ix->slots);
    ix->slots = slots;
    ix->cap = cap;
    ix->dirty = 0;
    return 1;

disable:
    if (ix->slots) platform_free(ix->slots);
    ix->slots = NULL;
    ix->cap = 0;
    ix->disabled = 1;
    return 0;
}

/* Probe a CLEAN index for KEY.  Caller holds the tables lock.
 * Pure memory reads — cannot error, cannot allocate. */
static CL_Obj alist_index_probe(CL_AlistIndex *ix, CL_Obj key)
{
    uint32_t mask = ix->cap - 1;
    uint32_t slot = alist_index_hash(key) & mask;
    while (!CL_NULL_P(ix->slots[slot].name)) {
        if (ix->slots[slot].name == key)
            return ix->slots[slot].entry;
        slot = (slot + 1) & mask;
    }
    return CL_NIL;
}

CL_Obj cl_alist_index_find(CL_AlistIndex *ix, CL_Obj key)
{
    CL_Obj list, result;

    cl_tables_rdlock();
    if (ix->slots && !ix->dirty && !ix->disabled) {
        result = alist_index_probe(ix, key);
        cl_tables_rwunlock();
        return result;
    }
    cl_tables_rwunlock();

    if (!ix->disabled) {
        cl_tables_wrlock();
        if (!ix->disabled &&
            (ix->slots && !ix->dirty ? 1 : alist_index_rebuild(ix))) {
            result = alist_index_probe(ix, key);
            cl_tables_rwunlock();
            return result;
        }
        cl_tables_rwunlock();
    }

    /* Fallback (index disabled): snapshot-and-release linear walk — the
     * alist is only ever PREPENDED to, so once we capture the head under
     * the rdlock we can release the lock and walk the snapshot. */
    cl_tables_rdlock();
    list = *ix->table_p;
    cl_tables_rwunlock();
    while (!CL_NULL_P(list)) {
        CL_Obj entry = cl_car(list);
        if (cl_car(entry) == key)
            return entry;
        list = cl_cdr(list);
    }
    return CL_NIL;
}

void cl_alist_index_prepend(CL_AlistIndex *ix, CL_Obj value)
{
    /* Cons OUTSIDE the write lock (arena allocation under the wrlock is
     * an STW-vs-rwlock deadlock — see cl_table_prepend_locked, whose
     * shape this mirrors); prepend + dirty-mark are ONE critical section
     * so a probe of a clean index is always consistent with the alist. */
    CL_Obj cell = cl_cons(value, CL_NIL);
    cl_tables_wrlock();
    ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = *ix->table_p;
    *ix->table_p = cell;
    ix->dirty = 1;
    cl_tables_rwunlock();
}

void cl_alist_index_invalidate(CL_AlistIndex *ix)
{
    ix->dirty = 1;
}

void cl_alist_index_reset(CL_AlistIndex *ix)
{
    if (ix->slots) platform_free(ix->slots);
    ix->slots = NULL;
    ix->cap = 0;
    ix->dirty = 0;
    ix->disabled = 0;
}

void cl_tables_dump_rdlock_holders(const char *header)
{
    int i;
    if (!CL_MT()) return;
    fprintf(stderr, "%s\n", header ? header : "[rwlock] cl_tables_rwlock readers:");
    for (i = 0; i < CL_MAX_THREADS; i++) {
        CL_Thread *t = cl_thread_table[i];
        if (t && t->rdlock_tables_held > 0) {
            int s, top;
            fprintf(stderr, "  thread tid=%u status=%u name=", t->id, t->status);
            if (CL_NULL_P(t->name)) {
                fprintf(stderr, "(unnamed)");
            } else if (CL_STRING_P(t->name)) {
                CL_String *str = (CL_String *)CL_OBJ_TO_PTR(t->name);
                fprintf(stderr, "\"%.*s\"", (int)str->length, str->data);
            } else {
                fprintf(stderr, "?");
            }
            fprintf(stderr, " held=%d\n", t->rdlock_tables_held);
            top = t->rdlock_tables_sites_top;
            if (top > CL_RDLOCK_SITES_MAX) top = CL_RDLOCK_SITES_MAX;
            for (s = top - 1; s >= 0; s--) {
                fprintf(stderr, "    [%d] %s\n", s,
                        t->rdlock_tables_sites[s]
                          ? t->rdlock_tables_sites[s] : "(unknown)");
            }
        }
    }
    fflush(stderr);
}

CL_Obj SETF_SYM_CAR = CL_NIL;
CL_Obj SETF_SYM_CDR = CL_NIL;
CL_Obj SETF_SYM_FIRST = CL_NIL;
CL_Obj SETF_SYM_REST = CL_NIL;
CL_Obj SETF_SYM_NTH = CL_NIL;
CL_Obj SETF_SYM_AREF = CL_NIL;
CL_Obj SETF_SYM_SVREF = CL_NIL;
CL_Obj SETF_SYM_CHAR = CL_NIL;
CL_Obj SETF_SYM_SCHAR = CL_NIL;
CL_Obj SETF_SYM_SYMBOL_VALUE = CL_NIL;
CL_Obj SETF_SYM_SYMBOL_FUNCTION = CL_NIL;
CL_Obj SETF_SYM_SYMBOL_PLIST = CL_NIL;
CL_Obj SETF_SYM_FDEFINITION = CL_NIL;
CL_Obj SETF_HELPER_NTH = CL_NIL;
CL_Obj SETF_HELPER_SV = CL_NIL;
CL_Obj SETF_HELPER_SF = CL_NIL;
CL_Obj SETF_HELPER_SP = CL_NIL;
CL_Obj SETF_SYM_GETHASH = CL_NIL;
CL_Obj SETF_HELPER_GETHASH = CL_NIL;
CL_Obj SETF_HELPER_AREF = CL_NIL;
CL_Obj SETF_SYM_ROW_MAJOR_AREF = CL_NIL;
CL_Obj SETF_HELPER_ROW_MAJOR_AREF = CL_NIL;
CL_Obj SETF_SYM_FILL_POINTER = CL_NIL;
CL_Obj SETF_HELPER_FILL_POINTER = CL_NIL;
CL_Obj SETF_SYM_BIT = CL_NIL;
CL_Obj SETF_HELPER_BIT = CL_NIL;
CL_Obj SETF_SYM_SBIT = CL_NIL;
CL_Obj SETF_HELPER_SBIT = CL_NIL;
CL_Obj SETF_SYM_GET = CL_NIL;
CL_Obj SETF_HELPER_GET = CL_NIL;
CL_Obj SETF_SYM_GETF = CL_NIL;
CL_Obj SETF_HELPER_GETF = CL_NIL;

/* Sentinel symbol used inside the lex-env alist built by
 * cl_build_lex_env to mark MACROLET-bound expanders.  An entry of the
 * form (SYM_LEX_LOCAL_MACRO . (name . expander-fn)) carries a local
 * macro; an ordinary (name . expansion-form) entry carries a symbol
 * macro.  Lookup walkers check the car against this sentinel to
 * disambiguate. */
CL_Obj SYM_LEX_LOCAL_MACRO = CL_NIL;

/* Proclaimed optimize baseline (DECLAIM/PROCLAIM).  Per-compile effective
 * settings live in CL_Compiler.optimize_settings instead of a shared
 * global — see compiler.h and cl_compiler_seed_optimize_settings below. */
CL_OptimizeSettings cl_optimize_global   = {1, 1, 1, 1};

/* AMILISP_FORCE_SPEED=0..3 pins the effective speed for every compile in
 * the process, overriding both the proclaimed baseline and body (declare
 * (optimize (speed ...))) forms.  -1 = no override.  Set once at startup
 * (main.c) before any user code compiles; the differential test harness
 * uses it to run an unmodified corpus with the peephole pass forced fully
 * on (3) and fully off (0) and compare results. */
int cl_optimize_force_speed = -1;

/* Local-function inlining switch — see compiler_internal.h; main.c clears
 * it for AMILISP_NO_LOCAL_INLINE=1. */
int cl_local_inline_enabled = 1;

/* A reference to an inlined FLET/LABELS function that the escape analysis
 * did not foresee (compile_flet's scan ran over a macro that expanded
 * differently, or one whose expansion failed in the scan).  There is no
 * closure to load, so say exactly what happened and how to get one. */
static void inline_local_misuse(CL_Compiler *c, CL_Obj name, const char *how)
{
    CL_UNUSED(c);
    cl_error(CL_ERR_GENERAL,
             "Compiler: local function %s was compiled inline (it keeps no "
             "closure), but %s reached the compiler after the analysis had "
             "ruled that out - usually a macro that expands differently on "
             "each expansion.  Add (declare (notinline %s)) to the FLET/LABELS "
             "body to keep a real closure.",
             cl_symbol_name(name), how, cl_symbol_name(name));
}

/* Seed a freshly pushed compiler's effective optimize settings: inherit
 * from the immediately enclosing compiler in the active chain, or from the
 * proclaimed baseline when COMP is the root of a fresh top-level compile
 * (no enclosing compiler on this thread).  Each CL_Compiler is exclusively
 * owned by the thread that pushed it, so the inherited-from-parent path
 * needs no lock; the global-baseline path takes cl_tables_rdlock() to pair
 * with the wrlock DECLAIM/PROCLAIM take when writing cl_optimize_global. */
static void cl_compiler_seed_optimize_settings(CL_Compiler *comp)
{
    if (comp->parent) {
        comp->optimize_settings = comp->parent->optimize_settings;
    } else {
        cl_tables_rdlock();
        comp->optimize_settings = cl_optimize_global;
        cl_tables_rwunlock();
    }
    if (cl_optimize_force_speed >= 0)
        comp->optimize_settings.speed = (uint8_t)cl_optimize_force_speed;
    comp->peep_speed_max = comp->optimize_settings.speed;
}

/* --- Source-file intern pool --- */
/* CL_Bytecode.source_file holds a const char* that must outlive the bytecode
 * (which can persist for the entire process lifetime).  Earlier versions
 * stored a pointer into bi_compile_file's stack-local in_path[] buffer; once
 * compile-file returned the pointer dangled and any later access (FASL
 * serialize, FATAL crash trace, debugger backtrace, vm dispatch) would read
 * arbitrary bytes from a re-used stack frame, occasionally corrupting CLOS
 * metadata referenced by ASDF and crashing in completely unrelated code.
 *
 * Fix: intern paths once into a process-lifetime pool and hand back a stable
 * pointer.  Linear search is fine — typical sessions touch a few hundred
 * source files at most. */
typedef struct CL_SourceFileEntry_s {
    char *path;
    struct CL_SourceFileEntry_s *next;
} CL_SourceFileEntry;

static CL_SourceFileEntry *cl_source_file_pool = NULL;
static void *cl_source_file_pool_lock = NULL;

const char *cl_intern_source_file(const char *path)
{
    CL_SourceFileEntry *e;
    char *copy;
    size_t len;

    if (!path || !path[0]) return NULL;
    len = strlen(path);

    if (!cl_source_file_pool_lock)
        platform_rwlock_init(&cl_source_file_pool_lock);

    platform_rwlock_wrlock(cl_source_file_pool_lock);
    for (e = cl_source_file_pool; e != NULL; e = e->next) {
        if (strcmp(e->path, path) == 0) {
            platform_rwlock_unlock(cl_source_file_pool_lock);
            return e->path;
        }
    }
    e = (CL_SourceFileEntry *)platform_alloc(sizeof(CL_SourceFileEntry));
    copy = (char *)platform_alloc(len + 1);
    if (!e || !copy) {
        if (e) platform_free(e);
        if (copy) platform_free(copy);
        platform_rwlock_unlock(cl_source_file_pool_lock);
        return NULL;
    }
    memcpy(copy, path, len + 1);
    e->path = copy;
    e->next = cl_source_file_pool;
    cl_source_file_pool = e;
    platform_rwlock_unlock(cl_source_file_pool_lock);
    return copy;
}

/* --- Code emission --- */

/* The bytecode and constants buffers start at these sizes and double up to
 * CL_MAX_CODE_SIZE / CL_MAX_CONSTANTS.  With fixed arrays at the caps a
 * CL_Compiler was 366 KB and the pool pre-warmed eight of them: 3 MB off-heap
 * before the first form, on machines with 8 MB.  Most functions need well
 * under a kilobyte of either. */
#define CL_COMPILER_CODE_INIT   1024
#define CL_COMPILER_CONST_INIT  64
/* A pooled compiler keeps buffers up to these sizes between compiles; larger
 * ones are freed on release (cl_compiler_pool_release). */
#define CL_COMPILER_CODE_KEEP   16384
#define CL_COMPILER_CONST_KEEP  1024

/* Largest buffers any compile grew to, for the AMILISP_MEM_DIAG report at
 * shutdown.  Written only when a buffer grows. */
static int compiler_code_cap_max = 0;
static int compiler_const_cap_max = 0;

static void compiler_grow_code(CL_Compiler *c)
{
    uint32_t cap;
    uint8_t *buf, *old;

    if (c->code_cap >= CL_MAX_CODE_SIZE)
        cl_error(CL_ERR_OVERFLOW,
                 "Bytecode too large: one compiled function may hold at most "
                 "%d bytes of bytecode -- split the form into smaller "
                 "functions", CL_MAX_CODE_SIZE);
    cap = c->code_cap > 0 ? (uint32_t)c->code_cap * 2 : CL_COMPILER_CODE_INIT;
    if (cap > CL_MAX_CODE_SIZE) cap = CL_MAX_CODE_SIZE;
    buf = (uint8_t *)platform_alloc(cap);
    if (!buf)
        cl_error(CL_ERR_STORAGE,
                 "Compiler out of memory: cannot grow the bytecode buffer "
                 "to %lu bytes", (unsigned long)cap);
    if (c->code_pos > 0)
        memcpy(buf, c->code, (size_t)c->code_pos);
    old = c->code;
    c->code = buf;
    c->code_cap = (int)cap;
    if (old) platform_free(old);
    if (cl_compiler_pool_lock) platform_mutex_lock(cl_compiler_pool_lock);
    if (c->code_cap > compiler_code_cap_max)
        compiler_code_cap_max = c->code_cap;
    if (cl_compiler_pool_lock) platform_mutex_unlock(cl_compiler_pool_lock);
}

/* No GC can run in here (platform_alloc is not the arena), so the constants
 * the GC walks through c->constants are never seen half-copied. */
static void compiler_grow_constants(CL_Compiler *c)
{
    uint32_t cap;
    CL_Obj *buf, *old;

    cap = c->const_cap > 0 ? (uint32_t)c->const_cap * 2 : CL_COMPILER_CONST_INIT;
    if (cap > CL_MAX_CONSTANTS) cap = CL_MAX_CONSTANTS;
    buf = (CL_Obj *)platform_alloc(cap * sizeof(CL_Obj));
    if (!buf)
        cl_error(CL_ERR_STORAGE,
                 "Compiler out of memory: cannot grow the constants table "
                 "to %lu entries", (unsigned long)cap);
    if (c->const_count > 0)
        memcpy(buf, c->constants, (size_t)c->const_count * sizeof(CL_Obj));
    old = c->constants;
    c->constants = buf;
    c->const_cap = (int)cap;
    if (old) platform_free(old);
    if (cl_compiler_pool_lock) platform_mutex_lock(cl_compiler_pool_lock);
    if (c->const_cap > compiler_const_cap_max)
        compiler_const_cap_max = c->const_cap;
    if (cl_compiler_pool_lock) platform_mutex_unlock(cl_compiler_pool_lock);
}

void cl_emit(CL_Compiler *c, uint8_t byte)
{
    if (c->code_pos >= c->code_cap)
        compiler_grow_code(c);          /* signals past CL_MAX_CODE_SIZE */
    c->code[c->code_pos++] = byte;
}

void cl_emit_u16(CL_Compiler *c, uint16_t val)
{
    cl_emit(c, (uint8_t)(val >> 8));
    cl_emit(c, (uint8_t)(val & 0xFF));
}

void cl_emit_i16(CL_Compiler *c, int16_t val)
{
    cl_emit_u16(c, (uint16_t)val);
}

void cl_emit_i32(CL_Compiler *c, int32_t val)
{
    cl_emit(c, (uint8_t)((val >> 24) & 0xFF));
    cl_emit(c, (uint8_t)((val >> 16) & 0xFF));
    cl_emit(c, (uint8_t)((val >> 8) & 0xFF));
    cl_emit(c, (uint8_t)(val & 0xFF));
}

int cl_add_constant(CL_Compiler *c, CL_Obj obj)
{
    int i;
    /* Check for existing identical constant */
    for (i = 0; i < c->const_count; i++) {
        if (c->constants[i] == obj) return i;
    }
    if (c->const_count >= CL_MAX_CONSTANTS) {
        cl_error(CL_ERR_OVERFLOW,
                 "Too many constants: one compiled function may reference at "
                 "most %d distinct constants -- split the form into smaller "
                 "functions", CL_MAX_CONSTANTS);
        return 0;
    }
    if (c->const_count >= c->const_cap)
        compiler_grow_constants(c);
    c->constants[c->const_count] = obj;
    return c->const_count++;
}

void cl_emit_const(CL_Compiler *c, CL_Obj obj)
{
    int idx = cl_add_constant(c, obj);
    cl_emit(c, OP_CONST);
    cl_emit_u16(c, (uint16_t)idx);
    c->mv_state = CL_MV_ONE;   /* OP_CONST pushes exactly one value */
}

int cl_emit_jump(CL_Compiler *c, uint8_t op)
{
    int pos;
    cl_emit(c, op);
    pos = c->code_pos;
    cl_emit_i32(c, 0); /* placeholder */
    return pos;
}

void cl_patch_jump(CL_Compiler *c, int patch_pos)
{
    int32_t offset = c->code_pos - (patch_pos + 4);
    c->code[patch_pos]     = (uint8_t)((offset >> 24) & 0xFF);
    c->code[patch_pos + 1] = (uint8_t)((offset >> 16) & 0xFF);
    c->code[patch_pos + 2] = (uint8_t)((offset >> 8) & 0xFF);
    c->code[patch_pos + 3] = (uint8_t)(offset & 0xFF);
}

/* --- Multiple-values hygiene ---
 *
 * See the CL_MV_* comment in compiler_internal.h for the model.  These two
 * are the whole interface: normalize before an observer, join at a merge. */

/* Emit a call opcode plus its argument count.  Centralized so the MV
 * classification happens in ONE place: the callee establishes the state and
 * may leave more than one value (CL_MV_MANY).  A raw cl_emit of OP_CALL
 * would leave whatever the last thing compiled was — typically the final
 * ARGUMENT — so a call whose last argument is a plain variable reference
 * would be classified CL_MV_STALE and the observer would reset it.  That
 * mis-classification silently dropped the second value of every function
 * whose body is a MULTIPLE-VALUE-PROG1 (serapeum's SWAPHASH). */
void cl_emit_call(CL_Compiler *c, uint8_t op, uint8_t nargs)
{
    cl_emit(c, op);
    cl_emit(c, nargs);
    c->mv_state = CL_MV_MANY;
}

/* OP_CALL_GLOBAL / OP_TAILCALL_GLOBAL: `u16 symbol-const-index, u8 nargs`.
 * Same MV classification as cl_emit_call. */
void cl_emit_call_global(CL_Compiler *c, uint8_t op, uint16_t sym_idx,
                         uint8_t nargs)
{
    cl_emit(c, op);
    cl_emit_u16(c, sym_idx);
    cl_emit(c, nargs);
    c->mv_state = CL_MV_MANY;
}

void cl_mv_normalize(CL_Compiler *c)
{
    if (c->mv_state == CL_MV_STALE) {
        cl_emit(c, OP_MV_RESET);
        c->mv_state = CL_MV_ONE;
    }
}

/* Commutative, associative merge of two arms' states.  CL_MV_ONE is the
 * identity, which makes it usable as an accumulator seed. */
int cl_mv_merge(int a, int b)
{
    if (a == b) return a;
    if (a != CL_MV_STALE && b != CL_MV_STALE)
        return CL_MV_MANY;          /* ONE + MANY: both self-describing */
    if (a == CL_MV_MANY || b == CL_MV_MANY) {
        /* STALE + MANY.  Resetting at the observer would throw away the
         * MANY arm's extra values, so keep the state and accept that the
         * stale arm still leaks — exactly what it did before this pass.
         * Normalizing the stale arm in place would need a jump stub; not
         * worth the bytes until a real program trips over it. */
        return CL_MV_MANY;
    }
    return CL_MV_STALE;             /* STALE + ONE: a later reset is a no-op */
}

void cl_mv_join(CL_Compiler *c, int other)
{
    c->mv_state = cl_mv_merge(c->mv_state, other);
}

/* Close a multi-armed join.  OK_CHAIN holds the pending jumps of arms that
 * left the MV state describing their own value, STALE_CHAIN those that left
 * it stale, and c->mv_state describes the arm that FALLS THROUGH to here.
 *
 * When the arms disagree — one of them may deliver several values while
 * another arrives through OP_LOAD — neither answer works for the whole
 * join: resetting at the join would throw away the multi-valued arm's extra
 * values, and not resetting lets the stale arm hand out an older form's.
 * So the stale arms get a reset of their own, on a landing only they reach:
 *
 *     [arm code]  JMP ok_chain / JMP stale_chain
 *     ...
 *     [fall-through arm]
 *     JMP Ljoin              ; only when the fall-through is NOT stale
 *   Lstub:  OP_MV_RESET      ; stale arms (and a stale fall-through) land here
 *   Ljoin:                   ; ok arms land here
 *
 * Costs one byte plus at most one jump, and only when the arms actually
 * disagree; the uniform cases just patch both chains to the same spot. */
void cl_mv_close_join(CL_Compiler *c, int ok_chain, int stale_chain,
                      int any_stale, int merged)
{
    if (any_stale && merged == CL_MV_MANY) {
        int fall_stale = (c->mv_state == CL_MV_STALE);
        int over = CL_JUMP_CHAIN_END;
        if (!fall_stale) over = cl_emit_jump(c, OP_JMP);
        cl_patch_jump_chain(c, stale_chain);
        cl_emit(c, OP_MV_RESET);
        if (!fall_stale) cl_patch_jump(c, over);
        cl_patch_jump_chain(c, ok_chain);
        c->mv_state = CL_MV_MANY;
        return;
    }
    cl_patch_jump_chain(c, stale_chain);
    cl_patch_jump_chain(c, ok_chain);
    c->mv_state = merged;
}

/* cl_mv_close_join for a join fed by an explicit patch array rather than
 * pending-jump chains — the local-jump BLOCK exits and the loop epilogue,
 * whose arms store their result into a slot and jump to a shared reload.
 * STALE_MASK has bit i set when patches[i] left the MV state stale. */
void cl_mv_close_patch_join(CL_Compiler *c, const int *patches, int n,
                            uint32_t stale_mask, int merged)
{
    int i;
    int fall_stale = (c->mv_state == CL_MV_STALE);
    if ((stale_mask != 0 || fall_stale) && merged == CL_MV_MANY) {
        int over = 0;
        if (!fall_stale) over = cl_emit_jump(c, OP_JMP);
        for (i = 0; i < n; i++)
            if (stale_mask & ((uint32_t)1 << i)) cl_patch_jump(c, patches[i]);
        cl_emit(c, OP_MV_RESET);
        if (!fall_stale) cl_patch_jump(c, over);
        for (i = 0; i < n; i++)
            if (!(stale_mask & ((uint32_t)1 << i))) cl_patch_jump(c, patches[i]);
        c->mv_state = CL_MV_MANY;
        return;
    }
    for (i = 0; i < n; i++)
        cl_patch_jump(c, patches[i]);
    c->mv_state = merged;
}

/* Pending-jump chains: instead of collecting patch positions in a
 * caller-side array, thread the pending list through the 32-bit operand
 * placeholders themselves — each unpatched jump's operand temporarily
 * holds the position of the previous unpatched jump (CL_JUMP_CHAIN_END
 * terminates).  A nesting level then needs one int of state instead of
 * a patch array, which matters on small fixed Amiga shell stacks where
 * nested AND/OR/COND/CASE compiles recurse once per level. */

int cl_emit_jump_chain(CL_Compiler *c, uint8_t op, int chain)
{
    int pos = cl_emit_jump(c, op);
    c->code[pos]     = (uint8_t)(((uint32_t)chain >> 24) & 0xFF);
    c->code[pos + 1] = (uint8_t)(((uint32_t)chain >> 16) & 0xFF);
    c->code[pos + 2] = (uint8_t)(((uint32_t)chain >> 8) & 0xFF);
    c->code[pos + 3] = (uint8_t)((uint32_t)chain & 0xFF);
    return pos;
}

void cl_patch_jump_chain(CL_Compiler *c, int chain)
{
    while (chain != CL_JUMP_CHAIN_END) {
        int next = (int32_t)(((uint32_t)c->code[chain] << 24) |
                             ((uint32_t)c->code[chain + 1] << 16) |
                             ((uint32_t)c->code[chain + 2] << 8) |
                             (uint32_t)c->code[chain + 3]);
        cl_patch_jump(c, chain);
        chain = next;
    }
}

void cl_emit_loop_jump(CL_Compiler *c, uint8_t op, int target)
{
    int32_t offset;
    cl_emit(c, op);
    offset = target - (c->code_pos + 4);
    cl_emit_i32(c, offset);
}

/* --- Helper --- */

int alloc_temp_slot(CL_CompEnv *env)
{
    int slot = env->local_count;
    if (slot >= CL_MAX_LOCALS) {
        /* Writing env->locals[CL_MAX_LOCALS] would overrun the fixed
         * locals[]/boxed[] arrays in the heap-allocated CL_CompEnv,
         * silently corrupting adjacent compiler state.  Raise a clean
         * compile error instead. */
        cl_error(CL_ERR_OVERFLOW,
                 "Too many local variable slots in one function (max %d): "
                 "simplify or split the function", CL_MAX_LOCALS);
        return 0;
    }
    env->locals[slot] = CL_NIL;
    env->local_count++;
    if (env->local_count > env->max_locals)
        env->max_locals = env->local_count;
    return slot;
}

/* --- Compile-time constant folding (spec 1.3, gated on speed >= 1) ---
 *
 * A call to a pure CL arithmetic/logic builtin whose arguments are all
 * compile-time constants is evaluated here and emitted as a single
 * constant load instead of CONST/CONST/OP.  Fixnum-only: any operand or
 * intermediate result outside fixnum range declines (returns 0) and the
 * call compiles normally, so bignum/float/ratio semantics stay with the
 * runtime.  Purely a read of the form tree — never allocates, so it is
 * GC-neutral by construction. */

enum {
    FOLD_NONE = 0,
    FOLD_ADD, FOLD_SUB, FOLD_MUL,
    FOLD_1PLUS, FOLD_1MINUS,
    FOLD_ASH, FOLD_LOGAND, FOLD_LOGIOR, FOLD_LOGXOR,
    FOLD_NOT,
    FOLD_NUMEQ, FOLD_LT, FOLD_GT, FOLD_LE, FOLD_GE
};

/* Nested folds recurse in C (the trampoline exists precisely because
 * source can nest deeply) — cap the depth and decline beyond it. */
#define CL_FOLD_MAX_DEPTH 32

/* Classify FUNC as a foldable pure CL builtin.  Mirrors
 * inline_builtin_opcode's reasoning: CLHS 11.1.2.1.2 forbids conforming
 * programs from redefining COMMON-LISP functions, so the compile-time
 * value is authoritative.  NULL folds like NOT (identical semantics). */
static int foldable_builtin(CL_Obj func)
{
    CL_Symbol *s;
    CL_String *name;

    if (!CL_SYMBOL_P(func)) return FOLD_NONE;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(func);
    if (s->package != cl_package_cl) return FOLD_NONE;
    name = (CL_String *)CL_OBJ_TO_PTR(s->name);

    switch (name->length) {
    case 1:
        switch (name->data[0]) {
        case '+': return FOLD_ADD;
        case '-': return FOLD_SUB;
        case '*': return FOLD_MUL;
        case '=': return FOLD_NUMEQ;
        case '<': return FOLD_LT;
        case '>': return FOLD_GT;
        }
        break;
    case 2:
        if (name->data[0] == '<' && name->data[1] == '=') return FOLD_LE;
        if (name->data[0] == '>' && name->data[1] == '=') return FOLD_GE;
        if (name->data[0] == '1' && name->data[1] == '+') return FOLD_1PLUS;
        if (name->data[0] == '1' && name->data[1] == '-') return FOLD_1MINUS;
        break;
    case 3:
        if (memcmp(name->data, "NOT", 3) == 0) return FOLD_NOT;
        if (memcmp(name->data, "ASH", 3) == 0) return FOLD_ASH;
        break;
    case 4:
        if (memcmp(name->data, "NULL", 4) == 0) return FOLD_NOT;
        break;
    case 6:
        if (memcmp(name->data, "LOGAND", 6) == 0) return FOLD_LOGAND;
        if (memcmp(name->data, "LOGIOR", 6) == 0) return FOLD_LOGIOR;
        if (memcmp(name->data, "LOGXOR", 6) == 0) return FOLD_LOGXOR;
        break;
    }
    return FOLD_NONE;
}

static int fold_in_fixnum_range(int64_t v)
{
    return v >= (int64_t)CL_FIXNUM_MIN && v <= (int64_t)CL_FIXNUM_MAX;
}

/* Attempt compile-time evaluation of FORM.  On success stores the value
 * (a fixnum, T, NIL, or a quoted literal) in *out and returns 1; on any
 * doubt returns 0 and the caller emits the form normally.  Never
 * allocates and never signals — malformed arg lists (dotted, wrong
 * arity) decline so the regular compile path produces its diagnostics. */
static int try_fold_constant(CL_Compiler *c, CL_Obj form, CL_Obj *out, int depth)
{
    CL_Obj head, rest;
    int op;

    if (depth > CL_FOLD_MAX_DEPTH) return 0;

    /* Constant atoms.  The symbol NIL is CL_NIL itself; other symbols
     * are variables (or symbol macros) — not folded. */
    if (CL_FIXNUM_P(form)) { *out = form; return 1; }
    if (CL_NULL_P(form))   { *out = CL_NIL; return 1; }
    if (form == SYM_T)     { *out = SYM_T;  return 1; }
    if (!CL_CONS_P(form))  return 0;

    head = cl_car(form);
    rest = cl_cdr(form);

    /* (quote X): X is a literal constant of any type — usable directly
     * by NOT, and by the numeric ops when X is a fixnum. */
    if (head == SYM_QUOTE && CL_CONS_P(rest) && CL_NULL_P(cl_cdr(rest))) {
        *out = cl_car(rest);
        return 1;
    }

    op = foldable_builtin(head);
    if (op == FOLD_NONE) return 0;

    /* A local FLET/LABELS binding or function upvalue shadows the CL
     * builtin; (declare (notinline ...)) explicitly forbids folding. */
    if (cl_env_lookup_local_fun(c->env, head) >= 0) return 0;
    if (c->env && cl_env_resolve_fun_upvalue(c->env, head) >= 0) return 0;
    if (cl_compiler_notinline_p(c, head)) return 0;

    if (op == FOLD_NOT) {
        CL_Obj v;
        if (!CL_CONS_P(rest) || !CL_NULL_P(cl_cdr(rest))) return 0;
        if (!try_fold_constant(c, cl_car(rest), &v, depth + 1)) return 0;
        *out = CL_NULL_P(v) ? SYM_T : CL_NIL;
        return 1;
    }

    if (op == FOLD_1PLUS || op == FOLD_1MINUS) {
        CL_Obj v;
        int64_t r;
        if (!CL_CONS_P(rest) || !CL_NULL_P(cl_cdr(rest))) return 0;
        if (!try_fold_constant(c, cl_car(rest), &v, depth + 1)) return 0;
        if (!CL_FIXNUM_P(v)) return 0;
        r = (int64_t)CL_FIXNUM_VAL(v) + (op == FOLD_1PLUS ? 1 : -1);
        if (!fold_in_fixnum_range(r)) return 0;
        *out = CL_MAKE_FIXNUM((int32_t)r);
        return 1;
    }

    if (op == FOLD_ASH) {
        CL_Obj vn, vc;
        int64_t n, r;
        int32_t count;
        if (!CL_CONS_P(rest) || !CL_CONS_P(cl_cdr(rest)) ||
            !CL_NULL_P(cl_cdr(cl_cdr(rest)))) return 0;
        if (!try_fold_constant(c, cl_car(rest), &vn, depth + 1)) return 0;
        if (!try_fold_constant(c, cl_car(cl_cdr(rest)), &vc, depth + 1)) return 0;
        if (!CL_FIXNUM_P(vn) || !CL_FIXNUM_P(vc)) return 0;
        n = CL_FIXNUM_VAL(vn);
        count = CL_FIXNUM_VAL(vc);
        if (count >= 0) {
            /* Left shift via multiply — shifting a negative int64 is UB.
             * count > 30 can only stay in fixnum range for n == 0; keep
             * the guard simple and decline (runtime handles bignums). */
            if (count > 30) return 0;
            r = n * ((int64_t)1 << count);
        } else {
            /* ASH right shift is floor division by 2^|count| (CLHS),
             * exact for negative n too. */
            int32_t k = -count;
            int64_t d;
            if (k > 62) k = 62;  /* |n| < 2^31: result is already 0 / -1 */
            d = (int64_t)1 << k;
            r = (n >= 0) ? n / d : (n - (d - 1)) / d;
        }
        if (!fold_in_fixnum_range(r)) return 0;
        *out = CL_MAKE_FIXNUM((int32_t)r);
        return 1;
    }

    /* Variadic arithmetic / bitwise / comparison chain */
    {
        int64_t acc = 0, prev = 0;
        int have_acc = 0, cmp_true = 1, n = 0;
        int is_arith = (op == FOLD_ADD || op == FOLD_SUB || op == FOLD_MUL);
        int is_logic = (op == FOLD_LOGAND || op == FOLD_LOGIOR || op == FOLD_LOGXOR);

        switch (op) {
        case FOLD_ADD:    acc = 0;  have_acc = 1; break;
        case FOLD_MUL:    acc = 1;  have_acc = 1; break;
        case FOLD_LOGAND: acc = -1; have_acc = 1; break;
        case FOLD_LOGIOR: acc = 0;  have_acc = 1; break;
        case FOLD_LOGXOR: acc = 0;  have_acc = 1; break;
        default: break;  /* FOLD_SUB / comparisons seed from first arg */
        }

        while (CL_CONS_P(rest)) {
            CL_Obj v;
            int64_t x;
            if (!try_fold_constant(c, cl_car(rest), &v, depth + 1)) return 0;
            if (!CL_FIXNUM_P(v)) return 0;
            x = CL_FIXNUM_VAL(v);
            switch (op) {
            case FOLD_ADD:    acc += x; break;
            case FOLD_MUL:    acc *= x; break;
            case FOLD_LOGAND: acc &= x; break;
            case FOLD_LOGIOR: acc |= x; break;
            case FOLD_LOGXOR: acc ^= x; break;
            case FOLD_SUB:
                if (!have_acc) { acc = x; have_acc = 1; }
                else acc -= x;
                break;
            default:  /* comparison chain over adjacent pairs */
                if (n > 0) {
                    switch (op) {
                    case FOLD_NUMEQ: if (prev != x) cmp_true = 0; break;
                    case FOLD_LT:    if (prev >= x) cmp_true = 0; break;
                    case FOLD_GT:    if (prev <= x) cmp_true = 0; break;
                    case FOLD_LE:    if (prev >  x) cmp_true = 0; break;
                    case FOLD_GE:    if (prev <  x) cmp_true = 0; break;
                    }
                }
                prev = x;
                break;
            }
            /* Immediate range check keeps the int64 arithmetic exact:
             * operands are 31-bit, so one op on an in-range accumulator
             * cannot overflow int64, and an out-of-range accumulator
             * aborts folding before the next op. */
            if (is_arith && !fold_in_fixnum_range(acc)) return 0;
            n++;
            rest = cl_cdr(rest);
        }
        if (!CL_NULL_P(rest)) return 0;  /* dotted arg list */

        if (is_arith || is_logic) {
            if (op == FOLD_SUB) {
                if (n == 0) return 0;  /* (-) is a program error — don't hide it */
                if (n == 1) {
                    int64_t r = -acc;
                    if (!fold_in_fixnum_range(r)) return 0;
                    *out = CL_MAKE_FIXNUM((int32_t)r);
                    return 1;
                }
            }
            /* (+) => 0, (*) => 1, (logand) => -1, ... are all valid CL */
            *out = CL_MAKE_FIXNUM((int32_t)acc);
            return 1;
        }
        if (n == 0) return 0;  /* (=) etc. is a program error */
        *out = cmp_true ? SYM_T : CL_NIL;
        return 1;
    }
}

/* Emit a folded constant value with the cheapest available opcode. */
static void emit_folded_constant(CL_Compiler *c, CL_Obj val)
{
    if (CL_NULL_P(val))       cl_emit(c, OP_NIL);
    else if (val == SYM_T)    cl_emit(c, OP_T);
    else                      cl_emit_const(c, val);
}

/* --- Basic special forms --- */

static void compile_quote(CL_Compiler *c, CL_Obj form)
{
    CL_Obj val = cl_car(cl_cdr(form));
    if (CL_NULL_P(val))
        cl_emit(c, OP_NIL);
    else if (val == SYM_T)
        cl_emit(c, OP_T);
    else
        cl_emit_const(c, val);
}

/* Trampoline-aware IF using a two-stage continuation pattern.
 *
 * Stage 1 (this fn): compile TEST, emit JNIL → jnil_pos, push an
 *   IF_AFTER_THEN frame carrying jnil_pos + the ELSE form, and return
 *   the THEN form.  The driver compiles THEN inside its trampoline,
 *   keeping the C stack flat.
 * Stage 2 (IF_AFTER_THEN postlude): emit JMP → jmp_pos, patch jnil_pos.
 *   If there's an ELSE form to dispatch, push an IF_AFTER_ELSE frame
 *   (carrying jmp_pos) and return the ELSE form so the driver
 *   trampolines into it; otherwise emit OP_NIL inline and patch jmp_pos
 *   immediately.
 * Stage 3 (IF_AFTER_ELSE postlude): patch jmp_pos.
 *
 * Without this, the ELSE branch was already trampolined but the THEN
 * branch still recursed via compile_expr — every IF nesting level on
 * the THEN side ate one C frame.  Heavily-inlined macro output
 * (serapeum's dispatch-case) blew the stack at a few hundred levels. */
static CL_Obj compile_if(CL_Compiler *c, CL_Obj form)
{
    CL_Obj rest, test, then_form, else_form;
    int saved_tail = c->in_tail;
    int jnil_pos;
    CL_TailFrame *tf;

    CL_GC_PROTECT(form);

    rest = cl_cdr(form);
    test = cl_car(rest);

    /* Dead-branch elimination (spec 1.3, speed >= 1): a compile-time
     * constant test selects one branch at compile time — the dead branch
     * is never emitted and the JNIL/JMP pair disappears.  Covers the
     * expansions of WHEN/UNLESS/AND/OR with constant operands.  A false
     * test with no ELSE returns the literal NIL form, which the
     * trampoline compiles to OP_NIL (CLHS 5.2: (if test then) with a
     * false test yields NIL).  try_fold_constant never allocates, so the
     * sub-form reads below cannot go stale. */
    if (c->optimize_settings.speed >= 1) {
        CL_Obj test_val;
        if (try_fold_constant(c, test, &test_val, 0)) {
            CL_Obj live;
            if (!CL_NULL_P(test_val)) {
                live = cl_car(cl_cdr(rest));  /* THEN */
            } else {
                CL_Obj cdr2 = cl_cdr(cl_cdr(rest));
                live = CL_NULL_P(cdr2) ? CL_NIL : cl_car(cdr2);  /* ELSE */
            }
            CL_GC_UNPROTECT(1);  /* form */
            return live;  /* c->in_tail untouched — live branch keeps tail position */
        }
    }

    c->in_tail = 0;
    compile_expr(c, test);
    jnil_pos = cl_emit_jump(c, OP_JNIL);

    /* GC SAFETY: re-derive then/else from `form` AFTER compile_expr(test).
     * compile_expr(test) allocates and can trigger a compacting (moving) GC.
     * `form` is GC-protected (above) so its cons cells stay live and are
     * forwarded, but a `then_form`/`else_form` read *before* that compaction
     * would be a stale C local holding a pre-move offset.  Returned as the
     * trampoline continuation, such a stale offset surfaces as a bogus
     * "self-evaluating" constant — an interior pointer into an unrelated
     * object — corrupting the constant pool under GC stress (observed as
     * "malformed call form (dotted pair)" / "CDR not a list" compiling a
     * WHEN/UNLESS/IF body inside a LET/DOTIMES).  Reading them here, from the
     * forwarded `form`, yields live offsets.
     *
     * `(if test then)` and `(if test then nil)` are equivalent (CLHS 5.2):
     * treat both as "no else" — we emit OP_NIL inline in the AFTER_THEN
     * postlude rather than dispatching the literal NIL. */
    rest = cl_cdr(form);  /* re-derive: the earlier `rest` local is now stale */
    then_form = cl_car(cl_cdr(rest));
    {
        CL_Obj cdr2 = cl_cdr(cl_cdr(rest));
        else_form = CL_NULL_P(cdr2) ? CL_NIL : cl_car(cdr2);
    }

    CL_GC_UNPROTECT(1);

    c->in_tail = saved_tail;
    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_IF_AFTER_THEN;
    tf->block_push_pos = jnil_pos;
    tf->saved_tail = saved_tail;
    tf->cont_form = else_form;  /* GC-traced via tail_stack walk */
    return then_form;
}

/* --- Tail-trampoline frame management --- */

/* Push a new tail frame, growing the stack lazily.  Returns a pointer to
 * the frame (caller fills in kind/state).  Aborts on alloc failure — the
 * compiler can't continue in that state. */
CL_TailFrame *cl_tail_push(CL_Compiler *c)
{
    CL_TailFrame *tf;
    if (c->tail_count == c->tail_capacity) {
        int new_cap = c->tail_capacity ? c->tail_capacity * 2 : 64;
        CL_TailFrame *nf = (CL_TailFrame *)platform_alloc((size_t)new_cap * sizeof(CL_TailFrame));
        if (!nf) cl_error(CL_ERR_STORAGE, "compiler: out of memory growing tail stack");
        if (c->tail_stack) {
            memcpy(nf, c->tail_stack, (size_t)c->tail_count * sizeof(CL_TailFrame));
            platform_free(c->tail_stack);
        }
        c->tail_stack = nf;
        c->tail_capacity = new_cap;
    }
    tf = &c->tail_stack[c->tail_count++];
    tf->cont_form = CL_NIL;  /* default: no continuation */
    tf->mv_state = CL_MV_MANY; /* default: never provokes a reset */
    /* Snapshot the effective optimize settings unconditionally (4 bytes) so
     * no declaration-accepting prelude can forget it; only postludes of
     * bodies that process declarations restore from it. */
    tf->saved_optimize = c->optimize_settings;
    return tf;
}

/* Forward declaration: drain helper used by compile_progn below to
 * handle any PROGN_ITER frames left over after compile_progn_tail. */
static CL_Obj emit_tail_postlude(CL_Compiler *c, CL_TailFrame *tf);

/* Compile every form in `forms` for value, with OP_POP between non-last
 * forms.  Used by trampoline-aware callers that want to splice the
 * body's first form into their outer trampoline.
 *
 * Returns CL_NIL for an empty body (caller emits OP_NIL itself).
 * Returns the only body form when forms has length 1 (no frame pushed).
 * Otherwise pushes a CL_TAIL_PROGN_ITER frame carrying the remainder
 * of the form list and returns the first form — the postlude emits
 * OP_POP and continues with the next form, keeping body iteration off
 * the C stack. */
CL_Obj compile_progn_tail(CL_Compiler *c, CL_Obj forms)
{
    CL_TailFrame *tf;
    int saved_tail;

    if (CL_NULL_P(forms)) return CL_NIL;
    if (CL_NULL_P(cl_cdr(forms)))
        return cl_car(forms);  /* single tail form — caller dispatches */

    saved_tail = c->in_tail;
    c->in_tail = 0;  /* non-tail position for first body form */
    tf = cl_tail_push(c);
    tf->kind = CL_TAIL_PROGN_ITER;
    tf->saved_tail = saved_tail;
    tf->cont_form = cl_cdr(forms);  /* remaining forms after the first */
    return cl_car(forms);
}

/* Backward-compatible wrapper for callers that don't want to manage the
 * tail form themselves.  Compiles the entire sequence inline.
 *
 * compile_progn_tail may push PROGN_ITER frames whose postludes need to
 * drain.  compile_expr's drain stops at its own tail_base (which
 * captures c->tail_count at entry, including any frames we pushed
 * before calling it).  So we run a manual drain here to clean those
 * PROGN_ITER frames up. */
void compile_progn(CL_Compiler *c, CL_Obj forms)
{
    int tail_base = c->tail_count;
    CL_Obj tail = compile_progn_tail(c, forms);
    /* See compile_body: a NIL tail with a pushed PROGN_ITER frame means a
     * leading literal NIL form with more forms after it — compile the NIL
     * and drain, don't short-circuit (which would drop the trailing forms). */
    if (CL_NULL_P(tail) && c->tail_count == tail_base) {
        cl_emit(c, OP_NIL);
        return;
    }
    compile_expr(c, tail);
    while (c->tail_count > tail_base) {
        CL_TailFrame frame = c->tail_stack[--c->tail_count];
        CL_Obj cont = emit_tail_postlude(c, &frame);
        if (!CL_NULL_P(cont))
            compile_expr(c, cont);
    }
}

/* --- Lambda --- */

static void parse_lambda_list(CL_Obj params, CL_ParsedLambdaList *ll)
{
    CL_Obj p = params;
    int state = 0; /* 0=required, 1=optional, 2=rest, 3=key, 4=aux */

    memset(ll, 0, sizeof(*ll));
    ll->rest_name = CL_NIL;

    /* GC SAFETY: cl_intern_keyword in the &key branch can intern a new
     * keyword (allocates, can compact) — protect the walk cursor so the
     * advancing cl_cdr doesn't follow a stale offset. */
    CL_GC_PROTECT(p);

    while (!CL_NULL_P(p)) {
        CL_Obj item = cl_car(p);
        p = cl_cdr(p);

        if (item == SYM_AMP_OPTIONAL) { state = 1; continue; }
        if (item == SYM_AMP_REST || item == SYM_AMP_BODY) { state = 2; continue; }
        if (item == SYM_AMP_KEY) { state = 3; continue; }
        if (item == SYM_AMP_ALLOW_OTHER_KEYS) { ll->allow_other_keys = 1; continue; }
        if (item == SYM_AMP_AUX) { state = 4; continue; }

        switch (state) {
        case 0:
            if (ll->n_required >= CL_MAX_LOCALS)
                cl_error(CL_ERR_GENERAL, "Too many required parameters (max %d)", CL_MAX_LOCALS);
            ll->required[ll->n_required++] = item;
            break;
        case 1:
            if (ll->n_optional >= CL_MAX_LOCALS)
                cl_error(CL_ERR_GENERAL, "Too many optional parameters (max %d)", CL_MAX_LOCALS);
            if (CL_CONS_P(item)) {
                ll->opt_names[ll->n_optional] = cl_car(item);
                ll->opt_defaults[ll->n_optional] = cl_car(cl_cdr(item));
                /* Third element is supplied-p variable: (name default svar) */
                {
                    CL_Obj cddr = cl_cdr(cl_cdr(item));
                    ll->opt_suppliedp[ll->n_optional] = CL_NULL_P(cddr) ? CL_NIL : cl_car(cddr);
                }
            } else {
                ll->opt_names[ll->n_optional] = item;
                ll->opt_defaults[ll->n_optional] = CL_NIL;
                ll->opt_suppliedp[ll->n_optional] = CL_NIL;
            }
            ll->n_optional++;
            break;
        case 2:
            ll->rest_name = item;
            ll->has_rest = 1;
            state = 3;
            break;
        case 3: {
            int ki;
            if (ll->n_keys >= CL_MAX_LOCALS)
                cl_error(CL_ERR_GENERAL, "Too many keyword parameters (max %d)", CL_MAX_LOCALS);
            ki = ll->n_keys;
            if (CL_CONS_P(item)) {
                CL_Obj name_part = cl_car(item);
                if (CL_CONS_P(name_part)) {
                    /* ((:keyword var) default svar) — explicit keyword name */
                    ll->key_keywords[ki] = cl_car(name_part);
                    ll->key_names[ki] = cl_car(cl_cdr(name_part));
                } else {
                    /* (name default svar) — keyword inferred from name */
                    ll->key_names[ki] = name_part;
                    ll->key_keywords[ki] = CL_NIL; /* set below if not set */
                }
                ll->key_defaults[ki] = cl_car(cl_cdr(item));
                /* Third element is supplied-p variable: (...  default svar) */
                {
                    CL_Obj cddr = cl_cdr(cl_cdr(item));
                    ll->key_suppliedp[ki] = CL_NULL_P(cddr) ? CL_NIL : cl_car(cddr);
                }
            } else {
                ll->key_names[ki] = item;
                ll->key_defaults[ki] = CL_NIL;
                ll->key_suppliedp[ki] = CL_NIL;
                ll->key_keywords[ki] = CL_NIL;
            }
            /* GC SAFETY: publish the slot to the compiler GC walkers BEFORE the
             * allocating cl_intern_keyword below — the mark/update walkers only
             * cover key_* indices < n_keys, so a compaction during the intern
             * would leave the name/default/suppliedp offsets just stored above
             * stale (never forwarded).  Bump n_keys first. */
            ll->n_keys++;
            /* Infer keyword from variable name if not explicitly set */
            if (ll->key_keywords[ki] == CL_NIL) {
                const char *name_str = cl_symbol_name(ll->key_names[ki]);
                ll->key_keywords[ki] = cl_intern_keyword(
                    name_str, (uint32_t)strlen(name_str));
            }
            break;
        }
        case 4:
            if (ll->n_aux >= CL_MAX_LOCALS)
                cl_error(CL_ERR_GENERAL, "Too many &aux bindings (max %d)", CL_MAX_LOCALS);
            if (CL_CONS_P(item)) {
                ll->aux_names[ll->n_aux] = cl_car(item);
                ll->aux_inits[ll->n_aux] = cl_car(cl_cdr(item));
            } else {
                ll->aux_names[ll->n_aux] = item;
                ll->aux_inits[ll->n_aux] = CL_NIL;
            }
            ll->n_aux++;
            break;
        }
    }
    CL_GC_UNPROTECT(1); /* p */
}

void compile_lambda(CL_Compiler *c, CL_Obj form)
{
    CL_Obj params = cl_car(cl_cdr(form));
    CL_Obj body = cl_cdr(cl_cdr(form));
    CL_Obj my_name;
    CL_Compiler *inner;
    CL_CompEnv *env;
    CL_Bytecode *bc;
    int const_idx;
    int i;
    /* ll, key_slot_indices, key_suppliedp_indices, param_vars, param_slots,
     * lambda_needs_boxing are all in CL_Compiler struct (heap-allocated)
     * to avoid stack overflow on AmigaOS 65KB stack */


    /* Heap-allocate inner compiler (too large for AmigaOS stack).
     * Routed through cl_compiler_pool_acquire so the block and its buffers
     * are recycled across calls and never returned to AmigaOS — see the
     * pool comment above cl_compiler_pool_head.  Comes back zeroed. */
    inner = cl_compiler_pool_acquire();
    if (!inner) {
        platform_write_string("[compile_lambda] pool_acquire FAILED\n");
        return;
    }
    inner->mv_state = CL_MV_ONE;  /* nothing emitted yet */

    /* Claim the name the caller handed over (compile_defun / compile_named_lambda
     * set pending_lambda_name just before compiling this form) BEFORE the body is
     * compiled, and clear the slot right away.
     *
     * The hand-off is a single per-thread slot, so reading it late — after
     * compile_body — hands the name to whichever lambda finishes first, and the
     * nested ones always finish before their parent.  `(defun higher () ...
     * (handler-bind ((foo (lambda (c) ...))) ...))` then names the *handler*
     * HIGHER and leaves HIGHER itself NIL, which is what the backtrace renders
     * as "<anonymous>".  Claiming at entry gives each lambda exactly the name
     * its own caller set, and leaves nested lambdas correctly anonymous. */
    my_name = pending_lambda_name;
    pending_lambda_name = CL_NIL;

    /* GC-protect the form and its sub-structures.  `params` and `body` are
     * arena-relative offsets into `form`; everything below (parse_lambda_list,
     * compile_expr for &optional/&key/&aux defaults, determine_boxed_vars,
     * scan_local_specials, cl_add_constant, the closure body compile) can
     * trigger a compacting GC.  Without protection those compactions relocate
     * the form and leave these C locals pointing at stale/freed memory, so the
     * closure body gets compiled from garbage — a free variable's symbol read
     * from a stale offset no longer matches the enclosing env entry and is
     * mis-emitted as a global load ("Unbound variable: BOX<n>") or the wrong
     * block tag ("RETURN-FROM: no block named NIL").  The caller protecting
     * `form` is NOT sufficient: compile_lambda holds its own by-value copy of
     * the offset, which the caller's root does not rewrite. */
    CL_GC_PROTECT(form);
    CL_GC_PROTECT(params);
    CL_GC_PROTECT(body);
    /* my_name is now a plain C local held across compile_body's allocations.
     * The GC's thread walker roots only the pending_lambda_name slot (mem.c),
     * which we just cleared, so without this the symbol goes stale on the first
     * compaction and bc->name ends up a dangling offset. */
    CL_GC_PROTECT(my_name);

    /* Register inner compiler for GC root marking.  The anchor records this
     * frame as the owner: an NLX landing in a VM run nested below this frame
     * (e.g. a macroexpansion doing a non-local exit) must not free this
     * compiler, while a longjmp that abandons this frame must — see
     * cl_compiler_unwind_to. */
    inner->parent = cl_active_compiler;
    inner->anchor = CL_CAPTURE_SP();
    cl_compiler_seed_optimize_settings(inner);
    cl_active_compiler = inner;

    parse_lambda_list(params, &inner->ll);

    env = cl_env_create(c->env);
    inner->env = env;
    inner->in_tail = 1;

    /* Propagate visible block names for cross-closure return-from */
    inner->outer_block_count = 0;
    for (i = 0; i < c->block_count; i++) {
        if (inner->outer_block_count < CL_MAX_BLOCKS)
            inner->outer_blocks[inner->outer_block_count++] = c->blocks[i].tag;
    }
    for (i = 0; i < c->outer_block_count; i++) {
        if (inner->outer_block_count < CL_MAX_BLOCKS)
            inner->outer_blocks[inner->outer_block_count++] = c->outer_blocks[i];
    }

    /* Propagate visible tagbody tags for cross-closure go */
    inner->outer_tag_count = 0;
    for (i = 0; i < c->tagbody_count; i++) {
        CL_TagbodyInfo *tb = &c->tagbodies[i];
        int j;
        if (!tb->uses_nlx) continue;  /* only NLX tagbodies need propagation */
        for (j = 0; j < tb->n_tags; j++) {
            if (inner->outer_tag_count < (int)(sizeof(inner->outer_tags)/sizeof(inner->outer_tags[0]))) {
                inner->outer_tags[inner->outer_tag_count].tag = tb->tags[j].tag;
                inner->outer_tags[inner->outer_tag_count].tagbody_id = tb->id;
                inner->outer_tags[inner->outer_tag_count].tag_index = j;
                inner->outer_tag_count++;
            }
        }
    }
    for (i = 0; i < c->outer_tag_count; i++) {
        if (inner->outer_tag_count < (int)(sizeof(inner->outer_tags)/sizeof(inner->outer_tags[0]))) {
            inner->outer_tags[inner->outer_tag_count] = c->outer_tags[i];
            inner->outer_tag_count++;
        }
    }


    for (i = 0; i < inner->ll.n_required; i++)
        cl_env_add_local(env, inner->ll.required[i]);

    /* &optional, &rest and the optionals' supplied-p variables.  Their
     * slots are fixed by the frame layout every frame setup (vm.c OP_CALL,
     * the JIT prologues) uses: the optionals right after the required
     * parameters, the &rest list right after them, then the supplied-p
     * variables.  Each slot is reserved under a non-symbol sentinel and its
     * name revealed only once the variable is bound, so an init form sees
     * the parameters to its left -- supplied-p variables included -- but not
     * its own or later ones (CLHS 3.4.1.2).  This matters for
     * &optional (*special* *special*), where the default must read the
     * dynamic value, not the uninitialized slot.  Names are revealed from
     * inner->ll (forwarded by the compiler's GC walkers), never from a C
     * copy: compile_expr can compact. */
    {
        int rest_slot = -1;
        int sp_slot = -1;       /* the next supplied-p slot (they are contiguous) */
        for (i = 0; i < inner->ll.n_optional; i++)
            cl_env_add_local(env, CL_MAKE_FIXNUM(0));
        if (inner->ll.has_rest)
            rest_slot = cl_env_add_local(env, CL_MAKE_FIXNUM(0));
        for (i = 0; i < inner->ll.n_optional; i++) {
            if (!CL_NULL_P(inner->ll.opt_suppliedp[i])) {
                int s = cl_env_add_local(env, CL_MAKE_FIXNUM(0));
                if (sp_slot < 0) sp_slot = s;
            }
        }
        for (i = 0; i < inner->ll.n_optional; i++) {
            int opt_slot = inner->ll.n_required + i;
            if (!CL_NULL_P(inner->ll.opt_defaults[i])) {
                int skip_pos;
                cl_emit(inner, OP_ARGC);
                cl_emit_const(inner, CL_MAKE_FIXNUM(inner->ll.n_required + i + 1));
                cl_emit(inner, OP_GE);
                skip_pos = cl_emit_jump(inner, OP_JTRUE);
                {
                    int saved = inner->in_tail;
                    inner->in_tail = 0;
                    compile_expr(inner, inner->ll.opt_defaults[i]);
                    inner->in_tail = saved;
                }
                cl_emit(inner, OP_STORE);
                cl_emit(inner, (uint8_t)opt_slot);
                cl_emit(inner, OP_POP);
                cl_patch_jump(inner, skip_pos);
            }
            if (opt_slot >= 0 && opt_slot < CL_MAX_LOCALS)
                env->locals[opt_slot] = inner->ll.opt_names[i];
            if (!CL_NULL_P(inner->ll.opt_suppliedp[i]) && sp_slot >= 0) {
                int skip_pos;
                /* NIL from the frame setup; T if the argument was passed. */
                cl_emit(inner, OP_ARGC);
                cl_emit_const(inner, CL_MAKE_FIXNUM(inner->ll.n_required + i + 1));
                cl_emit(inner, OP_GE);
                skip_pos = cl_emit_jump(inner, OP_JNIL);
                cl_emit_const(inner, CL_T);
                cl_emit(inner, OP_STORE);
                cl_emit(inner, (uint8_t)sp_slot);
                cl_emit(inner, OP_POP);
                cl_patch_jump(inner, skip_pos);
                env->locals[sp_slot] = inner->ll.opt_suppliedp[i];
                sp_slot++;
            }
        }
        if (rest_slot >= 0)
            env->locals[rest_slot] = inner->ll.rest_name;
    }

    for (i = 0; i < inner->ll.n_keys; i++)
        inner->key_slot_indices[i] = cl_env_add_local(env, inner->ll.key_names[i]);
    /* Always allocate a tracking slot per key for VM-level supplied-p.
     * If user also declared a supplied-p var, reuse the same slot. */
    for (i = 0; i < inner->ll.n_keys; i++) {
        if (!CL_NULL_P(inner->ll.key_suppliedp[i]))
            inner->key_suppliedp_indices[i] = cl_env_add_local(env, inner->ll.key_suppliedp[i]);
        else
            inner->key_suppliedp_indices[i] = alloc_temp_slot(env);
    }
    /* &aux locals are added one at a time during prologue emission
     * (see below) so each init form is compiled in an environment where
     * the variable being bound is NOT yet visible — matching LET* semantics
     * per CLHS 3.4.1.4.  This lets `(&aux (record (list record)))` see the
     * outer parameter `record` in the init form rather than the freshly-
     * shadowed (NIL) slot. */

    /* Emit prologue for key defaults: check the VM-set tracking slot,
     * not the key value itself (which could legitimately be NIL).
     * Per CL spec (CLHS 3.4.1), init-forms can reference earlier params
     * but NOT the current param or later ones.  We temporarily hide
     * params i..N-1 from the local env so that if a key param shadows a
     * special variable, the default form sees the dynamic value. */
    for (i = 0; i < inner->ll.n_keys; i++) {
        if (!CL_NULL_P(inner->ll.key_defaults[i])) {
            int skip_pos, j;
            /* Hide current and later key params from local env */
            for (j = i; j < inner->ll.n_keys; j++) {
                int s = inner->key_slot_indices[j];
                env->locals[s] = CL_MAKE_FIXNUM(0); /* non-symbol sentinel */
            }
            cl_emit(inner, OP_LOAD);
            cl_emit(inner, (uint8_t)inner->key_suppliedp_indices[i]);
            skip_pos = cl_emit_jump(inner, OP_JTRUE);
            {
                int saved = inner->in_tail;
                inner->in_tail = 0;
                compile_expr(inner, inner->ll.key_defaults[i]);
                inner->in_tail = saved;
            }
            cl_emit(inner, OP_STORE);
            cl_emit(inner, (uint8_t)inner->key_slot_indices[i]);
            cl_emit(inner, OP_POP);
            cl_patch_jump(inner, skip_pos);
            /* Restore hidden key params.
             * GC SAFETY: restore from inner->ll.key_names (forwarded by the
             * compiler GC walkers), NOT from a raw C save array — the
             * compile_expr above can compact, and a saved pre-compaction
             * symbol offset written back here leaves later key params
             * "Unbound variable" / corrupted.  The slots held exactly
             * ll.key_names[j] (set via cl_env_add_local above). */
            for (j = i; j < inner->ll.n_keys; j++) {
                env->locals[inner->key_slot_indices[j]] = inner->ll.key_names[j];
            }
        }
    }

    /* Emit prologue for &aux bindings.  Add the local AFTER compiling
     * its init form so the init sees the outer scope (parameter or earlier
     * binding) rather than a freshly-shadowed NIL slot of the same name —
     * LET* semantics per CLHS 3.4.1.4. */
    for (i = 0; i < inner->ll.n_aux; i++) {
        int aux_slot;
        {
            int saved = inner->in_tail;
            inner->in_tail = 0;
            compile_expr(inner, inner->ll.aux_inits[i]);
            inner->in_tail = saved;
        }
        aux_slot = cl_env_add_local(env, inner->ll.aux_names[i]);
        cl_emit(inner, OP_STORE);
        cl_emit(inner, (uint8_t)aux_slot);
        cl_emit(inner, OP_POP);
    }

    /* Box params that are both mutated and captured across closure boundary */
    {
        int n_params = 0;

        for (i = 0; i < inner->ll.n_required; i++) {
            inner->param_slots[n_params] = cl_env_lookup(env, inner->ll.required[i]);
            inner->param_vars[n_params] = inner->ll.required[i];
            n_params++;
        }
        for (i = 0; i < inner->ll.n_optional; i++) {
            inner->param_slots[n_params] = cl_env_lookup(env, inner->ll.opt_names[i]);
            inner->param_vars[n_params] = inner->ll.opt_names[i];
            n_params++;
        }
        if (inner->ll.has_rest) {
            inner->param_slots[n_params] = cl_env_lookup(env, inner->ll.rest_name);
            inner->param_vars[n_params] = inner->ll.rest_name;
            n_params++;
        }
        for (i = 0; i < inner->ll.n_keys; i++) {
            inner->param_slots[n_params] = inner->key_slot_indices[i];
            inner->param_vars[n_params] = inner->ll.key_names[i];
            n_params++;
        }
        for (i = 0; i < inner->ll.n_aux; i++) {
            inner->param_slots[n_params] = cl_env_lookup(env, inner->ll.aux_names[i]);
            inner->param_vars[n_params] = inner->ll.aux_names[i];
            n_params++;
        }

        if (n_params > 0) {
            determine_boxed_vars(body, inner->param_vars, n_params, inner->lambda_needs_boxing);
            for (i = 0; i < n_params; i++) {
                if (inner->lambda_needs_boxing[i] && inner->param_slots[i] >= 0) {
                    cl_emit(inner, OP_LOAD);
                    cl_emit(inner, (uint8_t)inner->param_slots[i]);
                    cl_emit(inner, OP_MAKE_CELL);
                    cl_emit(inner, OP_STORE);
                    cl_emit(inner, (uint8_t)inner->param_slots[i]);
                    cl_emit(inner, OP_POP);
                    env->boxed[inner->param_slots[i]] = 1;
                }
            }
        }
    }

    /* Emit OP_DYNBIND for parameters that are special.
     * Check both globally-proclaimed specials and locally-declared specials
     * from (declare (special ...)) in the function body.
     * The VM stores parameter values in local slots, but called functions
     * and closures need to see them via the dynamic binding stack. */
    {
        int special_param_count = 0;
        int pi;
        CL_Obj local_specials = scan_local_specials(body);
        for (pi = 0; pi < inner->ll.n_required; pi++) {
            if (cl_symbol_specialp(inner->ll.required[pi]) ||
                is_locally_special(inner->ll.required[pi], local_specials)) {
                int slot = cl_env_lookup(env, inner->ll.required[pi]);
                int idx = cl_add_constant(inner, inner->ll.required[pi]);
                cl_emit(inner, OP_LOAD);
                cl_emit(inner, (uint8_t)slot);
                cl_emit(inner, OP_DYNBIND);
                cl_emit_u16(inner, (uint16_t)idx);
                special_param_count++;
            }
        }
        for (pi = 0; pi < inner->ll.n_optional; pi++) {
            if (cl_symbol_specialp(inner->ll.opt_names[pi]) ||
                is_locally_special(inner->ll.opt_names[pi], local_specials)) {
                int slot = cl_env_lookup(env, inner->ll.opt_names[pi]);
                int idx = cl_add_constant(inner, inner->ll.opt_names[pi]);
                cl_emit(inner, OP_LOAD);
                cl_emit(inner, (uint8_t)slot);
                cl_emit(inner, OP_DYNBIND);
                cl_emit_u16(inner, (uint16_t)idx);
                special_param_count++;
            }
        }
        if (inner->ll.has_rest && (cl_symbol_specialp(inner->ll.rest_name) ||
            is_locally_special(inner->ll.rest_name, local_specials))) {
            int slot = cl_env_lookup(env, inner->ll.rest_name);
            int idx = cl_add_constant(inner, inner->ll.rest_name);
            cl_emit(inner, OP_LOAD);
            cl_emit(inner, (uint8_t)slot);
            cl_emit(inner, OP_DYNBIND);
            cl_emit_u16(inner, (uint16_t)idx);
            special_param_count++;
        }
        for (pi = 0; pi < inner->ll.n_keys; pi++) {
            if (cl_symbol_specialp(inner->ll.key_names[pi]) ||
                is_locally_special(inner->ll.key_names[pi], local_specials)) {
                int slot = inner->key_slot_indices[pi];
                int idx = cl_add_constant(inner, inner->ll.key_names[pi]);
                cl_emit(inner, OP_LOAD);
                cl_emit(inner, (uint8_t)slot);
                cl_emit(inner, OP_DYNBIND);
                cl_emit_u16(inner, (uint16_t)idx);
                special_param_count++;
            }
        }
        for (pi = 0; pi < inner->ll.n_aux; pi++) {
            if (cl_symbol_specialp(inner->ll.aux_names[pi]) ||
                is_locally_special(inner->ll.aux_names[pi], local_specials)) {
                int slot = cl_env_lookup(env, inner->ll.aux_names[pi]);
                int idx = cl_add_constant(inner, inner->ll.aux_names[pi]);
                cl_emit(inner, OP_LOAD);
                cl_emit(inner, (uint8_t)slot);
                cl_emit(inner, OP_DYNBIND);
                cl_emit_u16(inner, (uint16_t)idx);
                special_param_count++;
            }
        }
        if (special_param_count > 0)
            inner->in_tail = 0;
        inner->special_depth += special_param_count;

        compile_body(inner, body);

        inner->special_depth -= special_param_count;
        if (special_param_count > 0) {
            cl_emit(inner, OP_DYNUNBIND);
            cl_emit(inner, (uint8_t)special_param_count);
        }
    }
    /* OP_RET hands the MV state to the caller (CLHS 3.1.7): a body whose
     * tail form was a plain variable reference must return ONE value, not
     * whatever the last (values …) inside it happened to leave behind. */
    cl_mv_normalize(inner);
    cl_emit(inner, OP_RET);

    /* Peephole post-pass (no-op unless the effective speed reached >= 2;
     * rewrites inner->code/code_pos/line_entries in place, shrink-only).
     * Runs before the bytecode object is built so FASL serialization and
     * the m68k JIT both consume the optimized stream. */
    cl_peephole_optimize(inner);

    /* Build bytecode object */
    bc = (CL_Bytecode *)cl_alloc(TYPE_BYTECODE, sizeof(CL_Bytecode));
    if (!bc) {
        cl_active_compiler = inner->parent;
        cl_env_destroy(env);
        cl_compiler_pool_release(inner);
        CL_GC_UNPROTECT(4);  /* my_name, form, params, body */
        return;
    }

    bc->code = (uint8_t *)platform_alloc(inner->code_pos);
    if (bc->code) memcpy(bc->code, inner->code, inner->code_pos);
    bc->code_len = inner->code_pos;

    if (inner->const_count > 0) {
        bc->constants = (CL_Obj *)platform_alloc(
            inner->const_count * sizeof(CL_Obj));
        if (bc->constants) {
            for (i = 0; i < inner->const_count; i++)
                bc->constants[i] = inner->constants[i];
        }
        bc->n_constants = inner->const_count;
    } else {
        bc->constants = NULL;
        bc->n_constants = 0;
    }

    bc->arity = inner->ll.has_rest ? (inner->ll.n_required | 0x8000) : inner->ll.n_required;
    bc->n_locals = env->max_locals;
    bc->n_upvalues = env->upvalue_count;
    bc->name = my_name;   /* claimed at entry, before the body was compiled */
    /* Retain the original lambda-list for arglist introspection.  `params`
     * is a sub-form of `form`, which the caller keeps live throughout
     * compilation (body is read from it after many allocations below). */
    bc->source_lambda_list = params;
    bc->n_optional = (uint8_t)inner->ll.n_optional;
    bc->flags = ((inner->ll.n_keys > 0 || inner->ll.allow_other_keys) ? 1 : 0)
              | (inner->ll.allow_other_keys ? 2 : 0);
    bc->n_keys = (uint8_t)inner->ll.n_keys;

    if (inner->ll.n_keys > 0) {
        bc->key_syms = (CL_Obj *)platform_alloc(inner->ll.n_keys * sizeof(CL_Obj));
        bc->key_slots = (uint8_t *)platform_alloc(inner->ll.n_keys);
        bc->key_suppliedp_slots = (uint8_t *)platform_alloc(inner->ll.n_keys);
        for (i = 0; i < inner->ll.n_keys; i++) {
            bc->key_syms[i] = inner->ll.key_keywords[i];
            bc->key_slots[i] = (uint8_t)inner->key_slot_indices[i];
            bc->key_suppliedp_slots[i] = (uint8_t)inner->key_suppliedp_indices[i];
        }
    } else {
        bc->key_syms = NULL;
        bc->key_slots = NULL;
        bc->key_suppliedp_slots = NULL;
    }

    /* Transfer source line map */
    if (inner->line_entry_count > 0) {
        bc->line_map = (CL_LineEntry *)platform_alloc(
            inner->line_entry_count * sizeof(CL_LineEntry));
        if (bc->line_map) {
            for (i = 0; i < inner->line_entry_count; i++)
                bc->line_map[i] = inner->line_entries[i];
        }
        bc->line_map_count = (uint16_t)inner->line_entry_count;
    } else {
        bc->line_map = NULL;
        bc->line_map_count = 0;
    }
    bc->source_line = (uint16_t)c->current_line;
    bc->source_file = cl_current_source_file;

    bc->native_code = NULL;
    bc->native_len  = 0;
    bc->native_relocs = NULL;
    bc->native_reloc_count = 0;
    /* (optimize (speed 3)) anywhere in the lambda compiles it at definition
     * (jit.h).  Recorded on every target: the hint travels in the FASL, and
     * the host compiles the FASLs the m68k binaries load. */
    bc->jit_hot = (inner->peep_speed_max >= 3) ? CL_BC_JIT_SPEED : 0;
    cl_jit_note_definition(bc);  /* no-op on host / when JIT_M68K undefined */

    const_idx = cl_add_constant(c, CL_PTR_TO_OBJ(bc));
    cl_emit(c, OP_CLOSURE);
    cl_emit_u16(c, (uint16_t)const_idx);

    for (i = 0; i < env->upvalue_count; i++) {
        cl_emit(c, (uint8_t)env->upvalues[i].is_local);
        cl_emit(c, (uint8_t)env->upvalues[i].index);
    }

    /* Unregister inner compiler from GC root chain */
    cl_active_compiler = inner->parent;

    cl_env_destroy(env);
    cl_compiler_pool_release(inner);

    CL_GC_UNPROTECT(4);  /* my_name, form, params, body */
}

/* --- Boxing analysis (pre-scan for mutable closure bindings) --- */

/* Check if sym is one of the tracked vars; returns index or -1 */
static int find_var_index(CL_Obj sym, CL_Obj *vars, int n_vars)
{
    int i;
    /* Search from end so shadowed variables (let*) resolve to innermost binding */
    for (i = n_vars - 1; i >= 0; i--) {
        if (vars[i] == sym) return i;
    }
    return -1;
}

/* Walk a quasiquote template, only scanning UNQUOTE/UNQUOTE-SPLICING subforms.
 * Template structure (literal symbols, lists used as structure) is skipped. */
static void scan_qq_for_boxing(CL_Obj tmpl, CL_Obj *vars, int n_vars,
                               uint8_t *mutated, uint8_t *captured,
                               int closure_depth)
{
    /* Vector template — scan each element as a sub-template
     * (unquotes inside #(... ,foo ...) need to box `foo` if captured). */
    if (CL_VECTOR_P(tmpl)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(tmpl);
        uint32_t i, n = cl_vector_active_length(v);
        /* GC SAFETY: the recursive scan macroexpands unquoted subforms via
         * cl_vm_apply (allocates/compacts).  Protect the vector CL_Obj and
         * re-derive the raw data pointer each iteration — a raw `data` held
         * across the recursion would dangle after compaction. */
        CL_GC_PROTECT(tmpl);
        for (i = 0; i < n; i++) {
            CL_Obj *data = cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(tmpl));
            scan_qq_for_boxing(data[i], vars, n_vars,
                               mutated, captured, closure_depth);
        }
        CL_GC_UNPROTECT(1);
        return;
    }

    if (!CL_CONS_P(tmpl)) return;

    if (cl_car(tmpl) == SYM_UNQUOTE || cl_car(tmpl) == SYM_UNQUOTE_SPLICING) {
        /* The unquoted expression is evaluated at runtime — scan it */
        scan_body_for_boxing(cl_car(cl_cdr(tmpl)), vars, n_vars,
                             mutated, captured, closure_depth);
        return;
    }

    /* Nested quasiquote — skip deeper nesting levels */
    if (cl_car(tmpl) == SYM_QUASIQUOTE) return;

    /* Walk list elements looking for unquotes.
     * GC SAFETY: the recursive scan allocates — protect the walk cursor. */
    {
        CL_Obj cur = tmpl;
        CL_GC_PROTECT(cur);
        while (CL_CONS_P(cur)) {
            scan_qq_for_boxing(cl_car(cur), vars, n_vars,
                               mutated, captured, closure_depth);
            cur = cl_cdr(cur);
        }
        CL_GC_UNPROTECT(1);
    }
}

/*
 * Recursive tree walker that determines which vars are mutated (target of setq)
 * and which are captured (referenced inside a lambda/flet/labels body).
 * closure_depth starts at 0; increments when entering closure-creating forms.
 */
/* Recursion depth limits for scanner — prevents C stack overflow from deeply
 * nested forms and macro chains (e.g., misc-extensions new-let in fset) */
static int scan_macro_depth = 0;
#define SCAN_MACRO_MAX_DEPTH 50
static int scan_recurse_depth = 0;
#define SCAN_MAX_RECURSE_DEPTH 500

void scan_body_for_boxing(CL_Obj form, CL_Obj *vars, int n_vars,
                          uint8_t *mutated, uint8_t *captured,
                          int closure_depth)
{
    CL_Obj head, rest;

top:
    if (CL_NULL_P(form) || CL_FIXNUM_P(form) || CL_CHAR_P(form))
        return;

    if (scan_recurse_depth >= SCAN_MAX_RECURSE_DEPTH)
        return;

    /* Hard safety net against GC-root-stack exhaustion.  scan_recurse_depth
     * above only counts the general list walk — the macro-expansion
     * fall-through and the LET/MACROLET/FLET/COND/... special-form handlers
     * all recurse into scan_body_for_boxing WITHOUT bumping it, while each
     * level holds GC roots (the walk cursor, the body, the macro expansion)
     * across its recursive call.  Deeply macro-nested code therefore recurses
     * far past SCAN_MAX_RECURSE_DEPTH and can drive gc_root_count to the
     * per-thread CL_GC_ROOT_STACK_SIZE ceiling, where cl_gc_pop_roots
     * abort()s the process (observed compiling serapeum's sequences.lisp:
     * cascading DO-VECTOR / SEQ-DISPATCH / WITH-VREF expansions).  Bail out
     * before that.  This is a conservative best-effort pre-scan — bailing
     * only risks under-boxing a variable nested absurdly deep (the prior
     * behaviour when the macro/recurse limits fire), which is far safer than
     * crashing the compile.  Leave headroom for cl_cons's transient pushes
     * and the few roots this frame still takes below. */
    if (gc_root_count >= CL_GC_ROOT_STACK_SIZE - 64) {
#ifdef DEBUG_COMPILER
        static int warned_root_limit = 0;
        if (!warned_root_limit) {
            warned_root_limit = 1;
            fprintf(stderr,
                    "; note: boxing pre-scan truncated at GC-root limit "
                    "(deeply macro-nested form); some variables in the "
                    "deepest forms may not be boxed\n");
        }
#endif
        return;
    }

    /* Symbol reference */
    if (CL_SYMBOL_P(form)) {
        if (closure_depth > 0) {
            int idx = find_var_index(form, vars, n_vars);
            if (idx >= 0) {
                captured[idx] = 1;
            } else if (cl_active_compiler) {
                /* A SYMBOL-MACROLET symbol read inside a closure resolves to its
                 * expansion (often an enclosing variable); scan the expansion so
                 * that variable is marked captured for boxing. */
                CL_Obj sm = cl_env_lookup_symbol_macro(cl_active_compiler->env, form);
                if (!CL_NULL_P(sm)) {
                    CL_GC_PROTECT(sm);
                    scan_body_for_boxing(sm, vars, n_vars,
                                         mutated, captured, closure_depth);
                    CL_GC_UNPROTECT(1);
                }
            }
        }
        return;
    }

    if (!CL_CONS_P(form)) return;

    head = cl_car(form);
    rest = cl_cdr(form);

    /* (quote ...) — skip entirely */
    if (head == SYM_QUOTE) return;

    /* (declare specifier...) — declarations are metadata, never evaluated.
     * Walking them with the general fall-through tries to macroexpand any
     * cons whose head names a registered macro, but inside (type T-SPEC ...)
     * or (ftype T-SPEC ...) the T-SPEC is a *type* specifier, not a form.
     * Symbols can be both deftype'd and macrobound to different things
     * (e.g. serapeum's `->`), so the spurious expansion calls the macro
     * with type-spec arguments and corrupts compiler state. */
    if (head == SYM_DECLARE) return;

    /* (quasiquote tmpl) — only scan unquoted subforms, not the template
     * structure itself. Without this, macro calls inside backquote templates
     * get spuriously expanded by the scanner with raw UNQUOTE forms. */
    if (head == SYM_QUASIQUOTE) {
        scan_qq_for_boxing(cl_car(rest), vars, n_vars,
                           mutated, captured, closure_depth);
        return;
    }

    /* (setq var val var val ...) or (setf place val place val ...) */
    if (head == SYM_SETQ || head == SYM_SETF) {
        CL_Obj pairs = rest;
        /* GC SAFETY: the recursive scans and setf-expander cl_vm_apply below
         * allocate/compact — protect the walk cursor like the sibling
         * LET/CASE/DO/FLET/handler-bind handlers (fixed in prior tiers). */
        CL_GC_PROTECT(pairs);
        while (CL_CONS_P(pairs) && CL_CONS_P(cl_cdr(pairs))) {
            CL_Obj place = cl_car(pairs);
            CL_Obj val = cl_car(cl_cdr(pairs));
            /* For symbols (simple variable places), mark as mutated */
            if (CL_SYMBOL_P(place)) {
                int idx = find_var_index(place, vars, n_vars);
                if (idx >= 0) {
                    mutated[idx] = 1;
                    if (closure_depth > 0) captured[idx] = 1;
                } else if (cl_active_compiler) {
                    /* (setf SYM val) where SYM is a SYMBOL-MACROLET symbol —
                     * the real place is its expansion.  Scan (setf <expansion>
                     * val) so the underlying variable is marked mutated (and, in
                     * a closure, captured) and therefore boxed.  Without this a
                     * write through a symbol-macro inside a FLET/LABELS closure
                     * is silently dropped, the symbol-macrolet analogue of the
                     * macrolet boxing bug. */
                    CL_Obj sm = cl_env_lookup_symbol_macro(cl_active_compiler->env,
                                                           place);
                    if (!CL_NULL_P(sm)) {
                        CL_Obj sf;
                        CL_GC_PROTECT(sm);
                        sf = cl_cons(val, CL_NIL);
                        CL_GC_PROTECT(sf);
                        sf = cl_cons(sm, sf);
                        sf = cl_cons(SYM_SETF, sf);
                        scan_body_for_boxing(sf, vars, n_vars,
                                             mutated, captured, closure_depth);
                        CL_GC_UNPROTECT(2);
                    }
                }
            } else if (CL_CONS_P(place) && cl_car(place) == SYM_THE) {
                /* (setf (the type var) val) — the inner var is mutated */
                CL_Obj inner = cl_car(cl_cdr(cl_cdr(place)));
                if (CL_SYMBOL_P(inner)) {
                    int idx = find_var_index(inner, vars, n_vars);
                    if (idx >= 0) mutated[idx] = 1;
                    if (closure_depth > 0 && idx >= 0) captured[idx] = 1;
                } else {
                    scan_body_for_boxing(place, vars, n_vars,
                                         mutated, captured, closure_depth);
                }
            } else if (CL_CONS_P(place) && cl_car(place) == SYM_PROGN) {
                /* (setf (progn form... var) val) — the inner place is mutated */
                CL_Obj pforms = cl_cdr(place);
                /* Walk to last element (the actual place).
                 * GC SAFETY: the recursive scan allocates — protect cursor. */
                CL_GC_PROTECT(pforms);
                while (CL_CONS_P(pforms) && CL_CONS_P(cl_cdr(pforms))) {
                    scan_body_for_boxing(cl_car(pforms), vars, n_vars,
                                         mutated, captured, closure_depth);
                    pforms = cl_cdr(pforms);
                }
                if (CL_CONS_P(pforms)) {
                    CL_Obj inner = cl_car(pforms);
                    if (CL_SYMBOL_P(inner)) {
                        int idx = find_var_index(inner, vars, n_vars);
                        if (idx >= 0) mutated[idx] = 1;
                        if (closure_depth > 0 && idx >= 0) captured[idx] = 1;
                    } else {
                        scan_body_for_boxing(inner, vars, n_vars,
                                             mutated, captured, closure_depth);
                    }
                }
                CL_GC_UNPROTECT(1); /* pforms */
            } else if (CL_CONS_P(place) &&
                       cl_car(place) == cl_intern_in("VALUES", 6, cl_package_cl)) {
                /* (setf (values v1 v2 ...) val) — each vi is mutated.  This
                 * place is expanded specially by compile_setf_place (not via a
                 * registered setf-expander), so the scanner must mark each
                 * variable itself; otherwise a write through it inside a closure
                 * is silently dropped.  serapeum's MVFOLD compiler macro emits
                 * (setf (values seed0 seed1) (funcall ...)) hoisted into a
                 * do-each thunk, so seed0/seed1 must be boxed to survive. */
                CL_Obj vplaces = cl_cdr(place);
                CL_GC_PROTECT(vplaces);
                while (CL_CONS_P(vplaces)) {
                    CL_Obj vp = cl_car(vplaces);
                    if (CL_SYMBOL_P(vp)) {
                        int idx = find_var_index(vp, vars, n_vars);
                        if (idx >= 0) {
                            mutated[idx] = 1;
                            if (closure_depth > 0) captured[idx] = 1;
                        }
                    } else {
                        scan_body_for_boxing(vp, vars, n_vars,
                                             mutated, captured, closure_depth);
                    }
                    vplaces = cl_cdr(vplaces);
                }
                CL_GC_UNPROTECT(1);
            } else if (CL_CONS_P(place)) {
                /* Generalized place — check for define-setf-expander.
                 * If one is registered, call it to get the expansion form
                 * and scan THAT for mutations.  This catches cases like
                 * (setf (lookup coll key) val) where the expander generates
                 * (setq coll ...) internally. */
                CL_Obj place_head = cl_car(place);
                int expanded_setf = 0;
                if (CL_SYMBOL_P(place_head) && scan_macro_depth < SCAN_MACRO_MAX_DEPTH) {
                    CL_Obj expander_fn_found = CL_NIL;
                    {
                        CL_Obj exp_entry;
                        cl_tables_rdlock();
                        exp_entry = setf_expander_table;
                        while (!CL_NULL_P(exp_entry)) {
                            CL_Obj pair = cl_car(exp_entry);
                            if (cl_car(pair) == place_head) {
                                expander_fn_found = cl_cdr(pair);
                                break;
                            }
                            exp_entry = cl_cdr(exp_entry);
                        }
                        cl_tables_rwunlock();
                    }
                    if (!CL_NULL_P(expander_fn_found)) {
                        {
                            /* Found expander — call it with error recovery */
                            CL_Obj expander_fn = expander_fn_found;
                            CL_Obj call_args[2];
                            int saved_sp = cl_vm.sp;
                            int saved_fp = cl_vm.fp;
                            int saved_dyn = cl_dyn_top;
                            int saved_nlx = cl_nlx_top;
                            int saved_handler = cl_handler_top;
                            int saved_restart = cl_restart_top;
                            int saved_debugger = cl_debugger_enabled;
                            int saved_gc_roots = gc_root_count;
                            cl_debugger_enabled = 0;
                            call_args[0] = place;
                            call_args[1] = val;
                            scan_macro_depth++;
                            {
                                int err; CL_CATCH(err);
                                if (err == 0) {
                                    CL_Obj expansion;
                                    expansion = cl_vm_apply(expander_fn, call_args, 2);
                                    CL_UNCATCH();
                                    cl_debugger_enabled = saved_debugger;
                                    cl_handler_top = saved_handler;
                                    cl_restart_top = saved_restart;
                                    CL_GC_PROTECT(expansion);
                                    scan_body_for_boxing(expansion, vars, n_vars,
                                                         mutated, captured, closure_depth);
                                    CL_GC_UNPROTECT(1);
                                    expanded_setf = 1;
                                } else {
                                    CL_UNCATCH();
                                    cl_vm.sp = saved_sp;
                                    cl_vm.fp = saved_fp;
                                    cl_dynbind_restore_to(saved_dyn);
                                    cl_nlx_top = saved_nlx;
                                    cl_handler_top = saved_handler;
                                    cl_restart_top = saved_restart;
                                    cl_debugger_enabled = saved_debugger;
                                    gc_root_count = saved_gc_roots;
                                }
                            }
                            scan_macro_depth--;
                        }
                    }
                }
                if (!expanded_setf) {
                    /* No expander or expansion failed — scan sub-expressions */
                    scan_body_for_boxing(place, vars, n_vars,
                                         mutated, captured, closure_depth);
                }
            } else {
                scan_body_for_boxing(place, vars, n_vars,
                                     mutated, captured, closure_depth);
            }
            /* GC SAFETY: the place processing above can compact — re-read
             * `val` through the protected cursor before scanning it. */
            val = cl_car(cl_cdr(pairs));
            scan_body_for_boxing(val, vars, n_vars, mutated, captured, closure_depth);
            pairs = cl_cdr(cl_cdr(pairs));
        }
        CL_GC_UNPROTECT(1); /* pairs */
        return;
    }

    /* (lambda params . body) — body is at increased closure depth.
     * GC SAFETY: protect the `body` cursor across the recursive scan (which
     * macroexpands → allocates → can compact) so the advancing cl_cdr does not
     * follow a stale offset.  Same fix applies to every body-walking handler
     * below. */
    if (head == SYM_LAMBDA) {
        CL_Obj body;
        if (!CL_CONS_P(rest)) return; /* malformed lambda */
        body = cl_cdr(rest); /* skip param list */
        CL_GC_PROTECT(body);
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth + 1);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(1);
        return;
    }

    /* (defun name params . body) — body is at increased closure depth.
     * (named-lambda name params . body) has the same shape and is a closure
     * just like LAMBDA: scanning it as a generic call would walk the body at
     * the *enclosing* depth, so a variable it mutates would read as a plain
     * local write rather than a capture and never get boxed. */
    if (head == SYM_DEFUN || head == SYM_NAMED_LAMBDA) {
        CL_Obj body;
        if (!CL_CONS_P(rest) || !CL_CONS_P(cl_cdr(rest))) return;
        body = cl_cdr(cl_cdr(rest)); /* skip name and param list */
        CL_GC_PROTECT(body);
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth + 1);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(1);
        return;
    }

    /* (defmacro ...) — skip entirely; macro bodies are template code
     * that shouldn't be scanned for variable mutations */
    if (head == SYM_DEFMACRO) return;

    /* (case/ecase/typecase/etypecase keyform (key body...)...) —
     * Scan keyform and clause bodies, but NOT the keys (they are literals,
     * not evaluable forms; a key like TUPLE could be a macro name). */
    if (head == SYM_CASE || head == SYM_ECASE ||
        head == SYM_TYPECASE || head == SYM_ETYPECASE) {
        CL_Obj keyform, clauses;
        if (!CL_CONS_P(rest)) return;
        /* Read keyform and clauses up front, then protect: the keyform scan
         * below can compact, which would otherwise stale a `cl_cdr(rest)` read
         * afterwards (rest itself is an unprotected local). */
        keyform = cl_car(rest);
        clauses = cl_cdr(rest);
        CL_GC_PROTECT(keyform);
        CL_GC_PROTECT(clauses);
        /* Scan keyform */
        scan_body_for_boxing(keyform, vars, n_vars,
                             mutated, captured, closure_depth);
        /* Scan clause bodies (skip keys) */
        while (CL_CONS_P(clauses)) {
            CL_Obj clause = cl_car(clauses);
            if (CL_CONS_P(clause)) {
                /* Skip key (car), scan body forms (cdr) */
                CL_Obj cbody = cl_cdr(clause);
                CL_GC_PROTECT(cbody);
                while (CL_CONS_P(cbody)) {
                    scan_body_for_boxing(cl_car(cbody), vars, n_vars,
                                         mutated, captured, closure_depth);
                    cbody = cl_cdr(cbody);
                }
                CL_GC_UNPROTECT(1);
            }
            clauses = cl_cdr(clauses);
        }
        CL_GC_UNPROTECT(2);
        return;
    }

    /* (restart-case main-form (name (params...) [options...] body...) ...)
     * compile_restart_case turns every clause into a (lambda (params...) body)
     * closure that runs at the catch landing.  A clause body that mutates an
     * enclosing variable — e.g. a STORE-VALUE restart doing (setf place new),
     * as CCASE/CTYPECASE require — therefore captures that variable through a
     * closure, so it must be scanned at closure_depth + 1.  Without this the
     * variable is marked mutated but not captured, so it is never boxed and the
     * handler's setf updates a private copy that the main form can't see. */
    if (head == SYM_RESTART_CASE) {
        CL_Obj main_form, clauses;
        if (!CL_CONS_P(rest)) return;
        main_form = cl_car(rest);
        clauses = cl_cdr(rest);
        CL_GC_PROTECT(main_form);
        CL_GC_PROTECT(clauses);
        scan_body_for_boxing(main_form, vars, n_vars,
                             mutated, captured, closure_depth);
        while (CL_CONS_P(clauses)) {
            CL_Obj clause = cl_car(clauses);
            /* Skip the restart name (car) and param list (cadr); scan the
             * remaining options + body forms as closure bodies (depth + 1).
             * Scanning the :report/:interactive/:test option values at the
             * raised depth is also correct — they too become closures. */
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause))) {
                CL_Obj cbody = cl_cdr(cl_cdr(clause));
                CL_GC_PROTECT(cbody);
                while (CL_CONS_P(cbody)) {
                    scan_body_for_boxing(cl_car(cbody), vars, n_vars,
                                         mutated, captured, closure_depth + 1);
                    cbody = cl_cdr(cbody);
                }
                CL_GC_UNPROTECT(1);
            }
            clauses = cl_cdr(clauses);
        }
        CL_GC_UNPROTECT(2);
        return;
    }

    /* (handler-bind ((type handler-form) ...) body...)
     * The clause CAR is a condition TYPE specifier, not an evaluable form.
     * The general fall-through would walk each clause and try to macroexpand
     * (type handler-form) as a call — and a condition type that is ALSO a
     * macro (e.g. serapeum's DISPATCH-CASE-ERROR, a `(&key type datum)` macro
     * that is also the condition class name) gets bogus args, signaling
     * "odd number of keyword arguments" out of the speculative expansion.
     * (This is the same hazard the typecase/case/do/macrolet handlers above
     * guard against.)  Scan only the handler EXPRESSION (cadr) of each clause
     * and the body, skipping the type spec.  HANDLER-CASE is a macro that
     * expands to handler-bind + typecase, so this also covers it (the
     * typecase keys are skipped by the typecase handler above). */
    if (head == SYM_HANDLER_BIND) {
        CL_Obj clauses, body;
        if (!CL_CONS_P(rest)) return;
        clauses = cl_car(rest);
        body = cl_cdr(rest); /* skip the clause list */
        CL_GC_PROTECT(clauses);
        CL_GC_PROTECT(body);
        /* Scan each clause's handler expression (cadr), skip the type (car) */
        while (CL_CONS_P(clauses)) {
            CL_Obj clause = cl_car(clauses);
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause)))
                scan_body_for_boxing(cl_car(cl_cdr(clause)), vars, n_vars,
                                     mutated, captured, closure_depth);
            clauses = cl_cdr(clauses);
        }
        /* Scan body forms */
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(2); /* body, clauses */
        return;
    }

    /* (amilisp::%handler-case form (type ([var]) . body)...) — the HANDLER-CASE
     * special form.  The clause CAR is a type spec and the CADR a variable
     * list, not evaluated code; scan the form and each clause body. */
    if (cl_handler_case_sym != CL_NIL && head == cl_handler_case_sym) {
        CL_Obj clauses;
        if (!CL_CONS_P(rest)) return;
        clauses = cl_cdr(rest);
        CL_GC_PROTECT(clauses);
        scan_body_for_boxing(cl_car(rest), vars, n_vars,
                             mutated, captured, closure_depth);
        while (CL_CONS_P(clauses)) {
            CL_Obj clause = cl_car(clauses);
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause))) {
                CL_Obj cbody = cl_cdr(cl_cdr(clause));
                CL_GC_PROTECT(cbody);
                while (CL_CONS_P(cbody)) {
                    scan_body_for_boxing(cl_car(cbody), vars, n_vars,
                                         mutated, captured, closure_depth);
                    cbody = cl_cdr(cbody);
                }
                CL_GC_UNPROTECT(1);
            }
            clauses = cl_cdr(clauses);
        }
        CL_GC_UNPROTECT(1);
        return;
    }

    /* (macrolet ((name lambda-list . body) ...) . body)
     * The macro definitions are templates compiled at the macrolet site
     * (see compile_macrolet) — they are NOT regular code in this scope.
     * Walking them as code mistakes the binding car (the macro name) for
     * a function call, e.g. (plet-if (params) `template) gets seen as a
     * 2-arg call to a globally-defined plet-if and the scanner expands it
     * with bogus args.  Cascading expansions can hit a macro that errors
     * (signaling lparallel's %plet-if expecting 4 args, getting 2), which
     * longjmps through scan_body_for_boxing's CL_CATCH and — empirically —
     * smashes bi_compile_file's stack canary on return.  Skip the bindings
     * entirely and only scan the macrolet body. */
    if (head == SYM_MACROLET) {
        CL_Obj bindings, body;
        CL_CompEnv *env = cl_active_compiler ? cl_active_compiler->env : NULL;
        int saved_lmc = env ? env->local_macro_count : 0;
        if (!CL_CONS_P(rest)) return;
        bindings = cl_car(rest);
        body = cl_cdr(rest); /* skip the bindings list */
        CL_GC_PROTECT(body);
        /* Install the macrolet's local macro expanders into the compiler env so
         * the body scan below can macroexpand macrolet-defined macro calls and
         * thereby see the SETF/closure forms they hide.  Without this a variable
         * that is only ever assigned through a macrolet macro used inside a
         * FLET/LABELS closure is never marked mutated, so it is not boxed and the
         * write is silently dropped — the closure mutates a private copy while
         * the enclosing binding stays at its initial value.  This surfaced as
         * local-time:parse-timestring returning NIL for month/day (its
         * PARSE-INTEGER-INTO macrolet does `(setf month value)` inside the date
         * labels functions), which made chipi's influx persistence parse fail.
         * Mirror compile_macrolet, but guard the expander compile so a malformed
         * macrolet can't abort the whole boxing pre-scan (best effort: on failure
         * we just scan the body without expanders, the prior behaviour). */
        if (env && scan_macro_depth < SCAN_MACRO_MAX_DEPTH) {
            int saved_sp = cl_vm.sp;
            int saved_fp = cl_vm.fp;
            int saved_dyn = cl_dyn_top;
            int saved_nlx = cl_nlx_top;
            int saved_handler = cl_handler_top;
            int saved_restart = cl_restart_top;
            int saved_debugger = cl_debugger_enabled;
            int saved_gc_roots = gc_root_count;
            cl_debugger_enabled = 0;
            scan_macro_depth++;
            {
                int err; CL_CATCH(err);
                if (err == 0) {
                    cl_macrolet_install_expanders(env, bindings);
                    CL_UNCATCH();
                } else {
                    CL_UNCATCH();
                    cl_vm.sp = saved_sp;
                    cl_vm.fp = saved_fp;
                    cl_dynbind_restore_to(saved_dyn);
                    cl_nlx_top = saved_nlx;
                    gc_root_count = saved_gc_roots;
                    if (env) env->local_macro_count = saved_lmc;
                }
            }
            scan_macro_depth--;
            cl_debugger_enabled = saved_debugger;
            cl_handler_top = saved_handler;
            cl_restart_top = saved_restart;
        }
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(1);
        /* Pop the local macros we added so the env is unchanged for siblings
         * (compile_macrolet re-installs them for the real compile pass). */
        if (env) env->local_macro_count = saved_lmc;
        return;
    }

    /* (symbol-macrolet ((sym expansion) ...) . body)
     * Same reasoning as MACROLET — the bindings are (sym expansion) pairs,
     * not forms to evaluate.  Walking them generically would treat sym as
     * a function call head if it has any arguments.
     *
     * The bindings MUST be registered into the active compiler env while we
     * walk the body: this scanner speculatively macroexpands any global macro
     * it meets (to see through macro-generated closures), building that macro's
     * &environment from cl_build_lex_env(cl_active_compiler->env).  An
     * &environment-aware expander that resolves a symbol-macro via
     * (macroexpand sym env) — e.g. ironclad's GET-KECCAK-ROTATE-OFFSET, which
     * does (eval (trivial-macroexpand-all x env)) for an x bound by an
     * enclosing (symbol-macrolet ((x N)) ...) emitted by dotimes-unrolled —
     * will otherwise see x unbound and signal "Unbound variable: X" mid-scan.
     * That speculative failure cannot be reliably contained by the CL_CATCH in
     * the macro-expansion block below: cl_error signals through the condition
     * system and unwinds the NLX stack first, so an outer handler / unwind-
     * protect installed by ASDF's compile driver hijacks it and aborts the whole
     * compilation.  Registering the bindings makes the expansion SUCCEED (it
     * folds to the constant), so no spurious error is ever raised. */
    if (head == SYM_SYMBOL_MACROLET) {
        CL_Obj bindings, body;
        CL_CompEnv *env = cl_active_compiler ? cl_active_compiler->env : NULL;
        int saved_smc = env ? env->symbol_macro_count : 0;
        if (!CL_CONS_P(rest)) return;
        bindings = cl_car(rest);
        body = cl_cdr(rest);
        if (env) {
            CL_Obj b = bindings;
            while (CL_CONS_P(b)) {
                CL_Obj binding = cl_car(b);
                if (CL_CONS_P(binding) && CL_CONS_P(cl_cdr(binding)))
                    cl_env_add_symbol_macro(env, cl_car(binding),
                                            cl_car(cl_cdr(binding)));
                b = cl_cdr(b);
            }
        }
        CL_GC_PROTECT(body);
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(1);
        /* Pop the symbol-macros we added so the env is unchanged for siblings. */
        if (env) env->symbol_macro_count = saved_smc;
        return;
    }

    /* (flet/labels ((name params . body) ...) . body) */
    if (head == SYM_FLET || head == SYM_LABELS) {
        CL_Obj defs, body;
        if (!CL_CONS_P(rest)) return;
        defs = cl_car(rest);
        body = cl_cdr(rest);
        CL_GC_PROTECT(defs);
        CL_GC_PROTECT(body);
        /* Scan each function definition body at increased depth */
        while (CL_CONS_P(defs)) {
            CL_Obj def = cl_car(defs);
            CL_Obj fbody;
            if (!CL_CONS_P(def) || !CL_CONS_P(cl_cdr(def))) { defs = cl_cdr(defs); continue; }
            fbody = cl_cdr(cl_cdr(def)); /* skip name and params */
            CL_GC_PROTECT(fbody);
            while (CL_CONS_P(fbody)) {
                scan_body_for_boxing(cl_car(fbody), vars, n_vars,
                                     mutated, captured, closure_depth + 1);
                fbody = cl_cdr(fbody);
            }
            CL_GC_UNPROTECT(1);
            defs = cl_cdr(defs);
        }
        /* Scan outer body at current depth */
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(2);
        return;
    }

    /* (let/let* ((var value)...) body...)
     * Must NOT scan binding clauses as macro calls — the var name in
     * (check-type (unless ...)) would be mistaken for a macro call
     * when the var name shadows a registered macro (e.g. log4cl's use of
     * CHECK-TYPE as a let* variable shadowing the standard macro). */
    if (head == SYM_LET || head == SYM_LETSTAR) {
        CL_Obj bindings, body;
        if (!CL_CONS_P(rest)) return;
        bindings = cl_car(rest);
        body = cl_cdr(rest);
        /* GC SAFETY: the recursive scan_body_for_boxing below macroexpands and
         * therefore allocates/compacts; protect the `bindings` and `body`
         * cursors so the cl_cdr that advances them does not follow a stale
         * pre-compaction offset (observed as "CDR: argument is not of type
         * LIST" at compile time when a LET body form is a heavily-expanding
         * macro under heap pressure). */
        CL_GC_PROTECT(bindings);
        CL_GC_PROTECT(body);
        /* Scan value forms only (skip var names) */
        while (CL_CONS_P(bindings)) {
            CL_Obj clause = cl_car(bindings);
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause))) {
                /* (var value-form) — scan value-form */
                scan_body_for_boxing(cl_car(cl_cdr(clause)), vars, n_vars,
                                     mutated, captured, closure_depth);
            }
            /* (var) with no value-form, or bare symbol: nothing to scan */
            bindings = cl_cdr(bindings);
        }
        /* Scan body */
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(2);
        return;
    }

    /* (destructuring-bind pattern value-form body...)
     * The pattern is a lambda-list-like structure, NOT a form to evaluate.
     * Without this handler the general-form walk would recursively scan the
     * pattern and try to macroexpand any cons whose head collides with a
     * registered macro (e.g. fiveam's TEST vs a `(test)` pattern), which
     * can invoke the expander with wrong arity and crash the VM. */
    if (head == SYM_DESTRUCTURING_BIND) {
        CL_Obj value_form, body;
        if (!CL_CONS_P(rest) || !CL_CONS_P(cl_cdr(rest))) return;
        /* Read value-form and body up front, then protect: scanning value-form
         * can compact and otherwise stale a later cl_cdr(rest) for the body. */
        value_form = cl_car(cl_cdr(rest));   /* skip pattern (structural) */
        body = cl_cdr(cl_cdr(rest));
        CL_GC_PROTECT(value_form);
        CL_GC_PROTECT(body);
        scan_body_for_boxing(value_form, vars, n_vars,
                             mutated, captured, closure_depth);
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(2);
        return;
    }

    /* (multiple-value-bind (var...) values-form body...)
     * Same reasoning as let/let*: the var list is not a macro call. */
    if (head == SYM_MULTIPLE_VALUE_BIND) {
        CL_Obj value_form, body;
        if (!CL_CONS_P(rest) || !CL_CONS_P(cl_cdr(rest))) return;
        value_form = cl_car(cl_cdr(rest));
        body = cl_cdr(cl_cdr(rest));
        CL_GC_PROTECT(value_form);
        CL_GC_PROTECT(body);
        /* Scan values-form, then body */
        scan_body_for_boxing(value_form, vars, n_vars,
                             mutated, captured, closure_depth);
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(2);
        return;
    }

    /* (do/do* ((var init [step])...) (end-test result...) body...)
     * Must NOT scan binding clauses as macro calls — the var name in
     * (TUPLE TUPLE (CDR TUPLE)) would be mistaken for a macro call
     * when the var name has a macro definition (e.g. FSet's TUPLE macro). */
    if (head == SYM_DO || head == SYM_DO_STAR) {
        CL_Obj var_clauses, end_clause, body;
        if (!CL_CONS_P(rest)) return;
        var_clauses = cl_car(rest);
        if (!CL_CONS_P(cl_cdr(rest))) return;
        end_clause = cl_car(cl_cdr(rest));
        body = cl_cdr(cl_cdr(rest));
        /* Protect all three cursors: the init/step/end/body scans below compact
         * and would otherwise stale these bare locals (and the `res` cursor). */
        CL_GC_PROTECT(var_clauses);
        CL_GC_PROTECT(end_clause);
        CL_GC_PROTECT(body);
        /* Scan init and step forms (skip var names) */
        while (CL_CONS_P(var_clauses)) {
            CL_Obj clause = cl_car(var_clauses);
            if (CL_CONS_P(clause) && CL_CONS_P(cl_cdr(clause))) {
                /* init form */
                scan_body_for_boxing(cl_car(cl_cdr(clause)), vars, n_vars,
                                     mutated, captured, closure_depth);
                /* step form (if present) */
                if (CL_CONS_P(cl_cdr(cl_cdr(clause))))
                    scan_body_for_boxing(cl_car(cl_cdr(cl_cdr(clause))), vars, n_vars,
                                         mutated, captured, closure_depth);
            }
            var_clauses = cl_cdr(var_clauses);
        }
        /* Scan end-test and result forms */
        if (CL_CONS_P(end_clause)) {
            CL_Obj res = cl_cdr(end_clause);
            CL_GC_PROTECT(res);   /* protect before the end-test scan compacts */
            scan_body_for_boxing(cl_car(end_clause), vars, n_vars,
                                 mutated, captured, closure_depth);
            while (CL_CONS_P(res)) {
                scan_body_for_boxing(cl_car(res), vars, n_vars,
                                     mutated, captured, closure_depth);
                res = cl_cdr(res);
            }
            CL_GC_UNPROTECT(1);
        }
        /* Scan body */
        while (CL_CONS_P(body)) {
            scan_body_for_boxing(cl_car(body), vars, n_vars,
                                 mutated, captured, closure_depth);
            body = cl_cdr(body);
        }
        CL_GC_UNPROTECT(3); /* body, end_clause, var_clauses */
        return;
    }


    /* Expand macros before scanning so we can see through macro-generated
     * lambdas and setf forms. Save/restore VM state to handle expansion
     * errors gracefully without corrupting compiler state.
     *
     * LOCAL (macrolet) macros must be expanded here too: cl_macro_p only
     * knows the global macro table, so a macrolet-defined macro call would
     * otherwise fall through to the general walk and its hidden mutations
     * would be missed (the boxing bug fixed in the SYM_MACROLET handler
     * above only installs the expanders — this is where they get used).
     * cl_macroexpand_1_env consults the lex-env's local macros first, so
     * expansion below works once the env carries them. */
    if (CL_SYMBOL_P(head) &&
        (cl_macro_p(head) ||
         (cl_active_compiler &&
          !CL_NULL_P(cl_env_lookup_local_macro(cl_active_compiler->env, head)))) &&
        scan_macro_depth < SCAN_MACRO_MAX_DEPTH) {
        int saved_sp = cl_vm.sp;
        int saved_fp = cl_vm.fp;
        int saved_dyn = cl_dyn_top;
        int saved_nlx = cl_nlx_top;
        int saved_handler = cl_handler_top;
        int saved_restart = cl_restart_top;
        int saved_debugger = cl_debugger_enabled;
        int saved_gc_roots;
        /* GC SAFETY: FORM is read again after cl_build_lex_env below, which
         * conses whenever the compiler env carries local macros -- i.e. for
         * every global macro call inside a MACROLET body, and LOOP expands
         * into one.  Unprotected, the expander was handed a stale form
         * ("CAR: argument is not of type LIST (got FUNCTION)" out of the
         * speculative expansion, which then aborted the whole compile when
         * a HANDLER-CASE was watching), and the identity test and the
         * general walk below read it after the expansion as well.
         * Registered BEFORE the root count is saved, so the error path's
         * restore keeps it. */
        CL_GC_PROTECT(form);
        saved_gc_roots = gc_root_count;
        cl_debugger_enabled = 0;  /* Suppress debugger during expansion */
#ifdef DEBUG_SCANNER
        fprintf(stderr, "[scanner] expanding macro: %s\n", cl_symbol_name(head));
#endif
        scan_macro_depth++;
        {
        int err; CL_CATCH(err);
        if (err == 0) {
            CL_Obj expanded;
            CL_Obj scan_env = CL_NIL;
            if (cl_active_compiler)
                scan_env = cl_build_lex_env(cl_active_compiler->env);
            expanded = cl_macroexpand_1_env(form, scan_env);
            CL_UNCATCH();
            cl_debugger_enabled = saved_debugger;
            cl_handler_top = saved_handler;
            cl_restart_top = saved_restart;
            if (expanded != form) {
                CL_GC_PROTECT(expanded);
                scan_body_for_boxing(expanded, vars, n_vars,
                                     mutated, captured, closure_depth);
                CL_GC_UNPROTECT(2); /* expanded, form */
                scan_macro_depth--;
                return;
            }
        } else {
#ifdef DEBUG_SCANNER
            fprintf(stderr, "[scanner-err] macro %s expansion failed: %s\n",
                    cl_symbol_name(head), cl_error_msg);
            fflush(stderr);
#endif
            CL_UNCATCH();
            cl_vm.sp = saved_sp;
            cl_vm.fp = saved_fp;
            cl_dynbind_restore_to(saved_dyn);
            cl_nlx_top = saved_nlx;
            cl_handler_top = saved_handler;
            cl_restart_top = saved_restart;
            cl_debugger_enabled = saved_debugger;
            gc_root_count = saved_gc_roots;
        }
        }
        CL_GC_UNPROTECT(1); /* form */
        scan_macro_depth--;
    }

    /* General form: scan all sub-expressions (iterative list walk).
     * GC SAFETY: `cur` is re-read after the recursive scan_body_for_boxing
     * call (which can macroexpand and thus allocate/compact).  Protect it so
     * the walk doesn't follow a stale offset after a compaction. */
    {
        CL_Obj cur = form;
        scan_recurse_depth++;
        CL_GC_PROTECT(cur);
        while (CL_CONS_P(cur)) {
            CL_Obj elem = cl_car(cur);
            cur = cl_cdr(cur);
            if (CL_NULL_P(cur)) {
                /* Last element: tail-call via goto to save stack */
                scan_recurse_depth--;
                form = elem;
                CL_GC_UNPROTECT(1);
                goto top;
            }
            scan_body_for_boxing(elem, vars, n_vars,
                                 mutated, captured, closure_depth);
        }
        CL_GC_UNPROTECT(1);
        scan_recurse_depth--;
    }
}

/*
 * Determine which vars need boxing (both mutated AND captured across closure boundary).
 * body is the list of body forms to scan.
 * Sets boxed_out[i] = 1 for each var that needs boxing.
 */
void determine_boxed_vars(CL_Obj body, CL_Obj *vars, int n_vars,
                          uint8_t *boxed_out)
{
    uint8_t mutated[CL_MAX_BINDINGS];
    uint8_t captured[CL_MAX_BINDINGS];
    int i;

    if (n_vars > CL_MAX_BINDINGS) n_vars = CL_MAX_BINDINGS;
    memset(mutated, 0, (size_t)n_vars);
    memset(captured, 0, (size_t)n_vars);
    memset(boxed_out, 0, (size_t)n_vars);

    /* GC-protect vars[] so compaction inside scan_body_for_boxing (via
     * macroexpansion / cl_vm_apply) does not leave stale pre-compaction
     * offsets in the caller's array.  Without this the eq-compare in
     * find_var_index misses and mutated/captured is under-recorded.
     * CL_GC_PROTECT (not bare cl_gc_push_root) fills gc_root_files/lines
     * under DEBUG_GC so gc_dump_roots_dbg shows accurate source locations. */
    for (i = 0; i < n_vars; i++)
        CL_GC_PROTECT(vars[i]);

    /* Scan all body forms.
     * GC SAFETY: `cur` walks `body` and is re-read (cl_cdr) after
     * scan_body_for_boxing, which macroexpands macros (e.g. INCF) in the VM
     * and therefore allocates/compacts.  Protect `body` and `cur` so the
     * cursor is not left pointing at stale memory — otherwise, with two or
     * more body forms, cl_cdr(stale cur) dereferences garbage and the scan
     * derails (observed as a gensym binding mis-compiled to a global, i.e.
     * "Unbound variable: NEW<n>"). */
    {
        CL_Obj cur = body;
        CL_GC_PROTECT(body);
        CL_GC_PROTECT(cur);
        while (CL_CONS_P(cur)) {
            scan_body_for_boxing(cl_car(cur), vars, n_vars,
                                 mutated, captured, 0);
            cur = cl_cdr(cur);
        }
        CL_GC_UNPROTECT(2);
    }

    cl_gc_pop_roots(n_vars);

    for (i = 0; i < n_vars; i++) {
        boxed_out[i] = (mutated[i] && captured[i]) ? 1 : 0;
    }
}

/* --- Let --- */

static int var_is_special(CL_Obj var, CL_Obj local_specials)
{
    return cl_symbol_specialp(var) || is_locally_special(var, local_specials);
}

/* Prelude for (let ...) / (let* ...).  Does all the binding setup and
 * compiles every non-tail body form, then pushes a CL_TAIL_LET frame
 * carrying the postlude state so compile_expr's drain can emit OP_DYNUNBIND
 * and restore env->local_count.  Returns the body's last form so the
 * trampoline can continue with it (no extra C frame), or CL_NIL when the
 * body is empty (caller emits OP_NIL).
 *
 * Splitting compile_let into prelude+postlude is what lets a chain of
 * nested LETs compile in a flat C stack: each let-prelude runs in a
 * transient C frame, returns, and the trampoline loops back to dispatch
 * the next form — instead of a fresh compile_let frame for each level. */
static CL_Obj compile_let(CL_Compiler *c, CL_Obj form, int sequential)
{
    CL_Obj bindings = cl_car(cl_cdr(form));
    CL_Obj body = cl_cdr(cl_cdr(form));
    CL_CompEnv *env = c->env;
    int saved_local_count = env->local_count;
    int saved_tail = c->in_tail;
    int special_count = 0;
    int saved_notinline_count = c->notinline_count;
    /* Captured at entry (like saved_notinline_count): the LET tail frame is
     * pushed only AFTER process_body_declarations has applied any body
     * (declare (optimize ...)), so cl_tail_push's own snapshot would be too
     * late — it must be overwritten with this pre-declaration value. */
    CL_OptimizeSettings saved_optimize = c->optimize_settings;

    /* GC-protect form components that survive across allocating calls */
    CL_GC_PROTECT(bindings);
    CL_GC_PROTECT(body);

    /* CLHS 3.1.2.1 / LET: each binding is VAR or (VAR [INIT-FORM]).  The
     * binding loops below read only the car and cadr, so (x 1 2) used to
     * compile silently as (x 1) — the stray element vanishing without a
     * word — as did a non-symbol variable, a dotted binding list, and any
     * binding past CL_MAX_BINDINGS (dropped by the parallel branch's
     * n < CL_MAX_BINDINGS guard).  Reject all of them here, naming the
     * offending binding. */
    {
        const char *who = sequential ? "LET*" : "LET";
        CL_Obj b = bindings;
        int n_bind = 0;
        while (CL_CONS_P(b)) {
            CL_Obj binding = cl_car(b);
            CL_Obj var = binding;
            int bad = 0;
            if (CL_CONS_P(binding)) {
                CL_Obj rest = cl_cdr(binding);
                var = cl_car(binding);
                /* NIL, or exactly one init-form: (VAR) / (VAR INIT) */
                if (!CL_NULL_P(rest) &&
                    !(CL_CONS_P(rest) && CL_NULL_P(cl_cdr(rest))))
                    bad = 1;
            }
            if (!bad && (!CL_SYMBOL_P(var) || CL_NULL_P(var)))
                bad = 1;
            if (bad) {
                char bbuf[128];
                cl_prin1_to_string(binding, bbuf, (int)sizeof(bbuf));
                cl_error(CL_ERR_GENERAL,
                         "%s: malformed binding %s -- expected VAR or "
                         "(VAR [INIT-FORM])", who, bbuf);
            }
            if (++n_bind > CL_MAX_BINDINGS)
                cl_error(CL_ERR_GENERAL, "%s: too many bindings (max %d)",
                         who, CL_MAX_BINDINGS);
            b = cl_cdr(b);
        }
        if (!CL_NULL_P(b)) {
            char bbuf[128];
            cl_prin1_to_string(bindings, bbuf, (int)sizeof(bbuf));
            cl_error(CL_ERR_GENERAL,
                     "%s: bindings must be a proper list, got %s", who, bbuf);
        }
    }

    /* Pre-scan body for (declare (special ...)) to find locally-special vars */
    CL_Obj local_specials = scan_local_specials(body);
    CL_GC_PROTECT(local_specials); /* protect across scan_body_for_boxing / compile_expr */

    if (sequential) {
        /* Pre-scan all bindings + body for boxing analysis */
        CL_Obj all_vars[CL_MAX_BINDINGS];
        uint8_t needs_boxing[CL_MAX_BINDINGS];
        int n_all = 0;
        {
            CL_Obj b = bindings;
            while (!CL_NULL_P(b) && n_all < CL_MAX_BINDINGS) {
                CL_Obj binding = cl_car(b);
                all_vars[n_all++] = CL_CONS_P(binding) ? cl_car(binding) : binding;
                b = cl_cdr(b);
            }
        }
        {
            uint8_t mutated[CL_MAX_BINDINGS], captured[CL_MAX_BINDINGS];
            int bi;
            CL_Obj b;
            memset(mutated, 0, (size_t)n_all);
            memset(captured, 0, (size_t)n_all);
            /* GC-protect all_vars[] so compaction inside scan_body_for_boxing
             * (via macroexpansion) does not leave stale offsets; the
             * eq-compare in find_var_index would miss and boxing be
             * under-recorded for let* bindings.
             * CL_GC_PROTECT (not bare cl_gc_push_root) fills gc_root_files/lines
             * under DEBUG_GC so gc_dump_roots_dbg shows accurate source locations. */
            for (bi = 0; bi < n_all; bi++)
                CL_GC_PROTECT(all_vars[bi]);
            /* Scan each binding's init-form against only the vars defined
               so far (not including the current binding).  This ensures
               (let* ((x 10) (x (1+ x)))) resolves the init-form reference
               to the first x, not the second (which doesn't exist yet). */
            {
                int scan_count = 0;
                /* GC SAFETY: scan_body_for_boxing macroexpands via cl_vm_apply
                 * (allocates/compacts) — the walk cursor must be protected or
                 * cl_cdr(b) re-reads through a stale offset (the parallel-LET
                 * twin determine_boxed_vars protects its cursors). */
                b = bindings;
                CL_GC_PROTECT(b);
                while (!CL_NULL_P(b)) {
                    CL_Obj binding = cl_car(b);
                    if (CL_CONS_P(binding))
                        scan_body_for_boxing(cl_car(cl_cdr(binding)), all_vars,
                                             scan_count, mutated, captured, 0);
                    scan_count++;
                    b = cl_cdr(b);
                }
                CL_GC_UNPROTECT(1);
            }
            {
                CL_Obj cur = body;
                CL_GC_PROTECT(cur);
                while (CL_CONS_P(cur)) {
                    scan_body_for_boxing(cl_car(cur), all_vars, n_all,
                                         mutated, captured, 0);
                    cur = cl_cdr(cur);
                }
                CL_GC_UNPROTECT(1);
            }
            cl_gc_pop_roots(n_all);
            for (bi = 0; bi < n_all; bi++)
                needs_boxing[bi] = (mutated[bi] && captured[bi]) ? 1 : 0;
        }

        /* Compile bindings, boxing after each store if needed */
        {
            int var_idx = 0;
            while (!CL_NULL_P(bindings)) {
                CL_Obj binding = cl_car(bindings);
                CL_Obj var, val;

                if (CL_CONS_P(binding)) {
                    var = cl_car(binding);
                    val = cl_car(cl_cdr(binding));
                } else {
                    var = binding;
                    val = CL_NIL;
                }

                c->in_tail = 0;
                compile_expr(c, val);

                /* Re-read var from the GC-protected bindings cursor: compile_expr
                 * may trigger compaction, leaving the pre-captured var stale. */
                {
                    CL_Obj fresh_b = cl_car(bindings);
                    var = CL_CONS_P(fresh_b) ? cl_car(fresh_b) : fresh_b;
                }

                if (var_is_special(var, local_specials)) {
                    int idx = cl_add_constant(c, var);
                    cl_emit(c, OP_DYNBIND);
                    cl_emit_u16(c, (uint16_t)idx);
                    special_count++;
                } else {
                    int slot = cl_env_add_local(env, var);
                    cl_emit(c, OP_STORE);
                    cl_emit(c, (uint8_t)slot);
                    cl_emit(c, OP_POP);
                    if (needs_boxing[var_idx]) {
                        cl_emit(c, OP_LOAD);
                        cl_emit(c, (uint8_t)slot);
                        cl_emit(c, OP_MAKE_CELL);
                        cl_emit(c, OP_STORE);
                        cl_emit(c, (uint8_t)slot);
                        cl_emit(c, OP_POP);
                        env->boxed[slot] = 1;
                    }
                }
                var_idx++;
                bindings = cl_cdr(bindings);
            }
        }
    } else {
        CL_Obj vars[CL_MAX_BINDINGS];
        CL_Obj vals[CL_MAX_BINDINGS];
        int n = 0, i;
        CL_Obj b = bindings;

        while (!CL_NULL_P(b) && n < CL_MAX_BINDINGS) {
            CL_Obj binding = cl_car(b);
            if (CL_CONS_P(binding)) {
                vars[n] = cl_car(binding);
                vals[n] = cl_car(cl_cdr(binding));
            } else {
                vars[n] = binding;
                vals[n] = CL_NIL;
            }
            n++;
            b = cl_cdr(b);
        }

        /* GC-protect vars[] and vals[] before compile_expr loop: each
         * compile_expr call may trigger compaction, relocating symbols and
         * init-form cons cells stored in both arrays. */
        for (i = 0; i < n; i++)
            CL_GC_PROTECT(vars[i]);
        for (i = 0; i < n; i++)
            CL_GC_PROTECT(vals[i]);

        for (i = 0; i < n; i++) {
            c->in_tail = 0;
            compile_expr(c, vals[i]);
        }
        cl_gc_pop_roots(n);  /* vals[] — no longer needed after this point */

        {
            int lexical_slots[CL_MAX_BINDINGS];
            for (i = 0; i < n; i++) {
                if (var_is_special(vars[i], local_specials)) {
                    lexical_slots[i] = -1;
                } else {
                    lexical_slots[i] = cl_env_add_local(env, vars[i]);
                }
            }
            for (i = n - 1; i >= 0; i--) {
                if (lexical_slots[i] >= 0) {
                    cl_emit(c, OP_STORE);
                    cl_emit(c, (uint8_t)lexical_slots[i]);
                    cl_emit(c, OP_POP);
                } else {
                    int idx = cl_add_constant(c, vars[i]);
                    cl_emit(c, OP_DYNBIND);
                    cl_emit_u16(c, (uint16_t)idx);
                    special_count++;
                }
            }

            /* Box lexical vars that are both mutated and captured */
            {
                CL_Obj lex_vars[CL_MAX_BINDINGS];
                int lex_slots[CL_MAX_BINDINGS];
                int n_lex = 0;
                uint8_t needs_boxing[CL_MAX_BINDINGS];

                for (i = 0; i < n; i++) {
                    if (lexical_slots[i] >= 0) {
                        lex_vars[n_lex] = vars[i];
                        lex_slots[n_lex] = lexical_slots[i];
                        n_lex++;
                    }
                }
                if (n_lex > 0) {
                    determine_boxed_vars(body, lex_vars, n_lex, needs_boxing);
                    for (i = 0; i < n_lex; i++) {
                        if (needs_boxing[i]) {
                            cl_emit(c, OP_LOAD);
                            cl_emit(c, (uint8_t)lex_slots[i]);
                            cl_emit(c, OP_MAKE_CELL);
                            cl_emit(c, OP_STORE);
                            cl_emit(c, (uint8_t)lex_slots[i]);
                            cl_emit(c, OP_POP);
                            env->boxed[lex_slots[i]] = 1;
                        }
                    }
                }
            }
        }
        cl_gc_pop_roots(n);  /* vars[] */
    }

    /* If we have dynamic bindings, disable tail calls in the body so that
     * OP_DYNUNBIND executes before the function returns. Without this,
     * a tail call would replace the frame and skip the unwind. */
    c->in_tail = (special_count > 0) ? 0 : saved_tail;
    c->special_depth += special_count;

    /* Compile non-tail body forms here; return the last form for the
     * trampoline to take over.  Mirrors compile_body→compile_progn but
     * splits off the last form. */
    {
        CL_Obj rest = process_body_declarations(c, body);
        CL_Obj tail;
        CL_TailFrame *tf;

        /* GC-protect the body cursor: compile_expr below can trigger a
         * compaction (it allocates), which relocates the body's cons cells.
         * Without protecting &rest, the cursor advance (rest = cl_cdr(rest))
         * would follow a stale offset and tail = cl_car(rest) would read
         * garbage — e.g. a direct special-var read at the let tail compiling
         * to OP_NIL instead of OP_GLOAD (returns NIL at runtime). */
        CL_GC_PROTECT(rest);

        if (CL_NULL_P(rest)) {
            tail = CL_NIL;  /* empty body — caller emits OP_NIL */
        } else {
            while (!CL_NULL_P(cl_cdr(rest))) {
                int prev_tail = c->in_tail;
                c->in_tail = 0;
                compile_expr(c, cl_car(rest));
                c->in_tail = prev_tail;
                cl_emit(c, OP_POP);
                rest = cl_cdr(rest);
            }
            tail = cl_car(rest);
        }

        CL_GC_UNPROTECT(4);  /* rest, bindings, body, local_specials */

        tf = cl_tail_push(c);
        tf->kind = CL_TAIL_LET;
        tf->saved_local_count = saved_local_count;
        tf->special_count = special_count;
        tf->saved_tail = saved_tail;
        tf->saved_notinline_count = saved_notinline_count;
        tf->saved_optimize = saved_optimize;  /* pre-declaration snapshot */
        tf->n_gc_roots = 0;
        return tail;
    }
}

/* Restore the effective optimize settings saved at body entry (scoped
 * (declare (optimize ...)) ends with its body).  Cheap no-op when the
 * body declared nothing — the common case. */
static void restore_optimize_settings(CL_Compiler *c, const CL_OptimizeSettings *saved)
{
    if (memcmp(&c->optimize_settings, saved, sizeof(*saved)) != 0)
        c->optimize_settings = *saved;
}

/* Emit the deferred postlude for a popped tail frame.  Called from
 * compile_expr's drain loop in LIFO order.  Returns a "continuation
 * form" the driver should dispatch next, or CL_NIL if no further work
 * is needed.  Most postludes return CL_NIL; only IF_AFTER_THEN uses
 * the continuation channel (to dispatch the ELSE form after THEN has
 * compiled). */
static CL_Obj emit_tail_postlude(CL_Compiler *c, CL_TailFrame *tf)
{
    switch ((CL_TailKind)tf->kind) {
    case CL_TAIL_LET:
        c->special_depth -= tf->special_count;
        if (tf->special_count > 0) {
            cl_emit(c, OP_DYNUNBIND);
            cl_emit(c, (uint8_t)tf->special_count);
        }
        cl_env_clear_boxed(c->env, tf->saved_local_count);
        c->env->local_count = tf->saved_local_count;
        c->notinline_count = tf->saved_notinline_count;
        restore_optimize_settings(c, &tf->saved_optimize);
        return CL_NIL;
    case CL_TAIL_LOCALLY:
        /* Pure body wrapper, but pop any notinline names it declared. */
        c->notinline_count = tf->saved_notinline_count;
        restore_optimize_settings(c, &tf->saved_optimize);
        return CL_NIL;
    case CL_TAIL_PROGN:
        /* No postlude — pure body wrapper. */
        return CL_NIL;
    case CL_TAIL_SYMBOL_MACROLET:
        c->env->symbol_macro_count = tf->saved_macro_count;
        c->notinline_count = tf->saved_notinline_count;
        restore_optimize_settings(c, &tf->saved_optimize);
        return CL_NIL;
    case CL_TAIL_MACROLET:
        c->env->local_macro_count = tf->saved_macro_count;
        c->notinline_count = tf->saved_notinline_count;
        restore_optimize_settings(c, &tf->saved_optimize);
        return CL_NIL;
    case CL_TAIL_PROGV:
        cl_emit(c, OP_PROGV_UNBIND);
        c->in_tail = tf->saved_tail;
        return CL_NIL;
    case CL_TAIL_IF_AFTER_THEN: {
        /* THEN's bytecode is fully emitted.  We now need to JMP past
         * the ELSE branch, patch the JNIL landing, and either dispatch
         * the ELSE form (if there is one) or emit OP_NIL inline. */
        int jmp_pos = cl_emit_jump(c, OP_JMP);
        int then_mv = c->mv_state;   /* THEN's, for the join below */
        cl_patch_jump(c, tf->block_push_pos);  /* jnil_pos: JNIL → here */
        if (CL_NULL_P(tf->cont_form)) {
            /* No ELSE (or `(if t b nil)` collapsed): emit NIL inline
             * and patch the JMP immediately. */
            cl_emit(c, OP_NIL);
            cl_patch_jump(c, jmp_pos);
            c->mv_state = CL_MV_ONE;    /* the implicit NIL arm ... */
            cl_mv_join(c, then_mv);     /* ... so the join is THEN's */
            return CL_NIL;
        } else {
            /* Push AFTER_ELSE frame to patch jmp_pos once ELSE drains,
             * and return the ELSE form so the driver trampolines into
             * it without growing the C stack. */
            CL_TailFrame *new_tf = cl_tail_push(c);
            new_tf->kind = CL_TAIL_IF_AFTER_ELSE;
            new_tf->block_push_pos = jmp_pos;
            new_tf->saved_tail = tf->saved_tail;
            new_tf->mv_state = then_mv;  /* joined once ELSE has drained */
            c->in_tail = tf->saved_tail;
            return tf->cont_form;
        }
    }
    case CL_TAIL_IF_AFTER_ELSE: {
        /* c->mv_state is ELSE's (it falls through), tf->mv_state is THEN's
         * (it arrives via the JMP at block_push_pos).  When one arm can
         * deliver several values and the other arrives stale, the stale one
         * needs a reset on its own path — see cl_mv_close_join, of which
         * this is the two-arm, single-jump case. */
        int then_mv = tf->mv_state, else_mv = c->mv_state;
        int merged = cl_mv_merge(then_mv, else_mv);
        if (merged == CL_MV_MANY && else_mv == CL_MV_STALE) {
            cl_emit(c, OP_MV_RESET);            /* only ELSE reaches here */
            cl_patch_jump(c, tf->block_push_pos);
        } else if (merged == CL_MV_MANY && then_mv == CL_MV_STALE) {
            int over = cl_emit_jump(c, OP_JMP); /* ELSE hops the stub */
            cl_patch_jump(c, tf->block_push_pos);  /* THEN lands on it */
            cl_emit(c, OP_MV_RESET);
            cl_patch_jump(c, over);
        } else {
            cl_patch_jump(c, tf->block_push_pos);
        }
        c->mv_state = merged;
        return CL_NIL;
    }
    case CL_TAIL_HANDLER_BIND:
        cl_emit(c, OP_HANDLER_POP);
        cl_emit(c, (uint8_t)tf->saved_macro_count); /* count of handlers */
        c->in_tail = tf->saved_tail;  /* restore: body was forced non-tail */
        return CL_NIL;
    case CL_TAIL_MULTIPLE_VALUE_BIND:
        cl_env_clear_boxed(c->env, tf->saved_local_count);
        c->env->local_count = tf->saved_local_count;
        c->in_tail = tf->saved_tail;
        c->notinline_count = tf->saved_notinline_count;
        restore_optimize_settings(c, &tf->saved_optimize);
        return CL_NIL;
    case CL_TAIL_FLET:
    case CL_TAIL_LABELS:
        cl_env_clear_boxed(c->env, tf->saved_local_count);
        c->env->local_count = tf->saved_local_count;
        c->env->local_fun_count = tf->saved_block_count;  /* reused: saved_local_fun_count */
        c->in_tail = tf->saved_tail;
        c->notinline_count = tf->saved_notinline_count;
        restore_optimize_settings(c, &tf->saved_optimize);
        return CL_NIL;
    case CL_TAIL_EVAL_WHEN:
        /* No postlude — pure body wrapper. */
        return CL_NIL;
    case CL_TAIL_PROGN_ITER: {
        /* Non-tail body form just drained; emit OP_POP and continue
         * with the next form in cont_form.
         *
         * GOTCHA: the drain loop in compile_expr uses CL_NIL as the
         * "no continuation" sentinel.  But CL_NIL is also a perfectly
         * valid body form: iter expands BLOCK bodies to
         * `((TAGBODY ...) NIL)`, where the final NIL is the body's
         * tail value.  Returning CL_NIL here would make the drain
         * loop *skip* dispatching it — leaving no value on the stack
         * for the surrounding BLOCK_LOCAL postlude to peek-store,
         * which then captures whatever was below (e.g. a function
         * loaded for the enclosing CALL) and corrupts the caller's
         * stack frame.  Workaround: handle literal-NIL body forms
         * inline by emitting OP_NIL + (OP_POP for non-last) directly
         * here, so the drain never sees CL_NIL as a continuation.  We
         * loop, advancing past consecutive NIL non-last forms. */
        CL_Obj forms = tf->cont_form;
        cl_emit(c, OP_POP);  /* discard the just-drained form's value */

        /* Skip over leading NIL non-last forms inline. */
        while (!CL_NULL_P(cl_cdr(forms)) && CL_NULL_P(cl_car(forms))) {
            cl_emit(c, OP_NIL);
            cl_emit(c, OP_POP);
            forms = cl_cdr(forms);
        }

        if (CL_NULL_P(cl_cdr(forms))) {
            /* Single remaining form (the body tail). */
            CL_Obj last = cl_car(forms);
            c->in_tail = tf->saved_tail;
            if (CL_NULL_P(last)) {
                cl_emit(c, OP_NIL);
                return CL_NIL;
            }
            return last;
        }
        /* More forms remain; first form is non-NIL (we skipped any leading
         * NILs above).  Push a fresh PROGN_ITER frame. */
        {
            CL_TailFrame *new_tf = cl_tail_push(c);
            new_tf->kind = CL_TAIL_PROGN_ITER;
            new_tf->saved_tail = tf->saved_tail;
            new_tf->cont_form = cl_cdr(forms);
            c->in_tail = 0;
            return cl_car(forms);
        }
    }
    case CL_TAIL_BLOCK_LOCAL:
    case CL_TAIL_BLOCK_NLX:
    case CL_TAIL_RETURN_FROM_LOCAL:
    case CL_TAIL_RETURN_FROM_NLX:
    case CL_TAIL_OUTER_RETURN_FROM:
        emit_block_or_return_postlude(c, tf);
        return CL_NIL;
    }
    return CL_NIL;
}

/* --- Setq / Setf --- */

static void compile_setf_place(CL_Compiler *c, CL_Obj place, CL_Obj val_form);

/* (push item var) / (pop var) with VAR an unboxed lexical variable of the
 * current function — not a symbol macro, not captured-and-mutated (boxed),
 * not special, not an upvalue — compiles to OP_PUSH_LOCAL / OP_POP_LOCAL
 * (specs/performance.md 4.4): the item, then one opcode that conses onto
 * (or takes the car off) the slot in place.  Same evaluation order and the
 * same value as the macro's expansion (`(setq var (cons item var))` reads
 * the variable after the item; POP returns the car).  Returns 0 when the
 * form is not that shape — the caller then macroexpands it as before, so a
 * malformed form gets the macro's own diagnostic. */
static int try_compile_push_pop(CL_Compiler *c, CL_Obj form)
{
    CL_Obj head = cl_car(form);
    CL_Obj rest = cl_cdr(form);
    CL_Obj var, item = CL_NIL, expansion;
    int slot;
    int saved_tail = c->in_tail;

    if (!c->env) return 0;
    if (head == SYM_PUSH) {
        if (!CL_CONS_P(rest) || !CL_CONS_P(cl_cdr(rest)) ||
            !CL_NULL_P(cl_cdr(cl_cdr(rest))))
            return 0;
        item = cl_car(rest);
        var = cl_car(cl_cdr(rest));
    } else {
        if (!CL_CONS_P(rest) || !CL_NULL_P(cl_cdr(rest)))
            return 0;
        var = cl_car(rest);
    }
    if (!CL_SYMBOL_P(var) || CL_NULL_P(var)) return 0;
    if (cl_env_lookup_symbol_macro_p(c->env, var, &expansion)) return 0;
    slot = cl_env_lookup(c->env, var);
    if (slot < 0 || c->env->boxed[slot]) return 0;

    if (head == SYM_PUSH) {
        /* compile_expr can compact: re-derive the variable from the
         * protected form afterwards (its slot cannot change, but the
         * symbol offset can). */
        CL_GC_PROTECT(form);
        c->in_tail = 0;
        compile_expr(c, item);
        c->in_tail = saved_tail;
        CL_GC_UNPROTECT(1);
        cl_emit(c, OP_PUSH_LOCAL);
    } else {
        cl_emit(c, OP_POP_LOCAL);
    }
    cl_emit(c, (uint8_t)slot);
    c->mv_state = CL_MV_ONE;
    return 1;
}

static void compile_setq(CL_Compiler *c, CL_Obj form)
{
    CL_Obj rest = cl_cdr(form);

    CL_GC_PROTECT(rest);
    while (!CL_NULL_P(rest)) {
        CL_Obj var = cl_car(rest);
        CL_Obj val = cl_car(cl_cdr(rest));
        int slot;
        int saved_tail = c->in_tail;

        /* Check if var is a symbol macro — rewrite as (setf expansion val).
         * A lexical symbol-macro is shadow-aware (an inner LET wins).  A global
         * symbol-macro is consulted only when var is NOT lexically bound as a
         * variable or upvalue, so a LET binding of the same name shadows it
         * (CLHS 3.1.2.1.1) — serapeum's `local` assigns through such a
         * lexical rebinding of a global symbol-macro's name. */
        if (CL_SYMBOL_P(var)) {
            CL_Obj expansion = CL_NIL;
            int _uvd, _uvi;
            int lexically_bound =
                cl_env_lookup(c->env, var) >= 0 ||
                (c->env && cl_env_lookup_upvalue(c->env, var, &_uvd, &_uvi));
            if (cl_env_lookup_symbol_macro_p(c->env, var, &expansion) ||
                (!lexically_bound &&
                 cl_lookup_global_symbol_macro_p(var, &expansion))) {
                compile_setf_place(c, expansion, val);
                rest = cl_cdr(cl_cdr(rest));
                if (!CL_NULL_P(rest)) {
                    cl_emit(c, OP_POP);
                }
                continue;
            }
        }

        /* Check for constant symbols */
        if (CL_SYMBOL_P(var)) {
            CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(var);
            if (sym->flags & CL_SYM_CONSTANT) {
                cl_error(CL_ERR_GENERAL, "Cannot assign to constant variable: %s",
                         cl_symbol_name(var));
            }
        }

        c->in_tail = 0;
        compile_expr(c, val);
        c->in_tail = saved_tail;

        /* GC SAFETY: compile_expr(val) can compact — re-derive var from the
         * protected cursor, else a stale symbol offset is looked up and (for
         * the global path) emitted into the constant pool. */
        var = cl_car(rest);

        slot = cl_env_lookup(c->env, var);
        if (slot >= 0) {
            if (c->env->boxed[slot]) {
                cl_emit(c, OP_CELL_SET_LOCAL);
                cl_emit(c, (uint8_t)slot);
            } else {
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)slot);
            }
        } else if (c->env) {
            int uv_idx = cl_env_resolve_upvalue(c->env, var);
            if (uv_idx >= 0) {
                if (c->env->upvalues[uv_idx].is_boxed) {
                    cl_emit(c, OP_CELL_SET_UPVAL);
                    cl_emit(c, (uint8_t)uv_idx);
                } else {
                    /* Upvalue is not boxed — cannot mutate across closure boundary.
                     * This happens when the boxing scan missed a mutation (e.g. via macro).
                     * Fall back to global store as safety measure. */
                    int idx = cl_add_constant(c, var);
                    cl_emit(c, OP_GSTORE);
                    cl_emit_u16(c, (uint16_t)idx);
                }
            } else {
                int idx = cl_add_constant(c, var);
                cl_emit(c, OP_GSTORE);
                cl_emit_u16(c, (uint16_t)idx);
            }
        } else {
            int idx = cl_add_constant(c, var);
            cl_emit(c, OP_GSTORE);
            cl_emit_u16(c, (uint16_t)idx);
        }
        /* SETQ returns the primary value of the last assignment (CLHS 5.1.1),
         * but the stores above only peek — the MV buffer still holds the
         * value form's values.  See cl_mv_normalize. */
        c->mv_state = CL_MV_STALE;

        rest = cl_cdr(cl_cdr(rest));
        if (!CL_NULL_P(rest)) {
            cl_emit(c, OP_POP);
        }
    }
    CL_GC_UNPROTECT(1);
}

/* Check if sym is a composite c[ad]+r accessor (caar, cadr, cdar, cddr, etc.)
 * Returns 1 if so, 0 otherwise. */
/* SECOND..TENTH → NTH index mapping for setf */
static int is_nth_accessor(CL_Obj sym)
{
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    CL_String *name = (CL_String *)CL_OBJ_TO_PTR(s->name);
    if (s->package != cl_package_cl) return 0;
    static const char *names[] = {
        "SECOND", "THIRD", "FOURTH", "FIFTH", "SIXTH",
        "SEVENTH", "EIGHTH", "NINTH", "TENTH", NULL
    };
    int i;
    for (i = 0; names[i]; i++) {
        if (name->length == strlen(names[i]) &&
            memcmp(name->data, names[i], name->length) == 0)
            return 1;
    }
    return 0;
}

static int get_nth_index(CL_Obj sym)
{
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    CL_String *name = (CL_String *)CL_OBJ_TO_PTR(s->name);
    static const struct { const char *n; int idx; } map[] = {
        {"SECOND",1}, {"THIRD",2}, {"FOURTH",3}, {"FIFTH",4}, {"SIXTH",5},
        {"SEVENTH",6}, {"EIGHTH",7}, {"NINTH",8}, {"TENTH",9}, {NULL,0}
    };
    int i;
    for (i = 0; map[i].n; i++) {
        if (name->length == strlen(map[i].n) &&
            memcmp(name->data, map[i].n, name->length) == 0)
            return map[i].idx;
    }
    return -1;
}

static int is_composite_cadr(CL_Obj sym)
{
    CL_Symbol *s;
    CL_String *name;
    uint32_t k;
    if (!CL_SYMBOL_P(sym)) return 0;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    name = (CL_String *)CL_OBJ_TO_PTR(s->name);
    if (name->length < 4 || name->length > 6) return 0;
    if (name->data[0] != 'C' || name->data[name->length - 1] != 'R') return 0;
    for (k = 1; k < name->length - 1; k++) {
        if (name->data[k] != 'A' && name->data[k] != 'D') return 0;
    }
    return 1;
}

/* Resolve the setf-function for accessor ACCESSOR (the FOO in (setf FOO)).
 * Returns the symbol that names the (setf FOO) function: either the binding
 * registered via cl_register_setf_function (e.g. by (defun (setf foo) ...) /
 * (defmethod (setf foo) ...)), or — failing that — an optimistically-interned
 * %SETF-<name> symbol in the AMILISP package for late binding.  This is the
 * single source of truth for the (setf place) calling convention; both
 * setf-PLACE compilation and #'(setf foo) function references go through it
 * so they agree on which symbol holds the setter. */
/* Synthesize the hidden AMILISP symbol that stores the (setf ACCESSOR)
 * function.  The name is package-qualified — %SETF-<home-package>::<name> —
 * so that same-named accessors in different packages (e.g. LOCAL-TIME::SEC-OF
 * vs LOCAL-TIME-DURATION::SEC-OF) get DISTINCT storage symbols instead of
 * colliding on a single %SETF-<name> (which silently clobbered one writer).
 * Both the C (defun (setf foo)) path and the Lisp CLOS :accessor path go
 * through this (the latter via the %SETF-STORE-SYMBOL builtin), so they always
 * agree on the same symbol for a given accessor. */
CL_Obj cl_setf_store_symbol(CL_Obj accessor)
{
    extern CL_Obj cl_package_cl;
    CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(accessor);
    CL_String *sname = (CL_String *)CL_OBJ_TO_PTR(sym->name);
    char buf[512];
    int len;
    /* Standard COMMON-LISP accessors keep the canonical unqualified
     * %SETF-<name> form: their setters are hand-written (C builtins like
     * %SETF-MACRO-FUNCTION, defsetf forms, etc.) under that exact name, and
     * accessor names are unique within a package so they cannot collide.
     * Only non-CL (user/library) accessors get package-qualified — that is
     * where same-named accessors in different packages otherwise clobbered
     * each other (e.g. LOCAL-TIME::SEC-OF vs LOCAL-TIME-DURATION::SEC-OF). */
    if (!CL_NULL_P(sym->package) && sym->package != cl_package_cl) {
        CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(sym->package);
        CL_String *pname = (CL_String *)CL_OBJ_TO_PTR(pkg->name);
        len = snprintf(buf, sizeof(buf), "%%SETF-%.*s::%.*s",
                       (int)pname->length, pname->data,
                       (int)sname->length, sname->data);
    } else {
        /* COMMON-LISP accessor, or an uninterned symbol with no package. */
        len = snprintf(buf, sizeof(buf), "%%SETF-%.*s",
                       (int)sname->length, sname->data);
    }
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    return cl_intern_in(buf, (uint32_t)len, cl_package_amilisp);
}

static CL_Obj resolve_setf_fn_symbol(CL_Obj accessor)
{
    CL_Obj setf_fn = CL_NIL;
    CL_Obj sfe;
    cl_tables_rdlock();
    sfe = setf_fn_table;
    while (!CL_NULL_P(sfe)) {
        CL_Obj pair = cl_car(sfe);
        if (cl_car(pair) == accessor) {
            setf_fn = cl_cdr(pair);
            break;
        }
        sfe = cl_cdr(sfe);
    }
    cl_tables_rwunlock();
    if (CL_NULL_P(setf_fn))
        setf_fn = cl_setf_store_symbol(accessor);
    return setf_fn;
}

static void compile_setf_place(CL_Compiler *c, CL_Obj place, CL_Obj val_form)
{
    int saved_tail = c->in_tail;
    c->in_tail = 0;

    if (CL_SYMBOL_P(place)) {
        int slot;
        /* Check if place is a symbol macro — rewrite as (setf expansion val).
         * As in compile_symbol/compile_setq, a global symbol-macro is shadowed
         * by a lexically-bound variable or upvalue of the same name. */
        CL_Obj expansion = CL_NIL;
        int _uvd, _uvi;
        int lexically_bound =
            cl_env_lookup(c->env, place) >= 0 ||
            (c->env && cl_env_lookup_upvalue(c->env, place, &_uvd, &_uvi));
        if (cl_env_lookup_symbol_macro_p(c->env, place, &expansion) ||
            (!lexically_bound &&
             cl_lookup_global_symbol_macro_p(place, &expansion))) {
            compile_setf_place(c, expansion, val_form);
            c->in_tail = saved_tail;
            return;
        }
        /* Check for constant symbols */
        {
            CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(place);
            if (sym->flags & CL_SYM_CONSTANT) {
                cl_error(CL_ERR_GENERAL, "Cannot assign to constant variable: %s",
                         cl_symbol_name(place));
            }
        }
        /* GC SAFETY: compile_expr can compact — protect place so the lookup
         * and (global path) cl_add_constant below see the forwarded symbol,
         * not a stale pre-move offset. */
        CL_GC_PROTECT(place);
        compile_expr(c, val_form);
        CL_GC_UNPROTECT(1);
        slot = cl_env_lookup(c->env, place);
        if (slot >= 0) {
            if (c->env->boxed[slot]) {
                cl_emit(c, OP_CELL_SET_LOCAL);
                cl_emit(c, (uint8_t)slot);
            } else {
                cl_emit(c, OP_STORE);
                cl_emit(c, (uint8_t)slot);
            }
        } else if (c->env) {
            int uv_idx = cl_env_resolve_upvalue(c->env, place);
            if (uv_idx >= 0) {
                if (c->env->upvalues[uv_idx].is_boxed) {
                    cl_emit(c, OP_CELL_SET_UPVAL);
                    cl_emit(c, (uint8_t)uv_idx);
                } else {
                    /* Upvalue not boxed — fall back to global store */
                    int idx = cl_add_constant(c, place);
                    cl_emit(c, OP_GSTORE);
                    cl_emit_u16(c, (uint16_t)idx);
                }
            } else {
                int idx = cl_add_constant(c, place);
                cl_emit(c, OP_GSTORE);
                cl_emit_u16(c, (uint16_t)idx);
            }
        } else {
            int idx = cl_add_constant(c, place);
            cl_emit(c, OP_GSTORE);
            cl_emit_u16(c, (uint16_t)idx);
        }
        /* Every store above PEEKS the new value and leaves it on the stack
         * without touching the MV buffer, so the buffer still describes the
         * value form's values — but SETF of a variable yields exactly one
         * value (CLHS 5.1.1). */
        c->mv_state = CL_MV_STALE;
    } else if (CL_CONS_P(place)) {
        CL_Obj head = cl_car(place);

        /* Per CLHS 5.1.2.9: if a setf expander exists for the head,
         * use it even if head is also a macro.  Only macro-expand if
         * no setf expander is found. */
        if (CL_SYMBOL_P(head)) {
            /* Check define-setf-expander table FIRST (takes precedence over macros) */
            {
                CL_Obj expander_fn = CL_NIL;
                CL_Obj exp_entry;
                cl_tables_rdlock();
                exp_entry = setf_expander_table;
                while (!CL_NULL_P(exp_entry)) {
                    CL_Obj pair = cl_car(exp_entry);
                    if (cl_car(pair) == head) {
                        expander_fn = cl_cdr(pair);
                        break;
                    }
                    exp_entry = cl_cdr(exp_entry);
                }
                cl_tables_rwunlock();
                if (!CL_NULL_P(expander_fn)) {
                    CL_Obj call_args[2];
                    CL_Obj expansion;
                    call_args[0] = place;
                    call_args[1] = val_form;
                    CL_GC_PROTECT(place);
                    CL_GC_PROTECT(val_form);
                    expansion = cl_vm_apply(expander_fn, call_args, 2);
                    CL_GC_UNPROTECT(2);
                    compile_expr(c, expansion);
                    c->in_tail = saved_tail;
                    return;
                }
            }
            /* No setf expander — try local macros */
            {
                CL_Obj local_exp = cl_env_lookup_local_macro(c->env, head);
                if (!CL_NULL_P(local_exp)) {
                    /* Local macrolet expander is a 2-arg (form env) wrapper
                     * (see compile_macrolet); see also bi_call_macro_expander. */
                    CL_Obj call_args[2];
                    CL_Obj lex_env;
                    /* GC SAFETY: protect local_exp, place AND val_form before
                     * cl_build_lex_env / cl_vm_apply allocate/compact — same
                     * stale-closure hazard as the function-position
                     * local-macro path (val_form is recursed on after). */
                    CL_GC_PROTECT(local_exp);
                    CL_GC_PROTECT(place);
                    CL_GC_PROTECT(val_form);
                    lex_env = cl_build_lex_env(c->env);
                    call_args[0] = place;
                    call_args[1] = lex_env;
                    CL_GC_PROTECT(lex_env);
                    {
                        CL_Obj expanded;
                        expanded = cl_vm_apply(local_exp, call_args, 2);
                        CL_GC_UNPROTECT(4);
                        compile_setf_place(c, expanded, val_form);
                        c->in_tail = saved_tail;
                        return;
                    }
                }
            }
            /* No setf expander, no local macro — try global macros.
             * GC SAFETY: cl_build_lex_env allocates/compacts — protect place
             * (used by cl_macroexpand_1_env and, on fall-through, the rest of
             * this function) and val_form across it. */
            if (!CL_NULL_P(cl_get_macro(head))) {
                CL_Obj lex_env;
                CL_Obj expanded;
                CL_GC_PROTECT(place);
                CL_GC_PROTECT(val_form);
                lex_env = cl_build_lex_env(c->env);
                CL_GC_PROTECT(lex_env);
                expanded = cl_macroexpand_1_env(place, lex_env);
                CL_GC_UNPROTECT(3);
                if (expanded != place) {
                    compile_setf_place(c, expanded, val_form);
                    c->in_tail = saved_tail;
                    return;
                }
                /* Fall-through keeps using head — re-derive from the
                 * forwarded place (the expansion attempt may have compacted). */
                head = cl_car(place);
            }
        }

        /* (setf (values p1 p2 ...) val-form) →
         * (multiple-value-bind (t1 t2 ...) val-form (setf p1 t1) (setf p2 t2) ... t1) */
        if (head == cl_intern_in("VALUES", 6, cl_package_cl)) {
            CL_Obj places = cl_cdr(place);
            CL_Obj tmps = CL_NIL;
            CL_Obj setfs = CL_NIL;
            CL_Obj mvb_form, sym_mvb;
            int n = 0;
            CL_Obj p;

            CL_GC_PROTECT(tmps);
            CL_GC_PROTECT(setfs);
            /* GC SAFETY: val_form is consed into mvb_form after many
             * allocations below; the p cursor is re-read across them. */
            CL_GC_PROTECT(val_form);

            /* Build list of gensyms and setf forms (in reverse) */
            p = places;
            CL_GC_PROTECT(p);
            while (CL_CONS_P(p)) {
                char buf[16];
                CL_Obj tmp, name_str, form;
                int len;
                len = snprintf(buf, sizeof(buf), "%%MV%d", n);
                name_str = cl_make_string(buf, (uint32_t)len);
                tmp = cl_make_symbol(name_str);
                /* GC SAFETY: tmp is re-read in the setfs cons below, after
                 * the tmps cons may have compacted — keep it forwarded. */
                CL_GC_PROTECT(tmp);
                tmps = cl_cons(tmp, tmps);
                /* Build (setf place-i tmp-i) */
                form = cl_intern_in("SETF", 4, cl_package_cl);
                form = cl_list3(form, cl_car(p), tmp);
                setfs = cl_cons(form, setfs);
                CL_GC_UNPROTECT(1);
                n++;
                p = cl_cdr(p);
            }
            CL_GC_UNPROTECT(1);  /* p */

            /* Reverse tmps and setfs */
            {
                CL_Obj rev_tmps = CL_NIL, rev_setfs = CL_NIL;
                CL_GC_PROTECT(rev_tmps);
                CL_GC_PROTECT(rev_setfs);
                while (!CL_NULL_P(tmps)) {
                    rev_tmps = cl_cons(cl_car(tmps), rev_tmps);
                    tmps = cl_cdr(tmps);
                }
                while (!CL_NULL_P(setfs)) {
                    rev_setfs = cl_cons(cl_car(setfs), rev_setfs);
                    setfs = cl_cdr(setfs);
                }
                tmps = rev_tmps;
                setfs = rev_setfs;
                CL_GC_UNPROTECT(2);
            }

            /* Build: (multiple-value-bind tmps val-form (setf p1 t1) ... (setf pn tn) t1) */
            /* Append first tmp to end of setfs for return value */
            {
                CL_Obj body = cl_cons(cl_car(tmps), CL_NIL); /* (t1) */
                CL_Obj s = setfs;
                /* Reverse setfs to prepend to body */
                CL_Obj rev_s = CL_NIL;
                CL_GC_PROTECT(body);
                CL_GC_PROTECT(rev_s);
                while (!CL_NULL_P(s)) {
                    rev_s = cl_cons(cl_car(s), rev_s);
                    s = cl_cdr(s);
                }
                while (!CL_NULL_P(rev_s)) {
                    body = cl_cons(cl_car(rev_s), body);
                    rev_s = cl_cdr(rev_s);
                }
                /* GC SAFETY: build the chain one cl_cons at a time — a nested
                 * cl_cons(x, cl_cons(...)) may read x before the inner cons
                 * compacts; sym_mvb is interned only after the conses that
                 * precede its use. */
                mvb_form = cl_cons(val_form, body);
                mvb_form = cl_cons(tmps, mvb_form);
                sym_mvb = cl_intern_in("MULTIPLE-VALUE-BIND", 19, cl_package_cl);
                mvb_form = cl_cons(sym_mvb, mvb_form);
                CL_GC_UNPROTECT(2);
            }

            CL_GC_UNPROTECT(3);  /* tmps, setfs, val_form */
            compile_expr(c, mvb_form);
            c->in_tail = saved_tail;
            return;
        }

        /* (setf (progn form... place) val) → (progn form... (setf place val))
         * Per CLHS 5.1.2.3, PROGN is transparent to setf. */
        if (head == SYM_PROGN) {
            CL_Obj forms = cl_cdr(place);
            CL_Obj inner_place;
            /* GC-protect cursor and val_form — compile_expr can compact, making them stale */
            CL_GC_PROTECT(forms);
            CL_GC_PROTECT(val_form);
            /* Walk to last form (the actual place), compile leading forms */
            while (CL_CONS_P(forms) && CL_CONS_P(cl_cdr(forms))) {
                compile_expr(c, cl_car(forms));
                cl_emit(c, OP_POP);
                forms = cl_cdr(forms);
            }
            /* Last element is the place */
            inner_place = CL_CONS_P(forms) ? cl_car(forms) : CL_NIL;
            CL_GC_UNPROTECT(2);  /* forms, val_form */
            compile_setf_place(c, inner_place, val_form);
            c->in_tail = saved_tail;
            return;
        }

        /* (setf (if test then-place else-place) val)
         * → (if test (setf then-place val) (setf else-place val))
         * Extension supported by SBCL/CCL; required by FSet. */
        if (head == SYM_IF) {
            CL_Obj test_form = cl_car(cl_cdr(place));
            CL_Obj then_place = cl_car(cl_cdr(cl_cdr(place)));
            CL_Obj else_place = cl_car(cl_cdr(cl_cdr(cl_cdr(place))));
            CL_Obj setf_then = CL_NIL, setf_else = CL_NIL, new_if;

            CL_GC_PROTECT(test_form);
            CL_GC_PROTECT(then_place);
            CL_GC_PROTECT(else_place);
            CL_GC_PROTECT(val_form);
            CL_GC_PROTECT(setf_then);
            CL_GC_PROTECT(setf_else);

            setf_then = cl_list3(SYM_SETF, then_place, val_form);
            if (CL_NULL_P(else_place)) {
                new_if = cl_list3(SYM_IF, test_form, setf_then);
            } else {
                setf_else = cl_list3(SYM_SETF, else_place, val_form);
                new_if = cl_list4(SYM_IF, test_form, setf_then, setf_else);
            }

            CL_GC_UNPROTECT(6);
            compile_expr(c, new_if);
            c->in_tail = saved_tail;
            return;
        }

        /* (setf (let/let* bindings body... place) val)
         * → (let/let* bindings body... (setf place val))
         * Extension supported by SBCL/CCL; required by FSet. */
        if (head == SYM_LET || head == SYM_LETSTAR) {
            CL_Obj bindings = cl_car(cl_cdr(place));
            CL_Obj body = cl_cdr(cl_cdr(place));
            CL_Obj inner_place, new_body, setf_form, new_let;
            CL_Obj rev = CL_NIL;
            /* `head` is not forwarded: decide now, re-read the symbol later */
            int is_star = (head == SYM_LETSTAR);

            CL_GC_PROTECT(bindings);
            CL_GC_PROTECT(body);
            CL_GC_PROTECT(val_form);
            CL_GC_PROTECT(rev);

            /* Find last body form (the place), collect preceding in reverse */
            while (CL_CONS_P(body) && CL_CONS_P(cl_cdr(body))) {
                rev = cl_cons(cl_car(body), rev);
                body = cl_cdr(body);
            }
            inner_place = CL_CONS_P(body) ? cl_car(body) : CL_NIL;

            /* Build (setf inner-place val) */
            setf_form = cl_list3(SYM_SETF, inner_place, val_form);

            /* Build new body: preceding-forms... (setf place val) */
            new_body = cl_cons(setf_form, CL_NIL);
            CL_GC_PROTECT(new_body);
            while (!CL_NULL_P(rev)) {
                new_body = cl_cons(cl_car(rev), new_body);
                rev = cl_cdr(rev);
            }

            /* Build (let/let* bindings new-body...) */
            new_let = cl_list_star3(is_star ? SYM_LETSTAR : SYM_LET,
                                    bindings, new_body);

            CL_GC_UNPROTECT(5);
            compile_expr(c, new_let);
            c->in_tail = saved_tail;
            return;
        }

        /* (setf (the type place) val) → (setf place (the type val))
         * Per CL spec, THE is transparent to setf. */
        if (head == SYM_THE) {
            CL_Obj type_spec = cl_car(cl_cdr(place));
            CL_Obj inner_place = cl_car(cl_cdr(cl_cdr(place)));
            CL_Obj typed_val;
            CL_GC_PROTECT(type_spec);
            CL_GC_PROTECT(inner_place);
            CL_GC_PROTECT(val_form);
            /* Build (the type val_form) */
            typed_val = cl_list3(SYM_THE, type_spec, val_form);
            CL_GC_UNPROTECT(3);
            compile_setf_place(c, inner_place, typed_val);
            c->in_tail = saved_tail;
            return;
        }

        /* GC-protect val_form and place — place sub-expressions compiled first can
         * compact, making both stale before subsequent accesses. */
        CL_GC_PROTECT(val_form);
        CL_GC_PROTECT(place);

        if (head == SETF_SYM_CAR || head == SETF_SYM_FIRST) {
            compile_expr(c, cl_car(cl_cdr(place)));
            compile_expr(c, val_form);
            cl_emit(c, OP_RPLACA);
        } else if (head == SETF_SYM_CDR || head == SETF_SYM_REST) {
            compile_expr(c, cl_car(cl_cdr(place)));
            compile_expr(c, val_form);
            cl_emit(c, OP_RPLACD);
        } else if (CL_SYMBOL_P(head) && is_nth_accessor(head)) {
            /* SECOND..TENTH: rewrite as (setf (nth N list) val) */
            int nth_idx = get_nth_index(head);
            CL_Obj arg = cl_car(cl_cdr(place));
            CL_Obj nth_place;
            CL_GC_PROTECT(arg);
            nth_place = cl_list3(SETF_SYM_NTH, CL_MAKE_FIXNUM(nth_idx), arg);
            CL_GC_UNPROTECT(1);  /* arg */
            CL_GC_UNPROTECT(2);  /* place, val_form */
            compile_setf_place(c, nth_place, val_form);
            c->in_tail = saved_tail;
            return;
        } else if (is_composite_cadr(head)) {
            /* Composite c[ad]+r: decompose (setf (cXY..Zr arg) val)
             * into (setf (cXr (cY..Zr arg)) val) per CL spec */
            CL_Symbol *hsym = (CL_Symbol *)CL_OBJ_TO_PTR(head);
            CL_String *hname = (CL_String *)CL_OBJ_TO_PTR(hsym->name);
            CL_Obj outer_sym = (hname->data[1] == 'A') ? SETF_SYM_CAR : SETF_SYM_CDR;
            CL_Obj arg = cl_car(cl_cdr(place));
            char ibuf[32];
            uint32_t ilen = hname->length - 1, ki;
            CL_Obj isym, iplace, nplace;
            if (ilen >= sizeof(ibuf)) ilen = sizeof(ibuf) - 1;
            ibuf[0] = 'C';
            for (ki = 0; ki < ilen - 2; ki++)
                ibuf[ki + 1] = hname->data[ki + 2];
            ibuf[ilen - 1] = 'R';
            ibuf[ilen] = '\0';
            /* Rooted before the intern, which may already move them. */
            CL_GC_PROTECT(outer_sym);
            CL_GC_PROTECT(arg);
            isym = cl_intern_in(ibuf, ilen, cl_package_cl);
            iplace = cl_list2(isym, arg);
            nplace = cl_list2(outer_sym, iplace);
            CL_GC_UNPROTECT(2);  /* outer_sym, arg */
            CL_GC_UNPROTECT(2);  /* place, val_form */
            compile_setf_place(c, nplace, val_form);
            c->in_tail = saved_tail;
            return;
        } else if (head == SETF_SYM_AREF || head == SETF_SYM_SVREF ||
                   head == SETF_SYM_CHAR || head == SETF_SYM_SCHAR) {
            /* Count indices: place = (aref arr idx1 idx2 ...) */
            CL_Obj indices = cl_cdr(cl_cdr(place));
            int nindices = 0;
            CL_Obj tmp = indices;
            while (!CL_NULL_P(tmp)) { nindices++; tmp = cl_cdr(tmp); }

            if (nindices <= 1 || head == SETF_SYM_SVREF) {
                /* 1D fast path: use OP_ASET.  Re-derive index from GC-protected
                 * place — compile_expr(array) can compact, making indices stale. */
                compile_expr(c, cl_car(cl_cdr(place)));
                compile_expr(c, cl_car(cl_cdr(cl_cdr(place))));
                compile_expr(c, val_form);
                cl_emit(c, OP_ASET);
            } else {
                /* Multi-dim: call %SETF-AREF(array, val, idx1, idx2, ...) */
                int ci = cl_add_constant(c, SETF_HELPER_AREF);
                cl_emit(c, OP_FLOAD);
                cl_emit_u16(c, (uint16_t)ci);
                compile_expr(c, cl_car(cl_cdr(place)));  /* array */
                compile_expr(c, val_form);               /* value */
                /* Re-derive from GC-protected place — indices is stale after the
                 * two compile_expr calls above may have compacted. */
                tmp = cl_cdr(cl_cdr(place));
                CL_GC_PROTECT(tmp);
                while (!CL_NULL_P(tmp)) {
                    compile_expr(c, cl_car(tmp));
                    tmp = cl_cdr(tmp);
                }
                CL_GC_UNPROTECT(1);
                cl_emit_call(c, OP_CALL, (uint8_t)(2 + nindices));
            }
        } else if (head == SETF_SYM_NTH) {
            int idx = cl_add_constant(c, SETF_HELPER_NTH);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));
            compile_expr(c, cl_car(cl_cdr(cl_cdr(place))));
            compile_expr(c, val_form);
            cl_emit_call(c, OP_CALL, 3);
        } else if (head == SETF_SYM_SYMBOL_VALUE) {
            int idx = cl_add_constant(c, SETF_HELPER_SV);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));
            compile_expr(c, val_form);
            cl_emit_call(c, OP_CALL, 2);
        } else if (head == SETF_SYM_SYMBOL_FUNCTION || head == SETF_SYM_FDEFINITION) {
            int idx = cl_add_constant(c, SETF_HELPER_SF);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));
            compile_expr(c, val_form);
            cl_emit_call(c, OP_CALL, 2);
        } else if (head == SETF_SYM_SYMBOL_PLIST) {
            int idx = cl_add_constant(c, SETF_HELPER_SP);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));
            compile_expr(c, val_form);
            cl_emit_call(c, OP_CALL, 2);
        } else if (head == SETF_SYM_ROW_MAJOR_AREF) {
            /* (setf (row-major-aref arr idx) val) → (%setf-row-major-aref arr idx val) */
            int idx = cl_add_constant(c, SETF_HELPER_ROW_MAJOR_AREF);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));       /* array */
            compile_expr(c, cl_car(cl_cdr(cl_cdr(place)))); /* index */
            compile_expr(c, val_form);                     /* value */
            cl_emit_call(c, OP_CALL, 3);
        } else if (head == SETF_SYM_FILL_POINTER) {
            /* (setf (fill-pointer vec) val) → (%setf-fill-pointer vec val) */
            int idx = cl_add_constant(c, SETF_HELPER_FILL_POINTER);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));       /* vector */
            compile_expr(c, val_form);                     /* new fill-pointer */
            cl_emit_call(c, OP_CALL, 2);
        } else if (head == SETF_SYM_GETHASH) {
            /* (setf (gethash key ht) val) → (%setf-gethash key ht val) */
            int idx = cl_add_constant(c, SETF_HELPER_GETHASH);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));       /* key */
            compile_expr(c, cl_car(cl_cdr(cl_cdr(place)))); /* hash-table */
            compile_expr(c, val_form);                     /* value */
            cl_emit_call(c, OP_CALL, 3);
        } else if (head == SETF_SYM_BIT) {
            /* (setf (bit bv idx) val) → (%setf-bit bv idx val) */
            int idx = cl_add_constant(c, SETF_HELPER_BIT);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));       /* bit-vector */
            compile_expr(c, cl_car(cl_cdr(cl_cdr(place)))); /* index */
            compile_expr(c, val_form);                     /* value */
            cl_emit_call(c, OP_CALL, 3);
        } else if (head == SETF_SYM_SBIT) {
            /* (setf (sbit bv idx) val) → (%setf-sbit bv idx val) */
            int idx = cl_add_constant(c, SETF_HELPER_SBIT);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, cl_car(cl_cdr(place)));       /* bit-vector */
            compile_expr(c, cl_car(cl_cdr(cl_cdr(place)))); /* index */
            compile_expr(c, val_form);                     /* value */
            cl_emit_call(c, OP_CALL, 3);
        } else if (head == SETF_SYM_GET) {
            /* (setf (get sym indicator [default]) val) → (%setf-get sym indicator val).
             * Per CLHS §5.1.1.1.1, every subform of the place — including
             * DEFAULT — must be evaluated once, left-to-right, before VAL,
             * even though %setf-get itself doesn't use DEFAULT.  Emit code
             * for DEFAULT into the value stack, then OP_POP it. */
            int idx = cl_add_constant(c, SETF_HELPER_GET);
            CL_Obj sym_form  = cl_car(cl_cdr(place));
            CL_Obj ind_form  = cl_car(cl_cdr(cl_cdr(place)));
            CL_Obj rest_after_ind = cl_cdr(cl_cdr(cl_cdr(place)));
            int has_default = !CL_NULL_P(rest_after_ind);
            /* GC-protect ind_form and rest_after_ind — compile_expr(sym_form) can compact */
            CL_GC_PROTECT(ind_form);
            CL_GC_PROTECT(rest_after_ind);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
            compile_expr(c, sym_form);                     /* symbol */
            compile_expr(c, ind_form);                     /* indicator */
            if (has_default) {
                /* Evaluate DEFAULT for side effects, then discard. */
                compile_expr(c, cl_car(rest_after_ind));
                cl_emit(c, OP_POP);
            }
            CL_GC_UNPROTECT(2);                           /* ind_form, rest_after_ind */
            compile_expr(c, val_form);                     /* value */
            cl_emit_call(c, OP_CALL, 3);
        } else if (head == SETF_SYM_GETF) {
            /* (setf (getf PLACE IND [DEFAULT]) VAL).
             *
             * CLHS §5.1.2.4: if IND is missing from PLACE's plist, a new
             * (IND VAL) pair must be PREPENDED to PLACE — so PLACE itself
             * must be reassigned, not just mutated.  CLHS §5.1.1.1.1
             * additionally requires every subform of PLACE — that is, the
             * PLACE form, IND, and DEFAULT — to be evaluated once,
             * left-to-right, before VAL.  Per CLHS, DEFAULT may be
             * evaluated when used in a setf context even though its value
             * is ignored; the ansi-test suite (setf-getf.5) relies on this.
             *
             * We rewrite to
             *   (let* (T-subforms-of-PLACE...
             *          (#:i IND)
             *          (#:d DEFAULT)   ; only if DEFAULT supplied
             *          (#:v VAL))
             *     (setf PLACE-using-T-temps
             *           (%setf-getf PLACE-using-T-temps #:i #:v))
             *     #:v)
             * and recompile, letting nested SETF machinery handle whatever
             * kind of inner place we end up with.
             *
             * For a symbol PLACE there are no subform temps; for a CONS
             * place (e.g. (CAR X)) we bind each argument of the place to a
             * gensym and reuse them in both the SETF target and the helper
             * call so the place is read only once. */
            CL_Obj place_arg  = cl_car(cl_cdr(place));          /* PLACE */
            CL_Obj ind_arg    = cl_car(cl_cdr(cl_cdr(place)));  /* IND   */
            CL_Obj rest3      = cl_cdr(cl_cdr(cl_cdr(place)));  /* (DEFAULT) or () */
            int has_default   = !CL_NULL_P(rest3);
            CL_Obj default_arg = has_default ? cl_car(rest3) : CL_NIL;
            CL_Obj gensym_i, gensym_d, gensym_v;
            CL_Obj inner_place;            /* PLACE form rewritten with temps */
            CL_Obj sub_bindings = CL_NIL;  /* temps from PLACE's subforms (if cons) */
            CL_Obj helper_call, setf_place_form, let_bindings, let_form;

            /* GC SAFETY: protect the place subforms BEFORE the gensym
             * allocations (each can compact — protecting an already-stale
             * value would forward garbage), and each gensym as it is created
             * (the next gensym allocation would stale it). */
            CL_GC_PROTECT(place_arg);
            CL_GC_PROTECT(ind_arg);
            CL_GC_PROTECT(default_arg);
            gensym_i = cl_gensym_with_name("I");
            CL_GC_PROTECT(gensym_i);
            gensym_d = has_default ? cl_gensym_with_name("D") : CL_NIL;
            CL_GC_PROTECT(gensym_d);
            gensym_v = cl_gensym_with_name("V");
            CL_GC_PROTECT(gensym_v);
            CL_GC_PROTECT(sub_bindings);

            /* If PLACE is a CONS, decompose its subforms into temp bindings
             * and rebuild PLACE using those temps so it reads only once. */
            if (CL_CONS_P(place_arg)) {
                CL_Obj op = cl_car(place_arg);
                CL_Obj rest = cl_cdr(place_arg);
                CL_Obj temps_head = CL_NIL, temps_tail = CL_NIL;
                CL_Obj bind_head = CL_NIL, bind_tail = CL_NIL;
                /* op and rest are cursors into place_arg; the cl_cons/gensym
                 * calls below can compact and relocate those cells, so protect
                 * them (same class as the compile_let body-cursor bug). */
                CL_GC_PROTECT(op);
                CL_GC_PROTECT(rest);
                CL_GC_PROTECT(temps_head);
                CL_GC_PROTECT(temps_tail);
                CL_GC_PROTECT(bind_head);
                CL_GC_PROTECT(bind_tail);
                while (!CL_NULL_P(rest)) {
                    CL_Obj gs = cl_gensym_with_name("T");
                    CL_Obj sub, cell, binding;
                    /* GC SAFETY: gs is re-read across the conses below —
                     * keep it forwarded.  sub is read from the protected
                     * cursor only after the gensym allocation. */
                    CL_GC_PROTECT(gs);
                    sub = cl_car(rest);
                    /* binding: (gs sub) */
                    binding = cl_cons(sub, CL_NIL);
                    binding = cl_cons(gs, binding);
                    cell = cl_cons(binding, CL_NIL);
                    if (CL_NULL_P(bind_head)) bind_head = cell;
                    else ((CL_Cons *)CL_OBJ_TO_PTR(bind_tail))->cdr = cell;
                    bind_tail = cell;
                    /* temps list */
                    cell = cl_cons(gs, CL_NIL);
                    if (CL_NULL_P(temps_head)) temps_head = cell;
                    else ((CL_Cons *)CL_OBJ_TO_PTR(temps_tail))->cdr = cell;
                    temps_tail = cell;
                    CL_GC_UNPROTECT(1);  /* gs */
                    rest = cl_cdr(rest);
                }
                inner_place = cl_cons(op, temps_head);
                sub_bindings = bind_head;
                CL_GC_UNPROTECT(6);  /* op, rest, temps_head/tail, bind_head/tail */
            } else {
                inner_place = place_arg;
            }
            CL_GC_PROTECT(inner_place);

            /* (%setf-getf inner_place #:i #:v) */
            helper_call = cl_cons(gensym_v, CL_NIL);
            CL_GC_PROTECT(helper_call);
            helper_call = cl_cons(gensym_i, helper_call);
            helper_call = cl_cons(inner_place, helper_call);
            helper_call = cl_cons(SETF_HELPER_GETF, helper_call);

            /* (setf inner_place helper_call) */
            setf_place_form = cl_cons(helper_call, CL_NIL);
            CL_GC_PROTECT(setf_place_form);
            setf_place_form = cl_cons(inner_place, setf_place_form);
            setf_place_form = cl_cons(SYM_SETF, setf_place_form);

            /* Build let* bindings:
             *   sub_bindings ++ ((#:i IND) [(#:d DEFAULT)] (#:v VAL))
             */
            {
                CL_Obj b_v, b_i, b_d, head_b, tail_b;
                b_v = cl_cons(val_form, CL_NIL);
                b_v = cl_cons(gensym_v, b_v);
                b_v = cl_cons(b_v, CL_NIL);
                head_b = b_v; tail_b = b_v;
                if (has_default) {
                    b_d = cl_cons(default_arg, CL_NIL);
                    b_d = cl_cons(gensym_d, b_d);
                    b_d = cl_cons(b_d, head_b);
                    head_b = b_d;
                }
                b_i = cl_cons(ind_arg, CL_NIL);
                b_i = cl_cons(gensym_i, b_i);
                b_i = cl_cons(b_i, head_b);
                head_b = b_i;
                /* prepend sub_bindings */
                if (!CL_NULL_P(sub_bindings)) {
                    /* find tail of sub_bindings, link to head_b */
                    CL_Obj walk = sub_bindings, prev = CL_NIL;
                    while (!CL_NULL_P(walk)) { prev = walk; walk = cl_cdr(walk); }
                    ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = head_b;
                    let_bindings = sub_bindings;
                } else {
                    let_bindings = head_b;
                }
                CL_UNUSED(tail_b);
            }
            CL_GC_PROTECT(let_bindings);

            /* (let* let_bindings (setf ...) #:v) */
            let_form = cl_cons(gensym_v, CL_NIL);
            CL_GC_PROTECT(let_form);
            let_form = cl_cons(setf_place_form, let_form);
            let_form = cl_cons(let_bindings, let_form);
            let_form = cl_cons(SYM_LETSTAR, let_form);

            compile_expr(c, let_form);
            CL_GC_UNPROTECT(12); /* place_arg ind_arg default_arg gensym_i gensym_d gensym_v sub_bindings inner_place helper_call setf_place_form let_bindings let_form */
        } else {
            /* Check defsetf table (hash-indexed; an updater is always a
             * symbol, so NIL means "no defsetf for HEAD") */
            CL_Obj updater = cl_get_setf_updater(head);
            int found = !CL_NULL_P(updater);
            if (found) {
                /* Build (UPDATER place-args... VAL) and compile it as an
                 * ordinary call instead of hand-emitting OP_FLOAD/OP_CALL:
                 * compile_call then consults a compiler macro on the
                 * updater (e.g. %SET-SLOT-VALUE's fast-path inline in
                 * clos.lisp) and can emit dedicated builtin opcodes.
                 * Evaluation order is unchanged — place subforms
                 * left-to-right, then the value form (CLHS 5.1.1.1.1).
                 *
                 * GC SAFETY: cl_cons can compact — every CL_Obj local
                 * that lives across an allocation is protected, including
                 * the list cursor. */
                CL_Obj rev = CL_NIL, call_form = CL_NIL, arg_cursor;
                CL_GC_PROTECT(updater);
                CL_GC_PROTECT(place);
                CL_GC_PROTECT(val_form);
                CL_GC_PROTECT(rev);
                CL_GC_PROTECT(call_form);
                arg_cursor = cl_cdr(place);
                CL_GC_PROTECT(arg_cursor);
                while (!CL_NULL_P(arg_cursor)) {
                    rev = cl_cons(cl_car(arg_cursor), rev);
                    arg_cursor = cl_cdr(arg_cursor);
                }
                rev = cl_cons(val_form, rev);
                while (!CL_NULL_P(rev)) {
                    call_form = cl_cons(cl_car(rev), call_form);
                    rev = cl_cdr(rev);
                }
                call_form = cl_cons(updater, call_form);
                compile_expr(c, call_form);
                CL_GC_UNPROTECT(6);  /* updater place val_form rev call_form arg_cursor */
            } else {
                /* Check define-setf-expander table.
                 * Expander fn takes (place-form value-form) and returns
                 * a single expansion form to compile. */
                {
                    CL_Obj expander_fn = CL_NIL;
                    CL_Obj exp_entry;
                    cl_tables_rdlock();
                    exp_entry = setf_expander_table;
                    while (!CL_NULL_P(exp_entry)) {
                        CL_Obj pair = cl_car(exp_entry);
                        if (cl_car(pair) == head) {
                            expander_fn = cl_cdr(pair);
                            break;
                        }
                        exp_entry = cl_cdr(exp_entry);
                    }
                    cl_tables_rwunlock();
                    if (!CL_NULL_P(expander_fn)) {
                        CL_Obj call_args[2];
                        CL_Obj expansion;
                        call_args[0] = place;
                        call_args[1] = val_form;
                        CL_GC_PROTECT(place);
                        CL_GC_PROTECT(val_form);
                        expansion = cl_vm_apply(expander_fn, call_args, 2);
                        CL_GC_UNPROTECT(2);
                        compile_expr(c, expansion);
                        CL_GC_UNPROTECT(2);               /* outer place, val_form (from ~2418) */
                        return;
                    }
                }
                /* Setf function (late-bound): resolve the %SETF-<name> /
                 * registered setter symbol and emit FLOAD — resolved at
                 * runtime like normal function calls.  Handles both
                 * (defun (setf name) ...) and (defgeneric (setf name) ...) /
                 * (defmethod (setf name) ...).  See resolve_setf_fn_symbol. */
                CL_Obj setf_fn = resolve_setf_fn_symbol(head);
                /* Late-bound setf: (setf (foo a b) val) calls the (setf foo)
                 * function value-first: (%setf-foo val a b).  CLHS 5.1.1.1.1
                 * requires the PLACE subforms (a b) to be evaluated
                 * left-to-right BEFORE the value form.  That order is only
                 * OBSERVABLE when (a) a place subform has a side effect —
                 * compound non-quoted form — OR (b) val_form is compound and
                 * could modify a symbol that appears as an atom place subform.
                 * Take the fast path only when BOTH place subforms AND val_form
                 * are atoms or quoted constants (e.g. `(setf (acc obj) v)` where
                 * obj and v are both simple), avoiding a let*-per-setf-site
                 * compile cost that is heavy in large CLOS systems. */
                {
                    int needs_order = 0;
                    CL_Obj w = cl_cdr(place);
                    while (!CL_NULL_P(w)) {
                        CL_Obj a = cl_car(w);
                        if (CL_CONS_P(a) && cl_car(a) != SYM_QUOTE) {
                            needs_order = 1;
                            break;
                        }
                        w = cl_cdr(w);
                    }
                    /* val_form compound: may have side effects visible through
                     * atom place subforms — require ordered evaluation. */
                    if (!needs_order &&
                        CL_CONS_P(val_form) && cl_car(val_form) != SYM_QUOTE) {
                        needs_order = 1;
                    }
                    if (!needs_order) {
                        /* Fast path: value first (place subforms side-effect-free). */
                        CL_Obj args = cl_cdr(place);
                        int nargs = 0;
                        int idx = cl_add_constant(c, setf_fn);
                        cl_emit(c, OP_FLOAD);
                        cl_emit_u16(c, (uint16_t)idx);
                        /* GC-protect args cursor — compile_expr can compact it. */
                        CL_GC_PROTECT(args);
                        compile_expr(c, val_form);  /* new-value FIRST */
                        nargs++;
                        while (!CL_NULL_P(args)) {
                            compile_expr(c, cl_car(args));
                            nargs++;
                            args = cl_cdr(args);
                        }
                        CL_GC_UNPROTECT(1);          /* args */
                        cl_emit_call(c, OP_CALL, (uint8_t)nargs);
                        CL_GC_UNPROTECT(2);           /* place, val_form */
                        c->in_tail = saved_tail;
                        return;
                    }
                }
                {
                    /* Ordered path: a place subform or val_form may have side
                     * effects (ansi subseq.order.3/.4).  Rewrite via let* temps
                     * to force place-then-value order, returning the stored value:
                     *   (let* ((#:t1 a) (#:t2 b) (#:v val))
                     *     (%setf-foo #:v #:t1 #:t2) #:v) */
                    CL_Obj args = cl_cdr(place);
                    CL_Obj gv;
                    CL_Obj bind_head = CL_NIL, bind_tail = CL_NIL;
                    CL_Obj temp_head = CL_NIL, temp_tail = CL_NIL;
                    CL_Obj vbind, call_form, let_form;
                    /* setf_fn is an interned symbol that must survive the many
                     * allocations below — protect it (and every cursor). */
                    CL_GC_PROTECT(setf_fn);
                    CL_GC_PROTECT(args);
                    gv = cl_gensym_with_name("V");
                    CL_GC_PROTECT(gv);
                    CL_GC_PROTECT(bind_head);
                    CL_GC_PROTECT(bind_tail);
                    CL_GC_PROTECT(temp_head);
                    CL_GC_PROTECT(temp_tail);
                    while (!CL_NULL_P(args)) {
                        CL_Obj a = cl_car(args);
                        CL_Obj gt, binding, cell;
                        /* a and gt are held across allocating cl_cons/gensym
                         * calls — protect them (compacting GC). */
                        CL_GC_PROTECT(a);
                        gt = cl_gensym_with_name("T");
                        CL_GC_PROTECT(gt);
                        binding = cl_cons(a, CL_NIL);
                        binding = cl_cons(gt, binding);   /* (#:t a) */
                        cell = cl_cons(binding, CL_NIL);
                        if (CL_NULL_P(bind_head)) bind_head = cell;
                        else ((CL_Cons *)CL_OBJ_TO_PTR(bind_tail))->cdr = cell;
                        bind_tail = cell;
                        cell = cl_cons(gt, CL_NIL);       /* temp ref */
                        if (CL_NULL_P(temp_head)) temp_head = cell;
                        else ((CL_Cons *)CL_OBJ_TO_PTR(temp_tail))->cdr = cell;
                        temp_tail = cell;
                        CL_GC_UNPROTECT(2);               /* gt, a */
                        args = cl_cdr(args);
                    }
                    /* append (#:v val) binding last */
                    vbind = cl_cons(val_form, CL_NIL);
                    vbind = cl_cons(gv, vbind);
                    vbind = cl_cons(vbind, CL_NIL);
                    if (CL_NULL_P(bind_head)) bind_head = vbind;
                    else ((CL_Cons *)CL_OBJ_TO_PTR(bind_tail))->cdr = vbind;
                    /* (setf_fn #:v #:t1 #:t2 ...) */
                    call_form = cl_cons(gv, temp_head);
                    call_form = cl_cons(setf_fn, call_form);
                    CL_GC_PROTECT(call_form);
                    /* (let* bindings call_form #:v) */
                    let_form = cl_cons(gv, CL_NIL);
                    let_form = cl_cons(call_form, let_form);
                    let_form = cl_cons(bind_head, let_form);
                    let_form = cl_cons(SYM_LETSTAR, let_form);
                    CL_GC_PROTECT(let_form);
                    compile_expr(c, let_form);
                    CL_GC_UNPROTECT(9);  /* setf_fn args gv bind_head bind_tail temp_head temp_tail call_form let_form */
                }
            }
        }
        CL_GC_UNPROTECT(2);                               /* place, val_form (protected ~line 2418) */
    } else {
        if (CL_CONS_P(place) && CL_SYMBOL_P(cl_car(place)))
            cl_error(CL_ERR_GENERAL, "SETF: invalid place (%s ...)",
                     cl_symbol_name(cl_car(place)));
        else {
            char _pb[160];
            cl_prin1_to_string(place, _pb, sizeof(_pb));
            cl_error(CL_ERR_GENERAL, "SETF: invalid place: %s", _pb);
        }
    }

    c->in_tail = saved_tail;
}

static void compile_setf(CL_Compiler *c, CL_Obj form)
{
    CL_Obj rest = cl_cdr(form);

    CL_GC_PROTECT(rest);
    while (!CL_NULL_P(rest)) {
        CL_Obj place = cl_car(rest);
        CL_Obj val_form;

        if (CL_NULL_P(cl_cdr(rest)))
            cl_error(CL_ERR_ARGS, "SETF: odd number of arguments");
        val_form = cl_car(cl_cdr(rest));

        compile_setf_place(c, place, val_form);

        rest = cl_cdr(cl_cdr(rest));
        if (!CL_NULL_P(rest)) {
            cl_emit(c, OP_POP);
        }
    }
    CL_GC_UNPROTECT(1);
}

/* --- Function ref --- */

static void compile_function(CL_Compiler *c, CL_Obj form)
{
    CL_Obj name = cl_car(cl_cdr(form));

    if (CL_CONS_P(name) && cl_car(name) == SYM_LAMBDA) {
        compile_lambda(c, name);
    } else if (CL_SYMBOL_P(name)) {
        int fun_idx = cl_env_lookup_local_fun_index(c->env, name);
        if (fun_idx >= 0 && !CL_NULL_P(c->env->local_funs[fun_idx].inline_body))
            inline_local_misuse(c, name, "a #' (FUNCTION) reference");
        if (fun_idx >= 0) {
            int fun_slot = c->env->local_funs[fun_idx].slot;
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)fun_slot);
            if (c->env->boxed[fun_slot])
                cl_emit(c, OP_CELL_REF);
        } else if (c->env) {
            int uv_idx;
            if (cl_env_parent_fun_is_inline(c->env, name))
                inline_local_misuse(c, name, "a #' (FUNCTION) reference from inside a nested closure");
            uv_idx = cl_env_resolve_fun_upvalue(c->env, name);
            if (uv_idx >= 0) {
                cl_emit(c, OP_UPVAL);
                cl_emit(c, (uint8_t)uv_idx);
                if (c->env->upvalues[uv_idx].is_boxed)
                    cl_emit(c, OP_CELL_REF);
            } else {
                int idx = cl_add_constant(c, name);
                cl_emit(c, OP_FLOAD);
                cl_emit_u16(c, (uint16_t)idx);
            }
        } else {
            int idx = cl_add_constant(c, name);
            cl_emit(c, OP_FLOAD);
            cl_emit_u16(c, (uint16_t)idx);
        }
    } else if (CL_CONS_P(name) && cl_car(name) == SYM_SETF
               && CL_CONS_P(cl_cdr(name))
               && CL_SYMBOL_P(cl_car(cl_cdr(name)))
               && CL_NULL_P(cl_cdr(cl_cdr(name)))) {
        /* #'(setf accessor) — a function name of the form (setf symbol).
         * OP_FLOAD needs a SYMBOL, so resolve (setf accessor) to the symbol
         * that names its setter (the same one setf-PLACE compilation uses),
         * rather than FLOADing the raw (SETF ACCESSOR) cons (which is not a
         * symbol and corrupts the constant-pool slot at load time). */
        CL_Obj setf_fn = resolve_setf_fn_symbol(cl_car(cl_cdr(name)));
        int idx = cl_add_constant(c, setf_fn);
        cl_emit(c, OP_FLOAD);
        cl_emit_u16(c, (uint16_t)idx);
    } else {
        cl_error(CL_ERR_GENERAL,
                 "FUNCTION: %s is not a valid function name "
                 "(expected a symbol, (setf symbol), or a lambda expression)",
                 CL_CONS_P(name) && CL_SYMBOL_P(cl_car(name))
                     ? cl_symbol_name(cl_car(name)) : "form");
    }
}

/* --- Call --- */

/* Map a CL: builtin call site to a single-opcode inline form, when the
 * VM has an opcode that matches the function's semantics for this arity.
 * Returns the opcode if (name, nargs) is inlinable, or 0 otherwise.
 *
 * Per CLHS 11.1.2.1.2 conformant programs cannot redefine standard
 * COMMON-LISP symbols, so the inliner can ignore the function cell at
 * runtime — the opcode handler in vm.c is the authoritative meaning.
 *
 * Only the binary 2-arg case is inlined for arithmetic / comparison
 * (`+ - * = < > <= >=`).  CL allows variadic forms, but `(+ a b c)` =
 * `(+ (+ a b) c)` only for associative ops on exact reals — for floats
 * and bignums there are subtle ordering differences with bi_add's
 * single-pass loop, so the safe choice is to defer to bi_add for n != 2.
 * Likewise `(- a)` and `(- a b c)` need different bytecode shapes; only
 * the 2-arg case maps to OP_SUB cleanly.
 *
 * NULL and NOT both map to OP_NOT — semantically `cl:null` ≡ `cl:not`
 * (both return T iff arg is NIL, NIL otherwise). */
static uint8_t inline_builtin_opcode(CL_Obj func, int nargs)
{
    CL_Symbol *s;
    CL_String *name;

    if (!CL_SYMBOL_P(func)) return 0;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(func);
    if (s->package != cl_package_cl) return 0;
    name = (CL_String *)CL_OBJ_TO_PTR(s->name);

    switch (name->length) {
    case 1:
        if (nargs == 2) {
            switch (name->data[0]) {
            case '+': return OP_ADD;
            case '-': return OP_SUB;
            case '*': return OP_MUL;
            case '=': return OP_NUMEQ;
            case '<': return OP_LT;
            case '>': return OP_GT;
            }
        }
        break;
    case 2:
        if (nargs == 2) {
            if (name->data[0] == '<' && name->data[1] == '=') return OP_LE;
            if (name->data[0] == '>' && name->data[1] == '=') return OP_GE;
            if (name->data[0] == 'E' && name->data[1] == 'Q') return OP_EQ;
        }
        break;
    case 3:
        if (nargs == 1) {
            if (memcmp(name->data, "CAR", 3) == 0) return OP_CAR;
            if (memcmp(name->data, "CDR", 3) == 0) return OP_CDR;
            if (memcmp(name->data, "NOT", 3) == 0) return OP_NOT;
        }
        break;
    case 4:
        if (nargs == 1 && memcmp(name->data, "NULL", 4) == 0) return OP_NOT;
        if (nargs == 2 && memcmp(name->data, "CONS", 4) == 0) return OP_CONS;
        break;
    case 5:
        /* Two-argument CHAR= only: the n-ary form stays a call. */
        if (nargs == 2 && memcmp(name->data, "CHAR=", 5) == 0) return OP_CHAREQ;
        break;
    }
    return 0;
}

/* The one-index array readers OP_AREF inlines (specs/performance.md 4.4):
 * returns 1 + CL_AREF_KIND_* for a two-argument CL:AREF / SVREF / CHAR /
 * SCHAR call, 0 otherwise.  The kind byte keeps each accessor's own type
 * gate and error text (cl_vector_ref1). */
static int inline_aref_kind(CL_Obj func, int nargs)
{
    CL_Symbol *s;
    CL_String *name;
    if (nargs != 2 || !CL_SYMBOL_P(func)) return 0;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(func);
    if (s->package != cl_package_cl) return 0;
    name = (CL_String *)CL_OBJ_TO_PTR(s->name);
    switch (name->length) {
    case 4:
        if (memcmp(name->data, "AREF", 4) == 0) return 1 + CL_AREF_KIND_AREF;
        if (memcmp(name->data, "CHAR", 4) == 0) return 1 + CL_AREF_KIND_CHAR;
        break;
    case 5:
        if (memcmp(name->data, "SVREF", 5) == 0) return 1 + CL_AREF_KIND_SVREF;
        if (memcmp(name->data, "SCHAR", 5) == 0) return 1 + CL_AREF_KIND_SCHAR;
        break;
    }
    return 0;
}

/* Count proper-list length without allocating.  Caller must have
 * already validated that args is a proper list (or NIL). */
static int proper_list_length(CL_Obj list)
{
    int n = 0;
    while (!CL_NULL_P(list)) {
        list = cl_cdr(list);
        n++;
    }
    return n;
}

/* Emit an AmigaOS library call: compile the N_VALUE_ARGS forms in REST in
 * order (each pushed onto the VM stack), then OP_AMIGA_CALL with the
 * library-base symbol as a constant, the LVO offset, the packed regspec
 * (register nibbles bits 0-27, result kind bits 28-31) and the arg count.
 * Shared by the AMIGA:%FFI-CALL special form and the DEFCFUN stub inline
 * path below.  The JIT recognises the opcode and calls the trampoline
 * natively. */
static void emit_amiga_call(CL_Compiler *c, CL_Obj base_sym, int16_t lvo,
                            uint32_t regspec, CL_Obj rest, int n_value_args)
{
    int sym_idx = cl_add_constant(c, base_sym);

    /* Push value args in order.  Protect the cursor: compile_expr can
     * compact, relocating the arg list cells (same class as the
     * compile_let body-cursor bug). */
    CL_GC_PROTECT(rest);
    while (!CL_NULL_P(rest)) {
        compile_expr(c, cl_car(rest));
        rest = cl_cdr(rest);
    }
    CL_GC_UNPROTECT(1);

    cl_emit(c, OP_AMIGA_CALL);
    cl_emit_u16(c, (uint16_t)sym_idx);
    cl_emit_i16(c, lvo);
    cl_emit_i32(c, (int32_t)regspec);
    cl_emit(c, (uint8_t)n_value_args);
}

static void compile_call(CL_Compiler *c, CL_Obj form)
{
    CL_Obj func = cl_car(form);
    CL_Obj args = cl_cdr(form);
    int nargs = 0;
    int saved_tail = c->in_tail;
    int global_sym_idx = -1;
    char func_name_buf[128];

    /* Snapshot before any allocating call: compile_expr in the argument loop
     * can compact the heap, relocating `func` (unprotected CL_Obj local).
     * cl_symbol_name returns arena data that moves with the symbol on compaction. */
    if (CL_SYMBOL_P(func)) {
        const char *n = cl_symbol_name(func);
        strncpy(func_name_buf, n, sizeof(func_name_buf) - 1);
        func_name_buf[sizeof(func_name_buf) - 1] = '\0';
    } else {
        strncpy(func_name_buf, "<anonymous>", sizeof(func_name_buf) - 1);
        func_name_buf[sizeof(func_name_buf) - 1] = '\0';
    }

    /* Validate form structure — CDR must be a proper list */
    if (!CL_NULL_P(args) && !CL_CONS_P(args)) {
        cl_error(CL_ERR_GENERAL, "Compiler: malformed call form (dotted pair in argument list)");
        return;
    }

    c->in_tail = 0;

    /* GC-protect args: compile_expr for each arg may trigger macro expansion + GC */
    CL_GC_PROTECT(args);

    /* Compiler-macro expansion (CLHS 3.2.2.1).  Run before any opcode
     * inlining or call-emission so an expansion that opens up further
     * compile-time optimizations (e.g. defstruct's accessor macros
     * expanding to %struct-ref + literal index) can hit the dedicated
     * OP_STRUCT_REF / OP_AMIGA_CALL hooks below.  Only applies when the
     * symbol isn't shadowed by a local function or upvalue, matching
     * regular function-name resolution rules. */
    if (CL_SYMBOL_P(func) &&
        !cl_compiler_notinline_p(c, func) &&
        cl_env_lookup_local_fun(c->env, func) < 0 &&
        (!c->env || cl_env_resolve_fun_upvalue(c->env, func) < 0))
    {
        CL_Obj cmf = cl_get_compiler_macro(func);
        if (!CL_NULL_P(cmf)) {
            CL_Obj call_args[2];
            CL_Obj expanded = form;
            int expander_ok = 0;
            /* Compaction GC may run inside cl_vm_apply (the expander is
             * arbitrary user code).  Protect both the expander closure and
             * the form pointer we eq-compare against the result. */
            CL_GC_PROTECT(cmf);
            CL_GC_PROTECT(form);
            call_args[0] = form;
            call_args[1] = CL_NIL;  /* &environment — empty for now */

            /* Wrap the expander in CL_CATCH: an erroring compiler-macro
             * (e.g. an accessor expander whose struct-type isn't loaded
             * yet) must not longjmp through compile_call's caller chain,
             * which would smash the CL_CATCH frames in bi_compile_file.
             * On error, restore VM state and treat as "decline" — fall
             * through to normal call emission.  Mirrors the pattern in
             * scan_body_for_boxing at compiler.c:1115. */
            {
                int saved_sp = cl_vm.sp;
                int saved_fp = cl_vm.fp;
                int saved_dyn = cl_dyn_top;
                int saved_nlx = cl_nlx_top;
                int saved_handler = cl_handler_top;
                int saved_restart = cl_restart_top;
                int saved_debugger = cl_debugger_enabled;
                int saved_gc_roots = gc_root_count;
                int err;
                cl_debugger_enabled = 0;
                CL_CATCH(err);
                if (err == 0) {
                    expanded = cl_vm_apply(cmf, call_args, 2);
                    CL_UNCATCH();
                    cl_debugger_enabled = saved_debugger;
                    cl_handler_top = saved_handler;
                    cl_restart_top = saved_restart;
                    expander_ok = 1;
                } else {
                    CL_UNCATCH();
                    cl_vm.sp = saved_sp;
                    cl_vm.fp = saved_fp;
                    cl_dynbind_restore_to(saved_dyn);
                    cl_nlx_top = saved_nlx;
                    cl_handler_top = saved_handler;
                    cl_restart_top = saved_restart;
                    cl_debugger_enabled = saved_debugger;
                    gc_root_count = saved_gc_roots;
                    expanded = form;  /* decline on error */
                }
            }
            CL_GC_UNPROTECT(2);
            /* CLHS: returning the original form (eq) means "decline" */
            if (expander_ok && expanded != form) {
                CL_GC_UNPROTECT(1);  /* args */
                c->in_tail = saved_tail;
                compile_expr(c, expanded);
                return;
            }
            /* GC SAFETY: declined (or errored) — the expander may have
             * compacted.  form was protected across cl_vm_apply so the local
             * holds the forwarded offset; re-derive func from it (every
             * fall-through path below keeps using func, including
             * cl_add_constant for the FLOAD). */
            func = cl_car(form);
        }
    }

    /* Constant folding (spec 1.3, speed >= 1): a pure-builtin call whose
     * arguments are all compile-time constants collapses to one constant
     * load — (+ 1 2) emits CONST 3 instead of CONST 1; CONST 2; ADD.  Run
     * AFTER compiler-macro expansion has had a chance to fire (above): a
     * compiler macro defined on a foldable builtin name (+, -, ash, ...)
     * must still see the call even when every argument happens to be a
     * compile-time constant — folding first would silently bypass it. */
    if (c->optimize_settings.speed >= 1 && CL_SYMBOL_P(func)) {
        CL_Obj folded;
        if (try_fold_constant(c, form, &folded, 0)) {
            emit_folded_constant(c, folded);
            CL_GC_UNPROTECT(1);  /* args */
            c->in_tail = saved_tail;
            c->mv_state = CL_MV_ONE;   /* OP_CONST/OP_NIL/OP_T: one value */
            return;
        }
    }

    /* FFI:DEFCSTRUCT's bulk installer (ffi::%define-cstruct-accessors
     * '(entries)): register the accessor -> %SET-accessor DEFSETF pairs
     * NOW, at compile time, before compiling the call itself — the
     * per-field DEFSETF forms this call replaced had that immediate side
     * effect wherever they were compiled (top level or not), and code
     * relies on it: (progn (defcstruct pt (x :u16 0)) (setf (pt-x p) 1))
     * compiled as ONE form must resolve PT-X's updater while the SETF is
     * compiled.  Only a literal quoted list is inspected; anything else is
     * just a call.  The registrar allocates (intern, cons): root FORM and
     * re-derive FUNC afterwards, like the compiler-macro path above. */
    if (CL_SYMBOL_P(func) && cl_ffi_define_cstruct_accessors_sym != CL_NIL &&
        func == cl_ffi_define_cstruct_accessors_sym &&
        cl_env_lookup_local_fun(c->env, func) < 0 &&
        (!c->env || cl_env_resolve_fun_upvalue(c->env, func) < 0) &&
        CL_CONS_P(args) && CL_NULL_P(cl_cdr(args)))
    {
        CL_Obj q = cl_car(args);
        if (CL_CONS_P(q) && cl_car(q) == SYM_QUOTE &&
            CL_CONS_P(cl_cdr(q)) && CL_NULL_P(cl_cdr(cl_cdr(q)))) {
            CL_GC_PROTECT(form);
            cl_ffi_cstruct_register_setfs(cl_car(cl_cdr(q)));
            func = cl_car(form);
            CL_GC_UNPROTECT(1);
        }
        /* fall through: ordinary call emission */
    }

    /* DEFCFUN stub inline: a direct call to a symbol whose GLOBAL function
     * cell holds a CL_STUB_LIBCALL descriptor (what AMIGA.FFI:DEFCFUN
     * installs — types.h CL_FfiStub) compiles to a bare OP_AMIGA_CALL in
     * the caller: no FLOAD, no OP_CALL dispatch, no wrapper frame; the JIT
     * turns the opcode into a native trampoline call.  This is what the
     * per-function compiler macros DEFCFUN used to register did (one alist
     * entry per binding, scanned for every call the compiler saw); the
     * stub IS the definition, so the compiler reads it directly.
     *
     * Same scoping rules as a compiler macro: skipped when the name is
     * lexically shadowed (flet/labels, an upvalue) or declared notinline.
     * Same semantics too: the call site bakes in what the global
     * definition was at compile time — a later redefinition of the name
     * does not reach already-compiled callers (documented for the
     * bindings).  An arity mismatch declines, so the runtime arity error
     * (cl_ffi_stub_call) is the one the user sees, not a compile-time
     * surprise.  Field stubs (DEFCSTRUCT accessors) are NOT inlined: the
     * plain FLOAD + OP_CALL already reaches cl_ffi_stub_call with no
     * wrapper frame, which is as cheap as an inlined builtin call. */
    if (CL_SYMBOL_P(func) &&
        !cl_compiler_notinline_p(c, func) &&
        cl_env_lookup_local_fun(c->env, func) < 0 &&
        (!c->env || cl_env_resolve_fun_upvalue(c->env, func) < 0))
    {
        CL_Obj fn = ((CL_Symbol *)CL_OBJ_TO_PTR(func))->function;
        if (CL_FFI_STUB_P(fn)) {
            CL_FfiStub *fs = (CL_FfiStub *)CL_OBJ_TO_PTR(fn);
            if (fs->kind == CL_STUB_LIBCALL &&
                proper_list_length(args) == (int)fs->ctype) {
                /* copy the fields out first — emit_amiga_call compiles
                 * argument forms, which can compact and move the stub */
                CL_Obj base_sym = fs->aux;
                int16_t lvo = fs->b;
                uint32_t regspec = fs->a;
                int n = (int)fs->ctype;
                emit_amiga_call(c, base_sym, lvo, regspec, args, n);
                CL_GC_UNPROTECT(1);  /* args */
                c->in_tail = saved_tail;
                c->mv_state = CL_MV_ONE;   /* OP_AMIGA_CALL: one value */
                return;
            }
        }
    }

    /* AmigaOS FFI special form: (amiga:%ffi-call base-sym offset regspec args...)
     * Form invariant: arg 0 is a symbol literal naming the library-base var,
     * arg 1 is the LVO offset (fixnum), arg 2 is the packed regspec (fixnum;
     * 7 register nibbles in bits 0-27, result kind in bits 28-29 — only
     * kinds 0-3 fit a Lisp fixnum; DEFCFUN's stubs carry the wider kinds).
     * The rest are regular expressions, evaluated and pushed in order, then
     * OP_AMIGA_CALL pops them and trampolines.  Skips the OP_FLOAD + OP_CALL
     * dispatch and the call_builtin marshalling for every library call. */
    if (CL_SYMBOL_P(func) && cl_amiga_ffi_call_sym != CL_NIL &&
        func == cl_amiga_ffi_call_sym &&
        cl_env_lookup_local_fun(c->env, func) < 0 &&
        (!c->env || cl_env_resolve_fun_upvalue(c->env, func) < 0))
    {
        CL_Obj base_sym, offset_obj, regspec_obj, rest;
        int n_value_args;
        int16_t lvo_offset;
        uint32_t regspec_val;

        if (proper_list_length(args) < 3)
            cl_error(CL_ERR_GENERAL,
                     "AMIGA:%%FFI-CALL: needs at least base, offset, regspec");
        base_sym    = cl_car(args);
        offset_obj  = cl_car(cl_cdr(args));
        regspec_obj = cl_car(cl_cdr(cl_cdr(args)));
        rest        = cl_cdr(cl_cdr(cl_cdr(args)));

        if (!CL_SYMBOL_P(base_sym))
            cl_error(CL_ERR_TYPE,
                     "AMIGA:%%FFI-CALL: base must be a symbol literal");
        if (!CL_FIXNUM_P(offset_obj))
            cl_error(CL_ERR_TYPE,
                     "AMIGA:%%FFI-CALL: offset must be a fixnum literal");
        if (!CL_FIXNUM_P(regspec_obj))
            cl_error(CL_ERR_TYPE,
                     "AMIGA:%%FFI-CALL: regspec must be a fixnum literal");

        lvo_offset = (int16_t)CL_FIXNUM_VAL(offset_obj);
        regspec_val = (uint32_t)CL_FIXNUM_VAL(regspec_obj);
        n_value_args = proper_list_length(rest);
        if (n_value_args < 0 || n_value_args > 7)
            cl_error(CL_ERR_ARGS,
                     "AMIGA:%%FFI-CALL: too many register args (max 7), got %d",
                     n_value_args);

        emit_amiga_call(c, base_sym, lvo_offset, regspec_val, rest, n_value_args);

        CL_GC_UNPROTECT(1);  /* args */
        c->in_tail = saved_tail;
        c->mv_state = CL_MV_ONE;   /* OP_AMIGA_CALL: one value */
        return;
    }

    /* Inline (amilisp::%struct-ref obj <fixnum 0..255>) and
     * (amilisp::%struct-set obj <fixnum 0..255> val) as dedicated
     * opcodes when the index is a small literal — bypasses the wrapper
     * accessor function call AND the bi_struct_ref/set builtin frame.
     * Compound accessors like (line-sx l) defstruct expands to
     * (%struct-ref l 0); after the wrapper inlines, this hook fires.
     * Falls through to the regular call when idx isn't a u8 literal —
     * the builtin remains for dynamic-index callers. */
    if (CL_SYMBOL_P(func) &&
        cl_struct_ref_sym != CL_NIL &&
        cl_env_lookup_local_fun(c->env, func) < 0 &&
        (!c->env || cl_env_resolve_fun_upvalue(c->env, func) < 0))
    {
        int is_ref = (func == cl_struct_ref_sym);
        int is_set = (func == cl_struct_set_sym);
        if (is_ref || is_set) {
            int expected = is_ref ? 2 : 3;
            if (proper_list_length(args) == expected) {
                CL_Obj obj_form = cl_car(args);
                CL_Obj idx_form = cl_car(cl_cdr(args));
                CL_Obj val_form = is_set ? cl_car(cl_cdr(cl_cdr(args))) : CL_NIL;
                if (CL_FIXNUM_P(idx_form)) {
                    int32_t idx = CL_FIXNUM_VAL(idx_form);
                    if (idx >= 0 && idx <= 255) {
                        compile_expr(c, obj_form);
                        if (is_set) {
                            /* GC SAFETY: compile_expr(obj_form) can compact —
                             * re-derive val_form from the protected `args`
                             * rather than using the pre-compaction offset. */
                            val_form = cl_car(cl_cdr(cl_cdr(args)));
                            compile_expr(c, val_form);
                        }
                        cl_emit(c, is_ref ? OP_STRUCT_REF : OP_STRUCT_SET);
                        cl_emit(c, (uint8_t)idx);
                        CL_GC_UNPROTECT(1);
                        c->in_tail = saved_tail;
                        c->mv_state = CL_MV_ONE;   /* both push exactly one value */
                        return;
                    }
                }
            }
        }
    }

    /* Builtin inlining: if `func` is a CL: symbol that matches a
     * VM opcode for this arity AND isn't shadowed by a local
     * function or upvalue, emit the opcode directly.  Saves the
     * FLOAD + CALL (~3 bytes + dispatch through call_builtin) per
     * call site, and removes one symbol from the constants pool.
     * (declare (notinline ...)) forbids this per CLHS 3.2.2.3 —
     * such calls must compile as real out-of-line calls. */
    if (CL_SYMBOL_P(func) &&
        !cl_compiler_notinline_p(c, func) &&
        cl_env_lookup_local_fun(c->env, func) < 0 &&
        (!c->env || cl_env_resolve_fun_upvalue(c->env, func) < 0))
    {
        int candidate_nargs = proper_list_length(args);
        uint8_t opcode = inline_builtin_opcode(func, candidate_nargs);
        int aref_kind = opcode ? 0 : inline_aref_kind(func, candidate_nargs);
        if (opcode != 0 || aref_kind != 0) {
            CL_Obj rest = args;
            /* Protect the cursor: compile_expr can compact, relocating the
             * arg list cells; without &rest registered, rest = cl_cdr(rest)
             * would follow a stale offset (same class as the compile_let
             * body-cursor bug). */
            CL_GC_PROTECT(rest);
            while (!CL_NULL_P(rest)) {
                compile_expr(c, cl_car(rest));
                rest = cl_cdr(rest);
            }
            if (aref_kind != 0) {
                cl_emit(c, OP_AREF);
                cl_emit(c, (uint8_t)(aref_kind - 1));
            } else {
                cl_emit(c, opcode);
            }
            CL_GC_UNPROTECT(2);  /* rest, args */
            c->in_tail = saved_tail;
            c->mv_state = CL_MV_ONE;   /* inlined builtin opcodes: one value */
            return;
        }
    }

    /* A call to a GLOBAL function name does not push the function at all:
     * it compiles to OP_CALL_GLOBAL / OP_TAILCALL_GLOBAL, which resolve the
     * symbol's function binding in the handler after the arguments have been
     * pushed (one dispatch and no stack slot instead of FLOAD + CALL; the
     * symbol's binding is read at call time, so redefinition still takes
     * effect).  global_sym_idx >= 0 records that shape for the emit below. */
    if (CL_SYMBOL_P(func)) {
        int fun_idx = cl_env_lookup_local_fun_index(c->env, func);
        if (fun_idx >= 0 && !CL_NULL_P(c->env->local_funs[fun_idx].inline_body)) {
            /* A non-escaping local function keeps no closure: the call IS
             * its body, compiled in place (compiler_special.c).  It leaves
             * in_tail and mv_state as the body's compile set them. */
            compile_local_inline_call(c, fun_idx, args, saved_tail);
            CL_GC_UNPROTECT(1);  /* args */
            return;
        }
        if (fun_idx >= 0) {
            int fun_slot = c->env->local_funs[fun_idx].slot;
            cl_emit(c, OP_LOAD);
            cl_emit(c, (uint8_t)fun_slot);
            if (c->env->boxed[fun_slot])
                cl_emit(c, OP_CELL_REF);
        } else if (c->env) {
            int uv_idx;
            if (cl_env_parent_fun_is_inline(c->env, func))
                inline_local_misuse(c, func, "a call from inside a nested closure");
            uv_idx = cl_env_resolve_fun_upvalue(c->env, func);
            if (uv_idx >= 0) {
                cl_emit(c, OP_UPVAL);
                cl_emit(c, (uint8_t)uv_idx);
                if (c->env->upvalues[uv_idx].is_boxed)
                    cl_emit(c, OP_CELL_REF);
            } else {
                global_sym_idx = cl_add_constant(c, func);
            }
        } else {
            global_sym_idx = cl_add_constant(c, func);
        }
    } else {
        compile_expr(c, func);
    }

    while (!CL_NULL_P(args)) {
        CL_Obj arg_val = cl_car(args);
        if (CL_HEAP_P(arg_val) && arg_val >= cl_heap.arena_size) {
            fprintf(stderr, "[compile_call] BUG: arg %d = 0x%08x exceeds heap (args=0x%08x nargs=%d code_pos=%d)\n",
                    nargs, (unsigned)arg_val, (unsigned)args, nargs, c->code_pos);
        }
        compile_expr(c, arg_val);
        nargs++;
        args = cl_cdr(args);
    }

    CL_GC_UNPROTECT(1);
    c->in_tail = saved_tail;

    /* OP_CALL/OP_TAILCALL encode the argument count in a single byte; a call
     * with more than 255 args would silently wrap the count and desync the VM
     * stack (the callee slot would land on an argument value).  Signal a clean
     * compile error instead.  Compiler-generated calls that can grow unbounded
     * (notably quasiquote's APPEND) chunk themselves to stay within this bound;
     * see CL_QQ_MAX_CALL_ARGS in compiler_extra.c. */
    if (nargs > 255) {
        cl_error(CL_ERR_OVERFLOW,
                 "Too many arguments in call to %s (%d, max 255)",
                 func_name_buf,
                 nargs);
    }

    /* cl_emit_call also classifies the result CL_MV_MANY: the callee
     * establishes the MV state, which is what makes (floor 7 2) two-valued. */
    if (global_sym_idx >= 0)
        cl_emit_call_global(c, c->in_tail ? OP_TAILCALL_GLOBAL : OP_CALL_GLOBAL,
                            (uint16_t)global_sym_idx, (uint8_t)nargs);
    else
        cl_emit_call(c, c->in_tail ? OP_TAILCALL : OP_CALL, (uint8_t)nargs);
}

/* Trampoline-aware analog of compile_body.  Strips leading declarations
 * + docstrings, then returns the tail form (or CL_NIL for empty body). */
CL_Obj compile_body_tail(CL_Compiler *c, CL_Obj forms)
{
    CL_Obj rest = process_body_declarations(c, forms);
    return compile_progn_tail(c, rest);
}

void compile_body(CL_Compiler *c, CL_Obj forms)
{
    /* Mirrors compile_progn: compile_body_tail may push PROGN_ITER frames
     * that compile_expr won't drain (its own tail_base captures them
     * as already-existing).  We drain explicitly afterward. */
    int tail_base = c->tail_count;
    /* Scope any body (declare (optimize ...)) to this body: compile_body
     * drains its own frames (no enclosing postlude restores for us), so
     * snapshot here and restore on every exit.  Covers lambda/defun and
     * destructuring-bind bodies. */
    CL_OptimizeSettings saved_optimize = c->optimize_settings;
    CL_Obj tail = compile_body_tail(c, forms);
    /* A NIL `tail` is ambiguous: it can mean either an empty body OR that the
     * tail form is the literal NIL.  When compile_body_tail pushed a
     * PROGN_ITER frame (c->tail_count > tail_base), the body had a leading
     * literal NIL form followed by more forms — e.g. (lambda () nil 42), which
     * serapeum's define-case-macro emits.  In that case we must NOT short to
     * OP_NIL: compile the NIL tail (the leading form) and drain the frame so
     * the trailing forms are still emitted.  Only short-circuit when no frame
     * was pushed (genuinely empty body, or a single literal NIL form). */
    if (CL_NULL_P(tail) && c->tail_count == tail_base) {
        cl_emit(c, OP_NIL);
        restore_optimize_settings(c, &saved_optimize);
        return;
    }
    compile_expr(c, tail);
    while (c->tail_count > tail_base) {
        CL_TailFrame frame = c->tail_stack[--c->tail_count];
        CL_Obj cont = emit_tail_postlude(c, &frame);
        if (!CL_NULL_P(cont))
            compile_expr(c, cont);
    }
    restore_optimize_settings(c, &saved_optimize);
}

/* Look up a global symbol-macro expansion stored on the symbol's plist
   under the AMILISP::%SYMBOL-MACRO-EXPANSION indicator (set by the
   DEFINE-SYMBOL-MACRO macro in boot.lisp).  Returns CL_NIL when none. */
CL_Obj cl_lookup_global_symbol_macro(CL_Obj sym)
{
    CL_Obj out;
    if (cl_lookup_global_symbol_macro_p(sym, &out))
        return out;
    return CL_NIL;
}

/* Cached %SYMBOL-MACRO-EXPANSION indicator symbol.  Interned and rooted
 * during cl_compiler_init (not lazily): the interned symbol lives in the
 * arena and is relocated by the compacting GC, so the cache must be a
 * registered root — and registering at INIT (rather than on first use)
 * keeps it inside the image restore set's boot-root window, so a saved
 * heap image restores it instead of warning about a late-registered root
 * (specs/image-save-load.md).  A warm-FASL boot never compiles, which is
 * why the old first-use registration happened post-boot. */
static CL_Obj symbol_macro_indicator = 0;

int cl_lookup_global_symbol_macro_p(CL_Obj sym, CL_Obj *out)
{
    CL_Symbol *s;
    CL_Obj plist;

    if (!CL_SYMBOL_P(sym)) return 0;
    if (symbol_macro_indicator == 0) {
        /* Fallback for harnesses that bypass cl_compiler_init. */
        symbol_macro_indicator =
            cl_intern_in("%SYMBOL-MACRO-EXPANSION", 23, cl_package_amilisp);
        cl_gc_register_root(&symbol_macro_indicator);
    }

    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    plist = s->plist;
    while (!CL_NULL_P(plist) && !CL_NULL_P(cl_cdr(plist))) {
        if (cl_car(plist) == symbol_macro_indicator) {
            *out = cl_car(cl_cdr(plist));
            return 1;
        }
        plist = cl_cdr(cl_cdr(plist));
    }
    return 0;
}

static void compile_symbol(CL_Compiler *c, CL_Obj sym)
{
    int slot;

    /* Every path out of here pushes exactly one value; only the two that go
     * through OP_LOAD / OP_CELL_REF leave the MV buffer describing an older
     * form (CL_MV_STALE).  Being precise about the difference is what lets a
     * join like (if p x *global*) still see "stale" and normalize. */
    if (CL_NULL_P(sym)) { cl_emit(c, OP_NIL); c->mv_state = CL_MV_ONE; return; }
    if (sym == SYM_T)    { cl_emit(c, OP_T);  c->mv_state = CL_MV_ONE; return; }

    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
        if (s->package == cl_package_keyword) {
            cl_emit_const(c, sym);
            return;
        }
    }

    /* Lexical symbol-macro (symbol-macrolet) — innermost lexical binding.
     * cl_env_lookup_symbol_macro_p is shadow-aware: it returns 0 when an inner
     * LET binds the same name, so the lexical-variable lookup below wins.
     * Use the predicate form so a symbol-macro whose expansion is NIL (e.g.
     * (symbol-macrolet ((a nil)) a)) is still recognized as a symbol-macro
     * rather than falling through to an unbound-variable reference. */
    {
        CL_Obj expansion = CL_NIL;
        if (cl_env_lookup_symbol_macro_p(c->env, sym, &expansion)) {
            compile_expr(c, expansion);
            return;
        }
    }

    slot = cl_env_lookup(c->env, sym);
    if (slot >= 0) {
        cl_emit(c, OP_LOAD);
        cl_emit(c, (uint8_t)slot);
        if (c->env->boxed[slot])
            cl_emit(c, OP_CELL_REF);
        /* Neither OP_LOAD nor OP_CELL_REF touches the MV buffer, so it still
         * describes whatever ran before this reference — see cl_mv_normalize.
         * (Not resetting here is deliberate and load-bearing: the local-jump
         * BLOCK join stores and reloads its result through a slot and relies
         * on the MV state surviving that round trip.) */
        c->mv_state = CL_MV_STALE;
        return;
    }

    if (c->env) {
        int uv_idx = cl_env_resolve_upvalue(c->env, sym);
        if (uv_idx >= 0) {
            cl_emit(c, OP_UPVAL);
            cl_emit(c, (uint8_t)uv_idx);
            c->mv_state = CL_MV_ONE;       /* OP_UPVAL writes mv_count */
            if (c->env->upvalues[uv_idx].is_boxed) {
                cl_emit(c, OP_CELL_REF);
                c->mv_state = CL_MV_STALE; /* ... but OP_CELL_REF does not */
            }
            return;
        }
    }

    /* Global symbol-macro (define-symbol-macro) — checked only after lexical
     * variable / upvalue lookups, so a lexically-bound variable of the same
     * name shadows it (CLHS 3.1.2.1.1).  serapeum's `local` relies on this:
     * it binds a global symbol-macro's name as a fresh lexical variable and
     * then assigns through it. */
    {
        CL_Obj expansion = CL_NIL;
        if (cl_lookup_global_symbol_macro_p(sym, &expansion)) {
            compile_expr(c, expansion);
            return;
        }
    }

    {
        int idx = cl_add_constant(c, sym);
        cl_emit(c, OP_GLOAD);
        cl_emit_u16(c, (uint16_t)idx);
        c->mv_state = CL_MV_ONE;   /* OP_GLOAD writes mv_count */
    }
}

/* --- Main dispatcher ---
 *
 * compile_expr_step processes one form and either:
 *   - finishes by emitting bytecode (returns 0), or
 *   - tail-trampolines by writing a new form to *expr_p (returns 1).
 *
 * The driver compile_expr() runs compile_expr_step() in a loop, then
 * drains any tail frames pushed during the chain.  This keeps the C
 * stack flat across deeply nested forms like (let A (let B (let C ...))).
 */

static int compile_expr_step(CL_Compiler *c, CL_Obj *expr_p)
{
    CL_Obj expr = *expr_p;

    /* Deeply nested forms (heavy macro towers) recurse through here once
     * per level.  Without this check a form too deep for the task stack
     * silently corrupts memory — on AmigaOS with a small shell `stack`
     * this is a real field failure mode (compiling a large GUI function
     * at `stack 128000` overflowed, poisoning the session AND the FASL
     * it wrote).  cl_check_recursion_guards measures true headroom against
     * the task's stack bounds (and the GC-root-stack ceiling) and signals
     * a clean, actionable error. */
    cl_check_recursion_guards("cl_compile");

    /* MV hygiene default (see the CL_MV_* comment in compiler_internal.h):
     * assume this form's code establishes the MV state and may leave more
     * than one value, which is what a call does.  The forms whose value is
     * delivered by an opcode that leaves the MV buffer alone mark themselves
     * CL_MV_STALE instead — compile_symbol's OP_LOAD path, compile_setq's
     * peeking stores, the local-jump BLOCK join.  Erring towards MANY leaves
     * an existing leak in place; erring towards STALE would emit a reset
     * that destroys real values. */
    c->mv_state = CL_MV_MANY;

    if (CL_NULL_P(expr))    { cl_emit(c, OP_NIL); c->mv_state = CL_MV_ONE; return 0; }
    if (CL_FIXNUM_P(expr))  { cl_emit_const(c, expr); return 0; }
    if (CL_CHAR_P(expr))    { cl_emit_const(c, expr); return 0; }
    if (CL_ANY_STRING_P(expr)) { cl_emit_const(c, expr); return 0; }
    if (CL_BIGNUM_P(expr))  { cl_emit_const(c, expr); return 0; }
    if (CL_RATIO_P(expr))   { cl_emit_const(c, expr); return 0; }
    if (CL_COMPLEX_P(expr)) { cl_emit_const(c, expr); return 0; }
    if (CL_FLOATP(expr))    { cl_emit_const(c, expr); return 0; }
    if (CL_VECTOR_P(expr))  { cl_emit_const(c, expr); return 0; }
    if (CL_BIT_VECTOR_P(expr)) { cl_emit_const(c, expr); return 0; }
    if (CL_PATHNAME_P(expr))   { cl_emit_const(c, expr); return 0; }
    if (CL_STRUCT_P(expr))     { cl_emit_const(c, expr); return 0; }
    if (CL_HASHTABLE_P(expr))  { cl_emit_const(c, expr); return 0; }

    if (CL_SYMBOL_P(expr)) {
        compile_symbol(c, expr);
        return 0;
    }

    /* Any non-cons heap object that isn't a symbol is self-evaluating per
     * CLHS 3.1.2.1.3.  This catches streams, locks, condition-variables,
     * thread objects, random-state, conditions, instances, etc. — none of
     * them have a meaningful interpretation as a form, so EVAL returns them
     * unchanged.  Without this, e.g. lparallel's
     *   (loop for (nil . form) in specials collect (eval form))
     * blows up with "Cannot compile: unexpected STREAM" the first time a
     * thread is spawned with default special bindings. */
    if (CL_HEAP_P(expr) && !CL_CONS_P(expr)) {
        cl_emit_const(c, expr);
        return 0;
    }

    if (CL_CONS_P(expr)) {
        CL_Obj head = cl_car(expr);

        /* Record source location for this expression */
        {
            int line = cl_srcloc_lookup(expr);
            if (line > 0 && c->line_entry_count < CL_MAX_LINE_ENTRIES) {
                int last = c->line_entry_count - 1;
                /* Only add if different from last entry */
                if (last < 0 || c->line_entries[last].line != (uint16_t)line
                             || c->line_entries[last].pc != (uint16_t)c->code_pos) {
                    c->line_entries[c->line_entry_count].pc = (uint16_t)c->code_pos;
                    c->line_entries[c->line_entry_count].line = (uint16_t)line;
                    c->line_entry_count++;
                }
                c->current_line = line;
            }
        }

        /* Check local macros (macrolet) before global macros */
        if (CL_SYMBOL_P(head)) {
            CL_Obj local_expander = cl_env_lookup_local_macro(c->env, head);
            if (!CL_NULL_P(local_expander)) {
                /* Local macrolet expander stored by compile_macrolet is a
                 * 2-arg (form environment) wrapper — same shape as global
                 * macros installed by compile_defmacro. */
                CL_Obj expanded;
                CL_Obj call_args[2];
                CL_Obj lex_env;
                /* GC SAFETY: protect local_expander BEFORE cl_build_lex_env,
                 * which allocates and can compact — otherwise the expander
                 * closure offset goes stale and cl_vm_apply would run a
                 * relocated (wrong) closure.  Likewise the local `expr` copy
                 * goes stale across that compaction, so re-read it from the
                 * GC-protected *expr_p (the driver protects &expr, so it is
                 * forwarded) rather than passing the stale local form. */
                CL_GC_PROTECT(local_expander);
                lex_env = cl_build_lex_env(c->env);
                expr = *expr_p;
                call_args[0] = expr;
                call_args[1] = lex_env;
                CL_GC_PROTECT(lex_env);
                {
                    int _fp0 = cl_vm.fp, _sp0 = cl_vm.sp;
                    expanded = cl_vm_apply(local_expander, call_args, 2);
                    if (cl_vm.fp != _fp0 || cl_vm.sp != _sp0) {
                        fprintf(stderr, "[MXLEAK-LOCAL] local macrolet vm_apply leaked fp:%d->%d sp:%d->%d\n",
                                _fp0, cl_vm.fp, _sp0, cl_vm.sp);
                        fflush(stderr);
                    }
                }
                CL_GC_UNPROTECT(2);
                /* Trampoline into the expansion — outer compile_expr
                 * driver keeps `expr` GC-protected via &expr. */
                *expr_p = expanded;
                return 1;
            }
        }

        /* CLHS 3.1.2.1.2.4: a function form's CAR can be a global function,
         * a local FLET/LABELS function, OR a macro.  Local FLET/LABELS
         * bindings SHADOW global macro definitions of the same symbol
         * (per CLHS 3.1.2.1.2 "kinds of forms").  Without this check, a
         * `(labels ((test (c) ...)) ... (test c) ...)` whose `test`
         * symbol also has a global macro binding (e.g. fiveam:test from
         * a co-loaded library) would expand to the macro's body
         * `(register-test ...)`, breaking the labels invocation.  This
         * surfaced specifically as "Undefined function: REGISTER-TEST"
         * during cold compile of trivia.balland2006/optimizer.lisp's
         * `apply-fusion`, which uses `(labels ((test (c) ...)) ...)`. */
        if (CL_SYMBOL_P(head)) {
            if (cl_env_lookup_local_fun(c->env, head) >= 0)
                goto do_call;
            if (c->env && cl_env_resolve_fun_upvalue(c->env, head) >= 0)
                goto do_call;
        }

        /* Compiler-inlined control-flow forms that ALSO have a real Lisp
         * macro definition (lib/boot.lisp gives COND/AND/OR/CASE/… genuine
         * expansions so MACROEXPAND and code-walkers like iterate are
         * conformant).  These must be dispatched to their native compilers
         * HERE — before the generic macro-expansion check below — otherwise
         * the compiler would trampoline into the Lisp expansion and lose the
         * optimized special-form path (and any subtle semantic differences,
         * e.g. an empty CASE clause body, would leak in).  See
         * register_inlined_macro_stubs / the macroexpand-conformance note in
         * boot.lisp. */
        if (head == SYM_AND)         { compile_and(c, expr); return 0; }
        if (head == SYM_OR)          { compile_or(c, expr); return 0; }
        if (head == SYM_COND)        { compile_cond(c, expr); return 0; }
        if (head == SYM_CASE)        { compile_case(c, expr, 0); return 0; }
        if (head == SYM_ECASE)       { compile_case(c, expr, 1); return 0; }
        if (head == SYM_TYPECASE)    { compile_typecase(c, expr, 0); return 0; }
        if (head == SYM_ETYPECASE)   { compile_typecase(c, expr, 1); return 0; }
        /* destructuring-bind also has a genuine Lisp macro (lib/boot.lisp) for
         * MACROEXPAND/code-walker conformance, but must compile via its native
         * special-form path — so dispatch it here, ahead of the macro check. */
        if (head == SYM_DESTRUCTURING_BIND) { compile_destructuring_bind(c, expr); return 0; }
        /* PUSH / POP on an unboxed lexical variable compile to one opcode
         * (specs/performance.md 4.4); every other place goes through the
         * macro in lib/boot.lisp. */
        if ((head == SYM_PUSH || head == SYM_POP) && try_compile_push_pop(c, expr))
            return 0;

        if (CL_SYMBOL_P(head) && cl_macro_p(head)) {
            int _fp0 = cl_vm.fp, _sp0 = cl_vm.sp;
            CL_Obj expanded;
            CL_Obj lex_env = cl_build_lex_env(c->env);
            CL_GC_PROTECT(lex_env);
            /* GC SAFETY: cl_build_lex_env allocates (and, with local
             * macrolet/symbol-macrolet bindings in scope, conses a non-empty
             * lexical env), so a compaction may have run.  Our local `expr`
             * and `head` copies hold pre-compaction offsets and are now stale;
             * refresh them from the GC-protected *expr_p (the driver keeps
             * &expr protected, so it was forwarded).  Without this, when the
             * macro is an identity stub (expanded == expr, so no trampoline)
             * the stale `expr`/`head` fall through to special-form dispatch
             * and compile a relocated, aliased form — e.g. a macrolet body's
             * `(setf y v)` was dispatched with the enclosing PROGN form,
             * yielding a bogus "%SETF-SETF"/wrong value under GC stress. */
            expr = *expr_p;
            head = cl_car(expr);
            expanded = cl_macroexpand_1_env(expr, lex_env);
            CL_GC_UNPROTECT(1);
            if (cl_vm.fp != _fp0 || cl_vm.sp != _sp0) {
                const char *_mname = CL_SYMBOL_P(head) ? cl_symbol_name(head) : "?";
                fprintf(stderr, "[MXLEAK-GLOBAL] macroexpand_1(%s) leaked fp:%d->%d sp:%d->%d\n",
                        _mname, _fp0, cl_vm.fp, _sp0, cl_vm.sp);
                fflush(stderr);
            }
            if (expanded != expr) {
                /* Trampoline into the expansion — outer driver protects expr. */
                *expr_p = expanded;
                return 1;
            }
            /* Macro returned the form unchanged (stub) — fall through to
             * special-form dispatch so the form is compiled as-is. */
        }

        if (head == SYM_QUOTE)       { compile_quote(c, expr); return 0; }
        if (head == SYM_IF) {
            /* Always returns the THEN form; ELSE handling deferred to
             * the IF_AFTER_THEN postlude. */
            *expr_p = compile_if(c, expr);
            return 1;
        }
        /* Trampoline PROGN: walk all but last, then continue with last. */
        if (head == SYM_PROGN) {
            CL_Obj forms = cl_cdr(expr);
            if (CL_NULL_P(forms)) { cl_emit(c, OP_NIL); return 0; }
            /* GC-protect the body-list cursor across the recursive
             * compile_expr below.  Compiling a subform macroexpands and
             * allocates; under heap pressure that triggers a compacting GC
             * which relocates the (arena-consed) PROGN body.  Without this
             * protect `forms` keeps a stale offset and the subsequent
             * cl_cdr(forms) walks into relocated/garbage memory — silently
             * dropping later body forms (e.g. the DEFMETHOD emitted by a
             * DEFINE-CONDITION expansion compiled under stress). */
            CL_GC_PROTECT(forms);
            while (!CL_NULL_P(cl_cdr(forms))) {
                int prev_tail = c->in_tail;
                c->in_tail = 0;
                compile_expr(c, cl_car(forms));
                c->in_tail = prev_tail;
                cl_emit(c, OP_POP);
                forms = cl_cdr(forms);
            }
            *expr_p = cl_car(forms);
            CL_GC_UNPROTECT(1);
            return 1;
        }
        if (head == SYM_LAMBDA)      { compile_lambda(c, expr); return 0; }
        /* Trampoline LET / LET*: prelude pushes a CL_TAIL_LET frame and
         * returns the tail body form (or CL_NIL for empty body). */
        if (head == SYM_LET || head == SYM_LETSTAR) {
            CL_Obj tail = compile_let(c, expr, head == SYM_LETSTAR);
            if (CL_NULL_P(tail)) {
                cl_emit(c, OP_NIL);
                return 0;
            }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_SETQ)        { compile_setq(c, expr); return 0; }
        if (head == SYM_SETF)        { compile_setf(c, expr); return 0; }
        if (head == SYM_FUNCTION)    { compile_function(c, expr); return 0; }
        /* compiler_extra.c */
        if (head == SYM_NAMED_LAMBDA) { compile_named_lambda(c, expr); return 0; }
        if (head == SYM_DEFUN)       { compile_defun(c, expr); return 0; }
        if (head == SYM_DEFVAR)      { compile_defvar(c, expr); return 0; }
        if (head == SYM_DEFPARAMETER) { compile_defparameter(c, expr); return 0; }
        if (head == SYM_DEFCONSTANT)  { compile_defconstant(c, expr); return 0; }
        if (head == SYM_DEFMACRO)    { compile_defmacro(c, expr); return 0; }
        /* AND/OR/COND/CASE/ECASE/TYPECASE/ETYPECASE are dispatched earlier,
         * ahead of the macro-expansion check (they have real Lisp macros for
         * MACROEXPAND conformance but must compile via their native path). */
        if (head == SYM_QUASIQUOTE)  { compile_quasiquote(c, expr); return 0; }
        if (head == SYM_EVAL_WHEN) {
            CL_Obj tail = compile_eval_when(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_LOAD_TIME_VALUE) { compile_load_time_value(c, expr); return 0; }
        if (head == SYM_DEFSETF)     { compile_defsetf(c, expr); return 0; }
        if (head == SYM_DEFTYPE)     { compile_deftype(c, expr); return 0; }
        if (head == SYM_MULTIPLE_VALUE_BIND) {
            CL_Obj tail = compile_multiple_value_bind(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_MULTIPLE_VALUE_CALL)  { compile_multiple_value_call(c, expr); return 0; }
        if (head == SYM_MULTIPLE_VALUE_LIST)  { compile_multiple_value_list(c, expr); return 0; }
        if (head == SYM_MULTIPLE_VALUE_PROG1) { compile_multiple_value_prog1(c, expr); return 0; }
        if (head == SYM_NTH_VALUE)            { compile_nth_value(c, expr); return 0; }
        /* compiler_special.c */
        /* BLOCK / RETURN-FROM / RETURN trampoline.  The prelude emits any
         * setup bytecode and pushes a tail frame; we continue the trampoline
         * with the tail body / value form (or emit OP_NIL for empty body). */
        if (head == SYM_BLOCK) {
            CL_Obj tail = compile_block(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_RETURN_FROM) {
            CL_Obj tail = compile_return_from(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_RETURN) {
            CL_Obj tail = compile_return(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_FLET) {
            CL_Obj tail = compile_flet(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_LABELS) {
            CL_Obj tail = compile_labels(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_DO)          { compile_do(c, expr); return 0; }
        if (head == SYM_DO_STAR)     { compile_do_star(c, expr); return 0; }
        if (head == SYM_DOLIST)      { compile_dolist(c, expr); return 0; }
        if (head == SYM_DOTIMES)     { compile_dotimes(c, expr); return 0; }
        if (head == SYM_TAGBODY)     { compile_tagbody(c, expr); return 0; }
        if (head == SYM_GO)          { compile_go(c, expr); return 0; }
        if (head == SYM_CATCH)       { compile_catch(c, expr); return 0; }
        if (head == SYM_UNWIND_PROTECT) { compile_unwind_protect(c, expr); return 0; }
        if (head == SYM_PROGV) {
            CL_Obj tail = compile_progv(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_HANDLER_BIND) {
            CL_Obj tail = compile_handler_bind(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (cl_handler_case_sym != CL_NIL && head == cl_handler_case_sym) {
            compile_handler_case(c, expr);
            return 0;
        }
        if (head == SYM_RESTART_CASE) { compile_restart_case(c, expr); return 0; }
        if (head == SYM_DECLARE) {
            /* Tolerate misplaced declares (ignore them with a warning).
             * Some macro expansions place declares outside body position;
             * standard CL implementations just ignore them there. */
            platform_write_string("\033[33mWARNING: DECLARE is not allowed here -- ignoring\033[0m\n");
            cl_emit(c, OP_NIL);
            return 0;
        }
        if (head == SYM_DECLAIM)     { compile_declaim(c, expr); return 0; }
        if (head == SYM_LOCALLY) {
            CL_Obj tail = compile_locally(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_TRACE)       { compile_trace(c, expr); return 0; }
        if (head == SYM_UNTRACE)     { compile_untrace(c, expr); return 0; }
        if (head == SYM_TIME)        { compile_time(c, expr); return 0; }
        if (head == SYM_IN_PACKAGE)  { compile_in_package(c, expr); return 0; }
        if (head == SYM_MACROLET) {
            CL_Obj tail = compile_macrolet(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_SYMBOL_MACROLET) {
            CL_Obj tail = compile_symbol_macrolet(c, expr);
            if (CL_NULL_P(tail)) { cl_emit(c, OP_NIL); return 0; }
            *expr_p = tail;
            return 1;
        }
        if (head == SYM_THE)             { compile_the(c, expr); return 0; }

        /* (funcall fn arg1 arg2 ...) → compile fn as value, args, OP_CALL
         * Avoids C stack nesting by keeping the call in the VM dispatch loop. */
        if (head == SYM_FUNCALL) {
            CL_Obj fargs = cl_cdr(expr);
            int nargs = 0;
            int saved_tail;
            if (CL_NULL_P(fargs))
                cl_error(CL_ERR_ARGS, "FUNCALL requires at least one argument");
            saved_tail = c->in_tail;
            c->in_tail = 0;
            /* GC-protect fargs: compile_expr may trigger macro expansion + GC */
            CL_GC_PROTECT(fargs);
            /* Compile the function expression */
            compile_expr(c, cl_car(fargs));
            /* Compile remaining arguments */
            fargs = cl_cdr(fargs);
            while (!CL_NULL_P(fargs)) {
                compile_expr(c, cl_car(fargs));
                nargs++;
                fargs = cl_cdr(fargs);
            }
            CL_GC_UNPROTECT(1);
            c->in_tail = saved_tail;
            cl_emit_call(c, c->in_tail ? OP_TAILCALL : OP_CALL,
                         (uint8_t)nargs);
            return 0;
        }

        /* (apply fn arg1 ... arglist) → build full arglist via CONS, OP_APPLY
         * Avoids C stack nesting by using the VM's inline OP_APPLY handler. */
        if (head == SYM_APPLY) {
            CL_Obj fargs = cl_cdr(expr);
            int napply_args = 0;
            int saved_tail, i;
            if (CL_NULL_P(fargs) || CL_NULL_P(cl_cdr(fargs)))
                cl_error(CL_ERR_ARGS, "APPLY requires at least two arguments");
            saved_tail = c->in_tail;
            c->in_tail = 0;
            /* GC-protect fargs: compile_expr may trigger macro expansion + GC */
            CL_GC_PROTECT(fargs);
            /* Compile the function expression */
            compile_expr(c, cl_car(fargs));
            /* Compile all remaining args (leading args + final arglist) */
            fargs = cl_cdr(fargs);
            while (!CL_NULL_P(fargs)) {
                compile_expr(c, cl_car(fargs));
                napply_args++;
                fargs = cl_cdr(fargs);
            }
            CL_GC_UNPROTECT(1);
            /* CONS leading args onto the final arglist to build full arglist.
             * Stack has: fn a1 a2 ... arglist
             * We need (n-1) CONS ops to produce: fn (a1 a2 ... . arglist) */
            for (i = 0; i < napply_args - 1; i++)
                cl_emit(c, OP_CONS);
            c->in_tail = saved_tail;
            cl_emit(c, OP_APPLY);
            c->mv_state = CL_MV_MANY;   /* the callee establishes the MV state */
            return 0;
        }

    do_call:
        compile_call(c, expr);
        return 0;
    }

    cl_error(CL_ERR_GENERAL, "Cannot compile: unexpected %s in expression position",
             cl_type_name(expr));
    return 0;  /* unreachable */
}

/* Driver: trampoline-loop over compile_expr_step and drain pending tail
 * frames.  Each step either finishes (return 0) or sets a new form for
 * the trampoline (return 1).  The drain emits postludes (env restore,
 * OP_DYNUNBIND, etc.) for any frames the chain pushed.
 *
 * A postlude can return a continuation form (non-CL_NIL) — that form
 * becomes the next trampoline target, after which the drain resumes.
 * IF_AFTER_THEN uses this to dispatch the ELSE branch once THEN has
 * compiled, keeping both branches off the C stack. */
void compile_expr(CL_Compiler *c, CL_Obj expr)
{
    int tail_base = c->tail_count;

    /* expr is updated in place via compile_expr_step's *expr_p; protect &expr
     * for the entire chain so macroexpand/let-prelude allocations don't
     * dangling our current form. */
    CL_GC_PROTECT(expr);

    while (compile_expr_step(c, &expr)) { /* trampoline */ }

    while (c->tail_count > tail_base) {
        /* Copy by value: emit_tail_postlude may push new frames which
         * could realloc tail_stack and invalidate any pointer into it. */
        CL_TailFrame frame = c->tail_stack[--c->tail_count];
        CL_Obj cont = emit_tail_postlude(c, &frame);
        if (!CL_NULL_P(cont)) {
            /* expr is GC-protected via &expr; reassigning carries the
             * continuation form into protected territory. */
            expr = cont;
            while (compile_expr_step(c, &expr)) { /* trampoline */ }
        }
    }

    CL_GC_UNPROTECT(1);
}

/* --- Macro expansion (runtime, via VM) --- */

CL_Obj cl_macroexpand_1(CL_Obj form)
{
    return cl_macroexpand_1_env(form, CL_NIL);
}

/* Walk a lex-env alist (built by cl_build_lex_env) for a MACROLET entry
 * whose name matches sym.  Returns the expander or CL_NIL. */
static CL_Obj lex_env_local_macro(CL_Obj lex_env, CL_Obj sym)
{
    while (CL_CONS_P(lex_env)) {
        CL_Obj pair = cl_car(lex_env);
        if (CL_CONS_P(pair) && cl_car(pair) == SYM_LEX_LOCAL_MACRO) {
            CL_Obj inner = cl_cdr(pair);
            if (CL_CONS_P(inner) && cl_car(inner) == sym)
                return cl_cdr(inner);
        }
        lex_env = cl_cdr(lex_env);
    }
    return CL_NIL;
}

CL_Obj cl_macroexpand_1_env(CL_Obj form, CL_Obj lex_env)
{
    CL_Obj head = cl_car(form);
    CL_Obj expander;
    CL_Obj expanded;
    CL_Obj call_args[2];

    /* Local (macrolet) bindings shadow global macros — check the
     * captured lex-env first so &environment-aware expanders that
     * MACROEXPAND a sub-form see macrolet-bound macros (CLHS 5.1.4). */
    if (CL_SYMBOL_P(head) && CL_CONS_P(lex_env)) {
        expander = lex_env_local_macro(lex_env, head);
        if (!CL_NULL_P(expander)) {
            CL_Obj saved_expanding = cl_expansion_ctx_get(cl_expanding_form_sym);
            call_args[0] = form;
            call_args[1] = lex_env;
            CL_GC_PROTECT(form);
            CL_GC_PROTECT(expander);
            CL_GC_PROTECT(lex_env);
            CL_GC_PROTECT(saved_expanding);
            /* Error context: if the expander signals, cl_error snapshots
             * this so the report names the form being expanded.  On the
             * error path the restore below is skipped (longjmp) — correct:
             * the innermost failing form stays visible.  Per-thread TLV
             * value (see error.h), not a raw global, so a peer thread
             * expanding concurrently never sees or clobbers this. */
            cl_tlv_set(CT, cl_expanding_form_sym, form);
            expanded = cl_vm_apply(expander, call_args, 2);
            cl_tlv_set(CT, cl_expanding_form_sym, saved_expanding);
            CL_GC_UNPROTECT(4);
            return expanded;
        }
    }

    expander = cl_get_macro(head);
    if (CL_NULL_P(expander)) return form;

    /* Expander is a 2-arg (form environment) wrapper produced by
     * compile_defmacro (or a user-supplied function via SETF MACRO-FUNCTION).
     * The wrapper itself sets cl_current_lex_env for any &environment
     * binding inside the inner expander body. */
    call_args[0] = form;
    call_args[1] = lex_env;

    {
        CL_Obj saved_expanding = cl_expansion_ctx_get(cl_expanding_form_sym);
        CL_GC_PROTECT(form);
        CL_GC_PROTECT(expander);
        CL_GC_PROTECT(lex_env);
        CL_GC_PROTECT(saved_expanding);

        /* Error context — see the macrolet branch above. */
        cl_tlv_set(CT, cl_expanding_form_sym, form);
        expanded = cl_vm_apply(expander, call_args, 2);
        cl_tlv_set(CT, cl_expanding_form_sym, saved_expanding);

        CL_GC_UNPROTECT(4);
    }

    return expanded;
}


/* --- Macro table --- */

void cl_register_macro(CL_Obj name, CL_Obj expander)
{
    /* Both conses run OUTSIDE the write lock (see cl_table_prepend_locked
     * — allocating under cl_tables_wrlock can deadlock STW-vs-rwlock). */
    cl_table_prepend_locked(&macro_table, cl_cons(name, expander));
}

/* Snapshot-and-release iteration:
 *   macro_table is only ever PREPENDED-to (cl_register_macro builds a
 *   new head whose cdr is the prior head).  Old cells stay reachable
 *   from the new head's cdr chain, so once we capture the head pointer
 *   under the lock we can release the lock and walk the snapshot
 *   without races.  Holding the rdlock across cl_car (which can
 *   cl_error → longjmp on a corrupt cell) was leaking readers and
 *   blocking later writers — see the cl_tables_rwlock deadlock fix. */
int cl_macro_p(CL_Obj name)
{
    CL_Obj list;
    cl_tables_rdlock();
    list = macro_table;
    cl_tables_rwunlock();
    while (!CL_NULL_P(list)) {
        if (cl_car(cl_car(list)) == name)
            return 1;
        list = cl_cdr(list);
    }
    return 0;
}

CL_Obj cl_get_macro(CL_Obj name)
{
    CL_Obj list;
    cl_tables_rdlock();
    list = macro_table;
    cl_tables_rwunlock();
    while (!CL_NULL_P(list)) {
        CL_Obj pair = cl_car(list);
        if (cl_car(pair) == name)
            return cl_cdr(pair);
        list = cl_cdr(list);
    }
    return CL_NIL;
}

/* --- Compiler-macro table --- */

/* Compiler macros (CLHS 3.2.2.1): a per-name expander invoked by
 * compile_call before regular function dispatch.  The expander runs at
 * compile time on the call form; if it returns the *same* form (eq), the
 * compiler treats it as "decline" and proceeds with the normal call.
 * Otherwise the returned form replaces the call (re-compiled in place).
 *
 * Storage mirrors macro_table — prepend-only alist, GC-marked from
 * mem.c — but hash-indexed (CL_AlistIndex, compiler.h): compile_call
 * probes this table for EVERY call form it compiles, so with a few
 * hundred registered expanders (defstruct accessors, the generated OS
 * bindings used to add ~550) a linear walk taxed the compilation of
 * every unrelated call site.  defstruct uses these to inline accessor
 * wrappers like (line-sx l) → (amilisp::%struct-ref l 0) which then
 * trips the OP_STRUCT_REF compiler hook. */
static CL_AlistIndex cmacro_index = { &compiler_macro_table, NULL, 0, 0, 0 };

void cl_register_compiler_macro(CL_Obj name, CL_Obj expander)
{
    cl_alist_index_prepend(&cmacro_index, cl_cons(name, expander));
}

CL_Obj cl_get_compiler_macro(CL_Obj name)
{
    CL_Obj pair = cl_alist_index_find(&cmacro_index, name);
    return CL_NULL_P(pair) ? CL_NIL : cl_cdr(pair);
}

/* --- DEFSETF table: accessor -> updater function name ---
 *
 * Same shape and same reason: every (setf (f ...) v) the compiler sees
 * probes this table (compile_setf) — one entry per defsetf, i.e. per
 * struct field of every FFI:DEFCSTRUCT (~1550 for the four common OS
 * binding modules) — so it is hash-indexed too.  Registered at compile
 * time by compile_defsetf and at load time by OP_DEFSETF; read by
 * compile_setf and %GET-DEFSETF-SETTER. */
static CL_AlistIndex setf_index = { &setf_table, NULL, 0, 0, 0 };

void cl_register_setf_updater(CL_Obj accessor, CL_Obj updater)
{
    cl_alist_index_prepend(&setf_index, cl_cons(accessor, updater));
}

CL_Obj cl_get_setf_updater(CL_Obj accessor)
{
    CL_Obj pair = cl_alist_index_find(&setf_index, accessor);
    return CL_NULL_P(pair) ? CL_NIL : cl_cdr(pair);
}

/* --- Lexical notinline tracking (CLHS 3.2.2.1.3) --- */

/* Record FUNC as notinline in the current lexical scope.  Deduplicates and
 * silently ignores names past CL_MAX_NOTINLINE (falls back to old behavior).
 * Callers save c->notinline_count before the scope and restore it after. */
void cl_compiler_push_notinline(CL_Compiler *c, CL_Obj func)
{
    int i;
    if (!CL_SYMBOL_P(func)) return;
    for (i = 0; i < c->notinline_count; i++)
        if (c->notinline_fns[i] == func) return;   /* already active */
    if (c->notinline_count < CL_MAX_NOTINLINE)
        c->notinline_fns[c->notinline_count++] = func;
}

/* 1 if FUNC is lexically declared notinline — used to suppress
 * compiler-macro expansion, which CLHS says notinline must inhibit. */
int cl_compiler_notinline_p(CL_Compiler *c, CL_Obj func)
{
    int i;
    for (i = 0; i < c->notinline_count; i++)
        if (c->notinline_fns[i] == func)
            return 1;
    return 0;
}

/* --- Type table (for deftype) --- */

/* Hash-indexed: TYPEP consults cl_get_type_expander for every symbol
 * type spec that isn't a struct/condition type, so a linear walk made
 * TYPEP O(deftypes) (see CL_AlistIndex in compiler.h). */
static CL_AlistIndex type_index = { &type_table, NULL, 0, 0, 0 };

/* Compaction moved the indexes' key symbols and entry conses — mark all
 * three compiler-table indexes stale (called from the compaction update
 * phase, world stopped). */
void cl_compiler_indexes_gc_invalidate(void)
{
    cl_alist_index_invalidate(&type_index);
    cl_alist_index_invalidate(&cmacro_index);
    cl_alist_index_invalidate(&setf_index);
}

void cl_register_type(CL_Obj name, CL_Obj expander)
{
    cl_alist_index_prepend(&type_index, cl_cons(name, expander));
}

CL_Obj cl_get_type_expander(CL_Obj name)
{
    CL_Obj pair = cl_alist_index_find(&type_index, name);
    return CL_NULL_P(pair) ? CL_NIL : cl_cdr(pair);
}

/* --- Setf function table --- */

/* Public wrapper around resolve_setf_fn_symbol: given the accessor symbol FOO
 * (the FOO in a (setf FOO) function name), return the symbol that names the
 * (setf FOO) function.  Used by FDEFINITION/FBOUNDP/FMAKUNBOUND to honour the
 * (setf symbol) function-name form (CLHS glossary "function name"). */
CL_Obj cl_setf_function_symbol(CL_Obj accessor)
{
    return resolve_setf_fn_symbol(accessor);
}

void cl_register_setf_function(CL_Obj accessor, CL_Obj setf_fn_sym)
{
    cl_table_prepend_locked(&setf_fn_table, cl_cons(accessor, setf_fn_sym));
}

/* --- Public API --- */

/* A CL_Compiler is too large for the AmigaOS stack (its fixed tables --
 * blocks, tagbodies, lambda-list scratch -- are ~80 KB) and too costly to
 * alloc/free per call.  AllocVec/FreeVec churn of big blocks fragments the
 * AmigaOS system pool: when the struct still held its bytecode and constants
 * as fixed arrays (366 KB), the next AllocVec returned NULL after roughly 44
 * cycles loading lib/clos.lisp even though plenty of free RAM existed.
 * To avoid that we keep a process-wide free-list — popped on entry, pushed
 * on exit — so a successfully allocated CL_Compiler is never returned to
 * AmigaOS, and neither are the bytecode and constants buffers it grew, up to
 * CL_COMPILER_CODE_KEEP / CL_COMPILER_CONST_KEEP (bigger ones are rare and
 * go back, so one huge form does not pin them for the rest of the run).
 * Nested compiles (parent chain) are handled correctly: each call still gets
 * a distinct struct.  The chain is short (<= depth of compile-time macro
 * expansion) so memory growth is bounded. */
static CL_Compiler *cl_compiler_pool_head = NULL;
/* cl_compiler_pool_lock itself is forward-declared near the top of this
 * file (compiler_grow_code/compiler_grow_constants need it before this
 * point) and lazily initialized below, in cl_compiler_pool_init. */
/* Blocks currently parked on the free list.  Kept so cl_compiler_pool_init
 * can top the pool up to CL_COMPILER_POOL_PREWARM instead of blindly adding
 * another 8 blocks: cl_compiler_init runs again on every heap re-init (unit
 * tests, embedded restarts), and an unconditional pre-warm grew the pool by
 * ~2.9 MB per cycle. */
static int cl_compiler_pool_count = 0;

/* 68k AmigaOS: 2 blocks (~160 KB) instead of 8 (~640 KB of Fast RAM per
 * process, and Clamacs runs two).  The pool still grows on demand -- the
 * pre-warm only insures against AllocVec failing for a block after the system
 * pool has been churned, and since ab6e1001 a block is 80 KB rather than
 * 375 KB, so two reserved blocks cover the common nested-compile chain and a
 * deeper one falls back to an on-demand AllocVec that is now far more likely
 * to succeed.  MorphOS and the host have RAM to spare and keep 8. */
#if defined(PLATFORM_AMIGA) && !defined(PLATFORM_MORPHOS)
#define CL_COMPILER_POOL_PREWARM 2
#else
#define CL_COMPILER_POOL_PREWARM 8
#endif

static CL_Compiler *cl_compiler_pool_acquire(void)
{
    CL_Compiler *c = NULL;
    if (cl_compiler_pool_lock) platform_mutex_lock(cl_compiler_pool_lock);
    if (cl_compiler_pool_head) {
        c = cl_compiler_pool_head;
        cl_compiler_pool_head = (CL_Compiler *)c->parent; /* free-list link */
        cl_compiler_pool_count--;
    }
    if (cl_compiler_pool_lock) platform_mutex_unlock(cl_compiler_pool_lock);
    if (!c) {
        c = (CL_Compiler *)platform_alloc(sizeof(CL_Compiler));
        if (!c) return NULL;
    }
    /* Hand the block out zeroed, but keep the buffers a previous compile
     * grew: reusing them is what the pool is for.  Their contents are dead
     * (code_pos / const_count restart at 0). */
    {
        uint8_t *code = c->code;
        CL_Obj *constants = c->constants;
        int code_cap = c->code_cap, const_cap = c->const_cap;
        memset(c, 0, sizeof(*c));
        c->code = code;
        c->code_cap = code_cap;
        c->constants = constants;
        c->const_cap = const_cap;
    }
    return c;
}

static void cl_compiler_pool_release(CL_Compiler *c)
{
    if (!c) return;
    /* The tail stack is dropped; the bytecode and constants buffers stay
     * with the block unless one compile grew them past the keep size. */
    if (c->tail_stack) { platform_free(c->tail_stack); c->tail_stack = NULL; }
    if (c->code_cap > CL_COMPILER_CODE_KEEP) {
        platform_free(c->code);
        c->code = NULL;
        c->code_cap = 0;
    }
    if (c->const_cap > CL_COMPILER_CONST_KEEP) {
        platform_free(c->constants);
        c->constants = NULL;
        c->const_cap = 0;
    }
    if (cl_compiler_pool_lock) platform_mutex_lock(cl_compiler_pool_lock);
    c->parent = cl_compiler_pool_head; /* reuse parent slot as free-list link */
    cl_compiler_pool_head = c;
    cl_compiler_pool_count++;
    if (cl_compiler_pool_lock) platform_mutex_unlock(cl_compiler_pool_lock);
}

void cl_compiler_pool_init(void)
{
    if (!cl_compiler_pool_lock)
        platform_mutex_init(&cl_compiler_pool_lock);
    /* Pre-warm: an AmigaOS AllocVec this size succeeds reliably at startup
     * (system pool unfragmented) but starts to fail after a workload like
     * source-loading lib/clos.lisp churns memory.  Reserve enough blocks
     * up front to cover the worst-case nested-compile chain depth we have
     * seen (1 outer cl_compile + several compile_lambda for inner lambdas
     * in CLOS-heavy methods).  8 is comfortably above the high-water mark
     * observed.  The buffers are not pre-warmed: they start at a kilobyte
     * and the first compiles size them.
     *
     * Top up to the target rather than adding a fixed 8: this runs again on
     * every cl_compiler_init, i.e. once per heap re-init. */
    if (cl_compiler_pool_lock) platform_mutex_lock(cl_compiler_pool_lock);
    while (cl_compiler_pool_count < CL_COMPILER_POOL_PREWARM) {
        CL_Compiler *c = (CL_Compiler *)platform_alloc(sizeof(CL_Compiler));
        if (!c) break;
        c->tail_stack = NULL;
        c->parent = cl_compiler_pool_head;
        cl_compiler_pool_head = c;
        cl_compiler_pool_count++;
    }
    if (cl_compiler_pool_lock) platform_mutex_unlock(cl_compiler_pool_lock);
}

void cl_compiler_pool_stats(CL_CompilerPoolStats *st)
{
    CL_Compiler *c;

    memset(st, 0, sizeof(*st));
    if (cl_compiler_pool_lock) platform_mutex_lock(cl_compiler_pool_lock);
    for (c = cl_compiler_pool_head; c; c = (CL_Compiler *)c->parent) {
        uint32_t code = (uint32_t)c->code_cap;
        uint32_t consts = (uint32_t)c->const_cap * (uint32_t)sizeof(CL_Obj);
        st->parked++;
        st->buffer_bytes += code + consts;
        if (code > st->largest_parked) st->largest_parked = code;
        if (consts > st->largest_parked) st->largest_parked = consts;
    }
    if (cl_compiler_pool_lock) platform_mutex_unlock(cl_compiler_pool_lock);
    st->block_bytes = (uint32_t)sizeof(CL_Compiler);
    st->code_max = (uint32_t)compiler_code_cap_max;
    st->const_max = (uint32_t)compiler_const_cap_max;
}

/* Release the compiler pool at process exit.
 *
 * During a run the pool deliberately never returns a CL_Compiler to the OS
 * (see the comment above cl_compiler_pool_head): re-AllocVec-ing blocks this
 * size fragments the AmigaOS system pool until the allocation fails outright.
 * That reasoning stops at process exit, where the blocks and their buffers
 * are pure loss — on AmigaOS memory not handed back before the process ends
 * is gone from the system pool until reboot, so the pool (then eight 366 KB
 * blocks, ~2.9 MB) went missing on every amilisp launch.  This was the bulk of
 * a measured 3.74 MB of Fast RAM lost per run on a Vampire (see
 * tests/test_shutdown_leak.sh).
 *
 * Safe here because no compile can be in flight: main() calls this after the
 * exit hooks and the VM are done, so every block that was ever handed out has
 * been released back to the free list. */
void cl_compiler_shutdown(void)
{
    CL_Compiler *c;

    if (cl_compiler_pool_lock) platform_mutex_lock(cl_compiler_pool_lock);
    c = cl_compiler_pool_head;
    cl_compiler_pool_head = NULL;
    cl_compiler_pool_count = 0;
    if (cl_compiler_pool_lock) platform_mutex_unlock(cl_compiler_pool_lock);

    {
        int n = 0;
        unsigned long buffers = 0;
        while (c) {
            CL_Compiler *next = (CL_Compiler *)c->parent;  /* free-list link */
            if (c->tail_stack) platform_free(c->tail_stack);
            if (c->code) {
                buffers += (unsigned long)c->code_cap;
                platform_free(c->code);
            }
            if (c->constants) {
                buffers += (unsigned long)c->const_cap * sizeof(CL_Obj);
                platform_free(c->constants);
            }
            platform_free(c);
            c = next;
            n++;
        }
        if (cl_mem_diag) {
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "[mem] compiler pool: %d block(s) x %lu bytes + %lu "
                     "buffer bytes = %lu bytes released (largest buffers "
                     "grown: %d bytes of bytecode, %d constants)\n",
                     n, (unsigned long)sizeof(CL_Compiler), buffers,
                     (unsigned long)n * (unsigned long)sizeof(CL_Compiler)
                         + buffers,
                     compiler_code_cap_max, compiler_const_cap_max);
            platform_write_string(buf);
        }
    }

    /* Hash indexes over the deftype / compiler-macro / defsetf alists.  Each
     * is a platform_alloc'd table sized to twice the entry count, rebuilt
     * (not grown) whenever its alist changes, so only an explicit reset ever
     * returns one.  A few hundred deftypes already make these tens of KB. */
    cl_alist_index_reset(&type_index);
    cl_alist_index_reset(&setf_index);
    cl_alist_index_reset(&cmacro_index);

    if (cl_compiler_pool_lock) {
        platform_mutex_destroy(cl_compiler_pool_lock);
        cl_compiler_pool_lock = NULL;
    }
    if (cl_tables_rwlock) {
        platform_rwlock_destroy(cl_tables_rwlock);
        cl_tables_rwlock = NULL;
    }
}

/* Release the interned source-file pool: one entry plus one path copy per
 * file ever compiled, never shrinking during a run.
 *
 * Separate from cl_compiler_shutdown, and called LAST (main.c, after
 * cl_mem_shutdown), because every CL_Bytecode's `source_file` points into
 * this pool.  That is why bytecode_release_offheap must not free the string
 * per object — the pool owns it — and why freeing the pool while bytecodes
 * still reference it would leave dangling pointers that the crash handler
 * dereferences when it prints a frame's source location.  cl_mem_shutdown's
 * off-heap walk NULLs every source_file first, so by the time this runs
 * nothing points here any more. */
void cl_compiler_release_source_pool(void)
{
    CL_SourceFileEntry *e = cl_source_file_pool;
    cl_source_file_pool = NULL;
    while (e) {
        CL_SourceFileEntry *next = e->next;
        if (e->path) platform_free(e->path);
        platform_free(e);
        e = next;
    }
    if (cl_source_file_pool_lock) {
        platform_rwlock_destroy(cl_source_file_pool_lock);
        cl_source_file_pool_lock = NULL;
    }
}

/* Reconstruct a CompEnv's local-macro / symbol-macro bindings from a lex-env
 * alist (as produced by cl_build_lex_env).  Recurses to the tail first so the
 * innermost (head-of-alist) entries are added last and thus win in the
 * highest-index-wins lookup used by cl_env_lookup_local_macro. */
static void seed_env_from_lex_env(CL_CompEnv *env, CL_Obj lex_env)
{
    CL_Obj pair;
    if (!CL_CONS_P(lex_env)) return;
    seed_env_from_lex_env(env, cl_cdr(lex_env));
    pair = cl_car(lex_env);
    if (!CL_CONS_P(pair)) return;
    if (cl_car(pair) == SYM_LEX_LOCAL_MACRO) {
        CL_Obj inner = cl_cdr(pair);            /* (name . expander) */
        if (CL_CONS_P(inner))
            cl_env_add_local_macro(env, cl_car(inner), cl_cdr(inner));
    } else {
        /* symbol-macro entry: (name . expansion) */
        cl_env_add_symbol_macro(env, cl_car(pair), cl_cdr(pair));
    }
}

/* Core of cl_compile.  LEX_ENV (CL_NIL or an alist from cl_build_lex_env)
 * pre-populates the null compilation environment with macrolet/symbol-macrolet
 * bindings so a form lifted out of a top-level MACROLET still sees them
 * (CLHS 3.2.3.1).  The caller must keep LEX_ENV GC-protected. */
static CL_Obj cl_compile_env(CL_Obj expr, CL_Obj lex_env)
{
    CL_Compiler *comp;
    CL_CompEnv *env;
    CL_Bytecode *bc;

    comp = cl_compiler_pool_acquire();
    if (!comp) {
        platform_write_string("[compile] platform_alloc(CL_Compiler) FAILED\n");
        return CL_NIL;
    }
    env = cl_env_create(NULL);
    comp->env = env;
    comp->in_tail = 0;
    comp->mv_state = CL_MV_ONE;   /* nothing emitted yet */

    /* Register compiler for GC root marking.  The anchor records this frame
     * as the owner, which keeps the compiler alive across NLX landings in VM
     * runs nested below it: a subform macroexpansion runs in the VM
     * (cl_macroexpand_1_env → cl_vm_run); if it performs a non-local exit,
     * the BLOCK/CATCH/UNWIND-PROTECT unwind calls cl_compiler_unwind_to,
     * which must not free this compiler — and cl_env_destroy its env — out
     * from under the in-progress compile.  The subsequent c->env dereference
     * (e.g. cl_env_lookup_local_macro) would then be a heap-use-after-free
     * (crashed compiling large macro-generated forms, e.g. babel's
     * INSTANTIATE-CONCRETE-MAPPINGS).  A longjmp that abandons this frame
     * instead frees the compiler at the landing — leaving it on the chain
     * would poison every later top-level compile with this compiler's stale
     * optimize settings (DECLAIM appears dead for the rest of the session). */
    comp->parent = cl_active_compiler;
    comp->anchor = CL_CAPTURE_SP();
    cl_compiler_seed_optimize_settings(comp);
    cl_active_compiler = comp;

    /* Seed local macros AFTER linking into the active-compiler chain so the
     * installed expander closures are GC-rooted via comp->env during the
     * compile below. */
    if (CL_CONS_P(lex_env))
        seed_env_from_lex_env(env, lex_env);

    compile_expr(comp, expr);

    /* The caller of cl_vm_eval (EVAL, LOAD, the REPL's value echo) reads the
     * MV state this leaves behind, so it must describe EXPR's own values. */
    cl_mv_normalize(comp);
    cl_emit(comp, OP_HALT);

    /* Peephole post-pass — see the compile_lambda call site. */
    cl_peephole_optimize(comp);

    bc = (CL_Bytecode *)cl_alloc(TYPE_BYTECODE, sizeof(CL_Bytecode));
    if (!bc) {
        platform_write_string("[compile] cl_alloc(TYPE_BYTECODE) FAILED\n");
        cl_active_compiler = comp->parent;  /* unlink before freeing */
        cl_env_destroy(env);
        cl_compiler_pool_release(comp);
        return CL_NIL;
    }

    bc->code = (uint8_t *)platform_alloc(comp->code_pos);
    if (bc->code) memcpy(bc->code, comp->code, comp->code_pos);
    bc->code_len = comp->code_pos;

    if (comp->const_count > 0) {
        int i;
        bc->constants = (CL_Obj *)platform_alloc(
            comp->const_count * sizeof(CL_Obj));
        if (bc->constants) {
            for (i = 0; i < comp->const_count; i++)
                bc->constants[i] = comp->constants[i];
        }
        bc->n_constants = comp->const_count;
    } else {
        bc->constants = NULL;
        bc->n_constants = 0;
    }

    bc->arity = 0;
    bc->n_locals = env->max_locals;
    bc->n_upvalues = 0;
    bc->name = CL_NIL;
    bc->n_optional = 0;
    bc->flags = 0;
    bc->n_keys = 0;
    bc->key_syms = NULL;
    bc->key_slots = NULL;
    bc->key_suppliedp_slots = NULL;
    bc->source_lambda_list = CL_NIL;  /* top-level form: no user lambda-list */

    /* Transfer source line map */
    if (comp->line_entry_count > 0) {
        int i;
        bc->line_map = (CL_LineEntry *)platform_alloc(
            comp->line_entry_count * sizeof(CL_LineEntry));
        if (bc->line_map) {
            for (i = 0; i < comp->line_entry_count; i++)
                bc->line_map[i] = comp->line_entries[i];
        }
        bc->line_map_count = (uint16_t)comp->line_entry_count;
    } else {
        bc->line_map = NULL;
        bc->line_map_count = 0;
    }
    bc->source_line = (uint16_t)comp->current_line;
    bc->source_file = cl_current_source_file;

    bc->native_code = NULL;
    bc->native_len  = 0;
    bc->native_relocs = NULL;
    bc->native_reloc_count = 0;
    /* A top-level form runs once, through cl_vm_eval (never OP_CALL's
     * native path): it is compiled only in eager mode, as before. */
    bc->jit_hot = 0;
    cl_jit_note_definition(bc);  /* no-op on host / when JIT_M68K undefined */

    /* Unregister compiler from GC root chain */
    cl_active_compiler = comp->parent;

    cl_env_destroy(env);

    cl_compiler_pool_release(comp);

    return CL_PTR_TO_OBJ(bc);
}

CL_Obj cl_compile(CL_Obj expr)
{
    return cl_compile_env(expr, CL_NIL);
}

/* Compile EXPR with the macrolet/symbol-macrolet bindings recorded in
 * LEX_ENV (an alist from cl_build_lex_env) in scope.  Used by compile-file to
 * compile body forms lifted out of a top-level MACROLET/SYMBOL-MACROLET. */
CL_Obj cl_compile_lex(CL_Obj expr, CL_Obj lex_env)
{
    return cl_compile_env(expr, lex_env);
}

/* Build the lexical macro environment (alist, per cl_build_lex_env) for a
 * top-level MACROLET whose bindings are BINDINGS, layered over INHERITED_LEX_ENV
 * (CL_NIL at the outermost level).  Returns CL_NIL if there is nothing to add.
 * The result must be GC-protected by the caller. */
CL_Obj cl_macrolet_lex_env(CL_Obj bindings, CL_Obj inherited_lex_env)
{
    CL_Compiler *comp;
    CL_CompEnv *env;
    CL_Obj result;

    comp = cl_compiler_pool_acquire();
    if (!comp) return inherited_lex_env;
    memset(comp, 0, sizeof(*comp));
    env = cl_env_create(NULL);
    comp->env = env;
    comp->parent = cl_active_compiler;
    comp->anchor = CL_CAPTURE_SP();
    cl_compiler_seed_optimize_settings(comp);
    cl_active_compiler = comp;

    /* Layer inherited bindings first, then this macrolet's expanders on top. */
    if (CL_CONS_P(inherited_lex_env))
        seed_env_from_lex_env(env, inherited_lex_env);
    cl_macrolet_install_expanders(env, bindings);

    result = cl_build_lex_env(env);

    cl_active_compiler = comp->parent;
    cl_env_destroy(env);
    cl_compiler_pool_release(comp);
    return result;
}

/* Like cl_macrolet_lex_env, but for SYMBOL-MACROLET bindings ((sym expansion) ...). */
CL_Obj cl_symbol_macrolet_lex_env(CL_Obj bindings, CL_Obj inherited_lex_env)
{
    CL_Compiler *comp;
    CL_CompEnv *env;
    CL_Obj result, b;

    comp = cl_compiler_pool_acquire();
    if (!comp) return inherited_lex_env;
    memset(comp, 0, sizeof(*comp));
    env = cl_env_create(NULL);
    comp->env = env;
    comp->parent = cl_active_compiler;
    comp->anchor = CL_CAPTURE_SP();
    cl_compiler_seed_optimize_settings(comp);
    cl_active_compiler = comp;

    if (CL_CONS_P(inherited_lex_env))
        seed_env_from_lex_env(env, inherited_lex_env);
    for (b = bindings; CL_CONS_P(b); b = cl_cdr(b)) {
        CL_Obj binding = cl_car(b);
        if (CL_CONS_P(binding))
            cl_env_add_symbol_macro(env, cl_car(binding), cl_car(cl_cdr(binding)));
    }

    result = cl_build_lex_env(env);

    cl_active_compiler = comp->parent;
    cl_env_destroy(env);
    cl_compiler_pool_release(comp);
    return result;
}

CL_Obj cl_compile_defun(CL_Obj name, CL_Obj lambda_list, CL_Obj body)
{
    CL_Obj form;
    /* GC SAFETY: build the form one cons at a time with the arguments
     * protected — a bare nested cl_cons(name, cl_cons(...)) evaluates the
     * inner cons first, which can compact and stale `name`/`lambda_list`
     * before the outer cons reads them. */
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(lambda_list);
    CL_GC_PROTECT(body);
    form = cl_cons(lambda_list, body);
    CL_GC_PROTECT(form);
    form = cl_cons(name, form);
    form = cl_cons(SYM_DEFUN, form);
    CL_GC_UNPROTECT(4);
    return cl_compile(form);
}

/* GC root walk over a thread's active compiler chain: every CL_Obj slot the
 * compilers hold in platform_alloc'd memory (not reachable from the arena).
 * Part of gc_visit_thread_roots (gc_trace.c): the marker instantiates it
 * with "mark the value", the compactor with "forward the slot in place",
 * so a slot listed here is both kept alive and rewritten — and one missing
 * here is neither.  Each slot must be visited EXACTLY once per walk: the
 * compactor's visitor is not idempotent (it forwards an arena offset
 * through the relocation map; a second forward of the same slot rewrites
 * it to an unrelated object — "Unbound variable: BOX<n>" when handler-case
 * was compiled under GC stress, back when the env chain was walked twice). */
void cl_compiler_gc_visit_thread(CL_Thread *t, void (*visit)(CL_Obj *))
{
    CL_Compiler *c = t->active_compiler;
    while (c) {
        int i;
        for (i = 0; i < c->const_count; i++)
            visit(&c->constants[i]);
        /* Block/tagbody tags and outer block/tag references */
        for (i = 0; i < c->block_count; i++)
            visit(&c->blocks[i].tag);
        for (i = 0; i < c->tagbody_count; i++) {
            int j;
            visit(&c->tagbodies[i].id);
            for (j = 0; j < c->tagbodies[i].n_tags; j++)
                visit(&c->tagbodies[i].tags[j].tag);
        }
        for (i = 0; i < c->notinline_count; i++)
            visit(&c->notinline_fns[i]);
        for (i = 0; i < c->outer_block_count; i++)
            visit(&c->outer_blocks[i]);
        for (i = 0; i < c->outer_tag_count; i++) {
            visit(&c->outer_tags[i].tag);
            visit(&c->outer_tags[i].tagbody_id);
        }
        /* Pending tail frames hold continuation forms (e.g. the ELSE
         * branch of an IF whose THEN is still compiling) that must
         * survive GC during the body's compilation. */
        for (i = 0; i < c->tail_count; i++)
            visit(&c->tail_stack[i].cont_form);
        /* Lambda list symbols — live from parse_lambda_list through compile_body;
         * any allocating call (compile_expr for defaults, determine_boxed_vars)
         * can trigger compaction that would leave these stale otherwise. */
        {
            int j;
            for (j = 0; j < c->ll.n_required; j++)
                visit(&c->ll.required[j]);
            for (j = 0; j < c->ll.n_optional; j++) {
                visit(&c->ll.opt_names[j]);
                visit(&c->ll.opt_defaults[j]);
                visit(&c->ll.opt_suppliedp[j]);
            }
            if (c->ll.has_rest)
                visit(&c->ll.rest_name);
            for (j = 0; j < c->ll.n_keys; j++) {
                visit(&c->ll.key_names[j]);
                visit(&c->ll.key_keywords[j]);
                visit(&c->ll.key_defaults[j]);
                visit(&c->ll.key_suppliedp[j]);
            }
            for (j = 0; j < c->ll.n_aux; j++) {
                visit(&c->ll.aux_names[j]);
                visit(&c->ll.aux_inits[j]);
            }
        }
        /* The compile-time environment (platform_alloc'd, holds CL_Obj
         * refs): ONLY this compiler's own env, NOT the env->parent chain.
         *
         * Each compiler owns exactly one env: cl_compile_env and compile_lambda
         * each call cl_env_create once, and the LET family (LET, LET-star, FLET,
         * LABELS) reuses the compiler's env (no cl_env_create).  compile_lambda
         * builds its inner env with cl_env_create(c->env), so inner->env->parent
         * == the parent compiler's env: the env->parent chain mirrors the
         * compiler->parent chain exactly.  The c = c->parent loop below visits
         * every ancestor compiler — hence every ancestor env — exactly once;
         * walking the full parent chain here would visit each ancestor env
         * once per descendant, i.e. forward it twice (see above). */
        if (c->env) {
            CL_CompEnv *env = c->env;
            for (i = 0; i < env->local_count; i++)
                visit(&env->locals[i]);
            for (i = 0; i < env->local_fun_count; i++) {
                visit(&env->local_funs[i].name);
                visit(&env->local_funs[i].inline_body);
                visit(&env->local_funs[i].inline_params);
            }
            for (i = 0; i < env->local_macro_count; i++) {
                visit(&env->local_macros[i].name);
                visit(&env->local_macros[i].expander);
            }
            for (i = 0; i < env->symbol_macro_count; i++) {
                visit(&env->symbol_macros[i].name);
                visit(&env->symbol_macros[i].expansion);
            }
        }
        c = c->parent;
    }
}

/* Stub expander for inlined special forms: takes (form env), returns form.
 * Registered so MACRO-FUNCTION and FBOUNDP return non-NIL for AND, OR, etc.
 * The identity check in compile_expr (expanded == form) falls through to the
 * compiler's fast path, so compilation is unaffected. */
static CL_Obj bi_inlined_macro_stub(CL_Obj *args, int nargs)
{
    (void)nargs;
    return args[0];
}

static void register_inlined_macro_stubs(void)
{
    CL_Obj stub_fn = CL_NIL;
    CL_Obj sym = CL_NIL;

    CL_GC_PROTECT(stub_fn);
    CL_GC_PROTECT(sym);

    /* The stub is registered as a NAMED AMILISP builtin (not an anonymous
     * cl_make_function) so the image-relink registry can resolve it after a
     * restore — an image's macro_table references this function object, and
     * a nameless function cannot be relinked to its C pointer (builtins.h). */
    {
        extern void cl_register_builtin(const char *, CL_CFunc, int, int,
                                        CL_Obj);
        extern CL_Obj cl_package_amilisp;
        cl_register_builtin("%INLINED-MACRO-STUB", bi_inlined_macro_stub,
                            2, 2, cl_package_amilisp);
        sym = cl_intern_in("%INLINED-MACRO-STUB",
                           (uint32_t)strlen("%INLINED-MACRO-STUB"),
                           cl_package_amilisp);
        stub_fn = ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->function;
    }

    cl_register_macro(SYM_AND,                stub_fn);
    cl_register_macro(SYM_OR,                 stub_fn);
    cl_register_macro(SYM_COND,               stub_fn);
    cl_register_macro(SYM_CASE,               stub_fn);
    cl_register_macro(SYM_ECASE,              stub_fn);
    cl_register_macro(SYM_TYPECASE,           stub_fn);
    cl_register_macro(SYM_ETYPECASE,          stub_fn);
    cl_register_macro(SYM_DEFUN,              stub_fn);
    cl_register_macro(SYM_DEFMACRO,           stub_fn);
    cl_register_macro(SYM_LAMBDA,             stub_fn);
    cl_register_macro(SYM_DEFVAR,             stub_fn);
    cl_register_macro(SYM_DEFPARAMETER,       stub_fn);
    cl_register_macro(SYM_DEFCONSTANT,        stub_fn);
    cl_register_macro(SYM_DECLAIM,            stub_fn);
    cl_register_macro(SYM_DO,                 stub_fn);
    cl_register_macro(SYM_DO_STAR,            stub_fn);
    cl_register_macro(SYM_DOLIST,             stub_fn);
    cl_register_macro(SYM_DOTIMES,            stub_fn);
    cl_register_macro(SYM_DESTRUCTURING_BIND, stub_fn);
    cl_register_macro(SYM_MULTIPLE_VALUE_BIND, stub_fn);
    cl_register_macro(SYM_MULTIPLE_VALUE_LIST, stub_fn);
    cl_register_macro(SYM_NTH_VALUE,          stub_fn);
    cl_register_macro(SYM_RETURN,             stub_fn);
    cl_register_macro(SYM_SETF,               stub_fn);
    cl_register_macro(SYM_DEFSETF,            stub_fn);
    cl_register_macro(SYM_DEFTYPE,            stub_fn);
    cl_register_macro(SYM_HANDLER_BIND,       stub_fn);
    cl_register_macro(SYM_RESTART_CASE,       stub_fn);
    cl_register_macro(SYM_TRACE,              stub_fn);
    cl_register_macro(SYM_UNTRACE,            stub_fn);
    cl_register_macro(SYM_TIME,               stub_fn);
    cl_register_macro(SYM_IN_PACKAGE,         stub_fn);

    /* Symbols without a SYM_* constant */
    sym = cl_intern_in("WITH-CONDITION-RESTARTS", 23, cl_package_cl);
    cl_register_macro(sym, stub_fn);
    sym = cl_intern_in("STEP", 4, cl_package_cl);
    cl_register_macro(sym, stub_fn);
    sym = cl_intern_in("FORMATTER", 9, cl_package_cl);
    cl_register_macro(sym, stub_fn);
    sym = cl_intern_in("WITH-PACKAGE-ITERATOR", 21, cl_package_cl);
    cl_register_macro(sym, stub_fn);

    CL_GC_UNPROTECT(2);
}

/* EXT:*CAPTURE-DOCUMENTATION* -- when NIL, DEFUN / DEFMACRO / DEFVAR /
 * DEFPARAMETER / DEFCONSTANT / DEFTYPE compile without their docstrings
 * (the compiler-side switch of specs/documentation-introspection.md).
 * Read at compile time, so binding it around COMPILE-FILE is what makes
 * a lean FASL; scripts/compile-lib-fasls.sh --no-docstrings does that
 * for the release.  Default T. */
static CL_Obj SYM_CAPTURE_DOCUMENTATION = CL_NIL;

int cl_capture_documentation_p(void)
{
    if (CL_NULL_P(SYM_CAPTURE_DOCUMENTATION)) return 1;
    return !CL_NULL_P(cl_symbol_value(SYM_CAPTURE_DOCUMENTATION));
}

static void capture_documentation_init(void)
{
    CL_Obj sym = cl_intern_in("*CAPTURE-DOCUMENTATION*", 23, cl_package_ext);
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    s->flags |= CL_SYM_SPECIAL;
    s->value = SYM_T;
    SYM_CAPTURE_DOCUMENTATION = sym;
    cl_gc_register_root(&SYM_CAPTURE_DOCUMENTATION);
    cl_export_symbol(sym, cl_package_ext);
}

void cl_compiler_init(void)
{
    macro_table = CL_NIL;
    setf_table = CL_NIL;
    type_table = CL_NIL;
    capture_documentation_init();
    cl_alist_index_reset(&type_index);
    cl_alist_index_reset(&setf_index);
    cl_alist_index_reset(&cmacro_index);
    compiler_macro_table = CL_NIL;
    /* These two were missing from the reset: after a shutdown/re-init
     * cycle (test harnesses) they still hold PREVIOUS-arena offsets, and
     * gc_mark marks them — setting "mark bits" at interior positions of
     * unrelated fresh-arena objects, i.e. silent corruption whose victim
     * moves with heap layout (see cl_mem_init's n_global_roots reset). */
    setf_fn_table = CL_NIL;
    setf_expander_table = CL_NIL;

    if (!cl_tables_rwlock)
        platform_rwlock_init(&cl_tables_rwlock);

    cl_compiler_pool_init();

    SETF_SYM_CAR             = cl_intern_in("CAR", 3, cl_package_cl);
    SETF_SYM_CDR             = cl_intern_in("CDR", 3, cl_package_cl);
    SETF_SYM_FIRST           = cl_intern_in("FIRST", 5, cl_package_cl);
    SETF_SYM_REST            = cl_intern_in("REST", 4, cl_package_cl);
    SETF_SYM_NTH             = cl_intern_in("NTH", 3, cl_package_cl);
    SETF_SYM_AREF            = cl_intern_in("AREF", 4, cl_package_cl);
    SETF_SYM_SVREF           = cl_intern_in("SVREF", 5, cl_package_cl);
    SETF_SYM_CHAR            = cl_intern_in("CHAR", 4, cl_package_cl);
    SETF_SYM_SCHAR           = cl_intern_in("SCHAR", 5, cl_package_cl);
    SETF_SYM_SYMBOL_VALUE    = cl_intern_in("SYMBOL-VALUE", 12, cl_package_cl);
    SETF_SYM_SYMBOL_FUNCTION = cl_intern_in("SYMBOL-FUNCTION", 15, cl_package_cl);
    SETF_SYM_SYMBOL_PLIST    = cl_intern_in("SYMBOL-PLIST", 12, cl_package_cl);
    SETF_SYM_FDEFINITION     = cl_intern_in("FDEFINITION", 11, cl_package_cl);
    SETF_HELPER_NTH          = cl_intern_in("%SETF-NTH", 9, cl_package_amilisp);
    SETF_HELPER_SV           = cl_intern_in("%SET-SYMBOL-VALUE", 17, cl_package_amilisp);
    SETF_HELPER_SF           = cl_intern_in("%SET-SYMBOL-FUNCTION", 20, cl_package_amilisp);
    SETF_HELPER_SP           = cl_intern_in("%SET-SYMBOL-PLIST", 17, cl_package_amilisp);
    SETF_SYM_GETHASH         = cl_intern_in("GETHASH", 7, cl_package_cl);
    SETF_HELPER_GETHASH      = cl_intern_in("%SETF-GETHASH", 13, cl_package_amilisp);
    SETF_HELPER_AREF         = cl_intern_in("%SETF-AREF", 10, cl_package_amilisp);
    SETF_SYM_ROW_MAJOR_AREF = cl_intern_in("ROW-MAJOR-AREF", 14, cl_package_cl);
    SETF_HELPER_ROW_MAJOR_AREF = cl_intern_in("%SETF-ROW-MAJOR-AREF", 20, cl_package_amilisp);
    SETF_SYM_FILL_POINTER    = cl_intern_in("FILL-POINTER", 12, cl_package_cl);
    SETF_HELPER_FILL_POINTER = cl_intern_in("%SETF-FILL-POINTER", 18, cl_package_amilisp);
    SETF_SYM_BIT             = cl_intern_in("BIT", 3, cl_package_cl);
    SETF_HELPER_BIT          = cl_intern_in("%SETF-BIT", 9, cl_package_amilisp);
    SETF_SYM_SBIT            = cl_intern_in("SBIT", 4, cl_package_cl);
    SETF_HELPER_SBIT         = cl_intern_in("%SETF-SBIT", 10, cl_package_amilisp);
    SETF_SYM_GET             = cl_intern_in("GET", 3, cl_package_cl);
    SETF_HELPER_GET          = cl_intern_in("%SETF-GET", 9, cl_package_amilisp);
    SETF_SYM_GETF            = cl_intern_in("GETF", 4, cl_package_cl);
    SETF_HELPER_GETF         = cl_intern_in("%SETF-GETF", 10, cl_package_amilisp);
    SYM_LEX_LOCAL_MACRO      = cl_intern_in("%LEX-LOCAL-MACRO", 16, cl_package_amilisp);
    symbol_macro_indicator   = cl_intern_in("%SYMBOL-MACRO-EXPANSION", 23,
                                            cl_package_amilisp);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&symbol_macro_indicator);
    cl_gc_register_root(&SETF_SYM_CAR);
    cl_gc_register_root(&SETF_SYM_CDR);
    cl_gc_register_root(&SETF_SYM_FIRST);
    cl_gc_register_root(&SETF_SYM_REST);
    cl_gc_register_root(&SETF_SYM_NTH);
    cl_gc_register_root(&SETF_SYM_AREF);
    cl_gc_register_root(&SETF_SYM_SVREF);
    cl_gc_register_root(&SETF_SYM_CHAR);
    cl_gc_register_root(&SETF_SYM_SCHAR);
    cl_gc_register_root(&SETF_SYM_SYMBOL_VALUE);
    cl_gc_register_root(&SETF_SYM_SYMBOL_FUNCTION);
    cl_gc_register_root(&SETF_SYM_SYMBOL_PLIST);
    cl_gc_register_root(&SETF_SYM_FDEFINITION);
    cl_gc_register_root(&SETF_HELPER_NTH);
    cl_gc_register_root(&SETF_HELPER_SV);
    cl_gc_register_root(&SETF_HELPER_SF);
    cl_gc_register_root(&SETF_HELPER_SP);
    cl_gc_register_root(&SETF_SYM_GETHASH);
    cl_gc_register_root(&SETF_HELPER_GETHASH);
    cl_gc_register_root(&SETF_HELPER_AREF);
    cl_gc_register_root(&SETF_SYM_ROW_MAJOR_AREF);
    cl_gc_register_root(&SETF_HELPER_ROW_MAJOR_AREF);
    cl_gc_register_root(&SETF_SYM_FILL_POINTER);
    cl_gc_register_root(&SETF_HELPER_FILL_POINTER);
    cl_gc_register_root(&SETF_SYM_BIT);
    cl_gc_register_root(&SETF_HELPER_BIT);
    cl_gc_register_root(&SETF_SYM_SBIT);
    cl_gc_register_root(&SETF_HELPER_SBIT);
    cl_gc_register_root(&SETF_SYM_GET);
    cl_gc_register_root(&SETF_HELPER_GET);
    cl_gc_register_root(&SETF_SYM_GETF);
    cl_gc_register_root(&SETF_HELPER_GETF);
    cl_gc_register_root(&SYM_LEX_LOCAL_MACRO);

    register_inlined_macro_stubs();
}

/* --- Compiler chain save/restore for NLX --- */

void *cl_compiler_mark(void)
{
    return (void *)cl_active_compiler;
}

/* NLX-landing unwind of the active-compiler chain.  Frees every compiler
 * pushed since the mark whose owning C-stack frame was abandoned by the
 * longjmp, and ONLY those:
 *
 *   - A compiler anchored strictly deeper than LANDING_ANCHOR (the landing
 *     site's own frame) was pushed by a frame the longjmp discarded — it can
 *     never be used again.  Leaving it on the chain would make every later
 *     "top-level" compile a child that inherits its stale optimize settings
 *     (DECLAIM/PROCLAIM appear dead for the rest of the session) and lets a
 *     later compile walk its freed env (heap-use-after-free).
 *
 *   - A compiler anchored at or shallower than LANDING_ANCHOR is owned by a
 *     C frame that is still live — e.g. an in-progress cl_compile_env /
 *     compile_lambda whose macroexpansion re-entered the VM and did a
 *     non-local exit that lands inside that nested VM run.  The walk stops
 *     there; the owner frees it on its normal path.  (A live owner is always
 *     shallower than the currently-executing landing frame, so the strict
 *     depth test can never free a live compiler.)
 *
 * The anchor test also bounds the walk when SAVED is stale: the mark is a
 * raw pointer and the compiler pool recycles addresses, so an old mark can
 * ambiguously match a newer compiler.  Chain order is monotonic in stack
 * depth (nested pushes come from nested calls), so the first
 * at-or-shallower anchor ends the abandoned region regardless of the mark.
 *
 * No optimize-settings backstop is needed here: the effective settings live
 * on the CL_Compiler structs just freed, not in a process-wide global — the
 * next fresh top-level compile re-seeds from cl_optimize_global via
 * cl_compiler_seed_optimize_settings. */
void cl_compiler_unwind_to(void *saved, void *landing_anchor)
{
    CL_Compiler *target = (CL_Compiler *)saved;
    while (cl_active_compiler != target && cl_active_compiler != NULL
           && (uintptr_t)cl_active_compiler->anchor < (uintptr_t)landing_anchor) {
        CL_Compiler *c = cl_active_compiler;
        cl_active_compiler = c->parent;
        if (c->env) cl_env_destroy(c->env);
        cl_compiler_pool_release(c);
    }
}

/* Like cl_compiler_unwind_to but without the anchor bound: frees everything
 * down to the mark.  Used on the error-frame (condition) unwind path: a
 * cl_error longjmp abandons every C stack frame between the signal point and
 * the C-level catch, so every compiler created in that window — including
 * ones a compile_lambda / cl_compile_env C frame was still using — is now
 * unreachable.  They MUST be freed (newest first, so a child env is released
 * before the parent env it points at), otherwise the active-compiler chain is
 * left dangling and a later compile walks a freed parent env
 * (heap-use-after-free in e.g. cl_env_resolve_fun_upvalue).  Walking is
 * bounded by the saved target, which predates every abandoned frame, so live
 * compilers below it are untouched. */
void cl_compiler_force_restore_to(void *saved)
{
    CL_Compiler *target = (CL_Compiler *)saved;
    while (cl_active_compiler != target && cl_active_compiler != NULL) {
        CL_Compiler *c = cl_active_compiler;
        cl_active_compiler = c->parent;
        if (c->env) cl_env_destroy(c->env);
        cl_compiler_pool_release(c);
    }
    /* See cl_compiler_unwind_to: no optimize-settings backstop needed —
     * the effective settings live on the freed CL_Compiler structs, not a
     * process-wide global. */
}
