/* gc_compact.c — the sliding compactor: forwarding bitmap + page
 * table + JIT pin gaps, slot/reference update (slot-form root walk),
 * the slide, post-move rehash, and cl_gc_compact.  A facility every
 * collector may call (classic: fragmentation / sweep-forever escape;
 * the image writer).  State: gc_internal.h.
 */
#include "gc_internal.h"

/* Compaction forwarding structures — one transient platform_alloc block,
 * taken after the mark phase and handed back before cl_gc_compact returns:
 *
 *   gc_fwd_bits  one bit per CL_ALIGN granule of [0, bump), set when the
 *                granule lies inside a marked (live) object;
 *   gc_fwd_page  per GC_FWD_PAGE bytes of arena, the slide cursor at the
 *                page's first byte: CL_ALIGN + the live bytes below it +
 *                the holes pinned objects below it leave behind;
 *   gc_fwd_gap   per entry of the sorted jit_pinned[], the hole the slide
 *                leaves below that pin (pin offset - cursor on arrival).
 *
 *   new(obj) = gc_fwd_page[page(obj)]
 *            + CL_ALIGN * popcount(bits from the page start up to obj)
 *            + sum of gc_fwd_gap over pins in [page start, obj]
 *
 * The bitmap is 1/(8*CL_ALIGN) and the page table 4/GC_FWD_PAGE of the
 * used span — 1/32 + 1/64 on 32-bit, about 390 KB for an 8 MB heap.  The
 * previous design kept one uint32 forwarding address per granule, a second
 * block as large as the heap's used span.  An A1200 with the 8 MB heap in
 * its Zorro block cannot AllocVec another 7-8 MB once the trapdoor Fast
 * RAM holds the compiler pool and the stacks; every bump-exhausted
 * allocation then ran a full mark, failed the table, swept, and repeated
 * on the next allocation — test-amiga-lowend spent ~25 s per allocation in
 * the 'alloc sweep-forever escape' check (2026-09-24).  The size bound is
 * pinned by tests/test_gc_fwd_bitmap.c.
 *
 * A page is two bitmap words (64 granules: 256 bytes on 32-bit, 512 on a
 * 64-bit host), so a lookup pops at most one full word and one partial —
 * the page table costs half the bitmap for that.  The page start falls on
 * a word boundary by construction. */
#if CL_ALIGN == 8
#define GC_FWD_ALIGN_SHIFT 3
#else
#define GC_FWD_ALIGN_SHIFT 2
#endif
#define GC_FWD_WORDS_PER_PAGE 2u
#define GC_FWD_PAGE_SHIFT  (GC_FWD_ALIGN_SHIFT + 5 + 1)
#define GC_FWD_PAGE        (1u << GC_FWD_PAGE_SHIFT)
static uint32_t *gc_fwd_bits  = NULL;
static uint32_t *gc_fwd_page  = NULL;
static uint32_t *gc_fwd_gap   = NULL;
static void     *gc_fwd_block = NULL;   /* the allocation behind the three */

/* Bump level at which the last forwarding-table platform_alloc failed
 * (0 = none).  While the bump front is at or above this level a
 * sweep-triggered compaction attempt (cl_alloc trigger 2) would fail the
 * same way — full mark work for nothing — so trigger 2 is gated on it.
 * Cleared by a successful compaction (which also resets the bump). */
uint32_t gc_fwd_fail_bump = 0;

/* Fault injection (tests/test_gc_fwd_bitmap.c): while non-zero, a forwarding
 * block of at least this many bytes is refused as if platform_alloc had said
 * no — the only way a host test can drive the OOM fallback and its latch. */
uint32_t cl_gc_fwd_fail_over = 0;

/* Persistent buffer for the deduplicated registered-root forwarding pass
 * (see gc_update_registered_roots below) — defined here so
 * cl_mem_shutdown can free it. */
CL_Obj         **gc_root_slot_buf = NULL;
uint32_t         gc_root_slot_cap = 0;

/* Set during a compaction's reference-update pass when at least one
 * CL_Obj immediate baked into JIT native code was rewritten in place.
 * cl_gc_compact flushes the CPU caches once at the end so 68040/060
 * don't execute stale instruction-cache copies of the patched code. */
static int gc_native_code_patched = 0;

/* ================================================================
 * Compacting GC (Lisp-2 style sliding compaction)
 *
 * 4-pass algorithm:
 *   Pass 1: Mark (reuse existing gc_mark)
 *   Pass 2: Compute forwarding addresses
 *   Pass 3: Update all references (roots + heap objects)
 *   Pass 4: Slide live objects to their new positions
 *
 * Triggered when normal GC + free-list can't satisfy an allocation
 * (fragmentation is the bottleneck), or explicitly via cl_gc_compact().
 * ================================================================ */

/* Bytes the forwarding structures need at the current bump front (the
 * bitmap, the page table, one gap per pin).  Exported so a test can pin
 * the bound; a compaction allocates exactly this. */
uint32_t cl_gc_fwd_bytes(void)
{
    uint32_t words = (cl_heap.bump / CL_ALIGN + 31) / 32;
    uint32_t pages = (cl_heap.bump + GC_FWD_PAGE - 1) >> GC_FWD_PAGE_SHIFT;
    return (words + pages + (uint32_t)jit_pinned_count) * (uint32_t)sizeof(uint32_t);
}

/* Allocate / free the forwarding structures.  The whole block is zeroed:
 * the bitmap needs it, and so does the gap array — a pin that pass 2 does
 * not place (a duplicate entry, or a candidate that was never a marked
 * object start) must contribute a zero gap to the lookups, exactly as it
 * contributes nothing to the slide.  Pass 2 writes every page entry. */
static int gc_fwd_alloc(void)
{
    uint32_t words = (cl_heap.bump / CL_ALIGN + 31) / 32;
    uint32_t pages = (cl_heap.bump + GC_FWD_PAGE - 1) >> GC_FWD_PAGE_SHIFT;
    uint32_t bytes = cl_gc_fwd_bytes();
    uint32_t *blk;
    if (cl_gc_fwd_fail_over && bytes >= cl_gc_fwd_fail_over)
        return 0;
    blk = (uint32_t *)platform_alloc(bytes);
    if (!blk) return 0;
    gc_fwd_block = blk;
    gc_fwd_bits  = blk;
    gc_fwd_page  = blk + words;
    gc_fwd_gap   = gc_fwd_page + pages;
    memset(blk, 0, bytes);
    return 1;
}

static void gc_fwd_free(void)
{
    if (gc_fwd_block) {
        platform_free(gc_fwd_block);
        gc_fwd_block = NULL;
        gc_fwd_bits = gc_fwd_page = gc_fwd_gap = NULL;
    }
}

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__aarch64__))
/* One instruction on these hosts.  Not on m68k or PPC, where the builtin
 * becomes a libgcc call: the shift-and-mask form below is inlined there. */
#define gc_popcount32(x) ((uint32_t)__builtin_popcount((unsigned)(x)))
#else
static uint32_t gc_popcount32(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555u);
    x = (x & 0x33333333u) + ((x >> 2) & 0x33333333u);
    x = (x + (x >> 4)) & 0x0F0F0F0Fu;
    x += x >> 8;
    x += x >> 16;
    return x & 0x3Fu;
}
#endif

/* Set the bitmap for granules [g0, g1) — one live object. */
static void gc_fwd_set_range(uint32_t g0, uint32_t g1)
{
    uint32_t w0 = g0 >> 5, w1 = (g1 - 1) >> 5;
    uint32_t m0 = 0xFFFFFFFFu << (g0 & 31);
    uint32_t m1 = 0xFFFFFFFFu >> (31 - ((g1 - 1) & 31));
    if (w0 == w1) {
        gc_fwd_bits[w0] |= m0 & m1;
        return;
    }
    gc_fwd_bits[w0] |= m0;
    while (++w0 < w1)
        gc_fwd_bits[w0] = 0xFFFFFFFFu;
    gc_fwd_bits[w1] |= m1;
}

/* Sum of the holes left below pins in [lo, hi] (sorted jit_pinned[]). */
static uint32_t gc_fwd_pin_gaps(uint32_t lo, uint32_t hi)
{
    int a = 0, b = jit_pinned_count;
    uint32_t sum = 0;
    while (a < b) {                     /* first pin >= lo */
        int mid = (a + b) / 2;
        if (jit_pinned[mid] < lo) a = mid + 1; else b = mid;
    }
    while (a < jit_pinned_count && jit_pinned[a] <= hi)
        sum += gc_fwd_gap[a++];
    return sum;
}

/* The slide cursor: where the next unpinned live object goes.  Shared by
 * pass 2 (which also records each pin's gap) and pass 4, so the addresses
 * the lookup computes and the addresses the slide uses come from one
 * definition. */
typedef struct {
    uint32_t cur;       /* next free position in the new layout */
    int      pin_i;     /* index into the sorted jit_pinned[] */
} GcPlace;

static uint32_t gc_place(GcPlace *pl, uint32_t off, uint32_t size, int record_gap)
{
    uint32_t at;
    /* Advance past pins below this object (jit_pinned[] is sorted
     * ascending, and objects are visited in ascending offset order). */
    while (pl->pin_i < jit_pinned_count && jit_pinned[pl->pin_i] < off)
        pl->pin_i++;
    if (pl->pin_i < jit_pinned_count && jit_pinned[pl->pin_i] == off) {
        /* Pinned: keep in place.  The cursor is <= off here (all live data
         * below this object compacts into [CL_ALIGN, off)), so leaving
         * [cur, off) as a hole and resuming at the pin's end preserves the
         * monotonic, no-overlap invariant. */
        if (record_gap)
            gc_fwd_gap[pl->pin_i] = off - pl->cur;
        pl->pin_i++;
        /* The same object pinned from two scan chunks or two threads:
         * the duplicates keep their zero gap (gc_fwd_alloc). */
        while (pl->pin_i < jit_pinned_count && jit_pinned[pl->pin_i] == off)
            pl->pin_i++;
        pl->cur = off + size;
        return off;
    }
    at = pl->cur;
    pl->cur += size;
    return at;
}

/* Pass 2: Walk arena linearly, record live granules and the per-page slide
 * cursor from which gc_forward derives every forwarding address. */
static void gc_compute_forwarding(void)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;
    uint32_t npages = (cl_heap.bump + GC_FWD_PAGE - 1) >> GC_FWD_PAGE_SHIFT;
    uint32_t next_page = 0;
    GcPlace pl;
    pl.cur = CL_ALIGN;               /* skip offset 0 (NIL) */
    pl.pin_i = 0;

    /* jit_pinned[] must be ascending for the single-pass merge in gc_place.
     * A single thread's scan already appends in ascending order (monotonic
     * arena walk), but with multi-thread STW each stopped thread's JIT
     * stack is scanned in turn, so pins from different threads concatenate
     * out of order — sort. */
    if (jit_pinned_count > 1)
        qsort(jit_pinned, (size_t)jit_pinned_count, sizeof(jit_pinned[0]),
              cand_cmp);

    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        if (CL_HDR_MARKED(cl_heap.arena + off)) {
            uint32_t new_off;

            /* Pages whose first byte lies in the dead run before this
             * object, or exactly at it, see the cursor as it stands. */
            while (next_page < npages && (next_page << GC_FWD_PAGE_SHIFT) <= off)
                gc_fwd_page[next_page++] = pl.cur;

            new_off = gc_place(&pl, off, size, 1);

            /* Pages starting inside this object: it slides as a unit, so a
             * granule at page start lands at new_off + its distance. */
            while (next_page < npages && (next_page << GC_FWD_PAGE_SHIFT) < off + size) {
                gc_fwd_page[next_page] = new_off + ((next_page << GC_FWD_PAGE_SHIFT) - off);
                next_page++;
            }
            gc_fwd_set_range(off >> GC_FWD_ALIGN_SHIFT, (off + size) >> GC_FWD_ALIGN_SHIFT);
        }
    }
    while (next_page < npages)
        gc_fwd_page[next_page++] = pl.cur;
}

/* (The former gc_forward_jit_native_stack writeback pass was removed when
 * the compactor switched to pinning conservatively-referenced objects —
 * option B in specs/native-backend.md §"GC interaction".  Pinned objects
 * never move, so the JIT's spilled operand offsets on the C stack stay
 * valid with no writeback, and a coincidental-integer C-stack word that
 * merely equals a live object's offset is no longer mis-rewritten.) */

/* Translate a CL_Obj through the forwarding table.
 * Returns obj unchanged if it's not a movable heap pointer. */
CL_Obj gc_forward(CL_Obj obj)
{
    uint32_t g, w, live, fwd;
    if (CL_NULL_P(obj) || CL_FIXNUM_P(obj) || CL_CHAR_P(obj))
        return obj;
    if (obj == CL_UNBOUND)
        return obj;
    if (obj >= cl_heap.bump)
        return obj;
#ifdef CL_GENGC
    if (gen_minor_fwd) {
        /* Minor cycle: forward via binary search over the sorted survivor
         * list instead of a nursery-sized table (whose allocate+zero would
         * cost O(nursery) per cycle).  Old-space offsets never move in a
         * minor; a nursery offset absent from the list references a DEAD
         * object (possible in weak-ish side tables) and is left alone. */
        uint32_t lo, hi;
        if (obj < gen_old_top)
            return obj;
        lo = 0;
        hi = gen_live_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            if (gen_live[mid] < obj)      lo = mid + 1;
            else if (gen_live[mid] > obj) hi = mid;
            else                          return gen_live_new[mid];
        }
        return obj;
    }
#endif
    g = obj >> GC_FWD_ALIGN_SHIFT;
    if (!((gc_fwd_bits[g >> 5] >> (g & 31)) & 1u))
        return obj;      /* dead object or free space: left as it is */
    /* Live granules from the page start up to (not including) obj. */
    w = (obj >> GC_FWD_PAGE_SHIFT) * GC_FWD_WORDS_PER_PAGE;
    live = 0;
    while (w < (g >> 5))
        live += gc_popcount32(gc_fwd_bits[w++]);
    live += gc_popcount32(gc_fwd_bits[w] & ((1u << (g & 31)) - 1u));
    fwd = gc_fwd_page[obj >> GC_FWD_PAGE_SHIFT] + (live << GC_FWD_ALIGN_SHIFT);
    if (jit_pinned_count > 0)
        fwd += gc_fwd_pin_gaps(obj & ~(GC_FWD_PAGE - 1), obj);
    /* An offset inside a live object (never a valid CL_Obj, but a
     * conservative or corrupt slot can hold one) moves with its object:
     * the formula above is exact for interior granules too. */
    return fwd;
}

/* Update a CL_Obj slot in place via forwarding table */
static void gc_update_slot(CL_Obj *slot)
{
    *slot = gc_forward(*slot);
}

/* Pass 3a: Update children of a single heap object.  Shares the per-type slot
 * layout with gc_mark_children via the GC_WALK_OBJ_CHILDREN X-macro above;
 * here each slot is forwarded in place and TYPE_BYTECODE additionally patches
 * CL_Obj immediates baked into JIT'd native code. */
void gc_update_children(void *ptr, uint8_t type)
{
#define GC_VISIT(slot) gc_update_slot(&(slot))
    /* Forward CL_Obj heap references baked as 32-bit immediates into the JIT'd
     * native code.  native_relocs lists the byte offset of each such 4-byte
     * big-endian field; without this a moving compaction would leave stale
     * arena offsets in executable code (m68k JIT only — native_relocs is NULL
     * on host / non-JIT).  The code buffer is platform_alloc'd (does not move),
     * so the raw pointer stays valid whether we visit the object's old or new
     * copy; only the referenced objects shift.  Stop-the-world GC means no
     * thread is mid-fetch of these bytes. */
#define GC_BYTECODE_TAIL(bc)                                                  \
    do {                                                                      \
        if ((bc)->native_code && (bc)->native_relocs) {                       \
            uint32_t r_;                                                       \
            for (r_ = 0; r_ < (bc)->native_reloc_count; r_++) {               \
                uint8_t *p_ = (bc)->native_code + (bc)->native_relocs[r_];     \
                CL_Obj old = ((CL_Obj)p_[0] << 24) | ((CL_Obj)p_[1] << 16) |   \
                             ((CL_Obj)p_[2] << 8)  |  (CL_Obj)p_[3];           \
                CL_Obj nw = gc_forward(old);                                   \
                if (nw != old) {                                              \
                    p_[0] = (uint8_t)(nw >> 24); p_[1] = (uint8_t)(nw >> 16);  \
                    p_[2] = (uint8_t)(nw >> 8);  p_[3] = (uint8_t)(nw);        \
                    gc_native_code_patched = 1;                               \
                }                                                            \
            }                                                                \
        }                                                                    \
    } while (0)
#define GC_STREAM_TAIL(st) ((void)(st))
    GC_WALK_OBJ_CHILDREN(ptr, type);
#undef GC_VISIT
#undef GC_BYTECODE_TAIL
#undef GC_STREAM_TAIL
}

/* --- Registered-root forwarding with dedup (double-forward hardening) ---
 *
 * gc_forward is NOT idempotent: the forwarding table is keyed by
 * pre-compaction offsets, so forwarding the same slot twice maps the
 * already-forwarded offset through whatever object's OLD offset it now
 * coincides with — silently rewriting the slot to an unrelated object.
 * Historically each root registry was forwarded by its own loop, so any
 * slot reachable from two entries (an accidental double CL_GC_PROTECT,
 * a global both cl_gc_register_root'ed and protected, or a protect of an
 * already-rooted VM-stack slot like CL_GC_PROTECT(args[i])) corrupted
 * with layout-roulette timing.  This pass forwards all dynamically-
 * registered slots (global_roots[] + every thread's gc_roots[]) from one
 * sorted, deduplicated address list, and skips the addresses the thread
 * and shared root walks forward on their own (VM stack, mv_values, the
 * pending-throw values, dyn/handler/restart stacks, vm_extra_args, the
 * compiler chain, a table global, ...).  That alias set is not a list of
 * ranges that could drift from the walks: it is found by running the SAME
 * walks once more with a probe visitor that looks each visited address up
 * in the sorted list — a slot is "independently forwarded" exactly when
 * gc_visit_thread_roots / gc_visit_shared_roots visit it.  Duplicate
 * registration is therefore harmless by construction; cl_gc_audit_roots
 * remains the tool for locating the offending registration site.
 * (gc_root_slot_buf itself is declared with the other persistent GC
 * buffers near the top of the file, so cl_mem_shutdown can free it; its
 * per-entry alias flags live in the same block, right after the
 * gc_root_slot_cap pointers.) */
static int root_slot_cmp(const void *a, const void *b)
{
    CL_Obj * const *pa = (CL_Obj * const *)a;
    CL_Obj * const *pb = (CL_Obj * const *)b;
    if (*pa < *pb) return -1;
    if (*pa > *pb) return 1;
    return 0;
}

/* Single-address probe: is `gc_probe_slot` visited by the walks? */
static CL_Obj *gc_probe_slot;
static int     gc_probe_hit;
static void gc_probe_check(CL_Obj *slot)
{
    if (slot == gc_probe_slot) gc_probe_hit = 1;
}

/* Does slot alias a root the thread / shared walks forward themselves?
 * Such a slot must be SKIPPED by the registered-root pass or it is
 * forwarded twice.  One walk per call — used by the diagnostics
 * (cl_gc_audit_roots) and by the no-memory fallback below; the main pass
 * answers the question for the whole sorted list in one walk instead. */
int root_slot_independently_forwarded(CL_Obj *slot)
{
    CL_Thread *t;
    gc_probe_slot = slot;
    gc_probe_hit = 0;
    for (t = cl_thread_list; t && !gc_probe_hit; t = t->next)
        gc_visit_thread_roots(t, gc_probe_check);
    if (!gc_probe_hit)
        gc_visit_shared_roots(gc_probe_check);
    return gc_probe_hit;
}

/* Sorted-list probe: flag the (first copy of the) registered entry at
 * this address as aliased. */
static uint32_t gc_alias_n;
static uint8_t *gc_alias_flag;
static void gc_alias_probe(CL_Obj *slot)
{
    uint32_t lo = 0, hi = gc_alias_n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (gc_root_slot_buf[mid] < slot)      lo = mid + 1;
        else if (gc_root_slot_buf[mid] > slot) hi = mid;
        else {
            while (mid > 0 && gc_root_slot_buf[mid - 1] == slot)
                mid--;           /* duplicates: flag the copy the pass keeps */
            gc_alias_flag[mid] = 1;
            return;
        }
    }
}

#undef gc_root_count
static void gc_update_registered_roots(void)
{
    uint32_t total = (uint32_t)n_global_roots;
    uint32_t n = 0, i;
    CL_Thread *t;
    int j;
#ifdef DEBUG_GC
    uint32_t dups = 0, aliased = 0;
#endif

    for (t = cl_thread_list; t; t = t->next)
        total += (uint32_t)t->gc_root_count;
    if (total == 0)
        return;

    if (total > gc_root_slot_cap) {
        CL_Obj **buf = (CL_Obj **)platform_alloc(total * (sizeof(CL_Obj *) + 1));
        if (buf) {
            if (gc_root_slot_buf) platform_free(gc_root_slot_buf);
            gc_root_slot_buf = buf;
            gc_root_slot_cap = total;
        }
    }
    if (total > gc_root_slot_cap) {
        /* Buffer allocation failed — fall back to per-registry loops.
         * Exact-duplicate addresses would still double-forward here, but
         * skipping the update entirely would corrupt EVERY slot; the
         * alias probe needs no memory, so it still applies. */
        for (j = 0; j < n_global_roots; j++)
            if (!root_slot_independently_forwarded(global_roots[j]))
                gc_update_slot(global_roots[j]);
        for (t = cl_thread_list; t; t = t->next)
            for (j = 0; j < t->gc_root_count; j++)
                if (!root_slot_independently_forwarded(t->gc_roots[j]))
                    gc_update_slot(t->gc_roots[j]);
        return;
    }

    for (j = 0; j < n_global_roots; j++)
        gc_root_slot_buf[n++] = global_roots[j];
    for (t = cl_thread_list; t; t = t->next)
        for (j = 0; j < t->gc_root_count; j++)
            gc_root_slot_buf[n++] = t->gc_roots[j];

    qsort(gc_root_slot_buf, (size_t)n, sizeof(CL_Obj *), root_slot_cmp);

    /* One more pass over the thread + shared roots, flagging every
     * registered address they visit. */
    gc_alias_n = n;
    gc_alias_flag = (uint8_t *)(gc_root_slot_buf + gc_root_slot_cap);
    memset(gc_alias_flag, 0, n);
    for (t = cl_thread_list; t; t = t->next)
        gc_visit_thread_roots(t, gc_alias_probe);
    gc_visit_shared_roots(gc_alias_probe);

    for (i = 0; i < n; i++) {
        if (i > 0 && gc_root_slot_buf[i] == gc_root_slot_buf[i - 1]) {
#ifdef DEBUG_GC
            dups++;
#endif
            continue;               /* duplicate registration — forward once */
        }
        if (gc_alias_flag[i]) {
#ifdef DEBUG_GC
            aliased++;
#endif
            continue;               /* owned by the thread / shared walk */
        }
        gc_update_slot(gc_root_slot_buf[i]);
    }

#ifdef DEBUG_GC
    if (dups || aliased)
        fprintf(stderr, "GC: root dedup skipped %u duplicate and %u "
                "thread-region-aliased registration(s) this compaction "
                "(harmless, but run ext:%%gc-audit-roots to find the "
                "redundant CL_GC_PROTECT / cl_gc_register_root)\n",
                (unsigned)dups, (unsigned)aliased);
#endif
}
#define gc_root_count (CT->gc_root_count)

/* Pass 3 (roots half): update every non-heap root slot through the
 * forwarding table — the root walk of gc_trace.c instantiated with
 * gc_update_slot, plus the deduplicated registered-root pass.  Shared
 * verbatim between a full compaction and a minor (nursery) collection —
 * gc_forward leaves non-forwarded (old) offsets unchanged, so this is
 * safe and cheap in both.
 *
 * The JIT native stack needs no forward-update pass: the mark-phase scan
 * (gc_scan_jit_native_stack) pins every conservatively-referenced object,
 * so they stay at their current offset and the JIT's spilled C-stack
 * offsets remain valid without rewriting the stack. */
void gc_update_all_roots(void)
{
    CL_Thread *t;

    for (t = cl_thread_list; t; t = t->next)
        gc_visit_thread_roots(t, gc_update_slot);

    /* Registered root slots (global_roots[] + all thread gc_roots[]),
     * deduplicated so a doubly-registered slot is forwarded exactly once
     * and one aliasing a slot of the walks above/below is left to them. */
    gc_update_registered_roots();

    gc_visit_shared_roots(gc_update_slot);

    /* Side indexes keyed by the offsets that just moved: the struct
     * registry hash index (builtins_struct.c), the condition hierarchy
     * index, the compiler's deftype / compiler-macro / defsetf table
     * indexes (CL_AlistIndex, compiler.h) and the MAKE-LOAD-FORM
     * pre-pass's dedup indexes (fasl.c).  Mark them stale so they rebuild
     * lazily before the next lookup. */
    {
        extern void cl_struct_index_gc_invalidate(void);
        extern void cl_condition_index_gc_invalidate(void);
        extern void cl_compiler_indexes_gc_invalidate(void);
        cl_struct_index_gc_invalidate();
        cl_condition_index_gc_invalidate();
        cl_compiler_indexes_gc_invalidate();
        cl_fasl_mlf_gc_relocated();
    }
}

/* Pass 3: Walk all live heap objects + all roots and update references */
static void gc_update_all_references(void)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;

    gc_update_all_roots();

    /* Walk all live heap objects and update their children */
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        uint8_t *ptr = cl_heap.arena + off;
        if (CL_HDR_MARKED(ptr))
            gc_update_children(ptr, type);
    }
}

/* Chunk size for splitting a free gap of `total` bytes under alignment
 * `align`: at most CL_HDR_SIZE_MASK, align-multiple, and never leaving a
 * remainder too small to hold a CL_FreeBlock header.  With align=4
 * (Amiga) the naive cap CL_HDR_SIZE_MASK & ~3 can leave a 4-byte
 * remainder; the next iteration's CL_FreeBlock write (8 bytes) would
 * then overrun the gap and smash the following — pinned — object's
 * header.  Non-static so the unit test can exercise the align=4
 * arithmetic on the host (whose CL_ALIGN is 8). */
uint32_t gc_free_gap_chunk(uint32_t total, uint32_t align)
{
    uint32_t chunk = total;
    if (chunk > CL_HDR_SIZE_MASK) {
        chunk = CL_HDR_SIZE_MASK & ~(align - 1u);
        if (total - chunk < (uint32_t)sizeof(CL_FreeBlock))
            chunk -= CL_MIN_ALLOC_SIZE;
    }
    return chunk;
}

/* Write valid free-block header(s) covering [offset, offset+total) and
 * thread them onto the free list, so the arena's linear "walk by header
 * size" stays consistent across the gap.  Mirrors gc_sweep's free-block
 * format (size in the header low bits, type/mark = 0 → an unmarked block
 * the walk skips).  Splits gaps larger than the 23-bit size field into
 * multiple blocks (see gc_free_gap_chunk for the remainder invariant).
 * `total` is always a CL_ALIGN multiple and, when non-zero,
 * >= CL_MIN_ALLOC_SIZE (>= sizeof(CL_FreeBlock)), so every piece can
 * hold the 8-byte header. */
static void gc_make_free_gap(uint32_t offset, uint32_t total)
{
    while (total > 0) {
        uint32_t chunk = gc_free_gap_chunk(total, CL_ALIGN);
        if (chunk < (uint32_t)sizeof(CL_FreeBlock)) {
            /* Degenerate sub-header gap (caller invariant broken — total
             * itself smaller than a CL_FreeBlock).  Write a bare size-only
             * header so the linear arena walk stays in sync, but leave the
             * block off the free list: there is no room for the
             * next_offset link, and writing one would smash the following
             * object.  The space is reclaimed by the next compaction. */
            ((CL_Header *)(cl_heap.arena + offset))->header = chunk;
            if (GC_HDR_CROSSES(offset, chunk))
                gc_hdr_note(offset, chunk);
            return;
        }
        {
            CL_FreeBlock *fb = (CL_FreeBlock *)(cl_heap.arena + offset);
            fb->size = chunk;                       /* type=0, mark=0, size=chunk */
            fb->next_offset = cl_heap.free_list;
            cl_heap.free_list = offset;
#ifdef DEBUG_GC
            if (chunk > sizeof(CL_FreeBlock))
                memset((uint8_t *)fb + sizeof(CL_FreeBlock), 0xDE,
                       chunk - sizeof(CL_FreeBlock));
#endif
        }
        if (GC_HDR_CROSSES(offset, chunk))
            gc_hdr_note(offset, chunk);
        offset += chunk;
        total  -= chunk;
    }
}

/* Pass 4: Slide live objects to their forwarding addresses.
 * Objects only move downward (or stay), so forward copy is safe.
 *
 * Pinned objects (option B) keep their offset, which can leave a gap
 * between the compacted run below them and the pin.  Those gaps are
 * filled with free-block headers (gc_make_free_gap) and threaded onto
 * the free list — both so the linear arena walk stays valid and so the
 * allocator can reclaim the space.  With no pins this degenerates to a
 * gap-free compaction (free_list ends empty). */
static void gc_slide(void)
{
    GcWalk w;
    uint32_t old_offset, size;
    uint8_t type;
    uint32_t fill = CL_ALIGN;          /* next contiguous position in new layout */
    uint32_t live_total = 0;
    GcPlace pl;
    pl.cur = CL_ALIGN;
    pl.pin_i = 0;

    cl_heap.free_list = 0;

#ifdef CL_GENGC
    /* Full compaction rebuilds old space from scratch — restart the
     * crossing map; entries are re-noted as objects are placed below. */
    if (gen_enabled) {
        uint32_t i;
        for (i = 0; i < gen_npages; i++)
            gen_cross[i] = GEN_CROSS_NONE;
    }
#endif

    gc_walk_begin(&w);
    while (gc_walk_next(&w, &old_offset, &size, &type)) {
        uint8_t *ptr = cl_heap.arena + old_offset;

        if (CL_HDR_MARKED(ptr)) {
            uint32_t new_offset = gc_place(&pl, old_offset, size, 0);
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
            /* The lookup pass 3 used must agree with the placement the
             * slide performs — for the object start and for a granule
             * inside it. */
            if (gc_forward(old_offset) != new_offset ||
                (size > CL_ALIGN &&
                 gc_forward(old_offset + CL_ALIGN) != new_offset + CL_ALIGN)) {
                platform_write_string("GC: forwarding lookup disagrees with slide\n");
                abort();
            }
#endif

#ifdef CL_GENGC
            /* Sticky-mark invariant: in gen mode every live (old) object
             * keeps its mark bit set between collections, so the copied
             * survivor must stay marked; a major collection clears all
             * marks up front instead (see cl_gc_compact). */
            if (gen_enabled)
                gen_cross_note(new_offset);
            else
#endif
                CL_HDR_CLR_MARK(ptr);

            /* Gap before this object's new position (only at pins) → free. */
            if (new_offset > fill)
                gc_make_free_gap(fill, new_offset - fill);

            if (new_offset != old_offset)
                memmove(cl_heap.arena + new_offset, ptr, size);
            /* Block-start index: placed in ascending order, so the last
             * assignment per page is the covering block. */
            if (GC_HDR_CROSSES(new_offset, size))
                gc_hdr_note(new_offset, size);
            fill = new_offset + size;
            live_total += size;
        } else {
            /* Dead object — release external resources before overwriting */
            gc_finalize_dead(ptr);
        }
    }

    cl_heap.bump = fill;
    gc_hdr_bump_reset();
    /* total_allocated counts live data; gap free-blocks below bump are on
     * the free list and excluded (matches gc_sweep's accounting). */
    cl_heap.total_allocated = live_total;
}

/* Is a key's hash address-sensitive — i.e. does it change when the compacting
 * GC relocates objects? Mirrors hash_obj() in builtins_hashtable.c. If a table
 * has any such key it MUST be rehashed after a compaction, or stale buckets
 * make previously-inserted keys un-findable (gethash returns NIL). */
static int gc_key_addr_sensitive(CL_Obj key, uint32_t test)
{
    uint8_t type;
    if (!CL_HEAP_P(key))
        return 0;                 /* fixnum / char / immediate: stable */
    if (test == CL_HT_TEST_EQ)
        return 1;                 /* every heap object hashed by identity */
    type = CL_HDR_TYPE(CL_OBJ_TO_PTR(key));
    switch (type) {
    case TYPE_BIGNUM:
    case TYPE_RATIO:
    case TYPE_SINGLE_FLOAT:
    case TYPE_DOUBLE_FLOAT:
    case TYPE_COMPLEX:
        return 0;                 /* value-based hash under eql/equal/equalp */
    case TYPE_STRING:
#ifdef CL_WIDE_STRINGS
    case TYPE_WIDE_STRING:
#endif
    case TYPE_BIT_VECTOR:
        /* eql hashes these by identity; equal/equalp by contents (wide
         * strings and bit-vectors gained content hashes in tier-4 AH5). */
        return (test == CL_HT_TEST_EQL) ? 1 : 0;
    case TYPE_CONS:
        if (test == CL_HT_TEST_EQL)
            return 1;             /* cons hashed by identity under eql */
        /* equal/equalp: hash_obj() folds in only the car */
        return gc_key_addr_sensitive(((CL_Cons *)CL_OBJ_TO_PTR(key))->car, test);
    default:
        return 1;                 /* symbol, struct, instance, array, … */
    }
}

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
/* Chain-integrity check: every bucket chain must terminate within ht->count
 * steps.  A longer walk means a chain CYCLE (double-forwarded bucket slot or
 * a put through a stale table pointer) — without this check the next
 * gc_rehash_table spins forever, which is undebuggable in the field. */
static void gc_verify_ht_chains(CL_Hashtable *ht, const char *when)
{
    CL_Obj *bkts;
    uint32_t i, walked = 0;
    if (!CL_NULL_P(ht->bucket_vec))
        bkts = ((CL_Vector *)CL_OBJ_TO_PTR(ht->bucket_vec))->data;
    else
        bkts = ht->buckets;
    for (i = 0; i < ht->bucket_count; i++) {
        CL_Obj chain = bkts[i];
        while (!CL_NULL_P(chain)) {
            if (++walked > ht->count + 8) {
                CL_Cons *e0 = (CL_Cons *)CL_OBJ_TO_PTR(bkts[i]);
                fprintf(stderr,
                        "[GC-HT-BUG] %s: hashtable @0x%08x (test=%u count=%u "
                        "buckets=%u flags=0x%x) chain walk exceeded count at "
                        "bucket %u - cycle/corruption\n"
                        "  chain=0x%08x entry->car(pair)=0x%08x "
                        "entry->cdr=0x%08x\n",
                        when,
                        (unsigned)((uint8_t *)ht - cl_heap.arena),
                        (unsigned)ht->test, (unsigned)ht->count,
                        (unsigned)ht->bucket_count, (unsigned)ht->flags,
                        (unsigned)i,
                        (unsigned)bkts[i], (unsigned)e0->car,
                        (unsigned)e0->cdr);
                if (CL_CONS_P(e0->car)) {
                    CL_Cons *p0 = (CL_Cons *)CL_OBJ_TO_PTR(e0->car);
                    fprintf(stderr, "  pair key=0x%08x val=0x%08x\n",
                            (unsigned)p0->car, (unsigned)p0->cdr);
                }
                /* Re-walk the arena and print the last objects leading up to
                 * this table — a desynced walk (bad header size upstream)
                 * shows up as implausible type/size runs here. */
                {
                    uint8_t *wp = cl_heap.arena + CL_ALIGN;
                    uint8_t *wend = cl_heap.arena + cl_heap.bump;
                    uint32_t woff[8]; uint32_t wtype[8]; uint32_t wsize[8];
                    int wn = 0;
                    while (wp < wend && wp <= (uint8_t *)ht) {
                        uint32_t wsz = CL_HDR_SIZE(wp);
                        if (wsz == 0) break;
                        woff[wn & 7] = (uint32_t)(wp - cl_heap.arena);
                        wtype[wn & 7] = CL_HDR_TYPE(wp);
                        wsize[wn & 7] = wsz;
                        wn++;
                        wp += wsz;
                    }
                    {
                        int k, first = wn > 8 ? wn - 8 : 0;
                        for (k = first; k < wn; k++)
                            fprintf(stderr,
                                    "  walk[%d] @0x%08x type=%u size=%u\n",
                                    k, (unsigned)woff[k & 7],
                                    (unsigned)wtype[k & 7],
                                    (unsigned)wsize[k & 7]);
                        fprintf(stderr, "  walk ended at 0x%08x (ht at 0x%08x, bump 0x%08x)\n",
                                (unsigned)(wp - cl_heap.arena),
                                (unsigned)((uint8_t *)ht - cl_heap.arena),
                                (unsigned)cl_heap.bump);
                    }
                }
                /* Who references this table?  A symbol-value referrer names
                 * the variable holding it. */
                {
                    CL_Obj ht_obj = CL_PTR_TO_OBJ((uint8_t *)ht);
                    uint8_t *wp = cl_heap.arena + CL_ALIGN;
                    uint8_t *wend = cl_heap.arena + cl_heap.bump;
                    int found = 0;
                    while (wp < wend && found < 8) {
                        uint32_t wsz = CL_HDR_SIZE(wp);
                        uint32_t nw, w;
                        if (wsz == 0) break;
                        nw = wsz / 4;
                        for (w = 1; w < nw; w++) {
                            if (((uint32_t *)wp)[w] == ht_obj) {
                                fprintf(stderr,
                                        "  referrer @0x%08x type=%u word=%u",
                                        (unsigned)(wp - cl_heap.arena),
                                        (unsigned)CL_HDR_TYPE(wp), (unsigned)w);
                                if (CL_HDR_TYPE(wp) == TYPE_SYMBOL) {
                                    CL_Symbol *rs = (CL_Symbol *)wp;
                                    if (CL_HEAP_P(rs->name)) {
                                        CL_String *rn = (CL_String *)CL_OBJ_TO_PTR(rs->name);
                                        fprintf(stderr, " symbol=%.*s",
                                                (int)rn->length, rn->data);
                                    }
                                }
                                fprintf(stderr, "\n");
                                found++;
                            }
                        }
                        wp += wsz;
                    }
                    if (!found)
                        fprintf(stderr, "  no arena referrer (C-global/root/TLV?)\n");
                }
                cl_capture_backtrace();
                fprintf(stderr, "%s", cl_backtrace_buf);
                fflush(stderr);
                abort();
            }
            if (!CL_CONS_P(chain)) {
                fprintf(stderr,
                        "[GC-HT-BUG] %s: hashtable %p bucket %u chain entry "
                        "is not a cons (0x%08x)\n",
                        when, (void *)ht, (unsigned)i, (unsigned)chain);
                fflush(stderr);
                abort();
            }
            chain = ((CL_Cons *)CL_OBJ_TO_PTR(chain))->cdr;
        }
    }
}
/* Sweep the arena and verify every populated table (see gc_verify_ht_chains). */
static void gc_verify_all_ht_chains(const char *when)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;
    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        if (type == TYPE_HASHTABLE) {
            CL_Hashtable *ht = (CL_Hashtable *)(cl_heap.arena + off);
            if (ht->count > 0)
                gc_verify_ht_chains(ht, when);
        }
    }
}
#endif

/* Rehash a single hash table after compaction (no allocation). Recomputes each
 * key's bucket with the table's own test, since relocated objects now hash to
 * different values. Skips tables whose every key is hash-stable. */
void gc_rehash_table(CL_Hashtable *ht)
{
    CL_Obj *bkts;
    uint32_t bucket_count = ht->bucket_count;
    uint32_t test = ht->test;
    uint32_t i;
    int needs = 0;
    CL_Obj all_entries = CL_NIL;

    /* Get bucket array */
    if (!CL_NULL_P(ht->bucket_vec)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(ht->bucket_vec);
        bkts = v->data;
    } else {
        bkts = ht->buckets;
    }

#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    gc_verify_ht_chains(ht, "pre-rehash");
#endif

    /* Do any keys have address-sensitive hashes? If not, the buckets are still
     * valid after the move and we can leave the table untouched. */
    for (i = 0; i < bucket_count && !needs; i++) {
        CL_Obj chain = bkts[i];
        while (!CL_NULL_P(chain)) {
            CL_Cons *entry = (CL_Cons *)CL_OBJ_TO_PTR(chain);
            CL_Obj pair = entry->car;
            CL_Obj key = ((CL_Cons *)CL_OBJ_TO_PTR(pair))->car;
            if (gc_key_addr_sensitive(key, test)) { needs = 1; break; }
            chain = entry->cdr;
        }
    }
    if (!needs)
        return;

    /* Collect all entries into a single linked list, clear buckets */
    for (i = 0; i < bucket_count; i++) {
        CL_Obj chain = bkts[i];
        while (!CL_NULL_P(chain)) {
            CL_Cons *entry = (CL_Cons *)CL_OBJ_TO_PTR(chain);
            CL_Obj next = entry->cdr;
            entry->cdr = all_entries;
            all_entries = chain;
            chain = next;
        }
        bkts[i] = CL_NIL;
    }

    /* Redistribute using freshly computed hashes (post-move offsets) */
    while (!CL_NULL_P(all_entries)) {
        CL_Cons *entry = (CL_Cons *)CL_OBJ_TO_PTR(all_entries);
        CL_Obj next = entry->cdr;
        CL_Obj pair = entry->car;
        CL_Obj key = ((CL_Cons *)CL_OBJ_TO_PTR(pair))->car;
        uint32_t h = cl_hashtable_hash_key(key, test);
        uint32_t idx = h & (bucket_count - 1);
        entry->cdr = bkts[idx];
        bkts[idx] = all_entries;
        all_entries = next;
    }
}

/* Rehash ALL hash tables in the arena after compaction. Covers eq/eql/equal/
 * equalp: any of them can hash a key by object identity (objects fall through
 * to an identity hash), and identity changes when the GC moves the object. */
static void gc_rehash_tables(void)
{
    GcWalk w;
    uint32_t off, size;
    uint8_t type;

    gc_walk_begin(&w);
    while (gc_walk_next(&w, &off, &size, &type)) {
        if (type == TYPE_HASHTABLE) {
            CL_Hashtable *ht = (CL_Hashtable *)(cl_heap.arena + off);
            if (ht->count > 0)
                gc_rehash_table(ht);
        }
    }
}

/* Rebuild every thread's TLV (dynamic-binding) table after a compaction.
 * Like the user hash tables, the TLV table is keyed by symbol arena-offset and
 * must be re-hashed once symbols have moved, or active dynamic bindings (e.g.
 * a `let` of a special var) silently revert to their global value. */
void gc_rehash_tlv_tables(void)
{
    CL_Thread *t;
    /* During STW GC all threads are stopped; cl_thread_list is stable. */
    for (t = cl_thread_list; t; t = t->next)
        cl_tlv_rehash(t);
}

/* The once-per-cycle gate and the safe-point flag (see gc_internal.h).
 * gc_compact_pending is set by the classic allocation policy when it
 * detects fragmentation and cleared when compaction runs at a safe point;
 * the VM dispatch loop polls it.  gc_last_compact_cycle prevents infinite
 * loops when the heap is genuinely full (no fragmentation to reclaim). */
int gc_compact_pending = 0;
uint32_t gc_last_compact_cycle = 0xFFFFFFFF;

/* Run compaction if pending (called from safe points). */
void cl_gc_compact_if_pending(void)
{
    if (gc_compact_pending) {
        gc_compact_pending = 0;
        gc_last_compact_cycle = cl_heap.gc_count;
        cl_gc_compact();
    }
}

/* Main compaction entry point */
void cl_gc_compact(void)
{
    int multithread = (cl_thread_count > 1);
    uint64_t t0 = gc_stop_world_timed(multithread);

    /* Same invariant as cl_gc: no thread may keep cutting from a region the
     * passes below reclaim or slide — a chunk, or the bump front the slide
     * resets.  Chunk remainders are formatted holes, so forwarding/slide
     * parse them as dead regions. */
    gc_cursor_drop_all();

    /* The backend normalises its view first (gen mode: drop old-space
     * protection, clear the sticky marks so the full trace sees the true
     * live set). */
    gcb_compact_begin();

#ifdef DEBUG_GC
    platform_write_string("GC: compaction starting...\n");
#endif

    gc_native_code_patched = 0;

    /* Pass 1: Mark (standard) */
    cl_stream_outbuf_gc_mark_begin();
    gc_mark();
    /* Reclaim outbuf slots of streams that didn't survive marking, BEFORE the
     * slide overwrites their (now-dead) stream objects. */
    cl_stream_outbuf_gc_reclaim();
    gc_srcloc_invalidate_dead();

#ifdef DEBUG_GC
    /* Verify parent→child mark invariant immediately after marking so that
     * 'marked @X.field -> unmarked @Y' diagnostics fire at the exact
     * compaction that would corrupt the heap (not on the next sweep-only GC). */
    gc_verify_marked();
    if (gc_verify_errors > 0)
        gc_dump_roots_dbg();
#endif

    /* JIT pin-table OOM during marking: the pinned set is incomplete, so
     * moving anything would strand a live JIT frame's spilled offsets.
     * Degrade to a non-moving mark+sweep for this cycle (the mark phase
     * above is complete — only relocation is unsafe). */
    if (gc_jit_pin_oom) {
#ifdef DEBUG_GC
        platform_write_string("GC: compact suppressed (JIT pin-table OOM), "
                              "falling back to sweep\n");
#endif
        /* The backend's non-moving cycle takes over from the completed
         * mark (classic: a sweep; gen mode stands down to classic). */
        gcb_compact_fallback(GCB_COMPACT_FALLBACK_PIN);
        cl_heap.gc_count++;
        gc_time_compact_us += platform_time_us() - t0;
        if (gc_diag_on())
            gc_diag_report("compact-fallback", platform_time_us() - t0);
        if (multithread) cl_gc_resume_the_world();
        return;
    }

    /* Allocate the forwarding structures */
    if (!gc_fwd_alloc()) {
        if (gc_fwd_fail_bump == 0) {
            /* Say so once per episode (the latch below silences the
             * repeats): a heap that can no longer compact is a degraded
             * mode the user should see in the session log, not something
             * to infer from a sudden GC storm. */
            char msg[128];
            sprintf(msg, "clamiga: GC cannot compact - no %lu bytes of memory "
                         "for the forwarding table, sweeping instead\n",
                    (unsigned long)cl_gc_fwd_bytes());
            platform_write_string(msg);
        }
        /* See the pin-OOM fallback above. */
        gcb_compact_fallback(GCB_COMPACT_FALLBACK_FWD);
        cl_heap.gc_count++;
        /* Remember the bump level that failed: without this latch, the
         * classic policy's sweep-forever escape would re-run a doomed full
         * mark (+ failed table alloc + sweep) on EVERY following bump-
         * exhausted allocation — a permanent 100%-CPU GC-thrash regime.
         * Its trigger 2 skips re-attempts until the bump front is below
         * the recorded failure level (the table size is proportional to
         * it); trigger 1 (compaction as last resort) still always tries. */
        gc_fwd_fail_bump = cl_heap.bump;
        gc_time_compact_us += platform_time_us() - t0;
        if (gc_diag_on())
            gc_diag_report("compact-fallback", platform_time_us() - t0);
        if (multithread) cl_gc_resume_the_world();
        return;
    }

    /* Pass 2: Compute forwarding addresses */
    gc_compute_forwarding();

    /* Pass 3: Update all references */
    gc_update_all_references();
    gc_srcloc_forward();

    /* Pass 4: Slide objects */
    gc_slide();


    /* Clean up forwarding table */
    gc_fwd_free();

    /* If we rewrote any CL_Obj immediate inside JIT'd native code, flush
     * the CPU caches so the patched instruction bytes are re-fetched.
     * REQUIRED on every 68020+ — the 020/030 also have an I-cache (256 B)
     * that can serve stale pre-patch lines; only the 68000/010 lack one.
     * Do NOT "optimize" this to 040/060-only.  No-op on host. */
    if (gc_native_code_patched)
        platform_cache_clear(NULL, 0);

    /* Rehash hash tables whose keys hash by object identity (offsets changed) */
    gc_rehash_tables();
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    /* Every object in the arena is a live survivor here — a dirty chain now
     * means THIS compact (or the mutator window before it) corrupted it. */
    gc_verify_all_ht_chains("post-rehash");
#endif

    /* Rehash per-thread TLV tables (keyed by symbol offset, which changed) */
    gc_rehash_tlv_tables();

    cl_heap.gc_count++;
    cl_heap.compact_count++;
#if defined(DEBUG_GC) || defined(DEBUG_GC_STRESS)
    gc_hdr_index_selfcheck();
#endif

    gc_fwd_fail_bump = 0;   /* table allocation succeeded at this size */

    /* The arena is dense below the bump front, every survivor still
     * mark-bitted: the backend re-derives its state (classic: the
     * sweep-forever counter; gen mode: a fresh old space and dirty era). */
    gcb_compact_done();

#ifdef DEBUG_GC
    /* Post-compaction verification: check all live objects have valid refs */
    {
        uint8_t *vptr = cl_heap.arena + CL_ALIGN;
        uint8_t *vend = cl_heap.arena + cl_heap.bump;
        int vc_errs = 0;
        char vbuf[256];
        while (vptr < vend && vc_errs < 5) {
            uint32_t vsize = CL_HDR_SIZE(vptr);
            uint8_t vtype = CL_HDR_TYPE(vptr);
            uint32_t voff = (uint32_t)(vptr - cl_heap.arena);
            if (vsize == 0) break;
            if (vtype == TYPE_CONS) {
                CL_Cons *c = (CL_Cons *)vptr;
                if (!CL_NULL_P(c->car) && !CL_FIXNUM_P(c->car) && !CL_CHAR_P(c->car)
                    && c->car != CL_UNBOUND && c->car >= cl_heap.bump) {
                    snprintf(vbuf, sizeof(vbuf),
                        "COMPACT-VERIFY: @0x%08x.car -> 0x%08x (OOB, bump=0x%08x)\n",
                        (unsigned)voff, (unsigned)c->car, (unsigned)cl_heap.bump);
                    platform_write_string(vbuf);
                    vc_errs++;
                }
                if (!CL_NULL_P(c->cdr) && !CL_FIXNUM_P(c->cdr) && !CL_CHAR_P(c->cdr)
                    && c->cdr != CL_UNBOUND && c->cdr >= cl_heap.bump) {
                    snprintf(vbuf, sizeof(vbuf),
                        "COMPACT-VERIFY: @0x%08x.cdr -> 0x%08x (OOB, bump=0x%08x)\n",
                        (unsigned)voff, (unsigned)c->cdr, (unsigned)cl_heap.bump);
                    platform_write_string(vbuf);
                    vc_errs++;
                }
            } else if (vtype == TYPE_VECTOR) {
                CL_Vector *v = (CL_Vector *)vptr;
                uint32_t vi;
                uint32_t nelt = (v->flags & CL_VEC_FLAG_DISPLACED) ? CL_DISP_BASE_IDX(v) + 2 :
                                ((v->rank > 1) ? (uint32_t)v->rank + v->length : v->length);
                for (vi = 0; vi < nelt && vc_errs < 5; vi++) {
                    CL_Obj elt = v->data[vi];
                    if (!CL_NULL_P(elt) && !CL_FIXNUM_P(elt) && !CL_CHAR_P(elt)
                        && elt != CL_UNBOUND && (elt & CL_TAG_MASK_LO2) == 0
                        && elt >= cl_heap.bump) {
                        snprintf(vbuf, sizeof(vbuf),
                            "COMPACT-VERIFY: @0x%08x vec[%u] -> 0x%08x (OOB, bump=0x%08x)\n",
                            (unsigned)voff, (unsigned)vi, (unsigned)elt,
                            (unsigned)cl_heap.bump);
                        platform_write_string(vbuf);
                        vc_errs++;
                    }
                }
            }
            vptr += vsize;
        }
    }
#endif

#ifdef DEBUG_GC
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "GC: compact done: %lu/%lu bytes used\n",
                 (unsigned long)cl_heap.total_allocated,
                 (unsigned long)cl_heap.arena_size);
        platform_write_string(buf);
    }
#endif

    gc_time_compact_us += platform_time_us() - t0;
    if (gc_diag_on())
        gc_diag_report("compact", platform_time_us() - t0);

    if (multithread)
        cl_gc_resume_the_world();
}
