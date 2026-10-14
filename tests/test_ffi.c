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
#ifndef PLATFORM_AMIGA
#include <pthread.h>
#include <stdlib.h>
#endif

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
}

static void teardown(void)
{
    cl_mem_shutdown();
    platform_shutdown();
}

/* Helper: eval a string, return printed result */
static const char *eval_print(const char *str)
{
    static char buf[256];
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

/* ================================================================
 * C-level tests: TYPE_FOREIGN_POINTER
 * ================================================================ */

TEST(c_foreign_pointer_create)
{
    CL_Obj fp = cl_make_foreign_pointer(0xDEADBEEF, 16, 0);
    ASSERT(CL_FOREIGN_POINTER_P(fp));
    CL_ForeignPtr *p = (CL_ForeignPtr *)CL_OBJ_TO_PTR(fp);
    ASSERT_EQ_INT(p->address, (uint32_t)0xDEADBEEF);
    ASSERT_EQ_INT(p->size, 16);
    ASSERT_EQ_INT(p->flags, 0);
}

TEST(c_foreign_pointer_type_name)
{
    CL_Obj fp = cl_make_foreign_pointer(42, 0, 0);
    ASSERT_STR_EQ(cl_type_name(fp), "FOREIGN-POINTER");
}

TEST(c_foreign_pointer_not_fixnum)
{
    CL_Obj fp = cl_make_foreign_pointer(1, 0, 0);
    ASSERT(!CL_FIXNUM_P(fp));
    ASSERT(!CL_NULL_P(fp));
    ASSERT(!CL_CHAR_P(fp));
    ASSERT(CL_HEAP_P(fp));
}

TEST(c_foreign_pointer_zero_addr)
{
    CL_Obj fp = cl_make_foreign_pointer(0, 0, 0);
    CL_ForeignPtr *p = (CL_ForeignPtr *)CL_OBJ_TO_PTR(fp);
    ASSERT_EQ_INT(p->address, 0);
}

TEST(c_foreign_pointer_gc_survives)
{
    CL_Obj fp = cl_make_foreign_pointer(0x12345678, 32, CL_FPTR_FLAG_OWNED);
    CL_GC_PROTECT(fp);
    cl_gc();
    CL_ForeignPtr *p = (CL_ForeignPtr *)CL_OBJ_TO_PTR(fp);
    ASSERT_EQ_INT(p->address, (uint32_t)0x12345678);
    ASSERT_EQ_INT(p->size, 32);
    ASSERT_EQ_INT(p->flags, CL_FPTR_FLAG_OWNED);
    CL_GC_UNPROTECT(1);
}

/* ================================================================
 * Lisp-level tests: FFI package
 * ================================================================ */

TEST(lisp_ffi_package_exists)
{
    ASSERT_STR_EQ(eval_print("(package-name (find-package \"FFI\"))"), "\"FFI\"");
}

TEST(lisp_ffi_make_foreign_pointer)
{
    /* type-of should return FOREIGN-POINTER */
    ASSERT_STR_EQ(eval_print("(type-of (ffi:make-foreign-pointer 42))"),
                  "FOREIGN-POINTER");
}

TEST(lisp_ffi_foreign_pointer_p)
{
    ASSERT_STR_EQ(eval_print("(ffi:foreign-pointer-p (ffi:make-foreign-pointer 0))"), "T");
    ASSERT_STR_EQ(eval_print("(ffi:foreign-pointer-p 42)"), "NIL");
    ASSERT_STR_EQ(eval_print("(ffi:foreign-pointer-p nil)"), "NIL");
    ASSERT_STR_EQ(eval_print("(ffi:foreign-pointer-p \"hello\")"), "NIL");
}

TEST(lisp_ffi_null_pointer_p)
{
    ASSERT_STR_EQ(eval_print("(ffi:null-pointer-p (ffi:make-foreign-pointer 0))"), "T");
    ASSERT_STR_EQ(eval_print("(ffi:null-pointer-p (ffi:make-foreign-pointer 42))"), "NIL");
}

TEST(lisp_ffi_foreign_pointer_address)
{
    ASSERT_STR_EQ(eval_print("(ffi:foreign-pointer-address (ffi:make-foreign-pointer 42))"), "42");
    ASSERT_STR_EQ(eval_print("(ffi:foreign-pointer-address (ffi:make-foreign-pointer 0))"), "0");
}

TEST(lisp_ffi_alloc_free)
{
    /* Alloc should return a non-null foreign pointer */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 64)))"
        "  (prog1 (ffi:null-pointer-p p)"
        "    (ffi:free-foreign p)))"),
        "NIL");
}

TEST(lisp_ffi_peek_poke_u32)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16)))"
        "  (ffi:poke-u32 p 42)"
        "  (prog1 (ffi:peek-u32 p)"
        "    (ffi:free-foreign p)))"),
        "42");
}

TEST(lisp_ffi_peek_poke_u32_offset)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16)))"
        "  (ffi:poke-u32 p 100 0)"
        "  (ffi:poke-u32 p 200 4)"
        "  (prog1 (+ (ffi:peek-u32 p 0) (ffi:peek-u32 p 4))"
        "    (ffi:free-foreign p)))"),
        "300");
}

TEST(lisp_ffi_peek_poke_u16)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16)))"
        "  (ffi:poke-u16 p 1234)"
        "  (prog1 (ffi:peek-u16 p)"
        "    (ffi:free-foreign p)))"),
        "1234");
}

TEST(lisp_ffi_peek_poke_u8)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16)))"
        "  (ffi:poke-u8 p 255)"
        "  (prog1 (ffi:peek-u8 p)"
        "    (ffi:free-foreign p)))"),
        "255");
}

/* --- Bulk byte transfer: POKE-BYTES / PEEK-BYTES --- */

/* A vector source unpacks tagged fixnums; note amilisp upgrades
 * (UNSIGNED-BYTE 8) to T, so these really are boxed elements. */
TEST(lisp_ffi_poke_bytes_vector)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16))"
        "      (v (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 254 255))))"
        "  (let ((n (ffi:poke-bytes p v)))"
        "    (prog1 (list n (ffi:peek-u8 p 0) (ffi:peek-u8 p 1)"
        "                 (ffi:peek-u8 p 2) (ffi:peek-u8 p 3))"
        "      (ffi:free-foreign p))))"),
        "(4 1 2 254 255)");
}

/* A string source takes the memcpy path. */
TEST(lisp_ffi_poke_bytes_string)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16)))"
        "  (let ((n (ffi:poke-bytes p \"AB\" 8)))"
        "    (prog1 (list n (ffi:peek-u8 p 8) (ffi:peek-u8 p 9))"
        "      (ffi:free-foreign p))))"),
        "(2 65 66)");
}

/* OFFSET applies to the destination, START/END bound the source. */
TEST(lisp_ffi_poke_bytes_offset_start_end)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16))"
        "      (v (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 254 255))))"
        "  (let ((n (ffi:poke-bytes p v 12 1 3)))"
        "    (prog1 (list n (ffi:peek-u8 p 12) (ffi:peek-u8 p 13))"
        "      (ffi:free-foreign p))))"),
        "(2 2 254)");
}

/* An empty span is legal and writes nothing. */
TEST(lisp_ffi_poke_bytes_empty)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16))"
        "      (v (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :initial-element 7)))"
        "  (ffi:poke-u8 p 99)"
        "  (let ((n (ffi:poke-bytes p v 0 2 2)))"
        "    (prog1 (list n (ffi:peek-u8 p 0))"
        "      (ffi:free-foreign p))))"),
        "(0 99)");
}

TEST(lisp_ffi_peek_bytes_roundtrip)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16))"
        "      (v (make-array 3 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(9 0 200)))"
        "      (out (make-array 3 :element-type '(unsigned-byte 8)"
        "                         :initial-element 0)))"
        "  (ffi:poke-bytes p v)"
        "  (let ((n (ffi:peek-bytes p out)))"
        "    (prog1 (list n (coerce out 'list))"
        "      (ffi:free-foreign p))))"),
        "(3 (9 0 200))");
}

/* A known allocation size must reject an overrun rather than corrupt
 * memory past the buffer. */
TEST(lisp_ffi_poke_bytes_overrun_rejected)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 4))"
        "      (v (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :initial-element 1)))"
        "  (prog1 (handler-case (progn (ffi:poke-bytes p v 2) :no-error)"
        "           (error () :rejected))"
        "    (ffi:free-foreign p)))"),
        ":REJECTED");
}

/* The read-side mirror of the overrun check above: a known allocation size
 * must reject an overrun rather than read past the buffer. */
TEST(lisp_ffi_peek_bytes_overrun_rejected)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 4))"
        "      (out (make-array 4 :element-type '(unsigned-byte 8)"
        "                         :initial-element 0)))"
        "  (prog1 (handler-case (progn (ffi:peek-bytes p out 2) :no-error)"
        "           (error () :rejected))"
        "    (ffi:free-foreign p)))"),
        ":REJECTED");
}

/* A rank > 1 array is rejected rather than silently misread via
 * CL_VECTOR_ACTIVE_LENGTH/CL_VECTOR_DATA. */
TEST(lisp_ffi_poke_bytes_rank_rejected)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16))"
        "      (v (make-array '(2 2) :element-type '(unsigned-byte 8)"
        "                       :initial-element 1)))"
        "  (prog1 (handler-case (progn (ffi:poke-bytes p v) :no-error)"
        "           (error () :rejected))"
        "    (ffi:free-foreign p)))"),
        ":REJECTED");
}

TEST(lisp_ffi_peek_bytes_rank_rejected)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16))"
        "      (out (make-array '(2 2) :element-type '(unsigned-byte 8)"
        "                         :initial-element 0)))"
        "  (prog1 (handler-case (progn (ffi:peek-bytes p out) :no-error)"
        "           (error () :rejected))"
        "    (ffi:free-foreign p)))"),
        ":REJECTED");
}

TEST(lisp_ffi_poke_bytes_element_range_checked)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16)))"
        "  (prog1 (list (handler-case (progn (ffi:poke-bytes p (vector 1 300)) :no-error)"
        "                 (error () :rejected))"
        "               (handler-case (progn (ffi:poke-bytes p (vector 1 -1)) :no-error)"
        "                 (error () :rejected))"
        "               (handler-case (progn (ffi:poke-bytes p (vector 1 'x)) :no-error)"
        "                 (error () :rejected)))"
        "    (ffi:free-foreign p)))"),
        "(:REJECTED :REJECTED :REJECTED)");
}

TEST(lisp_ffi_poke_bytes_bad_span_and_source)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16))"
        "      (v (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :initial-element 1)))"
        "  (prog1 (list (handler-case (progn (ffi:poke-bytes p v 0 0 99) :no-error)"
        "                 (error () :rejected))"
        "               (handler-case (progn (ffi:poke-bytes p v 0 3 1) :no-error)"
        "                 (error () :rejected))"
        "               (handler-case (progn (ffi:poke-bytes p (list 1 2)) :no-error)"
        "                 (error () :rejected))"
        "               (handler-case (progn (ffi:poke-bytes v v) :no-error)"
        "                 (error () :rejected)))"
        "    (ffi:free-foreign p)))"),
        "(:REJECTED :REJECTED :REJECTED :REJECTED)");
}

/* A fill pointer bounds the source: only the active elements move. */
TEST(lisp_ffi_poke_bytes_respects_fill_pointer)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 16))"
        "      (v (make-array 4 :element-type '(unsigned-byte 8)"
        "                       :initial-contents '(1 2 3 4)"
        "                       :fill-pointer 2)))"
        "  (prog1 (list (ffi:poke-bytes p v)"
        "               (handler-case (progn (ffi:poke-bytes p v 0 0 4) :no-error)"
        "                 (error () :rejected)))"
        "    (ffi:free-foreign p)))"),
        "(2 :REJECTED)");
}

TEST(lisp_ffi_foreign_string)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:foreign-string \"hello\")))"
        "  (prog1 (ffi:foreign-to-string p)"
        "    (ffi:free-foreign p)))"),
        "\"hello\"");
}

TEST(lisp_ffi_foreign_string_empty)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:foreign-string \"\")))"
        "  (prog1 (ffi:foreign-to-string p)"
        "    (ffi:free-foreign p)))"),
        "\"\"");
}

/* No hidden cap: without MAX-LEN the string is read to its NUL.  An old
 * 4096-byte default cut every Clamacs TextEditor export (and so every
 * file it saved) at 4 KB, silently.  MAX-LEN still bounds the read. */
TEST(lisp_ffi_foreign_string_long)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((s (make-string 10000)))"
        "  (dotimes (i 10000) (setf (char s i) (code-char (+ 97 (mod i 26)))))"
        "  (let ((p (ffi:foreign-string s)))"
        "    (prog1 (list (string= s (ffi:foreign-to-string p))"
        "                 (length (ffi:foreign-to-string p nil))"
        "                 (string= (subseq s 0 4096) (ffi:foreign-to-string p 4096))"
        "                 (length (ffi:foreign-to-string p 20000))"
        "                 (ffi:foreign-to-string p 0))"
        "      (ffi:free-foreign p))))"),
        "(T 10000 T 10000 \"\")");
}

TEST(lisp_ffi_foreign_string_bad_max_len)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:foreign-string \"abc\")))"
        "  (prog1 (list (handler-case (ffi:foreign-to-string p -1)"
        "                 (type-error () :rejected))"
        "               (handler-case (ffi:foreign-to-string p \"4\")"
        "                 (type-error () :rejected))"
        "               (ffi:foreign-to-string p 2))"
        "    (ffi:free-foreign p)))"),
        "(:REJECTED :REJECTED \"ab\")");
}

TEST(lisp_ffi_pointer_plus)
{
    /* pointer+ should create a new pointer with offset address */
    ASSERT_STR_EQ(eval_print(
        "(ffi:foreign-pointer-address (ffi:pointer+ (ffi:make-foreign-pointer 100) 20))"),
        "120");
}

TEST(lisp_ffi_printer)
{
    /* Foreign pointers should print as #<FOREIGN-POINTER ...> */
    const char *result = eval_print("(ffi:make-foreign-pointer 42)");
    ASSERT(strstr(result, "#<FOREIGN-POINTER") != NULL);
}

/* ================================================================
 * Typed peek/poke: signed, 64-bit, float/double, pointer
 * (platform-independent — exercised on host and Amiga)
 * ================================================================ */

TEST(lisp_ffi_peek_poke_signed)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8))) (ffi:poke-i8 p -5) (prog1 (ffi:peek-i8 p) (ffi:free-foreign p)))"),
        "-5");
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8))) (ffi:poke-i16 p -1000) (prog1 (ffi:peek-i16 p) (ffi:free-foreign p)))"),
        "-1000");
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8))) (ffi:poke-i32 p -123456) (prog1 (ffi:peek-i32 p) (ffi:free-foreign p)))"),
        "-123456");
}

TEST(lisp_ffi_peek_poke_64)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8))) (ffi:poke-u64 p 4294967300) (prog1 (ffi:peek-u64 p) (ffi:free-foreign p)))"),
        "4294967300");
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8))) (ffi:poke-i64 p -5000000000) (prog1 (ffi:peek-i64 p) (ffi:free-foreign p)))"),
        "-5000000000");
}

TEST(lisp_ffi_peek_poke_float)
{
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8))) (ffi:poke-single p 2.5) (prog1 (ffi:peek-single p) (ffi:free-foreign p)))"),
        "2.5");
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8))) (ffi:poke-double p 6.25d0) (prog1 (ffi:peek-double p) (ffi:free-foreign p)))"),
        "6.25d0");
}

TEST(lisp_ffi_peek_poke_pointer)
{
    /* Store a pointer in memory and read it back; the read pointer must be
     * pointer-eq to the original. */
    ASSERT_STR_EQ(eval_print(
        "(let ((p (ffi:alloc-foreign 8))) (ffi:poke-pointer p p) "
        "(prog1 (ffi:pointer-eq p (ffi:peek-pointer p)) (ffi:free-foreign p)))"),
        "T");
}

TEST(lisp_ffi_pointer_eq)
{
    /* Two pointers to the same real address compare equal even though they
     * are distinct foreign-pointer objects (distinct side-table handles). */
    ASSERT_STR_EQ(eval_print(
        "(let* ((p (ffi:alloc-foreign 8)) (a (ffi:foreign-pointer-address p))) "
        "(prog1 (ffi:pointer-eq p (ffi:make-foreign-pointer a)) (ffi:free-foreign p)))"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(ffi:pointer-eq (ffi:make-foreign-pointer 16) (ffi:make-foreign-pointer 32))"),
        "NIL");
}

#ifndef PLATFORM_AMIGA
/* ================================================================
 * Host-only: dlopen / dlsym / libffi foreign calls + callbacks
 * ================================================================ */

TEST(lisp_ffi_symbol_pointer_found)
{
    /* abs is in the default (libc/libSystem) namespace. */
    ASSERT_STR_EQ(eval_print("(and (ffi:symbol-pointer \"abs\") t)"), "T");
}

TEST(lisp_ffi_symbol_pointer_missing)
{
    ASSERT_STR_EQ(eval_print("(ffi:symbol-pointer \"no_such_symbol_zzz\")"), "NIL");
}

/* AMIGA uses FFI, so AMIGA:CLOSE-LIBRARY and FFI:CLOSE-LIBRARY are one
 * symbol; on the host its function must be the dlclose one, not the
 * "only available on AmigaOS" stub that once replaced it (2026-09-25). */
TEST(lisp_ffi_close_library_is_the_host_one)
{
#if defined(__APPLE__)
    const char *lib = "libSystem.B.dylib";
#elif defined(_WIN32)
    const char *lib = "kernel32.dll";
#else
    const char *lib = "libc.so.6";
#endif
    char form[256];
    snprintf(form, sizeof form,
             "(let ((lib (ffi:load-library \"%s\")))"
             "  (and lib (handler-case (ffi:close-library lib)"
             "             (error (e) (princ-to-string e)))))", lib);
    ASSERT_STR_EQ(eval_print(form), "T");
    ASSERT_STR_EQ(eval_print("(eq 'ffi:close-library 'amiga:close-library)"), "T");
}

TEST(lisp_ffi_call_int)
{
    ASSERT_STR_EQ(eval_print(
        "(ffi:call-foreign (ffi:symbol-pointer \"abs\") :int32 '(:int32) '(-42))"),
        "42");
}

TEST(lisp_ffi_call_pointer_arg)
{
    /* strlen of a foreign string. */
    ASSERT_STR_EQ(eval_print(
        "(let ((s (ffi:foreign-string \"hello\"))) "
        "(prog1 (ffi:call-foreign (ffi:symbol-pointer \"strlen\") :uint64 '(:pointer) (list s)) "
        "(ffi:free-foreign s)))"),
        "5");
}

TEST(lisp_ffi_call_double)
{
    ASSERT_STR_EQ(eval_print(
        "(ffi:call-foreign (ffi:symbol-pointer \"pow\") :double '(:double :double) '(2.0d0 10.0d0))"),
        "1024.0d0");
}

TEST(lisp_ffi_callback_qsort)
{
    /* qsort an int array through a Lisp comparator callback. */
    ASSERT_STR_EQ(eval_print(
        "(let* ((arr (ffi:alloc-foreign 16)) "
        "       (cmp (ffi:make-callback :int32 '(:pointer :pointer) "
        "              (lambda (a b) (- (ffi:peek-i32 a) (ffi:peek-i32 b)))))) "
        "  (ffi:poke-i32 arr 9 0) (ffi:poke-i32 arr 2 4) "
        "  (ffi:poke-i32 arr 7 8) (ffi:poke-i32 arr 1 12) "
        "  (ffi:call-foreign (ffi:symbol-pointer \"qsort\") :void "
        "    '(:pointer :uint64 :uint64 :pointer) (list arr 4 4 cmp)) "
        "  (prog1 (list (ffi:peek-i32 arr 0) (ffi:peek-i32 arr 4) "
        "               (ffi:peek-i32 arr 8) (ffi:peek-i32 arr 12)) "
        "    (ffi:free-foreign arr)))"),
        "(1 2 7 9)");
}

TEST(lisp_ffi_free_callback)
{
    /* free-callback should reclaim the slot so it can be reused. */
    ASSERT_STR_EQ(eval_print(
        /* Create and immediately free CL_FFI_MAX_CALLBACKS+1 callbacks —
         * without free-callback this would exhaust the 64-slot table. */
        "(let ((n 0)) "
        "  (dotimes (i 65) "
        "    (let ((cb (ffi:make-callback :int32 '() (lambda () 42)))) "
        "      (ffi:free-callback cb) "
        "      (incf n))) "
        "  n)"),
        "65");
}

/* ---- The foreign-callback boundary (thread.h "Foreign-callback boundary",
 * builtins_ffi.c ffi_callback_handler): Lisp running inside a C callback
 * must never longjmp across the foreign caller's frames.  qsort is the
 * foreign caller here; the comparator is the callback. ---- */

/* A helper form: qsort four ints through comparator FN, return the sorted
 * list; the callback is freed on every exit. */
#define QSORT_WITH(fn) \
    "(let* ((arr (ffi:alloc-foreign 16)) " \
    "       (cmp (ffi:make-callback :int32 '(:pointer :pointer) " fn "))) " \
    "  (ffi:poke-i32 arr 9 0) (ffi:poke-i32 arr 2 4) " \
    "  (ffi:poke-i32 arr 7 8) (ffi:poke-i32 arr 1 12) " \
    "  (unwind-protect " \
    "      (progn (ffi:call-foreign (ffi:symbol-pointer \"qsort\") :void " \
    "               '(:pointer :uint64 :uint64 :pointer) (list arr 4 4 cmp)) " \
    "             (list (ffi:peek-i32 arr 0) (ffi:peek-i32 arr 4) " \
    "                   (ffi:peek-i32 arr 8) (ffi:peek-i32 arr 12))) " \
    "    (ffi:free-callback cmp) (ffi:free-foreign arr)))"

TEST(lisp_ffi_callback_error_is_deferred_to_caller)
{
    /* An unhandled ERROR inside the comparator does not unwind through
     * qsort: the callback returns 0, and once qsort has returned the
     * condition is re-signaled where a HANDLER-CASE around the foreign
     * call catches it -- with its original class and message. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case " QSORT_WITH("(lambda (a b) (declare (ignore a b)) (error \"boom ~A\" 42))")
        "  (simple-error (e) (format nil \"caught: ~A\" e)))"),
        "\"caught: boom 42\"");
    /* the class survives: a TYPE-ERROR is a TYPE-ERROR on the other side */
    ASSERT_STR_EQ(eval_print(
        "(handler-case " QSORT_WITH("(lambda (a b) (declare (ignore a b)) (car 5))")
        "  (type-error () :type-error) (error () :other))"),
        ":TYPE-ERROR");
    /* nothing left pending: the next foreign call is clean */
    ASSERT_STR_EQ(eval_print(QSORT_WITH("(lambda (a b) (- (ffi:peek-i32 a) (ffi:peek-i32 b)))")),
                  "(1 2 7 9)");
}

TEST(lisp_ffi_callback_error_not_seen_by_outer_handlers_inside)
{
    /* A HANDLER-BIND established around the foreign call is hidden while
     * the callback runs (its handler could transfer control across the C
     * frames) and runs at the re-signal instead: it sees the condition
     * exactly once, after qsort has returned. */
    ASSERT_STR_EQ(eval_print(
        "(let ((seen 0)) "
        "  (handler-case "
        "      (handler-bind ((error (lambda (c) (declare (ignore c)) (incf seen)))) "
        "        " QSORT_WITH("(lambda (a b) (declare (ignore a b)) (error \"x\"))") ") "
        "    (error () nil)) "
        "  seen)"),
        "1");
}

TEST(lisp_ffi_callback_nlx_across_boundary_refused)
{
    /* THROW / RETURN-FROM to a target outside the callback is not a
     * transfer but an error, caught at the boundary and deferred; the
     * message names the cause. */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (catch 'outer "
        "  " QSORT_WITH("(lambda (a b) (declare (ignore a b)) (throw 'outer :escaped))") ") "
        "  (error (e) (let ((m (format nil \"~A\" e))) "
        "               (list (and (search \"No catch for tag OUTER\" m) t) "
        "                     (and (search \"foreign callback\" m) t)))))"),
        "(T T)");
    ASSERT_STR_EQ(eval_print(
        "(handler-case (block outer "
        "  " QSORT_WITH("(lambda (a b) (declare (ignore a b)) (return-from outer :escaped))") ") "
        "  (error (e) (and (search \"foreign callback\" (format nil \"~A\" e)) t)))"),
        "T");
    /* an ABORT restart of the REPL is hidden too: no restart named ABORT */
    ASSERT_STR_EQ(eval_print(
        "(handler-case (restart-case "
        "  " QSORT_WITH("(lambda (a b) (declare (ignore a b)) (invoke-restart 'leave))") " "
        "  (leave () :left)) "
        "  (error () :refused))"),
        ":REFUSED");
}

TEST(lisp_ffi_callback_inner_handlers_and_nesting_work)
{
    /* HANDLER-CASE inside the callback is ordinary; a callback may itself
     * make a foreign call whose callback errors (handled inside). */
    ASSERT_STR_EQ(eval_print(
        QSORT_WITH("(lambda (a b) (handler-case (error \"inner\") "
                   "  (error () (- (ffi:peek-i32 a) (ffi:peek-i32 b)))))")),
        "(1 2 7 9)");
    ASSERT_STR_EQ(eval_print(
        QSORT_WITH("(lambda (a b) "
                   "  (handler-case " QSORT_WITH("(lambda (x y) (declare (ignore x y)) (error \"deep\"))")
                   "    (error () nil)) "
                   "  (- (ffi:peek-i32 a) (ffi:peek-i32 b)))")),
        "(1 2 7 9)");
    /* UNWIND-PROTECT cleanups inside the callback run on the way to the
     * boundary; a THROW to a catch INSIDE the callback is a normal exit */
    ASSERT_STR_EQ(eval_print(
        "(let ((cleaned 0)) "
        "  (list (handler-case " QSORT_WITH("(lambda (a b) (declare (ignore a b)) "
                                          "  (unwind-protect (error \"e\") (incf cleaned)))")
        "          (error () :caught)) "
        "        (> cleaned 0) "
        "        " QSORT_WITH("(lambda (a b) (catch 'in (throw 'in (- (ffi:peek-i32 a) (ffi:peek-i32 b)))))") "))"),
        "(:CAUGHT T (1 2 7 9))");
}

TEST(lisp_ffi_callback_on_worker_thread_under_list_churn)
{
    /* cl_thread_current_is_registered walks the thread list under a
     * non-blocking trylock (a foreign task must never block on it): a
     * callback on a Lisp WORKER, racing threads that register and
     * unregister, must still be recognised every time — a false "not
     * registered" would make the comparator silently return 0. */
    ASSERT_STR_EQ(eval_print(
        "(let ((churn (mp:make-thread (lambda () "
        "                (dotimes (i 100) (mp:join-thread (mp:make-thread (lambda () nil))))))) "
        "      (worker (mp:make-thread (lambda () "
        "                 (let ((bad 0)) "
        "                   (dotimes (i 150) "
        "                     (unless (equal " QSORT_WITH("(lambda (a b) (- (ffi:peek-i32 a) (ffi:peek-i32 b)))")
        "                                    '(1 2 7 9)) (incf bad))) "
        "                   bad))))) "
        "  (mp:join-thread churn) "
        "  (mp:join-thread worker))"),
        "0");
}

/* A callback hit from a thread the runtime never registered (a raw
 * pthread here; input.device's task on AmigaOS, which is where Intuition
 * runs a string gadget's edit hook) must not run Lisp: it answers 0, and
 * the hit is counted where Lisp can read it. */
static void *foreign_task_caller(void *cb)
{
    int32_t (*fn)(void *, void *) = (int32_t (*)(void *, void *))cb;
    static intptr_t result;
    result = fn(NULL, NULL);
    return &result;
}

TEST(lisp_ffi_callback_from_foreign_task_counted_not_run)
{
    CL_Obj cb;
    void *addr;
    void *ret;
    pthread_t th;
    int before_n = atoi(eval_print("(ext:%ffi-foreign-task-calls)"));
    cl_eval_string("(defvar cl-user::*foreign-task-ran* nil)");
    cb = cl_eval_string("(defparameter cl-user::*foreign-task-cb* "
                        "  (ffi:make-callback :int32 '(:pointer :pointer) "
                        "    (lambda (a b) (declare (ignore a b)) "
                        "      (setq cl-user::*foreign-task-ran* t) 42)))");
    cb = cl_eval_string("cl-user::*foreign-task-cb*");
    /* the host keeps a real pointer behind the 32-bit address field */
    addr = platform_ffi_resolve(((CL_ForeignPtr *)CL_OBJ_TO_PTR(cb))->address);
    ASSERT(addr != NULL);
    ASSERT(pthread_create(&th, NULL, foreign_task_caller, addr) == 0);
    ASSERT(pthread_join(th, &ret) == 0);
    ASSERT(*(intptr_t *)ret == 0);                       /* answered 0 */
    ASSERT_STR_EQ(eval_print("cl-user::*foreign-task-ran*"), "NIL");  /* never ran */
    {
        char expect[32];
        snprintf(expect, sizeof(expect), "%d", before_n + 1);
        ASSERT_STR_EQ(eval_print("(ext:%ffi-foreign-task-calls)"), expect);
    }
    /* the same callback on the Lisp thread still runs */
    ASSERT_STR_EQ(eval_print("(ffi:call-foreign cl-user::*foreign-task-cb* :int32 "
                             "  '(:pointer :pointer) (list (ffi:make-foreign-pointer 0) "
                             "                             (ffi:make-foreign-pointer 0)))"),
                  "42");
    ASSERT_STR_EQ(eval_print("cl-user::*foreign-task-ran*"), "T");
    eval_print("(ffi:free-callback cl-user::*foreign-task-cb*)");
}

TEST(lisp_ffi_callback_policy_variable)
{
    ASSERT_STR_EQ(eval_print("ext:*callback-error-policy*"), ":DEFER");
    ASSERT_STR_EQ(eval_print("(and (boundp 'ext:*callback-error-policy*) "
                             "     (eq (nth-value 1 (find-symbol \"*CALLBACK-ERROR-POLICY*\" \"EXT\")) :external))"),
                  "T");
}

TEST(lisp_ffi_make_callback_regs_argument)
{
    /* REGS (the AmigaOS register conventions) is accepted and validated
     * everywhere; on the host it is ignored -- the callback is a C
     * function and a register-spec comparator sorts like any other. */
    ASSERT_STR_EQ(eval_print(
        "(let* ((arr (ffi:alloc-foreign 8)) "
        "       (cmp (ffi:make-callback :int32 '(:pointer :pointer) "
        "              (lambda (a b) (- (ffi:peek-i32 a) (ffi:peek-i32 b))) '(:a0 :a1)))) "
        "  (ffi:poke-i32 arr 5 0) (ffi:poke-i32 arr 3 4) "
        "  (ffi:call-foreign (ffi:symbol-pointer \"qsort\") :void "
        "    '(:pointer :uint64 :uint64 :pointer) (list arr 2 4 cmp)) "
        "  (prog1 (list (ffi:peek-i32 arr 0) (ffi:peek-i32 arr 4)) "
        "    (ffi:free-callback cmp) (ffi:free-foreign arr)))"),
        "(3 5)");
    /* NIL entries = stack; a partial list is refused, so is an unknown
     * register, a non-list, and a non-function */
    ASSERT_STR_EQ(eval_print(
        "(let ((cb (ffi:make-callback :void '(:int32 :pointer) (lambda (a b) a b) '(nil :d0)))) "
        "  (ffi:free-callback cb) t)"),
        "T");
    ASSERT_STR_EQ(eval_print(
        "(list (handler-case (progn (ffi:make-callback :int32 '(:pointer :pointer) (lambda (a b) a b) '(:a0)) nil) "
        "        (error (e) (and (search \"one entry per argument\" (format nil \"~A\" e)) t))) "
        "      (handler-case (progn (ffi:make-callback :int32 '(:pointer) (lambda (a) a) '(:a9)) nil) "
        "        (error (e) (and (search \"unknown register A9\" (format nil \"~A\" e)) t))) "
        "      (handler-case (progn (ffi:make-callback :int32 '(:pointer) (lambda (a) a) '(:x0)) nil) "
        "        (error (e) (and (search \"unknown register X0\" (format nil \"~A\" e)) t))) "
        "      (handler-case (progn (ffi:make-callback :int32 '(:pointer) (lambda (a) a) :a0) nil) "
        "        (error (e) (and (search \"must be a list\" (format nil \"~A\" e)) t))) "
        "      (handler-case (progn (ffi:make-callback :int32 '() 42) nil) "
        "        (error (e) (and (search \"must be a function\" (format nil \"~A\" e)) t))))"),
        "(T T T T T)");
}
#endif

/* ================================================================
 * AMIGA package tests (POSIX: the package exists so lib/amiga/** can be
 * compiled and tested on the host; its builtins are error stubs — the
 * full contract is in tests/test_amiga_ffi.c)
 * ================================================================ */

#ifndef PLATFORM_AMIGA
TEST(lisp_amiga_package_present_on_posix_with_stubs)
{
    ASSERT_STR_EQ(eval_print("(package-name (find-package \"AMIGA\"))"), "\"AMIGA\"");
    ASSERT_STR_EQ(eval_print("(fboundp 'amiga:open-library)"), "T");
    /* the stub signals instead of returning a bogus base */
    ASSERT_STR_EQ(eval_print("(handler-case (amiga:open-library \"dos.library\" 0) "
                             "  (error () :platform-error))"), ":PLATFORM-ERROR");
}
#endif

int main(void)
{
    setup();

    printf("--- test_ffi ---\n");

    /* C-level tests */
    RUN(c_foreign_pointer_create);
    RUN(c_foreign_pointer_type_name);
    RUN(c_foreign_pointer_not_fixnum);
    RUN(c_foreign_pointer_zero_addr);
    RUN(c_foreign_pointer_gc_survives);

    /* Lisp-level FFI tests */
    RUN(lisp_ffi_package_exists);
    RUN(lisp_ffi_make_foreign_pointer);
    RUN(lisp_ffi_foreign_pointer_p);
    RUN(lisp_ffi_null_pointer_p);
    RUN(lisp_ffi_foreign_pointer_address);
    RUN(lisp_ffi_alloc_free);
    RUN(lisp_ffi_peek_poke_u32);
    RUN(lisp_ffi_peek_poke_u32_offset);
    RUN(lisp_ffi_peek_poke_u16);
    RUN(lisp_ffi_peek_poke_u8);
    RUN(lisp_ffi_poke_bytes_vector);
    RUN(lisp_ffi_poke_bytes_string);
    RUN(lisp_ffi_poke_bytes_offset_start_end);
    RUN(lisp_ffi_poke_bytes_empty);
    RUN(lisp_ffi_peek_bytes_roundtrip);
    RUN(lisp_ffi_poke_bytes_overrun_rejected);
    RUN(lisp_ffi_peek_bytes_overrun_rejected);
    RUN(lisp_ffi_poke_bytes_rank_rejected);
    RUN(lisp_ffi_peek_bytes_rank_rejected);
    RUN(lisp_ffi_poke_bytes_element_range_checked);
    RUN(lisp_ffi_poke_bytes_bad_span_and_source);
    RUN(lisp_ffi_poke_bytes_respects_fill_pointer);
    RUN(lisp_ffi_foreign_string);
    RUN(lisp_ffi_foreign_string_empty);
    RUN(lisp_ffi_foreign_string_long);
    RUN(lisp_ffi_foreign_string_bad_max_len);
    RUN(lisp_ffi_pointer_plus);
    RUN(lisp_ffi_printer);

    /* Typed peek/poke (signed, 64-bit, float/double, pointer) */
    RUN(lisp_ffi_peek_poke_signed);
    RUN(lisp_ffi_peek_poke_64);
    RUN(lisp_ffi_peek_poke_float);
    RUN(lisp_ffi_peek_poke_pointer);
    RUN(lisp_ffi_pointer_eq);

    /* AMIGA package tests */
#ifndef PLATFORM_AMIGA
    RUN(lisp_amiga_package_present_on_posix_with_stubs);

    /* Host-only: dlopen / libffi calls + callbacks */
    RUN(lisp_ffi_symbol_pointer_found);
    RUN(lisp_ffi_symbol_pointer_missing);
    RUN(lisp_ffi_close_library_is_the_host_one);
    RUN(lisp_ffi_call_int);
    RUN(lisp_ffi_call_pointer_arg);
    RUN(lisp_ffi_call_double);
    RUN(lisp_ffi_callback_qsort);
    RUN(lisp_ffi_free_callback);
    RUN(lisp_ffi_callback_error_is_deferred_to_caller);
    RUN(lisp_ffi_callback_error_not_seen_by_outer_handlers_inside);
    RUN(lisp_ffi_callback_nlx_across_boundary_refused);
    RUN(lisp_ffi_callback_from_foreign_task_counted_not_run);
    RUN(lisp_ffi_callback_inner_handlers_and_nesting_work);
    RUN(lisp_ffi_callback_on_worker_thread_under_list_churn);
    RUN(lisp_ffi_callback_policy_variable);
    RUN(lisp_ffi_make_callback_regs_argument);
#endif

    REPORT();
    teardown();
    return test_fail > 0 ? 1 : 0;
}
