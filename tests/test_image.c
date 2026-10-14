/*
 * test_image.c — heap image save/restore units (specs/image-save-load.md).
 *
 * Uses the shutdown/re-init harness ("second life in one process"): life 1
 * runs the full C init, builds session state, saves an image through the
 * production deferred-save path; life 2 re-initializes the runtime from
 * scratch and restores, then asserts state fidelity.  The two lives run the
 * IDENTICAL init sequence, which is exactly the property the image format
 * relies on (boot-root index i names the same C variable in both).
 *
 * Also covered: header/fingerprint/structure rejection (each a distinct
 * clean refusal BEFORE the heap is touched), the at-rest gating of the
 * deferred dump, save preconditions (open file stream, active FASL
 * reader), the builtin relink registry including the stale-builtin stub,
 * outbuf/readtable/TLV round-trips, and the SOURCES section (each source
 * file's name stored once; a bad index refused pre-arena).
 *
 * The end-to-end two-process legs (CLI flags, discovery, :quit,
 * .amilisprc interplay) are tests/test_image.sh.
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
#include "core/thread.h"
#include "core/vm.h"
#include "core/stream.h"
#include "core/builtins.h"
#include "core/debugger.h"
#include "core/repl.h"
#include "core/image.h"
#include "core/fasl.h"
#include "platform/platform.h"
#include <string.h>
#include <stdio.h>

#define IMG_PATH "build/host/test_image_unit.img"

static void full_init(uint32_t heap)
{
    platform_init();
    cl_thread_init();
    cl_error_init();
    cl_mem_init(heap);
    cl_package_init();
    cl_symbol_init();
    cl_reader_init();
    cl_printer_init();
    cl_compiler_init();
    cl_vm_init(0, 0);
    cl_stream_init();
    cl_builtins_init();
    cl_debugger_init();
    /* Same point as main.c: end of C init, before any boot Lisp. */
    cl_image_note_boot_roots();
    cl_repl_init_minimal();
}

static void full_teardown(void)
{
    cl_current_source_file = NULL;
    cl_stream_shutdown();
    cl_vm_shutdown();
    cl_mem_shutdown();
    platform_shutdown();
}

static int truthy(const char *expr)
{
    return cl_eval_string(expr) != CL_NIL;
}

static int eval_int(const char *expr)
{
    CL_Obj v = cl_eval_string(expr);
    return CL_FIXNUM_P(v) ? (int)CL_FIXNUM_VAL(v) : -999999;
}

/* Run the production deferred-save path to completion and report whether
 * the dump actually executed. */
static int save_now(const char *path)
{
    char form[256];
    snprintf(form, sizeof(form), "(ext:save-image \"%s\")", path);
    cl_eval_string(form);
    if (!cl_image_save_pending_p()) return 0;
    cl_image_save_run_if_pending();
    return !cl_image_save_pending_p() && platform_file_exists(path);
}

/* ---------- life 1 -> life 2 round trip ---------- */

TEST(save_restore_second_life)
{
    /* Life 1: build session state exercising every fixup class. */
    full_init(CL_DEFAULT_HEAP_SIZE);
    cl_eval_string("(defun ti-fib (n) (if (< n 2) n "
                   "(+ (ti-fib (- n 1)) (ti-fib (- n 2)))))");
    cl_eval_string("(defvar *ti-answer* 42)");
    cl_eval_string("(defvar *ti-ht* (make-hash-table :test 'eq))");
    cl_eval_string("(defvar *ti-key* (cons 1 2))");
    cl_eval_string("(%setf-gethash *ti-key* *ti-ht* 'hit)");
    cl_eval_string("(defvar *ti-sos* (make-string-output-stream))");
    cl_eval_string("(write-string \"pre-save;\" *ti-sos*)");
    cl_eval_string("(defvar *ti-close* (let ((n 7)) (lambda (x) (+ x n))))");
    ASSERT_EQ_INT(eval_int("(ti-fib 10)"), 55);

    ASSERT(save_now(IMG_PATH));

    /* State must be untouched in the SAVING session too. */
    ASSERT_EQ_INT(eval_int("(ti-fib 10)"), 55);
    full_teardown();

    /* Life 2: identical init, restore, verify. */
    full_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 0), 0);
    ASSERT_EQ_INT(cl_image_restore_staged(), 0);
    ASSERT(cl_image_restored_p());

    /* Bytecode blobs re-attached: a saved function runs. */
    ASSERT_EQ_INT(eval_int("(ti-fib 12)"), 144);
    /* Globals and EQ-hashtable identity (offset hashing preserved). */
    ASSERT_EQ_INT(eval_int("*ti-answer*"), 42);
    ASSERT(truthy("(eq (gethash *ti-key* *ti-ht*) 'hit)"));
    /* Closure over a cell. */
    ASSERT_EQ_INT(eval_int("(funcall *ti-close* 10)"), 17);
    /* String-output-stream buffer content (outbuf blob). */
    cl_eval_string("(write-string \"post\" *ti-sos*)");
    ASSERT(truthy("(string= (get-output-stream-string *ti-sos*) "
                  "\"pre-save;post\")"));
    /* EXT:*IMAGE-RESTORED-P* is T. */
    ASSERT(truthy("ext:*image-restored-p*"));
    /* The heap survives a GC + compaction after restore. */
    cl_eval_string("(dotimes (i 10000) (cons i i))");
    cl_gc();
    cl_gc_compact();
    ASSERT_EQ_INT(eval_int("(ti-fib 10)"), 55);
    ASSERT(truthy("(eq (gethash *ti-key* *ti-ht*) 'hit)"));
    full_teardown();
}

/* ---------- JIT state across an image ---------- */

static CL_Bytecode *fn_bytecode(const char *fn_expr)
{
    CL_Obj fn = cl_eval_string(fn_expr);
    if (CL_CLOSURE_P(fn))
        fn = ((CL_Closure *)CL_OBJ_TO_PTR(fn))->bytecode;
    return CL_BYTECODE_P(fn) ? (CL_Bytecode *)CL_OBJ_TO_PTR(fn) : NULL;
}

TEST(restore_restarts_jit_call_count_keeps_speed_hint)
{
    /* Native code never survives an image, so neither may the JIT's call
     * count: the saving session had settled most functions (compiled or
     * rejected them), and a settled count restored as-is would keep the
     * function from ever being compiled again -- every m68k image ran as
     * bytecode before 0.12.  The (speed 3) hint stays. */
    CL_Bytecode *bc;

    full_init(CL_DEFAULT_HEAP_SIZE);
    cl_eval_string("(defun ti-hot (x) (+ x 1))");
    cl_eval_string("(defun ti-fast (x) (declare (optimize (speed 3))) (* x 2))");
    bc = fn_bytecode("#'ti-hot");
    ASSERT(bc != NULL);
    bc->jit_hot = CL_BC_JIT_SETTLED;
    bc = fn_bytecode("#'ti-fast");
    ASSERT(bc != NULL);
    ASSERT(bc->jit_hot & CL_BC_JIT_SPEED);
    bc->jit_hot = CL_BC_JIT_SPEED | CL_BC_JIT_SETTLED;
    ASSERT(save_now(IMG_PATH));
    full_teardown();

    full_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 0), 0);
    ASSERT_EQ_INT(cl_image_restore_staged(), 0);
    bc = fn_bytecode("#'ti-hot");
    ASSERT(bc != NULL);
    ASSERT_EQ_INT(bc->jit_hot, 0);
    bc = fn_bytecode("#'ti-fast");
    ASSERT(bc != NULL);
    ASSERT_EQ_INT(bc->jit_hot, CL_BC_JIT_SPEED);
    ASSERT_EQ_INT(eval_int("(+ (ti-hot 1) (ti-fast 2))"), 6);
    full_teardown();
}

TEST(restore_into_larger_heap)
{
    full_init(CL_DEFAULT_HEAP_SIZE);
    cl_eval_string("(defvar *ti-big* 12321)");
    ASSERT(save_now(IMG_PATH));
    full_teardown();

    /* Save with the default heap, restore with a 3x arena. */
    full_init(CL_DEFAULT_HEAP_SIZE * 3);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 0), 0);
    ASSERT_EQ_INT(cl_image_restore_staged(), 0);
    ASSERT_EQ_INT(eval_int("*ti-big*"), 12321);
    full_teardown();
}

TEST(restore_too_small_heap_refused_pre_arena)
{
    uint32_t bump;
    full_init(CL_DEFAULT_HEAP_SIZE);
    /* ~1.2MB of live payload so the image cannot fit a 768K arena. */
    cl_eval_string("(defvar *ti-fat* (make-array 300000 :initial-element 5))");
    ASSERT(save_now(IMG_PATH));
    full_teardown();

    /* An arena smaller than the payload must be refused BEFORE the heap
     * is touched — the session then boots normally. */
    full_init(768 * 1024);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 0), 0);
    bump = cl_image_staged_bump();
    ASSERT(bump > 768 * 1024);
    ASSERT(cl_image_restore_staged() != 0);
    cl_image_discard_staged();
    /* The un-restored session still works. */
    ASSERT_EQ_INT(eval_int("(+ 1 2)"), 3);
    full_teardown();
}

/* ---------- deferred-dump gating ---------- */

TEST(save_deferred_until_at_rest)
{
    CL_Obj pin = CL_NIL;
    full_init(CL_DEFAULT_HEAP_SIZE);
    platform_file_delete(IMG_PATH);

    cl_eval_string("(ext:save-image \"" IMG_PATH "\")");
    ASSERT(cl_image_save_pending_p());
    ASSERT(!platform_file_exists(IMG_PATH));

    /* A live GC-protected local means the main thread is NOT at rest:
     * the dump must stay pending. */
    CL_GC_PROTECT(pin);
    ASSERT_EQ_INT(cl_image_save_run_if_pending(), 0);
    ASSERT(cl_image_save_pending_p());
    ASSERT(!platform_file_exists(IMG_PATH));
    CL_GC_UNPROTECT(1);

    /* At rest: the dump executes. */
    ASSERT_EQ_INT(cl_image_save_run_if_pending(), 0);
    ASSERT(!cl_image_save_pending_p());
    ASSERT(platform_file_exists(IMG_PATH));
    full_teardown();
}

/* ---------- preconditions ---------- */

TEST(save_refused_with_open_file_stream)
{
    int err;
    full_init(CL_DEFAULT_HEAP_SIZE);
    cl_eval_string("(defvar *ti-f* (open \"build/host/ti-open.tmp\" "
                   ":direction :output :if-exists :supersede))");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(ext:save-image \"" IMG_PATH "\")");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT_EQ_INT(err, CL_ERR_GENERAL);
    ASSERT(!cl_image_save_pending_p());
    cl_eval_string("(close *ti-f*)");
    platform_file_delete("build/host/ti-open.tmp");
    full_teardown();
}

TEST(save_refused_with_active_fasl_reader)
{
    static CL_FaslReader r;
    int err;
    full_init(CL_DEFAULT_HEAP_SIZE);
    memset(&r, 0, sizeof(r));
    cl_fasl_reader_register(&r);
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        cl_eval_string("(ext:save-image \"" IMG_PATH "\")");
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    cl_fasl_reader_unregister(&r);
    ASSERT_EQ_INT(err, CL_ERR_GENERAL);
    ASSERT(!cl_image_save_pending_p());
    full_teardown();
}

/* ---------- builtin relink registry ---------- */

TEST(builtin_registry_resolves_and_misses)
{
    full_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT(cl_builtin_registry_lookup("COMMON-LISP", 11, "CAR", 3) != NULL);
    ASSERT(cl_builtin_registry_lookup("EXT", 3, "SAVE-IMAGE", 10) != NULL);
    ASSERT(cl_builtin_registry_lookup("MP", 2, "MAKE-LOCK", 9) != NULL);
    /* The compiler's inlined-macro stub must be relinkable by name. */
    ASSERT(cl_builtin_registry_lookup("AMILISP", 7,
                                      "%INLINED-MACRO-STUB", 19) != NULL);
    ASSERT(cl_builtin_registry_lookup("COMMON-LISP", 11,
                                      "NO-SUCH-BUILTIN-XYZ", 19) == NULL);
    ASSERT(cl_builtin_registry_lookup("NO-SUCH-PKG", 11, "CAR", 3) == NULL);
    full_teardown();
}

TEST(stale_builtin_stub_signals)
{
    int err;
    full_init(CL_DEFAULT_HEAP_SIZE);
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        bi_stale_builtin(NULL, 0);
        CL_UNCATCH();
    } else {
        CL_UNCATCH();
    }
    ASSERT_EQ_INT(err, CL_ERR_UNDEFINED);
    full_teardown();
}

/* ---------- staging rejection paths ---------- */

/* Write COUNT bytes to PATH. */
static void write_file(const char *path, const void *data, uint32_t count)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(data, 1, count, f);
        fclose(f);
    }
}

/* Load IMG_PATH into buf; returns size (0 on failure). */
static unsigned long slurp(const char *path, char **buf)
{
    unsigned long size = 0;
    *buf = platform_file_read(path, &size);
    return *buf ? size : 0;
}

TEST(stage_rejects_garbage_truncation_version_fingerprint)
{
    char *buf;
    unsigned long size;

    full_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT(save_now(IMG_PATH));

    /* Not an image at all. */
    write_file("build/host/ti-bad.img", "hello, not an image", 19);
    ASSERT(cl_image_stage("build/host/ti-bad.img", 1) != 0);
    ASSERT(!cl_image_staged_p());

    size = slurp(IMG_PATH, &buf);
    ASSERT(size > 100);
    if (size > 100) {
        /* Truncated: header intact, sections cut off — refused at stage
         * (structural pre-parse) or pre-arena at restore; stage-time
         * structural checks only cover the header, so accept either by
         * asserting the RESTORE path refuses cleanly. */
        write_file("build/host/ti-bad.img", buf, (uint32_t)(size / 2));
        if (cl_image_stage("build/host/ti-bad.img", 1) == 0) {
            ASSERT(cl_image_restore_staged() != 0);
            cl_image_discard_staged();
        }

        /* Wrong format version. */
        memcpy(buf + 4, "\xEE\xEE", 2);
        write_file("build/host/ti-bad.img", buf, (uint32_t)size);
        ASSERT(cl_image_stage("build/host/ti-bad.img", 1) != 0);
        platform_free(buf);
    }

    /* Wrong fingerprint. */
    size = slurp(IMG_PATH, &buf);
    if (size > 100) {
        buf[12] = (char)(buf[12] ^ 0xFF);
        write_file("build/host/ti-bad.img", buf, (uint32_t)size);
        ASSERT(cl_image_stage("build/host/ti-bad.img", 1) != 0);
        platform_free(buf);
    }

    /* Boot-root count mismatch (n_roots and boot_roots bumped together so
     * the header stays structurally consistent): must be refused at
     * RESTORE time, before the arena is touched. */
    size = slurp(IMG_PATH, &buf);
    if (size > 100) {
        uint32_t n_roots, boot_roots;
        memcpy(&n_roots, buf + 48, 4);
        memcpy(&boot_roots, buf + 56, 4);
        n_roots += 1;
        boot_roots += 1;
        memcpy(buf + 48, &n_roots, 4);
        memcpy(buf + 56, &boot_roots, 4);
        write_file("build/host/ti-bad.img", buf, (uint32_t)size);
        ASSERT_EQ_INT(cl_image_stage("build/host/ti-bad.img", 1), 0);
        ASSERT(cl_image_restore_staged() != 0);
        cl_image_discard_staged();
        platform_free(buf);
    }

    platform_file_delete("build/host/ti-bad.img");
    /* Session unharmed by all the refusals. */
    ASSERT_EQ_INT(eval_int("(+ 20 22)"), 42);
    full_teardown();
}

/* Staging reads only the header; the payload is read by the restore, after
 * the arena exists (reading a 1.6 MB image first left no 8 MB block for the
 * arena on a 24 MB 68020).  So the file is opened twice, and whatever happens
 * to it in between must be refused BEFORE the arena is touched. */
TEST(stage_reads_header_only_payload_read_at_restore)
{
    char *buf;
    unsigned long size;

    full_init(CL_DEFAULT_HEAP_SIZE);
    cl_eval_string("(defvar *ti-late* 7)");
    ASSERT(save_now(IMG_PATH));
    full_teardown();

    size = slurp(IMG_PATH, &buf);
    ASSERT(size > CL_IMAGE_HEADER_BYTES);
    if (size <= CL_IMAGE_HEADER_BYTES) return;

    full_init(CL_DEFAULT_HEAP_SIZE);

    /* Deleted after staging: staging succeeded on the header alone, the
     * restore cannot read the payload and refuses. */
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 1), 0);
    ASSERT(cl_image_staged_p());
    ASSERT(cl_image_staged_bump() >= CL_ALIGN);
    platform_file_delete(IMG_PATH);
    ASSERT(cl_image_restore_staged() != 0);
    cl_image_discard_staged();
    ASSERT(!cl_image_staged_p());
    ASSERT_EQ_INT(cl_image_staged_bump(), 0);

    /* Grew after staging (size differs): refused. */
    write_file(IMG_PATH, buf, (uint32_t)size);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 1), 0);
    {
        FILE *f = fopen(IMG_PATH, "ab");
        if (f) { fputs("tail", f); fclose(f); }
    }
    ASSERT(cl_image_restore_staged() != 0);
    cl_image_discard_staged();

    /* Same size, different header (another image written in its place):
     * refused. */
    write_file(IMG_PATH, buf, (uint32_t)size);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 1), 0);
    buf[12] = (char)(buf[12] ^ 0xFF);
    write_file(IMG_PATH, buf, (uint32_t)size);
    buf[12] = (char)(buf[12] ^ 0xFF);
    ASSERT(cl_image_restore_staged() != 0);
    cl_image_discard_staged();

    /* Every refusal left the session intact. */
    ASSERT_EQ_INT(eval_int("(+ 40 2)"), 42);

    /* Unchanged file: the payload read at restore time restores. */
    write_file(IMG_PATH, buf, (uint32_t)size);
    platform_free(buf);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 1), 0);
    ASSERT_EQ_INT(cl_image_restore_staged(), 0);
    ASSERT_EQ_INT(eval_int("*ti-late*"), 7);
    full_teardown();
}

/* ---------- the SOURCES section (image v5) ---------- */

/* Occurrences of S in BUF[0..SIZE). */
static int count_bytes(const char *buf, unsigned long size, const char *s)
{
    unsigned long n = strlen(s), i;
    int count = 0;
    for (i = 0; i + n <= size; i++)
        if (memcmp(buf + i, s, n) == 0) count++;
    return count;
}

/* Offset of the SOURCES section (its u32 count) in the image file BUF:
 * past the header, ROOTS, READTABLES and THREAD0. */
static unsigned long sources_offset(const char *buf)
{
    uint32_t n_roots, rt_bytes, n_tlv;
    unsigned long pos = CL_IMAGE_HEADER_BYTES;
    memcpy(&n_roots, buf + 48, 4);
    pos += (unsigned long)n_roots * 4;
    memcpy(&rt_bytes, buf + pos + 4, 4);
    pos += 8 + rt_bytes;
    memcpy(&n_tlv, buf + pos + 8, 4);
    pos += 12 + (unsigned long)n_tlv * 8;
    return pos;
}

/* Offset of the source index of the first blob that has one (0 if none),
 * walking the BLOBS section of the image file BUF. */
static unsigned long first_source_index_offset(const char *buf,
                                               unsigned long size)
{
    unsigned long pos = sources_offset(buf);
    uint32_t n_sources, n_blobs, i;
    memcpy(&n_sources, buf + pos, 4);
    pos += 4;
    for (i = 0; i < n_sources; i++) {
        uint16_t len;
        memcpy(&len, buf + pos, 2);
        pos += 2 + (unsigned long)len;
    }
    memcpy(&n_blobs, buf + 52, 4);
    for (i = 0; i < n_blobs && pos < size; i++) {
        uint32_t code_len;
        uint16_t n_const, lm_count, idx;
        uint8_t n_keys;
        pos += 4;                                       /* bc_offset */
        memcpy(&code_len, buf + pos, 4);
        pos += 4 + (unsigned long)code_len;
        memcpy(&n_const, buf + pos, 2);
        pos += 2 + (unsigned long)n_const * 4;
        n_keys = (uint8_t)buf[pos];
        pos += 1 + (unsigned long)n_keys * 6;
        memcpy(&lm_count, buf + pos, 2);
        pos += 2 + (unsigned long)lm_count * sizeof(CL_LineEntry);
        memcpy(&idx, buf + pos, 2);
        if (idx) return pos;
        pos += 2;
    }
    return 0;
}

static const char *fn_source_file(const char *fn_expr)
{
    CL_Bytecode *bc = fn_bytecode(fn_expr);
    return bc ? bc->source_file : NULL;
}

TEST(source_files_stored_once_and_shared_after_restore)
{
    char *buf;
    unsigned long size, pos;
    uint32_t n_sources;
    uint16_t bad_idx;
    const char *one_a, *one_b, *two;

    /* Life 1: two "files" (the compiler records cl_current_source_file),
     * one with a nested lambda, and a function from no file at all. */
    full_init(CL_DEFAULT_HEAP_SIZE);
    cl_current_source_file = cl_intern_source_file("ti/src-one.lisp");
    cl_eval_string("(defun ti-src-1 (x) (+ x 1))");
    cl_eval_string("(defun ti-src-2 (x) (lambda (y) (+ x y)))");
    cl_current_source_file = cl_intern_source_file("ti/src-two.lisp");
    cl_eval_string("(defun ti-src-3 (x) (* x 3))");
    cl_current_source_file = NULL;
    cl_eval_string("(defun ti-src-none (x) x)");
    ASSERT(save_now(IMG_PATH));
    full_teardown();

    /* Each name is in the file once, however many functions share it. */
    size = slurp(IMG_PATH, &buf);
    ASSERT(size > 100);
    if (size <= 100) return;
    ASSERT_EQ_INT(count_bytes(buf, size, "ti/src-one.lisp"), 1);
    ASSERT_EQ_INT(count_bytes(buf, size, "ti/src-two.lisp"), 1);

    /* Life 2: every function gets its file back, and the functions of one
     * file share one interned name again. */
    full_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 0), 0);
    ASSERT_EQ_INT(cl_image_restore_staged(), 0);
    one_a = fn_source_file("#'ti-src-1");
    one_b = fn_source_file("(ti-src-2 1)");
    two = fn_source_file("#'ti-src-3");
    ASSERT(one_a != NULL && one_b != NULL && two != NULL);
    if (one_a && two) {
        ASSERT_STR_EQ(one_a, "ti/src-one.lisp");
        ASSERT_STR_EQ(two, "ti/src-two.lisp");
    }
    ASSERT(one_a == one_b);
    ASSERT(fn_source_file("#'ti-src-none") == NULL);
    ASSERT(truthy("(equal (car (ext:function-source-location #'ti-src-3)) "
                  "\"ti/src-two.lisp\")"));
    full_teardown();

    /* A blob naming a source past the table, and a table larger than a
     * u16 index reaches: both refused before the heap is touched -- the
     * good image still restores in the same (virgin) life afterwards. */
    pos = first_source_index_offset(buf, size);
    ASSERT(pos != 0);
    full_init(CL_DEFAULT_HEAP_SIZE);
    if (pos) {
        memcpy(&n_sources, buf + sources_offset(buf), 4);
        bad_idx = (uint16_t)(n_sources + 1);
        memcpy(buf + pos, &bad_idx, 2);
        write_file("build/host/ti-bad.img", buf, (uint32_t)size);
        ASSERT_EQ_INT(cl_image_stage("build/host/ti-bad.img", 1), 0);
        ASSERT(cl_image_restore_staged() != 0);
        cl_image_discard_staged();

        n_sources = 0x10000u;
        memcpy(buf + sources_offset(buf), &n_sources, 4);
        write_file("build/host/ti-bad.img", buf, (uint32_t)size);
        ASSERT_EQ_INT(cl_image_stage("build/host/ti-bad.img", 1), 0);
        ASSERT(cl_image_restore_staged() != 0);
        cl_image_discard_staged();
        platform_file_delete("build/host/ti-bad.img");
    }
    platform_free(buf);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 0), 0);
    ASSERT_EQ_INT(cl_image_restore_staged(), 0);
    ASSERT_EQ_INT(eval_int("(ti-src-3 14)"), 42);
    full_teardown();
}

/* ---------- readtable + TLV + save hooks ---------- */

TEST(readtable_and_hooks_round_trip)
{
    full_init(CL_DEFAULT_HEAP_SIZE);
    /* Custom reader macro: ! reads as 42.  (READ-FROM-STRING lives in
     * boot.lisp, which minimal init skips — go through the reader
     * directly by evaluating a form containing the macro character.) */
    cl_eval_string("(set-macro-character #\\! "
                   "(lambda (s c) (declare (ignore s c)) 42))");
    ASSERT_EQ_INT(eval_int("(+ ! 0)"), 42);
    /* A save hook that records it ran (the hook list itself lives in the
     * image, so the restore can see the marker). */
    cl_eval_string("(defvar *ti-save-hook-ran* nil)");
    cl_eval_string("(defvar *ti-restore-hook-ran* nil)");
    /* PUSH is a boot.lisp macro; minimal init means SETQ + CONS. */
    cl_eval_string("(setq ext:*save-hooks* "
                   "(cons (lambda () (setq *ti-save-hook-ran* t)) "
                   "ext:*save-hooks*))");
    cl_eval_string("(setq ext:*restore-hooks* "
                   "(cons (lambda () (setq *ti-restore-hook-ran* t)) "
                   "ext:*restore-hooks*))");
    ASSERT(save_now(IMG_PATH));
    ASSERT(truthy("*ti-save-hook-ran*"));
    full_teardown();

    full_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 0), 0);
    ASSERT_EQ_INT(cl_image_restore_staged(), 0);
    /* Readtable customization survived (READTABLES section). */
    ASSERT_EQ_INT(eval_int("(+ ! 0)"), 42);
    /* Restore hooks run through the production entry point. */
    ASSERT(truthy("(not *ti-restore-hook-ran*)"));
    cl_image_run_restore_hooks();
    ASSERT(truthy("*ti-restore-hook-ran*"));
    full_teardown();
}

/* ---------- condvar + dead-thread wrapper restore ---------- */

/* Covers the two TYPE_CONDVAR/TYPE_THREAD restore branches that
 * save_restore_second_life doesn't exercise: a condition variable (only
 * MP:LOCK was covered before) and a non-main thread wrapper whose worker
 * died with the saving process (only the live-main-thread wrapper was
 * covered before). Both are legal to save: SAVE-IMAGE only forbids STILL
 * RUNNING worker threads (cl_thread_count > 1), not a joined wrapper kept
 * alive by a global. */
TEST(condvar_and_dead_thread_restore)
{
    full_init(CL_DEFAULT_HEAP_SIZE);
    cl_eval_string("(defvar *ti-cv* (mp:make-condition-variable \"ti-cv\"))");
    cl_eval_string("(defvar *ti-dead-thr* (mp:make-thread (lambda () 99)))");
    cl_eval_string("(mp:join-thread *ti-dead-thr*)");
    ASSERT(save_now(IMG_PATH));
    full_teardown();

    full_init(CL_DEFAULT_HEAP_SIZE);
    ASSERT_EQ_INT(cl_image_stage(IMG_PATH, 0), 0);
    ASSERT_EQ_INT(cl_image_restore_staged(), 0);

    /* TYPE_CONDVAR: a condvar is a heap word (specs/mp-locks-heap-words.md);
     * restore zeroes its waiter count, so the restored condvar is usable,
     * not just type-tagged — CONDITION-NOTIFY on it must not signal. */
    ASSERT(truthy("(mp:condition-variable-p *ti-cv*)"));
    cl_eval_string("(mp:condition-notify *ti-cv*)");

    /* TYPE_THREAD, non-main wrapper: the worker died with the saving
     * process, so restore forces a table_gen mismatch against slot 0.
     * The wrapper must report not-alive, and JOIN-THREAD must signal a
     * clean "already exited" error rather than joining the live main
     * thread's unrelated slot or crashing. */
    ASSERT(!truthy("(mp:thread-alive-p *ti-dead-thr*)"));
    {
        int err;
        CL_CATCH(err);
        if (err == CL_ERR_NONE) {
            cl_eval_string("(mp:join-thread *ti-dead-thr*)");
            CL_UNCATCH();
        } else {
            CL_UNCATCH();
        }
        ASSERT_EQ_INT(err, CL_ERR_GENERAL);
    }
    full_teardown();
}

int main(void)
{
    test_init();
    RUN(save_restore_second_life);
    RUN(restore_restarts_jit_call_count_keeps_speed_hint);
    RUN(restore_into_larger_heap);
    RUN(restore_too_small_heap_refused_pre_arena);
    RUN(save_deferred_until_at_rest);
    RUN(save_refused_with_open_file_stream);
    RUN(save_refused_with_active_fasl_reader);
    RUN(builtin_registry_resolves_and_misses);
    RUN(stale_builtin_stub_signals);
    RUN(stage_rejects_garbage_truncation_version_fingerprint);
    RUN(stage_reads_header_only_payload_read_at_restore);
    RUN(source_files_stored_once_and_shared_after_restore);
    RUN(readtable_and_hooks_round_trip);
    RUN(condvar_and_dead_thread_restore);
    platform_file_delete(IMG_PATH);
    REPORT();
}
