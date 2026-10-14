#include "fasl.h"
#include "mem.h"
#include "symbol.h"
#include "package.h"
#include "error.h"
#include "float.h"
#include "vm.h"
#include "compiler.h"
#include "thread.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include "../jit/jit.h"
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================
 * Writer
 * ================================================================ */

void cl_fasl_writer_init(CL_FaslWriter *w, uint8_t *buf, uint32_t capacity)
{
    /* Sanity check: w must be a valid heap or stack pointer.
     * Detect corruption where w points into the binary's data segment
     * (happens when a non-volatile local survives longjmp with garbage). */
    if (!w) {
        cl_fatal_diag("[FASL] BUG: cl_fasl_writer_init called with NULL writer\n");
        abort();
    }
    w->data = buf;
    w->capacity = capacity;
    w->pos = 0;
    w->error = FASL_OK;
    w->gensym_count = 0;
    w->shared_count = 0;
    w->pad_ = 0;
    w->shared_objs = NULL;       /* lazily allocated on first use */
    w->shared_hash = NULL;       /* lazily allocated alongside shared_objs */
    w->shared_hash_cap = 0;
    w->nonportable_detail[0] = '\0';
}

void cl_fasl_writer_release(CL_FaslWriter *w)
{
    if (!w) return;
    if (w->shared_objs) {
        platform_free(w->shared_objs);
        w->shared_objs = NULL;
    }
    if (w->shared_hash) {
        platform_free(w->shared_hash);
        w->shared_hash = NULL;
    }
    w->shared_hash_cap = 0;
    w->shared_count = 0;
}

/* ================================================================
 * MAKE-LOAD-FORM writer support  (see fasl.h FASL_TAG_LOAD_FORM)
 * ================================================================
 *
 * Before a COMPILE-FILE run serializes its bytecode units, it calls
 * cl_fasl_mlf_prepass() over the collected bytecodes.  The pre-pass
 * walks the constant graph and, for every literal object whose class
 * has a user MAKE-LOAD-FORM method, calls amilisp::%FASL-LOAD-FORM to
 * obtain (creation-form . init-form) and records it.  fasl_ser_step's
 * TYPE_STRUCT case then emits FASL_TAG_LOAD_FORM for those objects
 * (looked up by EQ via cl_fasl_mlf_lookup) instead of dumping slots.
 *
 * Why a pre-pass and not an inline call?  %FASL-LOAD-FORM runs arbitrary
 * user code and allocates, so it can trigger compaction.  The
 * serializer's offset-keyed dedup tables (shared_objs / shared_hash)
 * and its work stack can't tolerate objects moving mid-serialization,
 * so ALL MAKE-LOAD-FORM calls happen here, up front — serialization
 * itself never allocates from the arena.  During the pre-pass the
 * walk's work/seen arrays and the result cache hold raw CL_Obj offsets,
 * so they are registered as GC roots (cl_fasl_gc_visit_mlf, mirroring the active-reader rooting).
 *
 * Single-threaded: like the DEBUG_FASL histogram globals, the writer
 * path assumes COMPILE-FILE is not run concurrently from multiple
 * threads, so a single file-static context is safe.
 *
 * Zero cost when unused: the pre-pass is gated on
 * amilisp::%MAKE-LOAD-FORM-ACTIVE-P — when no user MAKE-LOAD-FORM method
 * exists (the case for virtually every file), the graph walk is skipped
 * entirely and serialization behaves exactly as before. */

static uint32_t fasl_obj_hash(CL_Obj obj)
{
    /* Heap offsets are always CL_ALIGN-aligned (8-byte host / 4-byte
     * Amiga, see mem.h), so their low bits are always zero; shift those
     * out before applying Knuth's golden-ratio multiplier so every bit
     * of the result — including the low bits callers select via
     * `& mask` — carries entropy from the offset, instead of clustering
     * into a fraction of the table's slots. */
#if CL_ALIGN == 8
    return ((uint32_t)obj >> 3) * 2654435769u;
#else
    return ((uint32_t)obj >> 2) * 2654435769u;
#endif
}

/* Hash index over a backing CL_Obj array (seen[] / objs[] below).
 *
 * fasl_mlf_seen_p and cl_fasl_mlf_lookup were linear scans, which made
 * the pre-pass O(n²) in the number of unique heap objects reachable
 * from a file's constants — measured at ~85% of ALL cold-compile CPU
 * once any loaded library defines a MAKE-LOAD-FORM method (log4cl,
 * serapeum, cl-ppcre, local-time, ... all do, so in real sessions the
 * gate is effectively always open).  Each index is an open-addressing
 * table mapping CL_Obj -> position+1 in its backing array (0 = empty
 * slot), in the style of the writer's shared-object dedup table below.
 *
 * Compaction hazard: the backing arrays hold raw arena offsets that
 * cl_fasl_gc_visit_mlf (as the compactor instantiates it) rewrites when a %FASL-LOAD-FORM call triggers a
 * moving GC — invalidating every probe position keyed on the old bits.
 * The update hook therefore just marks the index dirty; it is rebuilt
 * lazily before the next probe.  Rebuilds use platform_alloc only
 * (never the arena), so a rebuild cannot itself trigger GC.
 *
 * OOM fallback: if a table allocation fails the index is disabled for
 * the rest of the pre-pass and callers fall back to the original
 * linear scan — slower, never wrong. */
typedef struct {
    uint32_t *slots;    /* position+1 into the backing array; 0 = empty */
    uint32_t  cap;      /* power of two; 0 = unallocated */
    int       dirty;    /* backing objects moved by compaction — rebuild */
    int       disabled; /* table allocation failed — linear fallback */
} FaslMlfIndex;

typedef struct {
    /* Result cache — persists through the whole serialize loop.  One
     * entry per literal object that has a MAKE-LOAD-FORM method. */
    CL_Obj  *objs;
    CL_Obj  *creations;
    CL_Obj  *inits;
    uint32_t count;
    uint32_t cap;
    /* Pre-pass-only graph-walk state (freed when the walk ends). */
    CL_Obj  *work;            /* worklist of objects still to walk */
    uint32_t work_count;
    uint32_t work_cap;
    CL_Obj  *seen;            /* container objects already walked (cycle break) */
    uint32_t seen_count;
    uint32_t seen_cap;
    int      active;          /* nonzero between prepass and cleanup */
    FaslMlfIndex seen_index;  /* hash index over seen[] (walk phase) */
    FaslMlfIndex objs_index;  /* hash index over objs[] (serialize phase) */
} FaslMlfState;

static FaslMlfState g_mlf;    /* zero-initialized; single-threaded compile-file */

static void fasl_mlf_index_free(FaslMlfIndex *ix)
{
    if (ix->slots) platform_free(ix->slots);
    ix->slots = NULL;
    ix->cap = 0;
    ix->dirty = 0;
    ix->disabled = 0;
}

/* (Re)build IX from ARR[0..count) (entries are unique by construction).
 * Returns 1 when the index is usable, 0 on OOM (index disabled). */
static int fasl_mlf_index_rebuild(FaslMlfIndex *ix, const CL_Obj *arr,
                                  uint32_t count)
{
    uint32_t cap = 64, k, mask;
    uint32_t *slots;
    while (cap < count * 2 + 1) cap <<= 1;
    slots = (uint32_t *)platform_alloc(cap * sizeof(uint32_t));
    if (!slots) {
        fasl_mlf_index_free(ix);
        ix->disabled = 1;
        return 0;
    }
    memset(slots, 0, cap * sizeof(uint32_t));
    mask = cap - 1;
    for (k = 0; k < count; k++) {
        uint32_t slot = fasl_obj_hash(arr[k]) & mask;
        while (slots[slot] != 0) slot = (slot + 1) & mask;
        slots[slot] = k + 1;
    }
    if (ix->slots) platform_free(ix->slots);
    ix->slots = slots;
    ix->cap = cap;
    ix->dirty = 0;
    return 1;
}

/* Ensure IX is allocated and current; 0 => caller must linear-scan. */
static int fasl_mlf_index_ready(FaslMlfIndex *ix, const CL_Obj *arr,
                                uint32_t count)
{
    if (ix->disabled) return 0;
    if (!ix->slots || ix->dirty)
        return fasl_mlf_index_rebuild(ix, arr, count);
    return 1;
}

/* Probe IX for OBJ; on hit store its ARR position in *POS and return 1.
 * Caller must have checked fasl_mlf_index_ready first. */
static int fasl_mlf_index_find(const FaslMlfIndex *ix, const CL_Obj *arr,
                               CL_Obj obj, uint32_t *pos)
{
    uint32_t mask = ix->cap - 1;
    uint32_t slot = fasl_obj_hash(obj) & mask;
    uint32_t e;
    while ((e = ix->slots[slot]) != 0) {
        if (arr[e - 1] == obj) {
            *pos = e - 1;
            return 1;
        }
        slot = (slot + 1) & mask;
    }
    return 0;
}

/* Register the just-appended ARR[count-1] in IX, growing (via rebuild)
 * when the new entry would push past 50% load.  Best-effort: on OOM the
 * index is disabled and lookups fall back to linear scans. */
static void fasl_mlf_index_add(FaslMlfIndex *ix, const CL_Obj *arr,
                               uint32_t count)
{
    uint32_t mask, slot;
    if (ix->disabled) return;
    if (!ix->slots || ix->dirty || count * 2 + 1 > ix->cap) {
        (void)fasl_mlf_index_rebuild(ix, arr, count);  /* inserts all */
        return;
    }
    mask = ix->cap - 1;
    slot = fasl_obj_hash(arr[count - 1]) & mask;
    while (ix->slots[slot] != 0) slot = (slot + 1) & mask;
    ix->slots[slot] = count;   /* position+1, position == count-1 */
}

/* Mirror of the metaobject test in fasl_ser_step's TYPE_STRUCT case:
 * STANDARD-CLASS / BUILT-IN-CLASS / FUNCALLABLE-STANDARD-CLASS structs
 * are cyclic CLOS metaobjects that serialize as FASL_TAG_CLASS_REF and
 * must never be treated as load-form candidates or walked into. */
static int fasl_struct_is_class_metaobject(CL_Obj obj)
{
    CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    CL_Symbol *td_sym;
    CL_String *td_nm;
    if (!CL_HEAP_P(st->type_desc) ||
        CL_HDR_TYPE(CL_OBJ_TO_PTR(st->type_desc)) != TYPE_SYMBOL)
        return 0;
    td_sym = (CL_Symbol *)CL_OBJ_TO_PTR(st->type_desc);
    td_nm  = (CL_String *)CL_OBJ_TO_PTR(td_sym->name);
    return (td_nm->length == 14 &&
            (memcmp(td_nm->data, "STANDARD-CLASS", 14) == 0 ||
             memcmp(td_nm->data, "BUILT-IN-CLASS", 14) == 0)) ||
           (td_nm->length == 26 &&
            memcmp(td_nm->data, "FUNCALLABLE-STANDARD-CLASS", 26) == 0);
}

/* Append v to a platform-allocated CL_Obj array, growing on demand.
 * Does not allocate from the arena, so it never triggers GC.  Returns 0
 * only on OOM (caller aborts the pre-pass). */
static int fasl_mlf_arr_push(CL_Obj **arr, uint32_t *count, uint32_t *cap,
                             CL_Obj v)
{
    if (*count >= *cap) {
        uint32_t ncap = *cap ? *cap * 2 : 64;
        CL_Obj *n = (CL_Obj *)platform_alloc(ncap * sizeof(CL_Obj));
        if (!n) return 0;
        if (*arr) {
            memcpy(n, *arr, (*count) * sizeof(CL_Obj));
            platform_free(*arr);
        }
        *arr = n;
        *cap = ncap;
    }
    (*arr)[(*count)++] = v;
    return 1;
}

/* Append (obj, creation, init) to the parallel result-cache arrays,
 * growing all three together.  No arena allocation, so GC-safe. */
static int fasl_mlf_cache_push(CL_Obj obj, CL_Obj creation, CL_Obj init)
{
    if (g_mlf.count >= g_mlf.cap) {
        uint32_t ncap = g_mlf.cap ? g_mlf.cap * 2 : 16;
        CL_Obj *no = (CL_Obj *)platform_alloc(ncap * sizeof(CL_Obj));
        CL_Obj *nc = (CL_Obj *)platform_alloc(ncap * sizeof(CL_Obj));
        CL_Obj *ni = (CL_Obj *)platform_alloc(ncap * sizeof(CL_Obj));
        if (!no || !nc || !ni) {
            if (no) platform_free(no);
            if (nc) platform_free(nc);
            if (ni) platform_free(ni);
            return 0;
        }
        if (g_mlf.objs) {
            memcpy(no, g_mlf.objs,      g_mlf.count * sizeof(CL_Obj));
            memcpy(nc, g_mlf.creations, g_mlf.count * sizeof(CL_Obj));
            memcpy(ni, g_mlf.inits,     g_mlf.count * sizeof(CL_Obj));
            platform_free(g_mlf.objs);
            platform_free(g_mlf.creations);
            platform_free(g_mlf.inits);
        }
        g_mlf.objs = no;
        g_mlf.creations = nc;
        g_mlf.inits = ni;
        g_mlf.cap = ncap;
    }
    g_mlf.objs[g_mlf.count] = obj;
    g_mlf.creations[g_mlf.count] = creation;
    g_mlf.inits[g_mlf.count] = init;
    g_mlf.count++;
    fasl_mlf_index_add(&g_mlf.objs_index, g_mlf.objs, g_mlf.count);
    return 1;
}

/* True if a container object has already been enqueued/walked. */
static int fasl_mlf_seen_p(CL_Obj obj)
{
    uint32_t k, pos;
    if (fasl_mlf_index_ready(&g_mlf.seen_index, g_mlf.seen,
                             g_mlf.seen_count))
        return fasl_mlf_index_find(&g_mlf.seen_index, g_mlf.seen, obj, &pos);
    for (k = 0; k < g_mlf.seen_count; k++)
        if (g_mlf.seen[k] == obj) return 1;
    return 0;
}

/* Push OBJ onto seen[] and register it in the seen index.  Returns 0
 * only on OOM of the array itself (caller aborts the walk). */
static int fasl_mlf_seen_push(CL_Obj obj)
{
    if (!fasl_mlf_arr_push(&g_mlf.seen, &g_mlf.seen_count,
                           &g_mlf.seen_cap, obj))
        return 0;
    fasl_mlf_index_add(&g_mlf.seen_index, g_mlf.seen, g_mlf.seen_count);
    return 1;
}

/* Look up OBJECT's cached load form.  Hot path: called for every struct
 * the serializer emits.  When no MAKE-LOAD-FORM methods exist the cache
 * is empty and this returns immediately. */
static int cl_fasl_mlf_lookup(CL_Obj obj, CL_Obj *creation, CL_Obj *init)
{
    uint32_t k, pos;
    if (g_mlf.count == 0) return 0;
    if (fasl_mlf_index_ready(&g_mlf.objs_index, g_mlf.objs, g_mlf.count)) {
        if (!fasl_mlf_index_find(&g_mlf.objs_index, g_mlf.objs, obj, &pos))
            return 0;
        *creation = g_mlf.creations[pos];
        *init = g_mlf.inits[pos];
        return 1;
    }
    for (k = 0; k < g_mlf.count; k++) {
        if (g_mlf.objs[k] == obj) {
            *creation = g_mlf.creations[k];
            *init = g_mlf.inits[k];
            return 1;
        }
    }
    return 0;
}

/* GC root walk: the walk arrays and the result cache hold raw arena
 * offsets that a collection during %FASL-LOAD-FORM must keep alive and a
 * compaction must forward.  Part of gc_visit_shared_roots (gc_trace.c),
 * called with the world stopped. */
void cl_fasl_gc_visit_mlf(void (*visit)(CL_Obj *))
{
    uint32_t k;
    if (!g_mlf.active) return;
    for (k = 0; k < g_mlf.count; k++) {
        visit(&g_mlf.objs[k]);
        visit(&g_mlf.creations[k]);
        visit(&g_mlf.inits[k]);
    }
    for (k = 0; k < g_mlf.work_count; k++) visit(&g_mlf.work[k]);
    for (k = 0; k < g_mlf.seen_count; k++) visit(&g_mlf.seen[k]);
}

/* The compactor rewrote the entries above: they hash differently now —
 * invalidate the indices; they rebuild lazily on the next probe. */
void cl_fasl_mlf_gc_relocated(void)
{
    if (!g_mlf.active) return;
    g_mlf.seen_index.dirty = 1;
    g_mlf.objs_index.dirty = 1;
}

/* Free all pre-pass state and reset.  Safe to call repeatedly. */
void cl_fasl_mlf_cleanup(void)
{
    if (g_mlf.objs)      platform_free(g_mlf.objs);
    if (g_mlf.creations) platform_free(g_mlf.creations);
    if (g_mlf.inits)     platform_free(g_mlf.inits);
    if (g_mlf.work)      platform_free(g_mlf.work);
    if (g_mlf.seen)      platform_free(g_mlf.seen);
    fasl_mlf_index_free(&g_mlf.seen_index);
    fasl_mlf_index_free(&g_mlf.objs_index);
    memset(&g_mlf, 0, sizeof(g_mlf));
}

/* Free the walk-only arrays once the walk is finished, keeping the
 * result cache for the serialize phase. */
static void fasl_mlf_free_walk_state(void)
{
    if (g_mlf.work) { platform_free(g_mlf.work); g_mlf.work = NULL; }
    if (g_mlf.seen) { platform_free(g_mlf.seen); g_mlf.seen = NULL; }
    g_mlf.work_count = g_mlf.work_cap = 0;
    g_mlf.seen_count = g_mlf.seen_cap = 0;
    fasl_mlf_index_free(&g_mlf.seen_index);
}

/* Resolve a CLAMIGA-package function by name; CL_NIL if undefined
 * (e.g. CLOS not loaded — the pre-pass then no-ops). */
static CL_Obj fasl_mlf_amilisp_fn(const char *name, int len)
{
    CL_Obj sym = cl_intern_in(name, len, cl_package_amilisp);
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    return s->function;
}

int cl_fasl_mlf_active_p(void)
{
    return g_mlf.active;
}

/* Enqueue OBJ for walking if it is a heap object. */
static int fasl_mlf_enqueue(CL_Obj obj)
{
    if (!CL_HEAP_P(obj)) return 1;
    return fasl_mlf_arr_push(&g_mlf.work, &g_mlf.work_count, &g_mlf.work_cap, obj);
}

/* Pre-pass entry point: walk the constant graph of the BC_COUNT bytecode
 * units in BC_VEC (a Lisp vector) and populate the load-form cache.
 * Leaves g_mlf.active = 1 and the cache live for the serialize phase;
 * the caller must invoke cl_fasl_mlf_cleanup() once serialization is
 * done.  GC-safe: %FASL-LOAD-FORM may compact, and all live walk state
 * is GC-rooted via cl_fasl_gc_visit_mlf. */
void cl_fasl_mlf_prepass(CL_Obj bc_vec, int bc_count)
{
    CL_Obj active_fn, fasl_fn;
    CL_Vector *bcv;
    int i;

    cl_fasl_mlf_cleanup();
    g_mlf.active = 1;

    /* bc_vec is dereferenced after the %MAKE-LOAD-FORM-ACTIVE-P apply,
     * and fasl_fn is applied for EVERY MLF candidate below — the first
     * user method's allocations would leave both stale (the caller's own
     * protect covers the caller's variable, not these parameter copies).
     * Root them for the whole prepass; every exit path unprotects. */
    CL_GC_PROTECT(bc_vec);

    /* Gate: only walk when MAKE-LOAD-FORM has at least one user method. */
    active_fn = fasl_mlf_amilisp_fn("%MAKE-LOAD-FORM-ACTIVE-P", 24);
    if (CL_NULL_P(active_fn) || active_fn == CL_UNBOUND) {
        CL_GC_UNPROTECT(1);
        return;  /* CLOS / helper not loaded */
    }
    if (CL_NULL_P(cl_vm_apply(active_fn, NULL, 0))) {
        CL_GC_UNPROTECT(1);
        return;
    }

    fasl_fn = fasl_mlf_amilisp_fn("%FASL-LOAD-FORM", 15);
    if (CL_NULL_P(fasl_fn) || fasl_fn == CL_UNBOUND) {
        CL_GC_UNPROTECT(1);
        return;
    }
    CL_GC_PROTECT(fasl_fn);

    if (CL_NULL_P(bc_vec) || !CL_HEAP_P(bc_vec)) goto done;
    bcv = (CL_Vector *)CL_OBJ_TO_PTR(bc_vec);
    for (i = 0; i < bc_count && (uint32_t)i < bcv->length; i++) {
        /* Re-fetch bcv each iteration — fasl_mlf_enqueue never allocates
         * from the arena, so no compaction here, but keep it robust. */
        bcv = (CL_Vector *)CL_OBJ_TO_PTR(bc_vec);
        if (!fasl_mlf_enqueue(cl_vector_data(bcv)[i])) goto done;
    }

    while (g_mlf.work_count > 0) {
        CL_Obj obj = g_mlf.work[--g_mlf.work_count];
        uint8_t htype;
        if (!CL_HEAP_P(obj)) continue;
        htype = CL_HDR_TYPE(CL_OBJ_TO_PTR(obj));

        switch (htype) {
        case TYPE_CONS: {
            if (fasl_mlf_seen_p(obj)) break;
            if (!fasl_mlf_seen_push(obj)) goto done;
            if (!fasl_mlf_enqueue(cl_car(obj))) goto done;
            if (!fasl_mlf_enqueue(cl_cdr(obj))) goto done;
            break;
        }
        case TYPE_VECTOR: {
            CL_Vector *v;
            uint32_t k;
            if (fasl_mlf_seen_p(obj)) break;
            if (!fasl_mlf_seen_push(obj)) goto done;
            v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
            for (k = 0; k < v->length; k++)
                if (!fasl_mlf_enqueue(cl_vector_data(v)[k])) goto done;
            break;
        }
        case TYPE_BYTECODE: {
            CL_Bytecode *bc;
            uint16_t k;
            if (fasl_mlf_seen_p(obj)) break;
            if (!fasl_mlf_seen_push(obj)) goto done;
            bc = (CL_Bytecode *)CL_OBJ_TO_PTR(obj);
            for (k = 0; k < bc->n_constants; k++)
                if (!fasl_mlf_enqueue(bc->constants[k])) goto done;
            for (k = 0; k < bc->n_keys; k++)
                if (!fasl_mlf_enqueue(bc->key_syms[k])) goto done;
            if (!fasl_mlf_enqueue(bc->name)) goto done;
            break;
        }
        case TYPE_CLOSURE: {
            CL_Closure *cl_obj;
            uint32_t n_upvalues, k, alloc_size;
            if (fasl_mlf_seen_p(obj)) break;
            if (!fasl_mlf_seen_push(obj)) goto done;
            cl_obj = (CL_Closure *)CL_OBJ_TO_PTR(obj);
            if (!fasl_mlf_enqueue(cl_obj->bytecode)) goto done;
            alloc_size = CL_HDR_SIZE(cl_obj);
            n_upvalues = (alloc_size - sizeof(CL_Closure)) / sizeof(CL_Obj);
            for (k = 0; k < n_upvalues; k++)
                if (!fasl_mlf_enqueue(cl_obj->upvalues[k])) goto done;
            break;
        }
        case TYPE_STRUCT: {
            CL_Obj forms = CL_NIL, arg;
            uint32_t seen_idx;
            int sf, ss, sn, sroots, err;
            if (fasl_mlf_seen_p(obj)) break;
            if (!fasl_mlf_seen_push(obj)) goto done;
            seen_idx = g_mlf.seen_count - 1;   /* obj's stable, GC-rooted slot */
            /* CLOS metaobjects serialize as CLASS_REF — never load forms,
             * and their cyclic slots must not be walked. */
            if (fasl_struct_is_class_metaobject(obj)) break;

            /* %FASL-LOAD-FORM runs an arbitrary user method.  obj is held
             * across its (compacting) execution via the GC-rooted seen[]
             * slot, re-read afterwards — no CL_GC_PROTECT, so a longjmp out
             * of a faulty method can't leak a protect entry.  An error is
             * treated as "no load form": obj falls back to the built-in
             * structure dump (a valid instance on load), and we keep going. */
            sf = cl_vm.fp; ss = cl_vm.sp; sn = cl_nlx_top;
            sroots = gc_root_count;
            arg = g_mlf.seen[seen_idx];
            CL_CATCH(err);
            if (err == CL_ERR_NONE) {
                forms = cl_vm_apply(fasl_fn, &arg, 1);
                CL_UNCATCH();
            } else {
                /* Restore VM/GC state leaked by the aborted method. */
                cl_vm.fp = sf; cl_vm.sp = ss; cl_nlx_top = sn;
                gc_root_count = sroots;
                CL_UNCATCH();
                forms = CL_NIL;
            }
            obj = g_mlf.seen[seen_idx];        /* forwarded across compaction */

            if (CL_HEAP_P(forms) &&
                CL_HDR_TYPE(CL_OBJ_TO_PTR(forms)) == TYPE_CONS) {
                CL_Obj creation = cl_car(forms);
                CL_Obj init = cl_cdr(forms);
                if (!fasl_mlf_cache_push(obj, creation, init)) goto done;
                /* Recurse into the load forms — their literals may include
                 * further objects that need MAKE-LOAD-FORM treatment. */
                if (!fasl_mlf_enqueue(creation)) goto done;
                if (!fasl_mlf_enqueue(init)) goto done;
            } else {
                /* No method: recurse into slots — a plain struct/cons can
                 * still contain an MLF object deeper down. */
                CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
                uint32_t k;
                if (!fasl_mlf_enqueue(st->type_desc)) goto done;
                for (k = 0; k < st->n_slots; k++) {
                    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);  /* re-fetch */
                    if (!fasl_mlf_enqueue(st->slots[k])) goto done;
                }
            }
            break;
        }
        default:
            /* Leaf (symbol, string, number, package, pathname, lock, …):
             * cannot transitively reach an MLF object. */
            break;
        }
    }

done:
    CL_GC_UNPROTECT(2);   /* bc_vec, fasl_fn */
    fasl_mlf_free_walk_state();
}

void cl_fasl_write_u8(CL_FaslWriter *w, uint8_t val)
{
    if (w->error) return;
    if (w->pos + 1 > w->capacity) { w->error = FASL_ERR_OVERFLOW; return; }
    w->data[w->pos++] = val;
}

void cl_fasl_write_u16(CL_FaslWriter *w, uint16_t val)
{
    if (w->error) return;
    if (w->pos + 2 > w->capacity) { w->error = FASL_ERR_OVERFLOW; return; }
    w->data[w->pos++] = (uint8_t)(val >> 8);
    w->data[w->pos++] = (uint8_t)(val);
}

void cl_fasl_write_u32(CL_FaslWriter *w, uint32_t val)
{
    if (w->error) return;
    if (w->pos + 4 > w->capacity) { w->error = FASL_ERR_OVERFLOW; return; }
    w->data[w->pos++] = (uint8_t)(val >> 24);
    w->data[w->pos++] = (uint8_t)(val >> 16);
    w->data[w->pos++] = (uint8_t)(val >> 8);
    w->data[w->pos++] = (uint8_t)(val);
}

void cl_fasl_write_bytes(CL_FaslWriter *w, const void *data, uint32_t len)
{
    if (w->error) return;
    if (w->pos + len > w->capacity) { w->error = FASL_ERR_OVERFLOW; return; }
    memcpy(w->data + w->pos, data, len);
    w->pos += len;
}

/* Double floats are stored big-endian on disk regardless of host byte order.
 * These two helpers fold the identical host-endianness detect-and-swap dance
 * that the DOUBLE_FLOAT writer and reader each carried inline.  Byte layout on
 * disk is unchanged, so no CL_FASL_VERSION bump is required. */
static void fasl_double_to_be(double v, uint8_t out[8])
{
    union { double d; uint8_t b[8]; } u;
    memcpy(out, &v, 8);
    u.d = 1.0;
    if (u.b[0] != 0x3F) {          /* little-endian host: reverse to big-endian */
        uint8_t rev[8];
        int i;
        for (i = 0; i < 8; i++) rev[i] = out[7 - i];
        memcpy(out, rev, 8);
    }
}

static double fasl_double_from_be(const uint8_t in[8])
{
    union { double d; uint8_t b[8]; } u;
    double d;
    u.d = 1.0;
    if (u.b[0] == 0x3F) {          /* big-endian host */
        memcpy(&d, in, 8);
    } else {                       /* little-endian host: reverse */
        uint8_t rev[8];
        int i;
        for (i = 0; i < 8; i++) rev[i] = in[7 - i];
        memcpy(&d, rev, 8);
    }
    return d;
}

void cl_fasl_write_header(CL_FaslWriter *w, uint32_t n_units)
{
    cl_fasl_write_u32(w, CL_FASL_MAGIC);
    cl_fasl_write_u16(w, CL_FASL_VERSION);
    cl_fasl_write_u16(w, 0);  /* flags */
    cl_fasl_write_u32(w, n_units);
}

/* --- Shared-object dedup hash table (writer side) ---
 *
 * Earlier the dedup set was a plain array scanned linearly on every
 * lookup — O(N) per query, O(N²) total over a serialize.  That was fine
 * when only a few hundred objects (closures + bytecodes + structs +
 * symbols + locks) reached the table.  Adding cons cells brings the
 * realistic working set into the tens of thousands, so we route lookups
 * through a hash table mapping CL_Obj -> id+1 (0 = empty slot).  Open
 * addressing with linear probing keeps the implementation small and
 * cache-friendly; load factor capped at 50%.
 *
 * Sizing: starts at 64 slots (128 B), grows by doubling, capped at
 * 131072 slots (256 KB) — twice FASL_MAX_SHARED so the hash never
 * reaches saturation while shared_count still has room.
 *
 * Failure handling: if any allocation along the way fails, the helper
 * returns -1.  The caller then silently skips dedup for this object —
 * the wire output is still correct (objects appear inline rather than
 * as OBJ_REF), only fatter.  This matches the prior behaviour where
 * a failed shared_objs alloc disabled dedup. */
#define FASL_SHARED_HASH_INIT_CAP 64u
#define FASL_SHARED_HASH_MAX_CAP  131072u

/* fasl_obj_hash is defined above the MAKE-LOAD-FORM pre-pass state,
 * which shares it. */

/* Resize shared_hash to new_cap (a power of two), rehashing existing
 * entries from shared_objs[0..shared_count).  Returns 1 on success, 0
 * on allocation failure (in which case the existing table is kept). */
static int fasl_shared_hash_resize(CL_FaslWriter *w, uint32_t new_cap)
{
    uint16_t *new_table;
    uint32_t mask;
    uint16_t i;

    new_table = (uint16_t *)platform_alloc(new_cap * sizeof(uint16_t));
    if (!new_table) return 0;
    memset(new_table, 0, new_cap * sizeof(uint16_t));
    mask = new_cap - 1;
    for (i = 0; i < w->shared_count; i++) {
        CL_Obj eo = w->shared_objs[i];
        uint32_t slot = fasl_obj_hash(eo) & mask;
        while (new_table[slot] != 0)
            slot = (slot + 1) & mask;
        new_table[slot] = (uint16_t)(i + 1);
    }
    if (w->shared_hash) platform_free(w->shared_hash);
    w->shared_hash = new_table;
    w->shared_hash_cap = new_cap;
    return 1;
}

/* Find or insert obj in the dedup table.
 *
 * Returns:
 *   1  hit  — obj was already present; *out_id receives its id
 *   0  miss — obj newly inserted at id *out_id (caller emits OBJ_DEF)
 *  -1  unable to dedup (allocation failed or table full); caller should
 *      emit the body inline without wrapping it in OBJ_DEF. */
static int fasl_shared_get_or_insert(CL_FaslWriter *w, CL_Obj obj,
                                     uint16_t *out_id)
{
    uint32_t mask, slot;
    uint16_t entry;

    if (!w->shared_objs) {
        w->shared_objs = (CL_Obj *)platform_alloc(
            FASL_MAX_SHARED * sizeof(CL_Obj));
        if (!w->shared_objs) return -1;
    }
    if (!w->shared_hash) {
        w->shared_hash_cap = FASL_SHARED_HASH_INIT_CAP;
        w->shared_hash = (uint16_t *)platform_alloc(
            w->shared_hash_cap * sizeof(uint16_t));
        if (!w->shared_hash) {
            w->shared_hash_cap = 0;
            return -1;
        }
        memset(w->shared_hash, 0, w->shared_hash_cap * sizeof(uint16_t));
    }

    /* Grow eagerly: the new entry would push us past 50% load.  If grow
     * fails we keep using the old table — linear probing still terminates
     * because the table never reaches FASL_MAX_SHARED entries (the table
     * is always >= 2*shared_count by construction, except when we're at
     * the cap; at that point we refuse new entries below). */
    if ((uint32_t)(w->shared_count + 1) > w->shared_hash_cap / 2 &&
        w->shared_hash_cap < FASL_SHARED_HASH_MAX_CAP) {
        uint32_t new_cap = w->shared_hash_cap * 2;
        if (new_cap > FASL_SHARED_HASH_MAX_CAP)
            new_cap = FASL_SHARED_HASH_MAX_CAP;
        (void)fasl_shared_hash_resize(w, new_cap);
        /* If resize fails, fall through and use the smaller table. */
    }

    mask = w->shared_hash_cap - 1;
    slot = fasl_obj_hash(obj) & mask;
    while ((entry = w->shared_hash[slot]) != 0) {
        if (w->shared_objs[entry - 1] == obj) {
            *out_id = (uint16_t)(entry - 1);
            return 1;  /* hit */
        }
        slot = (slot + 1) & mask;
    }

    if (w->shared_count >= FASL_MAX_SHARED) {
        /* Table is full — the wire format limits ids to u16.  Skip dedup
         * for this object; it will be emitted inline. */
        return -1;
    }
    w->shared_objs[w->shared_count] = obj;
    w->shared_hash[slot] = (uint16_t)(w->shared_count + 1);
    *out_id = w->shared_count;
    w->shared_count++;
    return 0;
}

/* Lookup-only probe of the dedup table — never inserts.  Returns 1 and
 * sets *out_id when obj is already registered, 0 otherwise.  Used by the
 * inline CDR-spine walk to detect a cons that has already been serialized
 * (a circular list whose tail loops back to an enclosing/earlier cons, or
 * a structurally-shared tail) so it can emit an OBJ_REF and stop instead
 * of walking the cycle forever.  Unlike get_or_insert this does not
 * allocate a new id for misses — cdr cells stay un-deduped on the wire
 * (no per-cell OBJ_DEF bloat), so acyclic lists serialize byte-for-byte
 * as before. */
static int fasl_shared_lookup(CL_FaslWriter *w, CL_Obj obj, uint16_t *out_id)
{
    uint32_t mask, slot;
    uint16_t entry;

    if (!w->shared_objs || !w->shared_hash || w->shared_hash_cap == 0)
        return 0;
    mask = w->shared_hash_cap - 1;
    slot = fasl_obj_hash(obj) & mask;
    while ((entry = w->shared_hash[slot]) != 0) {
        if (w->shared_objs[entry - 1] == obj) {
            *out_id = (uint16_t)(entry - 1);
            return 1;
        }
        slot = (slot + 1) & mask;
    }
    return 0;
}

/* --- Serialize a CL_Obj constant (iterative) ---
 *
 * Serialization is a pre-order tree walk over the constant graph.  An
 * earlier recursive implementation overflowed the C stack on deeply-nested
 * graphs (lparallel/cognate/psort.lisp, sento dispatch chains): each
 * recursion level cost ~150-300B, and inputs >4096 deep tripped a stack-
 * protector canary in bi_compile_file or crashed outright.
 *
 * The iterative driver below uses an explicit, heap-allocated worklist of
 * frames.  Each frame represents one in-progress object; it carries a
 * phase counter and a child index so a frame can yield to a child mid-way
 * and resume after the child completes.  Worklist capacity grows by
 * doubling, capped at FASL_SER_STACK_MAX_CAP frames.  The C stack stays
 * shallow and bounded regardless of input depth.
 *
 * GC safety: the serializer makes no allocations (only memcpys into the
 * pre-allocated writer buffer + read-only package lookups).  Therefore GC
 * never runs during a serialize call, and CL_Obj values held on the
 * worklist do not need explicit GC protection — they remain reachable
 * from their parents which are themselves on the worklist or the C stack.
 */

/* Worklist sizing: 256 initial capacity (doubles on demand), hard-capped
 * at 2M frames (~24 MB at 12B/frame including padding).  No realistic
 * input should ever come close — this only guards against runaway
 * graphs (e.g. accidental cycles in non-shared types). */
#define FASL_SER_STACK_INIT_CAP 256
#define FASL_SER_STACK_MAX_CAP  (1u << 21)

/* Frame phase tags.  Leaf types finish in PHASE_START with no children.
 * Composite types write their header in PHASE_START, push children in
 * reverse, set phase to PHASE_DONE, and yield (return 0).  When children
 * drain and the parent frame becomes top again, PHASE_DONE returns 1
 * so the driver pops it.  This dance is needed because the driver pops
 * the top frame on "done", and after pushing children the parent is no
 * longer on top — only the eventual re-entry at PHASE_DONE pops correctly.
 * BYTECODE/CLOSURE/STRUCT use additional phases to interleave header
 * bytes between children. */
#define PHASE_START          0  /* dispatch on type, write header, push children */
#define PHASE_BC_AFTER_TAG   1  /* code prefix written next */
#define PHASE_BC_CONSTANTS   2  /* constants iteration */
#define PHASE_BC_METADATA    3  /* metadata bytes */
#define PHASE_BC_KEY_SYMS    4  /* key_syms iteration */
#define PHASE_BC_POSTLUDE    5  /* key_slots, source info, line map; push lambda-list */
#define PHASE_BC_LAMBDA_LIST 6  /* source_lambda_list pushed; then push name */
#define PHASE_CLOSURE_AFTER_BC    0xF0  /* write n_upvalues, transition to NEXT_UPVAL */
#define PHASE_STRUCT_AFTER_TYPEDESC 0xF1 /* write n_slots, transition to NEXT_SLOT */
#define PHASE_CONS_NEXT_CAR       0xF2  /* iterating CDR chain inline */
#define PHASE_VECTOR_NEXT         0xF3  /* push one element per step (index = cursor) */
#define PHASE_STRUCT_NEXT_SLOT    0xF4  /* push one slot per step */
#define PHASE_CLOSURE_NEXT_UPVAL  0xF5  /* push one upvalue per step */
#define PHASE_DONE          0xFF  /* sentinel: return 1, frame is popped */

#ifdef DEBUG_FASL
/* Forward decls for histogram state — definitions live next to the drive
 * loop further down.  Declared up here so the OBJ_REF / CONST_REF emission
 * sites in fasl_ser_step can bump the dedup-hit counters. */
static int      g_fasl_hist_enabled;
static uint32_t g_fasl_dedup_obj_ref;
static uint32_t g_fasl_dedup_const_ref;
#endif

typedef struct {
    CL_Obj   obj;
    uint32_t index;  /* child cursor for arrays (constants, slots, vector elts) */
    uint8_t  phase;
    uint8_t  pad[3];
} FaslSerFrame;

typedef struct {
    FaslSerFrame *frames;
    uint32_t      depth;
    uint32_t      capacity;
} FaslSerStack;

static int fasl_ser_stack_push(CL_FaslWriter *w, FaslSerStack *s,
                               CL_Obj obj, uint8_t phase)
{
    if (s->depth == s->capacity) {
        uint32_t new_cap = s->capacity ? s->capacity * 2 : FASL_SER_STACK_INIT_CAP;
        FaslSerFrame *nf;
        if (new_cap > FASL_SER_STACK_MAX_CAP) {
#ifdef DEBUG_FASL_DEEP
            {
                /* Diagnostic dump on hitting the worklist cap.  The cap is
                 * deliberately huge (2M frames ~= 24MB) — anything that
                 * trips it is almost certainly an undetected cycle in the
                 * constant graph, not legitimate depth.  Histogram + top
                 * frames usually identify the cyclic type.
                 *
                 * fflush after every fprintf — when stderr is redirected
                 * to a file (block-buffered) and the process crashes
                 * shortly after, unflushed buffer is lost.  This
                 * diagnostic exists *because* something is wrong, so
                 * paying for the flushes is fine. */
                uint32_t i;
                uint32_t type_counts[64] = {0};
                int top_t = CL_HEAP_P(obj) ? CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) : -1;
                int parent_t = -1;
                CL_Obj parent_obj = CL_NIL;
                if (s->depth > 0) {
                    parent_obj = s->frames[s->depth - 1].obj;
                    if (CL_HEAP_P(parent_obj))
                        parent_t = CL_HDR_TYPE(CL_OBJ_TO_PTR(parent_obj));
                }
                fprintf(stderr,
                  "[FASL] hit FASL_SER_STACK_MAX_CAP=%u depth=%u\n"
                  "[FASL]   pushing  obj=0x%08x phase=%u type=%d\n"
                  "[FASL]   top-of-stack obj=0x%08x type=%d\n",
                  FASL_SER_STACK_MAX_CAP, s->depth,
                  (unsigned)obj, phase, top_t,
                  (unsigned)parent_obj, parent_t);
                fflush(stderr);
                if (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_STRUCT) {
                    CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
                    fprintf(stderr, "[FASL] pushed obj is STRUCT type_desc=0x%08x n_slots=%u\n",
                            (unsigned)st->type_desc, (unsigned)st->n_slots);
                    fflush(stderr);
                    if (CL_HEAP_P(st->type_desc) &&
                        CL_HDR_TYPE(CL_OBJ_TO_PTR(st->type_desc)) == TYPE_SYMBOL) {
                        CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(st->type_desc);
                        CL_String *nm = (CL_String *)CL_OBJ_TO_PTR(sym->name);
                        fprintf(stderr, "[FASL] type_desc symbol name=%.*s\n",
                                (int)nm->length, nm->data);
                        fflush(stderr);
                    }
                }
                for (i = 0; i < s->depth; i++) {
                    CL_Obj fobj = s->frames[i].obj;
                    if (CL_HEAP_P(fobj)) {
                        uint8_t t = CL_HDR_TYPE(CL_OBJ_TO_PTR(fobj));
                        if (t < 64) type_counts[t]++;
                    } else {
                        type_counts[63]++;
                    }
                }
                fprintf(stderr, "[FASL] frame type histogram (depth=%u):\n", s->depth);
                for (i = 0; i < 64; i++) {
                    if (type_counts[i] > 0)
                        fprintf(stderr, "  type %u: %u frames\n", i, type_counts[i]);
                }
                fflush(stderr);
                fprintf(stderr, "[FASL] top 20 frames:\n");
                for (i = (s->depth >= 20 ? s->depth - 20 : 0); i < s->depth; i++) {
                    CL_Obj fobj = s->frames[i].obj;
                    int t = CL_HEAP_P(fobj) ? CL_HDR_TYPE(CL_OBJ_TO_PTR(fobj)) : -1;
                    fprintf(stderr, "  [%u] obj=0x%08x phase=%u type=%d",
                            i, (unsigned)fobj, s->frames[i].phase, t);
                    if (t == TYPE_STRUCT) {
                        CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(fobj);
                        fprintf(stderr, " td=0x%08x n_slots=%u idx=%u",
                                (unsigned)st->type_desc, st->n_slots, s->frames[i].index);
                    }
                    fprintf(stderr, "\n");
                }
                fflush(stderr);
            }
#endif
            w->error = FASL_ERR_TOO_DEEP;
            return 0;
        }
        nf = (FaslSerFrame *)platform_alloc(new_cap * sizeof(FaslSerFrame));
        if (!nf) { w->error = FASL_ERR_OVERFLOW; return 0; }
        if (s->frames) {
            memcpy(nf, s->frames, s->depth * sizeof(FaslSerFrame));
            platform_free(s->frames);
        }
        s->frames = nf;
        s->capacity = new_cap;
    }
    s->frames[s->depth].obj    = obj;
    s->frames[s->depth].phase  = phase;
    s->frames[s->depth].index  = 0;
    s->frames[s->depth].pad[0] = 0;
    s->frames[s->depth].pad[1] = 0;
    s->frames[s->depth].pad[2] = 0;
    s->depth++;
    return 1;
}

/* Step the topmost frame.  Returns 1 if the frame is done (caller pops),
 * 0 if it pushed children (frame stays on stack, children processed first). */
/* --- Portable-FASL mode (see fasl.h) ---
 * AMILISP_FASL_PORTABLE=1 makes the writer refuse strings that need
 * FASL_TAG_WIDE_STRING; the refusal detail lives on the writer itself
 * (CL_FaslWriter.nonportable_detail) for bi_compile_file's "FASL unit
 * failed" diagnostic — not a global, so concurrent writers (e.g. two MP
 * threads each running compile-file) can't race on the same buffer. */
static int  fasl_portable_mode = -1;   /* -1 = environment not read yet */

int cl_fasl_portable_mode(void)
{
    if (fasl_portable_mode < 0) {
        char envbuf[16];
        const char *s = platform_getenv("AMILISP_FASL_PORTABLE", envbuf,
                                        (int)sizeof(envbuf));
        fasl_portable_mode = (s && s[0] && s[0] != '0') ? 1 : 0;
    }
    return fasl_portable_mode;
}

void cl_fasl_set_portable_mode(int on)
{
    fasl_portable_mode = on ? 1 : 0;
}

/* The source name a portable FASL records for PATH (the resolved, absolute
 * compile-file input).  CL_Bytecode.source_file is serialized into every
 * function's FASL unit and is what a backtrace, the debugger and
 * EXT.DEV's SOURCE-LOCATION print — so a release compiled on the build
 * host used to show "/Users/<builder>/.../cl-amiga/lib/amiga/ahi.lisp" in
 * every Amiga backtrace through a shipped module: a path that exists on no
 * user's machine.  In portable mode the name is made relative to the
 * compiling process's current directory when the source lies under it
 * ("lib/amiga/ahi.lisp": the release script, `make fasl` and
 * compile-lib-fasls.sh all run from the tree root, and that is also where
 * the file sits in the deployed layout).  A source outside the cwd, and
 * every non-portable compile, keeps the absolute path (the Sly/SLYNK M-.
 * backend needs it on the host).  Returns PATH itself or a suffix of it. */
const char *cl_fasl_portable_source_name(const char *path)
{
    char cwd[512];
    int n;

    if (!cl_fasl_portable_mode() || !path)
        return path;
    n = platform_getcwd(cwd, (int)sizeof(cwd));
    if (n <= 0 || n >= (int)sizeof(cwd) - 1)
        return path;
    /* A device-rooted cwd ("Work:") already ends in its separator; else
     * the match must stop at a directory boundary, not mid-component. */
    if (cwd[n - 1] != '/' && cwd[n - 1] != ':') {
        cwd[n++] = '/';
        cwd[n] = '\0';
    }
    if (strncmp(path, cwd, (size_t)n) == 0 && path[n] != '\0')
        return path + n;
    return path;
}

const char *cl_fasl_nonportable_detail(const CL_FaslWriter *w)
{
    return w->nonportable_detail;
}

static int fasl_ser_step(CL_FaslWriter *w, FaslSerStack *s)
{
    /* Cache frame fields locally — push() may reallocate s->frames. */
    uint32_t      idx   = s->depth - 1;
    FaslSerFrame *f     = &s->frames[idx];
    CL_Obj        obj   = f->obj;
    uint8_t       phase = f->phase;

    /* Sentinel: composite frame's children have drained, pop it. */
    if (phase == PHASE_DONE) return 1;

    /* PHASE_START handles all immediate / leaf cases inline and dispatches
     * heap-object types.  BYTECODE may transition to its phase chain. */
    if (phase == PHASE_START) {
        /* Immediates first */
        if (CL_NULL_P(obj))   { cl_fasl_write_u8(w, FASL_TAG_NIL);     return 1; }
        if (obj == CL_T)      { cl_fasl_write_u8(w, FASL_TAG_T);       return 1; }
        if (obj == CL_UNBOUND){ cl_fasl_write_u8(w, FASL_TAG_UNBOUND); return 1; }
        if (CL_FIXNUM_P(obj)) {
            cl_fasl_write_u8(w, FASL_TAG_FIXNUM);
            cl_fasl_write_u32(w, obj);
            return 1;
        }
        if (CL_CHAR_P(obj)) {
            cl_fasl_write_u8(w, FASL_TAG_CHARACTER);
            cl_fasl_write_u32(w, obj);
            return 1;
        }
        if (!CL_HEAP_P(obj)) {
            cl_fasl_write_u8(w, FASL_TAG_FIXNUM);
            cl_fasl_write_u32(w, obj);
            return 1;
        }

        /* Shared-object dedup for closures, bytecodes, structs, symbols,
         * locks, and conses.  If found, emit OBJ_REF and we're done.  If
         * new, register and emit OBJ_DEF, then fall through to the
         * type-specific case.
         *
         * STRUCT is included because cyclic CLOS metaobject graphs (generic
         * function ↔ method ↔ method-function-closure ↔ gf, slot definitions
         * cross-referencing their class, etc.) hit the writer through
         * compile-time constants and would otherwise walk indefinitely until
         * the buffer fills.
         *
         * SYMBOL is included because the same package-qualified symbol name
         * tends to recur dozens-to-thousands of times across a unit's
         * constant graph (slot names, class names, GF names, lambda-list
         * keywords).  Without dedup each reference re-emits the package
         * name + symbol name (~29 B avg); with dedup the second reference
         * costs 3 B.  Gensyms still hit FASL_TAG_GENSYM_DEF/REF in the
         * SYMBOL body case below — wrapping them in OBJ_DEF adds 3 B on
         * first emission but they are negligible by count.
         *
         * LOCK is included so that multiple references to the same
         * compile-time lock (e.g. a struct slot reachable from several
         * places in the constant graph) all resolve to the same
         * fresh-at-load-time lock object.  Without dedup, two slots that
         * shared a lock at compile time would each get an independent
         * fresh lock at load time and synchronization would be split
         * across them.
         *
         * CONS is included for two reasons.  First, structurally-shared
         * sub-lists (parameter lists, lambda lists, declaration forms,
         * pattern templates from `match`/`ematch`, class precedence lists,
         * etc.) recur many times across a unit's constants — without
         * dedup each reference re-walks the whole list.  Second, with
         * sufficiently nested structural sharing the per-walk amplification
         * pushes the worklist past FASL_SER_STACK_MAX_CAP (FASL_ERR_TOO_DEEP)
         * even though no real cycle exists; serapeum/internal-definitions.lisp
         * hits this through its trivia-based macroexpansion.  Only the head
         * cons of any list reaches this site (cdr cells are walked inline
         * by PHASE_CONS_NEXT_CAR), so dedup applies per list head — enough
         * to break the amplification and small enough not to bloat every
         * cdr cell with an OBJ_DEF wrapper.  PHASE_CONS_NEXT_CAR additionally
         * does a lookup-only probe per cdr cell (fasl_shared_lookup): a cdr
         * that loops back to an already-registered cons — a circular list
         * literal or a shared tail — emits an OBJ_REF and stops the spine
         * instead of walking the cycle forever.
         *
         *
         * Strings, vectors/arrays, bit and byte vectors and pathnames are
         * included for identity alone (v36): '(#1="s" #1#) has to come back
         * with both elements EQ, as it is in the source and as it was for
         * lists all along (CLHS 3.2.4.4 — the loader may coalesce similar
         * literals, it may not split one).  Costs 3 B per literal.
         *
         * The reader's OBJ_DEF/REF handler is type-agnostic, but a type with
         * children must take the pending id before it reads them (see
         * FASL_TAG_OBJ_DEF). */
        {
            uint8_t htype = CL_HDR_TYPE(CL_OBJ_TO_PTR(obj));
            if (htype == TYPE_CLOSURE || htype == TYPE_BYTECODE ||
                htype == TYPE_STRUCT  || htype == TYPE_SYMBOL  ||
                htype == TYPE_LOCK    || htype == TYPE_CONS    ||
                htype == TYPE_STRING  ||
#ifdef CL_WIDE_STRINGS
                htype == TYPE_WIDE_STRING ||
#endif
                htype == TYPE_VECTOR  || htype == TYPE_BIT_VECTOR  ||
                htype == TYPE_BYTE_VECTOR || htype == TYPE_PATHNAME) {
                uint16_t id;
                int rc = fasl_shared_get_or_insert(w, obj, &id);
                if (rc == 1) {
                    cl_fasl_write_u8(w, FASL_TAG_OBJ_REF);
                    cl_fasl_write_u16(w, id);
#ifdef DEBUG_FASL
                    if (g_fasl_hist_enabled) g_fasl_dedup_obj_ref++;
#endif
                    return 1;
                } else if (rc == 0) {
                    cl_fasl_write_u8(w, FASL_TAG_OBJ_DEF);
                    cl_fasl_write_u16(w, id);
                }
                /* rc < 0: dedup unavailable (alloc failed or table full).
                 * Body still emitted correctly inline — just no sharing. */
            }
        }

        switch (CL_HDR_TYPE(CL_OBJ_TO_PTR(obj))) {
        case TYPE_SYMBOL: {
            CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
            CL_String *name = (CL_String *)CL_OBJ_TO_PTR(sym->name);
            if (CL_NULL_P(sym->package)) {
                uint16_t gi;
                for (gi = 0; gi < w->gensym_count; gi++) {
                    if (w->gensym_objs[gi] == obj) {
                        cl_fasl_write_u8(w, FASL_TAG_GENSYM_REF);
                        cl_fasl_write_u16(w, gi);
                        return 1;
                    }
                }
                if (w->gensym_count < FASL_MAX_GENSYMS) {
                    w->gensym_objs[w->gensym_count] = obj;
                    cl_fasl_write_u8(w, FASL_TAG_GENSYM_DEF);
                    cl_fasl_write_u16(w, w->gensym_count);
                    w->gensym_count++;
                } else {
                    cl_fasl_write_u8(w, FASL_TAG_SYMBOL);
                    cl_fasl_write_u16(w, 0);
                }
                cl_fasl_write_u16(w, (uint16_t)name->length);
                cl_fasl_write_bytes(w, name->data, name->length);
                return 1;
            }
            cl_fasl_write_u8(w, FASL_TAG_SYMBOL);
            {
                CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(sym->package);
                CL_String  *pname = (CL_String *)CL_OBJ_TO_PTR(pkg->name);
                if (pkg == (CL_Package *)CL_OBJ_TO_PTR(
                        cl_find_package("KEYWORD", 7))) {
                    cl_fasl_write_u16(w, 0xFFFF);
                } else {
                    cl_fasl_write_u16(w, (uint16_t)pname->length);
                    cl_fasl_write_bytes(w, pname->data, pname->length);
                }
            }
            cl_fasl_write_u16(w, (uint16_t)name->length);
            cl_fasl_write_bytes(w, name->data, name->length);
            return 1;
        }

        case TYPE_STRING: {
            CL_String *str = (CL_String *)CL_OBJ_TO_PTR(obj);
            cl_fasl_write_u8(w, FASL_TAG_STRING);
            cl_fasl_write_u32(w, str->length);
            cl_fasl_write_bytes(w, str->data, str->length);
            return 1;
        }

#ifdef CL_WIDE_STRINGS
        case TYPE_WIDE_STRING: {
            CL_WideString *ws = (CL_WideString *)CL_OBJ_TO_PTR(obj);
            uint32_t i;
            int all_ascii = 1;
            for (i = 0; i < ws->length; i++) {
                if (ws->data[i] > 0x7F) { all_ascii = 0; break; }
            }
            if (all_ascii) {
                /* Downgrade to TAG_STRING so byte-only builds (e.g. the m68k
                 * Amiga binary, which is compiled without CL_WIDE_STRINGS to
                 * save RAM) can read FASLs that this wide-char host produces.
                 * Non-ASCII wide strings still need TAG_WIDE_STRING and remain
                 * unreadable by byte-only builds — but those are uncommon in
                 * source code. */
                cl_fasl_write_u8(w, FASL_TAG_STRING);
                cl_fasl_write_u32(w, ws->length);
                for (i = 0; i < ws->length; i++)
                    cl_fasl_write_u8(w, (uint8_t)ws->data[i]);
            } else if (cl_fasl_portable_mode()) {
                /* AMILISP_FASL_PORTABLE=1: this FASL is meant for a
                 * byte-string build, which has no decoder for
                 * TAG_WIDE_STRING — refuse now, on the host, and describe
                 * the string (ASCII prefix + first offending code point)
                 * for the compile-file diagnostic instead of letting the
                 * Amiga fail the unit with BAD_TAG at load time. */
                uint32_t cp = 0, n = 0;
                char head[48];
                for (i = 0; i < ws->length; i++)
                    if (ws->data[i] > 0x7F) { cp = ws->data[i]; break; }
                for (i = 0; i < ws->length && n < sizeof(head) - 1; i++) {
                    uint32_t c = ws->data[i];
                    head[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
                }
                head[n] = '\0';
                snprintf(w->nonportable_detail, sizeof(w->nonportable_detail),
                         "non-ASCII string literal (U+%04X in \"%s%s\") cannot "
                         "be loaded by byte-string builds -- use ASCII "
                         "(AMILISP_FASL_PORTABLE=1)",
                         (unsigned)cp, head, ws->length > n ? "..." : "");
                w->error = FASL_ERR_NONPORTABLE;
                return 0;
            } else {
                cl_fasl_write_u8(w, FASL_TAG_WIDE_STRING);
                cl_fasl_write_u32(w, ws->length);
                for (i = 0; i < ws->length; i++)
                    cl_fasl_write_u32(w, ws->data[i]);
            }
            return 1;
        }
#endif

        case TYPE_CONS: {
            /* Walk the CDR spine inline using the frame's obj as a cursor:
             * write a CONS tag, push the CAR, advance to PHASE_CONS_NEXT_CAR.
             * After the CAR completes, that phase consumes the next CDR cell
             * (writing another tag) until the spine ends, then pushes the
             * final CDR (NIL or atom).  Peak worklist depth stays O(1) for
             * proper lists of any length — the prior naive push-cdr-as-frame
             * approach grew O(N), exhausting the 2M cap on long lists. */
            CL_Obj car_v = cl_car(obj);
            CL_Obj cdr_v = cl_cdr(obj);
            cl_fasl_write_u8(w, FASL_TAG_CONS);
            s->frames[idx].obj   = cdr_v;
            s->frames[idx].phase = PHASE_CONS_NEXT_CAR;
            if (!fasl_ser_stack_push(w, s, car_v, PHASE_START)) return 1;
            return 0;
        }

        case TYPE_BYTECODE:
            /* Tag here, body via phase chain.  Stay on stack. */
            cl_fasl_write_u8(w, FASL_TAG_BYTECODE);
            s->frames[idx].phase = PHASE_BC_AFTER_TAG;
            return 0;

        case TYPE_CLOSURE: {
            /* Wire format: tag | bytecode-bytes | n_upvalues | upval-bytes...
             * The n_upvalues u16 appears after the bytecode subtree, so we
             * can't push everything at once.  Push bytecode now, defer
             * n_upvalues + upvalues to PHASE_CLOSURE_AFTER_BC. */
            CL_Closure *cl_obj = (CL_Closure *)CL_OBJ_TO_PTR(obj);
            cl_fasl_write_u8(w, FASL_TAG_CLOSURE);
            s->frames[idx].phase = PHASE_CLOSURE_AFTER_BC;
            if (!fasl_ser_stack_push(w, s, cl_obj->bytecode, PHASE_START))
                return 1;
            return 0;
        }

        case TYPE_FUNCTION: {
            CL_Function *fn = (CL_Function *)CL_OBJ_TO_PTR(obj);
            if (!CL_NULL_P(fn->name) && CL_HEAP_P(fn->name) &&
                CL_HDR_TYPE(CL_OBJ_TO_PTR(fn->name)) == TYPE_SYMBOL) {
                cl_fasl_write_u8(w, FASL_TAG_FUNCTION);
                s->frames[idx].phase = PHASE_DONE;
                if (!fasl_ser_stack_push(w, s, fn->name, PHASE_START)) return 1;
                return 0;
            }
            cl_fasl_write_u8(w, FASL_TAG_NIL);
            return 1;
        }

        case TYPE_FFI_STUB: {
            /* Wire format: tag | kind u8 | ctype u8 | b i16 | a u32 |
             * name(obj) | aux(obj).  Pure data — reconstructed at load,
             * no lookup by name (unlike FASL_TAG_FUNCTION).  Both child
             * objects are pushed aux-first so name is written first. */
            CL_FfiStub *fs = (CL_FfiStub *)CL_OBJ_TO_PTR(obj);
            CL_Obj name = fs->name, aux = fs->aux;
            cl_fasl_write_u8(w, FASL_TAG_FFI_STUB);
            cl_fasl_write_u8(w, fs->kind);
            cl_fasl_write_u8(w, fs->ctype);
            cl_fasl_write_u16(w, (uint16_t)fs->b);
            cl_fasl_write_u32(w, fs->a);
            s->frames[idx].phase = PHASE_DONE;
            if (!fasl_ser_stack_push(w, s, aux, PHASE_START)) return 1;
            if (!fasl_ser_stack_push(w, s, name, PHASE_START)) return 1;
            return 0;
        }

        case TYPE_SINGLE_FLOAT: {
            CL_SingleFloat *sf = (CL_SingleFloat *)CL_OBJ_TO_PTR(obj);
            uint32_t bits;
            cl_fasl_write_u8(w, FASL_TAG_SINGLE_FLOAT);
            memcpy(&bits, &sf->value, 4);
            cl_fasl_write_u32(w, bits);
            return 1;
        }

        case TYPE_DOUBLE_FLOAT: {
            CL_DoubleFloat *df = (CL_DoubleFloat *)CL_OBJ_TO_PTR(obj);
            uint8_t bytes[8];
            cl_fasl_write_u8(w, FASL_TAG_DOUBLE_FLOAT);
            fasl_double_to_be(df->value, bytes);
            cl_fasl_write_bytes(w, bytes, 8);
            return 1;
        }

        case TYPE_RATIO: {
            CL_Ratio *rat = (CL_Ratio *)CL_OBJ_TO_PTR(obj);
            CL_Obj num = rat->numerator;
            CL_Obj den = rat->denominator;
            cl_fasl_write_u8(w, FASL_TAG_RATIO);
            s->frames[idx].phase = PHASE_DONE;
            if (!fasl_ser_stack_push(w, s, den, PHASE_START)) return 1;
            if (!fasl_ser_stack_push(w, s, num, PHASE_START)) return 1;
            return 0;
        }

        case TYPE_COMPLEX: {
            CL_Complex *cx = (CL_Complex *)CL_OBJ_TO_PTR(obj);
            CL_Obj re = cx->realpart;
            CL_Obj im = cx->imagpart;
            cl_fasl_write_u8(w, FASL_TAG_COMPLEX);
            s->frames[idx].phase = PHASE_DONE;
            if (!fasl_ser_stack_push(w, s, im, PHASE_START)) return 1;
            if (!fasl_ser_stack_push(w, s, re, PHASE_START)) return 1;
            return 0;
        }

        case TYPE_BIGNUM: {
            CL_Bignum *bn = (CL_Bignum *)CL_OBJ_TO_PTR(obj);
            uint32_t i;
            cl_fasl_write_u8(w, FASL_TAG_BIGNUM);
            cl_fasl_write_u8(w, (uint8_t)bn->sign);
            cl_fasl_write_u32(w, bn->length);
            for (i = 0; i < bn->length; i++)
                cl_fasl_write_u16(w, bn->limbs[i]);
            return 1;
        }

        case TYPE_VECTOR: {
            CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
            uint32_t len = v->length;
            if (v->rank > 1) {
                /* Multidimensional array: preserve rank + dims so it doesn't
                 * reload as a flat 1-D vector.  Dimensions live in
                 * data[0..rank-1] as fixnums; elements follow at data[rank]. */
                uint8_t k;
                cl_fasl_write_u8(w, FASL_TAG_MD_ARRAY);
                cl_fasl_write_u32(w, len);
                cl_fasl_write_u8(w, v->rank);
                for (k = 0; k < v->rank; k++)
                    cl_fasl_write_u32(w, (uint32_t)CL_FIXNUM_VAL(v->data[k]));
            } else {
                cl_fasl_write_u8(w, FASL_TAG_VECTOR);
                cl_fasl_write_u32(w, len);
            }
            if (len == 0) return 1;
            /* Push elements one-at-a-time via PHASE_VECTOR_NEXT — keeps
             * worklist depth O(1) per vector regardless of length.  The phase
             * uses cl_vector_data(), which already skips the dim storage for
             * rank>1, so the same streaming works for both tags. */
            s->frames[idx].phase = PHASE_VECTOR_NEXT;
            s->frames[idx].index = 0;
            return 0;
        }

        case TYPE_BIT_VECTOR: {
            CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
            uint32_t n_words = CL_BV_WORDS(bv->length);
            uint32_t i;
            cl_fasl_write_u8(w, FASL_TAG_BIT_VECTOR);
            cl_fasl_write_u32(w, bv->length);
            for (i = 0; i < n_words; i++)
                cl_fasl_write_u32(w, bv->data[i]);
            return 1;
        }

        case TYPE_BYTE_VECTOR: {
            CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
            cl_fasl_write_u8(w, FASL_TAG_BYTE_VECTOR);
            cl_fasl_write_u32(w, bv->length);
            cl_fasl_write_u8(w, bv->is_signed);
            cl_fasl_write_u8(w, bv->elt_shift);
            if (bv->elt_shift) {
                /* 16-bit elements: write each element big-endian so the
                 * FASL is byte-identical across host (LE) and m68k (BE) —
                 * a raw memory dump would bake in the writer's endianness. */
                uint32_t i;
                for (i = 0; i < bv->length; i++)
                    cl_fasl_write_u16(w,
                        ((const uint16_t *)(const void *)bv->data)[i]);
            } else {
                cl_fasl_write_bytes(w, bv->data, bv->length);
            }
            return 1;
        }

        case TYPE_PACKAGE: {
            CL_Package *pkg = (CL_Package *)CL_OBJ_TO_PTR(obj);
            CL_String  *pname = (CL_String *)CL_OBJ_TO_PTR(pkg->name);
            cl_fasl_write_u8(w, FASL_TAG_PACKAGE);
            cl_fasl_write_u16(w, (uint16_t)pname->length);
            cl_fasl_write_bytes(w, pname->data, pname->length);
            return 1;
        }

        case TYPE_PATHNAME: {
            CL_Pathname *pn = (CL_Pathname *)CL_OBJ_TO_PTR(obj);
            CL_Obj host = pn->host, dev = pn->device, dir = pn->directory;
            CL_Obj name = pn->name, type = pn->type, ver = pn->version;
            cl_fasl_write_u8(w, FASL_TAG_PATHNAME);
            s->frames[idx].phase = PHASE_DONE;
            if (!fasl_ser_stack_push(w, s, ver,  PHASE_START)) return 1;
            if (!fasl_ser_stack_push(w, s, type, PHASE_START)) return 1;
            if (!fasl_ser_stack_push(w, s, name, PHASE_START)) return 1;
            if (!fasl_ser_stack_push(w, s, dir,  PHASE_START)) return 1;
            if (!fasl_ser_stack_push(w, s, dev,  PHASE_START)) return 1;
            if (!fasl_ser_stack_push(w, s, host, PHASE_START)) return 1;
            return 0;
        }

        case TYPE_STRUCT: {
            /* Wire format: tag | type_desc-bytes | n_slots | slot-bytes...
             * The n_slots u32 appears after the type_desc subtree, so we
             * defer slots to PHASE_STRUCT_AFTER_TYPEDESC. */
            CL_Struct *st;
            CL_Obj mlf_creation, mlf_init;
            /* MAKE-LOAD-FORM: if the pre-pass recorded a load form for this
             * object, emit FASL_TAG_LOAD_FORM (creation form, then init
             * form) instead of dumping slots.  The enclosing OBJ_DEF — the
             * shared-object wrapper already wrote it above and registered
             * obj's id — lets the init form's self-reference round-trip as
             * an OBJ_REF resolving to the creation result.  Serialize
             * creation first (push it last so the LIFO drive pops it
             * first), then the init form. */
            if (cl_fasl_mlf_lookup(obj, &mlf_creation, &mlf_init)) {
                cl_fasl_write_u8(w, FASL_TAG_LOAD_FORM);
                s->frames[idx].phase = PHASE_DONE;
                if (!fasl_ser_stack_push(w, s, mlf_init, PHASE_START)) return 1;
                if (!fasl_ser_stack_push(w, s, mlf_creation, PHASE_START)) return 1;
                return 0;
            }
            st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
            /* CLOS class metaobjects (STANDARD-CLASS / BUILT-IN-CLASS
             * structs) are cyclic by design: their effective-slots,
             * precedence-list, and direct-subclasses slots back-reference
             * the class.  Naive struct serialization walks the cycle
             * forever (FASL_ERR_TOO_DEEP).  Even if we broke the cycle,
             * deserializing into a *new* struct would lose identity —
             * defmethod specializers would no longer EQ the live class.
             *
             * Instead, emit a class-by-name reference that resolves via
             * (find-class 'name) at load time.  Identity is preserved
             * because find-class returns the singleton metaobject. */
            if (CL_HEAP_P(st->type_desc) &&
                CL_HDR_TYPE(CL_OBJ_TO_PTR(st->type_desc)) == TYPE_SYMBOL) {
                CL_Symbol *td_sym = (CL_Symbol *)CL_OBJ_TO_PTR(st->type_desc);
                CL_String *td_nm = (CL_String *)CL_OBJ_TO_PTR(td_sym->name);
                if ((td_nm->length == 14 &&
                     (memcmp(td_nm->data, "STANDARD-CLASS", 14) == 0 ||
                      memcmp(td_nm->data, "BUILT-IN-CLASS", 14) == 0)) ||
                    (td_nm->length == 26 &&
                     memcmp(td_nm->data, "FUNCALLABLE-STANDARD-CLASS", 26) == 0)) {
                    CL_Obj cname = st->slots[0];
                    cl_fasl_write_u8(w, FASL_TAG_CLASS_REF);
                    s->frames[idx].phase = PHASE_DONE;
                    if (!fasl_ser_stack_push(w, s, cname, PHASE_START)) return 1;
                    return 0;
                }
            }
            cl_fasl_write_u8(w, FASL_TAG_STRUCT);
            s->frames[idx].phase = PHASE_STRUCT_AFTER_TYPEDESC;
            if (!fasl_ser_stack_push(w, s, st->type_desc, PHASE_START)) return 1;
            return 0;
        }

        case TYPE_LOCK: {
            /* Wire format: tag | flags(u8) | name(obj).
             * The platform mutex itself can't be serialized — it's
             * per-process state.  Instead we record just enough metadata
             * to reconstruct an equivalent fresh lock at load time.
             * Sharing within a single FASL is preserved by OBJ_DEF/OBJ_REF
             * (TYPE_LOCK is in the shared_objs dedup set), so multiple
             * references to the *same* compile-time lock all resolve to
             * the *same* fresh-at-load lock object. */
            CL_Lock *lk = (CL_Lock *)CL_OBJ_TO_PTR(obj);
            uint8_t flags = (lk->flags & CL_LOCK_FLAG_RECURSIVE) ? 0x01 : 0x00;
            CL_Obj name = lk->name;
            cl_fasl_write_u8(w, FASL_TAG_LOCK);
            cl_fasl_write_u8(w, flags);
            s->frames[idx].phase = PHASE_DONE;
            if (!fasl_ser_stack_push(w, s, name, PHASE_START)) return 1;
            return 0;
        }

        default:
            cl_fasl_write_u8(w, FASL_TAG_NIL);
            return 1;
        }
    }

    /* CONS resumption: the most recent CAR has been serialized; obj is the
     * remaining CDR.  If still a CONS, write another tag, push next CAR,
     * stay in this phase.  Else push the final CDR and exit. */
    if (phase == PHASE_CONS_NEXT_CAR) {
        if (CL_HEAP_P(obj) && CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)) == TYPE_CONS) {
            CL_Obj car_v, cdr_v;
            uint16_t ref_id;
            /* Cycle / shared-tail guard.  If this cdr cell was already
             * serialized — e.g. a circular list literal `#1=(1 2 3 . #1#)`
             * whose tail loops back to the list head, or two lists sharing
             * a common tail — emit an OBJ_REF to it and terminate the spine.
             * Without this, the inline CDR walk follows the cycle forever,
             * filling the unit buffer until it overflows the cap (the
             * symptom that forced source-load fallback for serapeum's
             * copy-firstn test).  The list head and every CAR-reached
             * sub-list head are registered in the dedup table by the
             * OBJ_DEF/REF wrapper in PHASE_START, so a back-reference to any
             * of them is found here.  The reader already resolves an
             * OBJ_REF in final-cdr position (FASL_TAG_OBJ_REF →
             * shared_objs[id]), so no reader change is needed and acyclic
             * lists are unaffected (lookup-only, no per-cell OBJ_DEF). */
            if (fasl_shared_lookup(w, obj, &ref_id)) {
                cl_fasl_write_u8(w, FASL_TAG_OBJ_REF);
                cl_fasl_write_u16(w, ref_id);
                s->frames[idx].phase = PHASE_DONE;
                return 1;
            }
            car_v = cl_car(obj);
            cdr_v = cl_cdr(obj);
            cl_fasl_write_u8(w, FASL_TAG_CONS);
            s->frames[idx].obj = cdr_v;
            /* phase stays PHASE_CONS_NEXT_CAR */
            if (!fasl_ser_stack_push(w, s, car_v, PHASE_START)) return 1;
            return 0;
        }
        /* End of spine — push final cdr (NIL or atom). */
        s->frames[idx].phase = PHASE_DONE;
        if (!fasl_ser_stack_push(w, s, obj, PHASE_START)) return 1;
        return 0;
    }

    /* CLOSURE resumption: bytecode child has been serialized; now write
     * n_upvalues and transition to one-at-a-time upvalue iteration.  Use
     * the allocation-based upvalue count — safe against bc->n_upvalues
     * mismatch (can occur when bytecodes are recompiled via ASDF reload
     * while old closures are still live). */
    if (phase == PHASE_CLOSURE_AFTER_BC) {
        CL_Closure *cl_obj = (CL_Closure *)CL_OBJ_TO_PTR(obj);
        uint32_t alloc_size = CL_HDR_SIZE(cl_obj);
        uint16_t n_upvalues = (uint16_t)(
            (alloc_size - sizeof(CL_Closure)) / sizeof(CL_Obj));
        cl_fasl_write_u16(w, n_upvalues);
        if (n_upvalues == 0) return 1;
        s->frames[idx].phase = PHASE_CLOSURE_NEXT_UPVAL;
        s->frames[idx].index = 0;
        return 0;
    }

    if (phase == PHASE_CLOSURE_NEXT_UPVAL) {
        CL_Closure *cl_obj = (CL_Closure *)CL_OBJ_TO_PTR(obj);
        uint32_t alloc_size = CL_HDR_SIZE(cl_obj);
        uint16_t n_upvalues = (uint16_t)(
            (alloc_size - sizeof(CL_Closure)) / sizeof(CL_Obj));
        uint32_t k = s->frames[idx].index;
        if (k >= n_upvalues) return 1;
        s->frames[idx].index = k + 1;
        if (!fasl_ser_stack_push(w, s, cl_obj->upvalues[k], PHASE_START))
            return 1;
        return 0;
    }

    /* STRUCT resumption: type_desc child done; write n_slots and transition
     * to one-at-a-time slot iteration. */
    if (phase == PHASE_STRUCT_AFTER_TYPEDESC) {
        CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
        cl_fasl_write_u32(w, st->n_slots);
        if (st->n_slots == 0) return 1;
        s->frames[idx].phase = PHASE_STRUCT_NEXT_SLOT;
        s->frames[idx].index = 0;
        return 0;
    }

    if (phase == PHASE_STRUCT_NEXT_SLOT) {
        CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
        uint32_t k = s->frames[idx].index;
        if (k >= st->n_slots) return 1;
        s->frames[idx].index = k + 1;
        if (!fasl_ser_stack_push(w, s, st->slots[k], PHASE_START))
            return 1;
        return 0;
    }

    /* VECTOR resumption: write tag+len happens in PHASE_START; this just
     * iterates elements one at a time. */
    if (phase == PHASE_VECTOR_NEXT) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        CL_Obj *data = cl_vector_data(v);
        uint32_t k = s->frames[idx].index;
        if (k >= v->length) return 1;
        s->frames[idx].index = k + 1;
        if (!fasl_ser_stack_push(w, s, data[k], PHASE_START))
            return 1;
        return 0;
    }

    /* BYTECODE phase chain — see PHASE_BC_* definitions above. */
    {
        CL_Bytecode *bc = (CL_Bytecode *)CL_OBJ_TO_PTR(obj);
        uint16_t i;

        switch (phase) {
        case PHASE_BC_AFTER_TAG:
            cl_fasl_write_u32(w, bc->code_len);
            cl_fasl_write_bytes(w, bc->code, bc->code_len);
            cl_fasl_write_u16(w, bc->n_constants);
            s->frames[idx].phase = PHASE_BC_CONSTANTS;
            s->frames[idx].index = 0;
            return 0;

        case PHASE_BC_CONSTANTS: {
            uint16_t k = s->frames[idx].index;
            while (k < bc->n_constants) {
                CL_Obj cst = bc->constants[k];
                int found = 0;
                if (CL_HEAP_P(cst)) {
                    uint16_t j;
                    for (j = 0; j < k; j++) {
                        if (bc->constants[j] == cst) {
                            cl_fasl_write_u8(w, FASL_TAG_CONST_REF);
                            cl_fasl_write_u16(w, j);
                            found = 1;
#ifdef DEBUG_FASL
                            if (g_fasl_hist_enabled) g_fasl_dedup_const_ref++;
#endif
                            break;
                        }
                    }
                }
                if (found) { k++; continue; }
                /* Push child, advance index, yield. */
                s->frames[idx].index = k + 1;
                if (!fasl_ser_stack_push(w, s, cst, PHASE_START)) return 1;
                return 0;
            }
            s->frames[idx].phase = PHASE_BC_METADATA;
            s->frames[idx].index = 0;
            return 0;
        }

        case PHASE_BC_METADATA:
            cl_fasl_write_u16(w, bc->arity);
            cl_fasl_write_u16(w, bc->n_locals);
            cl_fasl_write_u16(w, bc->n_upvalues);
            cl_fasl_write_u8(w, bc->n_optional);
            /* Bit 7 of the wire byte is the (speed 3) JIT hint -- the
             * in-memory flags use bits 0-1 only (fasl.h v38). */
            cl_fasl_write_u8(w, (uint8_t)((bc->flags & 0x7F) |
                                          ((bc->jit_hot & CL_BC_JIT_SPEED) ? 0x80 : 0)));
            cl_fasl_write_u8(w, bc->n_keys);
            s->frames[idx].phase = PHASE_BC_KEY_SYMS;
            s->frames[idx].index = 0;
            return 0;

        case PHASE_BC_KEY_SYMS: {
            uint16_t k = s->frames[idx].index;
            if (k < bc->n_keys) {
                CL_Obj ks = bc->key_syms[k];
                s->frames[idx].index = k + 1;
                if (!fasl_ser_stack_push(w, s, ks, PHASE_START)) return 1;
                return 0;
            }
            s->frames[idx].phase = PHASE_BC_POSTLUDE;
            s->frames[idx].index = 0;
            return 0;
        }

        case PHASE_BC_POSTLUDE:
            for (i = 0; i < bc->n_keys; i++)
                cl_fasl_write_u8(w, bc->key_slots[i]);
            for (i = 0; i < bc->n_keys; i++)
                cl_fasl_write_u8(w, bc->key_suppliedp_slots[i]);
            cl_fasl_write_u16(w, bc->source_line);
            if (bc->source_file) {
                uint16_t slen = (uint16_t)strlen(bc->source_file);
                cl_fasl_write_u16(w, slen);
                cl_fasl_write_bytes(w, bc->source_file, slen);
            } else {
                cl_fasl_write_u16(w, 0);
            }
            cl_fasl_write_u16(w, bc->line_map_count);
            for (i = 0; i < bc->line_map_count; i++) {
                cl_fasl_write_u16(w, bc->line_map[i].pc);
                cl_fasl_write_u16(w, bc->line_map[i].line);
            }
            /* v9: serialize the source lambda-list, then the name.  Order
             * matters — the reader deserializes lambda-list before name. */
            s->frames[idx].phase = PHASE_BC_LAMBDA_LIST;
            if (!fasl_ser_stack_push(w, s, bc->source_lambda_list, PHASE_START))
                return 1;
            return 0;

        case PHASE_BC_LAMBDA_LIST:
            s->frames[idx].phase = PHASE_DONE;
            if (!fasl_ser_stack_push(w, s, bc->name, PHASE_START)) return 1;
            return 0;

        case PHASE_DONE:
            return 1;

        default:
            /* Unknown phase — treat as done to avoid infinite loop. */
            return 1;
        }
    }
}

#ifdef DEBUG_FASL
/* --- Histogram bucket layout ---
 *
 * Indexed by FASL_HIST_*.  Bytes attributed via position-delta around each
 * fasl_ser_step call.  Dedup hits are bumped at the OBJ_REF / CONST_REF
 * emission sites so we can compute the bytes-saved component too. */
#define FASL_HIST_IMMEDIATE   0   /* NIL/T/UNBOUND/FIXNUM/CHARACTER */
#define FASL_HIST_SYMBOL      1
#define FASL_HIST_GENSYM      2
#define FASL_HIST_STRING      3
#define FASL_HIST_CONS        4
#define FASL_HIST_VECTOR      5
#define FASL_HIST_BIT_VECTOR  6
#define FASL_HIST_BYTECODE    7
#define FASL_HIST_CLOSURE     8
#define FASL_HIST_FUNCTION    9
#define FASL_HIST_STRUCT      10
#define FASL_HIST_FLOAT       11
#define FASL_HIST_BIGNUM      12
#define FASL_HIST_RATIO       13
#define FASL_HIST_COMPLEX     14
#define FASL_HIST_PATHNAME    15
#define FASL_HIST_CLASS_REF   16
#define FASL_HIST_FFI_STUB    17
#define FASL_HIST_OTHER       18
#define FASL_HIST_COUNT       19

static const char *cl_fasl_hist_names[FASL_HIST_COUNT] = {
    "IMMEDIATE", "SYMBOL", "GENSYM", "STRING", "CONS", "VECTOR", "BIT_VECTOR",
    "BYTECODE", "CLOSURE", "FUNCTION", "STRUCT", "FLOAT", "BIGNUM", "RATIO",
    "COMPLEX", "PATHNAME", "CLASS_REF", "FFI_STUB", "OTHER"
};

static uint64_t g_fasl_hist_bytes[FASL_HIST_COUNT];
static uint32_t g_fasl_hist_count[FASL_HIST_COUNT];
/* g_fasl_dedup_obj_ref / g_fasl_dedup_const_ref / g_fasl_hist_enabled
 * are forward-declared near the top of this file so the dedup-hit emission
 * sites in fasl_ser_step can reference them. */

static int cl_fasl_classify(CL_Obj obj)
{
    if (CL_NULL_P(obj) || obj == CL_T || obj == CL_UNBOUND ||
        CL_FIXNUM_P(obj) || CL_CHAR_P(obj) || !CL_HEAP_P(obj))
        return FASL_HIST_IMMEDIATE;
    switch (CL_HDR_TYPE(CL_OBJ_TO_PTR(obj))) {
    case TYPE_SYMBOL: {
        CL_Symbol *sy = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
        return CL_NULL_P(sy->package) ? FASL_HIST_GENSYM : FASL_HIST_SYMBOL;
    }
    case TYPE_STRING:        return FASL_HIST_STRING;
    case TYPE_CONS:          return FASL_HIST_CONS;
    case TYPE_VECTOR:        return FASL_HIST_VECTOR;
    case TYPE_BIT_VECTOR:    return FASL_HIST_BIT_VECTOR;
    case TYPE_BYTECODE:      return FASL_HIST_BYTECODE;
    case TYPE_CLOSURE:       return FASL_HIST_CLOSURE;
    case TYPE_FUNCTION:      return FASL_HIST_FUNCTION;
    case TYPE_FFI_STUB:      return FASL_HIST_FFI_STUB;
    case TYPE_STRUCT:        return FASL_HIST_STRUCT;
    case TYPE_SINGLE_FLOAT:
    case TYPE_DOUBLE_FLOAT:  return FASL_HIST_FLOAT;
    case TYPE_BIGNUM:        return FASL_HIST_BIGNUM;
    case TYPE_RATIO:         return FASL_HIST_RATIO;
    case TYPE_COMPLEX:       return FASL_HIST_COMPLEX;
    case TYPE_PATHNAME:      return FASL_HIST_PATHNAME;
    default:                 return FASL_HIST_OTHER;
    }
}

void cl_fasl_hist_begin(void)
{
    int i;
    for (i = 0; i < FASL_HIST_COUNT; i++) {
        g_fasl_hist_bytes[i] = 0;
        g_fasl_hist_count[i] = 0;
    }
    g_fasl_dedup_obj_ref = 0;
    g_fasl_dedup_const_ref = 0;
    g_fasl_hist_enabled = 1;
}

void cl_fasl_hist_end(void)
{
    g_fasl_hist_enabled = 0;
}

void cl_fasl_hist_dump(const char *label, uint32_t bytes_written)
{
    int order[FASL_HIST_COUNT];
    int i, j;
    uint64_t total_bytes = 0;
    uint32_t total_count = 0;

    for (i = 0; i < FASL_HIST_COUNT; i++) order[i] = i;
    /* Insertion sort by bytes desc — small N, fine. */
    for (i = 1; i < FASL_HIST_COUNT; i++) {
        int k = order[i];
        for (j = i; j > 0 && g_fasl_hist_bytes[order[j - 1]] < g_fasl_hist_bytes[k]; j--)
            order[j] = order[j - 1];
        order[j] = k;
    }
    for (i = 0; i < FASL_HIST_COUNT; i++) {
        total_bytes += g_fasl_hist_bytes[i];
        total_count += g_fasl_hist_count[i];
    }
    fprintf(stderr, "[FASL hist] %s - bytes_written=%u total_attributed=%llu objs=%u\n",
            label, bytes_written,
            (unsigned long long)total_bytes, total_count);
    for (i = 0; i < FASL_HIST_COUNT; i++) {
        int b = order[i];
        if (g_fasl_hist_bytes[b] == 0) continue;
        fprintf(stderr, "  %-12s bytes=%10llu  count=%9u  avg=%5llu\n",
                cl_fasl_hist_names[b],
                (unsigned long long)g_fasl_hist_bytes[b],
                g_fasl_hist_count[b],
                g_fasl_hist_count[b]
                  ? (unsigned long long)(g_fasl_hist_bytes[b] / g_fasl_hist_count[b])
                  : 0ULL);
    }
    fprintf(stderr, "  dedup hits: OBJ_REF=%u  CONST_REF=%u\n",
            g_fasl_dedup_obj_ref, g_fasl_dedup_const_ref);
    fflush(stderr);
}
#endif

static void fasl_serialize_drive(CL_FaslWriter *w, FaslSerStack *s)
{
    while (s->depth > 0 && !w->error) {
#ifdef DEBUG_FASL
        CL_Obj   cur_obj = s->frames[s->depth - 1].obj;
        uint32_t pre_pos = w->pos;
        int      done    = fasl_ser_step(w, s);
        if (g_fasl_hist_enabled) {
            int bucket = cl_fasl_classify(cur_obj);
            uint32_t delta = w->pos - pre_pos;
            g_fasl_hist_bytes[bucket] += delta;
            /* Count once per leaf or per composite-frame completion (PHASE_DONE) */
            if (done) g_fasl_hist_count[bucket]++;
        }
        if (done) s->depth--;
#else
        if (fasl_ser_step(w, s)) {
            s->depth--;
        }
#endif
    }
}

void cl_fasl_serialize_obj(CL_FaslWriter *w, CL_Obj obj)
{
    FaslSerStack s;
    s.frames = NULL;
    s.depth = 0;
    s.capacity = 0;
    if (fasl_ser_stack_push(w, &s, obj, PHASE_START))
        fasl_serialize_drive(w, &s);
    if (s.frames) platform_free(s.frames);
}

/* Kept as a no-op for ABI compatibility; the iterative serializer no
 * longer uses recursion-depth or timeout state. */
void cl_fasl_reset_serialize_count(void)
{
}

/* --- Serialize a CL_Bytecode body (no leading tag) ---
 *
 * Used by bi_compile_file to write each top-level form as a length-prefixed
 * unit.  Skips the FASL_TAG_BYTECODE byte (the unit framing has its own
 * length prefix) and the shared-object dedup wrapper. */
void cl_fasl_serialize_bytecode(CL_FaslWriter *w, CL_Obj bc_obj)
{
    FaslSerStack s;
    s.frames = NULL;
    s.depth = 0;
    s.capacity = 0;
    /* Initial frame: skip dispatch+tag, jump directly to the BC body phases. */
    if (fasl_ser_stack_push(w, &s, bc_obj, PHASE_BC_AFTER_TAG))
        fasl_serialize_drive(w, &s);
    if (s.frames) platform_free(s.frames);
}

/* ================================================================
 * Reader
 * ================================================================ */

void cl_fasl_reader_init(CL_FaslReader *r, const uint8_t *data, uint32_t size)
{
    r->data = data;
    r->size = size;
    r->pos = 0;
    r->error = FASL_OK;
    r->gensym_count = 0;
    r->shared_count = 0;
    r->shared_objs = NULL;  /* lazily allocated when OBJ_DEF tag is encountered */
    r->pending_obj_def_id = 0xFFFFFFFFu;  /* none pending */
    /* Zero the gensym table: the reader is a stack local, and a corrupted
     * FASL whose first GENSYM_DEF carries a large id would set
     * gensym_count past uninitialized entries — the active-reader GC
     * hooks then MARK stack garbage as object offsets (corruption from
     * bad input instead of a clean load error). */
    memset(r->gensym_objs, 0, sizeof(r->gensym_objs));
}

/* --- Active-reader registry: GC-rooting of the dedup tables ---
 *
 * A reader's gensym_objs[] and shared_objs[] hold CL_Obj references to
 * already-deserialized objects.  A forward GENSYM_REF / OBJ_REF later in
 * the stream resolves through these tables, so the referenced object must
 * stay live AND its offset must be forwarded when the compacting GC moves
 * it — even when the object is not yet stitched into the GC-reachable
 * graph being built (e.g. a shared symbol defined in constant[0] but not
 * referenced until constant[5]).  These tables live in a stack-local
 * CL_FaslReader the collector knows nothing about, so without rooting them
 * a compaction mid-load leaves stale offsets and a later REF returns a
 * relocated object's old address — manifesting as corrupted forms such as
 * "Undefined function: PACKAGE" when loading a FASL under heap pressure.
 *
 * Loads can nest (a CLASS_REF runs FIND-CLASS, which may load another
 * FASL), so we keep a stack of active readers; it starts at these static
 * sizes and grows when LOADs nest deeper (fasl_reg_grow). */
#define FASL_MAX_ACTIVE_READERS 16
#define FASL_MAX_ACTIVE_WRITERS 8

/* Generic active-object registry shared by the reader and writer tables.
 * Both are a small fixed array of active CL_Fasl{Reader,Writer} pointers plus
 * a parallel owner-thread id per slot, guarded by one mutex.  The register /
 * unregister / save_count / restore_count logic is byte-identical between the
 * two, so it lives once here (fasl_reg_*) and the public entry points below
 * are thin wrappers.  items[] is void* so both element types share this code
 * without type-punning; the GC mark/update walkers cast back to the concrete
 * type.
 *
 * anchor[] and free_abandoned_item exist for the writer registry only, and
 * are NULL for the reader registry:
 *   - A reader is a C stack local of fasl_load, so the reader's own address
 *     IS a usable depth anchor (see cl_fasl_reader_unwind_to) and there is
 *     nothing to free beyond aux[] (its shared_objs table) — the reader
 *     itself sits in dead stack after an abandoning longjmp and must not be
 *     touched.
 *   - A writer is platform_alloc'd (bi_load heap-allocates it to keep its own
 *     stack frame small), so items[] is a heap pointer unrelated to C-stack
 *     depth: anchor[] carries a separate CL_CAPTURE_SP() taken at the
 *     registering frame instead.  And because the writer struct survives the
 *     longjmp (it's heap, not stack), free_abandoned_item can safely
 *     dereference it to hand back everything the abandoned registration
 *     still owns. */
typedef struct FaslRegistry FaslRegistry;
struct FaslRegistry {
    void    **items;   /* active CL_FaslReader* / CL_FaslWriter* pointers */
    uint32_t *owners;  /* CT->id that registered each slot */
    void    **aux;     /* off-heap table to free on abandonment: readers'
                        * shared_objs (fasl_reader_note_shared) or writers'
                        * per-unit scratch buffer (cl_fasl_writer_note_scratch) */
    void    **anchor;  /* writers only (NULL for readers): stack_anchor passed
                        * to cl_fasl_writer_register, see above */
    void    (*free_abandoned_item)(void *item);  /* writers only (NULL for
                        * readers): frees the item itself on abandonment */
    int       count;
    int       cap;
    int       grown;   /* arrays are platform_alloc'd (fasl_reg_grow), not the
                        * static initial ones */
};

static void fasl_writer_free_abandoned_item(void *item);

static void    *fasl_reader_slots[FASL_MAX_ACTIVE_READERS];
static uint32_t  fasl_reader_owners[FASL_MAX_ACTIVE_READERS];
static void    *fasl_reader_aux[FASL_MAX_ACTIVE_READERS];
static void    *fasl_writer_slots[FASL_MAX_ACTIVE_WRITERS];
static uint32_t  fasl_writer_owners[FASL_MAX_ACTIVE_WRITERS];
static void    *fasl_writer_aux[FASL_MAX_ACTIVE_WRITERS];
static void    *fasl_writer_anchor[FASL_MAX_ACTIVE_WRITERS];

static FaslRegistry fasl_reader_reg =
    { fasl_reader_slots, fasl_reader_owners, fasl_reader_aux, NULL, NULL,
      0, FASL_MAX_ACTIVE_READERS, 0 };
static FaslRegistry fasl_writer_reg =
    { fasl_writer_slots, fasl_writer_owners, fasl_writer_aux, fasl_writer_anchor,
      fasl_writer_free_abandoned_item, 0, FASL_MAX_ACTIVE_WRITERS, 0 };

/* Protects both registries in multi-threaded mode.  Initialized lazily on the
 * first register call, which always runs on the main thread during single-
 * threaded boot before any secondary thread is created. */
static void *fasl_registry_mutex = NULL;

/* Double the registry's arrays (caller holds the mutex in MT mode).  The
 * registry is as deep as LOADs nest, summed over all threads, so a fixed cap
 * is a nesting limit.  The GC walkers read items[]/count unlocked under
 * stop-the-world, which on AmigaOS suspends this task wherever it happens to
 * be: every new array is a full copy before its pointer is published, the
 * old ones are freed only after all four are, and cap goes last — a walker
 * sees a consistent prefix whichever mix of old and new it catches.  The
 * initial arrays are static and never freed. */
static int fasl_reg_grow(FaslRegistry *reg)
{
    int ncap = reg->cap * 2;
    int n = reg->count;
    void    **items  = (void **)platform_alloc((uint32_t)(ncap * sizeof(void *)));
    uint32_t *owners = (uint32_t *)platform_alloc((uint32_t)(ncap * sizeof(uint32_t)));
    void    **aux    = (void **)platform_alloc((uint32_t)(ncap * sizeof(void *)));
    void    **anchor = reg->anchor
        ? (void **)platform_alloc((uint32_t)(ncap * sizeof(void *))) : NULL;
    void *old_items = reg->items, *old_owners = reg->owners;
    void *old_aux = reg->aux, *old_anchor = reg->anchor;

    if (!items || !owners || !aux || (reg->anchor && !anchor)) {
        if (items) platform_free(items);
        if (owners) platform_free(owners);
        if (aux) platform_free(aux);
        if (anchor) platform_free(anchor);
        return 0;
    }
    memcpy(items, reg->items, n * sizeof(void *));
    memcpy(owners, reg->owners, n * sizeof(uint32_t));
    memcpy(aux, reg->aux, n * sizeof(void *));
    if (anchor) memcpy(anchor, reg->anchor, n * sizeof(void *));
    reg->items = items;
    reg->owners = owners;
    reg->aux = aux;
    if (anchor) reg->anchor = anchor;
    if (reg->grown) {
        platform_free(old_items);
        platform_free(old_owners);
        platform_free(old_aux);
        if (old_anchor) platform_free(old_anchor);
    }
    reg->grown = 1;
    reg->cap = ncap;
    return 1;
}

static void fasl_reg_add(FaslRegistry *reg, void *item, void *anchor, const char *what)
{
    int ok;
    /* Capture CL_MT() once so the unlock decision matches the lock decision:
     * cl_thread_count can drop 2->1 (a peer worker exiting) between them, and a
     * re-evaluated guard would skip the unlock, leaking fasl_registry_mutex. */
    int mt;
    if (!fasl_registry_mutex)
        platform_mutex_init(&fasl_registry_mutex);
    mt = CL_MT();

    if (mt) platform_mutex_lock(fasl_registry_mutex);
    ok = reg->count < reg->cap || fasl_reg_grow(reg);
    if (ok) {
        reg->items[reg->count] = item;
        reg->owners[reg->count] = CT->id;
        reg->aux[reg->count] = NULL;
        if (reg->anchor) reg->anchor[reg->count] = anchor;
        reg->count++;
    }
    if (mt) platform_mutex_unlock(fasl_registry_mutex);

    if (!ok)
        cl_error(CL_ERR_GENERAL,
                 "FASL: out of memory growing the active-%s table (%d in use)",
                 what, reg->cap);
}

/* Drop slot I (caller holds the mutex in MT mode).  ABANDONED = the owning C
 * frame was unwound by a longjmp and will never run its own cleanup: the
 * slot's off-heap table is freed here, from the registry's copy of the
 * pointer.  For the reader registry (free_abandoned_item is NULL) the reader
 * itself sits in dead stack and must not be read past aux[]; the writer
 * registry's free_abandoned_item may dereference items[i] freely (it is
 * heap-allocated, not a stack local — see the FaslRegistry comment above). */
static void fasl_reg_drop_at(FaslRegistry *reg, int i, int abandoned)
{
    int j;
    if (abandoned) {
        if (reg->aux && reg->aux[i])
            platform_free(reg->aux[i]);
        if (reg->free_abandoned_item)
            reg->free_abandoned_item(reg->items[i]);
    }
    for (j = i; j < reg->count - 1; j++) {
        reg->items[j] = reg->items[j + 1];
        reg->owners[j] = reg->owners[j + 1];
        if (reg->aux) reg->aux[j] = reg->aux[j + 1];
        if (reg->anchor) reg->anchor[j] = reg->anchor[j + 1];
    }
    reg->count--;
}

static void fasl_reg_remove(FaslRegistry *reg, void *item)
{
    int i;
    int mt = CL_MT();  /* capture once — see fasl_reg_add */
    if (mt) platform_mutex_lock(fasl_registry_mutex);
    /* Common case: LIFO pop (registration nests with load recursion). */
    if (reg->count > 0 && reg->items[reg->count - 1] == item) {
        reg->count--;
        if (mt) platform_mutex_unlock(fasl_registry_mutex);
        return;
    }
    /* Defensive out-of-order removal. */
    for (i = reg->count - 1; i >= 0; i--) {
        if (reg->items[i] == item) {
            fasl_reg_drop_at(reg, i, 0);
            break;
        }
    }
    if (mt) platform_mutex_unlock(fasl_registry_mutex);
}

/* Error-unwind support: cl_error longjmps out of a FASL load/save without
 * running the matching unregister, which would leave a dangling pointer to the
 * now-unwound stack-local reader/writer in the active list.  The error-frame
 * machinery snapshots the count on push and restores it on unwind (mirroring
 * saved_gc_roots).  In multi-threaded mode the count is PER-THREAD: save_count
 * returns how many entries the calling thread has active, and restore_count
 * removes only that thread's excess — a thread-A unwind never evicts an entry
 * registered concurrently by thread B. */
static int fasl_reg_save_count(FaslRegistry *reg)
{
    if (!CL_MT())
        return reg->count;
    {
        uint32_t tid = CT->id;
        int i, count = 0;
        if (!fasl_registry_mutex) return 0;
        platform_mutex_lock(fasl_registry_mutex);
        for (i = 0; i < reg->count; i++)
            if (reg->owners[i] == tid)
                count++;
        platform_mutex_unlock(fasl_registry_mutex);
        return count;
    }
}

static void fasl_reg_restore_count(FaslRegistry *reg, int n)
{
    int i;
    if (n < 0) return;
    if (!CL_MT()) {
        while (reg->count > n)
            fasl_reg_drop_at(reg, reg->count - 1, 1);
        return;
    }
    if (!fasl_registry_mutex) return;
    {
        uint32_t tid = CT->id;
        int thread_count = 0;
        platform_mutex_lock(fasl_registry_mutex);
        for (i = 0; i < reg->count; i++)
            if (reg->owners[i] == tid)
                thread_count++;
        /* Remove excess entries from the end (LIFO).  Backward scan is safe:
         * removing at index i shifts i+1..count-1 down, but those slots were
         * already visited in earlier iterations. */
        for (i = reg->count - 1; i >= 0 && thread_count > n; i--) {
            if (reg->owners[i] == tid) {
                fasl_reg_drop_at(reg, i, 1);
                thread_count--;
            }
        }
        platform_mutex_unlock(fasl_registry_mutex);
    }
}

/* --- Public reader registry entry points ---
 * A reader's gensym_objs[]/shared_objs[] hold CL_Obj references to already-
 * deserialized objects that a forward GENSYM_REF/OBJ_REF resolves through, so
 * they must stay live and be forwarded across a mid-load compaction even before
 * the objects are stitched into the reachable graph (see the rooting rationale
 * above cl_fasl_gc_visit_readers).  Loads nest (a CLASS_REF may load another
 * FASL), hence the stack of active readers. */
void cl_fasl_reader_register(CL_FaslReader *r)   { fasl_reg_add(&fasl_reader_reg, r, NULL, "reader"); }
void cl_fasl_reader_unregister(CL_FaslReader *r) { fasl_reg_remove(&fasl_reader_reg, r); }
int  cl_fasl_reader_save_count(void)             { return fasl_reg_save_count(&fasl_reader_reg); }
void cl_fasl_reader_restore_count(int n)         { fasl_reg_restore_count(&fasl_reader_reg, n); }

/* The reader just allocated its shared_objs table: keep a copy of the pointer
 * in its registry slot, so an unwind that abandons the reader can still hand
 * the table back (AmigaOS has no exit reclaim).  An unregistered reader (the
 * unit tests) has no slot and keeps freeing the table itself. */
static void fasl_reader_note_shared(CL_FaslReader *r)
{
    int i;
    int mt = CL_MT();
    if (mt && !fasl_registry_mutex) return;
    if (mt) platform_mutex_lock(fasl_registry_mutex);
    for (i = fasl_reader_reg.count - 1; i >= 0; i--) {
        if (fasl_reader_reg.items[i] == (void *)r) {
            fasl_reader_reg.aux[i] = (void *)r->shared_objs;
            break;
        }
    }
    if (mt) platform_mutex_unlock(fasl_registry_mutex);
}

/* NLX-landing unwind of an active-{reader,writer} registry — the counterpart
 * of cl_compiler_unwind_to, called next to it (twice, once per registry) at
 * every landing.  A Lisp HANDLER-CASE / RETURN-FROM / THROW out of a FASL
 * load passes no C error frame, so fasl_reg_restore_count never runs for it
 * and the registry keeps pointing at the abandoned reader/writer: the next GC
 * marks a dead stack frame as a reader (SEGV in gc_mark_children, or worse,
 * "forwards" whatever lives there by then), or keeps touching a writer whose
 * owning bi_load frame is long gone and will never free it.
 *
 * The depth test compares LANDING_ANCHOR against each entry's ANCHOR — for
 * the reader registry that is items[i] itself (a reader is always a C stack
 * local of fasl_load); for the writer registry (anchor non-NULL) it is the
 * separate CL_CAPTURE_SP() stashed at cl_fasl_writer_register time, since
 * items[i] there is a platform_alloc'd pointer unrelated to C-stack depth.
 * Either way: an anchor strictly deeper than LANDING_ANCHOR belongs to a
 * frame the longjmp discarded; one at or shallower is owned by a live
 * fasl_load whose unit re-entered the VM, and stays.  Only the calling
 * thread's entries are looked at — another thread's stack is not comparable.
 * The count test up front keeps the landing cheap when no load is in flight
 * (every BLOCK/CATCH/TAGBODY transfer comes through here); it is unlocked,
 * which is fine: an entry of THIS thread keeps the count non-zero whatever
 * the others do. */
static void fasl_reg_unwind_to(FaslRegistry *reg, void *landing_anchor)
{
    int i, mt;
    uint32_t tid;
    if (reg->count == 0) return;
    mt = CL_MT();
    if (mt && !fasl_registry_mutex) return;
    tid = CT->id;
    if (mt) platform_mutex_lock(fasl_registry_mutex);
    for (i = reg->count - 1; i >= 0; i--) {
        void *entry_anchor;
        if (mt && reg->owners[i] != tid) continue;
        entry_anchor = reg->anchor ? reg->anchor[i] : reg->items[i];
        if ((uintptr_t)entry_anchor >= (uintptr_t)landing_anchor)
            break;  /* live owner; everything older is shallower still */
        fasl_reg_drop_at(reg, i, 1);
    }
    if (mt) platform_mutex_unlock(fasl_registry_mutex);
}

void cl_fasl_reader_unwind_to(void *landing_anchor)
{
    fasl_reg_unwind_to(&fasl_reader_reg, landing_anchor);
}

/* --- Public writer registry entry points ---
 * bi_load's auto-cache serializes one FASL unit per top-level form, interleaved
 * with reading/compiling/evaluating the next forms (which allocate and
 * compact).  The persistent file-level writer's gensym_objs[] holds CL_Obj
 * offsets across all of that; without mark+update hooks a moved/collected
 * gensym's old offset could collide with a different live gensym's new offset,
 * making the EQ dedup emit a GENSYM_REF to the wrong id — a silently wrong
 * cached FASL. */
void cl_fasl_writer_register(CL_FaslWriter *w, void *stack_anchor)
{
    fasl_reg_add(&fasl_writer_reg, w, stack_anchor, "writer");
}
void cl_fasl_writer_unregister(CL_FaslWriter *w) { fasl_reg_remove(&fasl_writer_reg, w); }
int  cl_fasl_writer_save_count(void)             { return fasl_reg_save_count(&fasl_writer_reg); }
void cl_fasl_writer_restore_count(int n)         { fasl_reg_restore_count(&fasl_writer_reg, n); }

/* The caller (re)allocated its per-unit scratch buffer (bi_load's unit_buf):
 * keep a copy of the pointer in the writer's registry slot, so an unwind that
 * abandons the writer can still hand the buffer back (AmigaOS has no exit
 * reclaim) — mirrors fasl_reader_note_shared.  An unregistered writer (e.g.
 * bi_compile_file's, or the unit tests') has no slot and keeps freeing its
 * own scratch buffer, so this is a silent no-op for it. */
void cl_fasl_writer_note_scratch(CL_FaslWriter *w, void *unit_buf)
{
    int i;
    int mt = CL_MT();
    if (mt && !fasl_registry_mutex) return;
    if (mt) platform_mutex_lock(fasl_registry_mutex);
    for (i = fasl_writer_reg.count - 1; i >= 0; i--) {
        if (fasl_writer_reg.items[i] == (void *)w) {
            fasl_writer_reg.aux[i] = unit_buf;
            break;
        }
    }
    if (mt) platform_mutex_unlock(fasl_registry_mutex);
}

/* fasl_reg_drop_at's free_abandoned_item for the writer registry.  Unlike a
 * reader, W is platform_alloc'd — it survives the longjmp that abandoned its
 * registration, so it is safe to dereference here and hand back everything
 * bi_load would have on its own cleanup path: the shared/gensym dedup tables
 * (cl_fasl_writer_release), the growing output buffer (w->data, kept in sync
 * with bi_load's fasl_buf by cf_emit_fasl_unit/cf_emit_fasl_deps on every
 * grow), and the writer struct itself.  aux[i] (unit_buf) is already freed by
 * fasl_reg_drop_at before this runs. */
static void fasl_writer_free_abandoned_item(void *item)
{
    CL_FaslWriter *w = (CL_FaslWriter *)item;
    if (!w) return;
    cl_fasl_writer_release(w);
    if (w->data) platform_free(w->data);
    platform_free(w);
}

void cl_fasl_writer_unwind_to(void *landing_anchor)
{
    fasl_reg_unwind_to(&fasl_writer_reg, landing_anchor);
}

/* Process exit: hand back registry arrays that grew past the static ones
 * (LOADs nested deeper than the initial caps).  AmigaOS has no exit reclaim. */
static void fasl_reg_release(FaslRegistry *reg, void **items, uint32_t *owners,
                             void **aux, void **anchor, int cap)
{
    if (!reg->grown) return;
    platform_free(reg->items);
    platform_free(reg->owners);
    platform_free(reg->aux);
    if (reg->anchor) platform_free(reg->anchor);
    /* Back to the static arrays, so a re-init starts from a working registry. */
    reg->items = items;
    reg->owners = owners;
    reg->aux = aux;
    reg->anchor = anchor;
    reg->count = 0;
    reg->cap = cap;
    reg->grown = 0;
}

void cl_fasl_release_registries(void)
{
    fasl_reg_release(&fasl_reader_reg, fasl_reader_slots, fasl_reader_owners,
                     fasl_reader_aux, NULL, FASL_MAX_ACTIVE_READERS);
    fasl_reg_release(&fasl_writer_reg, fasl_writer_slots, fasl_writer_owners,
                     fasl_writer_aux, fasl_writer_anchor, FASL_MAX_ACTIVE_WRITERS);
}

/* GC root walks (part of gc_visit_shared_roots, gc_trace.c): the gensym
 * dedup entries of every registered writer, and the gensym/shared-object
 * tables of every registered reader.  The marker keeps them alive, the
 * compactor forwards them.  Run under stop-the-world GC — no registry
 * lock needed. */
void cl_fasl_gc_visit_writers(void (*visit)(CL_Obj *))
{
    int i;
    uint16_t k;
    for (i = 0; i < fasl_writer_reg.count; i++) {
        CL_FaslWriter *w = (CL_FaslWriter *)fasl_writer_reg.items[i];
        for (k = 0; k < w->gensym_count; k++)
            visit(&w->gensym_objs[k]);
    }
}

void cl_fasl_gc_visit_readers(void (*visit)(CL_Obj *))
{
    int i;
    uint16_t k;
    for (i = 0; i < fasl_reader_reg.count; i++) {
        CL_FaslReader *r = (CL_FaslReader *)fasl_reader_reg.items[i];
        for (k = 0; k < r->gensym_count; k++)
            visit(&r->gensym_objs[k]);
        if (r->shared_objs) {
            for (k = 0; k < r->shared_count; k++)
                visit(&r->shared_objs[k]);
        }
    }
}

/* OBJ_DEF / cycle-handling helpers — see notes in FASL_TAG_OBJ_DEF case. */
#define CL_FASL_NO_PENDING 0xFFFFFFFFu

static void cl_fasl_set_pending_obj_def(CL_FaslReader *r, uint16_t id)
{
    r->pending_obj_def_id = id;
}

/* Called by allocate-then-fill body deserializers (struct/closure/bytecode)
 * right after they allocate the shell.  Registers the shell at the pending
 * id (if any) so cyclic OBJ_REFs inside the body resolve to the shell.
 * Idempotent and safe to call when no id is pending. */
static void cl_fasl_consume_pending_obj_def(CL_FaslReader *r, CL_Obj shell)
{
    if (r->pending_obj_def_id == CL_FASL_NO_PENDING) return;
    if (r->shared_objs && r->pending_obj_def_id < FASL_MAX_SHARED) {
        uint16_t id = (uint16_t)r->pending_obj_def_id;
        r->shared_objs[id] = shell;
        if (id >= r->shared_count) r->shared_count = id + 1;
    }
    r->pending_obj_def_id = CL_FASL_NO_PENDING;
}

uint8_t cl_fasl_read_u8(CL_FaslReader *r)
{
    if (r->error) return 0;
    if (r->pos + 1 > r->size) { r->error = FASL_ERR_TRUNCATED; return 0; }
    return r->data[r->pos++];
}

uint16_t cl_fasl_read_u16(CL_FaslReader *r)
{
    uint16_t val;
    if (r->error) return 0;
    if (r->pos + 2 > r->size) { r->error = FASL_ERR_TRUNCATED; return 0; }
    val = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    return val;
}

uint32_t cl_fasl_read_u32(CL_FaslReader *r)
{
    uint32_t val;
    if (r->error) return 0;
    if (r->pos + 4 > r->size) { r->error = FASL_ERR_TRUNCATED; return 0; }
    val = ((uint32_t)r->data[r->pos] << 24) |
          ((uint32_t)r->data[r->pos + 1] << 16) |
          ((uint32_t)r->data[r->pos + 2] << 8) |
          ((uint32_t)r->data[r->pos + 3]);
    r->pos += 4;
    return val;
}

void cl_fasl_read_bytes(CL_FaslReader *r, void *out, uint32_t len)
{
    if (r->error) return;
    if (r->pos + len > r->size) { r->error = FASL_ERR_TRUNCATED; return; }
    memcpy(out, r->data + r->pos, len);
    r->pos += len;
}

/* Sanity-check an element count read from the byte stream.  Each element
 * occupies at least min_bytes of the remaining input, so any larger count
 * means a corrupted/truncated file — and must be rejected HERE: raw u32
 * counts fed to the allocators wrap their alloc_size arithmetic (see the
 * CL_MAX_* caps in mem.h), and FASL_TAG_STRING's own platform_alloc(len+1)
 * wraps to a tiny buffer that cl_fasl_read_bytes then overruns.  Rejecting
 * with BAD_LENGTH gives a clean "deserialize failed" error instead of a
 * storage-error abort (or a heap smash).  Returns 0 and sets r->error on
 * violation. */
static int fasl_check_count(CL_FaslReader *r, uint32_t count,
                            uint32_t min_bytes, uint32_t max_count)
{
    if (count > max_count ||
        (min_bytes > 0 && count > (r->size - r->pos) / min_bytes)) {
        r->error = FASL_ERR_BAD_LENGTH;
        return 0;
    }
    return 1;
}

uint32_t cl_fasl_read_header(CL_FaslReader *r)
{
    uint32_t magic;
    uint16_t version;

    magic = cl_fasl_read_u32(r);
    if (r->error) return 0;
    if (magic != CL_FASL_MAGIC) { r->error = FASL_ERR_BAD_MAGIC; return 0; }

    version = cl_fasl_read_u16(r);
    if (r->error) return 0;
    if (version != CL_FASL_VERSION) { r->error = FASL_ERR_BAD_VERSION; return 0; }

    cl_fasl_read_u16(r);  /* flags — reserved */
    return cl_fasl_read_u32(r);
}

/* --- Deserialize a CL_Obj constant --- */

CL_Obj cl_fasl_deserialize_obj(CL_FaslReader *r)
{
    uint8_t tag;

    if (r->error) return CL_NIL;

    tag = cl_fasl_read_u8(r);
    if (r->error) return CL_NIL;

    switch (tag) {
    case FASL_TAG_NIL:
        return CL_NIL;

    case FASL_TAG_T:
        return CL_T;

    case FASL_TAG_UNBOUND:
        return CL_UNBOUND;

    case FASL_TAG_FIXNUM:
        return cl_fasl_read_u32(r);

    case FASL_TAG_CHARACTER:
        return cl_fasl_read_u32(r);

    case FASL_TAG_SYMBOL: {
        uint16_t pkg_len = cl_fasl_read_u16(r);
        char pkg_buf[256], sym_buf[256];
        uint16_t sym_len;
        CL_Obj pkg_obj;

        if (r->error) return CL_NIL;

        if (pkg_len == 0) {
            /* Uninterned symbol */
            sym_len = cl_fasl_read_u16(r);
            if (r->error || sym_len >= sizeof(sym_buf)) return CL_NIL;
            cl_fasl_read_bytes(r, sym_buf, sym_len);
            sym_buf[sym_len] = '\0';
            return cl_make_symbol(cl_make_string(sym_buf, sym_len));
        }

        if (pkg_len == 0xFFFF) {
            /* Keyword.  Use cl_intern_keyword (not cl_intern_in) so the
             * symbol's value is set to itself and the constant flag is set
             * — otherwise (boundp :foo) returns NIL after loading the
             * FASL, even though plain reader interns set both fields.
             * Caught by ANSI BOUNDP.3 / SYMBOL-NAME.3 once those tests
             * loaded their fixtures from a FASL. */
            sym_len = cl_fasl_read_u16(r);
            if (r->error || sym_len >= sizeof(sym_buf)) return CL_NIL;
            cl_fasl_read_bytes(r, sym_buf, sym_len);
            sym_buf[sym_len] = '\0';
            return cl_intern_keyword(sym_buf, sym_len);
        }

        /* Named package */
        if (pkg_len >= sizeof(pkg_buf)) return CL_NIL;
        cl_fasl_read_bytes(r, pkg_buf, pkg_len);
        pkg_buf[pkg_len] = '\0';

        sym_len = cl_fasl_read_u16(r);
        if (r->error || sym_len >= sizeof(sym_buf)) return CL_NIL;
        cl_fasl_read_bytes(r, sym_buf, sym_len);
        sym_buf[sym_len] = '\0';

        pkg_obj = cl_find_package(pkg_buf, pkg_len);
        if (CL_NULL_P(pkg_obj)) {
            /* Package not found — intern in CL-USER as fallback */
            pkg_obj = cl_find_package("COMMON-LISP-USER", 16);
        }
        return cl_intern_in(sym_buf, sym_len, pkg_obj);
    }

    case FASL_TAG_GENSYM_DEF: {
        uint16_t id = cl_fasl_read_u16(r);
        uint16_t sym_len = cl_fasl_read_u16(r);
        char sym_buf[256];
        CL_Obj sym_obj;
        if (r->error || sym_len >= sizeof(sym_buf)) return CL_NIL;
        cl_fasl_read_bytes(r, sym_buf, sym_len);
        sym_buf[sym_len] = '\0';
        /* The writer allocates gensym ids contiguously (0, 1, 2, ...) — a
         * non-contiguous id means a corrupted/truncated FASL; reject it
         * instead of widening gensym_count over unwritten entries. */
        if (id >= FASL_MAX_GENSYMS || id > r->gensym_count) {
            r->error = FASL_ERR_BAD_TAG;
            return CL_NIL;
        }
        sym_obj = cl_make_symbol(cl_make_string(sym_buf, sym_len));
        r->gensym_objs[id] = sym_obj;
        if (id >= r->gensym_count)
            r->gensym_count = id + 1;
        return sym_obj;
    }

    case FASL_TAG_GENSYM_REF: {
        uint16_t id = cl_fasl_read_u16(r);
        if (r->error || id >= r->gensym_count) return CL_NIL;
        return r->gensym_objs[id];
    }

    case FASL_TAG_OBJ_DEF: {
        uint16_t id = cl_fasl_read_u16(r);
        CL_Obj result;
        if (r->error) return CL_NIL;
        /* Lazily allocate shared table */
        if (!r->shared_objs) {
            r->shared_objs = (CL_Obj *)platform_alloc(FASL_MAX_SHARED * sizeof(CL_Obj));
            if (r->shared_objs) {
                memset(r->shared_objs, 0, FASL_MAX_SHARED * sizeof(CL_Obj));
                fasl_reader_note_shared(r);
            }
        }
        /* Stash the id so allocate-then-fill body deserializers (struct,
         * closure, bytecode) can register their freshly-allocated shell
         * at this id BEFORE recursing into children.  This is required
         * for cyclic graphs (e.g. CLOS gf ↔ method ↔ method-fn-closure)
         * — the writer correctly emits OBJ_REF for self-references during
         * the body walk, but if the reader registers only AFTER the
         * recursive deserialize returns, those mid-body OBJ_REFs resolve
         * to NIL — corrupting slots/upvalues with NIL where a back-
         * reference belongs.  Symptom: runtime "%STRUCT-REF: not a
         * structure" when the corrupted slot is later accessed.
         *
         * If the body deserializer doesn't consume the pending id (string,
         * symbol, bit vector, lock), the fallback below registers here.  A
         * type whose children can be shells themselves (pathname: its
         * directory list) takes the id before reading them even though it
         * cannot be part of a cycle: a child shell written without an
         * OBJ_DEF of its own (writer table full) would claim it. */
        /* Ids restart with every unit of the file while the table lives as
         * long as the reader: clear the previous unit's entry, so "still
         * NIL" below means "this body registered nothing". */
        if (r->shared_objs && id < FASL_MAX_SHARED)
            r->shared_objs[id] = CL_NIL;
        cl_fasl_set_pending_obj_def(r, id);
        result = cl_fasl_deserialize_obj(r);
        /* Body didn't register a shell at this id — register the result now.
         * Asking the table rather than "is the id still pending": a child's
         * own OBJ_DEF (a lock's name string, since v36) sets and clears the
         * pending id on its way through, and the lock then came back
         * unregistered — its OBJ_REFs resolved to NIL.  An unregistered
         * entry is NIL (the table is zeroed); no OBJ_DEF'd object is. */
        if (r->shared_objs && id < FASL_MAX_SHARED &&
            CL_NULL_P(r->shared_objs[id])) {
            r->shared_objs[id] = result;
            if (id >= r->shared_count) r->shared_count = id + 1;
        }
        if (r->pending_obj_def_id == id)
            r->pending_obj_def_id = CL_FASL_NO_PENDING;
        return result;
    }

    case FASL_TAG_CLASS_REF: {
        /* Counterpart to FASL_TAG_CLASS_REF in the writer.  The class is
         * looked up by name at load time so identity matches the live
         * metaobject (defmethod specializers EQ the runtime class). */
        CL_Obj name = cl_fasl_deserialize_obj(r);
        CL_Obj find_class_sym, fn, arg, result;
        CL_Symbol *fc_sym;
        if (r->error || CL_NULL_P(name) ||
            !CL_HEAP_P(name) ||
            CL_HDR_TYPE(CL_OBJ_TO_PTR(name)) != TYPE_SYMBOL)
            return CL_NIL;
        CL_GC_PROTECT(name);
        find_class_sym = cl_intern_in("FIND-CLASS", 10, cl_package_cl);
        fc_sym = (CL_Symbol *)CL_OBJ_TO_PTR(find_class_sym);
        fn = fc_sym->function;
        if (CL_NULL_P(fn)) {
            CL_GC_UNPROTECT(1);
            return CL_NIL;
        }
        arg = name;
        result = cl_vm_apply(fn, &arg, 1);
        CL_GC_UNPROTECT(1);
        return result;
    }

    case FASL_TAG_OBJ_REF: {
        uint16_t id = cl_fasl_read_u16(r);
        if (r->error || !r->shared_objs || id >= r->shared_count) return CL_NIL;
        return r->shared_objs[id];
    }

    case FASL_TAG_LOAD_FORM: {
        /* MAKE-LOAD-FORM reconstruction (writer counterpart: the
         * TYPE_STRUCT case in fasl_ser_step).  Wire layout, under an
         * enclosing OBJ_DEF id:
         *     FASL_TAG_LOAD_FORM | <creation-form data> | <init-form data>
         *
         * 1. Evaluate the creation form to build the object.
         * 2. Register it at the enclosing OBJ_DEF id BEFORE reading the
         *    init form, so the init form's self-reference (an OBJ_REF to
         *    that id) resolves to the freshly-built object — the required
         *    circular self-reference.
         * 3. Evaluate the init form for its slot-setting effect.
         * The forms are plain data; they are compiled and evaluated here,
         * at load time.  Cost is negligible (small forms). */
        uint32_t pending = r->pending_obj_def_id;
        CL_Obj creation, obj, init;

        /* The creation form must not consume the OBJ_DEF id — the object
         * it builds does.  Clear pending across the creation-form read. */
        r->pending_obj_def_id = CL_FASL_NO_PENDING;
        creation = cl_fasl_deserialize_obj(r);
        if (r->error) return CL_NIL;
        CL_GC_PROTECT(creation);
        obj = cl_vm_eval(cl_compile(creation));
        CL_GC_PROTECT(obj);

        /* Register obj at the enclosing OBJ_DEF id so the init form's
         * OBJ_REF self-reference resolves to it. */
        r->pending_obj_def_id = pending;
        cl_fasl_consume_pending_obj_def(r, obj);

        init = cl_fasl_deserialize_obj(r);
        CL_GC_PROTECT(init);
        if (!r->error && !CL_NULL_P(init))
            cl_vm_eval(cl_compile(init));
        CL_GC_UNPROTECT(3);
        return obj;
    }

    case FASL_TAG_STRING: {
        uint32_t len = cl_fasl_read_u32(r);
        char *tmp;
        CL_Obj result;
        if (r->error) return CL_NIL;
        if (!fasl_check_count(r, len, 1, CL_MAX_STRING_CHARS)) return CL_NIL;
        tmp = (char *)platform_alloc(len + 1);
        if (!tmp) return CL_NIL;
        cl_fasl_read_bytes(r, tmp, len);
        tmp[len] = '\0';
        result = cl_make_string(tmp, len);
        platform_free(tmp);
        return result;
    }

#ifdef CL_WIDE_STRINGS
    case FASL_TAG_WIDE_STRING: {
        uint32_t len = cl_fasl_read_u32(r);
        uint32_t *tmp;
        uint32_t i;
        CL_Obj result;
        if (r->error) return CL_NIL;
        if (!fasl_check_count(r, len, 4, CL_MAX_WIDE_STRING_CHARS))
            return CL_NIL;
        tmp = (uint32_t *)platform_alloc(len * sizeof(uint32_t));
        if (!tmp) return CL_NIL;
        for (i = 0; i < len; i++)
            tmp[i] = cl_fasl_read_u32(r);
        result = cl_make_wide_string(tmp, len);
        platform_free(tmp);
        return result;
    }
#endif

    case FASL_TAG_CONS: {
        /* Iterative list building to avoid O(N) GC root stack depth.
         * Consecutive CONS tags are common (proper lists), so we build
         * the list with only 3 GC roots instead of one per element.
         *
         * The writer wraps shared cons heads in OBJ_DEF: when we get
         * here under a pending OBJ_DEF id, we must register the head
         * cell at that id BEFORE recursing into the car — otherwise the
         * car's own deserialize (which can hit OBJ_DEF for any of its
         * children) will overwrite the pending id, and the outer cons
         * head never gets recorded.  Save/clear the pending id around
         * the head allocation, then consume it on the head cell. */
        uint32_t pending_id_save = r->pending_obj_def_id;
        CL_Obj result = CL_NIL, tail = CL_NIL, car_val = CL_NIL;
        int first = 1;
        CL_GC_PROTECT(result);
        CL_GC_PROTECT(tail);
        CL_GC_PROTECT(car_val);

        /* Allocate the head shell first and register it at the pending
         * id (if any).  Subsequent cdr cells in this spine are NOT
         * dedup'd by the writer so they don't need shells registered. */
        r->pending_obj_def_id = CL_FASL_NO_PENDING;
        result = cl_cons(CL_NIL, CL_NIL);
        tail = result;
        r->pending_obj_def_id = pending_id_save;
        cl_fasl_consume_pending_obj_def(r, result);

        do {
            if (!first)
                r->pos++;  /* consume peeked FASL_TAG_CONS */

            car_val = cl_fasl_deserialize_obj(r);

            if (first) {
                /* Re-fetch — cl_cons in cl_fasl_deserialize_obj may have
                 * triggered a GC compaction that moved pointers (the
                 * GC_PROTECT slots track relocations, but raw pointers
                 * dereferenced here would not). */
                ((CL_Cons *)CL_OBJ_TO_PTR(result))->car = car_val;
                first = 0;
            } else {
                CL_Obj new_cell = cl_cons(car_val, CL_NIL);
                ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = new_cell;
                tail = new_cell;
            }
        } while (r->pos < r->size && r->data[r->pos] == FASL_TAG_CONS);

        /* Deserialize final cdr (NIL for proper lists, atom for dotted) */
        {
            CL_Obj final_cdr = cl_fasl_deserialize_obj(r);
            ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = final_cdr;
        }

        CL_GC_UNPROTECT(3);
        return result;
    }

    case FASL_TAG_BYTECODE:
        return cl_fasl_deserialize_bytecode(r);

    case FASL_TAG_SINGLE_FLOAT: {
        uint32_t bits = cl_fasl_read_u32(r);
        float f;
        memcpy(&f, &bits, 4);
        return cl_make_single_float(f);
    }

    case FASL_TAG_DOUBLE_FLOAT: {
        uint8_t bytes[8];
        cl_fasl_read_bytes(r, bytes, 8);
        if (r->error) return CL_NIL;
        return cl_make_double_float(fasl_double_from_be(bytes));
    }

    case FASL_TAG_RATIO: {
        CL_Obj num, den;
        num = cl_fasl_deserialize_obj(r);
        CL_GC_PROTECT(num);
        den = cl_fasl_deserialize_obj(r);
        CL_GC_PROTECT(den);
        {
            CL_Obj result = cl_make_ratio(num, den);
            CL_GC_UNPROTECT(2);
            return result;
        }
    }

    case FASL_TAG_COMPLEX: {
        CL_Obj real, imag;
        real = cl_fasl_deserialize_obj(r);
        CL_GC_PROTECT(real);
        imag = cl_fasl_deserialize_obj(r);
        CL_GC_PROTECT(imag);
        {
            CL_Obj result = cl_make_complex(real, imag);
            CL_GC_UNPROTECT(2);
            return result;
        }
    }

    case FASL_TAG_BIGNUM: {
        uint8_t sign = cl_fasl_read_u8(r);
        uint32_t n_limbs = cl_fasl_read_u32(r);
        CL_Obj result;
        CL_Bignum *bn;
        uint32_t i;
        if (r->error) return CL_NIL;
        if (!fasl_check_count(r, n_limbs, 2, CL_MAX_BIGNUM_LIMBS))
            return CL_NIL;
        result = cl_make_bignum(n_limbs, sign);
        bn = (CL_Bignum *)CL_OBJ_TO_PTR(result);
        for (i = 0; i < n_limbs; i++)
            bn->limbs[i] = cl_fasl_read_u16(r);
        return result;
    }

    case FASL_TAG_VECTOR: {
        uint32_t len = cl_fasl_read_u32(r);
        CL_Obj result;
        CL_Vector *v;
        uint32_t i;
        if (r->error) return CL_NIL;
        /* Each element is at least a 1-byte tag in the remaining input. */
        if (!fasl_check_count(r, len, 1, CL_MAX_VECTOR_ELTS)) return CL_NIL;
        result = cl_make_vector(len);
        cl_fasl_consume_pending_obj_def(r, result);   /* #1=#(... #1# ...) */
        CL_GC_PROTECT(result);
        v = (CL_Vector *)CL_OBJ_TO_PTR(result);
        for (i = 0; i < len; i++) {
            CL_Obj elt = cl_fasl_deserialize_obj(r);
            /* Re-fetch v since GC may have occurred */
            v = (CL_Vector *)CL_OBJ_TO_PTR(result);
            v->data[i] = elt;
        }
        CL_GC_UNPROTECT(1);
        return result;
    }

    case FASL_TAG_MD_ARRAY: {
        uint32_t len = cl_fasl_read_u32(r);
        uint8_t rank = cl_fasl_read_u8(r);
        uint32_t dims[16];   /* matches MAKE-ARRAY's dims buffer (rank<=16) */
        CL_Obj result;
        CL_Vector *v;
        uint32_t i;
        uint8_t k;
        if (rank < 2 || rank > 16) { r->error = 1; return CL_NIL; }
        for (k = 0; k < rank; k++)
            dims[k] = cl_fasl_read_u32(r);
        if (r->error) return CL_NIL;
        if (!fasl_check_count(r, len, 1, CL_MAX_VECTOR_ELTS)) return CL_NIL;
        /* len must equal the dims product (overflow-guarded) — a mismatch
         * means a corrupt file whose element loop would run past the
         * allocated total. */
        {
            uint32_t prod = 1;
            for (k = 0; k < rank; k++) {
                if (dims[k] != 0 && prod > CL_MAX_VECTOR_ELTS / dims[k]) {
                    r->error = FASL_ERR_BAD_LENGTH;
                    return CL_NIL;
                }
                prod *= dims[k];
            }
            if (prod != len) { r->error = FASL_ERR_BAD_LENGTH; return CL_NIL; }
        }
        result = cl_make_array(len, rank, dims, CL_VEC_FLAG_MULTIDIM,
                               CL_NO_FILL_POINTER);
        cl_fasl_consume_pending_obj_def(r, result);
        CL_GC_PROTECT(result);
        for (i = 0; i < len; i++) {
            CL_Obj elt = cl_fasl_deserialize_obj(r);
            /* Re-fetch v + element base since GC may have moved the array. */
            v = (CL_Vector *)CL_OBJ_TO_PTR(result);
            cl_vector_data(v)[i] = elt;
        }
        CL_GC_UNPROTECT(1);
        return result;
    }

    case FASL_TAG_BIT_VECTOR: {
        uint32_t nbits = cl_fasl_read_u32(r);
        uint32_t n_words;
        CL_Obj result;
        CL_BitVector *bv;
        uint32_t i;
        if (r->error) return CL_NIL;
        /* Cap nbits BEFORE CL_BV_WORDS (whose `+ 31` wraps near
         * UINT32_MAX), then require the words to fit the remaining input. */
        if (!fasl_check_count(r, nbits, 0, CL_MAX_BV_BITS)) return CL_NIL;
        n_words = CL_BV_WORDS(nbits);
        if (!fasl_check_count(r, n_words, 4, 0xFFFFFFFFu)) return CL_NIL;
        result = cl_make_bit_vector(nbits);
        bv = (CL_BitVector *)CL_OBJ_TO_PTR(result);
        for (i = 0; i < n_words; i++)
            bv->data[i] = cl_fasl_read_u32(r);
        return result;
    }

    case FASL_TAG_BYTE_VECTOR: {
        uint32_t blen = cl_fasl_read_u32(r);
        uint8_t is_signed = cl_fasl_read_u8(r);
        uint8_t elt_shift = cl_fasl_read_u8(r);
        CL_Obj result;
        CL_ByteVector *bv;
        if (r->error) return CL_NIL;
        if (elt_shift > 1) { r->error = FASL_ERR_BAD_LENGTH; return CL_NIL; }
        /* Cap the raw u32-from-disk length before allocating, and require
         * the bytes to actually be present in the remaining input.  The
         * per-element byte width scales both checks. */
        if (!fasl_check_count(r, blen, 1u << elt_shift,
                              CL_MAX_BYTEVEC_BYTES >> elt_shift))
            return CL_NIL;
        result = cl_make_byte_vector(blen, is_signed != 0, elt_shift);
        bv = (CL_ByteVector *)CL_OBJ_TO_PTR(result);
        if (elt_shift) {
            /* 16-bit elements are stored big-endian on the wire (see the
             * writer) — decode via read_u16 so LE hosts byte-swap. */
            uint32_t i;
            for (i = 0; i < blen; i++)
                ((uint16_t *)(void *)bv->data)[i] = cl_fasl_read_u16(r);
        } else {
            cl_fasl_read_bytes(r, bv->data, blen);
        }
        return result;
    }

    case FASL_TAG_PATHNAME: {
        CL_Obj components[6];
        int i;
        CL_Obj result;
        /* No shell to register ahead of the components; take the id so they
         * cannot clobber or claim it (see FASL_TAG_OBJ_DEF). */
        uint32_t def_id = r->pending_obj_def_id;
        r->pending_obj_def_id = CL_FASL_NO_PENDING;
        for (i = 0; i < 6; i++) {
            components[i] = cl_fasl_deserialize_obj(r);
            CL_GC_PROTECT(components[i]);
        }
        result = cl_make_pathname(components[0], components[1],
                                  components[2], components[3],
                                  components[4], components[5]);
        CL_GC_UNPROTECT(6);
        r->pending_obj_def_id = def_id;
        cl_fasl_consume_pending_obj_def(r, result);
        return result;
    }

    case FASL_TAG_STRUCT: {
        CL_Obj type_desc;
        uint32_t pending_id_save = r->pending_obj_def_id;
        uint32_t n_slots, i;
        CL_Obj st_obj;
        /* type_desc is read first; clear pending so it doesn't apply to
         * the type_desc symbol/struct.  We restore it before consuming. */
        r->pending_obj_def_id = CL_FASL_NO_PENDING;
        type_desc = cl_fasl_deserialize_obj(r);
        n_slots = cl_fasl_read_u32(r);
        if (r->error) return CL_NIL;
        /* Each slot is at least a 1-byte tag in the remaining input. */
        if (!fasl_check_count(r, n_slots, 1, CL_MAX_STRUCT_SLOTS))
            return CL_NIL;
        CL_GC_PROTECT(type_desc);
        st_obj = cl_make_struct(type_desc, n_slots);
        CL_GC_PROTECT(st_obj);
        /* Register shell at the OBJ_DEF id (if any) before reading slots
         * so cyclic OBJ_REFs in slots resolve to this shell. */
        r->pending_obj_def_id = pending_id_save;
        cl_fasl_consume_pending_obj_def(r, st_obj);
        {
            CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(st_obj);
            for (i = 0; i < n_slots; i++) {
                CL_Obj val = cl_fasl_deserialize_obj(r);
                /* Re-fetch pointer — GC may have moved during deserialization */
                st = (CL_Struct *)CL_OBJ_TO_PTR(st_obj);
                st->slots[i] = val;
            }
        }
        CL_GC_UNPROTECT(2);
        return st_obj;
    }

    case FASL_TAG_CLOSURE: {
        CL_Obj bc_val;
        uint16_t n_upvals;
        uint32_t pending_id_save = r->pending_obj_def_id;
        CL_Closure *cl;
        CL_Obj result;
        uint16_t i;
        /* Clear pending while reading the bytecode child; restore before
         * consuming so the OBJ_DEF id applies to this closure shell. */
        r->pending_obj_def_id = CL_FASL_NO_PENDING;
        bc_val = cl_fasl_deserialize_obj(r);
        n_upvals = cl_fasl_read_u16(r);
        if (r->error) return CL_NIL;
        CL_GC_PROTECT(bc_val);
        cl = (CL_Closure *)cl_alloc(TYPE_CLOSURE,
            sizeof(CL_Closure) + n_upvals * sizeof(CL_Obj));
        if (!cl) { CL_GC_UNPROTECT(1); return CL_NIL; }
        cl->bytecode = bc_val;
        result = CL_PTR_TO_OBJ(cl);
        CL_GC_PROTECT(result);
        /* Register shell at the OBJ_DEF id before reading upvalues. */
        r->pending_obj_def_id = pending_id_save;
        cl_fasl_consume_pending_obj_def(r, result);
        for (i = 0; i < n_upvals; i++) {
            CL_Obj uv = cl_fasl_deserialize_obj(r);
            cl = (CL_Closure *)CL_OBJ_TO_PTR(result); /* refresh */
            cl->upvalues[i] = uv;
        }
        CL_GC_UNPROTECT(2);
        return result;
    }

    case FASL_TAG_PACKAGE: {
        uint16_t name_len = cl_fasl_read_u16(r);
        char pkg_buf[256];
        if (r->error || name_len >= sizeof(pkg_buf)) return CL_NIL;
        cl_fasl_read_bytes(r, pkg_buf, name_len);
        pkg_buf[name_len] = '\0';
        return cl_find_package(pkg_buf, name_len);
    }

    case FASL_TAG_LOCK: {
        /* Counterpart to FASL_TAG_LOCK in the writer.  We can't restore
         * the original platform mutex (per-process state) — instead we
         * allocate a fresh, equivalent lock with the same name and
         * recursive-ness.  Sharing within this FASL is preserved by the
         * surrounding OBJ_DEF/OBJ_REF wrapper. */
        uint8_t flags = cl_fasl_read_u8(r);
        CL_Obj name = cl_fasl_deserialize_obj(r);
        CL_Obj lock_obj;
        if (r->error) return CL_NIL;
        CL_GC_PROTECT(name);
        lock_obj = cl_lock_alloc_obj((flags & 0x01) ? 1 : 0, name, "FASL");
        CL_GC_UNPROTECT(1);
        return lock_obj;
    }

    case FASL_TAG_FFI_STUB: {
        /* Counterpart to TYPE_FFI_STUB in the writer: kind, ctype, b, a,
         * then the name and aux objects; rebuilt as a fresh descriptor. */
        uint8_t kind = cl_fasl_read_u8(r);
        uint8_t ctype = cl_fasl_read_u8(r);
        int16_t b = (int16_t)cl_fasl_read_u16(r);
        uint32_t a = cl_fasl_read_u32(r);
        CL_Obj name, aux, stub;
        if (r->error) return CL_NIL;
        if (kind > CL_STUB_KIND_MAX ||
            (kind != CL_STUB_LIBCALL && ctype > CL_STUB_CT_MAX) ||
            (kind == CL_STUB_LIBCALL && ctype > 7)) {
            r->error = FASL_ERR_BAD_TAG;   /* corrupt descriptor */
            return CL_NIL;
        }
        name = cl_fasl_deserialize_obj(r);
        if (r->error) return CL_NIL;
        CL_GC_PROTECT(name);
        aux = cl_fasl_deserialize_obj(r);
        if (r->error) { CL_GC_UNPROTECT(1); return CL_NIL; }
        stub = cl_make_ffi_stub(kind, name, aux, a, b, ctype);
        CL_GC_UNPROTECT(1);
        return stub;
    }

    case FASL_TAG_FUNCTION: {
        /* C builtin — deserialize name symbol and look up its function */
        CL_Obj name_sym = cl_fasl_deserialize_obj(r);
        if (r->error || CL_NULL_P(name_sym)) return CL_NIL;
        if (CL_HEAP_P(name_sym) &&
            CL_HDR_TYPE(CL_OBJ_TO_PTR(name_sym)) == TYPE_SYMBOL) {
            CL_Symbol *sym = (CL_Symbol *)CL_OBJ_TO_PTR(name_sym);
            if (!CL_NULL_P(sym->function))
                return sym->function;
        }
        return CL_NIL;
    }

    default:
        r->error = FASL_ERR_BAD_TAG;
        return CL_NIL;
    }
}

/* --- Deserialize a CL_Bytecode --- */

CL_Obj cl_fasl_deserialize_bytecode(CL_FaslReader *r)
{
    CL_Bytecode *bc;
    CL_Obj bc_obj;
    uint32_t code_len;
    uint16_t n_consts, i;
    uint16_t source_file_len;

    if (r->error) return CL_NIL;

    /* Allocate bytecode on heap */
    bc = (CL_Bytecode *)cl_alloc(TYPE_BYTECODE, sizeof(CL_Bytecode));
    if (!bc) return CL_NIL;
    bc_obj = CL_PTR_TO_OBJ(bc);
    CL_GC_PROTECT(bc_obj);

    /* Register shell at OBJ_DEF id (if any) before reading constants —
     * a closure inside our constant pool may OBJ_REF this bytecode. */
    cl_fasl_consume_pending_obj_def(r, bc_obj);

    /* Zero out all fields */
    bc->code = NULL;
    bc->constants = NULL;
    bc->code_len = 0;
    bc->n_constants = 0;
    bc->arity = 0;
    bc->n_locals = 0;
    bc->n_upvalues = 0;
    bc->name = CL_NIL;
    bc->n_optional = 0;
    bc->flags = 0;
    bc->n_keys = 0;
    bc->key_syms = NULL;
    bc->key_slots = NULL;
    bc->key_suppliedp_slots = NULL;
    bc->line_map = NULL;
    bc->line_map_count = 0;
    bc->source_line = 0;
    bc->source_file = NULL;
    bc->source_lambda_list = CL_NIL;

    /* Code */
    code_len = cl_fasl_read_u32(r);
    if (r->error) { CL_GC_UNPROTECT(1); return CL_NIL; }

    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj); /* refresh after potential GC */
    bc->code = (uint8_t *)platform_alloc(code_len);
    if (!bc->code) { CL_GC_UNPROTECT(1); return CL_NIL; }
    cl_fasl_read_bytes(r, bc->code, code_len);
    bc->code_len = code_len;

    /* Constants */
    n_consts = cl_fasl_read_u16(r);
    if (r->error) { CL_GC_UNPROTECT(1); return CL_NIL; }

    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
    if (n_consts > 0) {
        bc->constants = (CL_Obj *)platform_alloc(n_consts * sizeof(CL_Obj));
        if (!bc->constants) { CL_GC_UNPROTECT(1); return CL_NIL; }
        bc->n_constants = n_consts;
        /* Initialize to NIL so GC doesn't see garbage */
        for (i = 0; i < n_consts; i++)
            bc->constants[i] = CL_NIL;
    }

    for (i = 0; i < n_consts; i++) {
        CL_Obj val;
        /* Check for back-reference to earlier constant (dedup) */
        if (!r->error && r->pos < r->size &&
            r->data[r->pos] == FASL_TAG_CONST_REF) {
            uint16_t ref_idx;
            cl_fasl_read_u8(r);  /* consume tag */
            ref_idx = cl_fasl_read_u16(r);
            if (r->error) { CL_GC_UNPROTECT(1); return CL_NIL; }
            bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
            bc->constants[i] = bc->constants[ref_idx];
            continue;
        }
        val = cl_fasl_deserialize_obj(r);
        if (r->error) { CL_GC_UNPROTECT(1); return CL_NIL; }
        bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj); /* refresh */
        bc->constants[i] = val;
    }

    /* Metadata */
    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
    bc->arity = cl_fasl_read_u16(r);
    bc->n_locals = cl_fasl_read_u16(r);
    bc->n_upvalues = cl_fasl_read_u16(r);
    bc->n_optional = cl_fasl_read_u8(r);
    {
        uint8_t wire = cl_fasl_read_u8(r);
        bc->flags = (uint8_t)(wire & 0x7F);
        bc->jit_hot = (wire & 0x80) ? CL_BC_JIT_SPEED : 0;
    }
    bc->n_keys = cl_fasl_read_u8(r);

    /* Key params */
    if (bc->n_keys > 0) {
        uint8_t nk = bc->n_keys;
        bc->key_syms = (CL_Obj *)platform_alloc(nk * sizeof(CL_Obj));
        bc->key_slots = (uint8_t *)platform_alloc(nk);
        bc->key_suppliedp_slots = (uint8_t *)platform_alloc(nk);
        if (!bc->key_syms || !bc->key_slots || !bc->key_suppliedp_slots) {
            CL_GC_UNPROTECT(1);
            return CL_NIL;
        }
        /* Initialize key_syms to NIL */
        for (i = 0; i < nk; i++)
            bc->key_syms[i] = CL_NIL;

        for (i = 0; i < nk; i++) {
            CL_Obj ksym = cl_fasl_deserialize_obj(r);
            bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
            bc->key_syms[i] = ksym;
        }
        bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
        for (i = 0; i < nk; i++)
            bc->key_slots[i] = cl_fasl_read_u8(r);
        for (i = 0; i < nk; i++)
            bc->key_suppliedp_slots[i] = cl_fasl_read_u8(r);
    }

    /* Source info */
    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
    bc->source_line = cl_fasl_read_u16(r);
    source_file_len = cl_fasl_read_u16(r);
    if (source_file_len > 0) {
        /* Read into a scratch buffer, then INTERN.  bc->source_file must
         * point into the shared source-file pool, the same as the compiler
         * (compiler.c) and image restore (image.c) produce: the pool owns
         * those strings and frees them once at shutdown, which is why
         * bytecode_release_offheap must not free the field per object.
         *
         * Keeping the raw copy here instead — as this did — was both a leak
         * and a waste: nothing ever freed it, and every function in a FASL
         * got its own copy of the same path, so loading boot.fasl + clos.fasl
         * alone carried ~1400 duplicates of two strings (~104 KB, measured
         * with DEBUG_MEM_TRACK).  Interning makes it two. */
        char *sf = (char *)platform_alloc(source_file_len + 1);
        if (sf) {
            cl_fasl_read_bytes(r, sf, source_file_len);
            sf[source_file_len] = '\0';
            bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
            bc->source_file = cl_intern_source_file(sf);
            platform_free(sf);
        } else {
            /* Out of memory for the scratch buffer: the name is forfeit, but
             * the bytes still have to be consumed or every later field in
             * this FASL is read from the wrong offset. */
            r->pos += source_file_len;
            if (r->pos > r->size) { r->pos = r->size; r->error = FASL_ERR_TRUNCATED; }
        }
    }

    /* Line map */
    {
        uint16_t lm_count;
        bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
        lm_count = cl_fasl_read_u16(r);
        if (lm_count > 0) {
            bc->line_map = (CL_LineEntry *)platform_alloc(
                lm_count * sizeof(CL_LineEntry));
            bc->line_map_count = lm_count;
            if (bc->line_map) {
                for (i = 0; i < lm_count; i++) {
                    bc->line_map[i].pc = cl_fasl_read_u16(r);
                    bc->line_map[i].line = cl_fasl_read_u16(r);
                }
            }
        }
    }

    /* Source lambda-list (v9) — written before the name by the serializer. */
    {
        CL_Obj sll = cl_fasl_deserialize_obj(r);
        bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
        bc->source_lambda_list = sll;
    }

    /* Name */
    {
        CL_Obj name = cl_fasl_deserialize_obj(r);
        bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
        bc->name = name;
    }

    /* Give the JIT a chance at FASL-loaded code too — the source-load
     * compile path (compiler.c) already hooks the JIT, but functions read
     * from a FASL bypass that and would otherwise always run as bytecode.
     * Eager or (speed 3) compiles here, anything else once it turns hot. */
    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
    cl_jit_note_definition(bc);

    CL_GC_UNPROTECT(1);
    return bc_obj;
}

/* ================================================================
 * DEPS trailer — structure layouts the compiled code depends on
 * ================================================================
 *
 * Wire format, after the last unit, present when the header carries
 * CL_FASL_FLAG_DEPS:
 *
 *   magic:  4 bytes  "DEPS"
 *   count:  2 bytes
 *   per entry:
 *     u16 pkg_len,  bytes   home package name of the structure's name
 *     u16 name_len, bytes   symbol name
 *     u32 hash              cl_struct_layout_hash at compile time
 *
 * Names travel as plain bytes, not as FASL_TAG_SYMBOL: the check runs
 * before any unit, when the package may not exist yet (the file's own
 * DEFPACKAGE is one of its units), and it must never intern or signal.
 * See builtins_struct.c, "Layout dependencies of compiled code". */

#define FASL_DEPS_MAGIC 0x44455053u  /* "DEPS" */

/* 1 when NAME can be written as bytes: the structure is registered (so it
 * has a hash), its name is interned, and both names are 8-bit strings that
 * fit a u16 length. */
static int fasl_dep_writable(CL_Obj name, uint32_t *hash_out,
                             CL_Obj *pkg_name_out, CL_Obj *sym_name_out)
{
    CL_Symbol *s;
    CL_Obj strs[2];
    int k;

    if (!CL_SYMBOL_P(name) || CL_NULL_P(name)) return 0;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(name);
    if (CL_NULL_P(s->package)) return 0;
    strs[0] = ((CL_Package *)CL_OBJ_TO_PTR(s->package))->name;
    strs[1] = s->name;
    for (k = 0; k < 2; k++) {
        uint32_t i, len;
        if (!CL_ANY_STRING_P(strs[k])) return 0;
        len = cl_string_length(strs[k]);
        if (len == 0 || len > 0xFFFFu) return 0;
        for (i = 0; i < len; i++)
            if (cl_string_char_at(strs[k], i) > 0xFF) return 0;
    }
    if (!cl_struct_layout_hash(name, hash_out)) return 0;
    *pkg_name_out = strs[0];
    *sym_name_out = strs[1];
    return 1;
}

/* One pass over DEPS: sizes the trailer (W == NULL) or writes its entries.
 * Returns the trailer's byte size, 0 when no entry is writable. */
static uint32_t fasl_deps_walk(CL_FaslWriter *w, CL_Obj deps,
                               uint32_t *count_out)
{
    uint32_t bytes = 6, count = 0;
    CL_Obj p;

    for (p = deps; CL_CONS_P(p); p = cl_cdr(p)) {
        uint32_t hash, i, len;
        CL_Obj strs[2];
        int k;
        if (count == 0xFFFFu) break;
        if (!fasl_dep_writable(cl_car(p), &hash, &strs[0], &strs[1])) continue;
        count++;
        for (k = 0; k < 2; k++) {
            len = cl_string_length(strs[k]);
            bytes += 2 + len;
            if (w) {
                cl_fasl_write_u16(w, (uint16_t)len);
                for (i = 0; i < len; i++)
                    cl_fasl_write_u8(w, (uint8_t)cl_string_char_at(strs[k], i));
            }
        }
        bytes += 4;
        if (w) cl_fasl_write_u32(w, hash);
    }
    if (count_out) *count_out = count;
    return count ? bytes : 0;
}

uint32_t cl_fasl_deps_size(CL_Obj deps)
{
    return fasl_deps_walk(NULL, deps, NULL);
}

void cl_fasl_write_deps(CL_FaslWriter *w, CL_Obj deps)
{
    uint32_t count = 0;

    if (fasl_deps_walk(NULL, deps, &count) == 0) return;
    cl_fasl_write_u32(w, FASL_DEPS_MAGIC);
    cl_fasl_write_u16(w, (uint16_t)count);
    fasl_deps_walk(w, deps, NULL);
    /* The header was written first; flag the trailer now that it exists.
     * flags is the big-endian u16 at bytes 6-7. */
    if (w->error == FASL_OK && w->pos >= 12)
        w->data[7] |= CL_FASL_FLAG_DEPS;
}

/* Compare a FASL's DEPS trailer against the live structure registry.
 * Returns 1 when the file may be loaded: no trailer, or every dependency
 * that is registered NOW still has the layout the file was compiled with.
 * A structure (or its package) that does not exist yet is not a mismatch —
 * a file that defines the structure it uses is the normal case.  Returns 0
 * on the first mismatch and names the structure in DETAIL.  A malformed
 * trailer counts as "no trailer": the unit loader reports real damage.
 * Works on the raw buffer; never interns, signals or touches the reader
 * registry. */
int cl_fasl_check_deps(const uint8_t *data, uint32_t size,
                       char *detail, uint32_t detail_size)
{
    CL_FaslReader r;
    uint32_t n_units, i, count;
    uint16_t flags;

    if (detail && detail_size) detail[0] = '\0';
    if (size < 12) return 1;
    flags = (uint16_t)(((uint16_t)data[6] << 8) | data[7]);
    if (!(flags & CL_FASL_FLAG_DEPS)) return 1;

    /* Only pos/size/error are used: the big dedup tables stay untouched. */
    r.data = data; r.size = size; r.pos = 8; r.error = FASL_OK;
    n_units = cl_fasl_read_u32(&r);
    for (i = 0; i < n_units; i++) {
        uint32_t len = cl_fasl_read_u32(&r);
        if (r.error || len > r.size - r.pos) return 1;
        r.pos += len;
    }
    if (cl_fasl_read_u32(&r) != FASL_DEPS_MAGIC || r.error) return 1;
    count = cl_fasl_read_u16(&r);

    for (i = 0; i < count; i++) {
        const uint8_t *pkg_name, *sym_name;
        uint32_t pkg_len, sym_len, want, have;
        CL_Obj pkg, sym;

        pkg_len = cl_fasl_read_u16(&r);
        if (r.error || pkg_len > r.size - r.pos) return 1;
        pkg_name = r.data + r.pos; r.pos += pkg_len;
        sym_len = cl_fasl_read_u16(&r);
        if (r.error || sym_len > r.size - r.pos) return 1;
        sym_name = r.data + r.pos; r.pos += sym_len;
        want = cl_fasl_read_u32(&r);
        if (r.error) return 1;

        pkg = cl_find_package((const char *)pkg_name, pkg_len);
        if (CL_NULL_P(pkg)) continue;
        sym = cl_package_find_symbol((const char *)sym_name, sym_len, pkg);
        if (CL_NULL_P(sym) || !CL_SYMBOL_P(sym)) continue;
        if (!cl_struct_layout_hash(sym, &have)) continue;
        if (have != want) {
            if (detail && detail_size)
                snprintf(detail, detail_size, "%.*s::%.*s",
                         (int)(pkg_len > 60 ? 60 : pkg_len), (const char *)pkg_name,
                         (int)(sym_len > 80 ? 80 : sym_len), (const char *)sym_name);
            return 0;
        }
    }
    return 1;
}

/* ================================================================
 * High-level FASL load
 * ================================================================ */

/* Map FASL_ERR_* code to a short name for diagnostics. */
static const char *fasl_err_name(int err)
{
    switch (err) {
    case FASL_ERR_OVERFLOW:    return "OVERFLOW";
    case FASL_ERR_TRUNCATED:   return "TRUNCATED";
    case FASL_ERR_BAD_MAGIC:   return "BAD_MAGIC";
    case FASL_ERR_BAD_VERSION: return "BAD_VERSION";
    case FASL_ERR_BAD_TAG:     return "BAD_TAG";
    case FASL_ERR_TOO_DEEP:    return "TOO_DEEP";
    case FASL_ERR_BAD_LENGTH:  return "BAD_LENGTH";
    default:                   return "UNKNOWN";
    }
}

static CL_Obj fasl_load(const uint8_t *data, uint32_t size, int check_deps);

CL_Obj cl_fasl_load(const uint8_t *data, uint32_t size)
{
    return fasl_load(data, size, 1);
}

CL_Obj cl_fasl_load_prechecked(const uint8_t *data, uint32_t size)
{
    return fasl_load(data, size, 0);
}

/* fasl_load is about to signal an error of its own: nothing below returns
 * here, so release the reader first.  (An error raised from INSIDE a unit
 * cannot do this; the error-frame restore and the NLX-landing unwind drop
 * that reader — and free its table — from the registry instead.) */
static void fasl_load_abandon(CL_FaslReader *r)
{
    cl_fasl_reader_unregister(r);
    if (r->shared_objs) { platform_free(r->shared_objs); r->shared_objs = NULL; r->shared_count = 0; }
}

static CL_Obj fasl_load(const uint8_t *data, uint32_t size, int check_deps)
{
    CL_FaslReader r;
    uint32_t n_units, i;

    cl_fasl_reader_init(&r, data, size);
    /* Root the reader's dedup tables for the whole load: forward GENSYM_REF /
     * OBJ_REF targets survive compaction triggered by any allocating
     * deserialize below.  Registered before the header read so an early error
     * path still unregisters via the single exit point. */
    cl_fasl_reader_register(&r);
    n_units = cl_fasl_read_header(&r);
    if (r.error) {
        int hdr_err = r.error;
        fasl_load_abandon(&r);
        switch (hdr_err) {
        case FASL_ERR_BAD_MAGIC:
            cl_error(CL_ERR_GENERAL, "FASL: invalid magic number");
            break;
        case FASL_ERR_BAD_VERSION:
            cl_error(CL_ERR_GENERAL, "FASL: unsupported version");
            break;
        default:
            cl_error(CL_ERR_GENERAL, "FASL: header read error");
            break;
        }
        return CL_NIL;  /* not reached */
    }

    /* Before any unit runs: the code must still match the structure
     * layouts it was compiled against.  LOAD's cache path checks this
     * itself first and recompiles quietly; reaching it here means a FASL
     * somebody asked for by name. */
    if (check_deps) {
        char dep[160];
        if (!cl_fasl_check_deps(data, size, dep, sizeof(dep))) {
            fasl_load_abandon(&r);
            cl_error(CL_ERR_GENERAL,
                     "FASL is stale: it was compiled against a different layout "
                     "of structure %s (its DEFSTRUCT changed since).  Inlined "
                     "slot accesses would hit the wrong slots - recompile the "
                     "file that uses it", dep);
            return CL_NIL;
        }
    }

    for (i = 0; i < n_units; i++) {
        CL_Obj bc_obj;
        uint32_t unit_start = r.pos;
        cl_fasl_read_u32(&r); /* skip unit length */

        if (r.error) {
            fasl_load_abandon(&r);
            cl_error(CL_ERR_GENERAL,
                     "FASL: truncated unit header at unit %u/%u (pos %u, size %u)",
                     (unsigned)i, (unsigned)n_units,
                     (unsigned)r.pos, (unsigned)r.size);
            return CL_NIL;
        }

        bc_obj = cl_fasl_deserialize_bytecode(&r);
        if (r.error) {
            int des_err = r.error;
            fasl_load_abandon(&r);
            cl_error(CL_ERR_GENERAL,
                     "FASL: deserialize failed: unit %u/%u %s at pos %u (unit_start %u, size %u)",
                     (unsigned)i, (unsigned)n_units,
                     fasl_err_name(des_err),
                     (unsigned)r.pos, (unsigned)unit_start, (unsigned)r.size);
            return CL_NIL;
        }

        /* One unit is one top-level form: drop the frame snapshot a
         * previous unit's handled error left, or a later EXT:BACKTRACE
         * reports a truncated window (the source loaders do the same). */
        cl_debug_base_fp = 0;
        if (!CL_NULL_P(bc_obj))
            cl_vm_eval(bc_obj);
    }

    cl_fasl_reader_unregister(&r);
    if (r.shared_objs) platform_free(r.shared_objs);
    return CL_T;
}
