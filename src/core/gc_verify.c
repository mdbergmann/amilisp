/* gc_verify.c — DEBUG_GC post-mark / post-sweep verifiers, the
 * root-registry audit, and the on-demand heap verifier
 * (ext:%heap-verify).  Nothing here runs on a release path except
 * the two audits.  State: gc_internal.h.
 */
#include "gc_internal.h"

#ifdef DEBUG_GC
int gc_verify_errors;  /* defined/reset inside gc_verify_marked */
#endif

#undef gc_root_count

/* Audit the GC root registries for double registration.
 *
 * gc_forward is NOT idempotent (the forwarding table is keyed by
 * pre-compaction offsets), so a CL_Obj slot reachable from two root
 * entries is forwarded twice on compaction: the second lookup maps the
 * already-forwarded offset through whatever object's OLD offset it now
 * coincides with, silently rewriting the root to an unrelated object.
 * Every slot must therefore appear at most once across global_roots and
 * all thread root stacks combined.  Returns the number of violations
 * and prints one line per offender so the source can be found.
 *
 * Walks cl_thread_list and every thread's gc_roots[]/gc_root_count, which
 * are otherwise only safe to read under a stop-the-world pause (peer
 * threads mutate their own root stack via CL_GC_PROTECT/UNPROTECT, and
 * thread create/exit mutates the list's `next` links).  Since this is a
 * diagnostic entry point reachable from Lisp at any time (ext:%gc-audit-roots),
 * bracket the walk with cl_gc_stop_the_world()/cl_gc_resume_the_world()
 * ourselves, matching the pattern in cl_gc_compact(). */
int cl_gc_audit_roots(void)
{
    int violations = 0;
    int i, j;
    CL_Thread *t, *t2;
    int multithread = (cl_thread_count > 1);

    if (multithread)
        cl_gc_stop_the_world();

    for (i = 0; i < n_global_roots; i++) {
        for (j = i + 1; j < n_global_roots; j++) {
            if (global_roots[i] == global_roots[j]) {
                fprintf(stderr, "GC root audit: address registered twice "
                        "in global roots\n");
                violations++;
            }
        }
        for (t = cl_thread_list; t; t = t->next) {
            for (j = 0; j < t->gc_root_count; j++) {
                if (t->gc_roots[j] == global_roots[i]) {
                    fprintf(stderr, "GC root audit: global root also on a "
                            "thread root stack (double-forward hazard)\n");
                    violations++;
                }
            }
        }
    }

    /* Duplicate address registered twice within the same thread's own
     * gc_roots[] stack (e.g. an accidental double CL_GC_PROTECT). */
    for (t = cl_thread_list; t; t = t->next) {
        for (i = 0; i < t->gc_root_count; i++) {
            for (j = i + 1; j < t->gc_root_count; j++) {
                if (t->gc_roots[i] == t->gc_roots[j]) {
                    fprintf(stderr, "GC root audit: address registered "
                            "twice on the same thread's root stack\n");
                    violations++;
                }
            }
        }
    }

    /* Same address registered on two different threads' root stacks. */
    for (t = cl_thread_list; t; t = t->next) {
        for (t2 = t->next; t2; t2 = t2->next) {
            for (i = 0; i < t->gc_root_count; i++) {
                for (j = 0; j < t2->gc_root_count; j++) {
                    if (t->gc_roots[i] == t2->gc_roots[j]) {
                        fprintf(stderr, "GC root audit: address registered "
                                "on two different thread root stacks "
                                "(double-forward hazard)\n");
                        violations++;
                    }
                }
            }
        }
    }

    /* Informational: roots aliasing an independently-forwarded thread
     * region (VM stack, mv_values, pending throw values, dyn/handler/
     * restart stacks, vm_extra_args, compiler chain).  Harmless by
     * construction — gc_update_registered_roots skips them — but each
     * marks a redundant CL_GC_PROTECT worth removing at the source.
     * Not counted as violations. */
    for (t = cl_thread_list; t; t = t->next) {
        for (i = 0; i < t->gc_root_count; i++) {
            if (root_slot_independently_forwarded(t->gc_roots[i])) {
                fprintf(stderr, "GC root audit: note - thread root #%d "
                        "aliases an independently-forwarded thread region "
                        "(redundant CL_GC_PROTECT; skipped by the root "
                        "dedup pass)\n", i);
            }
        }
    }

    if (multithread)
        cl_gc_resume_the_world();

    return violations;
}

/* Post-GC verification: check all marked objects have valid children.
 * Must be called AFTER gc_mark() but BEFORE gc_sweep() (marks still set).
 * Reports any marked object that points to an unmarked heap object. */
#ifdef DEBUG_GC

/* Dump the per-thread GC root stack with file:line tags (DEBUG_GC only).
 * Call this after gc_verify_marked() finds errors to name the C frames that
 * hold the suspicious roots — the same technique used to finger cl_cons's
 * arg at mem.c:378 in the PROGN-cursor bug.
 *
 * Must #undef gc_root_count because thread.h defines it as (CT->gc_root_count),
 * which collides with t->gc_root_count member access. */
#undef gc_root_count
void gc_dump_roots_dbg(void)
{
    /* During STW GC all threads are stopped; cl_thread_list is stable — no lock needed. */
    CL_Thread *t;
    char buf[256];

    for (t = cl_thread_list; t; t = t->next) {
        int i;
        snprintf(buf, sizeof(buf),
                 "GC-ROOTS: thread %u - %d protected roots:\n",
                 (unsigned)t->id, t->gc_root_count);
        platform_write_string(buf);

        for (i = 0; i < t->gc_root_count; i++) {
            CL_Obj val = *t->gc_roots[i];
            int is_heap = (!CL_NULL_P(val) && !CL_FIXNUM_P(val) && !CL_CHAR_P(val)
                           && val != CL_UNBOUND && val < cl_heap.arena_size);
            int marked  = (is_heap && CL_HDR_MARKED(CL_OBJ_TO_PTR(val)));

            snprintf(buf, sizeof(buf),
                     "  root[%d] val=0x%08x %s  @ %s:%d\n",
                     i, (unsigned)val,
                     is_heap ? (marked ? "(heap,marked)" : "(heap,UNMARKED)") : "(imm)",
                     t->gc_root_files[i] ? t->gc_root_files[i] : "?",
                     t->gc_root_lines[i]);
            platform_write_string(buf);
        }
    }
}
#define gc_root_count (CT->gc_root_count)

static void gc_verify_check_ref(CL_Obj parent_offset, const char *field,
                                CL_Obj child)
{
    void *child_ptr;
    if (CL_NULL_P(child) || CL_FIXNUM_P(child) || CL_CHAR_P(child))
        return;
    if (child >= cl_heap.arena_size) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "GC VERIFY: marked @0x%08x.%s -> OUT OF BOUNDS 0x%08x (arena 0x%08x)\n",
                 (unsigned)parent_offset, field,
                 (unsigned)child, (unsigned)cl_heap.arena_size);
        platform_write_string(buf);
        gc_verify_errors++;
        return;
    }
    child_ptr = CL_OBJ_TO_PTR(child);
    if (!CL_HDR_MARKED(child_ptr)) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "GC VERIFY: marked @0x%08x.%s -> unmarked @0x%08x (type %u)\n",
                 (unsigned)parent_offset, field,
                 (unsigned)child, (unsigned)CL_HDR_TYPE(child_ptr));
        platform_write_string(buf);
        gc_verify_errors++;
    }
}

void gc_verify_marked(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;
    gc_verify_errors = 0;

    while (ptr < end) {
        uint32_t size = CL_HDR_SIZE(ptr);
        if (size == 0) break;

        if (CL_HDR_MARKED(ptr)) {
            CL_Obj parent_off = (CL_Obj)(ptr - cl_heap.arena);
            uint8_t type = CL_HDR_TYPE(ptr);

            switch (type) {
            case TYPE_CONS: {
                CL_Cons *c = (CL_Cons *)ptr;
                gc_verify_check_ref(parent_off, "car", c->car);
                gc_verify_check_ref(parent_off, "cdr", c->cdr);
                break;
            }
            case TYPE_SYMBOL: {
                CL_Symbol *s = (CL_Symbol *)ptr;
                gc_verify_check_ref(parent_off, "name", s->name);
                if (s->value != CL_UNBOUND)
                    gc_verify_check_ref(parent_off, "value", s->value);
                if (s->function != CL_UNBOUND)
                    gc_verify_check_ref(parent_off, "function", s->function);
                gc_verify_check_ref(parent_off, "plist", s->plist);
                gc_verify_check_ref(parent_off, "package", s->package);
                break;
            }
            case TYPE_CLOSURE: {
                CL_Closure *cl = (CL_Closure *)ptr;
                uint32_t n = (size - sizeof(CL_Closure)) / sizeof(CL_Obj);
                uint32_t i;
                gc_verify_check_ref(parent_off, "bytecode", cl->bytecode);
                for (i = 0; i < n; i++)
                    gc_verify_check_ref(parent_off, "upval", cl->upvalues[i]);
                break;
            }
            case TYPE_BYTECODE: {
                CL_Bytecode *bc = (CL_Bytecode *)ptr;
                uint16_t i;
                gc_verify_check_ref(parent_off, "name", bc->name);
                gc_verify_check_ref(parent_off, "lambda-list", bc->source_lambda_list);
                for (i = 0; i < bc->n_constants; i++)
                    gc_verify_check_ref(parent_off, "const", bc->constants[i]);
                break;
            }
            case TYPE_VECTOR: {
                CL_Vector *v = (CL_Vector *)ptr;
                uint32_t i;
                if (v->flags & CL_VEC_FLAG_DISPLACED) {
                    gc_verify_check_ref(parent_off, "displaced",
                                        v->data[CL_DISP_BASE_IDX(v)]);
                } else {
                    uint32_t n = (v->rank > 1) ? (uint32_t)v->rank + v->length : v->length;
                    for (i = 0; i < n; i++)
                        gc_verify_check_ref(parent_off, "elt", v->data[i]);
                }
                break;
            }
            case TYPE_HASHTABLE: {
                CL_Hashtable *ht = (CL_Hashtable *)ptr;
                uint32_t i;
                gc_verify_check_ref(parent_off, "bucket_vec", ht->bucket_vec);
                if (CL_NULL_P(ht->bucket_vec)) {
                    for (i = 0; i < ht->bucket_count; i++)
                        gc_verify_check_ref(parent_off, "bucket", ht->buckets[i]);
                }
                break;
            }
            case TYPE_STRUCT: {
                CL_Struct *st = (CL_Struct *)ptr;
                uint32_t i;
                gc_verify_check_ref(parent_off, "type_desc", st->type_desc);
                for (i = 0; i < st->n_slots; i++)
                    gc_verify_check_ref(parent_off, "slot", st->slots[i]);
                break;
            }
            case TYPE_CONDITION: {
                CL_Condition *cond = (CL_Condition *)ptr;
                gc_verify_check_ref(parent_off, "type_name", cond->type_name);
                gc_verify_check_ref(parent_off, "slots", cond->slots);
                gc_verify_check_ref(parent_off, "report", cond->report_string);
                break;
            }
            case TYPE_RESTART: {
                CL_Restart *r = (CL_Restart *)ptr;
                gc_verify_check_ref(parent_off, "name", r->name);
                gc_verify_check_ref(parent_off, "function", r->function);
                gc_verify_check_ref(parent_off, "report", r->report);
                gc_verify_check_ref(parent_off, "interactive", r->interactive);
                gc_verify_check_ref(parent_off, "test", r->test);
                gc_verify_check_ref(parent_off, "tag", r->tag);
                break;
            }
            case TYPE_FFI_STUB: {
                CL_FfiStub *fs = (CL_FfiStub *)ptr;
                gc_verify_check_ref(parent_off, "name", fs->name);
                gc_verify_check_ref(parent_off, "aux", fs->aux);
                break;
            }
            case TYPE_STREAM: {
                CL_Stream *st = (CL_Stream *)ptr;
                gc_verify_check_ref(parent_off, "string_buf", st->string_buf);
                gc_verify_check_ref(parent_off, "element_type", st->element_type);
                break;
            }
            case TYPE_FUNCTION: {
                CL_Function *f = (CL_Function *)ptr;
                gc_verify_check_ref(parent_off, "name", f->name);
                break;
            }
            case TYPE_RATIO: {
                CL_Ratio *r = (CL_Ratio *)ptr;
                gc_verify_check_ref(parent_off, "num", r->numerator);
                gc_verify_check_ref(parent_off, "den", r->denominator);
                break;
            }
            case TYPE_COMPLEX: {
                CL_Complex *cx = (CL_Complex *)ptr;
                gc_verify_check_ref(parent_off, "real", cx->realpart);
                gc_verify_check_ref(parent_off, "imag", cx->imagpart);
                break;
            }
            case TYPE_PATHNAME: {
                CL_Pathname *pn = (CL_Pathname *)ptr;
                gc_verify_check_ref(parent_off, "host", pn->host);
                gc_verify_check_ref(parent_off, "device", pn->device);
                gc_verify_check_ref(parent_off, "dir", pn->directory);
                gc_verify_check_ref(parent_off, "name", pn->name);
                gc_verify_check_ref(parent_off, "type", pn->type);
                gc_verify_check_ref(parent_off, "version", pn->version);
                break;
            }
            case TYPE_CELL: {
                CL_Cell *cell = (CL_Cell *)ptr;
                gc_verify_check_ref(parent_off, "value", cell->value);
                break;
            }
            case TYPE_PACKAGE: {
                CL_Package *p = (CL_Package *)ptr;
                gc_verify_check_ref(parent_off, "name", p->name);
                gc_verify_check_ref(parent_off, "symbols", p->symbols);
                gc_verify_check_ref(parent_off, "use_list", p->use_list);
                gc_verify_check_ref(parent_off, "nicknames", p->nicknames);
                gc_verify_check_ref(parent_off, "local_nicknames", p->local_nicknames);
                gc_verify_check_ref(parent_off, "shadowing", p->shadowing_symbols);
                gc_verify_check_ref(parent_off, "exported", p->exported_symbols);
                gc_verify_check_ref(parent_off, "bindings", p->bindings);
                break;
            }
            default:
                break;
            }
            if (gc_verify_errors > 20) {
                platform_write_string("GC VERIFY: too many errors, stopping\n");
                return;
            }
        }
        ptr += size;
    }
}

/* Check if an arena offset points to a freed block by testing for poison
 * fill at offset 8 (sizeof(CL_FreeBlock) = 8).  All freed blocks >= 16
 * bytes (CL_MIN_ALLOC_SIZE) have bytes 8-11 poisoned with 0xDE. */
static int gc_is_freed(uint32_t offset)
{
    uint8_t *p = cl_heap.arena + offset;
    /* The poison word lives at bytes 8..11 — it must stay inside the
     * walked arena (callers pass arbitrary CL_Obj values here). */
    if (offset + 12 > cl_heap.bump)
        return 0;
    /* Blocks of exactly sizeof(CL_FreeBlock) — the 8-byte pin-gap chunks
     * gc_make_free_gap can emit — have no room for poison: their bytes
     * 8..11 belong to the NEXT block, so testing them would misreport
     * based on a neighbor's data.  Treat as not-freed (a false negative
     * only; this is a DEBUG_GC diagnostic helper). */
    if (CL_HDR_SIZE(p) <= sizeof(CL_FreeBlock))
        return 0;
    /* Freed blocks have poison at offset 8 (after the 8-byte CL_FreeBlock header) */
    return (p[8] == 0xDE && p[9] == 0xDE && p[10] == 0xDE && p[11] == 0xDE);
}

/* Post-sweep verification: check that no live object contains a reference
 * to a freed block (use-after-free detection).  Uses poison fill pattern
 * to identify freed blocks (can't use type==0 since TYPE_CONS==0). */
void gc_verify_after_sweep(void)
{
    uint8_t *ptr = cl_heap.arena + CL_ALIGN;
    uint8_t *end = cl_heap.arena + cl_heap.bump;
    int errors = 0;

    while (ptr < end) {
        uint32_t obj_size = CL_HDR_SIZE(ptr);
        CL_Obj parent_off = (CL_Obj)(ptr - cl_heap.arena);

        if (obj_size == 0) break;

        /* Skip free blocks */
        if (gc_is_freed(parent_off)) {
            ptr += obj_size;
            continue;
        }

        /* Live object — check CL_Obj fields for poison or dead refs */
        {
            uint8_t type = CL_HDR_TYPE(ptr);

            #define CHECK_FIELD(val, fname) do { \
                CL_Obj _v = (val); \
                if (_v == 0xDEDEDEDEu) { \
                    char buf[256]; \
                    snprintf(buf, sizeof(buf), \
                             "POST-SWEEP: @0x%08x.%s = POISON 0xDEDEDEDE (type %u)\n", \
                             (unsigned)parent_off, fname, (unsigned)type); \
                    platform_write_string(buf); \
                    errors++; \
                } else if (!CL_NULL_P(_v) && !CL_FIXNUM_P(_v) && !CL_CHAR_P(_v) \
                           && _v < cl_heap.arena_size && gc_is_freed(_v)) { \
                    char buf[256]; \
                    snprintf(buf, sizeof(buf), \
                             "POST-SWEEP: @0x%08x.%s -> freed @0x%08x (type %u)\n", \
                             (unsigned)parent_off, fname, (unsigned)_v, (unsigned)type); \
                    platform_write_string(buf); \
                    errors++; \
                } \
            } while(0)

            switch (type) {
            case TYPE_CONS: {
                CL_Cons *c = (CL_Cons *)ptr;
                CHECK_FIELD(c->car, "car");
                CHECK_FIELD(c->cdr, "cdr");
                break;
            }
            case TYPE_SYMBOL: {
                CL_Symbol *s = (CL_Symbol *)ptr;
                CHECK_FIELD(s->name, "name");
                if (s->value != CL_UNBOUND) CHECK_FIELD(s->value, "value");
                if (s->function != CL_UNBOUND) CHECK_FIELD(s->function, "function");
                CHECK_FIELD(s->plist, "plist");
                CHECK_FIELD(s->package, "package");
                break;
            }
            case TYPE_CLOSURE: {
                CL_Closure *cl = (CL_Closure *)ptr;
                uint32_t n = (obj_size - sizeof(CL_Closure)) / sizeof(CL_Obj);
                uint32_t i;
                CHECK_FIELD(cl->bytecode, "bytecode");
                for (i = 0; i < n; i++)
                    CHECK_FIELD(cl->upvalues[i], "upval");
                break;
            }
            case TYPE_BYTECODE: {
                CL_Bytecode *bc = (CL_Bytecode *)ptr;
                CHECK_FIELD(bc->name, "name");
                CHECK_FIELD(bc->source_lambda_list, "lambda-list");
                break;
            }
            case TYPE_VECTOR: {
                CL_Vector *v = (CL_Vector *)ptr;
                uint32_t i;
                if (v->flags & CL_VEC_FLAG_DISPLACED) {
                    CHECK_FIELD(v->data[CL_DISP_BASE_IDX(v)], "displaced");
                } else {
                    uint32_t n = (v->rank > 1) ? (uint32_t)v->rank + v->length : v->length;
                    for (i = 0; i < n; i++)
                        CHECK_FIELD(v->data[i], "elt");
                }
                break;
            }
            case TYPE_HASHTABLE: {
                CL_Hashtable *ht = (CL_Hashtable *)ptr;
                uint32_t i;
                CHECK_FIELD(ht->bucket_vec, "bucket_vec");
                if (CL_NULL_P(ht->bucket_vec)) {
                    for (i = 0; i < ht->bucket_count; i++)
                        CHECK_FIELD(ht->buckets[i], "bucket");
                }
                break;
            }
            default:
                break;
            }
            #undef CHECK_FIELD

            if (errors > 10) {
                platform_write_string("POST-SWEEP: too many errors, stopping\n");
                return;
            }
        }
        ptr += obj_size;
    }

    if (errors > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "POST-SWEEP: %d use-after-free errors detected\n", errors);
        platform_write_string(buf);
    }
}
#endif

/* ======================================================================
 * Heap verification on demand -- (ext:%heap-verify)
 *
 * A diagnostic for the field, where a corrupted heap first shows itself
 * as an unrelated error ("not a function", a keyword that is no symbol)
 * long after the corruption happened, and where stderr is not seen
 * (AmigaOS, a script's `>log').  It collects (so that every remaining
 * non-free block is a live object whose every reference must be
 * sound), then walks the arena and checks each object's header and
 * each of its child references against a bitmap of real object starts,
 * plus every registered static root and every thread's protect stack.
 * A reference that lands past the bump front, unaligned, inside another
 * object, in a free block, or on a header with an impossible type is
 * reported with its holder.  Answers the fault count; the report text
 * (bounded) is left in a static buffer for cl_gc_verify_heap_report,
 * which the builtin turns into a Lisp string AFTER the walk (the walk
 * itself allocates nothing).
 * ====================================================================== */

#define HV_REPORT_CAP 3072
/* Kept back from the ordinary lines: the "...\n" cut mark (4 bytes) and the
 * closing summary (87 characters at most, plus the NUL). */
#define HV_TAIL_ROOM  96
static char hv_report[HV_REPORT_CAP];
static int  hv_report_len;
static int  hv_cut;                 /* the ordinary lines ran out of room */
static int  hv_faults;
static uint8_t *hv_starts;          /* bit per CL_ALIGN unit: an object starts here */
static uint8_t *hv_free;            /* bit per CL_ALIGN unit: a free block starts here */

/* Append one formatted line if all of it fits below cap; answers whether
 * it did (a line that does not fit is left out whole). */
static int hv_append(int cap, const char *fmt, va_list ap)
{
    int room = cap - hv_report_len;
    int n = vsnprintf(hv_report + hv_report_len, room, fmt, ap);
    if (n < 0 || n >= room) {
        hv_report[hv_report_len] = '\0';
        return 0;
    }
    hv_report_len += n;
    return 1;
}

/* An ordinary line.  They stop HV_TAIL_ROOM short of the buffer's end, with
 * a single "..." to say lines were left out. */
static void hv_say(const char *fmt, ...)
{
    va_list ap;
    int ok;
    if (hv_cut) return;
    va_start(ap, fmt);
    ok = hv_append(HV_REPORT_CAP - HV_TAIL_ROOM, fmt, ap);
    va_end(ap);
    if (!ok) {
        strcpy(hv_report + hv_report_len, "...\n");
        hv_report_len += 4;
        hv_cut = 1;
    }
}

/* The closing line: it is written however full the faults left the buffer,
 * because a heavily corrupted heap is exactly where its count is wanted. */
static void hv_summary(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    hv_append(HV_REPORT_CAP, fmt, ap);
    va_end(ap);
}

#define HV_BIT_SET(map, off)  ((map)[((off) / CL_ALIGN) >> 3] |= (uint8_t)(1u << (((off) / CL_ALIGN) & 7u)))
#define HV_BIT_GET(map, off)  (((map)[((off) / CL_ALIGN) >> 3] >> (((off) / CL_ALIGN) & 7u)) & 1u)

/* What is wrong with a reference, or NULL when nothing is. */
static const char *hv_ref_fault(CL_Obj ref)
{
    if (CL_NULL_P(ref) || CL_FIXNUM_P(ref) || CL_CHAR_P(ref)) return NULL;
    if (ref == CL_UNBOUND) return "the UNBOUND sentinel";
    if (!CL_HEAP_P(ref)) return "not a tagged object";
    /* limit - size, not ref + size: size_t is 32 bits on the Amiga, and a
     * raw word near 0xFFFFFFFF would wrap the sum past the check */
    if (ref > cl_heap.bump - sizeof(CL_Header)) return "past the bump front";
    if ((ref & (CL_ALIGN - 1)) != 0) return "unaligned";
    if (HV_BIT_GET(hv_free, ref)) return "a free block";
    if (!HV_BIT_GET(hv_starts, ref)) return "inside an object (no header there)";
    if (CL_HDR_TYPE(CL_OBJ_TO_PTR(ref)) > CL_TYPE_MAX) return "an impossible type tag";
    return NULL;
}

static void hv_check_ref(const char *holder, CL_Obj holder_obj, int idx, CL_Obj ref)
{
    const char *fault = hv_ref_fault(ref);
    char a[80], b[80];
    if (!fault) return;
    hv_faults++;
    if (holder_obj != CL_NIL)
        hv_say("%s %s slot %d -> 0x%08lx is %s (%s)\n", holder,
               cl_obj_brief(holder_obj, a, sizeof(a)), idx,
               (unsigned long)ref, fault, cl_obj_brief(ref, b, sizeof(b)));
    else
        hv_say("%s slot %d -> 0x%08lx is %s (%s)\n", holder, idx,
               (unsigned long)ref, fault, cl_obj_brief(ref, b, sizeof(b)));
}

int cl_gc_verify_heap(void)
{
    uint32_t off, units, bytes, walk_end, n_objs = 0, n_free = 0;
    int i;
    CL_Thread *t;

    hv_report_len = 0;
    hv_report[0] = '\0';
    hv_cut = 0;
    hv_faults = 0;

    /* Everything that is not free is live after this. */
    cl_gc();

    units = cl_heap.bump / CL_ALIGN + 1;
    bytes = (units + 7) / 8;
    hv_starts = (uint8_t *)platform_alloc(bytes);
    hv_free   = (uint8_t *)platform_alloc(bytes);
    if (!hv_starts || !hv_free) {
        if (hv_starts) platform_free(hv_starts);
        if (hv_free) platform_free(hv_free);
        hv_starts = hv_free = NULL;
        hv_say("heap-verify: no memory for the %lu-byte start bitmaps\n",
               (unsigned long)bytes);
        return -1;
    }
    memset(hv_starts, 0, bytes);
    memset(hv_free, 0, bytes);

    /* Pass 1: every block start; header sanity.  A header that breaks the
     * walk ends it there: walk_end is where the later walks stop too, since
     * past it there is no telling where a block starts. */
    walk_end = cl_heap.bump;
    for (off = CL_ALIGN; off < cl_heap.bump; ) {
        uint8_t *hp = cl_heap.arena + off;
        uint32_t size = CL_HDR_SIZE(hp);
        uint8_t type = CL_HDR_TYPE(hp);
        if (size == 0 || (size & (CL_ALIGN - 1)) != 0 || off + size > cl_heap.bump) {
            hv_faults++;
            hv_say("block @0x%08lx: header size %lu (type %u) breaks the arena walk "
                   "(bump 0x%08lx); walk stopped\n", (unsigned long)off,
                   (unsigned long)size, (unsigned)type, (unsigned long)cl_heap.bump);
            walk_end = off;
            break;
        }
        if (type > CL_TYPE_MAX) {
            hv_faults++;
            hv_say("block @0x%08lx: impossible type tag %u (size %lu)\n",
                   (unsigned long)off, (unsigned)type, (unsigned long)size);
        }
#ifdef CL_GENGC
        if (!gen_enabled && CL_HDR_MARKED(hp)) {   /* old space keeps its mark under GENGC */
#else
        if (CL_HDR_MARKED(hp)) {
#endif
            hv_faults++;
            hv_say("block @0x%08lx: mark bit still set after the collection (type %u)\n",
                   (unsigned long)off, (unsigned)type);
        }
        HV_BIT_SET(hv_starts, off);
        n_objs++;
        off += size;
    }
    /* The free list: those blocks hold no references. */
    for (off = cl_heap.free_list; off != 0; ) {
        CL_FreeBlock *fb;
        if (off >= cl_heap.bump || (off & (CL_ALIGN - 1)) != 0 || !HV_BIT_GET(hv_starts, off)) {
            hv_faults++;
            hv_say("free list: link 0x%08lx is not a block start; list walk stopped\n",
                   (unsigned long)off);
            break;
        }
        HV_BIT_SET(hv_free, off);
        n_free++;
        fb = (CL_FreeBlock *)(cl_heap.arena + off);
        off = fb->next_offset;
        if (n_free > n_objs) {
            hv_faults++;
            hv_say("free list: cycles\n");
            break;
        }
    }
    /* A bare size-only header (too small for a free-list link) holds no
     * object either: nothing live is smaller than a cons. */
    for (off = CL_ALIGN; off < walk_end; ) {
        uint8_t *hp = cl_heap.arena + off;
        uint32_t size = CL_HDR_SIZE(hp);
        if (size == 0 || off + size > walk_end) break;
        if (size < sizeof(CL_Cons) && !HV_BIT_GET(hv_free, off)) {
            HV_BIT_SET(hv_free, off);
            n_free++;
        }
        off += size;
    }

    /* Pass 2: every live object's children. */
    for (off = CL_ALIGN; off < walk_end; ) {
        uint8_t *hp = cl_heap.arena + off;
        uint32_t size = CL_HDR_SIZE(hp);
        uint8_t type = CL_HDR_TYPE(hp);
        CL_Obj self = (CL_Obj)off;
        int slot = 0;
        if (size == 0 || off + size > walk_end) break;
        if (HV_BIT_GET(hv_free, off) || type > CL_TYPE_MAX) { off += size; continue; }
        if (type == TYPE_BYTECODE) {
            CL_Bytecode *bc = (CL_Bytecode *)hp;
            if (!bc->code || bc->code_len == 0) {
                hv_faults++;
                hv_say("bytecode @0x%08lx: code %p len %lu\n", (unsigned long)off,
                       (void *)bc->code, (unsigned long)bc->code_len);
            }
            if (!bc->constants && bc->n_constants > 0) {
                hv_faults++;
                hv_say("bytecode @0x%08lx: %u constants but no pool\n",
                       (unsigned long)off, (unsigned)bc->n_constants);
                off += size; continue;
            }
        }
#define GC_VISIT(s) hv_check_ref(cl_type_name(self), self, slot++, (s))
#define GC_BYTECODE_TAIL(bc) ((void)(bc))
#define GC_STREAM_TAIL(st) ((void)(st))
        GC_WALK_OBJ_CHILDREN(hp, type);
#undef GC_VISIT
#undef GC_BYTECODE_TAIL
#undef GC_STREAM_TAIL
        off += size;
    }

    /* The roots outside the arena: registered statics, thread protect stacks. */
    for (i = 0; i < n_global_roots; i++) {
        const char *fault = hv_ref_fault(*global_roots[i]);
        if (fault) {
            char b[80];
            const char *site = "?";
            int line = 0;
#ifdef DEBUG_GC
            site = global_root_files[i] ? global_root_files[i] : "?";
            line = global_root_lines[i];
#endif
            hv_faults++;
            hv_say("static root #%d (%s:%d) -> 0x%08lx is %s (%s)\n", i, site, line,
                   (unsigned long)*global_roots[i], fault,
                   cl_obj_brief(*global_roots[i], b, sizeof(b)));
        }
    }
#undef gc_root_count
    for (t = cl_thread_list; t; t = t->next) {
        int j;
        for (j = 0; j < t->gc_root_count; j++) {
            const char *fault = hv_ref_fault(*t->gc_roots[j]);
            if (fault) {
                char b[80];
                hv_faults++;
                hv_say("thread %p protect #%d -> 0x%08lx is %s (%s)\n", (void *)t, j,
                       (unsigned long)*t->gc_roots[j], fault,
                       cl_obj_brief(*t->gc_roots[j], b, sizeof(b)));
            }
        }
        for (j = 0; j < t->vm.sp; j++) {
            const char *fault = hv_ref_fault(t->vm.stack[j]);
            if (fault) {
                char b[80];
                hv_faults++;
                hv_say("thread %p VM stack[%d] -> 0x%08lx is %s (%s)\n", (void *)t, j,
                       (unsigned long)t->vm.stack[j], fault,
                       cl_obj_brief(t->vm.stack[j], b, sizeof(b)));
            }
        }
    }

#define gc_root_count (CT->gc_root_count)
    hv_summary("heap-verify: %lu blocks (%lu free), bump 0x%08lx, %d fault(s)\n",
               (unsigned long)n_objs, (unsigned long)n_free,
               (unsigned long)cl_heap.bump, hv_faults);
    platform_free(hv_starts);
    platform_free(hv_free);
    hv_starts = hv_free = NULL;
    return hv_faults;
}

const char *cl_gc_verify_heap_report(void)
{
    return hv_report;
}
