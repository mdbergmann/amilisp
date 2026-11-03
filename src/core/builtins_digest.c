/*
 * builtins_digest.c — SHA-256 and the two constructions Matter, TLS-style
 *                     key schedules and password hashing build on it:
 *                     EXT:SHA256, EXT:HMAC-SHA256, EXT:PBKDF2-SHA256.
 *
 * Why native: Ironclad's SHA-256 is written in (unsigned-byte 32) modular
 * arithmetic, which SBCL compiles to machine words but amilisp evaluates
 * through bignums on its 32-bit tagged representation — about 0.3 MB/s on
 * the host and a PBKDF2 with 1000 iterations in 2.5 s.  The same work here
 * runs at C speed (FIPS 180-4, RFC 2104, RFC 8018), so a protocol library
 * can hash and derive keys on a 68020 within a session timeout.
 *
 * Inputs are octet vectors: a (SIMPLE-ARRAY (UNSIGNED-BYTE 8)) byte vector
 * (fill pointer respected) or a simple general vector of integers 0..255.
 * Results are fresh (UNSIGNED-BYTE 8) byte vectors.
 *
 * GC SAFETY: hashing holds raw pointers into the arena and allocates
 * nothing; every result vector is allocated only after the last input byte
 * was read.  PBKDF2's iteration loop touches no heap object at all, so it
 * runs inside a GC safe region (other threads can collect and compact
 * meanwhile) and re-derives the protected result vector afterwards.
 */

#include "builtins.h"
#include "symbol.h"
#include "package.h"
#include "mem.h"
#include "error.h"
#include "thread.h"
#include "vm.h"
#include "../platform/platform.h"
#include <string.h>

/* ---------------------------------------------------------------------
 * SHA-256 core (FIPS 180-4).  Byte-wise loads/stores keep it endian-neutral
 * for the big-endian 68k and the little-endian hosts alike.
 * ------------------------------------------------------------------- */

typedef struct {
    uint32_t h[8];
    uint32_t len_lo, len_hi;   /* total input bytes, 64-bit in two halves */
    uint32_t buf_len;          /* bytes pending in buf */
    uint8_t  buf[64];
} SHA256_Ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define BSIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define BSIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SSIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SSIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static void sha256_init(SHA256_Ctx *c)
{
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->len_lo = c->len_hi = 0;
    c->buf_len = 0;
}

static void sha256_block(SHA256_Ctx *c, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, cc, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++, p += 4)
        w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
               ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    for (i = 16; i < 64; i++)
        w[i] = SSIG1(w[i - 2]) + w[i - 7] + SSIG0(w[i - 15]) + w[i - 16];
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3];
    e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + BSIG1(e) + CH(e, f, g) + sha256_k[i] + w[i];
        t2 = BSIG0(a) + MAJ(a, b, cc);
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_update(SHA256_Ctx *c, const uint8_t *data, uint32_t len)
{
    uint32_t lo = c->len_lo + len;
    if (lo < c->len_lo) c->len_hi++;
    c->len_lo = lo;
    if (c->buf_len) {
        uint32_t take = 64 - c->buf_len;
        if (take > len) take = len;
        memcpy(c->buf + c->buf_len, data, take);
        c->buf_len += take;
        data += take;
        len -= take;
        if (c->buf_len < 64) return;
        sha256_block(c, c->buf);
        c->buf_len = 0;
    }
    while (len >= 64) {
        sha256_block(c, data);
        data += 64;
        len -= 64;
    }
    if (len) {
        memcpy(c->buf, data, len);
        c->buf_len = len;
    }
}

static void sha256_final(SHA256_Ctx *c, uint8_t out[32])
{
    uint32_t bits_hi = (c->len_hi << 3) | (c->len_lo >> 29);
    uint32_t bits_lo = c->len_lo << 3;
    uint8_t pad[72];
    uint32_t pad_len = (c->buf_len < 56) ? (56 - c->buf_len) : (120 - c->buf_len);
    int i;
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    pad[pad_len + 0] = (uint8_t)(bits_hi >> 24); pad[pad_len + 1] = (uint8_t)(bits_hi >> 16);
    pad[pad_len + 2] = (uint8_t)(bits_hi >> 8);  pad[pad_len + 3] = (uint8_t)bits_hi;
    pad[pad_len + 4] = (uint8_t)(bits_lo >> 24); pad[pad_len + 5] = (uint8_t)(bits_lo >> 16);
    pad[pad_len + 6] = (uint8_t)(bits_lo >> 8);  pad[pad_len + 7] = (uint8_t)bits_lo;
    sha256_update(c, pad, pad_len + 8);
    for (i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)c->h[i];
    }
}

/* ---------------------------------------------------------------------
 * Octet-vector input
 * ------------------------------------------------------------------- */

typedef struct {
    const uint8_t *bytes;   /* byte vector storage, or NULL */
    CL_Vector *v;           /* general vector, or NULL */
    uint32_t len;           /* active length (fill pointer honoured) */
} OctetSrc;

static void octet_source(CL_Obj obj, const char *who, const char *what, OctetSrc *s)
{
    s->bytes = NULL;
    s->v = NULL;
    if (CL_BYTE_VECTOR_P(obj)) {
        CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
        if (bv->elt_shift)
            cl_error(CL_ERR_TYPE, "%s: %s must be an octet vector - a packed "
                     "(UNSIGNED-BYTE 16)/(SIGNED-BYTE 16) vector holds 2-byte "
                     "elements, not octets", who, what);
        s->bytes = bv->data;
        s->len = cl_bytevec_active_length(bv);
        return;
    }
    if (CL_STRING_P(obj))
        cl_error(CL_ERR_TYPE, "%s: %s is a string; digests take octets - encode "
                 "it first, e.g. (map '(vector (unsigned-byte 8)) #'char-code s) "
                 "for ASCII", who, what);
    if (!CL_VECTOR_P(obj))
        cl_error(CL_ERR_TYPE, "%s: %s must be an octet vector ((unsigned-byte 8) "
                 "elements), got a non-vector", who, what);
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(obj);
        if (v->flags & (CL_VEC_FLAG_DISPLACED | CL_VEC_FLAG_MULTIDIM | CL_VEC_FLAG_STRING))
            cl_error(CL_ERR_TYPE, "%s: %s must be a simple (non-displaced) octet "
                     "vector", who, what);
        s->v = v;
        s->len = (v->fill_pointer != CL_NO_FILL_POINTER) ? v->fill_pointer : v->length;
    }
}

/* Feed s[start..end) into the hash.  A general vector's elements are
 * checked as they go, so the error names the first offending index. */
static void sha256_update_src(SHA256_Ctx *c, const OctetSrc *s, uint32_t start,
                              uint32_t end, const char *who, const char *what)
{
    if (s->bytes) {
        sha256_update(c, s->bytes + start, end - start);
    } else {
        uint8_t chunk[64];
        uint32_t i = start, k = 0;
        for (; i < end; i++) {
            CL_Obj e = s->v->data[i];
            if (!CL_FIXNUM_P(e) || CL_FIXNUM_VAL(e) < 0 || CL_FIXNUM_VAL(e) > 255)
                cl_error(CL_ERR_TYPE, "%s: %s element %u is not an octet (0-255)",
                         who, what, (unsigned)i);
            chunk[k++] = (uint8_t)CL_FIXNUM_VAL(e);
            if (k == 64) { sha256_update(c, chunk, 64); k = 0; }
        }
        if (k) sha256_update(c, chunk, k);
    }
}

/* Bounding START/END (NIL = length) against an active length; the error
 * carries the numbers so a caller sees which side is off. */
static void octet_range(CL_Obj start_obj, CL_Obj end_obj, uint32_t len,
                        const char *who, uint32_t *start, uint32_t *end)
{
    if (!CL_FIXNUM_P(start_obj) || CL_FIXNUM_VAL(start_obj) < 0)
        cl_error(CL_ERR_TYPE, "%s: START must be a non-negative integer", who);
    *start = (uint32_t)CL_FIXNUM_VAL(start_obj);
    if (CL_NULL_P(end_obj)) {
        *end = len;
    } else {
        if (!CL_FIXNUM_P(end_obj) || CL_FIXNUM_VAL(end_obj) < 0)
            cl_error(CL_ERR_TYPE, "%s: END must be NIL or a non-negative integer", who);
        *end = (uint32_t)CL_FIXNUM_VAL(end_obj);
    }
    if (*end > len)
        cl_error(CL_ERR_GENERAL, "%s: END %u exceeds the vector's length %u",
                 who, (unsigned)*end, (unsigned)len);
    if (*start > *end)
        cl_error(CL_ERR_GENERAL, "%s: START %u exceeds END %u",
                 who, (unsigned)*start, (unsigned)*end);
}

static CL_Obj make_digest_result(const uint8_t *bytes, uint32_t len)
{
    CL_Obj obj = cl_make_byte_vector(len, 0, 0);
    CL_ByteVector *bv = (CL_ByteVector *)CL_OBJ_TO_PTR(obj);
    memcpy(bv->data, bytes, len);
    return obj;
}

/* ---------------------------------------------------------------------
 * HMAC-SHA256 (RFC 2104): the two keyed contexts are prepared once, then
 * copied per message — PBKDF2 reuses that for its inner loop.
 * ------------------------------------------------------------------- */

typedef struct {
    SHA256_Ctx inner;   /* after absorbing K xor ipad */
    SHA256_Ctx outer;   /* after absorbing K xor opad */
} HMAC256_Key;

static void hmac256_key_init(HMAC256_Key *k, const OctetSrc *key,
                             const char *who, const char *what)
{
    uint8_t block[64];
    int i;
    memset(block, 0, sizeof(block));
    if (key->len > 64) {
        SHA256_Ctx c;
        sha256_init(&c);
        sha256_update_src(&c, key, 0, key->len, who, what);
        sha256_final(&c, block);          /* 32 bytes, rest stays zero */
    } else if (key->bytes) {
        memcpy(block, key->bytes, key->len);
    } else {
        uint32_t j;
        for (j = 0; j < key->len; j++) {
            CL_Obj e = key->v->data[j];
            if (!CL_FIXNUM_P(e) || CL_FIXNUM_VAL(e) < 0 || CL_FIXNUM_VAL(e) > 255)
                cl_error(CL_ERR_TYPE, "%s: %s element %u is not an octet (0-255)",
                         who, what, (unsigned)j);
            block[j] = (uint8_t)CL_FIXNUM_VAL(e);
        }
    }
    sha256_init(&k->inner);
    sha256_init(&k->outer);
    for (i = 0; i < 64; i++) block[i] ^= 0x36;
    sha256_update(&k->inner, block, 64);
    for (i = 0; i < 64; i++) block[i] ^= 0x36 ^ 0x5c;
    sha256_update(&k->outer, block, 64);
    memset(block, 0, sizeof(block));
}

/* Finish an inner context (already fed with the message) into MAC. */
static void hmac256_finish(const HMAC256_Key *k, SHA256_Ctx *inner, uint8_t mac[32])
{
    SHA256_Ctx outer = k->outer;
    sha256_final(inner, mac);
    sha256_update(&outer, mac, 32);
    sha256_final(&outer, mac);
}

/* ---------------------------------------------------------------------
 * Builtins
 * ------------------------------------------------------------------- */

/* (ext:%sha256 octets start end) — positional core of (ext:sha256 octets
 * &key start end) in boot.lisp. */
static CL_Obj bi_sha256(CL_Obj *args, int n)
{
    static const char *who = "EXT:SHA256";
    OctetSrc src;
    SHA256_Ctx c;
    uint8_t out[32];
    uint32_t start, end;
    CL_UNUSED(n);
    octet_source(args[0], who, "OCTETS", &src);
    octet_range(args[1], args[2], src.len, who, &start, &end);
    sha256_init(&c);
    sha256_update_src(&c, &src, start, end, who, "OCTETS");
    sha256_final(&c, out);
    return make_digest_result(out, 32);
}

/* (ext:hmac-sha256 key data) */
static CL_Obj bi_hmac_sha256(CL_Obj *args, int n)
{
    static const char *who = "EXT:HMAC-SHA256";
    OctetSrc key, data;
    HMAC256_Key k;
    SHA256_Ctx inner;
    uint8_t mac[32];
    CL_UNUSED(n);
    octet_source(args[0], who, "KEY", &key);
    octet_source(args[1], who, "DATA", &data);
    hmac256_key_init(&k, &key, who, "KEY");
    inner = k.inner;
    sha256_update_src(&inner, &data, 0, data.len, who, "DATA");
    hmac256_finish(&k, &inner, mac);
    memset(&k, 0, sizeof(k));
    return make_digest_result(mac, 32);
}

/* Iterations per GC-safe-region stint before we come up for air (a Ctrl-C
 * poll, and a GC requested by another thread resumes us compacted). */
#define PBKDF2_STINT 1024

/* (ext:pbkdf2-sha256 password salt iterations length) — RFC 8018 §5.2 with
 * HMAC-SHA256 as the PRF.  The salt is absorbed once into a keyed inner
 * context; each block copies that and appends its big-endian index. */
static CL_Obj bi_pbkdf2_sha256(CL_Obj *args, int n)
{
    static const char *who = "EXT:PBKDF2-SHA256";
    OctetSrc password, salt;
    HMAC256_Key k;
    SHA256_Ctx salted;
    CL_Obj result;
    uint32_t iterations, length, nblocks, block, written = 0;
    CL_UNUSED(n);
    octet_source(args[0], who, "PASSWORD", &password);
    octet_source(args[1], who, "SALT", &salt);
    if (!CL_FIXNUM_P(args[2]) || CL_FIXNUM_VAL(args[2]) < 1)
        cl_error(CL_ERR_TYPE, "%s: ITERATIONS must be a positive integer", who);
    if (!CL_FIXNUM_P(args[3]) || CL_FIXNUM_VAL(args[3]) < 1)
        cl_error(CL_ERR_TYPE, "%s: LENGTH must be a positive integer (octets)", who);
    iterations = (uint32_t)CL_FIXNUM_VAL(args[2]);
    length = (uint32_t)CL_FIXNUM_VAL(args[3]);
    hmac256_key_init(&k, &password, who, "PASSWORD");
    salted = k.inner;
    sha256_update_src(&salted, &salt, 0, salt.len, who, "SALT");
    /* Every heap read is done; from here on only stack state and RESULT. */
    result = cl_make_byte_vector(length, 0, 0);
    CL_GC_PROTECT(result);
    nblocks = (length + 31) / 32;
    for (block = 1; block <= nblocks; block++) {
        SHA256_Ctx inner = salted;
        uint8_t idx[4], u[32], t[32];
        uint32_t i = 1, take;
        idx[0] = (uint8_t)(block >> 24); idx[1] = (uint8_t)(block >> 16);
        idx[2] = (uint8_t)(block >> 8);  idx[3] = (uint8_t)block;
        cl_gc_enter_safe_region();
        sha256_update(&inner, idx, 4);
        hmac256_finish(&k, &inner, u);
        memcpy(t, u, 32);
        while (i < iterations) {
            uint32_t stint = iterations - i;
            if (stint > PBKDF2_STINT) stint = PBKDF2_STINT;
            i += stint;
            while (stint--) {
                int j;
                inner = k.inner;
                sha256_update(&inner, u, 32);
                hmac256_finish(&k, &inner, u);
                for (j = 0; j < 32; j++) t[j] ^= u[j];
            }
            if (i < iterations) {
                cl_gc_leave_safe_region();
                cl_vm_poll_break();
                cl_gc_enter_safe_region();
            }
        }
        cl_gc_leave_safe_region();
        /* GC SAFETY: a collection during the stint may have moved RESULT;
         * the protected root was forwarded, the pointer is derived now. */
        take = length - written;
        if (take > 32) take = 32;
        memcpy(((CL_ByteVector *)CL_OBJ_TO_PTR(result))->data + written, t, take);
        written += take;
    }
    CL_GC_UNPROTECT(1);
    memset(&k, 0, sizeof(k));
    memset(&salted, 0, sizeof(salted));
    return result;
}

static void extfun(const char *name, CL_CFunc func, int min, int max)
{
    cl_register_builtin_exported(name, func, min, max, cl_package_ext);
}

void cl_builtins_digest_init(void)
{
    extfun("%SHA256", bi_sha256, 3, 3);
    extfun("HMAC-SHA256", bi_hmac_sha256, 2, 2);
    extfun("PBKDF2-SHA256", bi_pbkdf2_sha256, 4, 4);
}
