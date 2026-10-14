#include "builtins.h"
#include "bignum.h"
#include "float.h"
#include "ratio.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "vm.h"
#include "string_utils.h"
#include "thread.h"   /* CL_MT, platform_mutex_* (via platform_thread.h) */
#include "../platform/platform.h"
#include <string.h>

/* --- Thread-safe (CL_HT_FLAG_SYNC) table serialization ---
 *
 * The CLOS dispatch caches (lib/clos.lisp) are hash tables that are read and
 * written from multiple threads: the main thread loading a system while
 * worker / watcher threads (sento, log4cl) dispatch the same generic
 * functions.  Every other hash table in the system is single-owner, so the
 * common path stays completely lock-free; only tables created with the
 * CL_HT_FLAG_SYNC flag pay for serialization, and even those only on the
 * cache-miss slow path (the monomorphic inline-cache fast path in clos.lisp
 * never touches the table).
 *
 * A single global mutex serializes access to all synchronized tables.  That
 * is coarse but correct and cheap where it matters: contention is confined to
 * concurrent slow-path GF dispatches, which are rare, and correctness beats
 * throughput here.  Critical sections are deliberately kept ALLOCATION-FREE —
 * all consing / bucket-vector growth happens BEFORE the lock is taken — so a
 * holder can never park at a GC safepoint while holding the lock (which would
 * otherwise stall a stop-the-world GC waiting on a peer blocked on the lock).
 * That is exactly why a plain, non-reentrant platform mutex is safe here:
 * short, non-blocking sections, no callback into Lisp, no reentry. */
static void *ht_sync_mutex = NULL;

/* Enter/leave the sync section for HT.  Returns whether the lock was taken so
 * the matching leave is unconditional (cl_thread_count could in principle drop
 * between enter and leave; the boolean keeps the pairing exact). */
static int ht_sync_enter(CL_Hashtable *ht)
{
    if ((ht->flags & CL_HT_FLAG_SYNC) && ht_sync_mutex && CL_MT()) {
        platform_mutex_lock(ht_sync_mutex);
        return 1;
    }
    return 0;
}

static void ht_sync_leave(int locked)
{
    if (locked) platform_mutex_unlock(ht_sync_mutex);
}

/* --- Case-insensitive string hash for equalp --- */

static uint32_t hash_string_ci(const char *str, uint32_t len)
{
    uint32_t hash = 0;
    uint32_t i;
    for (i = 0; i < len; i++) {
        char ch = str[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        hash = ((hash << 5) | (hash >> 27)) ^ (uint8_t)ch;
    }
    return hash;
}

/* Content hash for any Lisp string representation, per code point: wide
 * strings, and adjustable / fill-pointer character vectors (CL_STRING_VECTOR_P,
 * hashed over the active length).  Must agree with cl_hash_string /
 * hash_string_ci for ASCII content: keys_equal compares a wide "abc", a
 * fill-pointer "abc" and a narrow "abc" as EQUAL, so all must land in the same
 * bucket.  XORing the low byte of each code point achieves that (collisions
 * between distinct wide chars are fine — only equal-must-hash-equal matters). */
static uint32_t hash_string_content(CL_Obj s, int ci)
{
    uint32_t hash = 0;
    uint32_t i, len = cl_string_length(s);
    for (i = 0; i < len; i++) {
        int ch = cl_string_char_at(s, i);
        if (ci && ch >= 'A' && ch <= 'Z') ch += 32;
        hash = ((hash << 5) | (hash >> 27)) ^ (uint8_t)ch;
    }
    return hash;
}

/* Content hash for a bit-vector over its active length, used under EQUAL —
 * keys_equal compares bit-vectors elementwise under EQUAL (CLHS), so the hash
 * must be content-based too or equal keys land in different buckets.  Under
 * EQUALP, hash_obj instead takes the type-blind array-hash path (see
 * equalp_xarr_len/equalp_xarr_elt) so bit-vectors hash compatibly with
 * equal-content byte/general vectors. */
static uint32_t hash_bit_vector_content(CL_Obj obj)
{
    CL_BitVector *bv = (CL_BitVector *)CL_OBJ_TO_PTR(obj);
    uint32_t len = cl_bv_active_length(bv);
    uint32_t hash = len * 2654435761u;
    uint32_t i;
    for (i = 0; i < len; i++)
        hash = ((hash << 5) | (hash >> 27)) ^ (uint32_t)cl_bv_get_bit(bv, i);
    return hash;
}

/* --- Cross-type EQUALP array length/element accessors ---
 *
 * bi_equalp (builtins.c equalp_arr1d_*) treats general vectors, bit-vectors,
 * and byte-vectors of equal rank-1 content as EQUALP-equal regardless of
 * concrete type.  hash_obj/keys_equal must honor that same equivalence for
 * :test 'equalp hash tables, or a key of one concrete array type becomes
 * unfindable via an EQUALP-equal key of another (e.g. a byte-vector key vs.
 * an equal-content general-vector lookup). */
static uint32_t equalp_xarr_len(CL_Obj x)
{
    uint8_t type = CL_HDR_TYPE(CL_OBJ_TO_PTR(x));
    if (type == TYPE_BIT_VECTOR)
        return cl_bv_active_length((CL_BitVector *)CL_OBJ_TO_PTR(x));
    if (type == TYPE_BYTE_VECTOR)
        return cl_bytevec_active_length((CL_ByteVector *)CL_OBJ_TO_PTR(x));
    return cl_vector_active_length((CL_Vector *)CL_OBJ_TO_PTR(x));
}

static CL_Obj equalp_xarr_elt(CL_Obj x, uint32_t i)
{
    uint8_t type = CL_HDR_TYPE(CL_OBJ_TO_PTR(x));
    if (type == TYPE_BIT_VECTOR)
        return CL_MAKE_FIXNUM(cl_bv_get_bit((CL_BitVector *)CL_OBJ_TO_PTR(x), i));
    if (type == TYPE_BYTE_VECTOR)
        return CL_MAKE_FIXNUM(cl_bytevec_get((CL_ByteVector *)CL_OBJ_TO_PTR(x), i));
    return cl_vector_data((CL_Vector *)CL_OBJ_TO_PTR(x))[i];
}

static int is_equalp_xarr(CL_Obj x)
{
    return CL_VECTOR_P(x) || CL_BIT_VECTOR_P(x) || CL_BYTE_VECTOR_P(x);
}

/* --- Bit mixer for identity/fixnum hashing --- */

static inline uint32_t hash_mix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x45d9f3bU;
    h ^= h >> 16;
    return h;
}

/* --- Bucket accessor: returns pointer to bucket array --- */

static inline CL_Obj *ht_get_buckets(CL_Hashtable *ht)
{
    if (!CL_NULL_P(ht->bucket_vec)) {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(ht->bucket_vec);
        return v->data;
    }
    return ht->buckets;
}

/* --- Hash function for CL objects --- */

static uint32_t hash_obj(CL_Obj obj, uint32_t test)
{
    /* For eq: hash by identity */
    if (test == CL_HT_TEST_EQ)
        return hash_mix(obj);

    /* For eql: bignums, ratios, floats, and complex need value-based hash */
    if (test == CL_HT_TEST_EQL) {
        if (CL_BIGNUM_P(obj)) return cl_bignum_hash(obj);
        if (CL_RATIO_P(obj)) return cl_ratio_hash(obj);
        if (CL_COMPLEX_P(obj)) {
            CL_Complex *cx = (CL_Complex *)CL_OBJ_TO_PTR(obj);
            return hash_obj(cx->realpart, CL_HT_TEST_EQL) * 31 +
                   hash_obj(cx->imagpart, CL_HT_TEST_EQL);
        }
        if (CL_SINGLE_FLOAT_P(obj)) {
            union { float f; uint32_t u; } conv;
            conv.f = ((CL_SingleFloat *)CL_OBJ_TO_PTR(obj))->value;
            return conv.u * 2654435761u;
        }
        if (CL_DOUBLE_FLOAT_P(obj)) {
            union { double d; uint32_t u[2]; } conv;
            conv.d = ((CL_DoubleFloat *)CL_OBJ_TO_PTR(obj))->value;
            return (conv.u[0] ^ conv.u[1]) * 2654435761u;
        }
        return hash_mix(obj);
    }

    /* For equal/equalp: structural hash */
    if (CL_NULL_P(obj)) return 0;
    if (CL_FIXNUM_P(obj)) return hash_mix(obj);
    if (CL_CHAR_P(obj)) {
        if (test == CL_HT_TEST_EQUALP) {
            /* Case-insensitive char hash */
            char ch = (char)CL_CHAR_VAL(obj);
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            return (uint32_t)CL_MAKE_CHAR(ch);
        }
        return (uint32_t)obj;
    }
    if (CL_BIGNUM_P(obj)) return cl_bignum_hash(obj);
    if (CL_RATIO_P(obj)) return cl_ratio_hash(obj);
    if (CL_COMPLEX_P(obj)) {
        CL_Complex *cx = (CL_Complex *)CL_OBJ_TO_PTR(obj);
        return hash_obj(cx->realpart, test) * 31 +
               hash_obj(cx->imagpart, test);
    }
    if (CL_SINGLE_FLOAT_P(obj)) {
        union { float f; uint32_t u; } conv;
        conv.f = ((CL_SingleFloat *)CL_OBJ_TO_PTR(obj))->value;
        return conv.u * 2654435761u;
    }
    if (CL_DOUBLE_FLOAT_P(obj)) {
        union { double d; uint32_t u[2]; } conv;
        conv.d = ((CL_DoubleFloat *)CL_OBJ_TO_PTR(obj))->value;
        return (conv.u[0] ^ conv.u[1]) * 2654435761u;
    }

    if (CL_HEAP_P(obj)) {
        uint8_t type = CL_HDR_TYPE(CL_OBJ_TO_PTR(obj));
        if (type == TYPE_STRING) {
            CL_String *s = (CL_String *)CL_OBJ_TO_PTR(obj);
            if (test == CL_HT_TEST_EQUALP)
                return hash_string_ci(s->data, s->length);
            return cl_hash_string(s->data, s->length);
        }
#ifdef CL_WIDE_STRINGS
        if (type == TYPE_WIDE_STRING)
            /* keys_equal content-compares wide strings under EQUAL/EQUALP —
             * an identity hash made equal keys un-findable after the first
             * rehash/collision (AH5). */
            return hash_string_content(obj, test == CL_HT_TEST_EQUALP);
#endif
        if (CL_STRING_VECTOR_P(obj))
            /* Adjustable / fill-pointer character vector: a STRING per CLHS,
             * and keys_equal content-compares it against base/wide strings
             * under EQUAL/EQUALP (a JSON parser's VECTOR-PUSH-EXTEND key
             * buffer looked up with a literal), so it must hash like one —
             * and like base strings it hashes as a string under EQUALP too,
             * not via the type-blind vector path below, which would fold in
             * length and first element only and never match a literal key. */
            return hash_string_content(obj, test == CL_HT_TEST_EQUALP);
        if ((type == TYPE_VECTOR || type == TYPE_BIT_VECTOR ||
             type == TYPE_BYTE_VECTOR) && test == CL_HT_TEST_EQUALP) {
            /* EQUALP descends general/bit/byte vectors alike (keys_equal) —
             * fold in length and the first element only, mirroring the cons
             * rule below.  Deliberately type-blind so equal-content arrays of
             * different concrete types (e.g. a byte-vector and a general
             * vector) hash identically — see is_equalp_xarr/keys_equal. */
            uint32_t len = equalp_xarr_len(obj);
            uint32_t h = len * 31u + 2u;
            if (len > 0)
                h = h * 31u + hash_obj(equalp_xarr_elt(obj, 0), test);
            return h;
        }
        if (type == TYPE_BIT_VECTOR)
            return hash_bit_vector_content(obj);
        if (type == TYPE_SYMBOL)
            return hash_mix(obj);
        if (type == TYPE_CONS) {
            /* Hash first element only (avoid deep recursion) */
            return hash_obj(cl_car(obj), test) * 31 + 1;
        }
    }

    /* Fallback: identity hash */
    return hash_mix(obj);
}

/* Public wrapper so the GC (mem.c) can recompute a key's hash with the table's
 * own test after a compaction has relocated objects (identity-based hashes go
 * stale when arena offsets change). Must stay in sync with hash_obj. */
uint32_t cl_hashtable_hash_key(CL_Obj key, uint32_t test)
{
    return hash_obj(key, test);
}

/* --- Key comparison --- */

/* Forward declaration of bi_equal from builtins.c */
static int keys_equal(CL_Obj a, CL_Obj b, uint32_t test)
{
    if (test == CL_HT_TEST_EQ)
        return a == b;

    if (test == CL_HT_TEST_EQL) {
        if (a == b) return 1;
        /* Value equality for bignums */
        if (CL_BIGNUM_P(a) && CL_BIGNUM_P(b))
            return cl_bignum_equal(a, b);
        if (CL_RATIO_P(a) && CL_RATIO_P(b))
            return cl_ratio_equal(a, b);
        if (CL_COMPLEX_P(a) && CL_COMPLEX_P(b)) {
            CL_Complex *ca_cx = (CL_Complex *)CL_OBJ_TO_PTR(a);
            CL_Complex *cb_cx = (CL_Complex *)CL_OBJ_TO_PTR(b);
            return keys_equal(ca_cx->realpart, cb_cx->realpart, CL_HT_TEST_EQL) &&
                   keys_equal(ca_cx->imagpart, cb_cx->imagpart, CL_HT_TEST_EQL);
        }
        /* Value equality for floats (same type required for eql) */
        if (CL_SINGLE_FLOAT_P(a) && CL_SINGLE_FLOAT_P(b))
            return ((CL_SingleFloat *)CL_OBJ_TO_PTR(a))->value ==
                   ((CL_SingleFloat *)CL_OBJ_TO_PTR(b))->value;
        if (CL_DOUBLE_FLOAT_P(a) && CL_DOUBLE_FLOAT_P(b))
            return ((CL_DoubleFloat *)CL_OBJ_TO_PTR(a))->value ==
                   ((CL_DoubleFloat *)CL_OBJ_TO_PTR(b))->value;
        return 0;
    }

    /* CL_HT_TEST_EQUAL or CL_HT_TEST_EQUALP: structural equality */
    if (a == b) return 1;
    if (CL_CHAR_P(a) && CL_CHAR_P(b)) {
        if (test == CL_HT_TEST_EQUALP) {
            /* int, not (char): a (char) cast truncated wide code points to
             * their low byte, so distinct wide chars sharing a low byte
             * compared "equalp" (e.g. U+4E41 vs #\A). */
            int ca = CL_CHAR_VAL(a);
            int cb = CL_CHAR_VAL(b);
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            return ca == cb;
        }
        return a == b;
    }
    if (CL_FIXNUM_P(a) || CL_CHAR_P(a)) return a == b;
    if (CL_BIGNUM_P(a) && CL_BIGNUM_P(b))
        return cl_bignum_equal(a, b);
    if (CL_RATIO_P(a) && CL_RATIO_P(b))
        return cl_ratio_equal(a, b);
    if (CL_COMPLEX_P(a) && CL_COMPLEX_P(b)) {
        CL_Complex *ca_cx = (CL_Complex *)CL_OBJ_TO_PTR(a);
        CL_Complex *cb_cx = (CL_Complex *)CL_OBJ_TO_PTR(b);
        return keys_equal(ca_cx->realpart, cb_cx->realpart, test) &&
               keys_equal(ca_cx->imagpart, cb_cx->imagpart, test);
    }
    /* Floats: equal is same as eql (same type, same value) */
    if (CL_SINGLE_FLOAT_P(a) && CL_SINGLE_FLOAT_P(b))
        return ((CL_SingleFloat *)CL_OBJ_TO_PTR(a))->value ==
               ((CL_SingleFloat *)CL_OBJ_TO_PTR(b))->value;
    if (CL_DOUBLE_FLOAT_P(a) && CL_DOUBLE_FLOAT_P(b))
        return ((CL_DoubleFloat *)CL_OBJ_TO_PTR(a))->value ==
               ((CL_DoubleFloat *)CL_OBJ_TO_PTR(b))->value;

    /* Strings in every representation — base, wide, and adjustable /
     * fill-pointer character vectors — compare elementwise (CLHS EQUAL on
     * strings; EQUALP case-insensitively).  cl_string_length/char_at handle
     * all three, and hash_obj hashes all three by content. */
    if ((CL_ANY_STRING_P(a) || CL_STRING_VECTOR_P(a)) &&
        (CL_ANY_STRING_P(b) || CL_STRING_VECTOR_P(b))) {
        uint32_t la = cl_string_length(a), lb = cl_string_length(b);
        uint32_t i;
        if (la != lb) return 0;
        if (test == CL_HT_TEST_EQUALP) {
            for (i = 0; i < la; i++) {
                int ca = cl_string_char_at(a, i), cb = cl_string_char_at(b, i);
                if (ca >= 'A' && ca <= 'Z') ca += 32;
                if (cb >= 'A' && cb <= 'Z') cb += 32;
                if (ca != cb) return 0;
            }
            return 1;
        }
        for (i = 0; i < la; i++) {
            if (cl_string_char_at(a, i) != cl_string_char_at(b, i)) return 0;
        }
        return 1;
    }

    if (CL_CONS_P(a) && CL_CONS_P(b)) {
        return keys_equal(cl_car(a), cl_car(b), test) &&
               keys_equal(cl_cdr(a), cl_cdr(b), test);
    }

    /* Bit-vectors: EQUAL (and EQUALP) descend bit-vectors elementwise over
     * the active length (CLHS 5.3 EQUAL).  Previously fell through to the
     * final return 0, so equal-content bit-vector keys never matched. */
    if (CL_BIT_VECTOR_P(a) && CL_BIT_VECTOR_P(b)) {
        CL_BitVector *ba = (CL_BitVector *)CL_OBJ_TO_PTR(a);
        CL_BitVector *bb = (CL_BitVector *)CL_OBJ_TO_PTR(b);
        uint32_t la = cl_bv_active_length(ba);
        uint32_t lb = cl_bv_active_length(bb);
        uint32_t i;
        if (la != lb) return 0;
        for (i = 0; i < la; i++)
            if (cl_bv_get_bit(ba, i) != cl_bv_get_bit(bb, i)) return 0;
        return 1;
    }

    /* Byte-vectors: only EQUALP descends them — under EQUAL they are plain
     * arrays, compared EQ (handled by the identity check at the top). */
    if (test == CL_HT_TEST_EQUALP &&
        CL_BYTE_VECTOR_P(a) && CL_BYTE_VECTOR_P(b)) {
        CL_ByteVector *ba = (CL_ByteVector *)CL_OBJ_TO_PTR(a);
        CL_ByteVector *bb = (CL_ByteVector *)CL_OBJ_TO_PTR(b);
        uint32_t la = cl_bytevec_active_length(ba);
        uint32_t lb = cl_bytevec_active_length(bb);
        uint32_t i;
        if (la != lb) return 0;
        for (i = 0; i < la; i++)
            if (cl_bytevec_get(ba, i) != cl_bytevec_get(bb, i)) return 0;
        return 1;
    }

    /* General vectors: only EQUALP descends them — CLHS EQUAL on arrays
     * (other than strings and bit-vectors) is EQ, which the identity check
     * at the top already handled.  The old unconditional descent violated
     * the contract with hash_obj (content compare + identity hash → misses).
     * Uses active length/cl_vector_data (fill-pointer aware) to agree with
     * equalp_xarr_len/elt below and with bi_equalp's equalp_arr1d_* family. */
    if (test == CL_HT_TEST_EQUALP && CL_VECTOR_P(a) && CL_VECTOR_P(b)) {
        CL_Vector *va = (CL_Vector *)CL_OBJ_TO_PTR(a);
        CL_Vector *vb = (CL_Vector *)CL_OBJ_TO_PTR(b);
        uint32_t la = cl_vector_active_length(va);
        uint32_t lb = cl_vector_active_length(vb);
        uint32_t i;
        if (la != lb) return 0;
        for (i = 0; i < la; i++) {
            if (!keys_equal(cl_vector_data(va)[i], cl_vector_data(vb)[i], test)) return 0;
        }
        return 1;
    }

    /* Cross-type EQUALP: a general vector, bit-vector, and byte-vector of
     * equal (active) length and elementwise-EQUALP-equal content are
     * EQUALP-equal regardless of concrete type (CLHS, mirrored from
     * bi_equalp's equalp_arr1d_* family in builtins.c).  Only reached when a
     * and b are DIFFERENT concrete array types — same-type pairs already
     * matched one of the blocks above.  Must stay in sync with hash_obj's
     * matching type-blind EQUALP array-hash formula (equalp_xarr_len/elt) or
     * lookups by test key won't even reach the right bucket. */
    if (test == CL_HT_TEST_EQUALP && is_equalp_xarr(a) && is_equalp_xarr(b)) {
        uint32_t la = equalp_xarr_len(a), lb = equalp_xarr_len(b);
        uint32_t i;
        if (la != lb) return 0;
        for (i = 0; i < la; i++)
            if (!keys_equal(equalp_xarr_elt(a, i), equalp_xarr_elt(b, i), test)) return 0;
        return 1;
    }

    return 0;
}

/* Default bucket count */
#define CL_HT_DEFAULT_BUCKETS 16

/* --- Keyword symbols for :test and :size --- */
static CL_Obj KW_TEST = CL_NIL;
static CL_Obj KW_SIZE = CL_NIL;
static CL_Obj SYM_EQ_HT = CL_NIL;
static CL_Obj SYM_EQL_HT = CL_NIL;
static CL_Obj SYM_EQUAL_HT = CL_NIL;
static CL_Obj SYM_EQUALP_HT = CL_NIL;

/* Type-check O is a hash table and return its CL_Hashtable*; signals a
 * TYPE-ERROR tagged with WHO otherwise.  Collapses the guard/deref idiom
 * repeated across the hash-table builtins.  The returned pointer is only valid
 * until the next allocation (same as an inline CL_OBJ_TO_PTR), so callers that
 * allocate must re-derive it — exactly as before. */
static CL_Hashtable *require_hash_table(CL_Obj o, const char *who)
{
    if (!CL_HASHTABLE_P(o))
        cl_error(CL_ERR_TYPE, "%s: not a hash table", who);
    return (CL_Hashtable *)CL_OBJ_TO_PTR(o);
}

/* --- Rehashing --- */

/* Grow the hash table when load factor > 75%.
 * Allocates a new CL_Vector for buckets, redistributes entries by relinking
 * existing cons cells (no new allocations during redistribution). */
static void ht_maybe_rehash(CL_Obj ht_obj)
{
    CL_Hashtable *ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
    uint32_t old_count = ht->bucket_count;
    uint32_t new_count, i;
    CL_Obj new_vec;
    CL_Obj *old_bkts, *new_bkts;

    /* Check load factor: rehash if count > bucket_count * 3/4 */
    if (ht->count <= (old_count * 3) / 4)
        return;

    new_count = old_count * 2;
    if (new_count > CL_HT_MAX_BUCKETS)
        return;  /* at the cap: keep working with longer chains */

    /* Allocate new bucket vector — GC may fire */
    CL_GC_PROTECT(ht_obj);
    new_vec = cl_make_vector(new_count);
    CL_GC_UNPROTECT(1);

    if (CL_NULL_P(new_vec)) return;  /* allocation failed, skip rehash */

    /* Re-dereference after potential GC */
    ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
    old_bkts = ht_get_buckets(ht);
    new_bkts = ((CL_Vector *)CL_OBJ_TO_PTR(new_vec))->data;

    /* Redistribute all entries — no allocations, just relinking cons cells */
    for (i = 0; i < old_count; i++) {
        CL_Obj chain = old_bkts[i];
        while (!CL_NULL_P(chain)) {
            CL_Obj next = cl_cdr(chain);
            CL_Obj pair = cl_car(chain);
            CL_Obj key = cl_car(pair);
            uint32_t new_idx = hash_obj(key, ht->test) & (new_count - 1);
            /* Splice this chain cell into the new bucket */
            ((CL_Cons *)CL_OBJ_TO_PTR(chain))->cdr = new_bkts[new_idx];
            new_bkts[new_idx] = chain;
            chain = next;
        }
    }

    /* Switch to new bucket storage */
    ht->bucket_vec = new_vec;
    ht->bucket_count = new_count;
}

/* Relink every entry of HT into NEW_VEC (a bucket vector the caller already
 * allocated and sized to the doubled bucket count) and switch HT over to it.
 * This is the allocation-free tail of ht_maybe_rehash — split out so the SYNC
 * insert path can grow the table from inside its allocation-free critical
 * section using a vector pre-allocated before the lock was taken. */
static void ht_rehash_into(CL_Hashtable *ht, CL_Obj new_vec)
{
    uint32_t old_count = ht->bucket_count;
    uint32_t new_count = ((CL_Vector *)CL_OBJ_TO_PTR(new_vec))->length;
    CL_Obj *old_bkts = ht_get_buckets(ht);
    CL_Obj *new_bkts = ((CL_Vector *)CL_OBJ_TO_PTR(new_vec))->data;
    uint32_t i;

    for (i = 0; i < old_count; i++) {
        CL_Obj chain = old_bkts[i];
        while (!CL_NULL_P(chain)) {
            CL_Obj next = cl_cdr(chain);
            CL_Obj pair = cl_car(chain);
            CL_Obj key = cl_car(pair);
            uint32_t new_idx = hash_obj(key, ht->test) & (new_count - 1);
            ((CL_Cons *)CL_OBJ_TO_PTR(chain))->cdr = new_bkts[new_idx];
            new_bkts[new_idx] = chain;
            chain = next;
        }
    }

    ht->bucket_vec = new_vec;
    ht->bucket_count = new_count;
}

/* --- Builtins --- */

static CL_Obj bi_make_hash_table(CL_Obj *args, int n)
{
    uint32_t test = CL_HT_TEST_EQL;  /* Default test is EQL */
    uint32_t size = CL_HT_DEFAULT_BUCKETS;
    int i;

    /* Parse keyword arguments */
    for (i = 0; i + 1 < n; i += 2) {
        if (args[i] == KW_TEST) {
            CL_Obj test_fn = args[i + 1];
            /* Compare by symbol name to handle cross-package references */
            if (CL_SYMBOL_P(test_fn)) {
                const char *name = cl_symbol_name(test_fn);
                if (strcmp(name, "EQ") == 0)
                    test = CL_HT_TEST_EQ;
                else if (strcmp(name, "EQL") == 0)
                    test = CL_HT_TEST_EQL;
                else if (strcmp(name, "EQUAL") == 0)
                    test = CL_HT_TEST_EQUAL;
                else if (strcmp(name, "EQUALP") == 0)
                    test = CL_HT_TEST_EQUALP;
                else
                    cl_error(CL_ERR_ARGS, "MAKE-HASH-TABLE: :test must be EQ, EQL, EQUAL, or EQUALP, got %s", name);
            } else if (CL_HEAP_P(test_fn) && CL_HDR_TYPE(CL_OBJ_TO_PTR(test_fn)) == TYPE_FUNCTION) {
                /* #'eq, #'eql, #'equal, #'equalp — check function name */
                CL_Function *fn = (CL_Function *)CL_OBJ_TO_PTR(test_fn);
                if (CL_SYMBOL_P(fn->name)) {
                    const char *name = cl_symbol_name(fn->name);
                    if (strcmp(name, "EQ") == 0)
                        test = CL_HT_TEST_EQ;
                    else if (strcmp(name, "EQL") == 0)
                        test = CL_HT_TEST_EQL;
                    else if (strcmp(name, "EQUAL") == 0)
                        test = CL_HT_TEST_EQUAL;
                    else if (strcmp(name, "EQUALP") == 0)
                        test = CL_HT_TEST_EQUALP;
                    else
                        cl_error(CL_ERR_ARGS, "MAKE-HASH-TABLE: :test must be EQ, EQL, EQUAL, or EQUALP");
                } else {
                    cl_error(CL_ERR_ARGS, "MAKE-HASH-TABLE: :test must be EQ, EQL, EQUAL, or EQUALP");
                }
            } else {
                cl_error(CL_ERR_ARGS, "MAKE-HASH-TABLE: :test must be EQ, EQL, EQUAL, or EQUALP");
            }
        } else if (args[i] == KW_SIZE) {
            uint32_t requested;
            if (!CL_FIXNUM_P(args[i + 1]))
                cl_error(CL_ERR_TYPE, "MAKE-HASH-TABLE: :size must be a number");
            if (CL_FIXNUM_VAL(args[i + 1]) < 0)
                cl_error(CL_ERR_TYPE,
                         "MAKE-HASH-TABLE: :size must be non-negative (got %d)",
                         (int)CL_FIXNUM_VAL(args[i + 1]));
            requested = (uint32_t)CL_FIXNUM_VAL(args[i + 1]);
            if (requested < 1) requested = 1;
            /* Cap :size so the ceil(size/0.75) computation below and the
             * bucket allocation cannot overflow uint32_t.  A huge :size
             * (e.g. 1073741823) previously wrapped `requested * 4 + 2`
             * and cl_make_hashtable's power-of-2 round-up, producing a
             * tiny allocation whose bucket_count field claimed 2^31
             * buckets — every gethash/puthash then indexed far past the
             * object (wild heap reads/writes).  :size is a hint (CLHS),
             * so clamping is conformant; cl_make_hashtable enforces its
             * own hard cap as well. */
            if (requested > CL_HT_MAX_BUCKETS)
                requested = CL_HT_MAX_BUCKETS;
            /* :size is expected entry count; set bucket_count = ceil(size / 0.75)
             * so we don't immediately rehash after filling to :size entries */
            size = (requested * 4 + 2) / 3;
            if (size > CL_HT_MAX_BUCKETS)
                size = CL_HT_MAX_BUCKETS;
        }
    }

    return cl_make_hashtable(size, test);
}

static CL_Obj bi_gethash(CL_Obj *args, int n)
{
    CL_Obj key = args[0];
    CL_Obj ht_obj = args[1];
    CL_Obj default_val = (n >= 3) ? args[2] : CL_NIL;
    CL_Hashtable *ht;
    uint32_t bucket_idx;
    CL_Obj chain;

    ht = require_hash_table(ht_obj, "GETHASH");
    /* For a synchronized table hold the lock across the whole walk so a
     * concurrent rehash (which swaps bucket_vec and bucket_count and relinks
     * cells between buckets) can never be observed half-applied.  The walk is
     * allocation-free, so the lock is released promptly.  For plain tables this
     * is a no-op and the walk runs exactly as before. */
    {
        int locked = ht_sync_enter(ht);
        CL_Obj *bkts = ht_get_buckets(ht);
        bucket_idx = hash_obj(key, ht->test) & (ht->bucket_count - 1);
        chain = bkts[bucket_idx];

        while (!CL_NULL_P(chain)) {
            CL_Obj pair = cl_car(chain);
            if (keys_equal(cl_car(pair), key, ht->test)) {
                /* Found — return value, T as second value */
                CL_Obj val = cl_cdr(pair);
                ht_sync_leave(locked);
                cl_mv_count = 2;
                cl_mv_values[0] = val;
                cl_mv_values[1] = SYM_T;
                return val;
            }
            chain = cl_cdr(chain);
        }
        ht_sync_leave(locked);
    }

    /* Not found — return default, NIL as second value */
    cl_mv_count = 2;
    cl_mv_values[0] = default_val;
    cl_mv_values[1] = CL_NIL;
    return default_val;
}

/* EQUALP comparison of two hash tables (CLHS equalp semantics).
   Two hash tables are equalp iff they have the same test, the same number of
   entries, and for each key in A there is an entry in B (matched under the
   table's test) whose value satisfies VAL_EQ. VAL_EQ supplies the recursive
   equalp on values (see cl_equalp_pred in builtins.c). Equalp does no
   allocation, so no GC protection is needed here. Called from bi_equalp. */
int cl_hashtable_equalp(CL_Obj a_obj, CL_Obj b_obj,
                        int (*val_eq)(CL_Obj, CL_Obj))
{
    CL_Hashtable *a = (CL_Hashtable *)CL_OBJ_TO_PTR(a_obj);
    CL_Hashtable *b = (CL_Hashtable *)CL_OBJ_TO_PTR(b_obj);
    uint32_t i;

    if (a == b) return 1;
    if (a->test != b->test) return 0;
    if (a->count != b->count) return 0;

    /* GC SAFETY: keys_equal/val_eq can compact (EQUALP tests recurse through
     * bi_equalp, whose numeric compares allocate).  Both chain cursors are
     * protected roots, the table pointers are re-derived from the rooted
     * handles, and keys/values are re-read from the cursors at each use. */
    {
        CL_Obj chain = CL_NIL, bchain = CL_NIL;
        uint32_t bucket_count = a->bucket_count;
        CL_GC_PROTECT(a_obj);
        CL_GC_PROTECT(b_obj);
        CL_GC_PROTECT(chain);
        CL_GC_PROTECT(bchain);
        for (i = 0; i < bucket_count; i++) {
            chain = ht_get_buckets((CL_Hashtable *)CL_OBJ_TO_PTR(a_obj))[i];
            while (!CL_NULL_P(chain)) {
                int found = 0;
                CL_Hashtable *bh = (CL_Hashtable *)CL_OBJ_TO_PTR(b_obj);
                uint32_t bidx = hash_obj(cl_car(cl_car(chain)), bh->test)
                                & (bh->bucket_count - 1);
                bchain = ht_get_buckets(bh)[bidx];
                while (!CL_NULL_P(bchain)) {
                    if (keys_equal(cl_car(cl_car(bchain)), cl_car(cl_car(chain)),
                                   ((CL_Hashtable *)CL_OBJ_TO_PTR(b_obj))->test)) {
                        if (!val_eq(cl_cdr(cl_car(chain)), cl_cdr(cl_car(bchain)))) {
                            CL_GC_UNPROTECT(4);
                            return 0;
                        }
                        found = 1;
                        break;
                    }
                    bchain = cl_cdr(bchain);
                }
                if (!found) {
                    CL_GC_UNPROTECT(4);
                    return 0;
                }
                chain = cl_cdr(chain);
            }
        }
        CL_GC_UNPROTECT(4);
    }
    return 1;
}

/* (SETF GETHASH) for a CL_HT_FLAG_SYNC table.  Kept separate from the plain
 * path so the common single-owner case is untouched.  The strategy for a
 * lock-safe, allocation-free critical section:
 *   1. Allocate the (key . value) pair and its chain cell BEFORE the lock —
 *      allocation may compact (a GC safepoint), which must never happen while
 *      the sync mutex is held.
 *   2. Speculatively pre-allocate a doubled bucket vector (also before the
 *      lock) if this insert is likely to cross the 3/4 load factor, so the
 *      rehash relink under the lock needs no allocation.
 *   3. Under the lock: recompute the bucket from the current (possibly
 *      GC-forwarded) key offset, update in place if the key is present, else
 *      splice the pre-consed cell and, if now over the load factor and the
 *      pre-allocated vector still fits, relink into it. */
static CL_Obj bi_setf_gethash_sync(CL_Obj ht_obj, CL_Obj key, CL_Obj value)
{
    CL_Hashtable *ht;
    CL_Obj pair, new_chain, pre_vec = CL_NIL;
    uint32_t bucket_idx, prealloc_buckets;
    int locked;

    CL_GC_PROTECT(ht_obj);
    CL_GC_PROTECT(key);
    CL_GC_PROTECT(value);
    pair = cl_cons(key, value);
    CL_GC_PROTECT(pair);
    new_chain = cl_cons(pair, CL_NIL);
    CL_GC_PROTECT(new_chain);

    /* Predict a rehash and pre-allocate its bucket vector while we still may
     * allocate.  If the prediction is wrong (key already present, or a peer
     * already grew the table) the spare vector is simply dropped for GC. */
    ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
    prealloc_buckets = ht->bucket_count;
    if (ht->count + 1 > (prealloc_buckets * 3) / 4 &&
        prealloc_buckets * 2 <= CL_HT_MAX_BUCKETS)
        pre_vec = cl_make_vector(prealloc_buckets * 2);
    CL_GC_PROTECT(pre_vec);

    /* --- allocation-free critical section --- */
    ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
    locked = ht_sync_enter(ht);
    {
        CL_Obj *bkts = ht_get_buckets(ht);
        CL_Obj cursor;
        /* Key offset may have moved during the allocations above. */
        key = ((CL_Cons *)CL_OBJ_TO_PTR(pair))->car;
        bucket_idx = hash_obj(key, ht->test) & (ht->bucket_count - 1);

        /* Update existing entry in place. */
        cursor = bkts[bucket_idx];
        while (!CL_NULL_P(cursor)) {
            CL_Obj p = cl_car(cursor);
            if (keys_equal(cl_car(p), key, ht->test)) {
                ((CL_Cons *)CL_OBJ_TO_PTR(p))->cdr = value;
                ht_sync_leave(locked);
                CL_GC_UNPROTECT(6);
                return value;
            }
            cursor = cl_cdr(cursor);
        }

        /* New entry: prepend the pre-consed cell. */
        ((CL_Cons *)CL_OBJ_TO_PTR(new_chain))->cdr = bkts[bucket_idx];
        bkts[bucket_idx] = new_chain;
        ht->count++;

        /* Grow using the pre-allocated vector, only if it still matches the
         * table's current bucket count (a peer may have grown it while we were
         * allocating, in which case we skip — the next insert re-checks). */
        if (!CL_NULL_P(pre_vec) &&
            ht->count > (ht->bucket_count * 3) / 4 &&
            ((CL_Vector *)CL_OBJ_TO_PTR(pre_vec))->length == ht->bucket_count * 2) {
            ht_rehash_into(ht, pre_vec);
        }
    }
    ht_sync_leave(locked);
    CL_GC_UNPROTECT(6);
    return value;
}

static CL_Obj bi_setf_gethash(CL_Obj *args, int n)
{
    /* (%SETF-GETHASH key hash-table value) — set and return value */
    CL_Obj key = args[0];
    CL_Obj ht_obj = args[1];
    CL_Obj value = args[2];
    CL_Hashtable *ht;
    uint32_t bucket_idx;
    CL_Obj chain;
    CL_UNUSED(n);

    ht = require_hash_table(ht_obj, "(SETF GETHASH)");
    if ((ht->flags & CL_HT_FLAG_SYNC) && CL_MT())
        return bi_setf_gethash_sync(ht_obj, key, value);

    {
        CL_Obj *bkts = ht_get_buckets(ht);
        bucket_idx = hash_obj(key, ht->test) & (ht->bucket_count - 1);
        chain = bkts[bucket_idx];
    }

    /* Check if key already exists */
    {
        CL_Obj cursor = chain;
        while (!CL_NULL_P(cursor)) {
            CL_Obj pair = cl_car(cursor);
            if (keys_equal(cl_car(pair), key, ht->test)) {
                /* Update existing entry */
                ((CL_Cons *)CL_OBJ_TO_PTR(pair))->cdr = value;
                return value;
            }
            cursor = cl_cdr(cursor);
        }
    }

    /* New entry: cons (key . value) and prepend to bucket.
     *
     * Under a compacting GC the cl_cons() allocations below can trigger a
     * stop-the-world compaction that *rehashes this very EQ table*: eq hashing
     * is by object offset, which the compactor changes, so gc_rehash_eq_tables
     * reorganizes every eq table's buckets after a move.  That invalidates the
     * `bucket_idx` and `chain` head captured before the allocations — splicing
     * the stale `chain` (whose cells the rehash moved into *other* buckets)
     * onto a stale `bucket_idx` cross-links the chains, which later manifests
     * as wild out-of-bounds offsets when gc_rehash_eq_table walks them.
     *
     * Fix: allocate the cells FIRST, then — with NO allocation in between —
     * recompute the bucket index from the now-current key offset and splice
     * onto the live bucket head. */
    {
        CL_Obj pair;
        CL_Obj new_chain;
        CL_Obj *bkts;

        CL_GC_PROTECT(key);
        CL_GC_PROTECT(ht_obj);
        CL_GC_PROTECT(value);

        pair = cl_cons(key, value);
        CL_GC_PROTECT(pair);
        new_chain = cl_cons(pair, CL_NIL);
        CL_GC_UNPROTECT(4);

        /* No allocation past this point. */
        ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
        key = ((CL_Cons *)CL_OBJ_TO_PTR(pair))->car;  /* current (forwarded) offset */
        bucket_idx = hash_obj(key, ht->test) & (ht->bucket_count - 1);
        bkts = ht_get_buckets(ht);
        ((CL_Cons *)CL_OBJ_TO_PTR(new_chain))->cdr = bkts[bucket_idx];
        bkts[bucket_idx] = new_chain;
        ht->count++;
    }

    /* Rehash if load factor exceeded.  That allocates the larger bucket
     * vector, and a collection inside it moves `value`: keep it rooted so
     * the caller gets the forwarded offset, not the one the pair above was
     * built from.  (The classic gc-stress regime never showed this — a
     * value allocated moments earlier sits at the dense bump front and
     * does not move; the Immix regime's line-sweep cycles scatter fresh
     * objects into holes, and the next compaction moves them.  Found by
     * `make test-gc-stress GC=immix`, 2026-10-13, as a stale
     * mv_values[0] from (setf (find-class name) class).) */
    CL_GC_PROTECT(value);
    ht_maybe_rehash(ht_obj);
    CL_GC_UNPROTECT(1);

    return value;
}

static CL_Obj bi_remhash(CL_Obj *args, int n)
{
    CL_Obj key = args[0];
    CL_Obj ht_obj = args[1];
    CL_Hashtable *ht;
    uint32_t bucket_idx;
    CL_Obj prev, cursor;
    CL_UNUSED(n);

    ht = require_hash_table(ht_obj, "REMHASH");
    /* Allocation-free — safe to hold the sync lock across the whole unlink. */
    {
        int locked = ht_sync_enter(ht);
        CL_Obj *bkts = ht_get_buckets(ht);
        bucket_idx = hash_obj(key, ht->test) & (ht->bucket_count - 1);
        cursor = bkts[bucket_idx];
        prev = CL_NIL;

        while (!CL_NULL_P(cursor)) {
            CL_Obj pair = cl_car(cursor);
            if (keys_equal(cl_car(pair), key, ht->test)) {
                /* Remove from chain */
                if (CL_NULL_P(prev)) {
                    ht_get_buckets(ht)[bucket_idx] = cl_cdr(cursor);
                } else {
                    ((CL_Cons *)CL_OBJ_TO_PTR(prev))->cdr = cl_cdr(cursor);
                }
                ht->count--;
                ht_sync_leave(locked);
                return SYM_T;
            }
            prev = cursor;
            cursor = cl_cdr(cursor);
        }
        ht_sync_leave(locked);
    }

    return CL_NIL;
}

/* NOTE: does not take ht_sync_enter/leave for CL_HT_FLAG_SYNC tables — the
 * bucket walk below calls arbitrary user Lisp per entry via cl_vm_apply,
 * which can allocate and park at a GC safepoint (or re-enter this same
 * table), so holding ht_sync_mutex across it would violate the
 * allocation-free critical-section invariant documented at the top of this
 * file and risks deadlock. See the CL_HT_FLAG_SYNC comment in types.h. */
static CL_Obj bi_maphash(CL_Obj *args, int n)
{
    CL_Obj func = cl_coerce_funcdesig(args[0], "MAPHASH");
    CL_Obj ht_obj = args[1];
    CL_Hashtable *ht;
    uint32_t i;
    CL_UNUSED(n);

    ht = require_hash_table(ht_obj, "MAPHASH");

    CL_GC_PROTECT(func);
    CL_GC_PROTECT(ht_obj);

    {
        /* GC SAFETY: the chain cursor is re-read (cl_cdr) after cl_vm_apply
         * runs arbitrary user code that can compact — without a root the
         * cursor holds a stale pre-move offset. */
        CL_Obj chain = CL_NIL;
        CL_GC_PROTECT(chain);
        for (i = 0; i < ht->bucket_count; i++) {
            chain = ht_get_buckets(ht)[i];
            while (!CL_NULL_P(chain)) {
                CL_Obj pair = cl_car(chain);
                CL_Obj call_args[2];
                call_args[0] = cl_car(pair);
                call_args[1] = cl_cdr(pair);

                if (CL_FUNCTION_OBJ_P(func)) {
                    /* cl_vm_apply GC-roots call_args across the call (the function
                     * may compact while reading its key/value args). */
                    cl_vm_apply(func, call_args, 2);
                } else {
                    cl_error(CL_ERR_TYPE, "MAPHASH: not a function");
                }

                /* Re-dereference after potential GC from function call */
                ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
                chain = cl_cdr(chain);
            }
        }
        CL_GC_UNPROTECT(1);
    }

    CL_GC_UNPROTECT(2);
    return CL_NIL;
}

static CL_Obj bi_clrhash(CL_Obj *args, int n)
{
    CL_Hashtable *ht;
    uint32_t i;
    CL_UNUSED(n);

    ht = require_hash_table(args[0], "CLRHASH");
    {
        int locked = ht_sync_enter(ht);
        CL_Obj *bkts = ht_get_buckets(ht);
        for (i = 0; i < ht->bucket_count; i++)
            bkts[i] = CL_NIL;
        ht->count = 0;
        ht_sync_leave(locked);
    }
    return args[0];
}

static CL_Obj bi_hash_table_count(CL_Obj *args, int n)
{
    CL_Hashtable *ht;
    CL_UNUSED(n);

    ht = require_hash_table(args[0], "HASH-TABLE-COUNT");
    return CL_MAKE_FIXNUM(ht->count);
}

DEFINE_TYPE_PREDICATE(bi_hash_table_p, CL_HASHTABLE_P)

/* CLHS: HASH-TABLE-SIZE — the "current size", i.e. the bucket capacity
   that the table was allocated with.  We return bucket_count, which is
   what we internally track. */
static CL_Obj bi_hash_table_size(CL_Obj *args, int n)
{
    CL_Hashtable *ht;
    CL_UNUSED(n);
    ht = require_hash_table(args[0], "HASH-TABLE-SIZE");
    return CL_MAKE_FIXNUM((int32_t)ht->bucket_count);
}

/* CLHS: HASH-TABLE-TEST — returns the test as a symbol for the four
   standard tests (eq/eql/equal/equalp). */
static CL_Obj bi_hash_table_test(CL_Obj *args, int n)
{
    CL_Hashtable *ht;
    CL_UNUSED(n);
    ht = require_hash_table(args[0], "HASH-TABLE-TEST");
    switch (ht->test) {
    case CL_HT_TEST_EQ:     return SYM_EQ_HT;
    case CL_HT_TEST_EQL:    return SYM_EQL_HT;
    case CL_HT_TEST_EQUAL:  return SYM_EQUAL_HT;
    case CL_HT_TEST_EQUALP: return SYM_EQUALP_HT;
    default:                return SYM_EQL_HT;
    }
}

/* CLHS: HASH-TABLE-REHASH-SIZE — positive number, how the table grows.
   We don't store this per-table; return a reasonable default (1.5). */
static CL_Obj bi_hash_table_rehash_size(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    (void)require_hash_table(args[0], "HASH-TABLE-REHASH-SIZE");
    return cl_make_single_float(1.5f);
}

/* CLHS: HASH-TABLE-REHASH-THRESHOLD — load factor, 0 < x <= 1.
   We don't store this per-table; return 0.75 which matches the load
   factor used internally by maybe_rehash. */
static CL_Obj bi_hash_table_rehash_threshold(CL_Obj *args, int n)
{
    CL_UNUSED(n);
    (void)require_hash_table(args[0], "HASH-TABLE-REHASH-THRESHOLD");
    return cl_make_single_float(0.75f);
}

/* NOTE: does not take ht_sync_enter/leave for CL_HT_FLAG_SYNC tables — the
 * bucket walk below builds the result list with cl_cons(), an allocating
 * call that can trigger a GC safepoint, so holding ht_sync_mutex across it
 * would violate the allocation-free critical-section invariant documented
 * at the top of this file. See the CL_HT_FLAG_SYNC comment in types.h. */
static CL_Obj bi_hash_table_pairs(CL_Obj *args, int n)
{
    CL_Obj ht_obj = args[0];
    CL_Hashtable *ht;
    CL_Obj result = CL_NIL;
    CL_Obj chain = CL_NIL;
    uint32_t i;
    CL_UNUSED(n);

    (void)require_hash_table(ht_obj, "%HASH-TABLE-PAIRS");

    CL_GC_PROTECT(ht_obj);
    CL_GC_PROTECT(result);
    /* The chain cursor is walked across the allocating cl_cons below —
     * without a root the compaction leaves it a stale offset and the next
     * cl_cdr walks garbage (mirrors the bi_maphash fix).  This backs
     * with-hash-table-iterator and LOOP hash iteration. */
    CL_GC_PROTECT(chain);

    ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
    for (i = 0; i < ht->bucket_count; i++) {
        chain = ht_get_buckets(ht)[i];
        while (!CL_NULL_P(chain)) {
            CL_Obj pair = cl_car(chain);
            result = cl_cons(pair, result);
            /* Re-dereference after potential GC from cl_cons */
            ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
            chain = cl_cdr(chain);
        }
    }

    CL_GC_UNPROTECT(3);
    return result;
}

/* AMILISP::%MAKE-SYNC-HASH-TABLE test-symbol — create a thread-safe hash table
 * (CL_HT_FLAG_SYNC).  Used internally by the CLOS dispatch caches, which are
 * mutated concurrently by peer threads.  TEST is one of the symbols
 * EQ/EQL/EQUAL/EQUALP. */
static CL_Obj bi_make_sync_hash_table(CL_Obj *args, int n)
{
    uint32_t test = CL_HT_TEST_EQ;
    CL_Obj ht_obj;
    CL_Hashtable *ht;
    CL_UNUSED(n);

    if (!CL_SYMBOL_P(args[0]))
        cl_error(CL_ERR_ARGS, "%%MAKE-SYNC-HASH-TABLE: test must be a symbol");
    {
        const char *name = cl_symbol_name(args[0]);
        if (strcmp(name, "EQ") == 0)          test = CL_HT_TEST_EQ;
        else if (strcmp(name, "EQL") == 0)    test = CL_HT_TEST_EQL;
        else if (strcmp(name, "EQUAL") == 0)  test = CL_HT_TEST_EQUAL;
        else if (strcmp(name, "EQUALP") == 0) test = CL_HT_TEST_EQUALP;
        else cl_error(CL_ERR_ARGS,
                      "%%MAKE-SYNC-HASH-TABLE: test must be EQ, EQL, EQUAL, or EQUALP");
    }

    ht_obj = cl_make_hashtable(CL_HT_DEFAULT_BUCKETS, test);
    if (CL_NULL_P(ht_obj)) return CL_NIL;
    ht = (CL_Hashtable *)CL_OBJ_TO_PTR(ht_obj);
    ht->flags |= CL_HT_FLAG_SYNC;
    return ht_obj;
}

/* --- sxhash --- */

static CL_Obj bi_sxhash(CL_Obj *args, int n)
{
    (void)n;
    uint32_t h = hash_obj(args[0], CL_HT_TEST_EQUAL);
    /* Return as non-negative fixnum (mask to 30 bits) */
    return CL_MAKE_FIXNUM((int32_t)(h & 0x3FFFFFFFu));
}

/* --- Registration --- */

void cl_builtins_hashtable_init(void)
{
    /* Intern keyword/symbols used by make-hash-table */
    KW_TEST     = cl_intern_keyword("TEST", 4);
    KW_SIZE     = cl_intern_keyword("SIZE", 4);
    SYM_EQ_HT   = cl_intern_in("EQ", 2, cl_package_cl);
    SYM_EQL_HT  = cl_intern_in("EQL", 3, cl_package_cl);
    SYM_EQUAL_HT = cl_intern_in("EQUAL", 5, cl_package_cl);
    SYM_EQUALP_HT = cl_intern_in("EQUALP", 6, cl_package_cl);

    defun("MAKE-HASH-TABLE", bi_make_hash_table, 0, -1);
    defun("GETHASH", bi_gethash, 2, 3);
    defun("REMHASH", bi_remhash, 2, 2);
    defun("MAPHASH", bi_maphash, 2, 2);
    defun("CLRHASH", bi_clrhash, 1, 1);
    defun("HASH-TABLE-COUNT", bi_hash_table_count, 1, 1);
    defun("HASH-TABLE-P", bi_hash_table_p, 1, 1);
    defun("HASH-TABLE-SIZE", bi_hash_table_size, 1, 1);
    defun("HASH-TABLE-TEST", bi_hash_table_test, 1, 1);
    defun("HASH-TABLE-REHASH-SIZE", bi_hash_table_rehash_size, 1, 1);
    defun("HASH-TABLE-REHASH-THRESHOLD", bi_hash_table_rehash_threshold, 1, 1);
    cl_register_builtin("%SETF-GETHASH", bi_setf_gethash, 3, 3, cl_package_amilisp);
    cl_register_builtin("%HASH-TABLE-PAIRS", bi_hash_table_pairs, 1, 1, cl_package_amilisp);
    cl_register_builtin("%MAKE-SYNC-HASH-TABLE", bi_make_sync_hash_table, 1, 1, cl_package_amilisp);
    defun("SXHASH", bi_sxhash, 1, 1);

    /* Global mutex serializing access to CL_HT_FLAG_SYNC (CLOS dispatch cache)
     * tables.  Initialized once at startup; never destroyed. */
    if (!ht_sync_mutex)
        platform_mutex_init(&ht_sync_mutex);

    /* Register cached symbols for GC compaction forwarding */
    cl_gc_register_root(&KW_TEST);
    cl_gc_register_root(&KW_SIZE);
    cl_gc_register_root(&SYM_EQ_HT);
    cl_gc_register_root(&SYM_EQL_HT);
    cl_gc_register_root(&SYM_EQUAL_HT);
    cl_gc_register_root(&SYM_EQUALP_HT);
}
