#ifndef CL_MEM_H
#define CL_MEM_H

#include "types.h"

/*
 * Arena allocator with mark-and-sweep GC.
 *
 * Single contiguous arena, bump allocation with free-list fallback.
 * Mark phase uses explicit stack (no C recursion).
 * Sweep phase coalesces adjacent free blocks.
 */

#define CL_DEFAULT_HEAP_SIZE  (4 * 1024 * 1024)  /* 4MB */

/* Per-thread allocation buffers (TLABs) — host-only by default.  On the
 * 8MB/68020 Amiga target, multi-threaded allocation rates never amortize
 * the extra code and per-thread chunk footprint, and binary size matters
 * there; the feature compiles out entirely.  Define CL_FORCE_TLAB to
 * enable it on Amiga anyway (experiments), CL_NO_TLAB to disable on host. */
#if (!defined(PLATFORM_AMIGA) || defined(CL_FORCE_TLAB)) && !defined(CL_NO_TLAB)
#define CL_TLAB 1
#endif
/* Generational GC (sliding nursery + hardware dirty-page tracking) — see
 * specs/generational-gc.md.  Requires page protection (mprotect), so it is
 * POSIX-only; the AmigaOS target (no MMU assumed) compiles it out entirely
 * and keeps the classic mark-sweep-compact collector.  CL_NO_GENGC
 * disables it at build time on hosts; CLAMIGA_GENGC=0 disables at runtime
 * (classic mode, for A/B benchmarking and as a field escape hatch). */
#if (defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)) && !defined(CL_NO_GENGC)
#define CL_GENGC 1
#endif

/* INITIAL GC mark-stack capacity (entries).  The stack grows geometrically
 * on demand up to a heap-proportional cap (see gc_mark_stack_grow in mem.c);
 * this constant only sizes the always-available static baseline buffer.
 * Overridable for experiments/regression tests. */
#ifndef CL_GC_MARK_STACK_SIZE
#define CL_GC_MARK_STACK_SIZE 4096
#endif
#define CL_GC_ROOT_STACK_SIZE 1024
#define CL_MIN_ALLOC_SIZE     16  /* Minimum allocation (aligned) */

/* Tripwire: force compaction after this many bump-exhausted sweeps without
 * an intervening compaction.  A sweep-only GC never resets the bump pointer,
 * so once bump is exhausted the allocator can "succeed" via a recycled free
 * block on every iteration while running a full mark-sweep — the
 * "sweep-forever" pathology.  Overridable at build time so the regression
 * test (test_alloc_sweep_escape.c) can tighten the bound independently of
 * the default value. */
#ifndef GC_SWEEPS_BEFORE_COMPACT
#define GC_SWEEPS_BEFORE_COMPACT 8
#endif

/* Arena alignment: 8-byte on 64-bit hosts (CL_Bytecode et al. have pointer
 * fields that require natural alignment), 4-byte on 32-bit (Amiga). */
#if UINTPTR_MAX > 0xFFFFFFFFu
#define CL_ALIGN              8
#else
#define CL_ALIGN              4
#endif

/* Free block header (overlays CL_Header in freed objects).
 * Uses arena-relative offsets (not native pointers) for the free-list
 * link so that sizeof(CL_FreeBlock) == 8 on every platform.  This is
 * critical on 64-bit hosts: a native pointer would be 8 bytes and
 * expand the struct to 16 bytes via alignment padding, consuming the
 * entire CL_MIN_ALLOC_SIZE block and making poison-fill ineffective
 * for detecting use-after-free on small objects (e.g. cons cells). */
typedef struct CL_FreeBlock {
    uint32_t size;              /* Total size including this header */
    uint32_t next_offset;       /* Arena offset of next free block (0 = end) */
} CL_FreeBlock;

/* Heap state */
typedef struct {
    uint8_t *arena;             /* Base of arena */
    uint32_t arena_size;        /* Total arena size */
    uint32_t bump;              /* Bump pointer offset from arena */
    uint32_t free_list;         /* Arena offset of first free block (0 = empty) */
    uint32_t total_allocated;   /* Bytes currently allocated */
    uint32_t total_consed;      /* Bytes ever allocated (monotonic, never reset) */
    uint32_t gc_count;          /* Number of GC cycles */
    uint32_t compact_count;     /* Number of compaction cycles */
    uint32_t freelist_steps;    /* Total free-list blocks walked (monotonic
                                 * diagnostic; spots O(n) walk pathologies) */
    uint32_t tlab_refills;      /* TLAB chunks carved (monotonic diagnostic;
                                 * high rate = chunk size too small) */
} CL_Heap;

extern CL_Heap cl_heap;

/* Initialize/shutdown heap.  cl_mem_try_init returns -1 (message printed)
 * when the arena cannot be allocated, leaving the caller to hand back what
 * it holds before exiting -- main() does, since on AmigaOS off-heap memory
 * not freed is gone until reboot.  cl_mem_init exits on that failure. */
int  cl_mem_try_init(uint32_t heap_size);
void cl_mem_init(uint32_t heap_size);
void cl_mem_shutdown(void);

/* CLAMIGA_MEM_DIAG=1: report memory handed back at shutdown (set by
 * cl_mem_init).  Matters most on AmigaOS, where anything not returned before
 * the process exits is lost to the system until reboot. */
extern int cl_mem_diag;
/* Read it from the environment: what cl_mem_init does, for an exit that
 * happens before there is a heap (main.c's early_exit). */
void cl_mem_diag_from_env(void);

/* Off-heap bytecode payload returned to the allocator since heap init: total
 * bytes and number of bytecode objects finalized.  A CL_Bytecode's body,
 * constants pool, &key arrays, line map and JIT artifacts all live OUTSIDE
 * the arena, so no arena statistic shows whether they are being reclaimed.
 * Either out-param may be NULL.  Exposed to Lisp as
 * (ext:%bytecode-offheap-stats). */
void cl_gc_offheap_stats(uint32_t *bytes, uint32_t *count);

/* Signal a storage error without allocating (safe when heap is exhausted) */
void cl_storage_error(const char *fmt, ...);

/* Allocate a heap object (triggers GC if needed) */
void *cl_alloc(uint8_t type, uint32_t size);

/* --- Per-type element-count caps ---
 * The 23-bit header size field caps any single allocation at
 * CL_HDR_SIZE_MASK bytes; cl_alloc guards the BYTE size at entry.  But
 * the convenience allocators compute alloc_size = header + count*elt in
 * uint32 arithmetic, which WRAPS for counts >= 2^30 (CL_Obj elements)
 * and sails past the byte guard with a tiny wrapped size — the element
 * loops then scribble far beyond the block.  Normal Lisp entry points
 * are bounded by ARRAY-DIMENSION-LIMIT checks, but the FASL reader
 * passes raw u32 lengths from disk, so a corrupted/truncated .fasl
 * reaches these allocators with arbitrary counts.  Each allocator
 * rejects counts above these limits BEFORE computing alloc_size
 * (cl_storage_error, same as cl_alloc's byte guard); the FASL reader
 * checks them too for a cleaner "deserialize failed" diagnostic. */
#define CL_MAX_STRING_CHARS \
    ((uint32_t)(CL_HDR_SIZE_MASK - sizeof(CL_String) - 1))
#ifdef CL_WIDE_STRINGS
#define CL_MAX_WIDE_STRING_CHARS \
    ((uint32_t)((CL_HDR_SIZE_MASK - sizeof(CL_WideString)) / sizeof(uint32_t)))
#endif
#define CL_MAX_VECTOR_ELTS \
    ((uint32_t)((CL_HDR_SIZE_MASK - sizeof(CL_Vector)) / sizeof(CL_Obj)))
#define CL_MAX_BYTEVEC_BYTES \
    ((uint32_t)(CL_HDR_SIZE_MASK - sizeof(CL_ByteVector)))
#define CL_MAX_STRUCT_SLOTS \
    ((uint32_t)((CL_HDR_SIZE_MASK - sizeof(CL_Struct)) / sizeof(CL_Obj)))
#define CL_MAX_BIGNUM_LIMBS \
    ((uint32_t)((CL_HDR_SIZE_MASK - sizeof(CL_Bignum)) / sizeof(uint16_t)))
#define CL_MAX_BV_BITS \
    ((uint32_t)((CL_HDR_SIZE_MASK - sizeof(CL_BitVector)) / sizeof(uint32_t)) * 32u)
/* Largest power-of-two bucket count whose inline bucket array fits the
 * header size field (cl_make_hashtable rounds up to a power of two, and
 * the round-up loop itself would spin forever on counts > 2^31). */
#define CL_MAX_HT_BUCKETS  (1u << 20)

/* Convenience allocators */
CL_Obj cl_cons(CL_Obj car, CL_Obj cdr);
/* Cons from ALREADY-ROOTED storage (VM stack slots, or a CL_GC_PROTECTed
 * local): the operands are passed by address and read after the
 * allocation, so no root-stack push is needed for them.  See mem.c. */
CL_Obj cl_cons_rooted(const CL_Obj *car_ref, const CL_Obj *cdr_ref);
/* Short lists, (a b), (a b c), (a b c d), and (a b . tail).  Like cl_cons
 * they root their arguments themselves, so a caller passes plain values --
 * but only values: never nest an allocating call inside the argument list
 * of a call that takes a CL_Obj.  C leaves the order in which arguments are
 * evaluated unspecified, so the other arguments may already have been read
 * when the nested call collects and moves them, and the outer call then
 * stores stale offsets.  tests/test_gc_arg_order.sh checks src/ for it. */
CL_Obj cl_list2(CL_Obj a, CL_Obj b);
CL_Obj cl_list3(CL_Obj a, CL_Obj b, CL_Obj c);
CL_Obj cl_list4(CL_Obj a, CL_Obj b, CL_Obj c, CL_Obj d);
CL_Obj cl_list_star3(CL_Obj a, CL_Obj b, CL_Obj tail);
CL_Obj cl_make_string(const char *str, uint32_t len);
#ifdef CL_WIDE_STRINGS
CL_Obj cl_make_wide_string(const uint32_t *chars, uint32_t len);
#endif
CL_Obj cl_make_symbol(CL_Obj name);
CL_Obj cl_make_function(CL_CFunc func, CL_Obj name, int min_args, int max_args);
CL_Obj cl_make_vector(uint32_t length);
CL_Obj cl_make_array(uint32_t total, uint8_t rank, uint32_t *dims,
                     uint8_t flags, uint32_t fill_ptr);
CL_Obj cl_make_hashtable(uint32_t bucket_count, uint32_t test);
/* Compute a key's hash under a given test (CL_HT_TEST_*). Used by the
 * post-compaction rehash in mem.c; must match the hashing used by gethash. */
uint32_t cl_hashtable_hash_key(CL_Obj key, uint32_t test);
CL_Obj cl_make_condition(CL_Obj type_name, CL_Obj slots, CL_Obj report_string);
CL_Obj cl_make_restart(CL_Obj name, CL_Obj function, CL_Obj report,
                       CL_Obj interactive, CL_Obj test, CL_Obj tag);
CL_Obj cl_make_struct(CL_Obj type_name, uint32_t n_slots);
CL_Obj cl_make_bignum(uint32_t n_limbs, uint32_t sign);
CL_Obj cl_make_ratio(CL_Obj numerator, CL_Obj denominator);
CL_Obj cl_make_complex(CL_Obj realpart, CL_Obj imagpart);
CL_Obj cl_make_single_float(float value);
CL_Obj cl_make_double_float(double value);
CL_Obj cl_make_random_state(uint32_t seed);
CL_Obj cl_make_bit_vector(uint32_t nbits);
/* elt_shift: log2 element bytes — 0 = (unsigned/signed-byte 8), 1 = 16. */
CL_Obj cl_make_byte_vector(uint32_t length, int is_signed, int elt_shift);
CL_Obj cl_make_pathname(CL_Obj host, CL_Obj device, CL_Obj directory,
                        CL_Obj name, CL_Obj type, CL_Obj version);
CL_Obj cl_make_cell(CL_Obj value);
CL_Obj cl_make_foreign_pointer(uint32_t address, uint32_t size, uint8_t flags);
/* FFI binding descriptor (types.h CL_FfiStub).  NAME / AUX are GC-protected
 * across the allocation. */
CL_Obj cl_make_ffi_stub(uint8_t kind, CL_Obj name, CL_Obj aux,
                        uint32_t a, int16_t b, uint8_t ctype);

/* GC root protection (temporary, C-stack variables) */
#ifdef DEBUG_GC
#define CL_GC_PROTECT(var) cl_gc_push_root_dbg(&(var), __FILE__, __LINE__)
#else
#define CL_GC_PROTECT(var) cl_gc_push_root(&(var))
#endif
#define CL_GC_UNPROTECT(n) cl_gc_pop_roots(n)

void cl_gc_push_root(CL_Obj *root);
#ifdef DEBUG_GC
void cl_gc_push_root_dbg(CL_Obj *root, const char *file, int line);
#endif
void cl_gc_pop_roots(int n);
void cl_gc_reset_roots(void);

/* Global root registration (static/global CL_Obj variables).
 * Registered roots are marked during GC and updated during compaction.
 * Use for cached interned symbols (keywords, type symbols, etc.).
 * Registration is idempotent per address; overflowing the table is a
 * loud boot-time fatal (a dropped root corrupts on the next compaction).
 * 1024 covers the ~220 symbol.c handles + ~230 registrations across the
 * builtins with headroom. */
#define CL_MAX_GLOBAL_ROOTS 1024
void cl_gc_register_root(CL_Obj *root_ptr);
#ifdef DEBUG_GC
/* Debug builds record the registration site of every global root so the
 * image writer can NAME late-registered (post-boot) roots — the spec-level
 * warning that catches a stateful root registered too late to be part of
 * the image restore set (see specs/image-save-load.md "Late-registered
 * roots").  The macro upgrades every existing call site untouched. */
void cl_gc_register_root_at(CL_Obj *root_ptr, const char *file, int line);
#define cl_gc_register_root(p) cl_gc_register_root_at((p), __FILE__, __LINE__)
#endif

/* --- Heap-image support (image.c; see specs/image-save-load.md) ---
 * The image writer/loader enumerate and overwrite the registered global
 * roots by index: boot is single-threaded and deterministic, so index i
 * names the same C variable in the saving and the restoring process of
 * the same build. */
int     cl_gc_global_root_count(void);
CL_Obj  cl_gc_global_root_get(int i);
void    cl_gc_global_root_set(int i, CL_Obj value);
#ifdef DEBUG_GC
const char *cl_gc_global_root_file(int i);
int         cl_gc_global_root_line(int i);
#endif

/* Adopt a restored image's arena payload.  begin() installs the payload's
 * bump/free-list/accounting immediately after the raw bytes were copied
 * over the arena; finish() runs after the relink walk and normalizes GC
 * state for the active collector in one linear pass (classic: clear all
 * mark bits; generational: set sticky marks, rebuild the crossing map,
 * set the old-space watermark to bump, clear dirty bits and re-arm
 * old-space protection). */
void cl_mem_adopt_image_begin(uint32_t bump);
void cl_mem_adopt_image_finish(void);

/* Audit for double-registered roots (global vs. global, global vs. any
 * thread root stack).  gc_forward is not idempotent, so a slot reachable
 * from two root entries is silently corrupted on compaction.  Returns
 * the number of violations (0 = clean). */
int cl_gc_audit_roots(void);

/* Audit the block-start index behind the conservative JIT native-stack
 * scan: every arena page below the bump front must map to a real header
 * at or below its first byte.  Returns the number of violating pages
 * (0 = clean), -1 when the index is absent (generational mode, or
 * CLAMIGA_HDR_INDEX=0).  See tests/test_gc_hdr_index.c. */
int cl_gc_audit_hdr_index(void);

/* Collect, then check every object header, every child reference of
 * every live object, every registered static root and every thread's
 * protect stack / VM stack against the real object starts.  Returns the
 * fault count (0 = clean, -1 = could not run); the bounded report is
 * read with cl_gc_verify_heap_report.  (ext:%heap-verify) in Lisp. */
int cl_gc_verify_heap(void);
const char *cl_gc_verify_heap_report(void);


/* Retire a thread's TLAB (flush cons accounting, drop the chunk).  The
 * uncut remainder is already formatted as a walkable hole; the next sweep
 * reclaims it.  Called by cl_thread_unregister; safe for a finished thread. */
#ifdef CL_TLAB
struct CL_Thread_s;
void cl_tlab_retire(struct CL_Thread_s *t);
#endif

/* Manually trigger GC */
void cl_gc(void);

/* --- Call generation (specs/jit-direct-calls.md §1) ---
 * One global word guarding every cached (callee, entry) pair a JIT call
 * site holds: a site is valid only while its recorded generation equals
 * cl_call_gen.  Everything that could make such a pair wrong bumps it —
 * every function-cell write (through cl_symbol_set_function, never a raw
 * function-slot store; tests/test_symfn_funnel.sh enforces it), every garbage
 * collection, native code replaced, a stop-the-world or interrupt request,
 * TRACE/UNTRACE and the JIT mode toggles.  Never 0: 0 marks an empty site.
 * Bumps are rare; nothing on a per-call path writes the word. */
extern volatile uint32_t cl_call_gen;

/* Advance cl_call_gen (atomically, skipping 0 on wrap).  WHY names the
 * event; DEBUG_JIT builds log it, so a bump storm is visible. */
void cl_call_gen_bump(const char *why);

/* Set SYM's function cell to FN (CL_UNBOUND = fmakunbound) and bump the
 * call generation.  The one place a live symbol's function cell is
 * written.  Non-allocating. */
void cl_symbol_set_function(CL_Obj sym, CL_Obj fn);

/* Cheap reclamation pass for bounded external-handle tables (locks,
 * condvars, threads): when a table fills, most dead handle-owners are
 * RECENT objects, so under the generational collector a minor cycle
 * usually frees the slots at a fraction of a full collection's cost.
 * Falls back to cl_gc() in classic mode.  Callers retry their table
 * allocation afterwards and escalate to cl_gc() (a full collection) on a
 * second miss. */
void cl_gc_reclaim_young(void);

/* Allocation-triggered GC with redundant-cycle dedup: collects only if the
 * GC epoch (cl_heap.gc_count) still equals seen_gc_count once this thread
 * has stopped the world — i.e. no peer collected in the meantime.  Returns
 * 1 if a collection ran, 0 if it was skipped as redundant (the caller
 * should retry its allocation and fall back to cl_gc() on a miss).
 * Single-threaded it always collects. */
int cl_gc_if_stale(uint32_t seen_gc_count);

/* Compacting GC — slides live objects to eliminate fragmentation.
 * MUST be called at a safe point where no C locals hold CL_Obj values.
 * Safe points: REPL top-level, explicit (ext:gc), after top-level eval. */
void cl_gc_compact(void);
/* Off-heap bytes a compaction allocates (transiently) for its forwarding
 * structures at the current bump front: about 1/32 + 1/64 of the used
 * span on 32-bit.  See tests/test_gc_fwd_bitmap.c. */
uint32_t cl_gc_fwd_bytes(void);
/* Fault injection for that allocation: while non-zero, a block of at least
 * this many bytes is refused as if the platform allocator had failed, so a
 * test can drive the "GC cannot compact" fallback.  0 = off (the default). */
extern uint32_t cl_gc_fwd_fail_over;

/* Run pending compaction if needed.  Call at safe points (REPL, top-level). */
void cl_gc_compact_if_pending(void);

#ifdef CL_GENGC
/* Minor (nursery) collection: trace live young objects from roots + dirty
 * old pages, slide survivors down onto the old-space watermark, reset the
 * nursery bump.  seen_gc_count enables the same redundant-cycle dedup as
 * cl_gc_if_stale.  Returns 1 if a minor cycle ran (or was deduped — either
 * way the caller should retry its allocation), 0 if minor collection is
 * unavailable/refused (caller escalates to cl_gc_compact). */
int cl_gc_minor(uint32_t seen_gc_count);

/* Is the generational collector active this session? */
int cl_gengc_enabled(void);

/* Pre-touch a heap range a SYSCALL is about to write into (file read,
 * socket recv, ...).  A userspace store to a protected old-space page
 * faults into the write-watch handler and retries transparently, but a
 * kernel write returns EFAULT instead — so such sites must dirty+unlock
 * the target range explicitly before the call.  No-op for nursery/foreign
 * pointers and when protection is not armed. */
void cl_gc_pretouch(const void *ptr, uint32_t len);

/* Diagnostics for ext:%gengc-stats: minor cycle count, cumulative minor
 * time (us), bytes promoted, old-space watermark, dirty pages scanned in
 * the last minor.  Any out-param may be NULL. */
void cl_gengc_stats(uint32_t *minors, uint64_t *minor_us,
                    uint64_t *promoted_bytes, uint32_t *old_top,
                    uint32_t *dirty_last);
#else
/* Classic-only builds: keep call sites unconditional. */
#define cl_gc_pretouch(ptr, len) ((void)0)
#endif

/* Debug/stats */
void cl_mem_stats(void);

/* GC mark-stack diagnostics: current capacity (entries), successful grow
 * operations, and full-arena overflow re-scan passes (monotonic; nonzero
 * means the quadratic overflow fallback ran — growth failed on cap/OOM).
 * Any out-param may be NULL.  Exposed to Lisp as (ext:%gc-mark-stats). */
void cl_gc_mark_stack_stats(uint32_t *cap_entries, uint32_t *grows,
                            uint32_t *rescan_passes);

/* Test hook: cap mark-stack growth at max_entries so the overflow re-scan
 * fallback can be exercised deterministically (0 = normal heap-proportional
 * cap).  Reset by cl_mem_init. */
void cl_gc_mark_stack_set_test_limit(uint32_t max_entries);

/* Cumulative GC phase timers (microseconds since cl_mem_init): time spent
 * waiting for stop-the-world, in the mark phase, in the sweep phase, and in
 * whole compaction cycles (compaction's internal mark/sweep counts toward
 * compact only).  Any out-param may be NULL.  Exposed to Lisp as
 * (ext:%gc-time-stats). */
void cl_gc_time_stats(uint64_t *stw_us, uint64_t *mark_us,
                      uint64_t *sweep_us, uint64_t *compact_us);

/* Stop-the-world diagnostics: number of multi-threaded stop events, the
 * worst single stop wait (microseconds), and how many allocation-triggered
 * collections were deduped as redundant (cl_gc_if_stale skips).  Any
 * out-param may be NULL. */
void cl_gc_stw_stats(uint32_t *stops, uint64_t *max_us, uint32_t *epoch_skips);

/* Free-list shape (classic collector): block count, total free bytes on the
 * list, largest block.  Walks the list under alloc_mutex when threads are
 * live — a diagnostic (ext:%heap-stats), not a hot path.  All zero in gen
 * mode, where the free list is unused.  Any out-param may be NULL. */
void cl_heap_free_list_stats(uint32_t *blocks, uint32_t *bytes,
                             uint32_t *largest);

#include "thread.h"

#endif /* CL_MEM_H */
