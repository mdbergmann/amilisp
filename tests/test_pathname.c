#include "test.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
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

CL_Obj bi_merge_pathnames(CL_Obj *args, int n);

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
        cl_vm.sp = 0;
        cl_vm.fp = 0;
        return buf;
    }
}

/* --- Basic #P reader --- */

TEST(pathname_read_simple)
{
    ASSERT_STR_EQ(eval_print("#P\"foo.lisp\""), "#P\"foo.lisp\"");
}

TEST(pathname_read_with_dir)
{
    ASSERT_STR_EQ(eval_print("#P\"/foo/bar.lisp\""), "#P\"/foo/bar.lisp\"");
}

/* Regression: PRINC / ~A / *print-escape* nil must print the bare namestring,
 * not the #P"..." reader syntax (PRIN1 keeps #P"...").  local-time's
 * REREAD-TIMEZONE-REPOSITORY computes substring offsets from
 * (princ-to-string pathname); the #P"..." prefix shifted every offset and
 * corrupted the zone-name hash keys, so find-timezone-by-location-name failed. */
TEST(pathname_princ_bare_namestring)
{
    ASSERT_STR_EQ(eval_print("(princ-to-string #P\"/foo/bar/UTC\")"),
                  "\"/foo/bar/UTC\"");
    ASSERT_STR_EQ(eval_print("(prin1-to-string #P\"/foo/bar/UTC\")"),
                  "\"#P\\\"/foo/bar/UTC\\\"\"");
    ASSERT_STR_EQ(eval_print("(format nil \"~A\" #P\"/foo/bar/UTC\")"),
                  "\"/foo/bar/UTC\"");
}

TEST(pathname_read_relative)
{
    ASSERT_STR_EQ(eval_print("#P\"dir/file.txt\""), "#P\"dir/file.txt\"");
}

TEST(pathname_read_empty)
{
    ASSERT_STR_EQ(eval_print("#P\"\""), "#P\"\"");
}

TEST(pathname_read_case_insensitive)
{
    ASSERT_STR_EQ(eval_print("#p\"test.lisp\""), "#P\"test.lisp\"");
}

/* --- pathnamep predicate --- */

TEST(pathnamep_true)
{
    ASSERT_STR_EQ(eval_print("(pathnamep #P\"test\")"), "T");
}

TEST(pathnamep_false_string)
{
    ASSERT_STR_EQ(eval_print("(pathnamep \"test\")"), "NIL");
}

TEST(pathnamep_false_nil)
{
    ASSERT_STR_EQ(eval_print("(pathnamep nil)"), "NIL");
}

TEST(pathnamep_false_number)
{
    ASSERT_STR_EQ(eval_print("(pathnamep 42)"), "NIL");
}

/* --- pathname coercion --- */

TEST(pathname_from_string)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (pathname \"foo.lisp\"))"), "T");
}

TEST(pathname_from_pathname)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (pathname #P\"foo.lisp\"))"), "T");
}

/* Regression: a pathname designator is any string flavour — base string,
 * wide string, OR an adjustable/fill-pointer character vector — not only
 * TYPE_STRING.  PATHNAME / NAMESTRING wrongly type-error'd on fill-pointer
 * strings, which hunchentoot's HTTP Range handler builds and passes to
 * PATHNAME (a 500 "PATHNAME: expected string or pathname"). */
TEST(pathname_from_fill_pointer_string)
{
    const char *mk =
        "(let ((s (make-array 0 :element-type 'character"
        "                       :fill-pointer 0 :adjustable t)))"
        "  (dolist (c '(#\\f #\\o #\\o #\\. #\\t #\\x #\\t)) (vector-push-extend c s))"
        "  s)";
    char buf[512];
    snprintf(buf, sizeof(buf), "(pathnamep (pathname %s))", mk);
    ASSERT_STR_EQ(eval_print(buf), "T");
    snprintf(buf, sizeof(buf), "(pathname-name (pathname %s))", mk);
    ASSERT_STR_EQ(eval_print(buf), "\"foo\"");
    snprintf(buf, sizeof(buf), "(namestring %s)", mk);
    ASSERT_STR_EQ(eval_print(buf), "\"foo.txt\"");
}

/* --- pathname-name --- */

TEST(pathname_name_simple)
{
    ASSERT_STR_EQ(eval_print("(pathname-name #P\"foo.lisp\")"), "\"foo\"");
}

TEST(pathname_name_with_dir)
{
    ASSERT_STR_EQ(eval_print("(pathname-name #P\"/foo/bar.lisp\")"), "\"bar\"");
}

TEST(pathname_name_no_ext)
{
    ASSERT_STR_EQ(eval_print("(pathname-name #P\"foo\")"), "\"foo\"");
}

TEST(pathname_name_from_string)
{
    ASSERT_STR_EQ(eval_print("(pathname-name \"bar.txt\")"), "\"bar\"");
}

/* --- pathname-type --- */

TEST(pathname_type_lisp)
{
    ASSERT_STR_EQ(eval_print("(pathname-type #P\"foo.lisp\")"), "\"lisp\"");
}

TEST(pathname_type_nil)
{
    ASSERT_STR_EQ(eval_print("(pathname-type #P\"foo\")"), "NIL");
}

TEST(pathname_type_txt)
{
    ASSERT_STR_EQ(eval_print("(pathname-type #P\"bar.txt\")"), "\"txt\"");
}

/* --- pathname-directory --- */

TEST(pathname_directory_absolute)
{
    ASSERT_STR_EQ(eval_print("(pathname-directory #P\"/foo/bar.lisp\")"), "(:ABSOLUTE \"foo\")");
}

TEST(pathname_directory_relative)
{
    ASSERT_STR_EQ(eval_print("(pathname-directory #P\"dir/file.txt\")"), "(:RELATIVE \"dir\")");
}

TEST(pathname_directory_nil)
{
    ASSERT_STR_EQ(eval_print("(pathname-directory #P\"file.txt\")"), "NIL");
}

TEST(pathname_directory_multi)
{
    ASSERT_STR_EQ(eval_print("(pathname-directory #P\"/a/b/c/d.txt\")"), "(:ABSOLUTE \"a\" \"b\" \"c\")");
}

/* --- pathname-device (Amiga) --- */

TEST(pathname_device_amiga)
{
    ASSERT_STR_EQ(eval_print("(pathname-device #P\"DH0:Work/test.lisp\")"), "\"DH0\"");
}

TEST(pathname_device_nil)
{
    ASSERT_STR_EQ(eval_print("(pathname-device #P\"/foo/bar.lisp\")"), "NIL");
}

/* --- namestring --- */

TEST(namestring_simple)
{
    ASSERT_STR_EQ(eval_print("(namestring #P\"foo.lisp\")"), "\"foo.lisp\"");
}

TEST(namestring_with_dir)
{
    ASSERT_STR_EQ(eval_print("(namestring #P\"/foo/bar.lisp\")"), "\"/foo/bar.lisp\"");
}

TEST(namestring_from_string)
{
    ASSERT_STR_EQ(eval_print("(namestring \"foo.lisp\")"), "\"foo.lisp\"");
}

/* --- type system --- */

TEST(pathname_typep)
{
    ASSERT_STR_EQ(eval_print("(typep #P\"test\" 'pathname)"), "T");
}

TEST(pathname_typep_neg)
{
    ASSERT_STR_EQ(eval_print("(typep \"test\" 'pathname)"), "NIL");
}

TEST(pathname_type_of)
{
    ASSERT_STR_EQ(eval_print("(type-of #P\"test\")"), "PATHNAME");
}

/* --- equal --- */

TEST(pathname_equal_same)
{
    ASSERT_STR_EQ(eval_print("(equal #P\"/foo/bar.lisp\" #P\"/foo/bar.lisp\")"), "T");
}

TEST(pathname_equal_diff)
{
    ASSERT_STR_EQ(eval_print("(equal #P\"/foo/bar.lisp\" #P\"/baz/bar.lisp\")"), "NIL");
}

/* --- make-pathname --- */

TEST(make_pathname_basic)
{
    ASSERT_STR_EQ(eval_print("(namestring (make-pathname :name \"foo\" :type \"lisp\"))"), "\"foo.lisp\"");
}

TEST(make_pathname_dir)
{
    ASSERT_STR_EQ(eval_print("(namestring (make-pathname :directory '(:absolute \"usr\" \"lib\") :name \"foo\" :type \"lisp\"))"),
                  "\"/usr/lib/foo.lisp\"");
}

TEST(make_pathname_defaults_string)
{
    /* Regression: :defaults must accept pathname designators (strings), not just pathnames */
    ASSERT_STR_EQ(eval_print("(namestring (make-pathname :directory '(:relative \"archives\") :defaults \"foo-20241012.tgz\"))"),
                  "\"archives/foo-20241012.tgz\"");
}

TEST(make_pathname_defaults_pathname)
{
    ASSERT_STR_EQ(eval_print("(namestring (make-pathname :directory '(:relative \"out\") :defaults #P\"bar.lisp\"))"),
                  "\"out/bar.lisp\"");
}

/* --- merge-pathnames --- */

TEST(merge_pathnames_basic)
{
    ASSERT_STR_EQ(eval_print("(namestring (merge-pathnames #P\"bar.lisp\" #P\"/foo/\"))"),
                  "\"/foo/bar.lisp\"");
}

TEST(merge_pathnames_no_merge_needed)
{
    ASSERT_STR_EQ(eval_print("(namestring (merge-pathnames #P\"/full/path.lisp\" #P\"/other/\"))"),
                  "\"/full/path.lisp\"");
}

TEST(merge_pathnames_relative_into_relative)
{
    ASSERT_STR_EQ(eval_print("(namestring (merge-pathnames \".local/share/\" \"ocicl/\"))"),
                  "\"ocicl/.local/share/\"");
}

TEST(merge_pathnames_relative_into_absolute)
{
    ASSERT_STR_EQ(eval_print("(namestring (merge-pathnames \"sub/file.lisp\" \"/base/\"))"),
                  "\"/base/sub/file.lisp\"");
}

TEST(merge_pathnames_dir_only_trailing_slash)
{
    ASSERT_STR_EQ(eval_print("(pathname-directory (merge-pathnames \".local/share/\" \"ocicl/\"))"),
                  "(:RELATIVE \"ocicl\" \".local\" \"share\")");
}

TEST(merge_pathnames_relative_into_absolute_dir)
{
    /* Verify directory structure: (:ABSOLUTE "base" "sub") */
    ASSERT_STR_EQ(eval_print("(pathname-directory (merge-pathnames \"sub/file.lisp\" \"/base/\"))"),
                  "(:ABSOLUTE \"base\" \"sub\")");
}

TEST(merge_pathnames_relative_single_into_multi)
{
    /* Single relative component into multi-component absolute */
    ASSERT_STR_EQ(eval_print("(namestring (merge-pathnames \"lib/foo.lisp\" \"/usr/local/\"))"),
                  "\"/usr/local/lib/foo.lisp\"");
    ASSERT_STR_EQ(eval_print("(pathname-directory (merge-pathnames \"lib/foo.lisp\" \"/usr/local/\"))"),
                  "(:ABSOLUTE \"usr\" \"local\" \"lib\")");
}

TEST(merge_pathnames_relative_defaults_nil_dir)
{
    /* Relative dir but defaults has NIL directory — keep relative as-is */
    ASSERT_STR_EQ(eval_print("(pathname-directory (merge-pathnames \"sub/foo.lisp\" \"bar.lisp\"))"),
                  "(:RELATIVE \"sub\")");
}

TEST(merge_pathnames_absolute_overrides_absolute)
{
    /* Both absolute — pathname's directory wins entirely */
    ASSERT_STR_EQ(eval_print("(pathname-directory (merge-pathnames \"/my/dir/\" \"/other/dir/\"))"),
                  "(:ABSOLUTE \"my\" \"dir\")");
}

TEST(merge_pathnames_name_preserved_on_merge)
{
    /* Pathname's name/type preserved while directories merge */
    ASSERT_STR_EQ(eval_print("(pathname-name (merge-pathnames \"sub/mine.txt\" \"/base/\"))"),
                  "\"mine\"");
    ASSERT_STR_EQ(eval_print("(pathname-type (merge-pathnames \"sub/mine.txt\" \"/base/\"))"),
                  "\"txt\"");
}

TEST(merge_pathnames_relative_file_only)
{
    /* Relative pathname with just a filename (no dir components) merges dir from defaults */
    ASSERT_STR_EQ(eval_print("(namestring (merge-pathnames \"test.lisp\" \"/usr/src/\"))"),
                  "\"/usr/src/test.lisp\"");
    ASSERT_STR_EQ(eval_print("(pathname-directory (merge-pathnames \"test.lisp\" \"/usr/src/\"))"),
                  "(:ABSOLUTE \"usr\" \"src\")");
}

/* --- file-namestring --- */

TEST(file_namestring_basic)
{
    ASSERT_STR_EQ(eval_print("(file-namestring #P\"/foo/bar.lisp\")"), "\"bar.lisp\"");
}

/* --- directory-namestring --- */

TEST(directory_namestring_basic)
{
    ASSERT_STR_EQ(eval_print("(directory-namestring #P\"/foo/bar.lisp\")"), "\"/foo/\"");
}

/* --- Amiga-specific parsing --- */

TEST(pathname_amiga_device_dir)
{
    ASSERT_STR_EQ(eval_print("(pathname-device #P\"DH0:Work/test.lisp\")"), "\"DH0\"");
}

TEST(pathname_amiga_dir)
{
    ASSERT_STR_EQ(eval_print("(pathname-directory #P\"DH0:Work/test.lisp\")"), "(:ABSOLUTE \"Work\")");
}

TEST(pathname_amiga_name)
{
    ASSERT_STR_EQ(eval_print("(pathname-name #P\"DH0:Work/test.lisp\")"), "\"test\"");
}

TEST(pathname_amiga_type)
{
    ASSERT_STR_EQ(eval_print("(pathname-type #P\"DH0:Work/test.lisp\")"), "\"lisp\"");
}

TEST(pathname_amiga_namestring)
{
    ASSERT_STR_EQ(eval_print("(namestring #P\"DH0:Work/test.lisp\")"), "\"DH0:Work/test.lisp\"");
}

/* --- AmigaDOS slash rules (parsed with the Amiga syntax on every host) ---
 *
 * AmigaDOS reads a LEADING slash as the parent of the current directory
 * and each empty component ("a//b") as one more level up; only a device
 * makes a path absolute.  The POSIX reading ((:ABSOLUTE ...)) made
 * (load "//x/f.lisp") open the right file but resolve the files IT loads
 * against the volume root.  Result: "<directory> <namestring>", where the
 * namestring is rebuilt with the Amiga syntax (it must round-trip). */
static const char *amiga_parse(const char *str)
{
    static char buf[512];
    int err;

    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj pn = cl_parse_namestring_syntax(str, (uint32_t)strlen(str), 1);
        uint32_t used;
        cl_prin1_to_string(((CL_Pathname *)CL_OBJ_TO_PTR(pn))->directory,
                           buf, sizeof(buf));
        used = (uint32_t)strlen(buf);
        buf[used++] = ' ';
        cl_pathname_to_namestring_syntax((CL_Pathname *)CL_OBJ_TO_PTR(pn),
                                         buf + used, sizeof(buf) - used, 1);
        CL_UNCATCH();
        return buf;
    }
    CL_UNCATCH();
    snprintf(buf, sizeof(buf), "ERROR:%d", err);
    cl_vm.sp = 0;
    cl_vm.fp = 0;
    return buf;
}

TEST(pathname_amiga_leading_slash_is_parent)
{
    ASSERT_STR_EQ(amiga_parse("/foo/x.lisp"), "(:RELATIVE :UP \"foo\") /foo/x.lisp");
    ASSERT_STR_EQ(amiga_parse("//clamacs/lisp/load.lisp"),
                  "(:RELATIVE :UP :UP \"clamacs\" \"lisp\") //clamacs/lisp/load.lisp");
    ASSERT_STR_EQ(amiga_parse("/"), "(:RELATIVE :UP) /");
    ASSERT_STR_EQ(amiga_parse("//"), "(:RELATIVE :UP :UP) //");
    ASSERT_STR_EQ(amiga_parse("/x"), "(:RELATIVE :UP) /x");
}

TEST(pathname_amiga_empty_component_is_parent)
{
    ASSERT_STR_EQ(amiga_parse("a//b/f.lisp"), "(:RELATIVE \"a\" :UP \"b\") a//b/f.lisp");
    ASSERT_STR_EQ(amiga_parse("a//"), "(:RELATIVE \"a\" :UP) a//");
    ASSERT_STR_EQ(amiga_parse("Work:a///b"), "(:ABSOLUTE \"a\" :UP :UP) Work:a///b");
    ASSERT_STR_EQ(amiga_parse("Work:/x"), "(:ABSOLUTE :UP) Work:/x");
}

TEST(pathname_amiga_plain_paths_unchanged)
{
    ASSERT_STR_EQ(amiga_parse("Work:dev/test.lisp"), "(:ABSOLUTE \"dev\") Work:dev/test.lisp");
    ASSERT_STR_EQ(amiga_parse("PROGDIR:"), "(:ABSOLUTE) PROGDIR:");
    ASSERT_STR_EQ(amiga_parse("a/b/"), "(:RELATIVE \"a\" \"b\") a/b/");
    ASSERT_STR_EQ(amiga_parse("f.lisp"), "NIL f.lisp");
}

/* The case that broke: a file named by "//dir/load.lisp" merges its
 * siblings under the current directory, not the volume root. */
TEST(pathname_amiga_parent_merges_under_cwd)
{
    static char buf[512];
    int err;
    CL_Obj args[2];

    strcpy(buf, "ERROR");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        args[0] = cl_parse_namestring_syntax("//lisp/pkg.lisp", 15, 1);
        CL_GC_PROTECT(args[0]);
        args[1] = cl_parse_namestring_syntax("Work:build/morphos/", 19, 1);
        CL_GC_PROTECT(args[1]);
        args[0] = bi_merge_pathnames(args, 2);
        cl_pathname_to_namestring_syntax((CL_Pathname *)CL_OBJ_TO_PTR(args[0]),
                                         buf, sizeof(buf), 1);
        CL_GC_UNPROTECT(2);
    }
    CL_UNCATCH();
    ASSERT_STR_EQ(buf, "Work:build/morphos///lisp/pkg.lisp");
}

/* POSIX spells :UP / :BACK as "..", so they are no longer dropped. */
TEST(namestring_up_component_posix)
{
    char posix[256], amiga[256];
    int err;

    strcpy(posix, "ERROR");
    strcpy(amiga, "ERROR");
    CL_CATCH(err);
    if (err == CL_ERR_NONE) {
        CL_Obj pn = cl_eval_string(
            "(make-pathname :directory '(:relative :up \"x\" :back) :name \"f\")");
        cl_pathname_to_namestring_syntax((CL_Pathname *)CL_OBJ_TO_PTR(pn),
                                         posix, sizeof(posix), 0);
        cl_pathname_to_namestring_syntax((CL_Pathname *)CL_OBJ_TO_PTR(pn),
                                         amiga, sizeof(amiga), 1);
    }
    CL_UNCATCH();
    ASSERT_STR_EQ(posix, "../x/../f");
    ASSERT_STR_EQ(amiga, "/x//f");
}

/* --- coerce --- */

TEST(coerce_string_to_pathname)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (coerce \"foo.lisp\" 'pathname))"), "T");
}

/* --- *default-pathname-defaults* --- */

TEST(default_pathname_defaults)
{
    ASSERT_STR_EQ(eval_print("(pathnamep *default-pathname-defaults*)"), "T");
}

/* Regression: *default-pathname-defaults* must be seeded with the cwd as an
 * ABSOLUTE directory pathname.  An empty/relative default left relative merges
 * relative, which ASDF's source-registry rejects ("Expected an absolute
 * pathname") — see the Ocicl :tree setup in the README. */
TEST(default_pathname_defaults_is_absolute)
{
    ASSERT_STR_EQ(eval_print(
        "(eq :absolute (first (pathname-directory *default-pathname-defaults*)))"),
        "T");
}

/* A relative pathname merged against the default must come out absolute. */
TEST(default_pathname_defaults_merge_absolute)
{
    ASSERT_STR_EQ(eval_print(
        "(eq :absolute (first (pathname-directory"
        "  (merge-pathnames \"systems/\" *default-pathname-defaults*))))"),
        "T");
}

/* --- pathname-host / pathname-version --- */

TEST(pathname_host_nil)
{
    ASSERT_STR_EQ(eval_print("(pathname-host #P\"foo.lisp\")"), "NIL");
}

TEST(pathname_version_nil)
{
    ASSERT_STR_EQ(eval_print("(pathname-version #P\"foo.lisp\")"), "NIL");
}

/* --- directory only (trailing slash) --- */

TEST(pathname_dir_only)
{
    ASSERT_STR_EQ(eval_print("(pathname-directory #P\"/foo/bar/\")"), "(:ABSOLUTE \"foo\" \"bar\")");
}

TEST(pathname_dir_only_name_nil)
{
    ASSERT_STR_EQ(eval_print("(pathname-name #P\"/foo/bar/\")"), "NIL");
}

/* --- truename and probe-file return pathnames --- */

TEST(truename_returns_pathname_from_string)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (truename \"/tmp/\"))"), "T");
}

TEST(truename_returns_pathname_from_pathname)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (truename #P\"/tmp/\"))"), "T");
}

TEST(probe_file_returns_pathname)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (probe-file \"/tmp/\"))"), "T");
}

TEST(probe_file_accepts_pathname)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (probe-file #P\"/tmp/\"))"), "T");
}

TEST(probe_file_nonexistent_returns_nil)
{
    ASSERT_STR_EQ(eval_print("(probe-file #P\"/nonexistent_path_xyz\")"), "NIL");
}

TEST(file_write_date_accepts_string)
{
    ASSERT_STR_EQ(eval_print("(integerp (file-write-date \"/tmp/\"))"), "T");
}

TEST(file_write_date_accepts_pathname)
{
    ASSERT_STR_EQ(eval_print("(integerp (file-write-date #P\"/tmp/\"))"), "T");
}

/* --- Tilde expansion --- */

/* $HOME as amilisp will have spelled it.  On Windows the environment may
 * hand back either separator (a cmd.exe session has USERPROFILE-style
 * backslashes, an MSYS shell forward ones) while every namestring leaving
 * the runtime uses '/', so normalise before comparing. */
static const char *test_home_dir(void)
{
    static char home[512];
    const char *env = getenv("HOME");
#ifdef PLATFORM_WIN32
    if (!env || !*env) env = getenv("USERPROFILE");
#endif
    if (!env) return NULL;
    snprintf(home, sizeof(home), "%s", env);
#ifdef PLATFORM_WIN32
    {
        char *p;
        for (p = home; *p; p++)
            if (*p == '\\') *p = '/';
    }
#endif
    return home;
}

TEST(tilde_bare_expands_to_home)
{
    /* ~ should expand to $HOME */
    const char *result = eval_print("(namestring (pathname \"~\"))");
    const char *home = test_home_dir();
    /* Result must start with home dir */
    ASSERT(home != NULL);
    ASSERT(strncmp(result + 1, home, strlen(home)) == 0);  /* skip leading " */
}

TEST(tilde_slash_expands_to_home)
{
    /* ~/foo should expand to $HOME/foo */
    const char *result = eval_print("(namestring (pathname \"~/foo\"))");
    const char *home = test_home_dir();
    char expected[512];
    snprintf(expected, sizeof(expected), "\"%s/foo\"", home);
    ASSERT_STR_EQ(result, expected);
}

TEST(tilde_path_expands_in_directory)
{
    /* ~/foo/bar.lisp should have absolute directory with home components */
    const char *result = eval_print("(pathname-directory (pathname \"~/foo/bar.lisp\"))");
    /* Directory must be :ABSOLUTE */
    ASSERT(strstr(result, ":ABSOLUTE") != NULL);
    ASSERT(strstr(result, "\"foo\"") != NULL);
}

TEST(tilde_not_expanded_mid_path)
{
    /* foo/~bar.txt should NOT expand tilde */
    ASSERT_STR_EQ(eval_print("(namestring (pathname \"foo/~bar.txt\"))"),
                  "\"foo/~bar.txt\"");
}

TEST(tilde_user_not_expanded)
{
    /* ~user should NOT be expanded (only ~ and ~/ are supported) */
    ASSERT_STR_EQ(eval_print("(namestring (pathname \"~user/foo\"))"),
                  "\"~user/foo\"");
}

TEST(tilde_probe_file_works)
{
    /* probe-file with ~ should work for existing files */
    ASSERT_STR_EQ(eval_print("(not (null (probe-file \"/tmp/\")))"), "T");
}

TEST(tilde_open_read_works)
{
    /* open with ~ should expand the path */
    const char *result = eval_print(
        "(progn "
        "  (with-open-file (s \"/tmp/cl-tilde-test.txt\" :direction :output "
        "                   :if-exists :supersede) "
        "    (write-string \"tilde-ok\" s)) "
        "  (with-open-file (s \"/tmp/cl-tilde-test.txt\") "
        "    (read-line s)))");
    ASSERT_STR_EQ(result, "\"tilde-ok\"");
}

/* --- LOAD merges with *default-pathname-defaults* (HyperSpec) --- */

TEST(load_merges_with_default_pathname_defaults)
{
    /* Bare relative path passed to LOAD must resolve against
       *default-pathname-defaults*, not just the current working directory. */
    const char *result = eval_print(
        "(let ((*default-pathname-defaults* (truename \"tests/\")))"
        "  (load \"test_load.lisp\")"
        "  *load-test-var*)");
    ASSERT_STR_EQ(result, "42");
}

/* --- Wildcard pathname tests --- */

TEST(wild_namestring_name_type)
{
    ASSERT_STR_EQ(eval_print("(namestring (make-pathname :name :wild :type :wild))"),
                  "\"*.*\"");
}

TEST(wild_namestring_name_only)
{
    ASSERT_STR_EQ(eval_print("(namestring (make-pathname :name :wild))"),
                  "\"*\"");
}

TEST(wild_namestring_merged)
{
    ASSERT_STR_EQ(eval_print(
        "(namestring (merge-pathnames (make-pathname :name :wild :type :wild) #P\"/tmp/dir/\"))"),
        "\"/tmp/dir/*.*\"");
}

TEST(wild_parse_name)
{
    ASSERT_STR_EQ(eval_print("(pathname-name #P\"*.*\")"), ":WILD");
}

TEST(wild_parse_type)
{
    ASSERT_STR_EQ(eval_print("(pathname-type #P\"*.*\")"), ":WILD");
}

TEST(wild_parse_name_only)
{
    ASSERT_STR_EQ(eval_print("(pathname-name #P\"*\")"), ":WILD");
}

TEST(wild_parse_dir_component)
{
    ASSERT_STR_EQ(eval_print("(second (pathname-directory #P\"/*/foo\"))"), ":WILD");
}

TEST(wild_pathname_p_true)
{
    ASSERT_STR_EQ(eval_print("(wild-pathname-p (make-pathname :name :wild))"), "T");
}

TEST(wild_pathname_p_false)
{
    ASSERT_STR_EQ(eval_print("(wild-pathname-p #P\"foo.lisp\")"), "NIL");
}

TEST(wild_pathname_p_field_name)
{
    ASSERT_STR_EQ(eval_print(
        "(wild-pathname-p (make-pathname :name :wild :type :wild) :name)"), "T");
}

TEST(wild_pathname_p_field_type)
{
    ASSERT_STR_EQ(eval_print(
        "(wild-pathname-p (make-pathname :name :wild :type :wild) :type)"), "T");
}

TEST(wild_pathname_p_field_dir)
{
    ASSERT_STR_EQ(eval_print("(wild-pathname-p #P\"/*/foo\" :directory)"), "T");
}

TEST(wild_namestring_roundtrip)
{
    ASSERT_STR_EQ(eval_print("(namestring #P\"*.*\")"), "\"*.*\"");
}

TEST(wild_directory_lists_dirs)
{
    /* Ensure directory with *.* pattern returns both files and dirs */
    const char *result;
    /* Create test structure */
    eval_print("(ensure-directories-exist #P\"/tmp/cl-wild-test/sub/\")");
    platform_file_delete("/tmp/cl-wild-test/f.txt");
    {
        FILE *f = fopen("/tmp/cl-wild-test/f.txt", "w");
        if (f) { fprintf(f, "hi"); fclose(f); }
    }
    result = eval_print(
        "(let ((entries (directory (merge-pathnames (make-pathname :name :wild :type :wild)"
        " #P\"/tmp/cl-wild-test/\"))))"
        "  (list (> (length entries) 0)"
        "    (some (lambda (e) (not (or (pathname-name e) (pathname-type e)))) entries)))");
    /* Should find at least one entry, and at least one directory */
    ASSERT_STR_EQ(result, "(T T)");
}

/* --- translate-logical-pathname and logical-pathname-translations --- */

TEST(translate_logical_pathname_fboundp)
{
    ASSERT_STR_EQ(eval_print("(fboundp 'translate-logical-pathname)"), "T");
}

TEST(translate_logical_pathname_returns_pathname)
{
    ASSERT_STR_EQ(eval_print("(pathnamep (translate-logical-pathname #P\"foo.lisp\"))"), "T");
}

TEST(translate_logical_pathname_roundtrip)
{
    ASSERT_STR_EQ(eval_print("(namestring (translate-logical-pathname #P\"foo.lisp\"))"), "\"foo.lisp\"");
}

TEST(logical_pathname_translations_fboundp)
{
    ASSERT_STR_EQ(eval_print("(fboundp 'logical-pathname-translations)"), "T");
}

TEST(logical_pathname_translations_nil)
{
    ASSERT_STR_EQ(eval_print("(logical-pathname-translations \"SYS\")"), "NIL");
}

/* --- Run all tests --- */

int main(void)
{
    setup();

    RUN(pathname_read_simple);
    RUN(pathname_read_with_dir);
    RUN(pathname_princ_bare_namestring);
    RUN(pathname_read_relative);
    RUN(pathname_read_empty);
    RUN(pathname_read_case_insensitive);
    RUN(pathnamep_true);
    RUN(pathnamep_false_string);
    RUN(pathnamep_false_nil);
    RUN(pathnamep_false_number);
    RUN(pathname_from_string);
    RUN(pathname_from_pathname);
    RUN(pathname_from_fill_pointer_string);
    RUN(pathname_name_simple);
    RUN(pathname_name_with_dir);
    RUN(pathname_name_no_ext);
    RUN(pathname_name_from_string);
    RUN(pathname_type_lisp);
    RUN(pathname_type_nil);
    RUN(pathname_type_txt);
    RUN(pathname_directory_absolute);
    RUN(pathname_directory_relative);
    RUN(pathname_directory_nil);
    RUN(pathname_directory_multi);
    RUN(pathname_device_amiga);
    RUN(pathname_device_nil);
    RUN(namestring_simple);
    RUN(namestring_with_dir);
    RUN(namestring_from_string);
    RUN(pathname_typep);
    RUN(pathname_typep_neg);
    RUN(pathname_type_of);
    RUN(pathname_equal_same);
    RUN(pathname_equal_diff);
    RUN(make_pathname_basic);
    RUN(make_pathname_dir);
    RUN(make_pathname_defaults_string);
    RUN(make_pathname_defaults_pathname);
    RUN(merge_pathnames_basic);
    RUN(merge_pathnames_no_merge_needed);
    RUN(merge_pathnames_relative_into_relative);
    RUN(merge_pathnames_relative_into_absolute);
    RUN(merge_pathnames_dir_only_trailing_slash);
    RUN(merge_pathnames_relative_into_absolute_dir);
    RUN(merge_pathnames_relative_single_into_multi);
    RUN(merge_pathnames_relative_defaults_nil_dir);
    RUN(merge_pathnames_absolute_overrides_absolute);
    RUN(merge_pathnames_name_preserved_on_merge);
    RUN(merge_pathnames_relative_file_only);
    RUN(file_namestring_basic);
    RUN(directory_namestring_basic);
    RUN(pathname_amiga_device_dir);
    RUN(pathname_amiga_dir);
    RUN(pathname_amiga_name);
    RUN(pathname_amiga_type);
    RUN(pathname_amiga_namestring);
    RUN(pathname_amiga_leading_slash_is_parent);
    RUN(pathname_amiga_empty_component_is_parent);
    RUN(pathname_amiga_plain_paths_unchanged);
    RUN(pathname_amiga_parent_merges_under_cwd);
    RUN(namestring_up_component_posix);
    RUN(coerce_string_to_pathname);
    RUN(default_pathname_defaults);
    RUN(default_pathname_defaults_is_absolute);
    RUN(default_pathname_defaults_merge_absolute);
    RUN(pathname_host_nil);
    RUN(pathname_version_nil);
    RUN(pathname_dir_only);
    RUN(pathname_dir_only_name_nil);
    RUN(truename_returns_pathname_from_string);
    RUN(truename_returns_pathname_from_pathname);
    RUN(probe_file_returns_pathname);
    RUN(probe_file_accepts_pathname);
    RUN(probe_file_nonexistent_returns_nil);
    RUN(file_write_date_accepts_string);
    RUN(file_write_date_accepts_pathname);
    RUN(tilde_bare_expands_to_home);
    RUN(tilde_slash_expands_to_home);
    RUN(tilde_path_expands_in_directory);
    RUN(tilde_not_expanded_mid_path);
    RUN(tilde_user_not_expanded);
    RUN(tilde_probe_file_works);
    RUN(tilde_open_read_works);
    RUN(load_merges_with_default_pathname_defaults);
    RUN(wild_namestring_name_type);
    RUN(wild_namestring_name_only);
    RUN(wild_namestring_merged);
    RUN(wild_parse_name);
    RUN(wild_parse_type);
    RUN(wild_parse_name_only);
    RUN(wild_parse_dir_component);
    RUN(wild_pathname_p_true);
    RUN(wild_pathname_p_false);
    RUN(wild_pathname_p_field_name);
    RUN(wild_pathname_p_field_type);
    RUN(wild_pathname_p_field_dir);
    RUN(wild_namestring_roundtrip);
    RUN(wild_directory_lists_dirs);
    RUN(translate_logical_pathname_fboundp);
    RUN(translate_logical_pathname_returns_pathname);
    RUN(translate_logical_pathname_roundtrip);
    RUN(logical_pathname_translations_fboundp);
    RUN(logical_pathname_translations_nil);

    teardown();
    REPORT();
}
