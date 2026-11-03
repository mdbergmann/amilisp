/*
 * test_digest.c — EXT:SHA256, EXT:HMAC-SHA256, EXT:PBKDF2-SHA256
 *                 (src/core/builtins_digest.c)
 *
 * Reference values: FIPS 180-4 / NIST CAVP for SHA-256, RFC 4231 for
 * HMAC-SHA256, RFC 7914 §11 for PBKDF2-HMAC-SHA256; the remaining vectors
 * were produced with Python's hashlib (sha256, hmac, pbkdf2_hmac).
 */
#include "test.h"
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
#include "platform/platform.h"

static void setup(void)
{
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
    /* HEX prints a byte vector as lowercase hex; OCT turns an ASCII string
     * into an octet vector; MSG evaluates a form and returns its error text. */
    cl_eval_string("(defun hex (v) (format nil \"~(~{~2,'0x~}~)\" (coerce v 'list)))");
    cl_eval_string("(defun oct (s) (map '(vector (unsigned-byte 8)) #'char-code s))");
    cl_eval_string("(defmacro msg (form) `(handler-case (progn ,form :no-error)"
                   " (error (e) (format nil \"~a\" e))))");
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

static const char *eval_print(const char *str)
{
    static char buf[1024];
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
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        return buf;
    }
}

/* Evaluate (hex FORM) and compare with the expected hex digest. */
static void assert_hex(const char *form, const char *expected)
{
    char src[1024], want[300];
    snprintf(src, sizeof(src), "(hex %s)", form);
    snprintf(want, sizeof(want), "\"%s\"", expected);
    ASSERT_STR_EQ(eval_print(src), want);
}

/* Evaluate (msg FORM) and require the error text to contain NEEDLE. */
static void assert_error_contains(const char *form, const char *needle)
{
    char src[1024];
    const char *r;
    snprintf(src, sizeof(src), "(msg %s)", form);
    r = eval_print(src);
    if (!strstr(r, needle)) {
        printf("  error text for %s\n    was %s\n    lacks \"%s\"\n", form, r, needle);
        test_current_failed = 1;
    }
}

/* --- SHA-256 --- */

TEST(sha256_abc)
{
    assert_hex("(ext:sha256 (oct \"abc\"))",
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(sha256_empty)
{
    assert_hex("(ext:sha256 (oct \"\"))",
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(sha256_two_blocks)
{
    /* FIPS 180-4 B.2: 56 bytes, so the padding spills into a second block. */
    assert_hex("(ext:sha256 (oct \"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq\"))",
               "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(sha256_million_a)
{
    assert_hex("(ext:sha256 (make-array 1000000 :element-type '(unsigned-byte 8)"
               " :initial-element 97))",
               "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(sha256_block_boundaries)
{
    /* Lengths around the 64-byte block and the 56-byte padding threshold,
     * over the byte sequence 0,1,2,...  */
    static const struct { int len; const char *hex; } cases[] = {
        { 55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59" },
        { 56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562" },
        { 63, "29af2686fd53374a36b0846694cc342177e428d1647515f078784d69cdb9e488" },
        { 64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108" },
        { 65, "4bfd2c8b6f1eec7a2afeb48b934ee4b2694182027e6d0fc075074f2fabb31781" },
    };
    char form[256];
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        snprintf(form, sizeof(form),
                 "(ext:sha256 (coerce (loop for i below %d collect i)"
                 " '(vector (unsigned-byte 8))))", cases[i].len);
        assert_hex(form, cases[i].hex);
    }
}

TEST(sha256_start_end)
{
    /* Bytes 3..8 of 0..15, from both vector representations. */
    assert_hex("(ext:sha256 (coerce (loop for i below 16 collect i)"
               " '(vector (unsigned-byte 8))) :start 3 :end 9)",
               "df3153762678d531e5a044492e169302be007fa367f33f9b38febcba6ffca465");
    assert_hex("(ext:sha256 (coerce (loop for i below 16 collect i) 'vector)"
               " :start 3 :end 9)",
               "df3153762678d531e5a044492e169302be007fa367f33f9b38febcba6ffca465");
    /* :end nil means the whole vector; :start alone trims the front. */
    assert_hex("(ext:sha256 (oct \"xxabc\") :start 2 :end nil)",
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert_hex("(ext:sha256 (oct \"abc\") :start 3)",
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(sha256_general_vector)
{
    assert_hex("(ext:sha256 #(97 98 99))",
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* 100 elements: the general-vector path crosses its 64-byte chunk. */
    assert_hex("(ext:sha256 (make-array 100 :initial-element 97))",
               "2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e");
}

TEST(sha256_fill_pointer)
{
    /* The active length is the fill pointer, for both representations. */
    assert_hex("(let ((v (make-array 10 :element-type '(unsigned-byte 8)"
               " :fill-pointer 0))) (vector-push 97 v) (vector-push 98 v)"
               " (vector-push 99 v) (ext:sha256 v))",
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    assert_hex("(let ((v (make-array 10 :fill-pointer 3 :initial-element 97)))"
               " (ext:sha256 v))",
               "9834876dcfb05cb167a5c24953eba58c4ac89b1adf57f28f2f9d09af107ee8f0");
}

TEST(sha256_result_is_fresh_octet_vector)
{
    ASSERT_STR_EQ(eval_print("(length (ext:sha256 (oct \"abc\")))"), "32");
    ASSERT_STR_EQ(eval_print("(typep (ext:sha256 (oct \"abc\"))"
                             " '(simple-array (unsigned-byte 8) (32)))"), "T");
    ASSERT_STR_EQ(eval_print("(eq (ext:sha256 (oct \"abc\")) (ext:sha256 (oct \"abc\")))"),
                  "NIL");
    ASSERT_STR_EQ(eval_print("(equalp (ext:sha256 (oct \"abc\")) (ext:sha256 (oct \"abc\")))"),
                  "T");
}

TEST(sha256_errors)
{
    assert_error_contains("(ext:sha256 \"abc\")", "is a string");
    assert_error_contains("(ext:sha256 \"abc\")", "char-code");
    assert_error_contains("(ext:sha256 'abc)", "must be an octet vector");
    assert_error_contains("(ext:sha256 #(1 256))", "element 1 is not an octet");
    assert_error_contains("(ext:sha256 #(1 -1))", "element 1 is not an octet");
    assert_error_contains("(ext:sha256 #(1 :x))", "element 1 is not an octet");
    assert_error_contains("(ext:sha256 (make-array 2 :element-type '(unsigned-byte 16)))",
                          "2-byte elements");
    assert_error_contains("(ext:sha256 (oct \"abc\") :start 2 :end 1)", "START 2 exceeds END 1");
    assert_error_contains("(ext:sha256 (oct \"abc\") :start 4)", "START 4 exceeds END 3");
    assert_error_contains("(ext:sha256 (oct \"abc\") :end 4)", "END 4 exceeds the vector's length 3");
    assert_error_contains("(ext:sha256 (oct \"abc\") :start -1)", "START must be a non-negative");
    assert_error_contains("(ext:sha256 (oct \"abc\") :end :x)", "END must be NIL or");
    assert_error_contains("(ext:sha256 (make-array '(2 2) :element-type '(unsigned-byte 8)))",
                          "simple (non-displaced)");
}

/* --- HMAC-SHA256 --- */

TEST(hmac_rfc4231_case1)
{
    assert_hex("(ext:hmac-sha256 (make-array 20 :element-type '(unsigned-byte 8)"
               " :initial-element #x0b) (oct \"Hi There\"))",
               "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(hmac_rfc4231_case2)
{
    assert_hex("(ext:hmac-sha256 (oct \"Jefe\") (oct \"what do ya want for nothing?\"))",
               "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(hmac_rfc4231_case6_long_key)
{
    /* 131-byte key: longer than the block, so it is hashed first. */
    assert_hex("(ext:hmac-sha256 (make-array 131 :element-type '(unsigned-byte 8)"
               " :initial-element #xaa)"
               " (oct \"Test Using Larger Than Block-Size Key - Hash Key First\"))",
               "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(hmac_key_lengths)
{
    /* Empty key and data; a key of exactly one block (not hashed). */
    assert_hex("(ext:hmac-sha256 (oct \"\") (oct \"\"))",
               "b613679a0814d9ec772f95d778c35fc5ff1697c493715653c6c712144292c5ad");
    assert_hex("(ext:hmac-sha256 (coerce (loop for i below 64 collect i)"
               " '(vector (unsigned-byte 8))) (oct \"abc\"))",
               "6ab541b4869dca71c4ca11d8bb1b02533b789a557583161429292c7404bc21f6");
}

TEST(hmac_general_vectors)
{
    /* Key and data as general vectors, including a long general key. */
    assert_hex("(ext:hmac-sha256 #(74 101 102 101) (coerce (oct \"what do ya want for nothing?\") 'vector))",
               "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    assert_hex("(ext:hmac-sha256 (make-array 131 :initial-element #xaa)"
               " (oct \"Test Using Larger Than Block-Size Key - Hash Key First\"))",
               "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(hmac_errors)
{
    assert_error_contains("(ext:hmac-sha256 \"key\" (oct \"x\"))", "KEY is a string");
    assert_error_contains("(ext:hmac-sha256 (oct \"k\") 'x)", "DATA must be an octet vector");
    assert_error_contains("(ext:hmac-sha256 #(300) (oct \"x\"))", "KEY element 0 is not an octet");
    assert_error_contains("(ext:hmac-sha256 (make-array 70 :initial-element 999) (oct \"x\"))",
                          "KEY element 0 is not an octet");
    assert_error_contains("(ext:hmac-sha256 (oct \"k\") #(1 2 1000))", "DATA element 2 is not an octet");
}

/* --- PBKDF2-HMAC-SHA256 --- */

TEST(pbkdf2_rfc7914_one_iteration)
{
    assert_hex("(ext:pbkdf2-sha256 (oct \"passwd\") (oct \"salt\") 1 64)",
               "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
               "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

TEST(pbkdf2_rfc7914_80000_iterations)
{
    assert_hex("(ext:pbkdf2-sha256 (oct \"Password\") (oct \"NaCl\") 80000 64)",
               "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
               "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
}

TEST(pbkdf2_partial_last_block)
{
    /* 33 octets: one full block plus one octet of the second. */
    assert_hex("(ext:pbkdf2-sha256 (oct \"passwd\") (oct \"salt\") 1 33)",
               "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49");
    /* 20 octets: a truncated single block, 1000 iterations. */
    assert_hex("(ext:pbkdf2-sha256 (oct \"passwd\") (oct \"salt\") 1000 20)",
               "fa9c158e678bfe94189b0b1a707a0921672fdc9f");
}

TEST(pbkdf2_edge_inputs)
{
    assert_hex("(ext:pbkdf2-sha256 (oct \"\") (oct \"\") 2 16)",
               "97398411d6aea43a77acef92226ab827");
    /* 70-byte password: longer than the HMAC block, hashed as the key. */
    assert_hex("(ext:pbkdf2-sha256 (make-array 70 :element-type '(unsigned-byte 8)"
               " :initial-element 112) (oct \"salt\") 3 32)",
               "96a1c57e266dc48cc008d5927daad2fc4daf6905e2a9365c04efaed5164f52f1");
    /* General vectors for password and salt give the same answer. */
    assert_hex("(ext:pbkdf2-sha256 #(112 97 115 115 119 100) #(115 97 108 116) 1 33)",
               "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49");
    ASSERT_STR_EQ(eval_print("(length (ext:pbkdf2-sha256 (oct \"p\") (oct \"s\") 1 100))"), "100");
    ASSERT_STR_EQ(eval_print("(typep (ext:pbkdf2-sha256 (oct \"p\") (oct \"s\") 1 7)"
                             " '(simple-array (unsigned-byte 8) (7)))"), "T");
}

TEST(pbkdf2_errors)
{
    assert_error_contains("(ext:pbkdf2-sha256 (oct \"p\") (oct \"s\") 0 32)",
                          "ITERATIONS must be a positive integer");
    assert_error_contains("(ext:pbkdf2-sha256 (oct \"p\") (oct \"s\") -5 32)",
                          "ITERATIONS must be a positive integer");
    assert_error_contains("(ext:pbkdf2-sha256 (oct \"p\") (oct \"s\") 1.5 32)",
                          "ITERATIONS must be a positive integer");
    assert_error_contains("(ext:pbkdf2-sha256 (oct \"p\") (oct \"s\") 1 0)",
                          "LENGTH must be a positive integer");
    assert_error_contains("(ext:pbkdf2-sha256 (oct \"p\") (oct \"s\") 1 nil)",
                          "LENGTH must be a positive integer");
    assert_error_contains("(ext:pbkdf2-sha256 \"p\" (oct \"s\") 1 32)", "PASSWORD is a string");
    assert_error_contains("(ext:pbkdf2-sha256 (oct \"p\") \"s\" 1 32)", "SALT is a string");
    assert_error_contains("(ext:pbkdf2-sha256 (oct \"p\") #(1 256) 1 32)", "SALT element 1");
}

/* The iteration loop runs in a GC safe region: a second thread allocating
 * and collecting meanwhile must neither block on it nor corrupt its result
 * (the result vector is re-derived after each stint). */
TEST(pbkdf2_concurrent_with_gc)
{
    ASSERT_STR_EQ(eval_print(
        "(let* ((expected (ext:pbkdf2-sha256 (oct \"Password\") (oct \"NaCl\") 20000 64))"
        "       (workers (loop repeat 3 collect"
        "                  (mp:make-thread (lambda ()"
        "                    (equalp expected"
        "                            (ext:pbkdf2-sha256 (oct \"Password\") (oct \"NaCl\") 20000 64))))))"
        "       (garbage 0))"
        "  (dotimes (i 200) (incf garbage (length (make-list 1000))) (ext:gc))"
        "  (and (every #'mp:join-thread workers) (= garbage 200000)))"),
        "T");
}

int main(void)
{
    test_init();
    setup();

    RUN(sha256_abc);
    RUN(sha256_empty);
    RUN(sha256_two_blocks);
    RUN(sha256_million_a);
    RUN(sha256_block_boundaries);
    RUN(sha256_start_end);
    RUN(sha256_general_vector);
    RUN(sha256_fill_pointer);
    RUN(sha256_result_is_fresh_octet_vector);
    RUN(sha256_errors);
    RUN(hmac_rfc4231_case1);
    RUN(hmac_rfc4231_case2);
    RUN(hmac_rfc4231_case6_long_key);
    RUN(hmac_key_lengths);
    RUN(hmac_general_vectors);
    RUN(hmac_errors);
    RUN(pbkdf2_rfc7914_one_iteration);
    RUN(pbkdf2_rfc7914_80000_iterations);
    RUN(pbkdf2_partial_last_block);
    RUN(pbkdf2_edge_inputs);
    RUN(pbkdf2_errors);
    RUN(pbkdf2_concurrent_with_gc);

    teardown();
    REPORT();
}
