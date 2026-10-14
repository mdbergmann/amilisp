/*
 * bindtab.c — demand-interned binding tables (see bindtab.h for the
 * design and the blob layout; specs/raw-bindings-footprint.md, Phase 2).
 *
 * Four pieces:
 *   - the PACKER (cl_bindtab_pack): DEFINE-BINDING-TABLE rows -> blob,
 *     run at macroexpansion time on the host, so a compiled module's
 *     FASL carries one byte-vector literal instead of ~2000 definitions;
 *   - the VALIDATOR + REGISTRAR (cl_bindtab_register): attaches a blob to
 *     a package and fills the symbols already present in it;
 *   - the PROBE (cl_bindtab_probe_nolock): binary search by name, guard
 *     evaluation — what package.c's lookup funnel calls on a miss;
 *   - the MATERIALISER (cl_bindtab_materialize / _all): builds a symbol
 *     from an entry, following package.c's lock discipline to the letter:
 *     every allocation outside cl_package_rwlock, plain stores under the
 *     write lock, re-check for a racing peer.
 *
 * GC: the blob is a heap byte vector and MOVES under compaction.  Every
 * routine that allocates copies what it needs out of the blob into C
 * memory first and re-derives the blob pointer from the (protected)
 * package afterwards; the probe and the validator never allocate.
 */

#include "bindtab.h"
#include "package.h"
#include "symbol.h"
#include "mem.h"
#include "error.h"
#include "builtins.h"
#include "bignum.h"
#include "compiler.h"
#include "thread.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>

/* ================================================================
 * Byte helpers (big-endian) and blob views
 * ================================================================ */

static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}

/* A decoded entry — the C-side mirror of one 16-byte record. */
typedef struct {
    uint32_t name_off;
    uint16_t name_len;
    uint8_t  kind, flags;
    uint32_t a;
    int16_t  b;
    uint8_t  c, d;
} BtEntry;

/* A read-only view of a blob: raw pointers, valid only while no
 * allocation happens (the blob moves under compaction). */
typedef struct {
    const uint8_t *base;     /* start of the byte vector's data */
    uint32_t       len;      /* total bytes */
    uint32_t       n;        /* entries */
    uint32_t       names_len;
    const uint8_t *entries;  /* base + CL_BT_HEADER_SIZE */
    const uint8_t *names;    /* entries + n * CL_BT_ENTRY_SIZE */
} BtView;

static void view_blob(CL_Obj blob, BtView *v)
{
    CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(blob);
    v->base = bv->data;
    v->len = bv->length;
    v->n = rd32(v->base + 8);
    v->names_len = rd32(v->base + 12);
    v->entries = v->base + CL_BT_HEADER_SIZE;
    v->names = v->entries + (uint32_t)v->n * CL_BT_ENTRY_SIZE;
}

static void decode_entry(const BtView *v, uint32_t i, BtEntry *e)
{
    const uint8_t *p = v->entries + (uint32_t)i * CL_BT_ENTRY_SIZE;
    e->name_off = rd32(p);
    e->name_len = rd16(p + 4);
    e->kind = p[6];
    e->flags = p[7];
    e->a = rd32(p + 8);
    e->b = (int16_t)rd16(p + 12);
    e->c = p[14];
    e->d = p[15];
}

static const uint8_t *entry_name(const BtView *v, const BtEntry *e)
{
    return v->names + e->name_off;
}

/* Total order on names: bytewise, shorter-is-less on a common prefix.
 * The packer sorts by it; the probe binary-searches by it. */
static int name_cmp(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen)
{
    uint32_t n = alen < blen ? alen : blen;
    int r = memcmp(a, b, n);
    if (r != 0) return r;
    return (alen < blen) ? -1 : (alen > blen) ? 1 : 0;
}

/* The value of a CONST/VAR entry, copied out of the blob into C memory
 * (the four encodings: u32, i32, a wide magnitude in the arena as u8
 * nbytes, u8 sign, big-endian bytes — any integer the NDK has, e.g. the
 * 13-byte packed-string constants of datatypes.i — or a string in the
 * arena as u8 len, ASCII bytes — mui.h's MUIC_Window "Window.mui"). */
typedef struct {
    int      wide;
    int      string;     /* CL_BT_F_STRING: mag[0..nbytes) are the characters */
    uint32_t a;          /* u32 / i32 payload when !wide && !string */
    int      is_signed;  /* CL_BT_F_SIGNED */
    uint8_t  neg, nbytes;
    uint8_t  mag[255];   /* big-endian magnitude when wide; the bytes when string */
} BtValue;

static void decode_value(const BtView *v, const BtEntry *e, BtValue *val)
{
    memset(val, 0, sizeof(*val));
    val->a = e->a;
    val->is_signed = (e->flags & CL_BT_F_SIGNED) != 0;
    if (e->flags & CL_BT_F_WIDE) {
        const uint8_t *p = v->names + e->a;
        val->wide = 1;
        val->nbytes = p[0];
        val->neg = p[1];
        memcpy(val->mag, p + 2, val->nbytes);
    } else if (e->flags & CL_BT_F_STRING) {
        const uint8_t *p = v->names + e->a;
        val->string = 1;
        val->nbytes = p[0];
        memcpy(val->mag, p + 1, val->nbytes);
    }
}

/* Box a decoded value: fixnum, bignum or a fresh simple string.  Allocates. */
static CL_Obj value_to_obj(const BtValue *val)
{
    if (val->string)
        return cl_make_string((const char *)val->mag, val->nbytes);
    if (!val->wide)
        return val->is_signed ? cl_ffi_i64_to_obj((int64_t)(int32_t)val->a)
                              : cl_ffi_u64_to_obj((uint64_t)val->a);
    {
        uint32_t n_limbs = ((uint32_t)val->nbytes + 1) / 2, i;
        CL_Obj bn = cl_make_bignum(n_limbs, val->neg ? 1 : 0);
        CL_Bignum *b = (CL_Bignum *)CL_OBJ_TO_PTR(bn);
        for (i = 0; i < n_limbs; i++) {
            /* limb i = bytes [nbytes-2i-2, nbytes-2i-1] (big-endian source) */
            int hi = (int)val->nbytes - 2 * (int)i - 2;
            int lo = (int)val->nbytes - 2 * (int)i - 1;
            uint16_t limb = (uint16_t)val->mag[lo];
            if (hi >= 0) limb |= (uint16_t)val->mag[hi] << 8;
            b->limbs[i] = limb;
        }
        return cl_bignum_normalize(bn);
    }
}

/* The package's bindings vector, or CL_NIL. */
static CL_Obj pkg_bindings(CL_Obj package)
{
    return ((CL_Package *)CL_OBJ_TO_PTR(package))->bindings;
}
static CL_Obj bindings_slot(CL_Obj vec, int slot)
{
    return ((CL_Vector *)CL_OBJ_TO_PTR(vec))->data[slot];
}

/* The package's blob, or CL_NIL when it has no table OR the table was SHED
 * (cl_bindtab_shed: the vector stays as the "was lazy" marker, its blob
 * slot is NIL).  Every reader of the blob goes through this — a shed table
 * must read as a guaranteed miss, never as a NIL byte vector. */
static CL_Obj pkg_blob(CL_Obj package)
{
    CL_Obj vec = pkg_bindings(package);
    if (CL_NULL_P(vec)) return CL_NIL;
    return bindings_slot(vec, CL_BT_SLOT_BLOB);
}

/* ================================================================
 * Keyword helpers (by name — no pre-interned roots needed)
 * ================================================================ */

static int kw_is(CL_Obj obj, const char *name)
{
    return CL_SYMBOL_P(obj) &&
           ((CL_Symbol *)CL_OBJ_TO_PTR(obj))->package == cl_package_keyword &&
           strcmp(cl_symbol_name(obj), name) == 0;
}

static const char *const reg_names[13] = {
    "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1", "A2", "A3", "A4"
};

/* :D0..:D7 / :A0..:A4 -> 0..12; -1 otherwise (A5 is the dispatcher's
 * scratch register, A6 the library base: neither can carry an argument). */
static int reg_index_of(CL_Obj kw)
{
    int i;
    if (!CL_SYMBOL_P(kw) ||
        ((CL_Symbol *)CL_OBJ_TO_PTR(kw))->package != cl_package_keyword)
        return -1;
    for (i = 0; i < 13; i++)
        if (strcmp(cl_symbol_name(kw), reg_names[i]) == 0) return i;
    return -1;
}

static CL_Obj keyword(const char *name)
{
    return cl_intern_keyword(name, (uint32_t)strlen(name));
}

/* Is :MORPHOS on *FEATURES*?  Non-allocating list walk. */
static int morphos_feature_p(void)
{
    CL_Obj feats;
    if (!CL_SYMBOL_P(SYM_STAR_FEATURES)) return 0;
    feats = cl_symbol_value(SYM_STAR_FEATURES);
    while (CL_CONS_P(feats)) {
        if (kw_is(cl_car(feats), "MORPHOS")) return 1;
        feats = cl_cdr(feats);
    }
    return 0;
}

/* ================================================================
 * Validation (shared by the registrar; the probe trusts a registered blob)
 * ================================================================ */

/* Returns NULL when BLOB is a well-formed table, else a static message. */
static const char *validate_blob(CL_Obj blob)
{
    BtView v;
    uint32_t i;
    CL_ByteVector *bv;
    BtEntry prev, e;

    if (!CL_BYTE_VECTOR_P(blob)) return "not a byte vector";
    bv = (CL_ByteVector *)CL_OBJ_TO_PTR(blob);
    if (bv->elt_shift != 0) return "not an (unsigned-byte 8) vector";
    if (bv->length < CL_BT_HEADER_SIZE) return "shorter than the header";
    if (rd32(bv->data) != CL_BT_MAGIC) return "bad magic (not a binding table)";
    if (bv->data[4] != CL_BT_VERSION) return "unsupported table version";
    view_blob(blob, &v);
    if (v.n > 0x00FFFFFFu) return "entry count out of range";
    if ((uint64_t)CL_BT_HEADER_SIZE + (uint64_t)v.n * CL_BT_ENTRY_SIZE +
        v.names_len != v.len)
        return "length does not match entry count + name arena";
    memset(&prev, 0, sizeof(prev));
    for (i = 0; i < v.n; i++) {
        uint32_t k;
        decode_entry(&v, i, &e);
        if (e.name_len == 0) return "empty name";
        if ((uint64_t)e.name_off + e.name_len > v.names_len) return "name outside the arena";
        for (k = 0; k < e.name_len; k++)
            if (entry_name(&v, &e)[k] >= 0x80) return "non-ASCII name";
        if (e.kind > CL_BT_KIND_MAX) return "unknown entry kind";
        if (e.flags & ~CL_BT_F_KNOWN) return "unknown entry flag";
        switch (e.kind) {
        case CL_BT_CONST: case CL_BT_VAR:
            if ((e.flags & CL_BT_F_WIDE) && (e.flags & CL_BT_F_STRING))
                return "value flagged both wide integer and string";
            if (e.flags & CL_BT_F_WIDE) {
                if ((uint64_t)e.a + 2 > v.names_len) return "wide value outside the arena";
                if (v.names[e.a] == 0 ||
                    (uint64_t)e.a + 2 + v.names[e.a] > v.names_len)
                    return "wide value outside the arena";
            }
            if (e.flags & CL_BT_F_STRING) {
                if ((uint64_t)e.a + 1 > v.names_len ||
                    (uint64_t)e.a + 1 + v.names[e.a] > v.names_len)
                    return "string value outside the arena";
                for (k = 0; k < v.names[e.a]; k++)
                    if (v.names[e.a + 1 + k] >= 0x80) return "non-ASCII string value";
            }
            break;
        case CL_BT_LIBCALL:
            if (e.c > 7) return "library call with more than 7 arguments";
            if (CL_AMIGA_RES_KIND(e.a) > CL_AMIGA_RES_KIND_MAX) return "bad result kind";
            for (k = 0; k < e.c; k++)
                if (((e.a >> (4 * k)) & 0xF) > 12) return "bad register in library call";
            if (e.b >= 0) return "library call LVO must be negative";
            break;
        case CL_BT_FIELD:
            if (e.d != CL_STUB_PEEK && e.d != CL_STUB_PEEK_IDX && e.d != CL_STUB_FIELD_PTR)
                return "bad field accessor kind";
            if (e.d != CL_STUB_FIELD_PTR && e.c > CL_STUB_CT_MAX) return "bad field C type";
            if (e.d == CL_STUB_PEEK_IDX &&
                e.b != 1 && e.b != 2 && e.b != 4 && e.b != 8)
                return "bad array element size";
            break;
        default:
            break;
        }
        if (i > 0) {
            int c = name_cmp(entry_name(&v, &prev), prev.name_len,
                             entry_name(&v, &e), e.name_len);
            if (c > 0) return "entries not sorted by name";
            if (c == 0 && !(prev.kind == CL_BT_LIBCALL && e.kind == CL_BT_LIBCALL))
                return "duplicate name (only library-call variants may repeat)";
        }
        prev = e;
    }
    return NULL;
}

/* ================================================================
 * Packer
 * ================================================================ */

typedef struct {
    uint8_t *name;      /* into BtBuild.names */
    uint32_t name_len;
    uint32_t order;     /* source position — sort tiebreaker (stability) */
    uint8_t  kind, flags, c, d;
    uint32_t a;
    int16_t  b;
    int      has_wide;
    uint32_t wide_off;  /* into BtBuild.wide: u8 nbytes, u8 sign, magnitude */
} BtRec;

typedef struct {
    BtRec   *recs;
    uint32_t n, cap;
    uint8_t *names;     /* concatenated name bytes */
    uint32_t names_len, names_cap;
    uint8_t *wide;      /* wide value payloads */
    uint32_t wide_len, wide_cap;
    uint32_t has_libcall;
    int      row;       /* 1-based row being parsed, for messages */
} BtBuild;

static void build_free(BtBuild *b)
{
    if (b->recs) platform_free(b->recs);
    if (b->names) platform_free(b->names);
    if (b->wide) platform_free(b->wide);
    b->recs = NULL; b->names = NULL; b->wide = NULL;
}

/* Format the message FIRST (it may name a record whose bytes live in the
 * buffers), then free the C buffers, then signal — cl_error longjmps. */
static CL_NORETURN void bt_fail(BtBuild *b, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    build_free(b);
    cl_error(CL_ERR_ARGS, "%s", msg);
}
#define BT_FAIL(b, ...) bt_fail(b, __VA_ARGS__)

static void build_reserve(BtBuild *b, uint32_t more_recs, uint32_t more_names)
{
    if (b->n + more_recs > b->cap) {
        uint32_t ncap = b->cap ? b->cap * 2 : 64;
        BtRec *nr;
        while (ncap < b->n + more_recs) ncap *= 2;
        nr = (BtRec *)platform_alloc(ncap * sizeof(BtRec));
        if (!nr) BT_FAIL(b, "DEFINE-BINDING-TABLE: out of memory");
        if (b->recs) { memcpy(nr, b->recs, b->n * sizeof(BtRec)); platform_free(b->recs); }
        b->recs = nr; b->cap = ncap;
    }
    if (b->names_len + more_names > b->names_cap) {
        uint32_t ncap = b->names_cap ? b->names_cap * 2 : 4096;
        uint8_t *nn;
        uint32_t i;
        while (ncap < b->names_len + more_names) ncap *= 2;
        nn = (uint8_t *)platform_alloc(ncap);
        if (!nn) BT_FAIL(b, "DEFINE-BINDING-TABLE: out of memory");
        if (b->names) {
            memcpy(nn, b->names, b->names_len);
            /* the records point into the old buffer — rebase them */
            for (i = 0; i < b->n; i++)
                b->recs[i].name = nn + (b->recs[i].name - b->names);
            platform_free(b->names);
        }
        b->names = nn; b->names_cap = ncap;
    }
}

/* Append NAME (LEN bytes, already validated) to the arena; returns the
 * record, zero-filled except name/order. */
static BtRec *build_add(BtBuild *b, const uint8_t *name, uint32_t len)
{
    BtRec *r;
    build_reserve(b, 1, len);
    r = &b->recs[b->n];
    memset(r, 0, sizeof(*r));
    r->name = b->names + b->names_len;
    memcpy(r->name, name, len);
    r->name_len = len;
    r->order = b->n;
    b->names_len += len;
    b->n++;
    return r;
}

/* A row's name: a string (or symbol) designator, 1..255 ASCII bytes.
 * Copied into BUF (256 bytes); returns the length. */
static uint32_t parse_name(BtBuild *b, CL_Obj obj, const char *what, uint8_t *buf)
{
    uint32_t len, i;
    if (CL_SYMBOL_P(obj)) obj = ((CL_Symbol *)CL_OBJ_TO_PTR(obj))->name;
    if (!CL_ANY_STRING_P(obj))
        BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d: %s must be a string, got %s",
                b->row, what, cl_type_name(obj));
    len = cl_string_length(obj);
    if (len == 0 || len > 255)
        BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d: %s must be 1..255 characters",
                b->row, what);
    for (i = 0; i < len; i++) {
        int ch = cl_string_char_at(obj, i);
        if (ch <= 0 || ch >= 0x80)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d: %s must be ASCII", b->row, what);
        buf[i] = (uint8_t)ch;
    }
    return len;
}

/* Room for MORE bytes in the wide-payload arena. */
static void wide_reserve(BtBuild *b, uint32_t more)
{
    if (b->wide_len + more > b->wide_cap) {
        uint32_t ncap = b->wide_cap ? b->wide_cap * 2 : 256;
        uint8_t *nw;
        while (ncap < b->wide_len + more) ncap *= 2;
        nw = (uint8_t *)platform_alloc(ncap);
        if (!nw) BT_FAIL(b, "DEFINE-BINDING-TABLE: out of memory");
        if (b->wide) { memcpy(nw, b->wide, b->wide_len); platform_free(b->wide); }
        b->wide = nw; b->wide_cap = ncap;
    }
}

/* Encode VALUE into the record.  An integer: a u32, an i32
 * (CL_BT_F_SIGNED), or — beyond 32 bits either way — a wide payload
 * (CL_BT_F_WIDE: u8 nbytes, u8 sign, big-endian magnitude of up to 255
 * bytes).  A string (a C header's string #define): CL_BT_F_STRING, the
 * payload u8 len + the bytes — ASCII only, like the names, so the FASL is
 * byte-identical on every platform and the reader never sees an encoding
 * question; at most 255 characters. */
static void encode_value(BtBuild *b, BtRec *r, CL_Obj value, const uint8_t *name, uint32_t nlen)
{
    int neg;
    uint8_t mag[256];     /* big-endian magnitude, leading zeros stripped */
    uint32_t nbytes = 0, i;

    if (CL_ANY_STRING_P(value)) {
        uint32_t len = cl_string_length(value);
        if (len > 255)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): string value must be at most 255 characters (got %u)",
                    b->row, (int)nlen, (const char *)name, (unsigned)len);
        for (i = 0; i < len; i++) {
            int ch = cl_string_char_at(value, i);
            if (ch < 0 || ch >= 0x80)
                BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): string value must be ASCII (character %u is code %d)",
                        b->row, (int)nlen, (const char *)name, (unsigned)i, ch);
            mag[i] = (uint8_t)ch;
        }
        wide_reserve(b, 1 + len);
        r->has_wide = 1;
        r->wide_off = b->wide_len;
        r->flags |= CL_BT_F_STRING;
        b->wide[b->wide_len++] = (uint8_t)len;
        memcpy(b->wide + b->wide_len, mag, len);
        b->wide_len += len;
        return;
    }
    if (CL_FIXNUM_P(value)) {
        int32_t v = CL_FIXNUM_VAL(value);
        uint32_t m;
        neg = v < 0;
        m = neg ? (uint32_t)(-(int64_t)v) : (uint32_t)v;
        for (i = 0; i < 4; i++) {
            uint8_t byte = (uint8_t)(m >> (8 * (3 - i)));
            if (nbytes == 0 && byte == 0) continue;
            mag[nbytes++] = byte;
        }
    } else if (CL_BIGNUM_P(value)) {
        CL_Bignum *bn = (CL_Bignum *)CL_OBJ_TO_PTR(value);
        if (bn->length * 2 > 255)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): value exceeds 255 bytes",
                    b->row, (int)nlen, (const char *)name);
        neg = bn->sign != 0;
        for (i = bn->length; i-- > 0; ) {
            uint8_t hi = (uint8_t)(bn->limbs[i] >> 8), lo = (uint8_t)bn->limbs[i];
            if (nbytes == 0 && hi == 0) { if (lo != 0) mag[nbytes++] = lo; continue; }
            mag[nbytes++] = hi;
            mag[nbytes++] = lo;
        }
    } else {
        BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): value must be an integer or a string, got %s",
                b->row, (int)nlen, (const char *)name, cl_type_name(value));
        return;
    }
    if (nbytes <= 4) {
        uint32_t m = 0;
        for (i = 0; i < nbytes; i++) m = (m << 8) | mag[i];
        if (!neg) {
            r->a = m;
            return;
        }
        if (m <= 0x80000000u) {
            r->a = (uint32_t)(int32_t)(-(int64_t)m);
            r->flags |= CL_BT_F_SIGNED;
            return;
        }
    }
    /* wide: append the payload to the wide arena */
    wide_reserve(b, 2 + nbytes);
    r->has_wide = 1;
    r->wide_off = b->wide_len;
    r->flags |= CL_BT_F_WIDE;
    b->wide[b->wide_len++] = (uint8_t)nbytes;
    b->wide[b->wide_len++] = (uint8_t)(neg ? 1 : 0);
    memcpy(b->wide + b->wide_len, mag, nbytes);
    b->wide_len += nbytes;
}

/* Field TYPE spec -> reader stub kind / ctype / element size / count. */
static void parse_field_type(BtBuild *b, CL_Obj type, const uint8_t *name, uint32_t nlen,
                             int *kind, int *ctype, int32_t *elt_size, int32_t *count)
{
    *kind = CL_STUB_PEEK; *ctype = 0; *elt_size = 0; *count = 0;
    if (CL_CONS_P(type) && kw_is(cl_car(type), "STRUCT")) {
        CL_Obj n = CL_CONS_P(cl_cdr(type)) ? cl_car(cl_cdr(type)) : CL_NIL;
        if (!CL_FIXNUM_P(n) || CL_FIXNUM_VAL(n) < 0)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): (:struct N) needs a non-negative size",
                    b->row, (int)nlen, (const char *)name);
        *kind = CL_STUB_FIELD_PTR;
        return;
    }
    if (CL_CONS_P(type) && kw_is(cl_car(type), "ARRAY")) {
        CL_Obj et = CL_CONS_P(cl_cdr(type)) ? cl_car(cl_cdr(type)) : CL_NIL;
        CL_Obj cnt = (CL_CONS_P(cl_cdr(type)) && CL_CONS_P(cl_cdr(cl_cdr(type))))
                     ? cl_car(cl_cdr(cl_cdr(type))) : CL_NIL;
        *ctype = cl_ffi_ctype_from_keyword(et);
        if (*ctype < 0)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): unknown array element type (expected :U8 :I8 :U16 :I16 :U32 :I32 :POINTER :FPTR :SINGLE :DOUBLE)",
                    b->row, (int)nlen, (const char *)name);
        if (!CL_FIXNUM_P(cnt) || CL_FIXNUM_VAL(cnt) <= 0 || CL_FIXNUM_VAL(cnt) > 0xFFFF)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): (:array T N) needs an element count 1..65535",
                    b->row, (int)nlen, (const char *)name);
        *kind = CL_STUB_PEEK_IDX;
        *elt_size = cl_ffi_ctype_size(*ctype);
        *count = CL_FIXNUM_VAL(cnt);
        return;
    }
    *ctype = cl_ffi_ctype_from_keyword(type);
    if (*ctype < 0)
        BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): unknown field type (expected :U8 :I8 :U16 :I16 :U32 :I32 :POINTER :FPTR :SINGLE :DOUBLE, (:struct n) or (:array type n))",
                b->row, (int)nlen, (const char *)name);
}

static void add_field(BtBuild *b, const uint8_t *name, uint32_t nlen, CL_Obj type, CL_Obj offset_obj)
{
    int kind, ctype; int32_t elt_size, count, offset;
    BtRec *r;
    if (!CL_FIXNUM_P(offset_obj) || CL_FIXNUM_VAL(offset_obj) < 0)
        BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): field offset must be a non-negative fixnum",
                b->row, (int)nlen, (const char *)name);
    offset = CL_FIXNUM_VAL(offset_obj);
    parse_field_type(b, type, name, nlen, &kind, &ctype, &elt_size, &count);
    if (kind == CL_STUB_PEEK_IDX && offset > 0xFFFF)
        BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): array field offset must be <= 65535",
                b->row, (int)nlen, (const char *)name);
    r = build_add(b, name, nlen);
    r->kind = CL_BT_FIELD;
    r->d = (uint8_t)kind;
    r->c = (uint8_t)ctype;
    r->a = (kind == CL_STUB_PEEK_IDX) ? ((uint32_t)offset | ((uint32_t)count << 16))
                                      : (uint32_t)offset;
    r->b = (int16_t)elt_size;
}

static int list_length_checked(CL_Obj l)
{
    int n = 0;
    while (CL_CONS_P(l)) { n++; l = cl_cdr(l); }
    return CL_NULL_P(l) ? n : -1;
}

static void parse_row(BtBuild *b, CL_Obj row)
{
    uint8_t nbuf[256];
    uint32_t nlen;
    CL_Obj kind, rest;

    if (list_length_checked(row) < 2)
        BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d: expected (:const|:var|:fn|:field|:struct|:name \"NAME\" ...)",
                b->row);
    kind = cl_car(row);
    nlen = parse_name(b, cl_car(cl_cdr(row)), "name", nbuf);
    rest = cl_cdr(cl_cdr(row));

    if (kw_is(kind, "CONST") || kw_is(kind, "VAR")) {
        BtRec *r;
        if (list_length_checked(rest) != 1)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): expected (%s \"NAME\" value)",
                    b->row, (int)nlen, (const char *)nbuf, kw_is(kind, "CONST") ? ":const" : ":var");
        r = build_add(b, nbuf, nlen);
        r->kind = kw_is(kind, "CONST") ? CL_BT_CONST : CL_BT_VAR;
        encode_value(b, r, cl_car(rest), nbuf, nlen);
        return;
    }
    if (kw_is(kind, "FN")) {
        CL_Obj lvo, regs, result, opt;
        int n, nregs, i;
        uint32_t nibbles = 0;
        int res, minver = 0;
        uint8_t flags = 0;
        BtRec *r;
        n = list_length_checked(rest);
        if (n < 3)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): expected (:fn \"NAME\" lvo (regs...) result [:not-morphos|:morphos] [min-version])",
                    b->row, (int)nlen, (const char *)nbuf);
        lvo = cl_car(rest); regs = cl_car(cl_cdr(rest)); result = cl_car(cl_cdr(cl_cdr(rest)));
        if (!CL_FIXNUM_P(lvo) || CL_FIXNUM_VAL(lvo) >= 0 || CL_FIXNUM_VAL(lvo) < -32768)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): LVO must be a negative integer >= -32768",
                    b->row, (int)nlen, (const char *)nbuf);
        nregs = list_length_checked(regs);
        if (nregs < 0 || nregs > 7)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): register list must hold 0..7 of :D0-:D7/:A0-:A4 (more registers go through AMIGA:CALL-LIBRARY)",
                    b->row, (int)nlen, (const char *)nbuf);
        for (i = 0; i < nregs; i++) {
            int ri = reg_index_of(cl_car(regs));
            if (ri < 0)
                BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): register %d is not one of :D0-:D7/:A0-:A4 (:A5 is the dispatcher's scratch register, :A6 the library base)",
                        b->row, (int)nlen, (const char *)nbuf, i);
            nibbles |= (uint32_t)ri << (4 * i);
            regs = cl_cdr(regs);
        }
        res = cl_ffi_res_kind_from_keyword(result);
        if (res < 0)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): result kind must be one of :UNSIGNED :VOID :POINTER :SIGNED :BOOL :U16 :I16 :U8 :I8",
                    b->row, (int)nlen, (const char *)nbuf);
        opt = cl_cdr(cl_cdr(cl_cdr(rest)));
        while (CL_CONS_P(opt)) {
            CL_Obj o = cl_car(opt);
            if (kw_is(o, "NOT-MORPHOS")) flags |= CL_BT_F_NOT_MORPHOS;
            else if (kw_is(o, "MORPHOS")) flags |= CL_BT_F_MORPHOS;
            else if (CL_FIXNUM_P(o) && CL_FIXNUM_VAL(o) > 0 && CL_FIXNUM_VAL(o) <= 255)
                minver = CL_FIXNUM_VAL(o);
            else
                BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): unknown option %s (expected :not-morphos, :morphos or a minimum library version 1..255)",
                        b->row, (int)nlen, (const char *)nbuf,
                        CL_SYMBOL_P(o) ? cl_symbol_name(o) : cl_type_name(o));
            opt = cl_cdr(opt);
        }
        if ((flags & CL_BT_F_NOT_MORPHOS) && (flags & CL_BT_F_MORPHOS))
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): :not-morphos and :morphos exclude each other",
                    b->row, (int)nlen, (const char *)nbuf);
        r = build_add(b, nbuf, nlen);
        r->kind = CL_BT_LIBCALL;
        r->flags = flags;
        r->a = CL_AMIGA_MAKE_REGSPEC(nibbles, res);
        r->b = (int16_t)CL_FIXNUM_VAL(lvo);
        r->c = (uint8_t)nregs;
        r->d = (uint8_t)minver;
        b->has_libcall = 1;
        return;
    }
    if (kw_is(kind, "FIELD")) {
        if (list_length_checked(rest) != 2)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): expected (:field \"NAME\" type offset)",
                    b->row, (int)nlen, (const char *)nbuf);
        add_field(b, nbuf, nlen, cl_car(rest), cl_car(cl_cdr(rest)));
        return;
    }
    if (kw_is(kind, "STRUCT")) {
        CL_Obj size, fields;
        uint8_t fbuf[256], full[256];
        uint32_t flen;
        BtRec *r;
        if (list_length_checked(rest) < 1 || !CL_FIXNUM_P(cl_car(rest)) || CL_FIXNUM_VAL(cl_car(rest)) < 0)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): expected (:struct \"NAME\" size (\"FIELD\" type offset)...)",
                    b->row, (int)nlen, (const char *)nbuf);
        size = cl_car(rest);
        /* *NAME-SIZE* as a special variable (what DEFCSTRUCT's DEFVAR was) */
        if (nlen + 7 > 255)
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): struct name too long",
                    b->row, (int)nlen, (const char *)nbuf);
        full[0] = '*'; memcpy(full + 1, nbuf, nlen); memcpy(full + 1 + nlen, "-SIZE*", 6);
        r = build_add(b, full, nlen + 7);
        r->kind = CL_BT_VAR;
        encode_value(b, r, size, full, nlen + 7);
        fields = cl_cdr(rest);
        while (CL_CONS_P(fields)) {
            CL_Obj f = cl_car(fields);
            if (list_length_checked(f) != 3)
                BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): struct field must be (\"FIELD\" type offset)",
                        b->row, (int)nlen, (const char *)nbuf);
            flen = parse_name(b, cl_car(f), "field name", fbuf);
            if (nlen + 1 + flen > 255)
                BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): accessor name too long",
                        b->row, (int)nlen, (const char *)nbuf);
            memcpy(full, nbuf, nlen); full[nlen] = '-'; memcpy(full + nlen + 1, fbuf, flen);
            add_field(b, full, nlen + 1 + flen, cl_car(cl_cdr(f)), cl_car(cl_cdr(cl_cdr(f))));
            fields = cl_cdr(fields);
        }
        if (!CL_NULL_P(fields))
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): struct fields must be a proper list",
                    b->row, (int)nlen, (const char *)nbuf);
        return;
    }
    if (kw_is(kind, "NAME")) {
        BtRec *r;
        if (!CL_NULL_P(rest))
            BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): expected (:name \"NAME\")",
                    b->row, (int)nlen, (const char *)nbuf);
        r = build_add(b, nbuf, nlen);
        r->kind = CL_BT_NAME;
        return;
    }
    BT_FAIL(b, "DEFINE-BINDING-TABLE: row %d (%.*s): unknown row kind %s (expected :const :var :fn :field :struct :name)",
            b->row, (int)nlen, (const char *)nbuf,
            CL_SYMBOL_P(kind) ? cl_symbol_name(kind) : cl_type_name(kind));
}

static int rec_cmp(const void *pa, const void *pb)
{
    const BtRec *a = (const BtRec *)pa, *b = (const BtRec *)pb;
    int c = name_cmp(a->name, a->name_len, b->name, b->name_len);
    if (c != 0) return c;
    return (a->order < b->order) ? -1 : (a->order > b->order) ? 1 : 0;
}

CL_Obj cl_bindtab_pack(CL_Obj rows)
{
    BtBuild b;
    CL_Obj list, blob;
    uint32_t i, n_wide = 0, total, off, names_off;
    uint8_t *out;

    memset(&b, 0, sizeof(b));
    if (list_length_checked(rows) < 0)
        cl_error(CL_ERR_ARGS, "DEFINE-BINDING-TABLE: rows must be a proper list");
    /* Parsing allocates nothing on the heap (keyword matches are by name,
     * values are decoded in place), so the row list and its strings stay
     * put while we walk them. */
    for (list = rows, b.row = 1; CL_CONS_P(list); list = cl_cdr(list), b.row++)
        parse_row(&b, cl_car(list));

    if (b.n > 0)
        qsort(b.recs, b.n, sizeof(BtRec), rec_cmp);
    /* duplicates: only library-call variants may share a name */
    for (i = 1; i < b.n; i++) {
        BtRec *p = &b.recs[i - 1], *q = &b.recs[i];
        if (name_cmp(p->name, p->name_len, q->name, q->name_len) == 0 &&
            !(p->kind == CL_BT_LIBCALL && q->kind == CL_BT_LIBCALL))
            BT_FAIL(&b, "DEFINE-BINDING-TABLE: duplicate name %.*s (only :fn rows may repeat a name, for platform/version variants)",
                    (int)q->name_len, (const char *)q->name);
    }
    for (i = 0; i < b.n; i++) if (b.recs[i].has_wide) n_wide++;

    /* Layout: header, entries, names, then the wide value payloads (both
     * inside the "name arena" the header measures). */
    names_off = CL_BT_HEADER_SIZE + b.n * CL_BT_ENTRY_SIZE;
    total = names_off + b.names_len + b.wide_len;
    blob = cl_make_byte_vector(total, 0, 0);
    out = ((CL_ByteVector *)CL_OBJ_TO_PTR(blob))->data;
    wr32(out, CL_BT_MAGIC);
    out[4] = CL_BT_VERSION;
    out[5] = (uint8_t)(b.has_libcall ? CL_BT_HF_LIBCALLS : 0);
    wr16(out + 6, 0);
    wr32(out + 8, b.n);
    wr32(out + 12, total - names_off);
    /* name arena: records in sorted order, names re-laid contiguously */
    off = 0;
    for (i = 0; i < b.n; i++) {
        BtRec *r = &b.recs[i];
        uint8_t *ep = out + CL_BT_HEADER_SIZE + i * CL_BT_ENTRY_SIZE;
        memcpy(out + names_off + off, r->name, r->name_len);
        wr32(ep, off);
        wr16(ep + 4, (uint16_t)r->name_len);
        ep[6] = r->kind;
        ep[7] = r->flags;
        wr32(ep + 8, r->a);
        wr16(ep + 12, (uint16_t)r->b);
        ep[14] = r->c;
        ep[15] = r->d;
        off += r->name_len;
    }
    if (n_wide) {
        /* the wide arena follows the names; entry.a = offset within the
         * (names + wide) arena the header measures */
        memcpy(out + names_off + b.names_len, b.wide, b.wide_len);
        for (i = 0; i < b.n; i++) {
            BtRec *r = &b.recs[i];
            if (!r->has_wide) continue;
            wr32(out + CL_BT_HEADER_SIZE + i * CL_BT_ENTRY_SIZE + 8,
                 b.names_len + r->wide_off);
        }
    }
    build_free(&b);
    return blob;
}

/* ================================================================
 * Probe
 * ================================================================ */

/* Lower bound: first entry whose name is >= NAME. */
static uint32_t lower_bound(const BtView *v, const uint8_t *name, uint32_t len)
{
    uint32_t lo = 0, hi = v->n;
    BtEntry e;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        decode_entry(v, mid, &e);
        if (name_cmp(entry_name(v, &e), e.name_len, name, len) < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static int entry_name_is(const BtView *v, uint32_t i, const uint8_t *name, uint32_t len)
{
    BtEntry e;
    if (i >= v->n) return 0;
    decode_entry(v, i, &e);
    return e.name_len == len && memcmp(entry_name(v, &e), name, len) == 0;
}

/* Platform/version guard of ENTRY, against the live *FEATURES* and the
 * table's version variable (unbound / NIL version = every versioned
 * entry fails, as (%version>= n) did). */
static int guard_ok(CL_Obj vec, const BtEntry *e)
{
    if (e->flags & (CL_BT_F_NOT_MORPHOS | CL_BT_F_MORPHOS)) {
        int mos = morphos_feature_p();
        if ((e->flags & CL_BT_F_NOT_MORPHOS) && mos) return 0;
        if ((e->flags & CL_BT_F_MORPHOS) && !mos) return 0;
    }
    if (e->kind == CL_BT_LIBCALL && e->d != 0) {
        CL_Obj vsym = bindings_slot(vec, CL_BT_SLOT_VERSION);
        CL_Obj val;
        if (!CL_SYMBOL_P(vsym)) return 0;
        val = cl_symbol_value(vsym);
        if (!CL_FIXNUM_P(val)) return 0;
        if (CL_FIXNUM_VAL(val) < (int32_t)e->d) return 0;
    }
    return 1;
}

int cl_bindtab_probe_nolock(CL_Obj package, const char *name, uint32_t len,
                            int allow_setter, int *name_idx, int *def_idx,
                            int *want_setter)
{
    CL_Obj vec = pkg_bindings(package);
    CL_Obj blob = pkg_blob(package);
    BtView v;
    uint32_t i;
    const uint8_t *nm = (const uint8_t *)name;

    if (CL_NULL_P(blob)) return 0;   /* no table, or shed at SAVE-IMAGE */
    view_blob(blob, &v);
    i = lower_bound(&v, nm, len);
    if (entry_name_is(&v, i, nm, len)) {
        uint32_t j = i;
        *name_idx = (int)i;
        *def_idx = -1;
        *want_setter = 0;
        while (entry_name_is(&v, j, nm, len)) {
            BtEntry e;
            decode_entry(&v, j, &e);
            if (guard_ok(vec, &e)) { *def_idx = (int)j; break; }
            j++;
        }
        return 1;
    }
    if (allow_setter && len > 5 && memcmp(name, "%SET-", 5) == 0) {
        i = lower_bound(&v, nm + 5, len - 5);
        if (entry_name_is(&v, i, nm + 5, len - 5)) {
            BtEntry e;
            decode_entry(&v, i, &e);
            if (e.kind == CL_BT_FIELD && e.d != CL_STUB_FIELD_PTR) {
                *name_idx = (int)i;
                *def_idx = (int)i;
                *want_setter = 1;
                return 1;
            }
        }
    }
    return 0;
}

/* ================================================================
 * Materialiser
 * ================================================================ */

static void pkg_wrlock(void)
{
    if (cl_package_rwlock) platform_rwlock_wrlock(cl_package_rwlock);
}
static void pkg_rdlock(void)
{
    if (cl_package_rwlock) platform_rwlock_rdlock(cl_package_rwlock);
}
static void pkg_unlock(void)
{
    if (cl_package_rwlock) platform_rwlock_unlock(cl_package_rwlock);
}

/* Copy the definition prepared for a fresh symbol onto an already-present
 * one (plain stores, under the write lock).  STUB's name field is patched
 * to the surviving symbol so errors/printing name the right object. */
static void install_definition(CL_Obj target, int kind, CL_Obj value, CL_Obj stub)
{
    CL_Symbol *t = (CL_Symbol *)CL_OBJ_TO_PTR(target);
    switch (kind) {
    case CL_BT_CONST:
        t->value = value;
        t->flags |= CL_SYM_CONSTANT;
        break;
    case CL_BT_VAR:
        t->value = value;
        t->flags |= CL_SYM_SPECIAL;
        break;
    case CL_BT_LIBCALL:
    case CL_BT_FIELD:
        ((CL_FfiStub *)CL_OBJ_TO_PTR(stub))->name = target;
        cl_symbol_set_function(target, stub);
        break;
    default:
        break;
    }
}

static int definition_missing(CL_Obj sym, int kind)
{
    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    switch (kind) {
    case CL_BT_CONST: case CL_BT_VAR:      return s->value == CL_UNBOUND;
    case CL_BT_LIBCALL: case CL_BT_FIELD:  return s->function == CL_UNBOUND;
    default:                               return 0;
    }
}

CL_Obj cl_bindtab_materialize(CL_Obj package, const char *query, uint32_t qlen,
                              int name_idx, int def_idx, int want_setter,
                              int force)
{
    uint8_t nbuf[256], sbuf[262], qbuf[256];
    uint32_t nlen, slen = 0;
    BtEntry e;
    BtValue val;
    int kind = -1, has_setter = 0;
    CL_Obj vec, base_sym = CL_NIL;
    CL_Obj name_str, sym, cell, ecell, value = CL_UNBOUND, stub = CL_NIL;
    CL_Obj sname_str = CL_NIL, ssym = CL_NIL, scell = CL_NIL, sstub = CL_NIL;
    CL_Obj result, sresult = CL_NIL;
    int nprot = 0;

    /* The queried spelling may point into the heap (a Lisp string's data):
     * copy it before anything can move it.  It is only needed for the
     * dropped-table fallback below. */
    if (query && qlen < sizeof(qbuf)) memcpy(qbuf, query, qlen); else qlen = 0;

    CL_GC_PROTECT(package); nprot++;

    /* 1. Copy everything we need out of the blob (it moves under GC). */
    vec = pkg_bindings(package);
    if (CL_NULL_P(pkg_blob(package))) {
        /* A concurrent flip materialised every entry and dropped the table
         * between the probe and here (then the name is present now), or a
         * concurrent SHED dropped the blob (then it is not, and NIL — "no
         * such symbol" — is the right answer). */
        CL_Obj present = CL_UNBOUND;
        if (qlen) {
            pkg_rdlock();
            present = cl_package_find_own_symbol_nolock((const char *)qbuf, qlen, package);
            pkg_unlock();
        }
        CL_GC_UNPROTECT(nprot);
        return present == CL_UNBOUND ? CL_NIL : present;
    }
    {
        BtView v;
        BtEntry ne;
        view_blob(bindings_slot(vec, CL_BT_SLOT_BLOB), &v);
        decode_entry(&v, (uint32_t)name_idx, &ne);
        nlen = ne.name_len;
        memcpy(nbuf, entry_name(&v, &ne), nlen);
        if (def_idx >= 0) {
            decode_entry(&v, (uint32_t)def_idx, &e);
            kind = e.kind;
            if (kind == CL_BT_CONST || kind == CL_BT_VAR)
                decode_value(&v, &e, &val);
            if (kind == CL_BT_FIELD && e.d != CL_STUB_FIELD_PTR) {
                has_setter = 1;
                memcpy(sbuf, "%SET-", 5);
                memcpy(sbuf + 5, nbuf, nlen);
                slen = nlen + 5;
            }
        }
        base_sym = bindings_slot(vec, CL_BT_SLOT_BASE);
    }
    CL_GC_PROTECT(base_sym); nprot++;

    /* 2. Allocate everything outside the lock. */
    name_str = cl_make_string((const char *)nbuf, nlen);
    CL_GC_PROTECT(name_str); nprot++;
    sym = cl_make_symbol(name_str);
    CL_GC_PROTECT(sym); nprot++;
    ((CL_Symbol *)CL_OBJ_TO_PTR(sym))->hash = cl_hash_string((const char *)nbuf, nlen);
    cell = cl_cons(sym, CL_NIL);
    CL_GC_PROTECT(cell); nprot++;
    ecell = cl_cons(sym, CL_NIL);
    CL_GC_PROTECT(ecell); nprot++;
    switch (kind) {
    case CL_BT_CONST: case CL_BT_VAR:
        value = value_to_obj(&val);
        CL_GC_PROTECT(value); nprot++;
        break;
    case CL_BT_LIBCALL:
        stub = cl_make_ffi_stub(CL_STUB_LIBCALL, sym, base_sym, e.a, e.b, e.c);
        CL_GC_PROTECT(stub); nprot++;
        break;
    case CL_BT_FIELD:
        stub = cl_make_ffi_stub(e.d, sym, CL_NIL, e.a, e.b, e.c);
        CL_GC_PROTECT(stub); nprot++;
        if (has_setter) {
            sname_str = cl_make_string((const char *)sbuf, slen);
            CL_GC_PROTECT(sname_str); nprot++;
            ssym = cl_make_symbol(sname_str);
            CL_GC_PROTECT(ssym); nprot++;
            ((CL_Symbol *)CL_OBJ_TO_PTR(ssym))->hash =
                cl_hash_string((const char *)sbuf, slen);
            scell = cl_cons(ssym, CL_NIL);
            CL_GC_PROTECT(scell); nprot++;
            sstub = cl_make_ffi_stub(e.d == CL_STUB_PEEK_IDX ? CL_STUB_POKE_IDX : CL_STUB_POKE,
                                     ssym, CL_NIL, e.a, e.b, e.c);
            CL_GC_PROTECT(sstub); nprot++;
        }
        break;
    default:
        break;
    }
    /* The fresh symbol is complete BEFORE it becomes visible. */
    if (kind >= 0) install_definition(sym, kind, value, stub);
    if (has_setter)
        cl_symbol_set_function(ssym, sstub);

    /* 3. Link under the write lock, re-checking for a peer's win. */
    pkg_wrlock();
    {
        CL_Obj existing = cl_package_find_own_symbol_nolock((const char *)nbuf, nlen, package);
        if (existing == CL_UNBOUND) {
            cl_package_add_symbol_cell(package, sym, cell);
            cl_package_push_export_cell_nolock(package, sym, ecell);
            result = sym;
        } else {
            result = existing;
            if (kind >= 0 && (force || definition_missing(existing, kind)))
                install_definition(existing, kind, value, stub);
            if (!cl_package_exported_p_nolock(existing, package)) {
                ((CL_Cons *)CL_OBJ_TO_PTR(ecell))->car = existing;
                cl_package_push_export_cell_nolock(package, existing, ecell);
            }
        }
        if (has_setter) {
            CL_Obj sexisting = cl_package_find_own_symbol_nolock((const char *)sbuf, slen, package);
            if (sexisting == CL_UNBOUND) {
                cl_package_add_symbol_cell(package, ssym, scell);
                sresult = ssym;
            } else {
                sresult = sexisting;
                if (force || ((CL_Symbol *)CL_OBJ_TO_PTR(sexisting))->function == CL_UNBOUND) {
                    ((CL_FfiStub *)CL_OBJ_TO_PTR(sstub))->name = sexisting;
                    cl_symbol_set_function(sexisting, sstub);
                }
            }
        }
    }
    pkg_unlock();

    /* 4. The DEFSETF pair (allocates — outside the lock; idempotent). */
    if (has_setter && cl_get_setf_updater(result) != sresult) {
        CL_GC_PROTECT(result); CL_GC_PROTECT(sresult);
        cl_register_setf_updater(result, sresult);
        CL_GC_UNPROTECT(2);
    }
    CL_GC_UNPROTECT(nprot);
    return want_setter ? sresult : result;
}

void cl_bindtab_materialize_all(CL_Obj package)
{
    uint32_t i, n;
    /* A SHED package (vector present, blob NIL) has nothing to materialise
     * and keeps its marker: this is not "flip me eager", it is "the names
     * are gone".  Without the pkg_blob check view_blob would read a NIL. */
    if (!CL_PACKAGE_P(package) || CL_NULL_P(pkg_blob(package))) return;
    CL_GC_PROTECT(package);
    {
        BtView v;
        view_blob(pkg_blob(package), &v);
        n = v.n;
    }
    for (i = 0; i < n; i++) {
        uint8_t nbuf[256];
        uint32_t nlen;
        int name_idx = 0, def_idx = -1, want_setter = 0, hit = 0;
        CL_Obj existing, blob;
        /* re-derive the blob each round: the previous materialisation
         * may have compacted the heap, or a peer flip may have dropped
         * the table (then everything is present already). */
        blob = pkg_blob(package);
        if (CL_NULL_P(blob)) break;   /* peer flip dropped it, or a shed */
        {
            BtView v;
            BtEntry e, p;
            view_blob(blob, &v);
            decode_entry(&v, i, &e);
            /* one materialisation per distinct name (variants are adjacent) */
            if (i > 0) {
                decode_entry(&v, i - 1, &p);
                if (p.name_len == e.name_len &&
                    memcmp(entry_name(&v, &p), entry_name(&v, &e), e.name_len) == 0)
                    continue;
            }
            nlen = e.name_len;
            memcpy(nbuf, entry_name(&v, &e), nlen);
        }
        pkg_rdlock();
        existing = cl_package_find_own_symbol_nolock((const char *)nbuf, nlen, package);
        if (existing == CL_UNBOUND)
            hit = cl_bindtab_probe_nolock(package, (const char *)nbuf, nlen, 0,
                                          &name_idx, &def_idx, &want_setter);
        pkg_unlock();
        if (existing == CL_UNBOUND && hit)
            (void)cl_bindtab_materialize(package, (const char *)nbuf, nlen,
                                         name_idx, def_idx, 0, 0);
    }
    pkg_wrlock();
    ((CL_Package *)CL_OBJ_TO_PTR(package))->bindings = CL_NIL;
    pkg_unlock();
    CL_GC_UNPROTECT(1);
}

/* ================================================================
 * Registration
 * ================================================================ */

void cl_bindtab_register(CL_Obj package, CL_Obj blob, CL_Obj base_sym,
                         CL_Obj version_sym)
{
    const char *err;
    CL_Obj vec;
    CL_Package *pkg;

    if (!CL_PACKAGE_P(package))
        cl_error(CL_ERR_TYPE, "%%REGISTER-BINDING-TABLE: not a package");
    err = validate_blob(blob);
    if (err) {
        CL_String *pn = (CL_String *)CL_OBJ_TO_PTR(((CL_Package *)CL_OBJ_TO_PTR(package))->name);
        cl_error(CL_ERR_ARGS, "%%REGISTER-BINDING-TABLE: %.*s: malformed binding table (%s)",
                 (int)pn->length, pn->data, err);
    }
    if (!CL_NULL_P(base_sym) && !CL_SYMBOL_P(base_sym))
        cl_error(CL_ERR_TYPE, "%%REGISTER-BINDING-TABLE: library base must be a symbol or NIL");
    if (!CL_NULL_P(version_sym) && !CL_SYMBOL_P(version_sym))
        cl_error(CL_ERR_TYPE, "%%REGISTER-BINDING-TABLE: version variable must be a symbol or NIL");
    if ((((CL_ByteVector *)CL_OBJ_TO_PTR(blob))->data[5] & CL_BT_HF_LIBCALLS) && CL_NULL_P(base_sym))
        cl_error(CL_ERR_ARGS,
                 "%%REGISTER-BINDING-TABLE: the table defines library calls but no :BASE variable was given");

    CL_GC_PROTECT(package);
    CL_GC_PROTECT(blob);
    CL_GC_PROTECT(base_sym);
    CL_GC_PROTECT(version_sym);
    vec = cl_make_vector(CL_BT_SLOTS);
    {
        CL_Vector *vp = (CL_Vector *)CL_OBJ_TO_PTR(vec);
        vp->data[CL_BT_SLOT_BLOB] = blob;
        vp->data[CL_BT_SLOT_BASE] = base_sym;
        vp->data[CL_BT_SLOT_VERSION] = version_sym;
    }
    CL_GC_PROTECT(vec);

    pkg_wrlock();
    pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
    pkg->bindings = vec;
    pkg_unlock();

    /* Registration pass: every symbol already present whose name the
     * table defines gets its definition now — the DEFPACKAGE :SHADOW
     * names (AMIGA.RAW.DOS:OPEN), and on a reload every name materialised
     * before.  Collect (index) pairs under the read lock, build outside. */
    {
        struct { int name_idx, def_idx, want_setter; } *hits = NULL;
        uint32_t nhits = 0, cap, i, bi;
        pkg_rdlock();
        pkg = (CL_Package *)CL_OBJ_TO_PTR(package);
        cap = pkg->sym_count + 1;
        hits = platform_alloc(cap * sizeof(*hits));
        if (hits) {
            CL_Vector *tbl = (CL_Vector *)CL_OBJ_TO_PTR(pkg->symbols);
            for (bi = 0; bi < tbl->length && nhits < cap; bi++) {
                CL_Obj list = tbl->data[bi];
                while (!CL_NULL_P(list) && nhits < cap) {
                    CL_Symbol *s = (CL_Symbol *)CL_OBJ_TO_PTR(cl_car(list));
                    CL_String *sn = (CL_String *)CL_OBJ_TO_PTR(s->name);
                    int ni, di, ws;
                    if (cl_bindtab_probe_nolock(package, sn->data, sn->length, 1,
                                                &ni, &di, &ws)) {
                        hits[nhits].name_idx = ni;
                        hits[nhits].def_idx = di;
                        hits[nhits].want_setter = ws;
                        nhits++;
                    }
                    list = cl_cdr(list);
                }
            }
        }
        pkg_unlock();
        if (!hits)
            cl_error(CL_ERR_STORAGE, "%%REGISTER-BINDING-TABLE: out of memory");
        for (i = 0; i < nhits; i++)
            (void)cl_bindtab_materialize(package, NULL, 0,
                                         hits[i].name_idx, hits[i].def_idx,
                                         hits[i].want_setter, 1);
        platform_free(hits);
    }
    CL_GC_UNPROTECT(5);
}

/* ================================================================
 * Shedding (SAVE-IMAGE :SHAKE-BINDINGS — spec Phase 3)
 * ================================================================ */

int cl_bindtab_shed_p(CL_Obj package)
{
    if (!CL_PACKAGE_P(package)) return 0;
    return !CL_NULL_P(pkg_bindings(package)) && CL_NULL_P(pkg_blob(package));
}

uint32_t cl_bindtab_shed(CL_Obj package)
{
    CL_Obj vec, blob;
    uint32_t bytes;

    if (!CL_PACKAGE_P(package)) return 0;
    vec = pkg_bindings(package);
    if (CL_NULL_P(vec)) return 0;
    blob = bindings_slot(vec, CL_BT_SLOT_BLOB);
    if (CL_NULL_P(blob)) return 0;            /* already shed */
    bytes = ((CL_ByteVector *)CL_OBJ_TO_PTR(blob))->length;

    /* Plain store under the write lock — no allocation, so the "never
     * allocate under the package lock" rule is trivially satisfied.  The
     * vector itself STAYS: its presence with a NIL blob is what
     * cl_bindtab_shed_p reports, and what the reader's "not exported"
     * error turns into an actionable message.  The blob's only reference
     * is this slot, so the next GC reclaims it. */
    pkg_wrlock();
    ((CL_Vector *)CL_OBJ_TO_PTR(vec))->data[CL_BT_SLOT_BLOB] = CL_NIL;
    pkg_unlock();
    return bytes;
}

uint32_t cl_bindtab_shed_all(uint32_t *bytes_out)
{
    CL_Obj reg;
    uint32_t count = 0, bytes = 0;

    /* Non-allocating walk of the package registry (an alist of
     * (name-string . package)), so nothing can move underneath it. */
    reg = cl_package_registry;
    while (!CL_NULL_P(reg)) {
        CL_Obj entry = cl_car(reg);
        if (CL_CONS_P(entry)) {
            uint32_t n = cl_bindtab_shed(cl_cdr(entry));
            if (n) { count++; bytes += n; }
        }
        reg = cl_cdr(reg);
    }
    if (bytes_out) *bytes_out = bytes;
    return count;
}

/* ================================================================
 * Builtins
 * ================================================================ */

static CL_Obj coerce_package(CL_Obj arg, const char *who)
{
    CL_Obj pkg = CL_NIL;
    if (CL_PACKAGE_P(arg)) return arg;
    if (CL_ANY_STRING_P(arg) || CL_SYMBOL_P(arg)) {
        char buf[256];
        CL_Obj s = CL_SYMBOL_P(arg) ? ((CL_Symbol *)CL_OBJ_TO_PTR(arg))->name : arg;
        uint32_t len = cl_string_length(s), i;
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        for (i = 0; i < len; i++) buf[i] = (char)cl_string_char_at(s, i);
        pkg = cl_find_package(buf, len);
        if (CL_NULL_P(pkg))
            cl_error(CL_ERR_GENERAL, "%s: package \"%.*s\" not found", who, (int)len, buf);
        return pkg;
    }
    cl_error(CL_ERR_TYPE, "%s: not a package designator", who);
    return CL_NIL;
}

/* (amilisp::%make-binding-table rows) -> byte vector */
static CL_Obj bi_make_binding_table(CL_Obj *args, int nargs)
{
    (void)nargs;
    return cl_bindtab_pack(args[0]);
}

/* (amilisp::%register-binding-table package blob base-sym version-sym) -> T */
static CL_Obj bi_register_binding_table(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_package(args[0], "%REGISTER-BINDING-TABLE");
    (void)nargs;
    cl_bindtab_register(pkg, args[1], args[2], args[3]);
    return CL_T;
}

/* (amilisp::%binding-table-materialize-all package) -> T */
static CL_Obj bi_binding_table_materialize_all(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_package(args[0], "%BINDING-TABLE-MATERIALIZE-ALL");
    (void)nargs;
    cl_bindtab_materialize_all(pkg);
    return CL_T;
}

/* (amilisp::%shed-binding-tables &optional package) -> bytes shed
 *   With PACKAGE, sheds that one; with no argument, every lazy package.
 *   The destructive delivery operation EXT:SAVE-IMAGE :SHAKE-BINDINGS
 *   performs — exposed here (internal, like %BINDING-TABLE-MATERIALIZE-ALL)
 *   so it is testable without writing an image. */
static CL_Obj bi_shed_binding_tables(CL_Obj *args, int nargs)
{
    uint32_t bytes = 0;
    if (nargs >= 1 && !CL_NULL_P(args[0]))
        bytes = cl_bindtab_shed(coerce_package(args[0], "%SHED-BINDING-TABLES"));
    else
        (void)cl_bindtab_shed_all(&bytes);
    return CL_MAKE_FIXNUM((int32_t)bytes);
}

/* (amilisp::%binding-table-info package)
 *   -> (:entries N :bytes B :symbols S :base SYM :version SYM :shed BOOL),
 *      or NIL when PACKAGE has no binding table.  :SYMBOLS is the number
 *      of symbols currently present (materialised + the eager ones).
 *      :SHED T means the table was dropped by SAVE-IMAGE :SHAKE-BINDINGS —
 *      the package is closed at the names it had then, and :ENTRIES /
 *      :BYTES read 0. */
static CL_Obj bi_binding_table_info(CL_Obj *args, int nargs)
{
    CL_Obj pkg = coerce_package(args[0], "%BINDING-TABLE-INFO");
    CL_Obj vec, plist = CL_NIL, blob, base, version;
    uint32_t n, bytes, syms;
    int shed;
    (void)nargs;
    vec = pkg_bindings(pkg);
    if (CL_NULL_P(vec)) return CL_NIL;
    blob = bindings_slot(vec, CL_BT_SLOT_BLOB);
    base = bindings_slot(vec, CL_BT_SLOT_BASE);
    version = bindings_slot(vec, CL_BT_SLOT_VERSION);
    shed = CL_NULL_P(blob);
    if (shed) {
        n = 0; bytes = 0;            /* shed: the marker without the table */
    } else {
        BtView v;
        view_blob(blob, &v);
        n = v.n; bytes = v.len;
    }
    syms = ((CL_Package *)CL_OBJ_TO_PTR(pkg))->sym_count;
    CL_GC_PROTECT(base);
    CL_GC_PROTECT(version);
    CL_GC_PROTECT(plist);
    /* built back to front */
    plist = cl_cons(shed ? CL_T : CL_NIL, plist);
    plist = cl_cons(keyword("SHED"), plist);
    plist = cl_cons(version, plist); plist = cl_cons(keyword("VERSION"), plist);
    plist = cl_cons(base, plist);    plist = cl_cons(keyword("BASE"), plist);
    plist = cl_cons(CL_MAKE_FIXNUM((int32_t)syms), plist);  plist = cl_cons(keyword("SYMBOLS"), plist);
    plist = cl_cons(CL_MAKE_FIXNUM((int32_t)bytes), plist); plist = cl_cons(keyword("BYTES"), plist);
    plist = cl_cons(CL_MAKE_FIXNUM((int32_t)n), plist);     plist = cl_cons(keyword("ENTRIES"), plist);
    CL_GC_UNPROTECT(3);
    return plist;
}

/* One decoded entry as a row in DEFINE-BINDING-TABLE syntax:
 *   (:const "N" v) (:var "N" v)        v = integer or string
 *   (:fn "N" lvo (:a0 ...) :result [:not-morphos|:morphos] [minver])
 *   (:field "N" type offset)   type = ctype keyword | (:array ct n) | :struct
 *   (:name "N")
 * Allocates — the entry is already decoded into C memory by the caller. */
static CL_Obj entry_row(const BtEntry *e, const uint8_t *name, const BtValue *val)
{
    CL_Obj row = CL_NIL, name_str, tmp;
    CL_GC_PROTECT(row);
    name_str = cl_make_string((const char *)name, e->name_len);
    CL_GC_PROTECT(name_str);
    switch (e->kind) {
    case CL_BT_CONST: case CL_BT_VAR:
        tmp = value_to_obj(val);
        CL_GC_PROTECT(tmp);
        row = cl_cons(tmp, CL_NIL);
        CL_GC_UNPROTECT(1);
        row = cl_cons(name_str, row);
        row = cl_cons(keyword(e->kind == CL_BT_CONST ? "CONST" : "VAR"), row);
        break;
    case CL_BT_LIBCALL: {
        int i;
        CL_Obj regs = CL_NIL;
        CL_GC_PROTECT(regs);
        row = CL_NIL;
        if (e->d) row = cl_cons(CL_MAKE_FIXNUM((int32_t)e->d), row);
        if (e->flags & CL_BT_F_MORPHOS) row = cl_cons(keyword("MORPHOS"), row);
        if (e->flags & CL_BT_F_NOT_MORPHOS) row = cl_cons(keyword("NOT-MORPHOS"), row);
        row = cl_cons(keyword(cl_ffi_res_kind_name(CL_AMIGA_RES_KIND(e->a))), row);
        for (i = (int)e->c - 1; i >= 0; i--)
            regs = cl_cons(keyword(reg_names[(e->a >> (4 * i)) & 0xF]), regs);
        row = cl_cons(regs, row);
        row = cl_cons(CL_MAKE_FIXNUM((int32_t)e->b), row);
        row = cl_cons(name_str, row);
        row = cl_cons(keyword("FN"), row);
        CL_GC_UNPROTECT(1);
        break;
    }
    case CL_BT_FIELD: {
        CL_Obj type;
        uint32_t offset = (e->d == CL_STUB_PEEK_IDX) ? (e->a & 0xFFFFu) : e->a;
        row = cl_cons(CL_MAKE_FIXNUM((int32_t)offset), CL_NIL);
        if (e->d == CL_STUB_FIELD_PTR) {
            type = keyword("STRUCT");
        } else if (e->d == CL_STUB_PEEK_IDX) {
            type = cl_cons(CL_MAKE_FIXNUM((int32_t)(e->a >> 16)), CL_NIL);
            CL_GC_PROTECT(type);
            type = cl_cons(keyword(cl_ffi_ctype_name(e->c)), type);
            type = cl_cons(keyword("ARRAY"), type);
            CL_GC_UNPROTECT(1);
        } else {
            type = keyword(cl_ffi_ctype_name(e->c));
        }
        CL_GC_PROTECT(type);
        row = cl_cons(type, row);
        CL_GC_UNPROTECT(1);
        row = cl_cons(name_str, row);
        row = cl_cons(keyword("FIELD"), row);
        break;
    }
    default:
        row = cl_cons(name_str, CL_NIL);
        row = cl_cons(keyword("NAME"), row);
        break;
    }
    CL_GC_UNPROTECT(2);
    return row;
}

/* (amilisp::%binding-table-entries package) -> list of rows (see
 * entry_row), in table (name) order; NIL for an ordinary package.  Also
 * accepts a blob directly, for round-trip tests. */
static CL_Obj bi_binding_table_entries(CL_Obj *args, int nargs)
{
    CL_Obj src = args[0], blob, result = CL_NIL;
    uint32_t n, i;
    (void)nargs;
    if (CL_BYTE_VECTOR_P(src)) {
        const char *err = validate_blob(src);
        if (err)
            cl_error(CL_ERR_ARGS, "%%BINDING-TABLE-ENTRIES: malformed binding table (%s)", err);
        blob = src;
    } else {
        CL_Obj pkg = coerce_package(src, "%BINDING-TABLE-ENTRIES");
        blob = pkg_blob(pkg);           /* NIL for ordinary AND shed */
        if (CL_NULL_P(blob)) return CL_NIL;
    }
    CL_GC_PROTECT(blob);
    CL_GC_PROTECT(result);
    {
        BtView v;
        view_blob(blob, &v);
        n = v.n;
    }
    /* back to front so the list comes out in table order without a reverse */
    for (i = n; i-- > 0; ) {
        uint8_t nbuf[256];
        BtEntry e;
        BtValue val;
        CL_Obj row;
        memset(&val, 0, sizeof(val));
        {
            BtView v;
            view_blob(blob, &v);      /* re-derive: entry_row allocates */
            decode_entry(&v, i, &e);
            memcpy(nbuf, entry_name(&v, &e), e.name_len);
            if (e.kind == CL_BT_CONST || e.kind == CL_BT_VAR)
                decode_value(&v, &e, &val);
        }
        row = entry_row(&e, nbuf, &val);
        CL_GC_PROTECT(row);
        result = cl_cons(row, result);
        CL_GC_UNPROTECT(1);
    }
    CL_GC_UNPROTECT(2);
    return result;
}

void cl_builtins_bindtab_init(void)
{
    cl_register_builtin("%MAKE-BINDING-TABLE", bi_make_binding_table, 1, 1, cl_package_amilisp);
    cl_register_builtin("%REGISTER-BINDING-TABLE", bi_register_binding_table, 4, 4, cl_package_amilisp);
    cl_register_builtin("%BINDING-TABLE-MATERIALIZE-ALL", bi_binding_table_materialize_all, 1, 1, cl_package_amilisp);
    cl_register_builtin("%SHED-BINDING-TABLES", bi_shed_binding_tables, 0, 1, cl_package_amilisp);
    cl_register_builtin("%BINDING-TABLE-INFO", bi_binding_table_info, 1, 1, cl_package_amilisp);
    cl_register_builtin("%BINDING-TABLE-ENTRIES", bi_binding_table_entries, 1, 1, cl_package_amilisp);
}
