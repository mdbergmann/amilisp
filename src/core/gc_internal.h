#ifndef CL_GC_INTERNAL_H
#define CL_GC_INTERNAL_H

/*
 * Shared state and entry points of the collector's translation units
 * (mem.c, gc_trace.c, gc_classic.c, gc_compact.c, gc_jitscan.c,
 * gc_verify.c).  Everything here was file-static in the one-file mem.c
 * and is still internal to the runtime: nothing outside src/core/gc_*.c
 * and mem.c may include this header.  The backend seam itself — what the
 * heap core and the shared facilities may ask of the collector — is
 * gc_backend.h; a section below is CL_GC_CLASSIC-only state.
 */

#include "mem.h"
#include "gc_backend.h"
#include "error.h"
#include "float.h"
#include "vm.h"
#include "readtable.h"
#include "package.h"
#include "stream.h"
#include "reader.h"   /* cl_srcloc_table (GC invalidate/forward hooks) */
#include "../jit/jit.h"    /* cl_jit_free_native */
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <stdarg.h>
#include <stdio.h>    /* CLAMIGA_GC_DIAG per-collection trace (fprintf/stderr) */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef DEBUG_GC_STRESS_CBT
#include <execinfo.h>
#endif

/* External roots needed for GC marking */
extern CL_Obj macro_table, setf_table, setf_fn_table, setf_expander_table, type_table, compiler_macro_table;
extern CL_Obj cl_clos_class_table;
extern CL_Obj struct_table;  /* builtins_struct.c: struct type registry */
extern CL_Obj condition_hierarchy;        /* builtins_condition.c */
extern CL_Obj condition_slot_table;       /* builtins_condition.c */
extern CL_Obj condition_default_initargs; /* builtins_condition.c */
extern CL_Obj condition_slot_initforms;   /* builtins_condition.c */

/* Active compiler chain is accessed via cl_active_compiler macro (thread.h) */
typedef struct CL_Compiler_s CL_Compiler;

/* thread.h defines `pending_lambda_name` as a macro for `(CT->pending_lambda_name)`,
 * but the GC walks every thread via an explicit `t->` pointer (not CT), so we
 * must reference the raw struct member.  Drop the macro here — mem.c only ever
 * touches this field through `t->pending_lambda_name`, never the macro form. */
#undef pending_lambda_name

/* STW GC coordination (defined in thread.c) */
extern void cl_gc_stop_the_world(void);
extern void cl_gc_resume_the_world(void);

/* --- mem.c ------------------------------------------------------------ */

extern void *alloc_mutex;   /* bump pointer, free list, heap metadata */

/* Cumulative GC phase timers and stop-the-world counters (see mem.c). */
extern uint64_t gc_time_stw_us;
extern uint64_t gc_time_mark_us;
extern uint64_t gc_time_sweep_us;
extern uint64_t gc_time_compact_us;
extern uint64_t gc_time_stw_max_us;
extern uint32_t gc_stw_stops;
extern uint32_t gc_epoch_skips;

int  gc_diag_on(void);
void gc_diag_report(const char *kind, uint64_t pause_us);

/* Stop the world (multi-threaded), accounting the wait into the STW
 * timers, bump the call generation, and return the timestamp at which
 * the world was stopped.  Every collection of every backend enters
 * through it. */
uint64_t gc_stop_world_timed(int multithread);

/* Registered global roots (mem.c). */
extern CL_Obj *global_roots[CL_MAX_GLOBAL_ROOTS];
extern int n_global_roots;
#ifdef DEBUG_GC
extern const char *global_root_files[CL_MAX_GLOBAL_ROOTS];
extern int         global_root_lines[CL_MAX_GLOBAL_ROOTS];
#endif

/* Drop one thread's chunk cursor (flush its deferred accounting; the
 * remainder is already a formatted hole), and every registered thread's
 * (mem.c).  Every collection calls the latter first thing after stopping
 * the world: the passes below rebuild the free list and move the bump
 * front under the cursors.  The caller of gc_cursor_drop serializes
 * against the owner: stop-the-world, thread retirement, or the owner. */
void gc_cursor_drop(CL_Thread *t);
void gc_cursor_drop_all(void);
int  cl_bump_fits(uint32_t bump, uint32_t size, uint32_t arena_size);

/* --- gc_trace.c ------------------------------------------------------- */

extern CL_Obj  *gc_mark_stack;
extern uint32_t gc_mark_stack_cap;
extern uint32_t gc_mark_top;
extern int gc_mark_overflow;
extern int gc_mark_grow_failed;
extern uint32_t gc_mark_stack_grows;
extern uint32_t gc_mark_rescan_passes;
extern uint32_t gc_bc_offheap_bytes;
extern uint32_t gc_bc_offheap_count;
extern uint32_t gc_mark_stack_test_limit;

void gc_mark_stack_release(void);
void gc_mark_obj(CL_Obj obj);
void gc_mark_children(void *ptr, uint8_t type);
void gc_mark(void);

/* Root enumeration in slot-visitor form (see gc_trace.c): the one list of
 * non-arena CL_Obj slots, instantiated by the mark phase (mark the value)
 * and by the compactor (forward the slot in place). */
typedef void (*gc_slot_fn)(CL_Obj *slot);
void gc_visit_thread_roots(CL_Thread *t, gc_slot_fn visit);
void gc_visit_registered_roots(gc_slot_fn visit);
void gc_visit_shared_roots(gc_slot_fn visit);
void gc_visit_roots(gc_slot_fn visit);

/* Off-arena CL_Obj holders owned by other modules, walked by the above. */
void cl_compiler_gc_visit_thread(CL_Thread *t, gc_slot_fn visit);  /* compiler.c */
void cl_vm_gc_visit_extra_thread(CL_Thread *t, gc_slot_fn visit);  /* vm.c */
void cl_fasl_gc_visit_readers(gc_slot_fn visit);                   /* fasl.c */
void cl_fasl_gc_visit_writers(gc_slot_fn visit);
void cl_fasl_gc_visit_mlf(gc_slot_fn visit);
void cl_fasl_mlf_gc_relocated(void);
void gc_srcloc_invalidate_dead(void);
void gc_srcloc_forward(void);
uint32_t bytecode_release_offheap(CL_Bytecode *bc);
void gc_finalize_dead(uint8_t *ptr);

/* --- gc_classic.c (CL_GC_CLASSIC only) -------------------------------- */

#ifdef CL_GC_CLASSIC
/* Free-list rebuild from the mark bits (the classic non-moving cycle). */
void  gc_sweep(void);

/* Number of sweep-only collections since the last compaction — the
 * sweep-forever escape (GC_SWEEPS_BEFORE_COMPACT, mem.h); reset by the
 * compactor through gcb_compact_done / gcb_compact_fallback. */
extern uint32_t gc_sweeps_since_compact;
#endif

#ifdef CL_GENGC
#define GEN_CROSS_NONE 0xFFFFFFFFu

extern int      gen_enabled;
extern uint32_t gen_old_top;
extern volatile uint8_t *gen_dirty;
extern uint32_t *gen_cross;
extern uint32_t gen_page;
extern uint32_t gen_npages;
extern int      gen_protected;
extern int      gen_arena_pages;
extern uint32_t gen_minor_count;
extern uint64_t gc_time_minor_us;
extern uint64_t gen_promoted_bytes;
extern uint32_t gen_dirty_last;
extern uint32_t *gen_ht_list;
extern uint32_t gen_ht_count, gen_ht_cap;
extern uint32_t *gen_live;
extern uint32_t *gen_live_new;
extern uint32_t gen_live_count, gen_live_cap;
extern int gen_minor_active;
extern int gen_minor_fwd;
extern int gen_live_oom;
extern uint32_t *gen_fin;
extern uint32_t gen_fin_count, gen_fin_cap;

int  gen_type_finalizable(uint8_t type);
void gen_live_note(uint32_t off);
void gen_fin_note(uint32_t off, int need_lock);
void gen_protect_old(void);
void gen_unprotect_all(void);
void gen_cross_note(uint32_t obj_offset);
void gen_reset(void);
#endif /* CL_GENGC */

/* --- gc_compact.c ----------------------------------------------------- */

/* The compactor's once-per-cycle gate: the GC epoch (cl_heap.gc_count) at
 * which a compaction was last scheduled, and the "a safe point should
 * compact" flag the VM dispatch loop polls (cl_gc_compact_if_pending).
 * The classic allocation policy reads the gate to compact at most once per
 * collection; a backend with no such policy leaves it alone. */
extern uint32_t gc_last_compact_cycle;
extern int gc_compact_pending;

extern uint32_t gc_fwd_fail_bump;
extern CL_Obj **gc_root_slot_buf;
extern uint32_t gc_root_slot_cap;

CL_Obj gc_forward(CL_Obj obj);
void   gc_update_children(void *ptr, uint8_t type);
int    root_slot_independently_forwarded(CL_Obj *slot);
void   gc_update_all_roots(void);
void   gc_rehash_table(CL_Hashtable *ht);
void   gc_rehash_tlv_tables(void);

/* --- gc_jitscan.c ----------------------------------------------------- */

/* Block-start index (classic collector; see gc_jitscan.c for the design). */
#define GC_HDR_PAGE_SHIFT 12
#define GC_HDR_PAGE       (1u << GC_HDR_PAGE_SHIFT)
#define GC_HDR_NONE       0xFFFFFFFFu

/* Does a block [off, off+size) cover the first byte of some page? */
#define GC_HDR_CROSSES(off, size) \
    ((((off) + GC_HDR_PAGE - 1) & ~(uint32_t)(GC_HDR_PAGE - 1)) < (off) + (size))

extern uint32_t *gc_hdr_page;
extern uint32_t  gc_hdr_bump_watch;

void gc_hdr_note(uint32_t off, uint32_t size);
void gc_hdr_bump_reset(void);
void gc_hdr_note_bump(uint32_t old, uint32_t size);
void gc_hdr_index_clear(void);
void gc_hdr_index_release(void);
void gc_hdr_index_init(void);
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
void gc_hdr_index_selfcheck(void);
#endif

extern uint32_t *jit_scan_cand_buf;
extern int       jit_scan_cand_cap;
extern uint32_t *jit_pinned;
extern int       jit_pinned_count;
extern int       jit_pinned_cap;
extern int       gc_jit_pin_oom;
extern uint32_t *jit_scan_free_snap;
extern int       jit_scan_free_count;
extern int       jit_scan_free_cap;
extern int       jit_scan_free_valid;

void gc_scan_jit_native_stack(CL_Thread *t);
int  cand_cmp(const void *a, const void *b);   /* ascending uint32 offsets */

/* --- gc_verify.c ------------------------------------------------------ */

#ifdef DEBUG_GC
extern int gc_verify_errors;
void gc_verify_marked(void);
void gc_dump_roots_dbg(void);
void gc_verify_after_sweep(void);
#endif

/* --- Shared object-slot walker ---------------------------------------- */

/* --- Shared object-slot walker (used by mark and compaction-update) ---------
 * gc_mark_children and gc_update_children walk the identical per-type set of
 * CL_Obj slots; they differ only in the operation applied to each slot
 * (mark-push a value vs forward a slot in place) and in two type-specific
 * tails.  The layout is therefore written ONCE here as an X-macro and each
 * function instantiates it by defining GC_VISIT plus the two _TAIL hooks.
 * Because it is macro expansion the generated code is identical to two
 * hand-written switches — zero per-object overhead on this hot GC path.
 *
 * Callers must define, before invoking:
 *   GC_VISIT(slot)          operate on one CL_Obj lvalue slot
 *   GC_BYTECODE_TAIL(bc)    extra work for TYPE_BYTECODE (native reloc forward)
 *   GC_STREAM_TAIL(st)      extra work for TYPE_STREAM   (outbuf pin)
 * and #undef them afterward. */
#define GC_WALK_OBJ_CHILDREN(ptr, type)                                       \
    switch (type) {                                                           \
    case TYPE_CONS: {                                                         \
        CL_Cons *c = (CL_Cons *)(ptr);                                        \
        GC_VISIT(c->car);                                                     \
        GC_VISIT(c->cdr);                                                     \
        break;                                                               \
    }                                                                        \
    case TYPE_SYMBOL: {                                                       \
        CL_Symbol *s = (CL_Symbol *)(ptr);                                    \
        GC_VISIT(s->name);                                                    \
        if (s->value != CL_UNBOUND) GC_VISIT(s->value);                       \
        if (s->function != CL_UNBOUND) GC_VISIT(s->function);                 \
        GC_VISIT(s->plist);                                                   \
        GC_VISIT(s->package);                                                 \
        break;                                                               \
    }                                                                        \
    case TYPE_FUNCTION: {                                                     \
        CL_Function *f = (CL_Function *)(ptr);                                \
        GC_VISIT(f->name);                                                    \
        break;                                                               \
    }                                                                        \
    case TYPE_CLOSURE: {                                                      \
        CL_Closure *cl = (CL_Closure *)(ptr);                                 \
        uint32_t size = CL_HDR_SIZE(ptr);                                     \
        uint32_t n_upvals = (size - sizeof(CL_Closure)) / sizeof(CL_Obj);     \
        uint32_t i;                                                           \
        GC_VISIT(cl->bytecode);                                               \
        for (i = 0; i < n_upvals; i++)                                        \
            GC_VISIT(cl->upvalues[i]);                                        \
        break;                                                               \
    }                                                                        \
    case TYPE_BYTECODE: {                                                     \
        CL_Bytecode *bc = (CL_Bytecode *)(ptr);                               \
        uint16_t i;                                                           \
        GC_VISIT(bc->name);                                                   \
        GC_VISIT(bc->source_lambda_list);                                     \
        for (i = 0; i < bc->n_constants; i++)                                 \
            GC_VISIT(bc->constants[i]);                                       \
        if (bc->key_syms) {                                                   \
            for (i = 0; i < bc->n_keys; i++)                                  \
                GC_VISIT(bc->key_syms[i]);                                    \
        }                                                                     \
        GC_BYTECODE_TAIL(bc);                                                 \
        break;                                                               \
    }                                                                        \
    case TYPE_VECTOR: {                                                       \
        CL_Vector *v = (CL_Vector *)(ptr);                                    \
        uint32_t i;                                                           \
        if (v->flags & CL_VEC_FLAG_DISPLACED) {                              \
            GC_VISIT(v->data[CL_DISP_BASE_IDX(v)]);                           \
        } else {                                                             \
            uint32_t n_entries = (v->rank > 1) ? (uint32_t)v->rank + v->length \
                                               : v->length;                   \
            for (i = 0; i < n_entries; i++)                                   \
                GC_VISIT(v->data[i]);                                         \
        }                                                                    \
        break;                                                               \
    }                                                                        \
    case TYPE_PACKAGE: {                                                      \
        CL_Package *p = (CL_Package *)(ptr);                                  \
        GC_VISIT(p->name);                                                    \
        GC_VISIT(p->symbols);                                                 \
        GC_VISIT(p->use_list);                                                \
        GC_VISIT(p->nicknames);                                               \
        GC_VISIT(p->local_nicknames);                                         \
        GC_VISIT(p->shadowing_symbols);                                       \
        GC_VISIT(p->exported_symbols);                                        \
        GC_VISIT(p->bindings);                                                \
        break;                                                               \
    }                                                                        \
    case TYPE_HASHTABLE: {                                                    \
        CL_Hashtable *ht = (CL_Hashtable *)(ptr);                             \
        uint32_t i;                                                           \
        GC_VISIT(ht->bucket_vec);                                             \
        if (!CL_NULL_P(ht->bucket_vec)) {                                     \
            /* Buckets live in the external vector; walking it covers them */ \
        } else {                                                             \
            for (i = 0; i < ht->bucket_count; i++)                            \
                GC_VISIT(ht->buckets[i]);                                     \
        }                                                                    \
        break;                                                               \
    }                                                                        \
    case TYPE_CONDITION: {                                                    \
        CL_Condition *cond = (CL_Condition *)(ptr);                           \
        GC_VISIT(cond->type_name);                                            \
        GC_VISIT(cond->slots);                                                \
        GC_VISIT(cond->report_string);                                       \
        break;                                                               \
    }                                                                        \
    case TYPE_RESTART: {                                                      \
        CL_Restart *r = (CL_Restart *)(ptr);                                  \
        GC_VISIT(r->name);                                                    \
        GC_VISIT(r->function);                                                \
        GC_VISIT(r->report);                                                  \
        GC_VISIT(r->interactive);                                             \
        GC_VISIT(r->test);                                                    \
        GC_VISIT(r->tag);                                                     \
        break;                                                               \
    }                                                                        \
    case TYPE_FFI_STUB: {                                                     \
        CL_FfiStub *fs = (CL_FfiStub *)(ptr);                                 \
        GC_VISIT(fs->name);                                                   \
        GC_VISIT(fs->aux);                                                    \
        break;                                                               \
    }                                                                        \
    case TYPE_STRUCT: {                                                       \
        CL_Struct *st = (CL_Struct *)(ptr);                                   \
        uint32_t i;                                                           \
        GC_VISIT(st->type_desc);                                              \
        for (i = 0; i < st->n_slots; i++)                                     \
            GC_VISIT(st->slots[i]);                                           \
        break;                                                               \
    }                                                                        \
    case TYPE_STREAM: {                                                       \
        CL_Stream *st = (CL_Stream *)(ptr);                                   \
        GC_VISIT(st->string_buf);                                             \
        GC_VISIT(st->element_type);                                           \
        GC_STREAM_TAIL(st);                                                   \
        break;                                                               \
    }                                                                        \
    case TYPE_RATIO: {                                                        \
        CL_Ratio *r = (CL_Ratio *)(ptr);                                      \
        GC_VISIT(r->numerator);                                               \
        GC_VISIT(r->denominator);                                             \
        break;                                                               \
    }                                                                        \
    case TYPE_COMPLEX: {                                                      \
        CL_Complex *cx = (CL_Complex *)(ptr);                                 \
        GC_VISIT(cx->realpart);                                               \
        GC_VISIT(cx->imagpart);                                              \
        break;                                                               \
    }                                                                        \
    case TYPE_PATHNAME: {                                                     \
        CL_Pathname *pn = (CL_Pathname *)(ptr);                               \
        GC_VISIT(pn->host);                                                   \
        GC_VISIT(pn->device);                                                 \
        GC_VISIT(pn->directory);                                             \
        GC_VISIT(pn->name);                                                   \
        GC_VISIT(pn->type);                                                   \
        GC_VISIT(pn->version);                                                \
        break;                                                               \
    }                                                                        \
    case TYPE_CELL: {                                                         \
        CL_Cell *cell = (CL_Cell *)(ptr);                                     \
        GC_VISIT(cell->value);                                                \
        break;                                                               \
    }                                                                        \
    case TYPE_THREAD: {                                                       \
        CL_ThreadObj *to = (CL_ThreadObj *)(ptr);                            \
        GC_VISIT(to->name);                                                   \
        GC_VISIT(to->result);                                                 \
        break;                                                               \
    }                                                                        \
    case TYPE_LOCK: {                                                         \
        CL_Lock *lk = (CL_Lock *)(ptr);                                       \
        GC_VISIT(lk->name);                                                   \
        break;                                                               \
    }                                                                        \
    case TYPE_CONDVAR: {                                                      \
        CL_CondVar *cv = (CL_CondVar *)(ptr);                                 \
        GC_VISIT(cv->name);                                                   \
        break;                                                               \
    }                                                                        \
    case TYPE_STRING:                                                         \
    case TYPE_BIGNUM:                                                         \
    case TYPE_SINGLE_FLOAT:                                                    \
    case TYPE_DOUBLE_FLOAT:                                                    \
    case TYPE_RANDOM_STATE:                                                    \
    case TYPE_BIT_VECTOR:                                                      \
    case TYPE_BYTE_VECTOR:                                                     \
    case TYPE_FOREIGN_POINTER:                                                 \
    CL_GC_WIDE_STRING_CASE                                                     \
        /* No CL_Obj children — raw numeric/state/byte data */                \
        break;                                                               \
    default:                                                                  \
        break;                                                               \
    }

#ifdef CL_WIDE_STRINGS
#define CL_GC_WIDE_STRING_CASE case TYPE_WIDE_STRING:
#else
#define CL_GC_WIDE_STRING_CASE
#endif

#endif /* CL_GC_INTERNAL_H */
