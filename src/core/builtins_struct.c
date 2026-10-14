/*
 * builtins_struct.c — Structure type infrastructure
 *
 * Provides struct type registry, constructors, accessors, copier,
 * and type predicates for defstruct.
 *
 * Registry format (struct_table alist):
 *   ((name n-slots parent (slot-names...)) ...)
 */

#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "compiler.h"
#include "vm.h"
#include "string_utils.h"
#include "float.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>
#include <stdio.h>

/* Struct type registry:
 * alist of (name n-slots parent (slot-names...)) */
CL_Obj struct_table = CL_NIL;

/* Sentinel symbols matched by compile_call to emit OP_STRUCT_REF /
 * OP_STRUCT_SET — interned in cl_package_amilisp during init.  Same
 * pattern as cl_amiga_ffi_call_sym: external (.h) so the compiler can
 * reference them without depending on this file's internals. */
CL_Obj cl_struct_ref_sym = CL_NIL;
CL_Obj cl_struct_set_sym = CL_NIL;

/* --- Registry lookup helpers --- */

/* --- Registry hash index ---
 *
 * find_struct_entry used to walk the struct_table alist linearly.  The
 * registry is PREPENDED on registration, so early-defined types (base
 * conditions, library structs) sit at the tail and pay a full
 * O(registered-types) walk — and SLOT-VALUE on a struct instance
 * resolves its slot through here on EVERY access (2026-07-05 sento
 * runtime profile: the largest attackable CPU cluster after the VM
 * dispatch loop itself).
 *
 * The index is an open-addressing table mapping type-name symbol ->
 * registry entry, in the style of the FASL writer's MLF index (fasl.c):
 *
 * - Compaction moves symbols and entries, invalidating both the hashed
 *   key bits and the cached entry values.  cl_struct_index_gc_invalidate
 *   (called from the compaction update phase, all mutators stopped)
 *   marks the index dirty; it is rebuilt lazily on the next lookup.
 * - Registration marks the index dirty in the same wrlock critical
 *   section that prepends the entry, so a probe of a CLEAN index is
 *   always consistent with the alist.  Registration is rare and a
 *   rebuild is O(types) — negligible even during cold loads.
 * - Rebuilds allocate with platform_alloc only (never the arena), so a
 *   rebuild cannot itself trigger GC, and they walk the alist with raw
 *   cons access — cl_car can cl_error→longjmp, which must not happen
 *   while holding the wrlock (leaked-reader lesson, see fallback below).
 * - On OOM or a malformed alist cell the index is disabled permanently
 *   and lookups fall back to the linear walk — slower, never wrong. */

typedef struct {
    CL_Obj name;    /* type-name symbol; CL_NIL (== 0) = empty slot */
    CL_Obj entry;   /* registry entry (name n-slots parent (specs)) */
} StructIndexSlot;

/* Second-level index: (type-name, slot-name) -> slot index.  Built in
 * the same rebuild pass, guarded by the same dirty/disabled flags and
 * the same tables lock, so it can never be inconsistent with the type
 * index or the alist.  It exists because struct_slot_resolve — the
 * per-access cost of every SLOT-VALUE — otherwise walks the type's
 * specs list linearly: ~0.75ns per slot walked, so wide CLOS classes
 * paid measurably more for their later slots (59 -> 67ns at slot 11 of
 * 12, host 2026-07-10).  A clean-index probe answers any slot of any
 * type in O(1), and a probe miss is definitive ("no such slot") because
 * the rebuild enumerates every spec of every winning entry. */
typedef struct {
    CL_Obj name;    /* type-name symbol; CL_NIL (== 0) = empty slot */
    CL_Obj slot;    /* slot-name symbol */
    int32_t idx;    /* position in the instance's slot vector */
} SlotIdxSlot;

static struct {
    StructIndexSlot *slots;
    uint32_t cap;   /* power of two; 0 = unallocated */
    SlotIdxSlot *pairs;
    uint32_t pair_cap;  /* power of two; 0 = unallocated */
    int dirty;      /* alist changed or compaction moved objects */
    int disabled;   /* OOM / malformed cell — permanent linear fallback */
} struct_index;

static uint32_t struct_name_hash(CL_Obj obj)
{
    /* Heap offsets are CL_ALIGN-aligned, so shift the always-zero low
     * bits out before Knuth's golden-ratio multiplier (see
     * fasl_obj_hash in fasl.c). */
#if CL_ALIGN == 8
    return ((uint32_t)obj >> 3) * 2654435769u;
#else
    return ((uint32_t)obj >> 2) * 2654435769u;
#endif
}

static uint32_t slot_pair_hash(CL_Obj name, CL_Obj slot)
{
    /* Mix the two symbol hashes with distinct multipliers so (A,B) and
     * (B,A) land differently. */
    return struct_name_hash(name) ^ (struct_name_hash(slot) * 2246822519u);
}

static CL_Obj struct_entry_specs_raw(CL_Obj entry);  /* defined below */

/* Registry layout generation.  Bumped by exactly the two events that set
 * struct_index.dirty — a struct/class type (re)registration, and a
 * compaction that relocates the symbols every slot cache is keyed by — and
 * read by the per-thread slot caches (thread.h, CL_SlotICEntry) to decide
 * whether their entries still mean anything.  Starts at 1 so a zeroed
 * thread's entries (gen 0) never match.
 *
 * volatile because peer threads read it without taking the tables lock:
 * that bounds how long a thread can keep using a cached index after
 * another thread redefines a class to the hardware's coherency window.
 * (Redefining a class while other threads dispatch on it is already
 * unsynchronized in this implementation; this narrows the window rather
 * than widening it.) */
static volatile uint32_t cl_struct_layout_gen = 1;

/* Mark the index stale.  Called from gc_update_shared_roots during the
 * compaction update phase (world stopped) — and mirrored inline, under
 * the tables wrlock, by bi_register_struct_type. */
void cl_struct_index_gc_invalidate(void)
{
    struct_index.dirty = 1;
    cl_struct_layout_gen++;
}

/* (Re)build the index from the current struct_table.  Caller holds the
 * tables wrlock.  Returns 1 when the index is usable; 0 disables it. */
static int struct_index_rebuild(void)
{
    uint32_t count = 0, cap, mask;
    StructIndexSlot *slots;
    CL_Obj list;

    /* Validate + count with raw cons access (no cl_car: it can
     * cl_error→longjmp out from under the held wrlock). */
    for (list = struct_table; !CL_NULL_P(list); ) {
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
    slots = (StructIndexSlot *)platform_alloc(cap * sizeof(StructIndexSlot));
    if (!slots) goto disable;
    memset(slots, 0, cap * sizeof(StructIndexSlot));   /* CL_NIL == 0 */
    mask = cap - 1;

    /* Head-first insert, first key wins — matches the linear walk's
     * first-match semantics when a type has been re-registered. */
    for (list = struct_table; !CL_NULL_P(list); ) {
        CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(list);
        CL_Obj entry = c->car;
        CL_Obj name = ((CL_Cons *)CL_OBJ_TO_PTR(entry))->car;
        uint32_t slot = struct_name_hash(name) & mask;
        while (!CL_NULL_P(slots[slot].name) && slots[slot].name != name)
            slot = (slot + 1) & mask;
        if (CL_NULL_P(slots[slot].name)) {
            slots[slot].name = name;
            slots[slot].entry = entry;
        }
        list = c->cdr;
    }

    /* Second pass: the (type, slot) -> index pair table, enumerated
     * from the WINNING entry of each type (the type table just built),
     * so re-registered types contribute only their current layout.
     * Spec cells may be (name default) conses or bare name symbols —
     * same tolerance as struct_slot_resolve's walk; a malformed spec
     * tail just ends that type's slots (the walk stops there too). */
    {
        SlotIdxSlot *pairs;
        uint32_t pair_count = 0, pair_cap, pair_mask, i;

        for (i = 0; i < cap; i++) {
            CL_Obj specs;
            if (CL_NULL_P(slots[i].name)) continue;
            specs = struct_entry_specs_raw(slots[i].entry);
            while (CL_CONS_P(specs)) {
                pair_count++;
                specs = ((CL_Cons *)CL_OBJ_TO_PTR(specs))->cdr;
            }
        }

        pair_cap = 64;
        while (pair_cap < pair_count * 2 + 1) pair_cap <<= 1;
        pairs = (SlotIdxSlot *)platform_alloc(pair_cap * sizeof(SlotIdxSlot));
        if (!pairs) { platform_free(slots); goto disable; }
        memset(pairs, 0, pair_cap * sizeof(SlotIdxSlot));  /* CL_NIL == 0 */
        pair_mask = pair_cap - 1;

        for (i = 0; i < cap; i++) {
            CL_Obj specs;
            int32_t idx = 0;
            if (CL_NULL_P(slots[i].name)) continue;
            specs = struct_entry_specs_raw(slots[i].entry);
            while (CL_CONS_P(specs)) {
                CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(specs);
                CL_Obj sname = CL_CONS_P(c->car)
                    ? ((CL_Cons *)CL_OBJ_TO_PTR(c->car))->car
                    : c->car;
                uint32_t p = slot_pair_hash(slots[i].name, sname) & pair_mask;
                while (!CL_NULL_P(pairs[p].name) &&
                       !(pairs[p].name == slots[i].name && pairs[p].slot == sname))
                    p = (p + 1) & pair_mask;
                /* first duplicate wins — matches the walk's first match */
                if (CL_NULL_P(pairs[p].name)) {
                    pairs[p].name = slots[i].name;
                    pairs[p].slot = sname;
                    pairs[p].idx  = idx;
                }
                idx++;
                specs = c->cdr;
            }
        }

        if (struct_index.pairs) platform_free(struct_index.pairs);
        struct_index.pairs = pairs;
        struct_index.pair_cap = pair_cap;
    }

    if (struct_index.slots) platform_free(struct_index.slots);
    struct_index.slots = slots;
    struct_index.cap = cap;
    struct_index.dirty = 0;
    return 1;

disable:
    if (struct_index.slots) platform_free(struct_index.slots);
    struct_index.slots = NULL;
    struct_index.cap = 0;
    if (struct_index.pairs) platform_free(struct_index.pairs);
    struct_index.pairs = NULL;
    struct_index.pair_cap = 0;
    struct_index.disabled = 1;
    return 0;
}

/* Probe a CLEAN index for TYPE_NAME.  Caller holds the tables lock.
 * Pure memory reads — cannot error, cannot allocate. */
static CL_Obj struct_index_probe(CL_Obj type_name)
{
    uint32_t mask = struct_index.cap - 1;
    uint32_t slot = struct_name_hash(type_name) & mask;
    while (!CL_NULL_P(struct_index.slots[slot].name)) {
        if (struct_index.slots[slot].name == type_name)
            return struct_index.slots[slot].entry;
        slot = (slot + 1) & mask;
    }
    return CL_NIL;
}

/* Find registry entry for a struct type name.
 * Returns the entry (name n-slots parent (slot-names...)) or NIL.
 *
 * Fast path: O(1) probe of the hash index under the rdlock.  A dirty
 * or unbuilt index is rebuilt under the wrlock first.
 *
 * Fallback (index disabled): snapshot-and-release linear walk —
 * struct_table is only ever PREPENDED to, so once we capture the head
 * pointer under the rdlock we can release the lock and walk the
 * snapshot.  Holding the rdlock across cl_car (which can cl_error →
 * longjmp on a corrupt cell) leaked readers and ultimately tripped the
 * bi_condition_wait safety abort in sento dispatcher tests. */
static CL_Obj find_struct_entry(CL_Obj type_name)
{
    CL_Obj list, result;

    cl_tables_rdlock();
    if (struct_index.slots && !struct_index.dirty && !struct_index.disabled) {
        result = struct_index_probe(type_name);
        cl_tables_rwunlock();
        return result;
    }
    cl_tables_rwunlock();

    if (!struct_index.disabled) {
        cl_tables_wrlock();
        if (!struct_index.disabled &&
            (struct_index.slots && !struct_index.dirty
             ? 1 : struct_index_rebuild())) {
            result = struct_index_probe(type_name);
            cl_tables_rwunlock();
            return result;
        }
        cl_tables_rwunlock();
    }

    cl_tables_rdlock();
    list = struct_table;
    cl_tables_rwunlock();
    while (!CL_NULL_P(list)) {
        CL_Obj entry = cl_car(list);
        if (cl_car(entry) == type_name)
            return entry;
        list = cl_cdr(list);
    }
    return CL_NIL;
}

/* --- Public C API for printer and typep integration --- */

/* Get slot specs list for a struct type.
 * Returns list of ((name default) ...), or NIL.
 * entry = (name n-slots parent ((slot-name default) ...)) */
static CL_Obj get_slot_specs(CL_Obj type_name)
{
    CL_Obj entry = find_struct_entry(type_name);
    if (CL_NULL_P(entry)) return CL_NIL;
    return cl_car(cl_cdr(cl_cdr(cl_cdr(entry))));  /* cadddr */
}

/* Get slot names list for a struct type (for printer).
 * Extracts just the names from slot specs.
 * Returns list of slot name symbols, or NIL. */
CL_Obj cl_struct_slot_names(CL_Obj type_name)
{
    CL_Obj specs = get_slot_specs(type_name);
    CL_Obj result = CL_NIL;
    CL_Obj tail = CL_NIL;

    CL_GC_PROTECT(specs);
    CL_GC_PROTECT(result);
    CL_GC_PROTECT(tail);

    /* Extract car of each spec pair */
    while (!CL_NULL_P(specs)) {
        CL_Obj spec = cl_car(specs);
        CL_Obj name = CL_CONS_P(spec) ? cl_car(spec) : spec;
        CL_Obj cell = cl_cons(name, CL_NIL);
        if (CL_NULL_P(result)) {
            result = cell;
        } else {
            ((CL_Cons *)CL_OBJ_TO_PTR(tail))->cdr = cell;
        }
        tail = cell;
        specs = cl_cdr(specs);
    }
    CL_GC_UNPROTECT(3);
    return result;
}

/* Check if a symbol is a registered struct type. */
int cl_is_struct_type(CL_Obj type_sym)
{
    return !CL_NULL_P(find_struct_entry(type_sym));
}

/* Check if obj_type is a subtype of (or equal to) test_type.
 * Walks the parent chain in the struct registry. */
int cl_struct_type_matches(CL_Obj obj_type, CL_Obj test_type)
{
    CL_Obj entry;

    if (obj_type == test_type)
        return 1;

    /* Walk parent chain */
    entry = find_struct_entry(obj_type);
    if (CL_NULL_P(entry)) return 0;

    /* entry = (name n-slots parent (slot-names...)) */
    {
        CL_Obj parent = cl_car(cl_cdr(cl_cdr(entry)));  /* caddr */
        if (CL_NULL_P(parent)) return 0;
        return cl_struct_type_matches(parent, test_type);
    }
}

/* --- CLOS class table for typep with multiple inheritance ---
 *
 * The struct registry only tracks a single parent, but CLOS classes
 * can have multiple superclasses. The class-precedence-list (CPL)
 * stored in each class metaobject (slot 3) is the correct authority.
 * This global is set from Lisp via %set-clos-class-table. */

CL_Obj cl_clos_class_table = 0;  /* NIL until CLOS loads */

/* Eq hash table lookup from C (for *class-table* which uses :test 'eq) */
static CL_Obj ht_eq_lookup(CL_Obj ht_obj, CL_Obj key)
{
    CL_Hashtable *ht;
    uint32_t hash, bucket_idx;
    CL_Obj chain;

    if (!CL_HASHTABLE_P(ht_obj)) return CL_NIL;

    ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
    hash = (uint32_t)key;
    hash ^= hash >> 16;
    hash *= 0x45d9f3bU;
    hash ^= hash >> 16;
    bucket_idx = hash & (ht->bucket_count - 1);
    /* Access buckets through indirection (supports rehashed tables) */
    if (!CL_NULL_P(ht->bucket_vec)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(ht->bucket_vec);
        chain = v->data[bucket_idx];
    } else {
        chain = ht->buckets[bucket_idx];
    }

    while (!CL_NULL_P(chain)) {
        CL_Obj pair = cl_car(chain);
        if (cl_car(pair) == key)
            return cl_cdr(pair);
        chain = cl_cdr(chain);
    }
    return CL_NIL;
}

/* Check CLOS class hierarchy via class-precedence-list.
 * Returns 1 if obj_type is a subtype of test_type per CLOS CPL.
 *
 * Snapshot-and-release: capture the class metaobject under the rdlock,
 * release, then walk its CPL.  Holding the rdlock across cl_car (which
 * can cl_error→longjmp on a corrupt cell) leaks readers — and the CPL
 * walker calls into CLOS-aware code (cl_car can hit a malformed cell). */
int cl_clos_type_matches(CL_Obj obj_type, CL_Obj test_type)
{
    CL_Obj class_obj, cpl;
    CL_Struct *class_st;

    cl_tables_rdlock();
    if (CL_NULL_P(cl_clos_class_table)) {
        cl_tables_rwunlock();
        return 0;
    }
    class_obj = ht_eq_lookup(cl_clos_class_table, obj_type);
    cl_tables_rwunlock();

    if (CL_NULL_P(class_obj) || !CL_STRUCT_P(class_obj))
        return 0;

    /* CPL is in slot 3 of the class metaobject */
    class_st = (CL_Struct *)CL_OBJ_TO_PTR(class_obj);
    if (class_st->n_slots < 4)
        return 0;
    cpl = class_st->slots[3];

    while (!CL_NULL_P(cpl)) {
        CL_Obj cpl_class = cl_car(cpl);
        if (CL_STRUCT_P(cpl_class)) {
            CL_Struct *cpl_st = (CL_Struct *)CL_OBJ_TO_PTR(cpl_class);
            if (cpl_st->n_slots > 0 && cpl_st->slots[0] == test_type)
                return 1;
        }
        cpl = cl_cdr(cpl);
    }
    return 0;
}

/* Check if a name is in the CLOS class table. */
int cl_clos_class_exists(CL_Obj name)
{
    int result;
    cl_tables_rdlock();
    if (CL_NULL_P(cl_clos_class_table)) {
        cl_tables_rwunlock();
        return 0;
    }
    result = !CL_NULL_P(ht_eq_lookup(cl_clos_class_table, name));
    cl_tables_rwunlock();
    return result;
}

static CL_Obj bi_set_clos_class_table(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    cl_tables_wrlock();
    cl_clos_class_table = args[0];
    cl_tables_rwunlock();
    return args[0];
}

/* --- Builtins --- */

/* (%register-struct-type name n-slots parent slot-names) */
static CL_Obj bi_register_struct_type(CL_Obj *args, int n)
{
    CL_Obj name = args[0];
    CL_Obj n_slots_obj = args[1];
    CL_Obj parent = args[2];
    CL_Obj slot_names = args[3];
    CL_Obj entry;
    CL_UNUSED(n);

    if (!CL_SYMBOL_P(name))
        cl_error(CL_ERR_TYPE, "%%REGISTER-STRUCT-TYPE: name must be a symbol");
    if (!CL_FIXNUM_P(n_slots_obj))
        cl_error(CL_ERR_TYPE, "%%REGISTER-STRUCT-TYPE: n-slots must be a fixnum");

    /* Build entry: (name n-slots parent (slot-names...)) */
    CL_GC_PROTECT(name);
    CL_GC_PROTECT(parent);
    CL_GC_PROTECT(slot_names);

    entry = cl_cons(slot_names, CL_NIL);
    entry = cl_cons(parent, entry);
    entry = cl_cons(n_slots_obj, entry);
    entry = cl_cons(name, entry);
    /* Prepend + index dirty-mark must be ONE wrlock critical section,
     * so a probe of a clean index is always consistent with the alist.
     * The head cell is consed OUTSIDE the lock (arena allocation under
     * the wrlock is an STW-vs-rwlock deadlock — see
     * cl_table_prepend_locked, whose shape this mirrors). */
    {
        CL_Obj cell = cl_cons(entry, CL_NIL);
        cl_tables_wrlock();
        ((CL_Cons *)CL_OBJ_TO_PTR(cell))->cdr = struct_table;
        struct_table = cell;
        struct_index.dirty = 1;
        /* Retires every thread's cached slot index for this type — and,
         * cheaply, for all others (see cl_struct_layout_gen). */
        cl_struct_layout_gen++;
        cl_tables_rwunlock();
    }

    CL_GC_UNPROTECT(3);
    return name;
}

/* --- Layout dependencies of compiled code ---
 *
 * DEFSTRUCT's compiler macros bake a structure's layout into the CALLER:
 * an accessor call compiles to OP_STRUCT_REF/SET with a constant slot
 * index, a keyword constructor call to a positional %MAKE-STRUCT with the
 * slot defaults spliced in, and an (:INCLUDE parent) copies the parent's
 * slot specs.  A FASL of such code is only valid against the layout it was
 * compiled with, and the file that holds the DEFSTRUCT is usually another
 * one — so a FASL kept across an edit of that DEFSTRUCT (LOAD's implicit
 * cache is keyed by the dependent file's OWN mtime) reads and writes the
 * wrong slots without any error.
 *
 * Each of those expansions therefore reports the structure's name here.
 * While a file is being compiled for a FASL, *STRUCT-LAYOUT-DEPS* is bound
 * to the list of names collected so far (LOAD and COMPILE-FILE bind it to
 * NIL per file, so nested loads keep their own lists); its global value T
 * means "nobody is recording".  The FASL writer stores a layout hash per
 * name (cl_struct_layout_hash) and the loader compares it against the live
 * registry BEFORE running any unit — see the DEPS trailer in fasl.c.  The
 * struct-access opcodes stay unchecked: the cost is paid once per file
 * load, not once per slot access. */
CL_Obj cl_struct_deps_sym = CL_NIL;

void cl_struct_note_layout_use(CL_Obj name)
{
    CL_Obj deps, p;

    if (!CL_SYMBOL_P(name) || CL_NULL_P(name)) return;
    deps = cl_symbol_value(cl_struct_deps_sym);
    if (!CL_NULL_P(deps) && !CL_CONS_P(deps)) return;   /* T: not recording */
    for (p = deps; CL_CONS_P(p); p = cl_cdr(p))
        if (cl_car(p) == name) return;
    /* NAME is an argument of the calling builtin (rooted); DEPS is reachable
     * from the binding — but read it again after the allocation, which may
     * have moved it. */
    CL_GC_PROTECT(name);
    p = cl_cons(name, CL_NIL);
    CL_GC_UNPROTECT(1);
    ((CL_Cons *)CL_OBJ_TO_PTR(p))->cdr = cl_symbol_value(cl_struct_deps_sym);
    cl_set_symbol_value(cl_struct_deps_sym, p);
}

#define LAYOUT_HASH_MAX_DEPTH 64

static uint32_t layout_hash_byte(uint32_t h, uint32_t b)
{
    return (h ^ (b & 0xFFu)) * 16777619u;   /* FNV-1a */
}

static uint32_t layout_hash_u32(uint32_t h, uint32_t v)
{
    h = layout_hash_byte(h, v >> 24);
    h = layout_hash_byte(h, v >> 16);
    h = layout_hash_byte(h, v >> 8);
    return layout_hash_byte(h, v);
}

static uint32_t layout_hash_chars(uint32_t h, CL_Obj str)
{
    uint32_t i, len = cl_string_length(str);
    for (i = 0; i < len; i++)
        h = layout_hash_u32(h, (uint32_t)cl_string_char_at(str, i));
    return h;
}

/* Hash a slot-spec tree by CONTENT, never by heap offset: the value has to
 * come out the same in the process that compiled a file and in the one
 * that loads it — another run, a host-built FASL on the Amiga, a narrow or
 * a wide-string build.  Symbols count by name and, unless keyword, home
 * package name (an uninterned one only as "a gensym": its name carries a
 * counter), strings by code point, fixnums and characters by value,
 * bignums/floats/ratios/complexes by their numeric content (recursively for
 * ratio/complex, since their components are themselves numbers), anything
 * else by its heap type alone.  Never allocates and never signals.
 *
 * Float bit patterns are extracted with memcpy rather than a union or
 * pointer cast: a FLOAT's in-memory byte order already follows the host's
 * own multi-byte convention (big-endian m68k vs little-endian host), so the
 * memcpy'd bits, read back as an integer on that same host, always equal
 * the canonical IEEE-754 bit pattern — and layout_hash_u32 hashes it
 * byte-by-byte via shifts, so the resulting hash itself doesn't depend on
 * host endianness either. */
static uint32_t layout_hash_obj(uint32_t h, CL_Obj obj, int depth)
{
    for (;;) {
        if (CL_NULL_P(obj)) return layout_hash_byte(h, 3);
        if (CL_FIXNUM_P(obj))
            return layout_hash_u32(layout_hash_byte(h, 1),
                                   (uint32_t)CL_FIXNUM_VAL(obj));
        if (CL_CHAR_P(obj))
            return layout_hash_u32(layout_hash_byte(h, 2),
                                   (uint32_t)CL_CHAR_VAL(obj));
        if (CL_SYMBOL_P(obj)) {
            CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(obj);
            if (CL_NULL_P(s->package)) return layout_hash_byte(h, 5);
            if (s->package == cl_package_keyword)
                return layout_hash_chars(layout_hash_byte(h, 10), s->name);
            h = layout_hash_chars(layout_hash_byte(h, 4),
                    ((CL_Package *)CL_OBJ_TO_PTR(s->package))->name);
            return layout_hash_chars(h, s->name);
        }
        if (CL_ANY_STRING_P(obj))
            return layout_hash_chars(layout_hash_byte(h, 6), obj);
        if (CL_SINGLE_FLOAT_P(obj)) {
            CL_SingleFloat *sf = (CL_SingleFloat *)CL_OBJ_TO_PTR(obj);
            uint32_t bits;
            memcpy(&bits, &sf->value, sizeof(bits));
            return layout_hash_u32(layout_hash_byte(h, 12), bits);
        }
        if (CL_DOUBLE_FLOAT_P(obj)) {
            CL_DoubleFloat *df = (CL_DoubleFloat *)CL_OBJ_TO_PTR(obj);
            uint64_t bits;
            memcpy(&bits, &df->value, sizeof(bits));
            h = layout_hash_byte(h, 13);
            h = layout_hash_u32(h, (uint32_t)(bits >> 32));
            return layout_hash_u32(h, (uint32_t)bits);
        }
        if (CL_BIGNUM_P(obj)) {
            CL_Bignum *bn = (CL_Bignum *)CL_OBJ_TO_PTR(obj);
            uint32_t i;
            h = layout_hash_u32(layout_hash_byte(h, 14), bn->sign);
            h = layout_hash_u32(h, bn->length);
            for (i = 0; i < bn->length; i++)
                h = layout_hash_u32(h, (uint32_t)bn->limbs[i]);
            return h;
        }
        if (CL_RATIO_P(obj)) {
            CL_Ratio *r = (CL_Ratio *)CL_OBJ_TO_PTR(obj);
            if (depth >= LAYOUT_HASH_MAX_DEPTH) return layout_hash_byte(h, 11);
            h = layout_hash_obj(layout_hash_byte(h, 15), r->numerator, depth + 1);
            return layout_hash_obj(h, r->denominator, depth + 1);
        }
        if (CL_COMPLEX_P(obj)) {
            CL_Complex *c = (CL_Complex *)CL_OBJ_TO_PTR(obj);
            if (depth >= LAYOUT_HASH_MAX_DEPTH) return layout_hash_byte(h, 11);
            h = layout_hash_obj(layout_hash_byte(h, 16), c->realpart, depth + 1);
            return layout_hash_obj(h, c->imagpart, depth + 1);
        }
        if (!CL_CONS_P(obj)) {
            h = layout_hash_byte(h, 9);
            return CL_HEAP_P(obj)
                ? layout_hash_byte(h, (uint32_t)CL_HDR_TYPE(CL_OBJ_TO_PTR(obj)))
                : h;
        }
        if (depth >= LAYOUT_HASH_MAX_DEPTH) return layout_hash_byte(h, 11);
        h = layout_hash_byte(h, 7);
        h = layout_hash_obj(h, ((CL_Cons *)CL_OBJ_TO_PTR(obj))->car, depth + 1);
        obj = ((CL_Cons *)CL_OBJ_TO_PTR(obj))->cdr;
        depth++;
    }
}

/* Layout hash of a registered structure type: slot count, parent and the
 * full slot specs (names in order, defaults, options), inherited slots
 * included — everything DEFSTRUCT's compiler macros copy into a caller.
 * Returns 0 when TYPE_NAME is not registered (then *hash_out is untouched). */
int cl_struct_layout_hash(CL_Obj type_name, uint32_t *hash_out)
{
    CL_Obj entry = find_struct_entry(type_name);
    if (!CL_CONS_P(entry)) return 0;
    /* entry = (name n-slots parent specs); the name is the lookup key */
    *hash_out = layout_hash_obj(2166136261u, cl_cdr(entry), 0);
    return 1;
}

/* (%note-struct-use name) — see "Layout dependencies" above. */
static CL_Obj bi_note_struct_use(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    cl_struct_note_layout_use(args[0]);
    return CL_NIL;
}

/* (%struct-acc-form name idx obj)     => (amilisp::%struct-ref obj idx)
 * (%struct-acc-form name idx obj val) => (amilisp::%struct-set obj idx val)
 * The body of every DEFSTRUCT accessor's compiler macro: builds the inline
 * access form AND records that the code being compiled depends on NAME's
 * layout, in one call, so the accessor macros stay as small as they were. */
static CL_Obj bi_struct_acc_form(CL_Obj *args, int n)
{
    cl_struct_note_layout_use(args[0]);
    if (n == 4)
        return cl_list4(cl_struct_set_sym, args[2], args[1], args[3]);
    return cl_list3(cl_struct_ref_sym, args[2], args[1]);
}

/* (%make-struct name slot-val...) */
static CL_Obj bi_make_struct(CL_Obj *args, int n)
{
    CL_Obj name = args[0];
    int n_slots = n - 1;
    CL_Obj obj;
    CL_Struct *st;
    int i;

    if (!CL_SYMBOL_P(name))
        cl_error(CL_ERR_TYPE, "%%MAKE-STRUCT: name must be a symbol");

    obj = cl_make_struct(name, (uint32_t)n_slots);
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    for (i = 0; i < n_slots; i++)
        st->slots[i] = args[i + 1];

    return obj;
}

/* (%register-funcallable-gf-type type-name) — declare TYPE-NAME (a struct-type
 * name symbol naming a STANDARD-GENERIC-FUNCTION subclass) as a funcallable
 * generic-function type, so its instances dispatch as functions.  Supports
 * custom generic-function metaclasses (CLHS :generic-function-class). */
static CL_Obj bi_register_funcallable_gf_type(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    if (!CL_SYMBOL_P(args[0]))
        cl_error(CL_ERR_TYPE,
                 "%%REGISTER-FUNCALLABLE-GF-TYPE: name must be a symbol");
    cl_register_funcallable_gf_type(args[0]);
    return args[0];
}

/* (%struct-ref obj index) */
static CL_Obj bi_struct_ref(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    int idx;
    CL_Struct *st;
    CL_UNUSED(n);

    if (!CL_STRUCT_P(obj))
        cl_error(CL_ERR_TYPE, "%%STRUCT-REF: not a structure");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "%%STRUCT-REF: index must be a fixnum");

    /* Validate struct object is within arena bounds */
    if (obj >= cl_heap.arena_size) {
        fprintf(stderr, "[BUG] %%STRUCT-REF: obj 0x%08x beyond arena (size 0x%08x)\n",
                (unsigned)obj, (unsigned)cl_heap.arena_size);
        cl_capture_backtrace();
        fprintf(stderr, "%s", cl_backtrace_buf);
        cl_error(CL_ERR_GENERAL, "%%STRUCT-REF: struct 0x%08x out of arena", (unsigned)obj);
    }

    idx = CL_FIXNUM_VAL(args[1]);
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);

    /* Validate n_slots field is sane */
    if (st->n_slots > 1000) {
        fprintf(stderr, "[BUG] %%STRUCT-REF: obj 0x%08x n_slots=%u (likely corrupt)\n",
                (unsigned)obj, (unsigned)st->n_slots);
        cl_capture_backtrace();
        fprintf(stderr, "%s", cl_backtrace_buf);
        cl_error(CL_ERR_GENERAL, "%%STRUCT-REF: corrupt struct (n_slots=%u)", (unsigned)st->n_slots);
    }

    if (idx < 0 || (uint32_t)idx >= st->n_slots)
        cl_error(CL_ERR_ARGS, "%%STRUCT-REF: index %d out of range (n_slots=%u)",
                 idx, (unsigned)st->n_slots);

    /* Validate slot value is a valid CL_Obj before returning */
    {
        CL_Obj val = st->slots[idx];
        if (CL_HEAP_P(val) && val >= cl_heap.arena_size) {
            fprintf(stderr, "[BUG] %%STRUCT-REF: obj 0x%08x slot[%d]=0x%08x beyond arena\n",
                    (unsigned)obj, idx, (unsigned)val);
            cl_capture_backtrace();
            fprintf(stderr, "%s", cl_backtrace_buf);
            cl_error(CL_ERR_GENERAL, "%%STRUCT-REF: slot[%d]=0x%08x out of arena", idx, (unsigned)val);
        }
        return val;
    }
}

/* (%struct-set obj index val) */
static CL_Obj bi_struct_set(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    int idx;
    CL_Struct *st;
    CL_UNUSED(n);

    if (!CL_STRUCT_P(obj))
        cl_error(CL_ERR_TYPE, "%%STRUCT-SET: not a structure");
    if (!CL_FIXNUM_P(args[1]))
        cl_error(CL_ERR_TYPE, "%%STRUCT-SET: index must be a fixnum");

    idx = CL_FIXNUM_VAL(args[1]);
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);

    if (idx < 0 || (uint32_t)idx >= st->n_slots)
        cl_error(CL_ERR_ARGS, "%%STRUCT-SET: index %d out of range", idx);

    /* Publication barrier — same reasoning as OP_STRUCT_SET in vm.c: the
     * stored value may be a freshly-built object graph read lock-free by
     * peer threads (CLOS dispatch metadata); order its initializing
     * stores before the publishing store on weakly-ordered CPUs. */
    if (CL_MT()) platform_memory_barrier();
    st->slots[idx] = args[2];
    return args[2];
}

/* (%copy-struct obj) */
static CL_Obj bi_copy_struct(CL_Obj *args, int n)
{
    CL_Obj obj = args[0];
    CL_Struct *src;
    CL_Obj copy;
    CL_Struct *dst;
    uint32_t i;
    CL_UNUSED(n);

    if (!CL_STRUCT_P(obj))
        cl_error(CL_ERR_TYPE, "%%COPY-STRUCT: not a structure");

    src = (CL_Struct *)CL_OBJ_TO_PTR(obj);

    CL_GC_PROTECT(obj);
    copy = cl_make_struct(src->type_desc, src->n_slots);
    CL_GC_UNPROTECT(1);

    /* Re-fetch src after potential GC */
    src = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    dst = (CL_Struct *)CL_OBJ_TO_PTR(copy);
    for (i = 0; i < src->n_slots; i++)
        dst->slots[i] = src->slots[i];

    return copy;
}

/* (%struct-type-name obj) */
static CL_Obj bi_struct_type_name(CL_Obj *args, int n)
{
    CL_Struct *st;
    CL_UNUSED(n);

    if (!CL_STRUCT_P(args[0]))
        cl_error(CL_ERR_TYPE, "%%STRUCT-TYPE-NAME: not a structure");

    st = (CL_Struct *)CL_OBJ_TO_PTR(args[0]);
    return st->type_desc;
}

/* (structurep obj) */
DEFINE_TYPE_PREDICATE(bi_structurep, CL_STRUCT_P)

/* (%struct-slot-names type-name) — for :include support in macro */
static CL_Obj bi_struct_slot_names(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return cl_struct_slot_names(args[0]);
}

/* (%struct-slot-specs type-name) — returns ((name default) ...) for :include */
static CL_Obj bi_struct_slot_specs(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return get_slot_specs(args[0]);
}

/* (%struct-slot-index type-name slot-name) — position of SLOT-NAME in
 * struct type TYPE-NAME's slots, or NIL.  Zero-allocation: SLOT-VALUE /
 * (SETF SLOT-VALUE) / SLOT-BOUNDP on struct instances resolve slots
 * through here on every access (clos.lisp %find-struct-slot-index); the
 * previous shape (%STRUCT-SLOT-NAMES + Lisp-side linear match) consed a
 * fresh name list per access. */
static CL_Obj bi_struct_slot_index(CL_Obj *args, int n)
{
    CL_Obj specs = get_slot_specs(args[0]);
    int32_t idx = 0;
    CL_UNUSED(n);

    while (!CL_NULL_P(specs)) {
        CL_Obj spec = cl_car(specs);
        CL_Obj name = CL_CONS_P(spec) ? cl_car(spec) : spec;
        if (name == args[1])
            return CL_MAKE_FIXNUM(idx);
        idx++;
        specs = cl_cdr(specs);
    }
    return CL_NIL;
}

/* --- Fused fast-path slot access (spec 3.1, second half) ---
 *
 * SLOT-VALUE and the DEFCLASS-generated accessor functions used to pay
 * ~4 Lisp calls + ~8 builtin dispatches per access (class-of, the
 * slot-index-table branch, %STRUCT-SLOT-INDEX, then a separate
 * %STRUCT-REF).  These two builtins fuse the whole common case into ONE
 * builtin call: type-name -> registry entry (O(1) hash probe) -> slot
 * index (short EQ scan of the specs) -> direct slot read/write.
 *
 * They are deliberately NON-ERRORING and NON-ALLOCATING: any case the
 * fast path cannot handle (not a struct, unregistered type, no such
 * instance slot, index beyond n_slots on an obsolete instance) reports
 * a miss and the Lisp caller falls back to the full SLOT-VALUE protocol
 * — which owns :CLASS-allocated slots, conditions, SLOT-UNBOUND, and
 * the "no slot named X" error.  Because the registry entry is looked up
 * by the instance's own type_desc on EVERY access, the resolved index
 * is always the current layout: correct across class redefinition and
 * for subclass instances whose inherited slots sit at different indices
 * (the hazard a captured-index accessor closure would have).
 *
 * The specs list is walked with raw checked cons access, not cl_car:
 * these run on the hottest Lisp path and must not error out of a
 * malformed registry cell — a malformed cell is just a miss. */

/* Return the specs list ((name default) ...) of a registry entry, or
 * CL_NIL if the entry shape is unexpected.  Raw, non-erroring cadddr. */
static CL_Obj struct_entry_specs_raw(CL_Obj entry)
{
    int i;
    for (i = 0; i < 3; i++) {
        if (!CL_CONS_P(entry)) return CL_NIL;
        entry = ((CL_Cons *)CL_OBJ_TO_PTR(entry))->cdr;
    }
    if (!CL_CONS_P(entry)) return CL_NIL;
    return ((CL_Cons *)CL_OBJ_TO_PTR(entry))->car;
}

/* Probe a CLEAN pair index for (TYPE_NAME, SLOT_NAME).  Caller holds
 * the tables lock.  Returns the slot index, or -1 — and -1 is
 * definitive, because the rebuild enumerated every spec of every
 * winning entry.  Pure memory reads — cannot error, cannot allocate. */
static int32_t slot_pair_probe(CL_Obj type_name, CL_Obj slot_name)
{
    uint32_t mask = struct_index.pair_cap - 1;
    uint32_t p = slot_pair_hash(type_name, slot_name) & mask;
    while (!CL_NULL_P(struct_index.pairs[p].name)) {
        if (struct_index.pairs[p].name == type_name &&
            struct_index.pairs[p].slot == slot_name)
            return struct_index.pairs[p].idx;
        p = (p + 1) & mask;
    }
    return -1;
}

/* Resolve SLOT_NAME to its index in OBJ's registered layout.
 * Returns the index, or -1 on any miss.  Non-erroring, non-allocating.
 *
 * A per-thread (type, slot) -> index cache sits in front of the shared
 * pair index (thread.h, CL_SlotICEntry).  On a repeat access — which is
 * what a slot read in a loop, an accessor fallback, or WITH-SLOTS always
 * is — the answer costs a generation compare and two identity compares,
 * with no tables rdlock/unlock pair and no hash probe.  The index bound
 * check stays OUTSIDE the cache: two instances of one type_desc can have
 * different n_slots (an instance obsoleted by class redefinition), so the
 * cached position is the type's layout position, re-checked against THIS
 * instance every time. */
static int32_t struct_slot_resolve(CL_Obj obj, CL_Obj slot_name)
{
    CL_Struct *st;
    CL_Obj entry, specs, type_desc;
    CL_SlotICEntry *ic;
    uint32_t gen;
    int32_t raw = -1;
    int32_t idx = 0;

    if (!CL_STRUCT_P(obj))
        return -1;
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    type_desc = st->type_desc;

    /* Read the generation once: no allocation happens below, so it cannot
     * change under us, and the compare and the store must agree. */
    gen = cl_struct_layout_gen;
    ic = &cl_get_current_thread()->slot_ic[
             slot_pair_hash(type_desc, slot_name) & (CL_SLOT_IC_SIZE - 1)];
    if (ic->gen == gen && ic->type == type_desc && ic->slot == slot_name) {
        raw = ic->idx;
        return (raw >= 0 && (uint32_t)raw < st->n_slots) ? raw : -1;
    }

    /* O(1) pair-index probe — the shared fallback.  Falls through to the
     * rebuild-then-walk path only when the index is dirty, unbuilt, or
     * disabled. */
    cl_tables_rdlock();
    if (struct_index.pairs && !struct_index.dirty && !struct_index.disabled) {
        raw = slot_pair_probe(type_desc, slot_name);
        cl_tables_rwunlock();
        goto found;
    }
    cl_tables_rwunlock();

    /* find_struct_entry never allocates from the arena (index rebuilds
     * use platform_alloc), so st cannot move underneath us. */
    entry = find_struct_entry(type_desc);
    if (CL_NULL_P(entry))
        goto found;                 /* raw stays -1: unregistered type */
    specs = struct_entry_specs_raw(entry);
    while (CL_CONS_P(specs)) {
        CL_Cons *c = (CL_Cons *)CL_OBJ_TO_PTR(specs);
        CL_Obj name = CL_CONS_P(c->car)
            ? ((CL_Cons *)CL_OBJ_TO_PTR(c->car))->car
            : c->car;
        if (name == slot_name) {
            raw = idx;
            goto found;
        }
        idx++;
        specs = c->cdr;
    }

found:
    /* Cache the LAYOUT answer, negative results included: "this type has
     * no such slot" is just as worth not re-deriving as a hit. */
    ic->type = type_desc;
    ic->slot = slot_name;
    ic->idx  = raw;
    ic->gen  = gen;
    return (raw >= 0 && (uint32_t)raw < st->n_slots) ? raw : -1;
}

/* (%struct-slot-value obj slot-name miss) — value of the instance slot
 * SLOT-NAME in OBJ, or MISS when the fast path does not apply.  Callers
 * pass the slot-unbound marker as MISS, so an unbound CLOS slot (whose
 * storage holds that marker) also reads back as a miss and routes to
 * the slow path's SLOT-UNBOUND protocol for free. */
static CL_Obj bi_struct_slot_value(CL_Obj *args, int n)
{
    int32_t idx = struct_slot_resolve(args[0], args[1]);
    CL_UNUSED(n);
    if (idx < 0)
        return args[2];
    return ((CL_Struct *)CL_OBJ_TO_PTR(args[0]))->slots[idx];
}

/* (%struct-slot-store obj slot-name value) — store VALUE into the
 * instance slot SLOT-NAME of OBJ.  Returns T when stored, NIL when the
 * fast path does not apply (caller falls back to (SETF SLOT-VALUE)'s
 * full protocol). */
static CL_Obj bi_struct_slot_store(CL_Obj *args, int n)
{
    int32_t idx = struct_slot_resolve(args[0], args[1]);
    CL_UNUSED(n);
    if (idx < 0)
        return CL_NIL;
    /* Publication barrier — see OP_STRUCT_SET in vm.c. */
    if (CL_MT()) platform_memory_barrier();
    ((CL_Struct *)CL_OBJ_TO_PTR(args[0]))->slots[idx] = args[2];
    return SYM_T;
}

/* (%struct-slot-count type-name) — for :include support in macro */
static CL_Obj bi_struct_slot_count(CL_Obj *args, int n)
{
    CL_Obj entry;
    CL_UNUSED(n);

    entry = find_struct_entry(args[0]);
    if (CL_NULL_P(entry)) return CL_MAKE_FIXNUM(0);

    /* entry = (name n-slots parent ((slot-spec) ...)) */
    return cl_car(cl_cdr(entry));  /* cadr = n-slots */
}

/*
 * (%class-of obj) — return CL type-name symbol for any object.
 * For structs: returns the struct type name (e.g., POINT).
 * For built-in types: returns canonical CL class name symbol.
 * Used by CLOS class-of to map objects to class metaobjects.
 */
/* Resolve a built-in class NAME to its canonical class-name symbol.
 *
 * The built-in class names (SYMBOL, CONS, STRING, FIXNUM, BIGNUM, RATIO, ...)
 * are all standard COMMON-LISP-package symbols, and *class-table* (see
 * clos.lisp) is keyed by those exact CL-package symbols.  They MUST be
 * resolved in the COMMON-LISP package — NOT via *PACKAGE*-relative cl_intern.
 *
 * cl_intern interns relative to cl_current_package (historically a shared C
 * global that other threads' *PACKAGE* binds leaked through; per-thread since
 * the SLYNK-IO-PACKAGE FASL-poisoning fix, but still *PACKAGE*-relative).  If
 * this thread's *PACKAGE* is a package that does not resolve the name to its
 * CL-package symbol — e.g. KEYWORD during a #. / #+ reader excursion —
 * cl_intern would
 * return a *different* symbol.  (gethash that-symbol *class-table*) then misses
 * and class-of silently falls back to the T class, which makes CLOS dispatch
 * compute an empty applicable-method set even though a method plainly applies
 * ("No applicable method for ... (X SYMBOL)").  Struct/condition objects are
 * unaffected (they return their stored type_desc/type_name), which is exactly
 * why the field failures only ever mis-classified the built-in-typed argument.
 * Interning in cl_package_cl makes class-of independent of *PACKAGE*
 * entirely. */
/* The built-in class names, resolved ONCE at init and held in GC-registered
 * statics rather than re-interned per call.
 *
 * cl_intern_in hashes the name string and probes the package table on every
 * call; class_of_type_name runs on every CLOS dispatch whose receiver is not
 * a struct — a fixnum, a symbol, a string — so the intern showed up in the
 * sento profile under %GF-IC-EMF.  The names are all standard COMMON-LISP
 * symbols that exist for the life of the process, so caching them is pure
 * profit.  Indices are private to this file; keep them in step with
 * class_name_init below. */
enum {
    CN_NULL = 0, CN_FIXNUM, CN_CHARACTER, CN_CONS, CN_SYMBOL, CN_STRING,
    CN_FUNCTION, CN_COMPILED_FUNCTION, CN_VECTOR, CN_ARRAY, CN_PACKAGE,
    CN_HASH_TABLE, CN_BIGNUM, CN_SINGLE_FLOAT, CN_DOUBLE_FLOAT, CN_RATIO,
    CN_STREAM, CN_RANDOM_STATE, CN_BIT_VECTOR, CN_PATHNAME, CN_T,
    CN_COUNT
};

static CL_Obj class_name_cache[CN_COUNT];

static const struct { const char *name; uint32_t len; } class_name_table[] = {
    { "NULL", 4 }, { "FIXNUM", 6 }, { "CHARACTER", 9 }, { "CONS", 4 },
    { "SYMBOL", 6 }, { "STRING", 6 }, { "FUNCTION", 8 },
    { "COMPILED-FUNCTION", 17 }, { "VECTOR", 6 }, { "ARRAY", 5 },
    { "PACKAGE", 7 }, { "HASH-TABLE", 10 }, { "BIGNUM", 6 },
    { "SINGLE-FLOAT", 12 }, { "DOUBLE-FLOAT", 12 }, { "RATIO", 5 },
    { "STREAM", 6 }, { "RANDOM-STATE", 12 }, { "BIT-VECTOR", 10 },
    { "PATHNAME", 8 }, { "T", 1 }
};

/* Resolve every built-in class name in the COMMON-LISP package and register
 * the cache as GC roots so compaction forwards them.  Called from
 * cl_builtins_struct_init.
 *
 * The names MUST be resolved in COMMON-LISP, never through a *PACKAGE*-
 * relative cl_intern: *class-table* (clos.lisp) is keyed by those exact
 * CL-package symbols, and a KEYWORD-relative excursion (a #. or #+ reader
 * form) used to yield a different symbol, whereupon class-of silently fell
 * back to the T class and CLOS computed an empty applicable-method set for
 * an argument a method plainly applied to. */
static void class_name_init(void)
{
    int i;
    for (i = 0; i < CN_COUNT; i++) {
        class_name_cache[i] = cl_intern_in(class_name_table[i].name,
                                           class_name_table[i].len,
                                           cl_package_cl);
        cl_gc_register_root(&class_name_cache[i]);
    }
}

/* Built-in class name by index.  Falls back to interning if init has not
 * run yet (a partially built runtime in a unit test), so this is never
 * wrong, only occasionally slower. */
static CL_Obj class_name_sym(int idx)
{
    CL_Obj s = class_name_cache[idx];
    if (CL_NULL_P(s))
        return cl_intern_in(class_name_table[idx].name,
                            class_name_table[idx].len, cl_package_cl);
    return s;
}

/* Core of %CLASS-OF: map any object to its CL class-name symbol.
 * Non-allocating once class_name_init has run (the boot case falls back to
 * interning); a struct or condition answers from its own header. */
static CL_Obj class_of_type_name(CL_Obj obj)
{
    if (CL_NULL_P(obj))
        return class_name_sym(CN_NULL);
    if (CL_FIXNUM_P(obj))
        return class_name_sym(CN_FIXNUM);
    if (CL_CHAR_P(obj))
        return class_name_sym(CN_CHARACTER);

    if (CL_HEAP_P(obj)) {
        switch (CL_HDR_TYPE(CL_OBJ_TO_PTR(obj))) {
        case TYPE_CONS:
            return class_name_sym(CN_CONS);
        case TYPE_SYMBOL:
            return class_name_sym(CN_SYMBOL);
        case TYPE_STRING:
#ifdef CL_WIDE_STRINGS
        case TYPE_WIDE_STRING:
#endif
            return class_name_sym(CN_STRING);
        case TYPE_FUNCTION:
        case TYPE_CLOSURE:
        case TYPE_FFI_STUB:
            return class_name_sym(CN_FUNCTION);
        case TYPE_BYTECODE:
            return class_name_sym(CN_COMPILED_FUNCTION);
        case TYPE_VECTOR: {
            CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
            if (v->flags & CL_VEC_FLAG_STRING)
                return class_name_sym(CN_STRING);
            return (v->rank <= 1)
                ? class_name_sym(CN_VECTOR)
                : class_name_sym(CN_ARRAY);
        }
        case TYPE_PACKAGE:
            return class_name_sym(CN_PACKAGE);
        case TYPE_HASHTABLE:
            return class_name_sym(CN_HASH_TABLE);
        case TYPE_CONDITION: {
            CL_Condition *cond = (CL_Condition *)CL_OBJ_TO_PTR(obj);
            return cond->type_name;
        }
        case TYPE_STRUCT: {
            CL_Struct *st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
            return st->type_desc;
        }
        case TYPE_BIGNUM:
            return class_name_sym(CN_BIGNUM);
        case TYPE_SINGLE_FLOAT:
            return class_name_sym(CN_SINGLE_FLOAT);
        case TYPE_DOUBLE_FLOAT:
            return class_name_sym(CN_DOUBLE_FLOAT);
        case TYPE_RATIO:
            return class_name_sym(CN_RATIO);
        case TYPE_STREAM:
            return class_name_sym(CN_STREAM);
        case TYPE_RANDOM_STATE:
            return class_name_sym(CN_RANDOM_STATE);
        case TYPE_BIT_VECTOR:
            return class_name_sym(CN_BIT_VECTOR);
        case TYPE_BYTE_VECTOR:
            /* No standard class for specialized byte vectors — they are
             * instances of the built-in VECTOR class for CLOS dispatch. */
            return class_name_sym(CN_VECTOR);
        case TYPE_PATHNAME:
            return class_name_sym(CN_PATHNAME);
        default:
            break;
        }
    }
    return class_name_sym(CN_T);
}

static CL_Obj bi_class_of(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    return class_of_type_name(args[0]);
}

/* Is CLS a class metaobject whose CLASS-NAME is NAME?  Slot 0 of a class
 * struct holds its name (clos.lisp CLASS-NAME).  Non-erroring: anything
 * that is not a struct with at least one slot simply is not that class. */
static int cl_class_named_p(CL_Obj cls, CL_Obj name)
{
    CL_Struct *cs;
    if (!CL_STRUCT_P(cls))
        return 0;
    cs = (CL_Struct *)CL_OBJ_TO_PTR(cls);
    return cs->n_slots >= 1 && cs->slots[0] == name;
}

/* (%gf-ic-emf gf a) / (%gf-ic-emf gf a b) — probe GF's inline cache
 * (GF slot 8: (class . emf) for 1-arg GFs, (class1 class2 . emf) for
 * 2-arg) against the receivers' classes.  Returns the cached EMF on a
 * hit, NIL on any miss — the Lisp slow path (%GF-DISPATCH-*-SLOW) owns
 * everything else, including populating the cache.
 *
 * This fuses the per-call dispatch chain the arity-specialized
 * discriminators used to run in Lisp — (gf-inline-cache gf) call,
 * CLASS-OF call (builtin + *CLASS-TABLE* gethash), EQ compare — into
 * one builtin call (spec 3.1, second half: the class-of/generic-lookup
 * chain was the measured remainder of accessor cost).
 *
 * Non-erroring by design: any unexpected shape is a miss.  A receiver
 * whose type name is not in *CLASS-TABLE* is a miss too — the slow
 * path's CLASS-OF resolves it to the T class and dispatches correctly
 * (such a receiver can never legitimately hit an IC entry, since ICs
 * are populated with table-resident classes).
 *
 * The check runs in the direction that needs no table and no lock: rather
 * than resolving the receiver's class NAME to a class object through
 * *CLASS-TABLE* (a tables rdlock plus an EQ-hash probe per dispatch, and
 * per ARGUMENT for a 2-arg GF), it compares the receiver's name against
 * the name of the class already cached in the IC — slot 0 of a class
 * metaobject is its name (CLASS-NAME, clos.lisp).  The two are equivalent
 * for cache-validation purposes: a hit still means "the receiver's class
 * is exactly the cached one", and every event that could make a cached
 * class stale (class redefinition, ADD-METHOD, REMOVE-METHOD) already
 * clears slot 8.  A receiver whose type name is not in *CLASS-TABLE*
 * misses either way — before, because the lookup yielded NIL; now,
 * because no cached class carries that name.
 *
 * GC note: class_of_type_name allocates only if it has to intern a class
 * name, which cannot happen once class_name_init has run — but "cannot
 * happen after boot" is not a licence to hold an unprotected CL_Obj across
 * it.  So both names are resolved FIRST (name1 protected across name2's
 * resolution), and the GF's own fields are read from the rooted argument
 * slot afterwards; nothing here outlives an allocation. */
static CL_Obj bi_gf_ic_emf(CL_Obj *args, int n)
{
    CL_Obj name1, name2 = CL_NIL, ic;
    CL_Struct *st;
    CL_Cons *c;
    int two = (n == 3);

    if (!CL_STRUCT_P(args[0]))
        return CL_NIL;

    name1 = class_of_type_name(args[1]);
    if (two) {
        CL_GC_PROTECT(name1);
        name2 = class_of_type_name(args[2]);
        CL_GC_UNPROTECT(1);
    }

    /* args[] are GC-rooted VM-stack slots, so re-deriving st here is safe
     * even if the resolutions above compacted. */
    st = (CL_Struct *)CL_OBJ_TO_PTR(args[0]);
    if (st->n_slots < 9)
        return CL_NIL;
    ic = st->slots[8];
    if (!CL_CONS_P(ic))
        return CL_NIL;

    c = (CL_Cons *)CL_OBJ_TO_PTR(ic);
    if (!cl_class_named_p(c->car, name1))
        return CL_NIL;
    if (!two)
        return c->cdr;
    if (!CL_CONS_P(c->cdr))
        return CL_NIL;
    c = (CL_Cons *)CL_OBJ_TO_PTR(c->cdr);
    if (!cl_class_named_p(c->car, name2))
        return CL_NIL;
    return c->cdr;
}

/* (%gf-reader-ic gf obj miss) — hit path for a reader-only generic
 * function (see %MAKE-READER-DISCRIMINATOR in clos.lisp).  GF slot 8
 * holds the reader inline cache: a list of up to CL_READER_IC_MAX
 * (TYPE-NAME . SLOT-INDEX) entries, most-recently-missed first, so one
 * call site serving a few receiver classes hits for all of them instead
 * of thrashing through full dispatch on every alternation (measured
 * ~80x per access).  The EMF inline caches that share slot 8 put a
 * class object in their car ((class . emf) / (class1 class2 . emf));
 * reader entries are conses, so a non-cons first element means "not a
 * reader cache" and reads as a miss — the shapes can never be confused,
 * and every existing slot-8 invalidation (ADD-METHOD, REMOVE-METHOD,
 * class redefinition) invalidates this cache too.
 *
 * A struct's class identity is its type_desc symbol, right in the object
 * header, so each probe step is a single word compare.  It pays neither
 * CLASS-OF's intern nor the *CLASS-TABLE* and slot-index hash probes
 * that %STRUCT-SLOT-VALUE pays on every access — that resolution chain
 * was the measured bulk of accessor cost.
 *
 * Non-erroring and non-allocating by design: any unexpected shape
 * returns MISS and the caller falls back to full dispatch.  Callers pass
 * *SLOT-UNBOUND-MARKER* as MISS, so an unbound slot (whose storage holds
 * exactly that marker) reads back as a miss and routes to the
 * SLOT-UNBOUND protocol for free — the same trick bi_struct_slot_value
 * uses.  Allocation-free means no GC can run here, so nothing needs
 * protecting across the type_desc read. */
/* CLOS's *SLOT-UNBOUND-MARKER*, published from clos.lisp via
 * %SET-SLOT-UNBOUND-MARKER.  cl_gf_reader_ic_probe needs it to tell an
 * unbound slot (whose storage holds the marker) apart from a real value,
 * because unlike the Lisp entry point it has no caller-supplied MISS to
 * hand back.  GC root: it is an arena-resident uninterned symbol that the
 * compactor relocates.  While it is still CL_NIL (before clos.lisp loads)
 * the probe reports a miss for every call, so the fast path is inert
 * rather than wrong. */
static CL_Obj cl_slot_unbound_marker = CL_NIL;

/* Max reader-IC entries the probe walks.  %GF-READER-1-MISS in clos.lisp
 * caps installs at the same depth (1 fresh + 3 kept); the walk cap also
 * bounds OP_CALL's worst case against a malformed over-long list. */
#define CL_READER_IC_MAX 4

/* Shared hit path for a reader-only GF.  Returns the slot value, or
 * CL_UNBOUND to mean "miss, use the full dispatch".  CL_UNBOUND is not a
 * valid CL_Obj and can never be a slot's value, so it is unambiguous.
 *
 * An unbound slot reports a miss, which routes the caller to the
 * SLOT-UNBOUND protocol.  So does a slot holding the marker before the
 * marker has been published (both are CL_NIL then) — inert, not wrong. */
CL_Obj cl_gf_reader_ic_probe(CL_Obj gfobj, CL_Obj obj)
{
    CL_Struct *gf, *st;
    CL_Cons *spine, *e;
    CL_Obj ic, entry, v;
    int32_t idx;
    int i;

    if (!CL_STRUCT_P(gfobj) || !CL_STRUCT_P(obj))
        return CL_UNBOUND;
    gf = (CL_Struct *)CL_OBJ_TO_PTR(gfobj);
    if (gf->n_slots < 9)
        return CL_UNBOUND;
    ic = gf->slots[8];
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    for (i = 0; i < CL_READER_IC_MAX && CL_CONS_P(ic); i++) {
        spine = (CL_Cons *)CL_OBJ_TO_PTR(ic);
        entry = spine->car;
        if (!CL_CONS_P(entry))
            return CL_UNBOUND; /* EMF-shaped cache, not ours */
        e = (CL_Cons *)CL_OBJ_TO_PTR(entry);
        if (!CL_FIXNUM_P(e->cdr))
            return CL_UNBOUND; /* not a reader entry */
        if (e->car == st->type_desc) {
            idx = (int32_t)CL_FIXNUM_VAL(e->cdr);
            if (idx < 0 || (uint32_t)idx >= st->n_slots)
                return CL_UNBOUND;
            v = st->slots[idx];
            if (v == cl_slot_unbound_marker)
                return CL_UNBOUND; /* unbound -> SLOT-UNBOUND protocol */
            return v;
        }
        ic = spine->cdr;
    }
    return CL_UNBOUND;
}

static CL_Obj bi_gf_reader_ic(CL_Obj *args, int n)
{
    CL_Obj v;
    CL_UNUSED(n);
    v = cl_gf_reader_ic_probe(args[0], args[1]);
    return (v == CL_UNBOUND) ? args[2] : v;
}

/* Writer counterpart of cl_gf_reader_ic_probe: (setf (x obj) val) on a
 * promoted writer-only GF stores straight into the cached slot index.
 * Writer IC entries reuse the (TYPE-NAME . FIXNUM) cons shape but encode
 * the slot index as (- -1 idx) — always negative — so the reader and
 * writer probes can never answer from each other's caches: the reader
 * probe rejects a negative index, and this probe rejects a non-negative
 * one.  (Without that, a 1-arg call to a promoted writer GF whose
 * receiver matched an entry would return the slot value instead of
 * signalling the arity error.)  EMF caches stay unambiguous as before:
 * their first element is a class object, never a cons.
 *
 * Returns VAL on a hit; CL_UNBOUND means "miss, take full dispatch".
 * Storing the unbound-slot marker itself is reported as a miss so the
 * full SLOT-VALUE protocol decides what that means — the probe must
 * never manufacture an unbound slot on its own.  Non-allocating and
 * non-erroring, same as the reader probe. */
CL_Obj cl_gf_writer_ic_probe(CL_Obj gfobj, CL_Obj val, CL_Obj obj)
{
    CL_Struct *gf, *st;
    CL_Cons *spine, *e;
    CL_Obj ic, entry;
    int32_t idx;
    int i;

    if (!CL_STRUCT_P(gfobj) || !CL_STRUCT_P(obj))
        return CL_UNBOUND;
    if (val == cl_slot_unbound_marker)
        return CL_UNBOUND; /* never fabricate an unbound slot */
    gf = (CL_Struct *)CL_OBJ_TO_PTR(gfobj);
    if (gf->n_slots < 9)
        return CL_UNBOUND;
    ic = gf->slots[8];
    st = (CL_Struct *)CL_OBJ_TO_PTR(obj);
    for (i = 0; i < CL_READER_IC_MAX && CL_CONS_P(ic); i++) {
        spine = (CL_Cons *)CL_OBJ_TO_PTR(ic);
        entry = spine->car;
        if (!CL_CONS_P(entry))
            return CL_UNBOUND; /* EMF-shaped cache, not ours */
        e = (CL_Cons *)CL_OBJ_TO_PTR(entry);
        if (!CL_FIXNUM_P(e->cdr))
            return CL_UNBOUND; /* not an accessor entry */
        idx = (int32_t)CL_FIXNUM_VAL(e->cdr);
        if (idx >= 0)
            return CL_UNBOUND; /* reader IC, not a writer's */
        if (e->car == st->type_desc) {
            idx = -1 - idx;
            if ((uint32_t)idx >= st->n_slots)
                return CL_UNBOUND;
            /* Publication barrier — see OP_STRUCT_SET in vm.c.  This is
             * the (setf accessor) fast path: it stores user values into
             * instance slots just like %STRUCT-SLOT-STORE, so it gets
             * the same ordering guarantee. */
            if (CL_MT()) platform_memory_barrier();
            st->slots[idx] = val;
            return val;
        }
        ic = spine->cdr;
    }
    return CL_UNBOUND;
}

static CL_Obj bi_gf_writer_ic(CL_Obj *args, int n)
{
    CL_Obj v;
    CL_UNUSED(n);
    v = cl_gf_writer_ic_probe(args[0], args[1], args[2]);
    return (v == CL_UNBOUND) ? args[3] : v;
}

/* (%set-slot-unbound-marker marker) — publish CLOS's unbound-slot marker
 * to the VM's reader fast path.  Called once from clos.lisp. */
static CL_Obj bi_set_slot_unbound_marker(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    cl_slot_unbound_marker = args[0];
    return args[0];
}

/* --- Registration --- */

/* (%struct-change-class obj new-type-name new-slot-count)
 * Modify a struct's type_desc and n_slots in-place if the allocated size allows.
 * Returns T on success, NIL if the struct doesn't have enough allocated space. */
static CL_Obj bi_struct_change_class(CL_Obj *args, int n)
{
    CL_Struct *st;
    uint32_t new_n_slots, alloc_size, needed_size;
    CL_UNUSED(n);

    if (!CL_STRUCT_P(args[0]))
        cl_error(CL_ERR_TYPE, "%%STRUCT-CHANGE-CLASS: not a structure");
    if (!CL_FIXNUM_P(args[2]))
        cl_error(CL_ERR_TYPE, "%%STRUCT-CHANGE-CLASS: slot count must be a fixnum");

    st = (CL_Struct *)CL_OBJ_TO_PTR(args[0]);
    new_n_slots = (uint32_t)CL_FIXNUM_VAL(args[2]);

    /* Check if current allocation can hold the new slot count */
    alloc_size = CL_HDR_SIZE(st);
    needed_size = sizeof(CL_Struct) + new_n_slots * sizeof(CL_Obj);
    if (needed_size > alloc_size)
        return CL_NIL;  /* Not enough space */

    /* Zero any new slots beyond old n_slots */
    if (new_n_slots > st->n_slots) {
        uint32_t i;
        for (i = st->n_slots; i < new_n_slots; i++)
            st->slots[i] = CL_NIL;
    }

    st->type_desc = args[1];
    st->n_slots = new_n_slots;
    return SYM_T;
}

/* Release the struct/CLOS slot index at process exit.
 *
 * Both arrays are platform_alloc'd (outside the GC arena) and are rebuilt
 * from scratch whenever the registry changes, so nothing but an explicit
 * teardown ever hands them back.  They scale with the image: a CLOS-heavy
 * session interns one `pairs` entry per (type, slot) pair, which reaches
 * tens of KB before ASDF is even loaded.  On AmigaOS that is Fast RAM the
 * machine does not get back until reboot. */
void cl_builtins_struct_shutdown(void)
{
    if (struct_index.slots) platform_free(struct_index.slots);
    struct_index.slots = NULL;
    struct_index.cap = 0;
    if (struct_index.pairs) platform_free(struct_index.pairs);
    struct_index.pairs = NULL;
    struct_index.pair_cap = 0;
    struct_index.dirty = 0;
    struct_index.disabled = 0;
}

void cl_builtins_struct_init(void)
{
    /* A re-initialized runtime (test harnesses: cl_mem_shutdown +
     * cl_mem_init + *_init again) starts with a FRESH arena, but these
     * statics survive and still hold the previous runtime's arena
     * offsets.  A stale struct_table tail gets marked/updated by the GC
     * as if live — garbage treated as object starts, corrupting the
     * heap on the next compaction (surfaced as a layout-dependent
     * "CAR: not of type LIST" in test_gc_markstack's fresh-runtime
     * phase).  Reset registry, CLOS class table and index; in a normal
     * once-per-process boot this is a no-op. */
    struct_table = CL_NIL;
    cl_clos_class_table = CL_NIL;
    cl_slot_unbound_marker = CL_NIL;
    /* Same reasoning for the class-name cache: these are registered GC
     * roots, so a leftover offset from the previous arena would be marked
     * and forwarded as if live by the first collection the registration
     * calls below trigger.  class_name_init at the end of this function
     * refills them; until then class_name_sym interns on demand. */
    {
        int cni;
        for (cni = 0; cni < CN_COUNT; cni++) class_name_cache[cni] = CL_NIL;
    }
    if (struct_index.slots) platform_free(struct_index.slots);
    struct_index.slots = NULL;
    struct_index.cap = 0;
    if (struct_index.pairs) platform_free(struct_index.pairs);
    struct_index.pairs = NULL;
    struct_index.pair_cap = 0;
    struct_index.dirty = 0;
    struct_index.disabled = 0;

    cl_register_builtin("%REGISTER-STRUCT-TYPE", bi_register_struct_type, 4, 4, cl_package_amilisp);
    cl_register_builtin("%MAKE-STRUCT", bi_make_struct, 1, -1, cl_package_amilisp);
    cl_register_builtin("%REGISTER-FUNCALLABLE-GF-TYPE",
                        bi_register_funcallable_gf_type, 1, 1, cl_package_amilisp);
    cl_register_builtin("%STRUCT-REF", bi_struct_ref, 2, 2, cl_package_amilisp);
    cl_register_builtin("%STRUCT-SET", bi_struct_set, 3, 3, cl_package_amilisp);
    cl_register_builtin("%COPY-STRUCT", bi_copy_struct, 1, 1, cl_package_amilisp);
    defun("COPY-STRUCTURE", bi_copy_struct, 1, 1);
    cl_register_builtin("%STRUCT-TYPE-NAME", bi_struct_type_name, 1, 1, cl_package_amilisp);
    /* STRUCTUREP — implementation predicate in AMILISP.
     * CL :uses AMILISP so existing internal code sees it as a bare symbol.
     * User packages that only (:use :common-lisp) must add (:use :amilisp)
     * or qualify with the CLAMIGA: prefix. */
    cl_register_builtin("STRUCTUREP", bi_structurep, 1, 1, cl_package_amilisp);
    cl_register_builtin("%STRUCT-SLOT-NAMES", bi_struct_slot_names, 1, 1, cl_package_amilisp);
    cl_register_builtin("%STRUCT-SLOT-SPECS", bi_struct_slot_specs, 1, 1, cl_package_amilisp);
    cl_register_builtin("%STRUCT-SLOT-INDEX", bi_struct_slot_index, 2, 2, cl_package_amilisp);
    cl_register_builtin("%STRUCT-SLOT-VALUE", bi_struct_slot_value, 3, 3, cl_package_amilisp);
    cl_register_builtin("%STRUCT-SLOT-STORE", bi_struct_slot_store, 3, 3, cl_package_amilisp);
    cl_register_builtin("%STRUCT-SLOT-COUNT", bi_struct_slot_count, 1, 1, cl_package_amilisp);
    cl_register_builtin("%CLASS-OF", bi_class_of, 1, 1, cl_package_amilisp);
    cl_register_builtin("%GF-IC-EMF", bi_gf_ic_emf, 2, 3, cl_package_amilisp);
    cl_register_builtin("%GF-READER-IC", bi_gf_reader_ic, 3, 3, cl_package_amilisp);
    cl_register_builtin("%GF-WRITER-IC", bi_gf_writer_ic, 4, 4, cl_package_amilisp);
    cl_register_builtin("%SET-SLOT-UNBOUND-MARKER", bi_set_slot_unbound_marker,
                        1, 1, cl_package_amilisp);
    cl_register_builtin("%SET-CLOS-CLASS-TABLE", bi_set_clos_class_table, 1, 1, cl_package_amilisp);
    cl_register_builtin("%STRUCT-CHANGE-CLASS", bi_struct_change_class, 3, 3, cl_package_amilisp);
    cl_register_builtin("%NOTE-STRUCT-USE", bi_note_struct_use, 1, 1, cl_package_amilisp);
    cl_register_builtin("%STRUCT-ACC-FORM", bi_struct_acc_form, 3, 4, cl_package_amilisp);

    /* Cache the symbols compile_call matches against to emit dedicated
     * struct-access bytecodes.  The %STRUCT-REF / %STRUCT-SET builtins
     * stay registered above as the runtime fallback for callers with a
     * dynamically-computed slot index. */
    /* Built-in class-name symbols for %CLASS-OF / the GF inline cache. */
    class_name_init();

    cl_struct_ref_sym = cl_intern_in("%STRUCT-REF", 11, cl_package_amilisp);
    cl_struct_set_sym = cl_intern_in("%STRUCT-SET", 11, cl_package_amilisp);
    cl_gc_register_root(&cl_struct_ref_sym);
    cl_gc_register_root(&cl_struct_set_sym);
    cl_gc_register_root(&cl_slot_unbound_marker);

    /* *STRUCT-LAYOUT-DEPS*: global value T = not recording (see "Layout
     * dependencies of compiled code" above). */
    cl_struct_deps_sym = cl_intern_in("*STRUCT-LAYOUT-DEPS*", 20, cl_package_amilisp);
    {
        CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(cl_struct_deps_sym);
        s->flags |= CL_SYM_SPECIAL;
        s->value = SYM_T;
    }
    cl_gc_register_root(&cl_struct_deps_sym);
}
