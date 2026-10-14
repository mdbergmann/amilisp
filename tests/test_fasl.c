#include "test.h"
#include "jit/jit.h"   /* CL_JIT_NATIVE */
#include "core/types.h"
#include "core/mem.h"
#include "core/error.h"
#include "core/package.h"
#include "core/symbol.h"
#include "core/reader.h"
#include "core/printer.h"
#include "core/compiler.h"
#include "core/vm.h"
#include "core/builtins.h"
#include "core/repl.h"
#include "core/fasl.h"
#include "core/float.h"
#include "core/opcodes.h"
#include "core/thread.h"
#include "platform/platform.h"
#include <string.h>
#include <math.h>

/* Helper: eval a string, return the CL_Obj */
static CL_Obj eval_obj(const char *str)
{
    return cl_eval_string(str);
}

/* Helper: eval a string, return printed result */
static const char *eval_print(const char *str)
{
    static char buf[512];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj result = cl_eval_string(str);
        cl_prin1_to_string(result, buf, sizeof(buf));
        CL_UNCATCH();
        return buf;
    } else {
        CL_UNCATCH();
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        return buf;
    }
}

/* Helper: build a minimal CL_Bytecode that pushes constant[0] and returns.
 * code: OP_CONST 0x00 0x00 OP_RET */
static CL_Obj make_simple_bytecode(CL_Obj constant)
{
    CL_Bytecode *bc = (CL_Bytecode *)cl_alloc(TYPE_BYTECODE, sizeof(CL_Bytecode));
    if (!bc) return CL_NIL;

    bc->code = (uint8_t *)platform_alloc(4);
    bc->code[0] = OP_CONST;
    bc->code[1] = 0x00;  /* high byte of constant index */
    bc->code[2] = 0x00;  /* low byte of constant index */
    bc->code[3] = OP_RET;
    bc->code_len = 4;

    bc->constants = (CL_Obj *)platform_alloc(sizeof(CL_Obj));
    bc->constants[0] = constant;
    bc->n_constants = 1;

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
    bc->source_line = 42;
    bc->source_file = NULL;

    return CL_PTR_TO_OBJ(bc);
}

/* Helper: build a bytecode with source file and line map */
static CL_Obj make_bytecode_with_source(CL_Obj constant, const char *file)
{
    CL_Bytecode *bc;
    CL_Obj obj = make_simple_bytecode(constant);
    if (CL_NULL_P(obj)) return CL_NIL;
    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(obj);

    /* Set source file */
    if (file) {
        char *sf = (char *)platform_alloc(strlen(file) + 1);
        strcpy(sf, file);
        bc->source_file = sf;
    }

    /* Add a line map */
    bc->line_map = (CL_LineEntry *)platform_alloc(2 * sizeof(CL_LineEntry));
    bc->line_map[0].pc = 0;
    bc->line_map[0].line = 10;
    bc->line_map[1].pc = 3;
    bc->line_map[1].line = 11;
    bc->line_map_count = 2;

    return obj;
}

/* Helper: build a bytecode with keyword params */
static CL_Obj make_bytecode_with_keys(void)
{
    CL_Bytecode *bc;
    CL_Obj obj = make_simple_bytecode(CL_MAKE_FIXNUM(0));
    if (CL_NULL_P(obj)) return CL_NIL;
    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(obj);

    bc->arity = 0;
    bc->n_locals = 4;
    bc->flags = 1;  /* has_key */
    bc->n_keys = 2;
    bc->key_syms = (CL_Obj *)platform_alloc(2 * sizeof(CL_Obj));
    bc->key_syms[0] = cl_intern_in("X", 1, cl_find_package("KEYWORD", 7));
    bc->key_syms[1] = cl_intern_in("Y", 1, cl_find_package("KEYWORD", 7));
    bc->key_slots = (uint8_t *)platform_alloc(2);
    bc->key_slots[0] = 0;
    bc->key_slots[1] = 1;
    bc->key_suppliedp_slots = (uint8_t *)platform_alloc(2);
    bc->key_suppliedp_slots[0] = 2;
    bc->key_suppliedp_slots[1] = 0xFF;

    return obj;
}

/* ================================================================
 * Writer/Reader primitive tests
 * ================================================================ */

TEST(write_read_u8)
{
    uint8_t buf[16];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_u8(&w, 0x00);
    cl_fasl_write_u8(&w, 0xFF);
    cl_fasl_write_u8(&w, 0x42);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(w.pos, 3);

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(cl_fasl_read_u8(&r), 0x00);
    ASSERT_EQ_INT(cl_fasl_read_u8(&r), 0xFF);
    ASSERT_EQ_INT(cl_fasl_read_u8(&r), 0x42);
    ASSERT_EQ_INT(r.error, FASL_OK);
}

TEST(write_read_u16)
{
    uint8_t buf[16];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_u16(&w, 0x0000);
    cl_fasl_write_u16(&w, 0xFFFF);
    cl_fasl_write_u16(&w, 0x1234);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(w.pos, 6);

    /* Verify big-endian byte order */
    ASSERT_EQ_INT(buf[4], 0x12);
    ASSERT_EQ_INT(buf[5], 0x34);

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(cl_fasl_read_u16(&r), 0x0000);
    ASSERT_EQ_INT(cl_fasl_read_u16(&r), 0xFFFF);
    ASSERT_EQ_INT(cl_fasl_read_u16(&r), 0x1234);
    ASSERT_EQ_INT(r.error, FASL_OK);
}

TEST(write_read_u32)
{
    uint8_t buf[16];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_u32(&w, 0x00000000);
    cl_fasl_write_u32(&w, 0xDEADBEEF);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(w.pos, 8);

    /* Verify big-endian byte order */
    ASSERT_EQ_INT(buf[4], 0xDE);
    ASSERT_EQ_INT(buf[5], 0xAD);
    ASSERT_EQ_INT(buf[6], 0xBE);
    ASSERT_EQ_INT(buf[7], 0xEF);

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT(cl_fasl_read_u32(&r) == 0x00000000);
    ASSERT(cl_fasl_read_u32(&r) == 0xDEADBEEF);
    ASSERT_EQ_INT(r.error, FASL_OK);
}

TEST(write_read_bytes)
{
    uint8_t buf[32];
    CL_FaslWriter w;
    CL_FaslReader r;
    uint8_t out[5];
    const char *data = "hello";

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_bytes(&w, data, 5);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(w.pos, 5);

    cl_fasl_reader_init(&r, buf, w.pos);
    cl_fasl_read_bytes(&r, out, 5);
    ASSERT_EQ_INT(r.error, FASL_OK);
    ASSERT(memcmp(out, data, 5) == 0);
}

/* ================================================================
 * Overflow/truncation error tests
 * ================================================================ */

TEST(writer_overflow)
{
    uint8_t buf[2];
    CL_FaslWriter w;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_u32(&w, 0x12345678);
    ASSERT_EQ_INT(w.error, FASL_ERR_OVERFLOW);
}

TEST(reader_truncated)
{
    uint8_t buf[2] = {0x12, 0x34};
    CL_FaslReader r;

    cl_fasl_reader_init(&r, buf, 2);
    cl_fasl_read_u32(&r);
    ASSERT_EQ_INT(r.error, FASL_ERR_TRUNCATED);
}

TEST(reader_truncated_u8)
{
    CL_FaslReader r;
    cl_fasl_reader_init(&r, NULL, 0);
    cl_fasl_read_u8(&r);
    ASSERT_EQ_INT(r.error, FASL_ERR_TRUNCATED);
}

TEST(reader_truncated_u16)
{
    uint8_t buf[1] = {0x12};
    CL_FaslReader r;
    cl_fasl_reader_init(&r, buf, 1);
    cl_fasl_read_u16(&r);
    ASSERT_EQ_INT(r.error, FASL_ERR_TRUNCATED);
}

TEST(reader_truncated_bytes)
{
    uint8_t buf[2] = {0x12, 0x34};
    uint8_t out[4];
    CL_FaslReader r;
    cl_fasl_reader_init(&r, buf, 2);
    cl_fasl_read_bytes(&r, out, 4);
    ASSERT_EQ_INT(r.error, FASL_ERR_TRUNCATED);
}

TEST(error_propagation_writer)
{
    uint8_t buf[2];
    CL_FaslWriter w;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_u32(&w, 0x12345678);  /* overflow */
    ASSERT_EQ_INT(w.error, FASL_ERR_OVERFLOW);

    /* Additional writes are no-ops */
    cl_fasl_write_u8(&w, 0x42);
    ASSERT_EQ_INT(w.error, FASL_ERR_OVERFLOW);
    ASSERT_EQ_INT(w.pos, 0);
}

TEST(error_propagation_reader)
{
    uint8_t buf[1] = {0x12};
    CL_FaslReader r;

    cl_fasl_reader_init(&r, buf, 1);
    cl_fasl_read_u32(&r);  /* truncated */
    ASSERT_EQ_INT(r.error, FASL_ERR_TRUNCATED);

    ASSERT_EQ_INT(cl_fasl_read_u8(&r), 0);
    ASSERT_EQ_INT(r.error, FASL_ERR_TRUNCATED);
}

/* ================================================================
 * Header tests
 * ================================================================ */

TEST(write_read_header)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;
    uint32_t n_units;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_header(&w, 5);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(w.pos, 12);

    cl_fasl_reader_init(&r, buf, w.pos);
    n_units = cl_fasl_read_header(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);
    ASSERT_EQ_INT(n_units, 5);
}

TEST(header_bad_magic)
{
    uint8_t buf[12] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1};
    CL_FaslReader r;
    cl_fasl_reader_init(&r, buf, 12);
    cl_fasl_read_header(&r);
    ASSERT_EQ_INT(r.error, FASL_ERR_BAD_MAGIC);
}

TEST(header_bad_version)
{
    uint8_t buf[12];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_u32(&w, CL_FASL_MAGIC);
    cl_fasl_write_u16(&w, 99);  /* bad version */
    cl_fasl_write_u16(&w, 0);
    cl_fasl_write_u32(&w, 0);

    cl_fasl_reader_init(&r, buf, 12);
    cl_fasl_read_header(&r);
    ASSERT_EQ_INT(r.error, FASL_ERR_BAD_VERSION);
}

TEST(header_zero_units)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_header(&w, 0);

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(cl_fasl_read_header(&r), 0);
    ASSERT_EQ_INT(r.error, FASL_OK);
}

/* ================================================================
 * Object serialization round-trip tests
 * ================================================================ */

TEST(serialize_nil)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_NIL);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(w.pos, 1);

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT(CL_NULL_P(cl_fasl_deserialize_obj(&r)));
    ASSERT_EQ_INT(r.error, FASL_OK);
}

TEST(serialize_t)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_T);

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT(cl_fasl_deserialize_obj(&r) == CL_T);
    ASSERT_EQ_INT(r.error, FASL_OK);
}

TEST(serialize_unbound)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_UNBOUND);

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT(cl_fasl_deserialize_obj(&r) == CL_UNBOUND);
    ASSERT_EQ_INT(r.error, FASL_OK);
}

TEST(serialize_fixnum_zero)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj result;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_MAKE_FIXNUM(0));

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_FIXNUM_P(result));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(result), 0);
}

TEST(serialize_fixnum_positive)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_MAKE_FIXNUM(42));

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_fasl_deserialize_obj(&r)), 42);
}

TEST(serialize_fixnum_negative)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_MAKE_FIXNUM(-99));

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_fasl_deserialize_obj(&r)), -99);
}

TEST(serialize_fixnum_max)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_MAKE_FIXNUM(CL_FIXNUM_MAX));

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_fasl_deserialize_obj(&r)), CL_FIXNUM_MAX);
}

TEST(serialize_fixnum_min)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_MAKE_FIXNUM(CL_FIXNUM_MIN));

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_fasl_deserialize_obj(&r)), CL_FIXNUM_MIN);
}

TEST(serialize_character)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj result;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_MAKE_CHAR('A'));

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_CHAR_P(result));
    ASSERT_EQ_INT(CL_CHAR_VAL(result), 'A');
}

TEST(serialize_character_newline)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_MAKE_CHAR('\n'));

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(CL_CHAR_VAL(cl_fasl_deserialize_obj(&r)), '\n');
}

TEST(serialize_string_empty)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj str, result;
    CL_String *s;

    str = cl_make_string("", 0);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, str);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_STRING_P(result));
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(s->length, 0);
}

TEST(serialize_string)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj str, result;
    CL_String *s;

    str = cl_make_string("hello world", 11);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, str);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_STRING_P(result));
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(s->length, 11);
    ASSERT_STR_EQ(s->data, "hello world");
}

TEST(serialize_long_string)
{
    uint8_t buf[2048];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj str, result;
    CL_String *s;
    char long_str[513];
    int i;

    for (i = 0; i < 512; i++) long_str[i] = 'A' + (i % 26);
    long_str[512] = '\0';

    str = cl_make_string(long_str, 512);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, str);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    s = (CL_String *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(s->length, 512);
    ASSERT(memcmp(s->data, long_str, 512) == 0);
}

#ifdef CL_WIDE_STRINGS
/* Host (wide-char) builds emit FASLs that the m68k Amiga binary (built
 * without CL_WIDE_STRINGS) must be able to read.  An ASCII-only wide string
 * must therefore serialize as the byte-form FASL_TAG_STRING, not the
 * wide-only FASL_TAG_WIDE_STRING.  Non-ASCII wide strings keep WIDE_STRING. */
TEST(serialize_wide_string_ascii_downgrades)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    uint32_t chars[5] = { 'h', 'e', 'l', 'l', 'o' };
    CL_Obj ws, result;

    ws = cl_make_wide_string(chars, 5);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, ws);

    /* First byte after the (optional) shared-object framing must be the
     * STRING tag — this is what makes the FASL portable. */
    ASSERT_EQ_INT(buf[0], FASL_TAG_OBJ_DEF);   /* u16 id follows (v36) */
    ASSERT_EQ_INT(buf[3], FASL_TAG_STRING);

    /* And a byte-only reader (the Amiga case) must be able to round-trip. */
    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_STRING_P(result) || CL_WIDE_STRING_P(result));
}

TEST(serialize_wide_string_nonascii_stays_wide)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    uint32_t chars[3] = { 'a', 0x00E9 /* é */, 'b' };
    CL_Obj ws;

    ws = cl_make_wide_string(chars, 3);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, ws);

    /* Non-ASCII codepoint forces WIDE_STRING. */
    ASSERT_EQ_INT(buf[0], FASL_TAG_OBJ_DEF);   /* u16 id follows (v36) */
    ASSERT_EQ_INT(buf[3], FASL_TAG_WIDE_STRING);
}

/* CLAMIGA_FASL_PORTABLE=1 (cl_fasl_set_portable_mode): a string that would
 * need TAG_WIDE_STRING is REFUSED with FASL_ERR_NONPORTABLE, and the detail
 * names the offending code point and the ASCII prefix, so the compile-file
 * diagnostic on the host points at the literal instead of the Amiga dying
 * with BAD_TAG when it loads the lib FASL.  ASCII-only wide strings still
 * downgrade; with the mode off the writer behaves as before. */
TEST(serialize_wide_string_nonascii_portable_refused)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    uint32_t chars[4] = { 'a', 'b', 0x2014 /* em dash */, 'c' };
    uint32_t ascii[2] = { 'o', 'k' };
    CL_Obj ws;

    cl_fasl_set_portable_mode(1);

    ws = cl_make_wide_string(chars, 4);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, ws);
    ASSERT_EQ_INT(w.error, FASL_ERR_NONPORTABLE);
    ASSERT(strstr(cl_fasl_nonportable_detail(&w), "U+2014") != NULL);
    ASSERT(strstr(cl_fasl_nonportable_detail(&w), "\"ab?c\"") != NULL);
    ASSERT(strstr(cl_fasl_nonportable_detail(&w), "CLAMIGA_FASL_PORTABLE") != NULL);

    /* ASCII-only wide strings are unaffected (downgraded to TAG_STRING). */
    ws = cl_make_wide_string(ascii, 2);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, ws);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(buf[0], FASL_TAG_OBJ_DEF);   /* u16 id follows (v36) */
    ASSERT_EQ_INT(buf[3], FASL_TAG_STRING);

    /* Mode off: the non-ASCII string is written as WIDE_STRING as before. */
    cl_fasl_set_portable_mode(0);
    ws = cl_make_wide_string(chars, 4);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, ws);
    ASSERT_EQ_INT(w.error, FASL_OK);
    ASSERT_EQ_INT(buf[0], FASL_TAG_OBJ_DEF);   /* u16 id follows (v36) */
    ASSERT_EQ_INT(buf[3], FASL_TAG_WIDE_STRING);
}
#endif

TEST(serialize_symbol_cl)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj sym, result;

    sym = cl_intern_in("CAR", 3, cl_package_cl);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, sym);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_SYMBOL_P(result));
    ASSERT(result == sym);
}

TEST(serialize_symbol_keyword)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj sym, result;

    sym = cl_intern_in("TEST", 4, cl_find_package("KEYWORD", 7));
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, sym);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(result == sym);
}

TEST(serialize_symbol_user)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj sym, result;

    sym = cl_intern_in("MY-VAR", 6, cl_package_cl_user);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, sym);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(result == sym);
}

TEST(serialize_cons)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj pair, result;

    pair = cl_cons(CL_MAKE_FIXNUM(1), CL_MAKE_FIXNUM(2));
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, pair);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_CONS_P(result));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(result)), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(result)), 2);
}

TEST(serialize_list)
{
    uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj list, result, cur;

    list = cl_cons(CL_MAKE_FIXNUM(3), CL_NIL);
    CL_GC_PROTECT(list);
    list = cl_cons(CL_MAKE_FIXNUM(2), list);
    list = cl_cons(CL_MAKE_FIXNUM(1), list);
    CL_GC_UNPROTECT(1);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, list);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);

    cur = result;
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cur)), 1); cur = cl_cdr(cur);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cur)), 2); cur = cl_cdr(cur);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cur)), 3); cur = cl_cdr(cur);
    ASSERT(CL_NULL_P(cur));
}

TEST(serialize_dotted_list)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj list, result;

    /* (1 2 . 3) */
    {
        CL_Obj tail = cl_cons(CL_MAKE_FIXNUM(2), CL_MAKE_FIXNUM(3));
        CL_GC_PROTECT(tail);
        list = cl_cons(CL_MAKE_FIXNUM(1), tail);
        CL_GC_UNPROTECT(1);
    }

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, list);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);

    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(result)), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(result))), 2);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(cl_cdr(result))), 3);
}

TEST(serialize_nested_cons)
{
    uint8_t buf[1024];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj nested, result;

    /* ((1 . 2) . (3 . 4)) */
    {
        CL_Obj a = cl_cons(CL_MAKE_FIXNUM(1), CL_MAKE_FIXNUM(2));
        CL_GC_PROTECT(a);
        CL_Obj b = cl_cons(CL_MAKE_FIXNUM(3), CL_MAKE_FIXNUM(4));
        CL_GC_PROTECT(b);
        nested = cl_cons(a, b);
        CL_GC_UNPROTECT(2);
    }

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, nested);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);

    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_car(result))), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(cl_car(result))), 2);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(result))), 3);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_cdr(cl_cdr(result))), 4);
}

/* Regression: a circular list constant — `#1=(1 2 3 . #1#)` — must
 * serialize without the inline CDR walk looping back over the cycle
 * forever (which filled the unit buffer until it overflowed the 64 MB
 * cap and forced a slow source-load fallback; see serapeum's copy-firstn
 * test).  The writer now emits an OBJ_REF when the spine loops back to an
 * already-serialized cons, and the reader reconstructs the cycle. */
TEST(serialize_circular_list)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj c0, c1, c2, result, cur;

    /* Build #1=(1 2 3 . #1#): c0=(1 . c1), c1=(2 . c2), c2=(3 . c0). */
    c2 = cl_cons(CL_MAKE_FIXNUM(3), CL_NIL); CL_GC_PROTECT(c2);
    c1 = cl_cons(CL_MAKE_FIXNUM(2), c2);     CL_GC_PROTECT(c1);
    c0 = cl_cons(CL_MAKE_FIXNUM(1), c1);     CL_GC_PROTECT(c0);
    /* Close the cycle: cdr of the last cell points back to the head. */
    ((CL_Cons *)CL_OBJ_TO_PTR(c2))->cdr = c0;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, c0);
    /* The whole cycle must fit easily — no overflow, terminated by OBJ_REF. */
    ASSERT_EQ_INT(w.error, FASL_OK);
    CL_GC_UNPROTECT(3);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    /* Walk 1 2 3 then verify the 4th cdr is EQ the head (cycle closed). */
    cur = result;
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cur)), 1); cur = cl_cdr(cur);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cur)), 2); cur = cl_cdr(cur);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cur)), 3); cur = cl_cdr(cur);
    ASSERT(cur == result);
    /* And one more lap stays consistent. */
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(cur))), 2);
}

/* Regression: a sublist that appears both as a list element (reached as a
 * CAR, so registered by the OBJ_DEF/REF wrapper) and again as a CDR tail
 * must round-trip as the SAME object — the CDR walk resolves the second
 * occurrence to an OBJ_REF rather than re-emitting it.  Covers the cycle
 * guard's lookup hitting a wrapper-registered cons that is not the spine
 * head. */
TEST(serialize_shared_sublist)
{
    uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj shared, lst, result;

    /* lst = (#1=(9 9) 1 . #1#): element 0 is `shared`, and the final cdr
     * is `shared` again. */
    shared = cl_cons(CL_MAKE_FIXNUM(9),
                     cl_cons(CL_MAKE_FIXNUM(9), CL_NIL));
    CL_GC_PROTECT(shared);
    lst = cl_cons(shared, cl_cons(CL_MAKE_FIXNUM(1), shared));
    CL_GC_PROTECT(lst);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, lst);
    ASSERT_EQ_INT(w.error, FASL_OK);
    CL_GC_UNPROTECT(2);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    /* car == (9 9); cddr must be EQ that same object. */
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_car(result))), 9);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(result))), 1);
    ASSERT(cl_cdr(cl_cdr(result)) == cl_car(result));
}

TEST(serialize_deeply_nested_list)
{
    uint8_t buf[4096];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj list, result;
    int i, depth = 20;

    list = cl_cons(CL_MAKE_FIXNUM(42), CL_NIL);
    CL_GC_PROTECT(list);
    for (i = 1; i < depth; i++)
        list = cl_cons(list, CL_NIL);
    CL_GC_UNPROTECT(1);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, list);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);

    for (i = 1; i < depth; i++) {
        ASSERT(CL_CONS_P(result));
        result = cl_car(result);
    }
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(result)), 42);
}

/* Regression for the iterative-serializer rewrite: the old recursive
 * serializer overflowed the C stack at ~16k depth (per-frame ~150-300B at
 * -O3), tripping a stack-protector canary in the caller before its own
 * epilogue ran.  50k deep here is well past the prior FASL_MAX_DEPTH 4096
 * cap and verifies that no depth limit is enforced by the serializer.
 *
 * Note: the deserializer is still recursive and protects 3 GC roots per
 * CONS frame, so deep-CAR-chain round-trip is bounded by CL_GC_ROOT_STACK_SIZE
 * (currently 1024).  This test only verifies serialization-side handling. */
TEST(serialize_deep_car_chain_no_overflow)
{
    const uint32_t depth = 50000;
    uint32_t buf_size = depth * 8 + 4096;
    uint8_t *buf = (uint8_t *)platform_alloc(buf_size);
    CL_FaslWriter w;
    CL_Obj list;
    uint32_t i;

    ASSERT(buf != NULL);

    list = cl_cons(CL_MAKE_FIXNUM(42), CL_NIL);
    CL_GC_PROTECT(list);
    for (i = 1; i < depth; i++)
        list = cl_cons(list, CL_NIL);

    cl_fasl_writer_init(&w, buf, buf_size);
    cl_fasl_serialize_obj(&w, list);
    ASSERT_EQ_INT(w.error, FASL_OK);
    /* Each level emits FASL_TAG_CONS (1B); innermost emits FIXNUM (1+4B)
     * and the trailing NILs (1B each).  Just verify a sane lower bound. */
    ASSERT(w.pos > depth);

    CL_GC_UNPROTECT(1);
    platform_free(buf);
}

/* Long proper list: stresses the CONS frame's CDR-as-child path.  Peak
 * worklist depth stays O(1) (cons frame + at most one pending cdr + one
 * car), regardless of list length.  Round-trips because the deserializer's
 * CONS spine is iterative on CDR. */
TEST(serialize_long_proper_list_50k)
{
    const uint32_t len = 50000;
    uint32_t buf_size = len * 8 + 4096;
    uint8_t *buf = (uint8_t *)platform_alloc(buf_size);
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj list, result, p;
    uint32_t i;

    ASSERT(buf != NULL);

    list = CL_NIL;
    CL_GC_PROTECT(list);
    for (i = 0; i < len; i++)
        list = cl_cons(CL_MAKE_FIXNUM(i & 0xFFFF), list);

    cl_fasl_writer_init(&w, buf, buf_size);
    cl_fasl_serialize_obj(&w, list);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    p = result;
    for (i = 0; i < len; i++) {
        ASSERT(CL_CONS_P(p));
        ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(p)), (int)((len - 1 - i) & 0xFFFF));
        p = cl_cdr(p);
    }
    ASSERT(CL_NULL_P(p));

    CL_GC_UNPROTECT(1);
    platform_free(buf);
}

/* Deep nested vectors: each level is a 1-element vector pointing to the
 * next.  Exercises the VECTOR case's reverse-push + PHASE_DONE resumption
 * across many levels — serializer side only (deserializer's per-frame GC
 * protection limits round-trip depth). */
TEST(serialize_deep_vector_chain_no_overflow)
{
    const uint32_t depth = 20000;
    uint32_t buf_size = depth * 16 + 4096;
    uint8_t *buf = (uint8_t *)platform_alloc(buf_size);
    CL_FaslWriter w;
    CL_Obj cur;
    uint32_t i;

    ASSERT(buf != NULL);

    cur = cl_make_vector(1);
    CL_GC_PROTECT(cur);
    {
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(cur);
        v->data[0] = CL_MAKE_FIXNUM(99);
    }
    for (i = 1; i < depth; i++) {
        CL_Obj outer = cl_make_vector(1);
        CL_Vector *v = (CL_Vector *)CL_OBJ_TO_PTR(outer);
        v->data[0] = cur;
        cur = outer;
    }

    cl_fasl_writer_init(&w, buf, buf_size);
    cl_fasl_serialize_obj(&w, cur);
    ASSERT_EQ_INT(w.error, FASL_OK);

    CL_GC_UNPROTECT(1);
    platform_free(buf);
}

TEST(serialize_single_float)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj flt, result;
    CL_SingleFloat *sf;

    flt = cl_make_single_float(3.14f);
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, flt);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_SINGLE_FLOAT_P(result));
    sf = (CL_SingleFloat *)CL_OBJ_TO_PTR(result);
    ASSERT(fabsf(sf->value - 3.14f) < 0.001f);
}

TEST(serialize_single_float_zero)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_SingleFloat *sf;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, cl_make_single_float(0.0f));

    cl_fasl_reader_init(&r, buf, w.pos);
    sf = (CL_SingleFloat *)CL_OBJ_TO_PTR(cl_fasl_deserialize_obj(&r));
    ASSERT(sf->value == 0.0f);
}

TEST(serialize_single_float_negative)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_SingleFloat *sf;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, cl_make_single_float(-1.5f));

    cl_fasl_reader_init(&r, buf, w.pos);
    sf = (CL_SingleFloat *)CL_OBJ_TO_PTR(cl_fasl_deserialize_obj(&r));
    ASSERT(sf->value == -1.5f);
}

TEST(serialize_double_float)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_DoubleFloat *df;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, cl_make_double_float(2.718281828459045));

    cl_fasl_reader_init(&r, buf, w.pos);
    df = (CL_DoubleFloat *)CL_OBJ_TO_PTR(cl_fasl_deserialize_obj(&r));
    ASSERT(fabs(df->value - 2.718281828459045) < 1e-15);
}

TEST(serialize_double_float_zero)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_DoubleFloat *df;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, cl_make_double_float(0.0));

    cl_fasl_reader_init(&r, buf, w.pos);
    df = (CL_DoubleFloat *)CL_OBJ_TO_PTR(cl_fasl_deserialize_obj(&r));
    ASSERT(df->value == 0.0);
}

TEST(serialize_double_float_negative)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_DoubleFloat *df;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, cl_make_double_float(-123.456));

    cl_fasl_reader_init(&r, buf, w.pos);
    df = (CL_DoubleFloat *)CL_OBJ_TO_PTR(cl_fasl_deserialize_obj(&r));
    ASSERT(fabs(df->value - (-123.456)) < 1e-10);
}

TEST(serialize_ratio)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj rat, result;
    CL_Ratio *rt;

    rat = cl_make_ratio(CL_MAKE_FIXNUM(3), CL_MAKE_FIXNUM(4));
    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, rat);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_RATIO_P(result));
    rt = (CL_Ratio *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(rt->numerator), 3);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(rt->denominator), 4);
}

TEST(serialize_bignum)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bn, result;
    CL_Bignum *b;

    bn = cl_make_bignum(2, 0);
    b = (CL_Bignum *)CL_OBJ_TO_PTR(bn);
    b->limbs[0] = 0x1234;
    b->limbs[1] = 0x5678;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, bn);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_BIGNUM_P(result));
    b = (CL_Bignum *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(b->length, 2);
    ASSERT_EQ_INT(b->sign, 0);
    ASSERT(b->limbs[0] == 0x1234);
    ASSERT(b->limbs[1] == 0x5678);
}

TEST(serialize_bignum_negative)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bn, result;
    CL_Bignum *b;

    bn = cl_make_bignum(1, 1);
    b = (CL_Bignum *)CL_OBJ_TO_PTR(bn);
    b->limbs[0] = 42;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, bn);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    b = (CL_Bignum *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(b->sign, 1);
    ASSERT(b->limbs[0] == 42);
}

TEST(serialize_vector)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj vec, result;
    CL_Vector *v;

    vec = cl_make_vector(3);
    v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
    v->data[0] = CL_MAKE_FIXNUM(10);
    v->data[1] = CL_MAKE_FIXNUM(20);
    v->data[2] = CL_MAKE_FIXNUM(30);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, vec);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_VECTOR_P(result));
    v = (CL_Vector *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(v->length, 3);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[0]), 10);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[1]), 20);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[2]), 30);
}

/* A multidimensional array must round-trip with its rank and dimensions
 * intact — previously every CL_Vector serialized as a flat element count, so
 * #2A((1 2)(3 4)) reloaded from a FASL as the rank-1 vector #(1 2 3 4). */
TEST(serialize_md_array_preserves_rank)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj arr, result;
    CL_Vector *v;
    CL_Obj *elts;

    arr = cl_eval_string("#2a((1 2) (3 4))");
    ASSERT(CL_VECTOR_P(arr));
    v = (CL_Vector *)CL_OBJ_TO_PTR(arr);
    ASSERT_EQ_INT(v->rank, 2);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, arr);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);
    ASSERT(CL_VECTOR_P(result));
    v = (CL_Vector *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(v->rank, 2);
    ASSERT_EQ_INT(v->length, 4);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[0]), 2);   /* dim 0 */
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[1]), 2);   /* dim 1 */
    elts = cl_vector_data(v);                       /* skips dim storage */
    ASSERT_EQ_INT(CL_FIXNUM_VAL(elts[0]), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(elts[1]), 2);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(elts[2]), 3);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(elts[3]), 4);
}

TEST(serialize_vector_empty)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Vector *v;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, cl_make_vector(0));

    cl_fasl_reader_init(&r, buf, w.pos);
    v = (CL_Vector *)CL_OBJ_TO_PTR(cl_fasl_deserialize_obj(&r));
    ASSERT_EQ_INT(v->length, 0);
}

TEST(serialize_vector_mixed_types)
{
    uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj vec, result;
    CL_Vector *v;

    vec = cl_make_vector(3);
    CL_GC_PROTECT(vec);
    v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
    v->data[0] = CL_MAKE_FIXNUM(42);
    v->data[1] = cl_make_string("test", 4);
    v = (CL_Vector *)CL_OBJ_TO_PTR(vec);
    v->data[2] = CL_T;
    CL_GC_UNPROTECT(1);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, vec);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    v = (CL_Vector *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(v->data[0]), 42);
    ASSERT(CL_STRING_P(v->data[1]));
    ASSERT(v->data[2] == CL_T);
}

TEST(serialize_bit_vector)
{
    uint8_t buf[128];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bv, result;
    CL_BitVector *b;

    bv = cl_make_bit_vector(8);
    b = (CL_BitVector *)CL_OBJ_TO_PTR(bv);
    cl_bv_set_bit(b, 0, 1);
    cl_bv_set_bit(b, 3, 1);
    cl_bv_set_bit(b, 7, 1);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, bv);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_BIT_VECTOR_P(result));
    b = (CL_BitVector *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(b->length, 8);
    ASSERT_EQ_INT(cl_bv_get_bit(b, 0), 1);
    ASSERT_EQ_INT(cl_bv_get_bit(b, 1), 0);
    ASSERT_EQ_INT(cl_bv_get_bit(b, 3), 1);
    ASSERT_EQ_INT(cl_bv_get_bit(b, 7), 1);
}

TEST(serialize_bit_vector_empty)
{
    uint8_t buf[64];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_BitVector *b;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, cl_make_bit_vector(0));

    cl_fasl_reader_init(&r, buf, w.pos);
    b = (CL_BitVector *)CL_OBJ_TO_PTR(cl_fasl_deserialize_obj(&r));
    ASSERT_EQ_INT(b->length, 0);
}

TEST(serialize_pathname)
{
    uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj pn, result;
    CL_Pathname *p;
    CL_String *s;

    {
        CL_Obj name_str = cl_make_string("test", 4);
        CL_GC_PROTECT(name_str);
        CL_Obj type_str = cl_make_string("lisp", 4);
        CL_GC_PROTECT(type_str);
        pn = cl_make_pathname(CL_NIL, CL_NIL, CL_NIL,
                              name_str, type_str, CL_NIL);
        CL_GC_UNPROTECT(2);
    }

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, pn);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT(CL_PATHNAME_P(result));
    p = (CL_Pathname *)CL_OBJ_TO_PTR(result);
    ASSERT(CL_NULL_P(p->host));
    ASSERT(CL_NULL_P(p->device));
    ASSERT(CL_NULL_P(p->directory));
    s = (CL_String *)CL_OBJ_TO_PTR(p->name);
    ASSERT_STR_EQ(s->data, "test");
    s = (CL_String *)CL_OBJ_TO_PTR(p->type);
    ASSERT_STR_EQ(s->data, "lisp");
}

TEST(serialize_heterogeneous_list)
{
    uint8_t buf[2048];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj list, result, cur;

    /* Build (42 "hello" :key T NIL #\A) */
    list = CL_NIL;
    CL_GC_PROTECT(list);
    list = cl_cons(CL_MAKE_CHAR('A'), list);
    list = cl_cons(CL_NIL, list);
    list = cl_cons(CL_T, list);
    {
        CL_Obj kw = cl_intern_in("KEY", 3, cl_find_package("KEYWORD", 7));
        list = cl_cons(kw, list);
    }
    {
        CL_Obj s = cl_make_string("hello", 5);
        list = cl_cons(s, list);
    }
    list = cl_cons(CL_MAKE_FIXNUM(42), list);
    CL_GC_UNPROTECT(1);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, list);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);

    cur = result;
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cur)), 42); cur = cl_cdr(cur);
    ASSERT(CL_STRING_P(cl_car(cur))); cur = cl_cdr(cur);
    ASSERT(CL_SYMBOL_P(cl_car(cur))); cur = cl_cdr(cur);
    ASSERT(cl_car(cur) == CL_T); cur = cl_cdr(cur);
    ASSERT(CL_NULL_P(cl_car(cur))); cur = cl_cdr(cur);
    ASSERT_EQ_INT(CL_CHAR_VAL(cl_car(cur)), 'A');
}

TEST(serialize_multiple_objects_sequential)
{
    uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, CL_MAKE_FIXNUM(1));
    cl_fasl_serialize_obj(&w, CL_T);
    cl_fasl_serialize_obj(&w, CL_NIL);

    cl_fasl_reader_init(&r, buf, w.pos);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_fasl_deserialize_obj(&r)), 1);
    ASSERT(cl_fasl_deserialize_obj(&r) == CL_T);
    ASSERT(CL_NULL_P(cl_fasl_deserialize_obj(&r)));
    ASSERT_EQ_INT(r.error, FASL_OK);
}

TEST(deserialize_bad_tag)
{
    uint8_t buf[4] = {0xFE};
    CL_FaslReader r;

    cl_fasl_reader_init(&r, buf, 1);
    cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_ERR_BAD_TAG);
}

/* TYPE_LOCK round-trip: a lock embedded in a constant (e.g. a struct slot
 * captured at compile time, like log4cl's appender %LOCK initform) used
 * to deserialize as NIL because TYPE_LOCK had no FASL serialization.
 * Verify that round-tripping a lock now produces a fresh, usable lock
 * with the same name and recursive-flag — a distinct, free lock object. */
TEST(serialize_lock_nonrecursive)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj name, lk_obj, result;
    CL_Lock *lk_in, *lk_out;
    CL_String *out_name;

    name = cl_make_string("test-lock", 9);
    CL_GC_PROTECT(name);
    lk_obj = cl_lock_alloc_obj(0, name, "TEST");
    CL_GC_PROTECT(lk_obj);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, lk_obj);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);
    ASSERT(CL_LOCK_P(result));

    lk_in  = (CL_Lock *)CL_OBJ_TO_PTR(lk_obj);
    lk_out = (CL_Lock *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(lk_out->flags & CL_LOCK_FLAG_RECURSIVE, 0);
    ASSERT(lk_in != lk_out);                   /* a fresh lock object */
    ASSERT_EQ_INT((int)lk_out->state, 0);      /* free, depth 0 */
    ASSERT_EQ_INT((int)lk_out->depth, 0);
    ASSERT(CL_STRING_P(lk_out->name));
    out_name = (CL_String *)CL_OBJ_TO_PTR(lk_out->name);
    ASSERT_STR_EQ(out_name->data, "test-lock");

    CL_GC_UNPROTECT(2);
}

TEST(serialize_lock_recursive_preserves_flag)
{
    uint8_t buf[256];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj name, lk_obj, result;
    CL_Lock *lk_out;

    name = cl_make_string("rec", 3);
    CL_GC_PROTECT(name);
    lk_obj = cl_lock_alloc_obj(1, name, "TEST");
    CL_GC_PROTECT(lk_obj);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, lk_obj);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);
    ASSERT(CL_LOCK_P(result));

    lk_out = (CL_Lock *)CL_OBJ_TO_PTR(result);
    ASSERT(lk_out->flags & CL_LOCK_FLAG_RECURSIVE);

    CL_GC_UNPROTECT(2);
}

/* A string or vector literal that occurs twice is one object after the
 * round trip (v36), also when it sits inside a shared vector whose other
 * elements carry OBJ_DEFs of their own — the vector has to be registered
 * before its elements are read. */
TEST(serialize_string_and_vector_sharing_preserved)
{
    uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj str, vec, list, result, v1;

    str = cl_make_string("shared", 6);
    CL_GC_PROTECT(str);
    vec = cl_make_vector(3);
    CL_GC_PROTECT(vec);
    ((CL_Vector *)CL_OBJ_TO_PTR(vec))->data[0] = str;
    ((CL_Vector *)CL_OBJ_TO_PTR(vec))->data[1] = str;
    ((CL_Vector *)CL_OBJ_TO_PTR(vec))->data[2] = vec;      /* itself */
    /* (vec vec str) */
    list = cl_cons(str, CL_NIL);
    CL_GC_PROTECT(list);
    list = cl_cons(vec, list);
    list = cl_cons(vec, list);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, list);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    v1 = cl_car(result);
    ASSERT(CL_VECTOR_P(v1));
    ASSERT(v1 == cl_car(cl_cdr(result)));
    ASSERT(CL_STRING_P(((CL_Vector *)CL_OBJ_TO_PTR(v1))->data[0]));
    ASSERT(((CL_Vector *)CL_OBJ_TO_PTR(v1))->data[0] ==
           ((CL_Vector *)CL_OBJ_TO_PTR(v1))->data[1]);
    ASSERT(((CL_Vector *)CL_OBJ_TO_PTR(v1))->data[2] == v1);
    ASSERT(((CL_Vector *)CL_OBJ_TO_PTR(v1))->data[0] ==
           cl_car(cl_cdr(cl_cdr(result))));

    cl_fasl_writer_release(&w);
    if (r.shared_objs) platform_free(r.shared_objs);
    CL_GC_UNPROTECT(3);
}

/* Shared-object ids restart with every unit while one reader (and its table)
 * reads the whole file: an id defined again must resolve to the NEW unit's
 * object, not to what the previous unit left in the slot. */
TEST(serialize_shared_ids_restart_per_unit)
{
    uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj s1, s2, l1, l2, r1, r2;
    uint32_t unit1_len;

    s1 = cl_make_string("one", 3);
    CL_GC_PROTECT(s1);
    s2 = cl_make_string("two", 3);
    CL_GC_PROTECT(s2);
    l1 = cl_cons(s1, CL_NIL);
    CL_GC_PROTECT(l1);
    l1 = cl_cons(s1, l1);
    l2 = cl_cons(s2, CL_NIL);
    CL_GC_PROTECT(l2);
    l2 = cl_cons(s2, l2);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, l1);
    ASSERT_EQ_INT(w.error, FASL_OK);
    unit1_len = w.pos;
    cl_fasl_writer_release(&w);
    cl_fasl_writer_init(&w, buf + unit1_len, sizeof(buf) - unit1_len);
    cl_fasl_serialize_obj(&w, l2);                 /* ids from 0 again */
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, unit1_len + w.pos);
    r1 = cl_fasl_deserialize_obj(&r);
    CL_GC_PROTECT(r1);
    r2 = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);
    ASSERT(cl_car(r1) == cl_car(cl_cdr(r1)));
    ASSERT(cl_car(r2) == cl_car(cl_cdr(r2)));
    ASSERT(cl_car(r1) != cl_car(r2));
    ASSERT(CL_STRING_P(cl_car(r2)));
    ASSERT_EQ_INT(((CL_String *)CL_OBJ_TO_PTR(cl_car(r2)))->data[1], 'w');

    cl_fasl_writer_release(&w);
    if (r.shared_objs) platform_free(r.shared_objs);
    CL_GC_UNPROTECT(5);
}

/* When the same lock is referenced twice within a unit, dedup via
 * OBJ_DEF/OBJ_REF must collapse them to a single fresh-at-load lock —
 * otherwise two struct slots that shared a lock at compile time would
 * each get an independent lock at load time and synchronization would
 * be split across them. */
TEST(serialize_lock_sharing_preserved)
{
    uint8_t buf[512];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj name, lk_obj, list, result;
    CL_Obj first, second;

    name = cl_make_string("shared", 6);
    CL_GC_PROTECT(name);
    lk_obj = cl_lock_alloc_obj(0, name, "TEST");
    CL_GC_PROTECT(lk_obj);
    /* (lk lk) — same object twice */
    list = cl_cons(lk_obj, CL_NIL);
    CL_GC_PROTECT(list);
    list = cl_cons(lk_obj, list);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, list);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    first  = cl_car(result);
    second = cl_car(cl_cdr(result));
    ASSERT(CL_LOCK_P(first));
    ASSERT(CL_LOCK_P(second));
    ASSERT(first == second);  /* identity preserved via OBJ_DEF/OBJ_REF */

    CL_GC_UNPROTECT(3);
}

/* When the same cons head is referenced twice within a unit (typical
 * of compile-time sharing — a parameter list quoted from multiple
 * macroexpansions, a class precedence list reachable from several
 * slots, a trivia pattern template), the writer's OBJ_DEF/OBJ_REF
 * dedup must collapse them so the two references resolve to the same
 * cons cell after load.  Without head-cons dedup, both references
 * would re-walk the entire list and the deserialised result would
 * have two structurally-equal but non-eq lists.  Also a tight check
 * that the hash-based dedup table works correctly for cons cells. */
TEST(serialize_cons_head_sharing_preserved)
{
    uint8_t buf[1024];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj inner, outer, result;
    CL_Obj first_ref, second_ref, third_ref;

    /* Build (a b c) once, then a wrapper list that references it three
     * times: ((a b c) (a b c) (a b c)). */
    inner = cl_cons(CL_MAKE_FIXNUM(3), CL_NIL);
    CL_GC_PROTECT(inner);
    inner = cl_cons(CL_MAKE_FIXNUM(2), inner);
    inner = cl_cons(CL_MAKE_FIXNUM(1), inner);

    outer = cl_cons(inner, CL_NIL);
    CL_GC_PROTECT(outer);
    outer = cl_cons(inner, outer);
    outer = cl_cons(inner, outer);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_obj(&w, outer);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    first_ref  = cl_car(result);
    second_ref = cl_car(cl_cdr(result));
    third_ref  = cl_car(cl_cdr(cl_cdr(result)));
    ASSERT(CL_CONS_P(first_ref));
    ASSERT(CL_CONS_P(second_ref));
    ASSERT(CL_CONS_P(third_ref));
    /* All three should be the SAME cons cell after dedup. */
    ASSERT(first_ref == second_ref);
    ASSERT(first_ref == third_ref);
    /* And the elements of the shared list survived correctly. */
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(first_ref)), 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(first_ref))), 2);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(cl_car(cl_cdr(cl_cdr(first_ref)))), 3);

    CL_GC_UNPROTECT(2);
}

/* Many distinct shared cons cells in one unit — exercises the writer-
 * side hash table beyond the initial capacity (which starts at 64
 * slots and grows by doubling).  Without the hash table, this lookup
 * would be O(N²) over thousands of conses, and far too slow for the
 * large compile-time literal graphs that hit this path in practice
 * (e.g. cl-unicode/hash-tables.lisp's huge alists).  Also smoke-tests
 * that resize handles many entries without losing any. */
TEST(serialize_many_shared_cons_cells)
{
    const uint32_t n_unique = 500;
    uint32_t buf_size = n_unique * 64 + 8192;  /* plenty of headroom */
    uint8_t *buf = (uint8_t *)platform_alloc(buf_size);
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj outer, result, p;
    CL_Obj *uniques;
    uint32_t i;

    ASSERT(buf != NULL);

    uniques = (CL_Obj *)platform_alloc(n_unique * sizeof(CL_Obj));
    ASSERT(uniques != NULL);

    /* Build n_unique distinct cons cells (each with a unique fixnum
     * car) and stitch them into a list referenced TWICE so each cons
     * head reaches the dedup path on its second appearance. */
    outer = CL_NIL;
    CL_GC_PROTECT(outer);
    for (i = 0; i < n_unique; i++) {
        uniques[i] = cl_cons(CL_MAKE_FIXNUM((int)i), CL_NIL);
        CL_GC_PROTECT(uniques[i]);
        outer = cl_cons(uniques[i], outer);
    }
    /* Reference each unique cons a second time, in reverse order. */
    for (i = 0; i < n_unique; i++) {
        outer = cl_cons(uniques[i], outer);
    }

    cl_fasl_writer_init(&w, buf, buf_size);
    cl_fasl_serialize_obj(&w, outer);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_obj(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    /* Walk the result and verify each "first reference" matches the
     * "second reference" by EQ identity. */
    p = result;
    {
        CL_Obj first_n[500];
        for (i = 0; i < n_unique; i++) {
            ASSERT(CL_CONS_P(p));
            first_n[i] = cl_car(p);
            ASSERT(CL_CONS_P(first_n[i]));
            p = cl_cdr(p);
        }
        /* Second batch was prepended in the same order, so it appears
         * in the deserialised list in the same order as the first batch. */
        for (i = 0; i < n_unique; i++) {
            ASSERT(CL_CONS_P(p));
            ASSERT(cl_car(p) == first_n[i]);
            p = cl_cdr(p);
        }
        ASSERT(CL_NULL_P(p));
    }

    CL_GC_UNPROTECT(1 + n_unique);
    platform_free(uniques);
    platform_free(buf);
}

/* ================================================================
 * Bytecode serialization — manually constructed bytecodes
 * ================================================================ */

TEST(serialize_bytecode_simple)
{
    uint8_t buf[1024];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bc_obj, result;
    CL_Bytecode *bc, *bc2;

    bc_obj = make_simple_bytecode(CL_MAKE_FIXNUM(42));
    ASSERT(!CL_NULL_P(bc_obj));
    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_bytecode(&w, bc_obj);
    ASSERT_EQ_INT(w.error, FASL_OK);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_bytecode(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);
    ASSERT(CL_BYTECODE_P(result));

    bc2 = (CL_Bytecode *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(bc2->code_len, bc->code_len);
    ASSERT(memcmp(bc2->code, bc->code, bc->code_len) == 0);
    ASSERT_EQ_INT(bc2->n_constants, 1);
    ASSERT_EQ_INT(CL_FIXNUM_VAL(bc2->constants[0]), 42);
    ASSERT_EQ_INT(bc2->arity, 0);
    ASSERT_EQ_INT(bc2->n_locals, 0);
    ASSERT_EQ_INT(bc2->n_upvalues, 0);
    ASSERT_EQ_INT(bc2->n_optional, 0);
    ASSERT_EQ_INT(bc2->flags, 0);
    ASSERT_EQ_INT(bc2->n_keys, 0);
    ASSERT_EQ_INT(bc2->source_line, 42);
    ASSERT(CL_NULL_P(bc2->name));
}

TEST(serialize_bytecode_eval)
{
    uint8_t buf[1024];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bc_obj, result, val;

    /* OP_CONST 0, OP_RET with constant = 99 */
    bc_obj = make_simple_bytecode(CL_MAKE_FIXNUM(99));

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_bytecode(&w, bc_obj);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_bytecode(&r);

    val = cl_vm_eval(result);
    ASSERT(CL_FIXNUM_P(val));
    ASSERT_EQ_INT(CL_FIXNUM_VAL(val), 99);
}

TEST(serialize_bytecode_with_string_constant)
{
    uint8_t buf[1024];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bc_obj, result, val;
    CL_Obj str;
    CL_String *s;

    str = cl_make_string("hello", 5);
    CL_GC_PROTECT(str);
    bc_obj = make_simple_bytecode(str);
    CL_GC_UNPROTECT(1);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_bytecode(&w, bc_obj);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_bytecode(&r);

    val = cl_vm_eval(result);
    ASSERT(CL_STRING_P(val));
    s = (CL_String *)CL_OBJ_TO_PTR(val);
    ASSERT_STR_EQ(s->data, "hello");
}

TEST(serialize_bytecode_with_source_info)
{
    uint8_t buf[2048];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bc_obj, result;
    CL_Bytecode *bc2;

    bc_obj = make_bytecode_with_source(CL_MAKE_FIXNUM(1), "test.lisp");

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_bytecode(&w, bc_obj);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_bytecode(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    bc2 = (CL_Bytecode *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(bc2->source_line, 42);
    ASSERT(bc2->source_file != NULL);
    ASSERT_STR_EQ(bc2->source_file, "test.lisp");
    ASSERT_EQ_INT(bc2->line_map_count, 2);
    ASSERT_EQ_INT(bc2->line_map[0].pc, 0);
    ASSERT_EQ_INT(bc2->line_map[0].line, 10);
    ASSERT_EQ_INT(bc2->line_map[1].pc, 3);
    ASSERT_EQ_INT(bc2->line_map[1].line, 11);
}

TEST(serialize_bytecode_no_source_file)
{
    uint8_t buf[1024];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bc_obj, result;
    CL_Bytecode *bc2;

    bc_obj = make_simple_bytecode(CL_MAKE_FIXNUM(1));

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_bytecode(&w, bc_obj);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_bytecode(&r);
    bc2 = (CL_Bytecode *)CL_OBJ_TO_PTR(result);
    ASSERT(bc2->source_file == NULL);
    ASSERT_EQ_INT(bc2->line_map_count, 0);
}

TEST(serialize_bytecode_with_keys)
{
    uint8_t buf[2048];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bc_obj, result;
    CL_Bytecode *bc2;

    bc_obj = make_bytecode_with_keys();

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_bytecode(&w, bc_obj);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_bytecode(&r);
    ASSERT_EQ_INT(r.error, FASL_OK);

    bc2 = (CL_Bytecode *)CL_OBJ_TO_PTR(result);
    ASSERT_EQ_INT(bc2->n_keys, 2);
    ASSERT_EQ_INT(bc2->flags & 1, 1);  /* has_key */
    ASSERT(CL_SYMBOL_P(bc2->key_syms[0]));
    ASSERT(CL_SYMBOL_P(bc2->key_syms[1]));
    ASSERT_EQ_INT(bc2->key_slots[0], 0);
    ASSERT_EQ_INT(bc2->key_slots[1], 1);
    ASSERT_EQ_INT(bc2->key_suppliedp_slots[0], 2);
    ASSERT_EQ_INT(bc2->key_suppliedp_slots[1], 0xFF);
}

/* The (optimize (speed 3)) JIT hint travels in bit 7 of the serialized flags
 * byte (fasl.h v38); the in-memory flags keep bits 0-1, which the JIT's
 * eligibility checks compare against 0.  The host writes the FASLs the m68k
 * binaries load, so the hint has to survive a host round trip -- and the
 * call count must not travel at all. */
TEST(serialize_bytecode_speed3_jit_hint)
{
    uint8_t buf[1024];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bc_obj, result;
    CL_Bytecode *bc, *bc2;
    uint8_t flags;
    int hinted;

    bc_obj = make_bytecode_with_keys();
    CL_GC_PROTECT(bc_obj);
    for (hinted = 0; hinted <= 1; hinted++) {
        bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
        flags = bc->flags;
        bc->jit_hot = (uint8_t)((hinted ? CL_BC_JIT_SPEED : 0) | 5);

        cl_fasl_writer_init(&w, buf, sizeof(buf));
        cl_fasl_serialize_bytecode(&w, bc_obj);
        cl_fasl_reader_init(&r, buf, w.pos);
        result = cl_fasl_deserialize_bytecode(&r);
        ASSERT_EQ_INT(r.error, FASL_OK);

        bc2 = (CL_Bytecode *)CL_OBJ_TO_PTR(result);
        ASSERT_EQ_INT(bc2->flags, flags);
        ASSERT_EQ_INT(bc2->flags & 0x80, 0);
        /* Only the (speed 3) hint travels.  The count bits are the hot
         * policy's: a JIT build (cl_jit_note_definition, which the FASL
         * reader calls) rewrites them on load, so they are not checked. */
        ASSERT_EQ_INT(bc2->jit_hot & CL_BC_JIT_SPEED, hinted ? CL_BC_JIT_SPEED : 0);
#ifndef CL_JIT_NATIVE
        /* Without a JIT nothing touches them: the count (5) stayed behind. */
        ASSERT_EQ_INT(bc2->jit_hot, hinted ? CL_BC_JIT_SPEED : 0);
#endif
    }
    CL_GC_UNPROTECT(1);
}

/* The compiler records the hint for (speed 3) anywhere in the lambda -- a
 * local DECLARE included -- on every target, JIT or not. */
TEST(compiler_records_speed3_jit_hint)
{
    CL_Obj fn;
    CL_Bytecode *bc;

    fn = eval_obj("(lambda (x) (declare (optimize (speed 3))) (+ x 1))");
    ASSERT(CL_CLOSURE_P(fn) || CL_BYTECODE_P(fn));
    bc = CL_CLOSURE_P(fn)
        ? (CL_Bytecode *)CL_OBJ_TO_PTR(((CL_Closure *)CL_OBJ_TO_PTR(fn))->bytecode)
        : (CL_Bytecode *)CL_OBJ_TO_PTR(fn);
    ASSERT(bc->jit_hot & CL_BC_JIT_SPEED);

    fn = eval_obj("(lambda (x) (+ x 1))");
    bc = CL_CLOSURE_P(fn)
        ? (CL_Bytecode *)CL_OBJ_TO_PTR(((CL_Closure *)CL_OBJ_TO_PTR(fn))->bytecode)
        : (CL_Bytecode *)CL_OBJ_TO_PTR(fn);
    ASSERT_EQ_INT(bc->jit_hot & CL_BC_JIT_SPEED, 0);
}

TEST(serialize_bytecode_with_symbol_name)
{
    uint8_t buf[1024];
    CL_FaslWriter w;
    CL_FaslReader r;
    CL_Obj bc_obj, result;
    CL_Bytecode *bc, *bc2;
    CL_Obj name_sym;

    bc_obj = make_simple_bytecode(CL_MAKE_FIXNUM(0));
    bc = (CL_Bytecode *)CL_OBJ_TO_PTR(bc_obj);
    name_sym = cl_intern_in("MY-FUNC", 7, cl_package_cl_user);
    bc->name = name_sym;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_serialize_bytecode(&w, bc_obj);

    cl_fasl_reader_init(&r, buf, w.pos);
    result = cl_fasl_deserialize_bytecode(&r);
    bc2 = (CL_Bytecode *)CL_OBJ_TO_PTR(result);
    ASSERT(bc2->name == name_sym);
}

/* ================================================================
 * Full FASL file round-trip (header + units)
 * ================================================================ */

/* Helper: write a single bytecode unit into a FASL writer */
static void write_fasl_unit(CL_FaslWriter *w, CL_Obj bc_obj)
{
    uint8_t tmp[4096];
    CL_FaslWriter tw;
    cl_fasl_writer_init(&tw, tmp, sizeof(tmp));
    cl_fasl_serialize_bytecode(&tw, bc_obj);
    cl_fasl_write_u32(w, tw.pos);
    cl_fasl_write_bytes(w, tmp, tw.pos);
}

TEST(full_fasl_single_unit)
{
    uint8_t buf[4096];
    CL_FaslWriter w;
    CL_Obj bc_obj, result;

    bc_obj = make_simple_bytecode(CL_MAKE_FIXNUM(77));

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_header(&w, 1);
    write_fasl_unit(&w, bc_obj);
    ASSERT_EQ_INT(w.error, FASL_OK);

    result = cl_fasl_load(buf, w.pos);
    ASSERT(result == CL_T);
}

TEST(full_fasl_two_units)
{
    uint8_t buf[8192];
    CL_FaslWriter w;
    CL_Obj bc1, bc2;

    bc1 = make_simple_bytecode(CL_MAKE_FIXNUM(1));
    CL_GC_PROTECT(bc1);
    bc2 = make_simple_bytecode(CL_MAKE_FIXNUM(2));
    CL_GC_UNPROTECT(1);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_header(&w, 2);
    write_fasl_unit(&w, bc1);
    write_fasl_unit(&w, bc2);
    ASSERT_EQ_INT(w.error, FASL_OK);

    ASSERT(cl_fasl_load(buf, w.pos) == CL_T);
}

TEST(full_fasl_defun_via_compile)
{
    uint8_t buf[8192];
    CL_FaslWriter w;
    CL_Obj bc_obj;

    /* Use cl_compile to compile a defun — this works with minimal init
     * since defun is a compiler special form, not a boot.lisp macro */
    bc_obj = cl_compile(eval_obj("'(defun fasl-square (n) (* n n))"));
    ASSERT(!CL_NULL_P(bc_obj));

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_header(&w, 1);
    write_fasl_unit(&w, bc_obj);

    cl_fasl_load(buf, w.pos);
    ASSERT_STR_EQ(eval_print("(fasl-square 7)"), "49");
}

TEST(full_fasl_defvar_via_compile)
{
    uint8_t buf[8192];
    CL_FaslWriter w;
    CL_Obj bc1, bc2;

    bc1 = cl_compile(eval_obj("'(defvar *fasl-x* 100)"));
    CL_GC_PROTECT(bc1);
    bc2 = cl_compile(eval_obj("'(defvar *fasl-y* 200)"));
    CL_GC_UNPROTECT(1);

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_header(&w, 2);
    write_fasl_unit(&w, bc1);
    write_fasl_unit(&w, bc2);

    cl_fasl_load(buf, w.pos);
    ASSERT_STR_EQ(eval_print("*fasl-x*"), "100");
    ASSERT_STR_EQ(eval_print("*fasl-y*"), "200");
}

TEST(full_fasl_zero_units)
{
    uint8_t buf[64];
    CL_FaslWriter w;

    cl_fasl_writer_init(&w, buf, sizeof(buf));
    cl_fasl_write_header(&w, 0);

    ASSERT(cl_fasl_load(buf, w.pos) == CL_T);
}

/* ================================================================
 * compile-file and load tests (require full boot.lisp)
 *
 * These write temp .lisp files, compile-file them to .fasl,
 * then load the .fasl and verify the results.
 * ================================================================ */

/* Helper: write a string to a file */
static void write_test_file(const char *path, const char *content)
{
    PlatformFile fh = platform_file_open(path, PLATFORM_FILE_WRITE);
    if (fh != PLATFORM_FILE_INVALID) {
        platform_file_write_buf(fh, content, (uint32_t)strlen(content));
        platform_file_close(fh);
    }
}

/* Helper: get the cache path for a source file (via compile-file-pathname) */
static const char *get_fasl_path(const char *lisp_expr_buf)
{
    /* Returns pointer to static eval_print buffer */
    return eval_print(lisp_expr_buf);
}

/* Helper: delete a FASL file given its source path */
static void delete_cached_fasl(const char *source_path)
{
    char expr[256];
    const char *fasl_path;
    snprintf(expr, sizeof(expr),
        "(namestring (compile-file-pathname \"%s\"))", source_path);
    fasl_path = eval_print(expr);
    /* fasl_path is quoted like "\"/path/to/file.fasl\"" — strip quotes */
    if (fasl_path && fasl_path[0] == '"') {
        char path_buf[1024];
        size_t len = strlen(fasl_path);
        if (len > 2 && len - 2 < sizeof(path_buf)) {
            memcpy(path_buf, fasl_path + 1, len - 2);
            path_buf[len - 2] = '\0';
            platform_file_delete(path_buf);
        }
    }
}

TEST(compile_file_pathname_uses_cache)
{
    /* compile-file-pathname should return a cache path under ~/.cache */
    const char *result = eval_print(
        "(namestring (compile-file-pathname \"/tmp/test.lisp\"))");
    /* Should contain the cache directory structure and version */
    ASSERT(result != NULL);
    ASSERT(strstr(result, ".cache/common-lisp/cl-amiga-") != NULL);
    ASSERT(strstr(result, CL_VERSION_STRING) != NULL);
    /* Should end with test.fasl (path may include /private/tmp on macOS) */
    ASSERT(strstr(result, "test.fasl") != NULL);
}

TEST(compile_file_pathname_no_ext)
{
    /* No extension -> appends .fasl */
    const char *result = eval_print(
        "(namestring (compile-file-pathname \"/tmp/myfile\"))");
    ASSERT(result != NULL);
    ASSERT(strstr(result, "tmp/myfile.fasl") != NULL);
}

TEST(compile_file_pathname_output_file_override)
{
    /* :output-file overrides cache path */
    ASSERT_STR_EQ(
        eval_print("(namestring (compile-file-pathname \"/tmp/x.lisp\" "
                    ":output-file \"/tmp/custom.fasl\"))"),
        "\"/tmp/custom.fasl\"");
}

TEST(compile_file_basic)
{
    /* Write source, compile, verify .fasl exists at cache path */
    write_test_file("/tmp/cf-test1.lisp",
        "(defvar *cf-test-val* 42)\n");

    eval_obj("(compile-file \"/tmp/cf-test1.lisp\")");

    /* Verify output file exists at the path compile-file-pathname returns */
    {
        const char *result = eval_print(
            "(probe-file (compile-file-pathname \"/tmp/cf-test1.lisp\"))");
        ASSERT(result != NULL);
        /* probe-file returns pathname if exists, NIL if not */
        ASSERT(strcmp(result, "NIL") != 0);
    }

    /* Verify FASL magic by loading the cached file */
    {
        char expr[512];
        snprintf(expr, sizeof(expr),
            "(with-open-file (s (compile-file-pathname \"/tmp/cf-test1.lisp\") "
            ":element-type '(unsigned-byte 8)) "
            "(list (read-byte s) (read-byte s) (read-byte s) (read-byte s)))");
        /* C=67, L=76, F=70, A=65 */
        ASSERT_STR_EQ(eval_print(expr), "(67 76 70 65)");
    }

    /* Clean up */
    platform_file_delete("/tmp/cf-test1.lisp");
    delete_cached_fasl("/tmp/cf-test1.lisp");
}

TEST(compile_file_and_load)
{
    /* Full round-trip: write .lisp -> compile-file -> load .fasl -> verify */
    write_test_file("/tmp/cf-test2.lisp",
        "(defun cf-double (x) (* x 2))\n");

    eval_obj("(compile-file \"/tmp/cf-test2.lisp\")");

    /* Redefine function to something else */
    eval_obj("(defun cf-double (x) x)");

    /* Load the FASL directly by its cache path */
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test2.lisp\"))");

    ASSERT_STR_EQ(eval_print("(cf-double 21)"), "42");

    platform_file_delete("/tmp/cf-test2.lisp");
    delete_cached_fasl("/tmp/cf-test2.lisp");
}

TEST(compile_file_output_file_keyword)
{
    /* :output-file keyword overrides cache path */
    write_test_file("/tmp/cf-test3.lisp", "(defvar *cf-custom* :ok)\n");

    eval_obj("(compile-file \"/tmp/cf-test3.lisp\" :output-file \"/tmp/cf-custom.fasl\")");
    ASSERT(platform_file_exists("/tmp/cf-custom.fasl"));

    eval_obj("(load \"/tmp/cf-custom.fasl\")");
    ASSERT_STR_EQ(eval_print("*cf-custom*"), ":OK");

    platform_file_delete("/tmp/cf-test3.lisp");
    platform_file_delete("/tmp/cf-custom.fasl");
}

TEST(compile_file_multiple_forms)
{
    /* Multiple top-level forms */
    write_test_file("/tmp/cf-test4.lisp",
        "(defvar *cf-a4* 10)\n"
        "(defvar *cf-b4* 20)\n"
        "(defvar *cf-c4* 30)\n"
        "(defun cf-sum4 () (+ *cf-a4* *cf-b4* *cf-c4*))\n");

    eval_obj("(compile-file \"/tmp/cf-test4.lisp\")");
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test4.lisp\"))");

    ASSERT_STR_EQ(eval_print("(cf-sum4)"), "60");

    platform_file_delete("/tmp/cf-test4.lisp");
    delete_cached_fasl("/tmp/cf-test4.lisp");
}

TEST(compile_file_returns_values)
{
    /* compile-file returns (values output-truename nil nil) */
    write_test_file("/tmp/cf-test5.lisp", "(+ 1 2)\n");

    {
        CL_Obj result = eval_obj("(compile-file \"/tmp/cf-test5.lisp\")");
        ASSERT(CL_PATHNAME_P(result));
    }

    platform_file_delete("/tmp/cf-test5.lisp");
    delete_cached_fasl("/tmp/cf-test5.lisp");
}

TEST(compile_file_preserves_package)
{
    /* in-package in compiled file doesn't leak to caller */
    write_test_file("/tmp/cf-test6.lisp",
        "(in-package :cl-user)\n"
        "(defvar *cf-pkg-test6* t)\n");

    {
        CL_Obj pkg_before = cl_current_package;
        eval_obj("(compile-file \"/tmp/cf-test6.lisp\")");
        ASSERT(cl_current_package == pkg_before);
    }

    platform_file_delete("/tmp/cf-test6.lisp");
    delete_cached_fasl("/tmp/cf-test6.lisp");
}

/* NOTE: the compile-file reader-package regression (PROGN/EVAL-WHEN-wrapped
 * IN-PACKAGE keeps switching the reader package, and transient compile-time
 * *PACKAGE* churn does not leak to the reader) is covered by the shell test
 * tests/test_compile_file_package.sh, which runs amilisp fresh per case.  It
 * is not a C unit test here because compile-file under test_fasl's
 * accumulated/never-compacted heap hits the O(n^2) free-list slow path and
 * trips the per-test watchdog (a pre-existing harness limitation, unrelated
 * to the fix). */

TEST(load_fasl_preserves_package)
{
    /* in-package in loaded FASL doesn't leak */
    write_test_file("/tmp/cf-test7.lisp",
        "(in-package :cl-user)\n"
        "(defvar *cf-pkg-test7* t)\n");

    eval_obj("(compile-file \"/tmp/cf-test7.lisp\")");

    {
        CL_Obj pkg_before = cl_current_package;
        eval_obj("(load (compile-file-pathname \"/tmp/cf-test7.lisp\"))");
        ASSERT(cl_current_package == pkg_before);
    }

    platform_file_delete("/tmp/cf-test7.lisp");
    delete_cached_fasl("/tmp/cf-test7.lisp");
}

TEST(load_source_still_works)
{
    /* Loading .lisp source files still works as before */
    write_test_file("/tmp/cf-test8.lisp",
        "(defvar *cf-source-load8* :source-ok)\n");

    eval_obj("(load \"/tmp/cf-test8.lisp\")");
    ASSERT_STR_EQ(eval_print("*cf-source-load8*"), ":SOURCE-OK");

    platform_file_delete("/tmp/cf-test8.lisp");
}

TEST(compile_file_with_macros)
{
    /* Macros defined early must work for later forms (eval-during-compile) */
    write_test_file("/tmp/cf-test9.lisp",
        "(defmacro cf-triple (x) `(* 3 ,x))\n"
        "(defun cf-apply-triple (n) (cf-triple n))\n");

    eval_obj("(compile-file \"/tmp/cf-test9.lisp\")");
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test9.lisp\"))");

    ASSERT_STR_EQ(eval_print("(cf-apply-triple 5)"), "15");

    platform_file_delete("/tmp/cf-test9.lisp");
    delete_cached_fasl("/tmp/cf-test9.lisp");
}

TEST(compile_file_defclass_initform)
{
    /* defclass with :initform creates closures that must survive FASL round-trip */
    write_test_file("/tmp/cf-test-dc.lisp",
        "(defclass cf-test-cls () ((x :initform 42) (y :initform \"hello\")))\n");

    eval_obj("(compile-file \"/tmp/cf-test-dc.lisp\")");
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test-dc.lisp\"))");

    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'cf-test-cls) 'x)"), "42");
    ASSERT_STR_EQ(eval_print("(slot-value (make-instance 'cf-test-cls) 'y)"),
                  "\"hello\"");

    platform_file_delete("/tmp/cf-test-dc.lisp");
    delete_cached_fasl("/tmp/cf-test-dc.lisp");
}

TEST(compile_file_lambda_constant)
{
    /* Lambda in compiled code must be serialized as closure in FASL */
    write_test_file("/tmp/cf-test-lam.lisp",
        "(defvar *cf-fn* (lambda (x) (* x 10)))\n");

    eval_obj("(compile-file \"/tmp/cf-test-lam.lisp\")");
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test-lam.lisp\"))");

    ASSERT_STR_EQ(eval_print("(funcall *cf-fn* 5)"), "50");

    platform_file_delete("/tmp/cf-test-lam.lisp");
    delete_cached_fasl("/tmp/cf-test-lam.lisp");
}

TEST(compile_file_skips_atom_top_level)
{
    /* Per CLHS 3.2.3.1: a top-level atom form (bare symbol, number,
     * string) has no side effect worth evaluating at compile time.
     * Libraries like string-case.lisp leave demo markers like `=>`
     * at top level after a #+bad reader-conditional skip — compile-file
     * must not raise "Unbound variable" for these.  The useful forms
     * around the atom must still compile and load normally. */
    write_test_file("/tmp/cf-test-atom.lisp",
        "(defvar *cf-atom-before* 1)\n"
        "=>\n"
        "42\n"
        "(defvar *cf-atom-after* 2)\n");

    {
        CL_Obj result = eval_obj("(compile-file \"/tmp/cf-test-atom.lisp\")");
        ASSERT(CL_PATHNAME_P(result));  /* compile succeeded, FASL written */
    }
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test-atom.lisp\"))");

    ASSERT_STR_EQ(eval_print("*cf-atom-before*"), "1");
    ASSERT_STR_EQ(eval_print("*cf-atom-after*"), "2");

    platform_file_delete("/tmp/cf-test-atom.lisp");
    delete_cached_fasl("/tmp/cf-test-atom.lisp");
}

TEST(compile_file_descends_toplevel_progn)
{
    /* Per CLHS 3.2.3.1: each subform of a top-level (progn ...) is
     * itself processed as a top-level form. So a (defmacro X) at the
     * head of a progn must take effect before sibling (X ...) forms
     * are macroexpanded. Serapeum's range.lisp relies on exactly this
     * pattern (progn (defmacro define-real-range ...) (define-real-range ...)). */
    write_test_file("/tmp/cf-test-tl-progn.lisp",
        "(progn"
        "  (defmacro cf-tl-progn-double (x) `(* 2 ,x))"
        "  (defvar *cf-tl-progn-val* (cf-tl-progn-double 21)))\n");

    {
        CL_Obj result = eval_obj("(compile-file \"/tmp/cf-test-tl-progn.lisp\")");
        ASSERT(CL_PATHNAME_P(result));
    }
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test-tl-progn.lisp\"))");
    ASSERT_STR_EQ(eval_print("*cf-tl-progn-val*"), "42");

    platform_file_delete("/tmp/cf-test-tl-progn.lisp");
    delete_cached_fasl("/tmp/cf-test-tl-progn.lisp");
}

TEST(compile_file_descends_nested_toplevel_progn)
{
    /* Nested top-level progn: (progn (progn (defmacro X) (X ...)))
     * also needs to descend recursively. */
    write_test_file("/tmp/cf-test-tl-progn-nest.lisp",
        "(progn"
        "  (progn"
        "    (defmacro cf-nested-triple (x) `(* 3 ,x))"
        "    (defvar *cf-nested-val* (cf-nested-triple 14))))\n");

    eval_obj("(compile-file \"/tmp/cf-test-tl-progn-nest.lisp\")");
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test-tl-progn-nest.lisp\"))");
    ASSERT_STR_EQ(eval_print("*cf-nested-val*"), "42");

    platform_file_delete("/tmp/cf-test-tl-progn-nest.lisp");
    delete_cached_fasl("/tmp/cf-test-tl-progn-nest.lisp");
}

TEST(compile_file_source_file_outlives_compile_file)
{
    /* Regression: bi_compile_file used to assign cl_current_source_file
     * (and thus every CL_Bytecode->source_file emitted while compiling)
     * to its stack-local in_path[1024] buffer. After compile-file
     * returned that pointer dangled; later access (debugger backtrace,
     * crash trace, FASL serialize) read whatever bytes happened to
     * occupy the re-used stack frame. With ASDF + CLOS this manifested
     * as garbled source paths in backtraces and (eventually) a SIGSEGV
     * inside a CLOS GF dispatch chain after compile-file returned.
     *
     * Fix: intern paths into a process-lifetime pool. After compile-file
     * returns, every bytecode's source_file must still point to a valid,
     * unchanged C string equal to the original input path. */
    write_test_file("/tmp/cf-test-srcfile-A.lisp",
        "(defun cf-srcfile-fn-A () 42)\n");
    write_test_file("/tmp/cf-test-srcfile-different-name-B.lisp",
        "(defun cf-srcfile-fn-B () 99)\n");

    /* Compile A, then load it to install the function.  Then compile B with a
     * longer/different path.  If A's CL_Bytecode->source_file pointed into
     * bi_compile_file's stack-local in_path[], B's compile would overwrite
     * that buffer, and A's source_file would now read garbage.
     *
     * Per CLHS 3.2.3.1, DEFUN is not eval'd at compile time, so we must load
     * A's FASL before the function symbol is accessible for inspection. */
    eval_obj("(compile-file \"/tmp/cf-test-srcfile-A.lisp\")");
    eval_obj("(load \"/tmp/cf-test-srcfile-A.lisp\")");
    eval_obj("(compile-file \"/tmp/cf-test-srcfile-different-name-B.lisp\")");

    /* Inspect A's bytecode source_file — it must still be A's path. */
    {
        CL_Obj fn_sym = cl_eval_string("(find-symbol \"CF-SRCFILE-FN-A\" *package*)");
        CL_Obj fn_obj;
        CL_Bytecode *bc;
        ASSERT(!CL_NULL_P(fn_sym));
        fn_obj = ((CL_Symbol *)CL_OBJ_TO_PTR(fn_sym))->function;
        ASSERT(CL_BYTECODE_P(fn_obj) || CL_CLOSURE_P(fn_obj));
        if (CL_CLOSURE_P(fn_obj)) {
            CL_Closure *c = (CL_Closure *)CL_OBJ_TO_PTR(fn_obj);
            bc = (CL_Bytecode *)CL_OBJ_TO_PTR(c->bytecode);
        } else {
            bc = (CL_Bytecode *)CL_OBJ_TO_PTR(fn_obj);
        }
        ASSERT(bc->source_file != NULL);
        /* The recorded path is absolutised, which on Windows prefixes the
         * drive ("C:/tmp/..."); what must hold on every platform is that it
         * still names A and not B. */
        {
            const char *want = "/tmp/cf-test-srcfile-A.lisp";
            size_t have_len = strlen(bc->source_file), want_len = strlen(want);
            ASSERT(have_len >= want_len);
            ASSERT_STR_EQ(bc->source_file + (have_len - want_len), want);
        }
    }

    platform_file_delete("/tmp/cf-test-srcfile-A.lisp");
    platform_file_delete("/tmp/cf-test-srcfile-different-name-B.lisp");
    delete_cached_fasl("/tmp/cf-test-srcfile-A.lisp");
    delete_cached_fasl("/tmp/cf-test-srcfile-different-name-B.lisp");
}

TEST(compile_file_declare_with_macro_named_type)
{
    /* Regression: scan_body_for_boxing used to walk DECLARE forms with the
     * general fall-through, which tried to macroexpand any cons inside the
     * declaration whose head named a registered macro.  When a symbol is
     * BOTH deftype'd and defmacro'd (CL allows this — type and function
     * namespaces are separate), a (declare (ftype (THE-SYM ...) NAME))
     * would call the macro with the type spec as argument, corrupting
     * compiler state — manifesting as a stack canary smash on
     * compile-file return.  Trigger came from serapeum's `->`, which is
     * `(deftype -> (args &optional values) ...)` and
     * `(defmacro -> (functions (&rest args) &optional values) ...)`.
     * The macro destructures `(&rest args)` against a single type spec and
     * fails violently. */
    write_test_file("/tmp/cf-test-decl-macroty.lisp",
        "(defmacro cf-decl-macroty (functions (&rest args) &optional values)"
        "  (declare (ignore functions args values))"
        "  '(progn))"
        "\n"
        "(deftype cf-decl-macroty (args &optional values)"
        "  `(function ,args ,@(when values (list values))))"
        "\n"
        "(defun cf-decl-macroty-victim (test)"
        "  (flet ((ge (a b) (funcall test a b)))"
        "    (declare (ftype (cf-decl-macroty (t t) t) ge))"
        "    (ge 1 2)))\n");

    {
        CL_Obj result = eval_obj("(compile-file \"/tmp/cf-test-decl-macroty.lisp\")");
        ASSERT(CL_PATHNAME_P(result));
    }
    eval_obj("(load (compile-file-pathname \"/tmp/cf-test-decl-macroty.lisp\"))");
    ASSERT_STR_EQ(eval_print("(cf-decl-macroty-victim #'eql)"), "NIL");

    platform_file_delete("/tmp/cf-test-decl-macroty.lisp");
    delete_cached_fasl("/tmp/cf-test-decl-macroty.lisp");
}

TEST(compile_file_class_metaobject_in_constants)
{
    /* Regression: a top-level form whose bytecode constant pool contains a
     * CLOS class metaobject (e.g. defmethod with `#.(class-of foo)` as a
     * specializer) used to crash compile-file with FASL_ERR_TOO_DEEP after
     * spinning the FASL serializer up to its 2M-frame cap.  Class metaobjects
     * are STANDARD-CLASS / BUILT-IN-CLASS structs whose effective-slots,
     * precedence-list, and direct-subclasses slots back-reference the class
     * (cycles by design).  Even if we walked the cycle once, deserializing
     * into a *new* struct would lose identity — the live class would no
     * longer EQ the deserialized one, and (defmethod foo ((x #.cls)) ...)
     * would dispatch to the wrong specializer.  Fix: writer detects the
     * metaclass and emits FASL_TAG_CLASS_REF + name; reader resolves via
     * (find-class 'name) so identity matches the runtime metaobject. */
    write_test_file("/tmp/cf-class-metaobj.lisp",
        "(defclass cf-cls-mo () ((x :initarg :x :reader cf-cls-mo-x)))\n"
        "(defun cf-make-cls-mo () (make-instance 'cf-cls-mo :x 42))\n"
        "(defmethod cf-cls-mo-method ((self #.(find-class 'cf-cls-mo)))\n"
        "  (cf-cls-mo-x self))\n");
    {
        CL_Obj result = eval_obj("(compile-file \"/tmp/cf-class-metaobj.lisp\")");
        ASSERT(CL_PATHNAME_P(result));
    }
    /* Wipe the class binding so the cached FASL must rebuild it via the
     * compiled defclass form, *then* the cached defmethod must EQ the
     * freshly-built class metaobject (because of the find-class lookup). */
    eval_obj("(setf (find-class 'cf-cls-mo) nil)");
    eval_obj("(load (compile-file-pathname \"/tmp/cf-class-metaobj.lisp\"))");
    ASSERT_STR_EQ(eval_print("(cf-cls-mo-method (cf-make-cls-mo))"), "42");

    platform_file_delete("/tmp/cf-class-metaobj.lisp");
    delete_cached_fasl("/tmp/cf-class-metaobj.lisp");
}

/* ================================================================
 * load-time-value under COMPILE-FILE (regression: deflate.lisp)
 * ================================================================ */

TEST(compile_file_ltv_forward_function)
{
    /* LTV referencing a function defun'd earlier in the same file must not
     * signal Undefined function during compile-file (the deflate.lisp regression).
     * After loading the FASL the function must return the LTV's value. */
    write_test_file("/tmp/cf-ltv-fwd.lisp",
        "(defun cf-ltv-gen-table () (list 1 2 3))\n"
        "(defun cf-ltv-get-table () (load-time-value (cf-ltv-gen-table)))\n");

    {
        CL_Obj result = eval_obj("(compile-file \"/tmp/cf-ltv-fwd.lisp\")");
        ASSERT(CL_PATHNAME_P(result));
    }
    eval_obj("(load (compile-file-pathname \"/tmp/cf-ltv-fwd.lisp\"))");
    ASSERT_STR_EQ(eval_print("(cf-ltv-get-table)"), "(1 2 3)");

    platform_file_delete("/tmp/cf-ltv-fwd.lisp");
    delete_cached_fasl("/tmp/cf-ltv-fwd.lisp");
}

TEST(compile_file_ltv_eq_stable)
{
    /* A load-time-value form must return the same (EQ) object on every call. */
    write_test_file("/tmp/cf-ltv-eq.lisp",
        "(defun cf-ltv-eq-fn () (load-time-value (cons 1 2)))\n");

    {
        CL_Obj result = eval_obj("(compile-file \"/tmp/cf-ltv-eq.lisp\")");
        ASSERT(CL_PATHNAME_P(result));
    }
    eval_obj("(load (compile-file-pathname \"/tmp/cf-ltv-eq.lisp\"))");
    ASSERT_STR_EQ(eval_print("(eq (cf-ltv-eq-fn) (cf-ltv-eq-fn))"), "T");

    platform_file_delete("/tmp/cf-ltv-eq.lisp");
    delete_cached_fasl("/tmp/cf-ltv-eq.lisp");
}

TEST(compile_file_ltv_distinct_forms)
{
    /* Two distinct LTV forms in the same file must produce independent objects
     * (no eq-dedup cross-contamination from the constants table). */
    write_test_file("/tmp/cf-ltv-dist.lisp",
        "(defun cf-ltv-dist-f1 () (load-time-value (cons 'a 'b)))\n"
        "(defun cf-ltv-dist-f2 () (load-time-value (cons 'a 'b)))\n");

    {
        CL_Obj result = eval_obj("(compile-file \"/tmp/cf-ltv-dist.lisp\")");
        ASSERT(CL_PATHNAME_P(result));
    }
    eval_obj("(load (compile-file-pathname \"/tmp/cf-ltv-dist.lisp\"))");

    ASSERT_STR_EQ(eval_print("(eq (cf-ltv-dist-f1) (cf-ltv-dist-f1))"), "T");
    ASSERT_STR_EQ(eval_print("(eq (cf-ltv-dist-f2) (cf-ltv-dist-f2))"), "T");
    ASSERT_STR_EQ(eval_print("(eq (cf-ltv-dist-f1) (cf-ltv-dist-f2))"), "NIL");

    platform_file_delete("/tmp/cf-ltv-dist.lisp");
    delete_cached_fasl("/tmp/cf-ltv-dist.lisp");
}

TEST(compile_file_ltv_runs_at_load_time)
{
    /* LTV must NOT be evaluated during compile-file (deferred); it evaluates
     * exactly once on the first call after the FASL is loaded. */
    eval_obj("(defvar *cf-ltv-counter* 0)");

    write_test_file("/tmp/cf-ltv-time.lisp",
        "(defun cf-ltv-timed ()"
        "  (load-time-value (progn (incf *cf-ltv-counter*) (cons 'done t))))\n");

    eval_obj("(compile-file \"/tmp/cf-ltv-time.lisp\")");
    ASSERT_STR_EQ(eval_print("*cf-ltv-counter*"), "0"); /* not evaluated at compile time */

    eval_obj("(load (compile-file-pathname \"/tmp/cf-ltv-time.lisp\"))");
    ASSERT_STR_EQ(eval_print("*cf-ltv-counter*"), "1"); /* evaluated at load time (CLHS 3.2.4.4) */

    eval_obj("(cf-ltv-timed)");
    ASSERT_STR_EQ(eval_print("*cf-ltv-counter*"), "1"); /* memoized — no re-evaluation */

    eval_obj("(cf-ltv-timed)");
    ASSERT_STR_EQ(eval_print("*cf-ltv-counter*"), "1"); /* memoized — no re-evaluation */

    platform_file_delete("/tmp/cf-ltv-time.lisp");
    delete_cached_fasl("/tmp/cf-ltv-time.lisp");
}

TEST(load_auto_finds_cached_fasl)
{
    /* load of source file should auto-discover cached FASL */
    write_test_file("/tmp/cf-test10.lisp",
        "(defun cf-auto-fn () :from-fasl)\n");

    /* Compile to create cached FASL */
    eval_obj("(compile-file \"/tmp/cf-test10.lisp\")");

    /* Now modify the source to return something different */
    write_test_file("/tmp/cf-test10.lisp",
        "(defun cf-auto-fn () :from-source)\n");

    /* But don't touch the FASL — it's still newer because we just wrote
       the source AFTER compile-file. Actually the source was just written,
       so it will be newer. We need the FASL to be newer. */
    /* Re-compile so FASL is newer than source */
    write_test_file("/tmp/cf-test10.lisp",
        "(defun cf-auto-fn () :from-fasl)\n");
    eval_obj("(compile-file \"/tmp/cf-test10.lisp\")");

    /* Now load by source path — should pick up the cached FASL */
    eval_obj("(load \"/tmp/cf-test10.lisp\")");
    ASSERT_STR_EQ(eval_print("(cf-auto-fn)"), ":FROM-FASL");

    platform_file_delete("/tmp/cf-test10.lisp");
    delete_cached_fasl("/tmp/cf-test10.lisp");
}

TEST(load_fasl_input_bypasses_user_cache)
{
    /* When LOAD is called with a .fasl path, it must open *that* file —
     * never get redirected to a sibling in the per-user FASL cache.
     *
     * Setup: create two FASLs that share the same cache key but live at
     * different paths, then arrange the cache file to be newer than the
     * direct .fasl.  Pre-fix bi_load would honour the (newer) cache; the
     * fix bypasses the cache lookup for .fasl inputs and loads what was
     * named.  This guards lib/clos.fasl and any hand-compiled .fasl
     * against silent shadowing by stale cache entries. */
    write_test_file("/tmp/cf-prio-direct.lisp",
        "(defvar *cf-cache-prio* :direct-version)\n");
    eval_obj("(compile-file \"/tmp/cf-prio-direct.lisp\" "
             ":output-file \"/tmp/cf-prio.fasl\")");
    ASSERT(platform_file_exists("/tmp/cf-prio.fasl"));

    /* Sleep so the next compile produces a strictly newer mtime
     * (filesystem mtimes typically have 1-second resolution). */
    platform_sleep_ms(1100);

    /* Compile a different source whose cache path collides with the
     * cache path of /tmp/cf-prio.fasl.  make_fasl_cache_path strips the
     * trailing extension, so /tmp/cf-prio.lisp and /tmp/cf-prio.fasl
     * both map to ~/.cache/.../tmp/cf-prio.fasl — exactly the
     * shadow-via-cache scenario we're guarding against. */
    write_test_file("/tmp/cf-prio.lisp",
        "(defvar *cf-cache-prio* :cached-version)\n");
    eval_obj("(compile-file \"/tmp/cf-prio.lisp\")");

    /* The cache version is now newer than /tmp/cf-prio.fasl.  Loading
     * /tmp/cf-prio.fasl must still produce :DIRECT-VERSION. */
    eval_obj("(load \"/tmp/cf-prio.fasl\")");
    ASSERT_STR_EQ(eval_print("*cf-cache-prio*"), ":DIRECT-VERSION");

    platform_file_delete("/tmp/cf-prio-direct.lisp");
    platform_file_delete("/tmp/cf-prio.lisp");
    platform_file_delete("/tmp/cf-prio.fasl");
    delete_cached_fasl("/tmp/cf-prio.lisp");
}

TEST(load_stale_fasl_falls_back_to_source)
{
    /* If source is newer than FASL, load should use source */
    write_test_file("/tmp/cf-test11.lisp",
        "(defun cf-stale-fn () :old-fasl)\n");

    eval_obj("(compile-file \"/tmp/cf-test11.lisp\")");

    /* Wait a moment and rewrite source with different content */
    platform_sleep_ms(1100);  /* Ensure mtime differs (1-sec resolution) */

    write_test_file("/tmp/cf-test11.lisp",
        "(defun cf-stale-fn () :new-source)\n");

    /* Load source path — FASL is stale, should load source */
    eval_obj("(load \"/tmp/cf-test11.lisp\")");
    ASSERT_STR_EQ(eval_print("(cf-stale-fn)"), ":NEW-SOURCE");

    platform_file_delete("/tmp/cf-test11.lisp");
    delete_cached_fasl("/tmp/cf-test11.lisp");
}

TEST(cache_paths_differ_for_different_dirs)
{
    /* Two files with same name in different dirs get different cache paths */
    const char *path_a = eval_print(
        "(namestring (compile-file-pathname \"/tmp/a/test.lisp\"))");
    char buf_a[1024];
    strncpy(buf_a, path_a, sizeof(buf_a) - 1);
    buf_a[sizeof(buf_a) - 1] = '\0';

    const char *path_b = eval_print(
        "(namestring (compile-file-pathname \"/tmp/b/test.lisp\"))");

    /* Paths must be different */
    ASSERT(strcmp(buf_a, path_b) != 0);
    /* But both should contain .fasl */
    ASSERT(strstr(buf_a, ".fasl") != NULL);
    ASSERT(strstr(path_b, ".fasl") != NULL);
}

/* ================================================================
 * main
 * ================================================================ */

int main(void)
{
    test_init();

    /* === Primitive read/write (no CL init needed) === */
    RUN(write_read_u8);
    RUN(write_read_u16);
    RUN(write_read_u32);
    RUN(write_read_bytes);
    RUN(writer_overflow);
    RUN(reader_truncated);
    RUN(reader_truncated_u8);
    RUN(reader_truncated_u16);
    RUN(reader_truncated_bytes);
    RUN(error_propagation_writer);
    RUN(error_propagation_reader);
    RUN(write_read_header);
    RUN(header_bad_magic);
    RUN(header_bad_version);
    RUN(header_zero_units);

    /* === Init CL runtime (once, minimal — no boot.lisp) === */
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_builtins_init();
    cl_repl_init_minimal();

    /* Object serialization */
    RUN(serialize_nil);
    RUN(serialize_t);
    RUN(serialize_unbound);
    RUN(serialize_fixnum_zero);
    RUN(serialize_fixnum_positive);
    RUN(serialize_fixnum_negative);
    RUN(serialize_fixnum_max);
    RUN(serialize_fixnum_min);
    RUN(serialize_character);
    RUN(serialize_character_newline);
    RUN(serialize_string_empty);
    RUN(serialize_string);
    RUN(serialize_long_string);
#ifdef CL_WIDE_STRINGS
    RUN(serialize_wide_string_ascii_downgrades);
    RUN(serialize_wide_string_nonascii_stays_wide);
    RUN(serialize_wide_string_nonascii_portable_refused);
#endif
    RUN(serialize_symbol_cl);
    RUN(serialize_symbol_keyword);
    RUN(serialize_symbol_user);
    RUN(serialize_cons);
    RUN(serialize_list);
    RUN(serialize_dotted_list);
    RUN(serialize_nested_cons);
    RUN(serialize_circular_list);
    RUN(serialize_shared_sublist);
    RUN(serialize_deeply_nested_list);
    RUN(serialize_deep_car_chain_no_overflow);
    RUN(serialize_long_proper_list_50k);
    RUN(serialize_deep_vector_chain_no_overflow);
    RUN(serialize_single_float);
    RUN(serialize_single_float_zero);
    RUN(serialize_single_float_negative);
    RUN(serialize_double_float);
    RUN(serialize_double_float_zero);
    RUN(serialize_double_float_negative);
    RUN(serialize_ratio);
    RUN(serialize_bignum);
    RUN(serialize_bignum_negative);
    RUN(serialize_vector);
    RUN(serialize_md_array_preserves_rank);
    RUN(serialize_vector_empty);
    RUN(serialize_vector_mixed_types);
    RUN(serialize_bit_vector);
    RUN(serialize_bit_vector_empty);
    RUN(serialize_pathname);
    RUN(serialize_heterogeneous_list);
    RUN(serialize_multiple_objects_sequential);
    RUN(deserialize_bad_tag);
    RUN(serialize_lock_nonrecursive);
    RUN(serialize_lock_recursive_preserves_flag);
    RUN(serialize_string_and_vector_sharing_preserved);
    RUN(serialize_shared_ids_restart_per_unit);
    RUN(serialize_lock_sharing_preserved);
    RUN(serialize_cons_head_sharing_preserved);
    RUN(serialize_many_shared_cons_cells);

    /* Bytecode serialization (manually constructed) */
    RUN(serialize_bytecode_simple);
    RUN(serialize_bytecode_eval);
    RUN(serialize_bytecode_with_string_constant);
    RUN(serialize_bytecode_with_source_info);
    RUN(serialize_bytecode_no_source_file);
    RUN(serialize_bytecode_with_keys);
    RUN(serialize_bytecode_speed3_jit_hint);
    RUN(compiler_records_speed3_jit_hint);
    RUN(serialize_bytecode_with_symbol_name);

    /* Full FASL file round-trips */
    RUN(full_fasl_single_unit);
    RUN(full_fasl_two_units);
    RUN(full_fasl_defun_via_compile);
    RUN(full_fasl_defvar_via_compile);
    RUN(full_fasl_zero_units);

    /* === Reinit with full boot.lisp for compile-file tests === */
    cl_mem_shutdown();
    platform_shutdown();
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(CL_DEFAULT_HEAP_SIZE);
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_builtins_init();
    cl_repl_init();

    /* compile-file-pathname tests */
    RUN(compile_file_pathname_uses_cache);
    RUN(compile_file_pathname_no_ext);
    RUN(compile_file_pathname_output_file_override);

    /* compile-file + load round-trip tests */
    RUN(compile_file_basic);
    RUN(compile_file_and_load);
    RUN(compile_file_output_file_keyword);
    RUN(compile_file_multiple_forms);
    RUN(compile_file_returns_values);
    RUN(compile_file_preserves_package);
    RUN(load_fasl_preserves_package);
    RUN(load_source_still_works);
    RUN(compile_file_with_macros);
    RUN(compile_file_defclass_initform);
    RUN(compile_file_lambda_constant);
    RUN(compile_file_skips_atom_top_level);
    RUN(compile_file_descends_toplevel_progn);
    RUN(compile_file_descends_nested_toplevel_progn);
    RUN(compile_file_source_file_outlives_compile_file);
    RUN(compile_file_declare_with_macro_named_type);
    RUN(compile_file_class_metaobject_in_constants);

    /* load-time-value under COMPILE-FILE */
    RUN(compile_file_ltv_forward_function);
    RUN(compile_file_ltv_eq_stable);
    RUN(compile_file_ltv_distinct_forms);
    RUN(compile_file_ltv_runs_at_load_time);

    /* FASL cache behavior tests */
    RUN(load_auto_finds_cached_fasl);
    RUN(load_fasl_input_bypasses_user_cache);
    RUN(load_stale_fasl_falls_back_to_source);
    RUN(cache_paths_differ_for_different_dirs);

    cl_mem_shutdown();
    platform_shutdown();

    REPORT();
}
