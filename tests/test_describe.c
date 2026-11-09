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
#include <string.h>

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

/* Helper: evaluate expression and return result as C string */
static const char *eval_str(const char *expr)
{
    static char buf[1024];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj result = cl_eval_string(expr);
        cl_prin1_to_string(result, buf, sizeof(buf));
        CL_UNCATCH();
        return buf;
    } else {
        CL_UNCATCH();
        snprintf(buf, sizeof(buf), "ERROR:%d", err);
        return buf;
    }
}

/* Helper: evaluate describe via with-output-to-string, return captured output */
static const char *describe_output(const char *describe_expr)
{
    static char expr_buf[512];
    snprintf(expr_buf, sizeof(expr_buf),
        "(with-output-to-string (s) %s)", describe_expr);
    return eval_str(expr_buf);
}

/* --- Tests --- */

TEST(describe_fixnum)
{
    const char *out = describe_output("(describe 42 s)");
    ASSERT(strstr(out, "FIXNUM") != NULL);
    ASSERT(strstr(out, "42") != NULL);
}

TEST(describe_negative_fixnum)
{
    const char *out = describe_output("(describe -7 s)");
    ASSERT(strstr(out, "FIXNUM") != NULL);
    ASSERT(strstr(out, "-7") != NULL);
}

TEST(describe_nil)
{
    const char *out = describe_output("(describe nil s)");
    ASSERT(strstr(out, "NIL") != NULL);
    ASSERT(strstr(out, "NULL") != NULL);
}

TEST(describe_string)
{
    const char *out = describe_output("(describe \"hello\" s)");
    ASSERT(strstr(out, "STRING") != NULL);
    ASSERT(strstr(out, "Length") != NULL);
    ASSERT(strstr(out, "5") != NULL);
}

TEST(describe_symbol)
{
    const char *out = describe_output("(describe 'car s)");
    ASSERT(strstr(out, "SYMBOL") != NULL);
    ASSERT(strstr(out, "CAR") != NULL);
}

TEST(describe_symbol_with_value)
{
    /* T has a constant value */
    const char *out = describe_output("(describe t s)");
    ASSERT(strstr(out, "SYMBOL") != NULL);
    ASSERT(strstr(out, "CONSTANT") != NULL);
}

TEST(describe_cons)
{
    const char *out = describe_output("(describe '(1 2 3) s)");
    ASSERT(strstr(out, "CONS") != NULL);
    ASSERT(strstr(out, "Length") != NULL);
    ASSERT(strstr(out, "3") != NULL);
}

TEST(describe_vector)
{
    const char *out = describe_output("(describe (vector 10 20 30) s)");
    ASSERT(strstr(out, "VECTOR") != NULL);
    ASSERT(strstr(out, "Length") != NULL);
}

TEST(describe_hashtable)
{
    const char *out = describe_output(
        "(let ((h (make-hash-table))) "
        "  (setf (gethash 'a h) 1) "
        "  (describe h s))");
    ASSERT(strstr(out, "HASH-TABLE") != NULL);
    ASSERT(strstr(out, "Test") != NULL);
    ASSERT(strstr(out, "Count") != NULL);
}

TEST(describe_function)
{
    const char *out = describe_output("(describe #'car s)");
    ASSERT(strstr(out, "FUNCTION") != NULL);
    ASSERT(strstr(out, "built-in") != NULL);
    ASSERT(strstr(out, "CAR") != NULL);
}

TEST(describe_compiled_function)
{
    const char *out = describe_output(
        "(progn (defun my-test-fn (x) x) (describe #'my-test-fn s))");
    ASSERT(strstr(out, "COMPILED-FUNCTION") != NULL);
    ASSERT(strstr(out, "MY-TEST-FN") != NULL);
}

TEST(describe_character)
{
    const char *out = describe_output("(describe #\\A s)");
    ASSERT(strstr(out, "CHARACTER") != NULL);
    ASSERT(strstr(out, "65") != NULL);
}

TEST(describe_pathname)
{
    const char *out = describe_output("(describe #P\"/tmp/foo.txt\" s)");
    ASSERT(strstr(out, "PATHNAME") != NULL);
    ASSERT(strstr(out, "Name") != NULL);
}

TEST(describe_stream)
{
    /* Describe the string stream itself */
    const char *out = describe_output(
        "(let ((x (make-string-output-stream))) (describe x s))");
    ASSERT(strstr(out, "STREAM") != NULL);
    ASSERT(strstr(out, "OUTPUT") != NULL);
}

TEST(describe_struct)
{
    const char *out = describe_output(
        "(progn (defstruct point x y) (describe (make-point :x 1 :y 2) s))");
    ASSERT(strstr(out, "STRUCTURE") != NULL);
    ASSERT(strstr(out, "POINT") != NULL);
}

TEST(describe_returns_nil)
{
    /* describe should return NIL */
    const char *result = eval_str("(describe 42)");
    ASSERT(strstr(result, "NIL") != NULL);
}

/* --- Room --- */

TEST(room_no_error)
{
    const char *result = eval_str("(progn (room) t)");
    ASSERT(strstr(result, "T") != NULL);
}

TEST(room_writes_to_standard_output)
{
    const char *out = eval_str(
        "(with-output-to-string (*standard-output*) (room))");
    ASSERT(strstr(out, "Heap:") != NULL);
    ASSERT(strstr(out, "GC:") != NULL);
}

/* The collector's own lines (gcb_stats_report) are part of ROOM's output
 * and go through *standard-output* like the rest of it -- a backend that
 * wrote them to the raw console would leave the captured string without
 * them. */
TEST(room_reports_the_gc_backend)
{
    const char *out = eval_str(
        "(with-output-to-string (*standard-output*) (room))");
#ifdef CL_GC_IMMIX
    ASSERT(strstr(out, "Immix: ") != NULL);
    ASSERT(strstr(out, " lines of ") != NULL);
    ASSERT(strstr(out, "policy compactions") != NULL);
    ASSERT(strstr(out, "Classic: ") == NULL);
#else
    ASSERT(strstr(out, "Classic: free list") != NULL);
    ASSERT(strstr(out, "probe steps") != NULL);
    ASSERT(strstr(out, "Immix: ") == NULL);
#endif
}

/* ROOM's "(N%)" is used*100/total.  The product overflowed uint32 once
 * `used` passed 42 MB, so a 256 MB heap with 215 MB in use printed "(0%)".
 * cl_percent_of forms it in 64 bits; these are the exact values from the
 * field report plus the boundaries. */
TEST(room_percent_survives_large_heaps)
{
    ASSERT_EQ(80, (int)cl_percent_of(215136400u, 268435456u));  /* the report */
    ASSERT_EQ(0,  (int)cl_percent_of(0u, 268435456u));
    ASSERT_EQ(100, (int)cl_percent_of(268435456u, 268435456u));
    ASSERT_EQ(50, (int)cl_percent_of(2147483648u, 4294967295u)); /* 2 GB of ~4 GB */
    ASSERT_EQ(99, (int)cl_percent_of(4294967294u, 4294967295u));
    ASSERT_EQ(16, (int)cl_percent_of(43000000u, 268435456u));   /* just past the old overflow point */
    ASSERT_EQ(1,  (int)cl_percent_of(50000u, 4194304u));        /* small heap, unchanged */
    ASSERT_EQ(0,  (int)cl_percent_of(5u, 0u));                   /* no arena: no division */
}

/* The percentage ROOM prints must agree with the two byte counts on the
 * same line, whatever the heap size. */
TEST(room_percent_matches_its_byte_counts)
{
    const char *out = eval_str(
        "(with-output-to-string (*standard-output*) (room))");
    unsigned long used = 0, total = 0, pct = 999;
    const char *line = strstr(out, "Heap:");
    ASSERT(line != NULL);
    line = strchr(line, '\n');
    ASSERT(line != NULL);
    ASSERT_EQ(3, sscanf(line + 1, "  %lu / %lu bytes used (%lu%%)",
                        &used, &total, &pct));
    ASSERT(total > 0);
    ASSERT(used <= total);
    ASSERT_EQ((int)(used * 100 / total), (int)pct);
}

int main(void)
{
    test_init();
    setup();

    RUN(describe_fixnum);
    RUN(describe_negative_fixnum);
    RUN(describe_nil);
    RUN(describe_string);
    RUN(describe_symbol);
    RUN(describe_symbol_with_value);
    RUN(describe_cons);
    RUN(describe_vector);
    RUN(describe_hashtable);
    RUN(describe_function);
    RUN(describe_compiled_function);
    RUN(describe_character);
    RUN(describe_pathname);
    RUN(describe_stream);
    RUN(describe_struct);
    RUN(describe_returns_nil);
    RUN(room_no_error);
    RUN(room_writes_to_standard_output);
    RUN(room_reports_the_gc_backend);
    RUN(room_percent_survives_large_heaps);
    RUN(room_percent_matches_its_byte_counts);

    teardown();
    REPORT();
}
