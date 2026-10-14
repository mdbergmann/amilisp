# AmiLisp Makefile

UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
# Targets: host (Linux), amiga-m68k, amiga-ppc

CC_HOST     = gcc

# Windows (mingw-w64 under MSYS2: CLANGARM64 / UCRT64 / MINGW64, or Git Bash
# with a mingw compiler on PATH).  `make host` there builds a NATIVE .exe —
# platform_win32.c + Winsock, no msys-2.0.dll — and everything else in this
# Makefile is shared with the POSIX hosts.
ifneq (,$(filter MINGW% MSYS% CLANGARM64% CLANG64% UCRT64%,$(UNAME_S)))
WINDOWS := 1
EXE     := .exe
endif

# libffi + dlopen: the host FFI engine (foreign-funcall / dlsym / callbacks).
# pkg-config locates libffi on macOS, Linux and MSYS2 alike; fall back to -lffi.
FFI_CFLAGS := $(shell pkg-config --cflags libffi 2>/dev/null)
FFI_LIBS   := $(shell pkg-config --libs libffi 2>/dev/null || echo -lffi)
ifdef WINDOWS
# Winsock, plus winpthreads for the pthreads API platform_thread_posix.c uses.
# No -ldl: LoadLibrary lives in kernel32, which links implicitly.
PLATFORM_DEF   = -DPLATFORM_WIN32
PLATFORM_IMPL  = $(SRCDIR)/platform/platform_win32.c \
                 $(SRCDIR)/platform/win32_compat.c
# shell32: CommandLineToArgvW, which main.c uses to recover the UTF-16
# command line (argv arrives ANSI-mangled otherwise).
PLATFORM_LIBS  = -lws2_32 -lshell32
PTHREAD_FLAGS  = -pthread
else
PLATFORM_DEF   = -DPLATFORM_POSIX
PLATFORM_IMPL  = $(SRCDIR)/platform/platform_posix.c
PLATFORM_LIBS  =
ifneq ($(UNAME_S),Darwin)
FFI_LIBS   += -ldl
# glibc < 2.34 (e.g. Debian 11 / older Raspberry Pi OS) ships libpthread as a
# separate library, so the pthread_* symbols in platform_thread_posix.c need
# -pthread at compile AND link time (newer glibc and macOS resolve them from
# libc/libSystem, which is why the omission only surfaced there).
PTHREAD_FLAGS = -pthread
endif
endif

# The AArch64 template JIT (specs/native-backend-a64.md): arm64 macOS and
# arm64 Linux hosts build it by default; `make host JIT=0` builds the
# interpreter only.
JIT ?= 1
ifneq ($(filter Darwin-arm64-1 Linux-aarch64-1 Linux-arm64-1,$(UNAME_S)-$(UNAME_M)-$(JIT)),)
JIT_A64 := 1
JIT_DEF := -DJIT_A64
endif

# GC backend (specs/gc-backend-immix.md): compile time, exactly one per
# binary.  `make host GC=classic` (the default: mark-sweep-compact with the
# generational mode on POSIX hosts) or `make host GC=immix` (the mark-region
# backend, gc_immix.c).  An immix build goes to its own BUILDDIR so the host
# A/B is two binaries, like the FPU=1 cross variant; Makefile.cross and
# Makefile.mos take the same knob.
GC ?= classic
ifeq ($(GC),classic)
GC_DEF  := -DCL_GC_CLASSIC
GC_SRC   = $(SRCDIR)/core/gc_classic.c
GC_DIR_SUFFIX :=
else ifeq ($(GC),immix)
GC_DEF  := -DCL_GC_IMMIX
GC_SRC   = $(SRCDIR)/core/gc_immix.c
BUILDDIR = build/host-immix
# The dedicated debug builds below (gc-stress, memleak, race) get their own
# directories too: their objects carry the backend define.
GC_DIR_SUFFIX := -immix
else
$(error GC=$(GC): unknown GC backend (classic or immix))
endif

CFLAGS_HOST = -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -g -O3 -flto $(PLATFORM_DEF) $(JIT_DEF) $(GC_DEF) -DCL_WIDE_STRINGS $(PTHREAD_FLAGS) $(FFI_CFLAGS) $(DEBUG_FLAGS)
HOST_LIBS   = -lm $(PTHREAD_FLAGS) $(FFI_LIBS) $(PLATFORM_LIBS)

# Test builds deliberately drop -flto and use -O1 instead of -O3.  The shipped
# amilisp binary is built once at -O3 -flto, but the ~50 unit-test binaries each
# link the entire runtime, and with -flto every one of those links re-runs
# whole-program LTO over the whole runtime — that link step, repeated per test
# binary, dominates `make test` wall-clock.  -O1 + no LTO compiles fast and runs
# fast enough for the suite (gc-stress, which needs the optimized binary, builds
# its own -O3 amilisp separately and is unaffected).  Test objects live in their
# own tree so they never clash with the -O3 -flto objects linked into amilisp.
CFLAGS_TEST = -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -g -O1 $(PLATFORM_DEF) $(JIT_DEF) $(GC_DEF) -DCL_WIDE_STRINGS $(PTHREAD_FLAGS) $(FFI_CFLAGS) $(DEBUG_FLAGS)

SRCDIR   = src
BUILDDIR ?= build/host
# The linker appends .exe on Windows; name the target the same way so make
# sees that it was built (and so the shell test scripts find it).
HOST_BIN = $(BUILDDIR)/amilisp$(EXE)

# Source files
PLATFORM_SRC = $(PLATFORM_IMPL) \
               $(SRCDIR)/platform/platform_thread_posix.c \
               $(SRCDIR)/platform/tls_openssl.c
CORE_SRC     = $(SRCDIR)/core/types.c \
               $(SRCDIR)/core/mem.c \
               $(SRCDIR)/core/gc_trace.c \
               $(GC_SRC) \
               $(SRCDIR)/core/gc_compact.c \
               $(SRCDIR)/core/gc_jitscan.c \
               $(SRCDIR)/core/gc_verify.c \
               $(SRCDIR)/core/mem_track.c \
               $(SRCDIR)/core/error.c \
               $(SRCDIR)/core/symbol.c \
               $(SRCDIR)/core/package.c \
               $(SRCDIR)/core/reader.c \
               $(SRCDIR)/core/readtable.c \
               $(SRCDIR)/core/printer.c \
               $(SRCDIR)/core/env.c \
               $(SRCDIR)/core/compiler.c \
               $(SRCDIR)/core/compiler_special.c \
               $(SRCDIR)/core/compiler_extra.c \
               $(SRCDIR)/core/peephole.c \
               $(SRCDIR)/core/vm.c \
               $(SRCDIR)/core/builtins.c \
               $(SRCDIR)/core/builtins_arith.c \
               $(SRCDIR)/core/builtins_io.c \
               $(SRCDIR)/core/builtins_format.c \
               $(SRCDIR)/core/builtins_mutation.c \
               $(SRCDIR)/core/builtins_strings.c \
               $(SRCDIR)/core/builtins_lists.c \
               $(SRCDIR)/core/builtins_hashtable.c \
               $(SRCDIR)/core/builtins_sequence.c \
               $(SRCDIR)/core/builtins_sequence2.c \
               $(SRCDIR)/core/builtins_type.c \
               $(SRCDIR)/core/builtins_condition.c \
               $(SRCDIR)/core/builtins_package.c \
               $(SRCDIR)/core/builtins_struct.c \
               $(SRCDIR)/core/builtins_array.c \
               $(SRCDIR)/core/builtins_stream.c \
               $(SRCDIR)/core/builtins_random.c \
               $(SRCDIR)/core/builtins_bitvector.c \
               $(SRCDIR)/core/builtins_pathname.c \
               $(SRCDIR)/core/builtins_describe.c \
               $(SRCDIR)/core/builtins_inspect.c \
               $(SRCDIR)/core/builtins_thread.c \
               $(SRCDIR)/core/builtins_ffi.c \
               $(SRCDIR)/core/builtins_amiga.c \
               $(SRCDIR)/core/bindtab.c \
               $(SRCDIR)/core/stream.c \
               $(SRCDIR)/core/bignum.c \
               $(SRCDIR)/core/ratio.c \
               $(SRCDIR)/core/float.c \
               $(SRCDIR)/core/float_dtoa.c \
               $(SRCDIR)/core/float_math.c \
               $(SRCDIR)/core/debugger.c \
               $(SRCDIR)/core/repl.c \
               $(SRCDIR)/core/fasl.c \
               $(SRCDIR)/core/image.c \
               $(SRCDIR)/core/cmdline.c \
               $(SRCDIR)/core/color.c \
               $(SRCDIR)/core/thread.c \
               $(SRCDIR)/core/string_utils.c
# Portable JIT pieces (no m68k codegen — those live only in
# Makefile.cross).  Compiled into every host build so unit tests can
# exercise the code buffer and the AArch64 encoders anywhere; the AArch64
# backend itself joins them on arm64 macOS and Linux (JIT_A64 above).
JIT_SRC      = $(SRCDIR)/jit/codebuf.c \
               $(SRCDIR)/jit/asm_a64.c
ifdef JIT_A64
JIT_SRC     += $(SRCDIR)/jit/jit_common.c \
               $(SRCDIR)/jit/jit_a64.c \
               $(SRCDIR)/jit/codeheap.c \
               $(SRCDIR)/jit/runtime.c \
               $(SRCDIR)/jit/runtime_nlx.c \
               $(SRCDIR)/jit/runtime_vmstack.c
endif
MAIN_SRC     = $(SRCDIR)/main.c

HOST_SRCS = $(MAIN_SRC) $(PLATFORM_SRC) $(CORE_SRC) $(JIT_SRC)
HOST_OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(HOST_SRCS))

# Test sources
TEST_SRCDIR = tests
TEST_SRCS   = $(wildcard $(TEST_SRCDIR)/test_*.c)
TEST_BINS   = $(patsubst $(TEST_SRCDIR)/%.c,$(BUILDDIR)/tests/%,$(TEST_SRCS))

# Core sources without main (for linking with tests)
LIB_SRCS = $(PLATFORM_SRC) $(CORE_SRC) $(JIT_SRC)
LIB_OBJS = $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(LIB_SRCS))

# Separate, non-LTO object tree for the test binaries (see CFLAGS_TEST above).
TESTOBJDIR    = $(BUILDDIR)/test-obj
LIB_TEST_OBJS = $(patsubst $(SRCDIR)/%.c,$(TESTOBJDIR)/%.o,$(LIB_SRCS))

DESTDIR ?=
PREFIX ?= /usr/local

.PHONY: host test test-fast test-jit-eager test-plus test-extra linux-test clean verify-amiga install-hooks docs-check docs-update guide test-gc-stress test-memleak test-mt-thread-exit-race fasl fasl-amiga clean-fasl-amiga image install install-layout uninstall

host: $(HOST_BIN)

# Keep the docs/*.md package symbol lists in sync with the real exports.
# docs-check (also run by `make test`: tests/test_docs_symbols.sh) fails if any documented extension package's export
# set drifts from docs/package-symbols.txt, or if docs/clamiga.md references a
# CLAMIGA symbol that is no longer exported.  docs-update regenerates the
# snapshot after you have updated the prose.  See tools/docs/package-symbols.sh.
docs-check: host
	@sh tools/docs/package-symbols.sh check $(HOST_BIN)

docs-update: host
	@sh tools/docs/package-symbols.sh generate $(HOST_BIN) > docs/package-symbols.txt
	@echo "docs-update: regenerated docs/package-symbols.txt"

# The documentation as AmigaGuide: README-FIRST.md, README.md and docs/*.md
# (and the clamacs README when the submodule is present) converted by
# tools/docs/md2guide.lisp, run by the host binary, into build/guide/ laid
# out as the binary release root is (README-FIRST.guide, amilisp.guide,
# clamacs.guide at the top, the reference under docs/) -- see
# tools/docs/md2guide.sh and specs/amigaguide-docs.md.  The binary release
# runs the same conversion into its staged root; the generated files are not
# committed.  tests/test_md2guide.sh (in `make test`) converts the same
# sources, so Markdown the converter cannot render fails the fast tier.
GUIDE_DIR = build/guide
guide: host
	@sh tools/docs/md2guide.sh $(HOST_BIN) $(GUIDE_DIR)

$(HOST_BIN): $(HOST_OBJS)
	@mkdir -p $(dir $@)
	$(CC_HOST) $(CFLAGS_HOST) -o $@ $^ $(HOST_LIBS)

# vm.c is compiled WITHOUT link-time optimization.  cl_vm_run is one giant
# computed-goto function whose register allocation is shared by every opcode;
# under -flto the link-time code generator makes different inlining/allocation
# choices for it than the plain compiler does, and from d467727 on those
# choices cost every opcode ~15-25% (call-free local-variable loops, measured
# by trunk/bench-opt.lisp vm.* rows; bisected, and nothing in the source of
# the hot handlers had changed).  Building vm.o at -O3 without LTO puts the
# interpreter back at (or under) its 0.4 speed while the rest of the runtime
# keeps whole-program LTO.  Per-file rather than global: the no-LTO shape is
# measurably slower for everything else (docs/sento-bench-results-0.8.md).
$(BUILDDIR)/core/vm.o: OBJ_CFLAGS_EXTRA = -fno-lto

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC_HOST) $(CFLAGS_HOST) $(OBJ_CFLAGS_EXTRA) -I$(SRCDIR) -MMD -MP -c -o $@ $<

# Include auto-generated dependency files
-include $(HOST_OBJS:.o=.d)

# Per-test-binary wall-clock watchdog (seconds).  Belt-and-suspenders on top
# of the in-process SIGALRM watchdog in tests/test.h: catches a hang that
# happens outside a RUN() (e.g. in setup) so a deadlock fails the CI job in
# minutes instead of blocking until GitHub's job timeout.  Used only when a
# timeout/gtimeout binary is on PATH.
TEST_BIN_TIMEOUT ?= 300

# Temp directory for the shell tests.  On Windows the shell and the binary
# under test disagree about what "/tmp" is: MSYS2 maps it to a mount a native
# amilisp.exe knows nothing about, and while command-line arguments are
# path-converted on the way to a native process, a temp path that travels
# through an environment variable or inside a generated .lisp file is not.
# Handing every script the Windows spelling up front makes both sides agree.
# Empty (and therefore inert) everywhere else.
ifdef WINDOWS
TEST_TMPDIR_ENV := TMPDIR=$(shell cygpath -m /tmp 2>/dev/null)
endif

# Tests
# Shell-driven tests, run by test-fast after the C unit tests.  A list plus
# one loop, rather than an inlined block per script, for a concrete reason:
# make hands a recipe to the shell as a SINGLE command line, and thirty
# inlined blocks pushed it past the 32K command-line limit on Windows (the
# symptom is `syntax error: unexpected end of file` from a truncated
# script).  Add a new shell test by adding its name here.
SHELL_TESTS = \
test_batch test_repl_values test_repl_paste test_boot_log test_mx_error_context \
                test_lib_search_cwd test_shim_registry test_cl_plus_ssl_facade test_fasl_compat \
                test_boot_fasl_recovery test_boot_source_compile test_load_exit \
                test_exit_hooks \
                test_gray_streams_reload test_gray_file_position test_gc_stream_finalize \
                test_mt_print_controls test_mt_package_isolation test_mt_gc_regression \
                test_mt_gc_compact_hang test_mt_stream_mutex_leak test_mt_dispatch_addmethod_race \
                test_mt_dispatch_cache_race test_mt_thread_exit_gc test_mt_thread_identity \
                test_mt_intern_stw test_mt_stream_close_race test_mt_interrupt_parked \
                test_mt_thread_exit_drain test_mt_stop_workers test_mt_thread_death_hook \
                test_lock_diag test_break_diag test_call_diag test_call_gen test_jit_a64 test_jit_a64_walk test_heap_verify test_cpu_selftest test_debugger_backtrace test_backtrace_lines test_backtrace_after_handled_error \
                test_debugger_eof test_inspect_eof \
                test_io_diag test_ql_socket_timeouts test_stream_outbuf_leak \
                test_shutdown_leak \
                test_tls_loopback test_compiler_chain_unwind test_reader_label_gc \
                test_fasl_reader_unwind test_mt_lock_contention_throughput \
                test_mp_heap_locks \
                test_mt_print_stress test_load_keywords test_load_rebind \
                test_dev_commands test_dev_tcp test_userinit test_compile_file_package \
                test_compile_file_stderr test_fasl_cache_dir test_fasl_struct_deps test_fasl_literal_identity test_make_load_form \
                test_struct_slot_access test_defconstant_fasl test_peephole_diff \
                test_tier4_phase1 test_tier4_phase2 test_tier4_phase3 test_local_inline \
                test_scan_opcodes test_runtime_forms test_compiler_buffers \
                test_defvar_special_fasl test_defvar_init_once_fasl test_stack_depth test_argv_utf8 \
                test_command_line_args \
                test_utf8_filenames test_image test_executable test_boot_image_scripts test_install_layout \
                test_finish_output_flush \
                test_amiga_bindgen \
                test_amiga_boopsi test_amiga_reaction test_amiga_mui test_amiga_curated_vs_raw \
                test_amiga_asyncio test_amiga_ahi test_amiga_iff test_amiga_gfx_examples \
                test_lib_fasl_portable test_fasl_source_name test_md2guide test_docs_symbols

# The ones that take no amilisp binary: two drive make itself, one the
# Aminet upload script (dry runs only, no network), one greps the sources
# (user-visible strings must be ASCII: the Amiga console is not UTF-8),
# one pins the byte layout of the release's Workbench icons (icons/), one
# keeps allocating calls out of argument lists next to heap values, one
# pins the release script's host-only editor exemptions to clamacs.lisp,
# one keeps function-cell writes inside the call-generation setter.
SHELL_TESTS_NOARG = test_cross_wide_knob test_test_extra test_aminet_upload \
                    test_ascii_messages test_icons test_gc_arg_order \
                    test_release_clamacs_modules test_symfn_funnel \
                    test_gc_mark_macros

test-fast: $(TEST_BINS) host
	@echo "=== Running tests (fast tier: skips sento/host-cold-test) ==="
	@export CLAMIGA_NO_USERINIT=1 $(TEST_TMPDIR_ENV); \
	failed=0; \
	tmo=$$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true); \
	for t in $(TEST_BINS); do \
		echo "--- $$(basename $$t) ---"; \
		rc=0; \
		if [ -n "$$tmo" ]; then \
			$$tmo $(TEST_BIN_TIMEOUT) $$t </dev/null || rc=$$?; \
		else \
			$$t </dev/null || rc=$$?; \
		fi; \
		if [ $$rc -eq 0 ]; then \
			echo "PASS"; \
		else \
			if [ $$rc -eq 124 ]; then \
				echo "FAIL ($$(basename $$t) TIMED OUT after $(TEST_BIN_TIMEOUT)s — likely deadlock)"; \
			else \
				echo "FAIL"; \
			fi; \
			failed=1; \
		fi; \
	done; \
	for s in $(SHELL_TESTS); do \
		echo "--- $$s ---"; \
		if sh $(TEST_SRCDIR)/$$s.sh $(HOST_BIN); then \
			echo "PASS"; \
		else \
			echo "FAIL"; \
			failed=1; \
		fi; \
	done; \
	for s in $(SHELL_TESTS_NOARG); do \
		echo "--- $$s ---"; \
		if sh $(TEST_SRCDIR)/$$s.sh; then \
			echo "PASS"; \
		else \
			echo "FAIL"; \
			failed=1; \
		fi; \
	done; \
	if [ $$failed -ne 0 ]; then echo "=== Some tests failed ==="; exit 1; fi; \
	echo "=== Fast tests passed ==="

# `make test` runs the fast tier only (the everyday gate).
test: test-fast

# `make test-jit-eager` runs the fast tier with every function compiled at
# definition (CLAMIGA_JIT_HOT=0): on an arm64 Mac, the whole suite as a
# differential run of the AArch64 walker against the interpreter.  Without
# a backend it is plain `make test`.  Expected to fail where a test reads
# frames back: native frames are not in EXT:BACKTRACE, line attribution or
# FRAME locals unless %JIT-SET-FRAMES is on (specs/native-backend-a64.md,
# phase 1 status) -- every other failure is a walker bug.
test-jit-eager:
	@CLAMIGA_JIT_HOT=0 $(MAKE) --no-print-directory test-fast

# `make test-gc-stress` builds a dedicated DEBUG_GC_STRESS binary (forces a
# compacting GC before every allocation) and runs the GC-stress regression
# suite against it.  Kept out of the fast tier because it needs a separate,
# slow build; run it after touching GC, the FASL reader/writer, the compiler,
# or any C builtin that holds CL_Obj values across allocating calls.
GC_STRESS_BUILDDIR = build/host-gcstress$(GC_DIR_SUFFIX)
test-gc-stress:
	@$(MAKE) --no-print-directory host \
		BUILDDIR=$(GC_STRESS_BUILDDIR) \
		DEBUG_FLAGS="-DDEBUG_GC_STRESS"
	@echo "--- test_gc_stress_regression ---"
	@$(TEST_TMPDIR_ENV) sh $(TEST_SRCDIR)/test_gc_stress_regression.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
ifeq ($(GC),immix)
	@echo "--- test_gc_stress_regression (CLAMIGA_GC_STRESS_IMMIX=8: line-sweep cycles between the compactions) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS_IMMIX=8 sh $(TEST_SRCDIR)/test_gc_stress_regression.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
endif
	@echo "--- test_mt_intern_stw (CLAMIGA_GC_STRESS=1) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_mt_intern_stw.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_mt_thread_death_hook (CLAMIGA_GC_STRESS=1) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_mt_thread_death_hook.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_peephole_diff (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_peephole_diff.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_tier4_phase1 (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_tier4_phase1.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_tier4_phase2 (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_tier4_phase2.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_local_inline (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_local_inline.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_tier4_phase3 (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_tier4_phase3.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_scan_opcodes (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_scan_opcodes.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_runtime_forms (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_runtime_forms.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_compiler_buffers (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_compiler_buffers.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_tls_loopback (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_tls_loopback.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_image (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_image.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_executable (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_executable.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_command_line_args (CLAMIGA_GC_STRESS=1, forced compaction: the argument list is consed at boot) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_command_line_args.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_shutdown_leak (CLAMIGA_GC_STRESS=1, forced compaction) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_shutdown_leak.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_md2guide (CLAMIGA_GC_STRESS=1, forced compaction: string/cons-heavy Lisp) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_md2guide.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_heap_verify (CLAMIGA_GC_STRESS=1, forced compaction: the verifier collects and allocates its report; light churn) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_heap_verify.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_call_diag (CLAMIGA_GC_STRESS=1, forced compaction: the diagnostics' error messages) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_call_diag.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_call_gen (CLAMIGA_GC_STRESS=1, forced compaction: every collection must bump the call generation) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_call_gen.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_jit_a64 (CLAMIGA_GC_STRESS=1, forced compaction: native entry, stubs swept with dead functions) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_jit_a64.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_jit_a64_walk (CLAMIGA_GC_STRESS=1, forced compaction: native frames, constants and helper calls across every collection) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_jit_a64_walk.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_defvar_init_once_fasl (CLAMIGA_GC_STRESS=1, forced compaction: DEFVAR's BOUNDP-guarded codegen) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_defvar_init_once_fasl.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_dev_tcp (CLAMIGA_GC_STRESS=1, forced compaction: the TCP dev port's frames, server, client, REPL leg and the two builtins under it) ---"
	@$(TEST_TMPDIR_ENV) CLAMIGA_GC_STRESS=1 sh $(TEST_SRCDIR)/test_dev_tcp.sh $(GC_STRESS_BUILDDIR)/amilisp$(EXE)

# `make test-memleak` builds a DEBUG_MEM_TRACK binary — every platform_alloc
# tagged with its call site — and asserts that a run ends with ZERO off-heap
# bytes outstanding.  This is the strong form of tests/test_shutdown_leak.sh:
# that one checks the shutdown paths report plausible figures, this one proves
# nothing at all was left behind, and names the file:line if something was.
#
# It matters far more than a host build suggests.  AmigaOS has no per-process
# reclaim: memory amilisp does not hand back before exiting is gone from the
# system pool until reboot, so an unfreed block is Fast RAM the user loses on
# every launch.  Kept out of the fast tier because it needs a separate build;
# run it after touching any shutdown path, the compiler pool, the FASL reader,
# or anything that holds platform_alloc'd memory in a static.
MEMTRACK_BUILDDIR = build/host-memtrack$(GC_DIR_SUFFIX)
test-memleak:
	@$(MAKE) --no-print-directory host \
		BUILDDIR=$(MEMTRACK_BUILDDIR) \
		DEBUG_FLAGS="-DDEBUG_MEM_TRACK"
	@echo "--- test_memleak_tracked (zero off-heap bytes at exit) ---"
	@$(TEST_TMPDIR_ENV) sh $(TEST_SRCDIR)/test_memleak_tracked.sh $(MEMTRACK_BUILDDIR)/amilisp$(EXE)

# `make test-mt-thread-exit-race` builds a dedicated DEBUG_THREAD_RACE_HOOKS
# binary whose sole purpose (see the constructor in src/core/thread.c) is to
# deterministically force the exact STW-hang-on-thread-exit race window that
# cl_thread_unregister's gc_condvar broadcast fixes, instead of relying on
# scheduler timing to hit it.  Kept out of the fast tier because it needs a
# separate build; run it after touching thread registration, safepoints, or
# stop-the-world coordination in thread.c.
RACE_BUILDDIR = build/host-race$(GC_DIR_SUFFIX)
test-mt-thread-exit-race: host
	@$(MAKE) --no-print-directory host \
		BUILDDIR=$(RACE_BUILDDIR) \
		DEBUG_FLAGS="-DDEBUG_THREAD_RACE_HOOKS"
	@echo "--- test_mt_thread_exit_gc (deterministic race) ---"
	@$(TEST_TMPDIR_ENV) sh $(TEST_SRCDIR)/test_mt_thread_exit_gc.sh $(HOST_BIN) $(RACE_BUILDDIR)/amilisp$(EXE)
	@echo "--- test_mt_lock_barge_race (deterministic race) ---"
	@$(TEST_TMPDIR_ENV) sh $(TEST_SRCDIR)/test_mt_lock_barge_race.sh $(HOST_BIN) $(RACE_BUILDDIR)/amilisp$(EXE)

# `make test-plus` adds the host-cold-test (sento cold-load smoke test) on top
# of the fast tier.
test-plus: test-fast
	@export CLAMIGA_NO_USERINIT=1 $(TEST_TMPDIR_ENV); \
	echo "--- host-cold-test ---"; \
	if $(MAKE) --no-print-directory host-cold-test; then \
		echo "PASS"; \
	else \
		echo "FAIL"; \
		exit 1; \
	fi; \
	echo "=== All tests passed ==="

$(TESTOBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC_HOST) $(CFLAGS_TEST) -I$(SRCDIR) -MMD -MP -c -o $@ $<

# Without this, make treats the test objects as intermediate files (they are
# only reached through the pattern rule above) and deletes them after each
# build — forcing a full recompile of the runtime on every `make test`.
# .SECONDARY keeps them so incremental test builds are fast.
.SECONDARY: $(LIB_TEST_OBJS)

-include $(LIB_TEST_OBJS:.o=.d)

$(BUILDDIR)/tests/%: $(TEST_SRCDIR)/%.c $(TEST_SRCDIR)/test.h $(LIB_TEST_OBJS)
	@mkdir -p $(dir $@)
	$(CC_HOST) $(CFLAGS_TEST) -I$(SRCDIR) -I$(TEST_SRCDIR) -o $@ $< $(LIB_TEST_OBJS) $(HOST_LIBS)

# Cold-load smoke test: runs the sento test suite end-to-end from an
# empty FASL cache.  Exercises the source-load + auto-cache path that
# carries lib/clos.lisp + lib/asdf.lisp + sento's full dependency tree
# through the compiler and FASL writer in one shot.  Catches regressions
# the C tests don't see — e.g. a missing/stock trivial-garbage backend
# (upstream trivial-garbage rejects cl-amiga as an unsupported Lisp,
# so the AmiLisp fork must be present in local-projects).
#
# Auto-skipped when prerequisites aren't met (no quicklisp install, no
# trunk script, no AmiLisp library forks in local-projects) so the
# target is safe to keep in `make test` for contributors without a
# quicklisp setup.
HOST_COLD_TEST_SCRIPT  = trunk/load-and-test-sento-system.lisp
HOST_COLD_TEST_LOG     = $(BUILDDIR)/cold-test.log
# Wall-clock watchdog (seconds). Matches the cold-sento headroom in
# trunk/run-load-and-test-all.sh so a genuine in-suite hang fails loudly
# instead of blocking `make test` forever.
HOST_COLD_TEST_TIMEOUT = 1800
host-cold-test: host
	@set -e; \
	if [ ! -f "$(HOME)/quicklisp/setup.lisp" ]; then \
	  echo "=== host-cold-test: ~/quicklisp not installed — skipped ==="; \
	  exit 0; \
	fi; \
	if [ ! -f "$(HOST_COLD_TEST_SCRIPT)" ]; then \
	  echo "=== host-cold-test: $(HOST_COLD_TEST_SCRIPT) missing — skipped ==="; \
	  exit 0; \
	fi; \
	if [ ! -d "$(QL_LOCAL_PROJECTS)/trivial-garbage" ]; then \
	  echo "=== host-cold-test: AmiLisp library forks not in local-projects — skipped ==="; \
	  exit 0; \
	fi; \
	echo "=== host-cold-test: clearing FASL cache and running $(HOST_COLD_TEST_SCRIPT) ==="; \
	rm -rf $(HOME)/.cache/common-lisp/cl-amiga-*; \
	mkdir -p $(dir $(HOST_COLD_TEST_LOG)); \
	tmo=$$(command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null || true); \
	if [ -n "$$tmo" ]; then \
	  runner="$$tmo $(HOST_COLD_TEST_TIMEOUT)"; \
	else \
	  runner=""; \
	  echo "=== host-cold-test: no timeout/gtimeout on PATH — running without watchdog ==="; \
	fi; \
	rc=0; \
	$$runner $(HOST_BIN) --no-userinit --heap 384M --non-interactive --load $(HOST_COLD_TEST_SCRIPT) \
	  </dev/null > $(HOST_COLD_TEST_LOG) 2>&1 || rc=$$?; \
	if [ $$rc -eq 124 ]; then \
	  echo "=== FAIL: host-cold-test timed out after $(HOST_COLD_TEST_TIMEOUT)s (see $(HOST_COLD_TEST_LOG)) ==="; \
	  tail -30 $(HOST_COLD_TEST_LOG); \
	  exit 1; \
	fi; \
	if grep -q "FATAL Signal" $(HOST_COLD_TEST_LOG); then \
	  echo "=== FAIL: amilisp crashed during cold load (see $(HOST_COLD_TEST_LOG)) ==="; \
	  grep -B2 -A1 "FATAL Signal" $(HOST_COLD_TEST_LOG) | head -20; \
	  exit 1; \
	fi; \
	if ! grep -qE "Did [0-9]+ checks" $(HOST_COLD_TEST_LOG); then \
	  echo "=== FAIL: sento test suite never reported its result line ==="; \
	  tail -30 $(HOST_COLD_TEST_LOG); \
	  exit 1; \
	fi; \
	if [ $$rc -ne 0 ]; then \
	  echo "=== FAIL: amilisp exited rc=$$rc ==="; \
	  tail -30 $(HOST_COLD_TEST_LOG); \
	  exit 1; \
	fi; \
	checks=$$(grep -oE "Did [0-9]+ checks" $(HOST_COLD_TEST_LOG) | head -1); \
	pass=$$(grep -oE "Pass: [0-9]+" $(HOST_COLD_TEST_LOG) | head -1); \
	fail=$$(grep -oE "Fail: [0-9]+" $(HOST_COLD_TEST_LOG) | head -1); \
	echo "=== host-cold-test: PASS ($$checks; $$pass; $$fail) ==="

# Run all trunk/load-and-test-*.lisp integration scripts and print an
# aggregate pass/fail tally.  Requires quicklisp and installed shims.
# NOT wired into 'make test' — heavyweight, needs quicklisp/ansi-tests.
# Set COLD=1 to clear the FASL cache before each script (cold-boot mode).
test-extra: host
	@export CLAMIGA_NO_USERINIT=1 $(TEST_TMPDIR_ENV); \
	sh trunk/run-load-and-test-all.sh $(if $(filter 1,$(COLD)),--cold)

# Run host build + tests inside an Ubuntu container (matches GitHub Actions
# `ubuntu-latest`).  Requires a working `docker` CLI (Docker Desktop, OrbStack,
# Colima, ...).  Mounts the working tree read-write so build artifacts land in
# build/host inside the container — wipe with `make clean` afterwards if
# needed since they are Linux ELFs, not host binaries.
LINUX_TEST_IMAGE ?= ubuntu:24.04
linux-test:
	@command -v docker >/dev/null 2>&1 || { \
	  echo "docker CLI not found — install Docker Desktop, OrbStack, or Colima"; \
	  exit 1; \
	}
	docker run --rm -v "$(CURDIR)":/work -w /work $(LINUX_TEST_IMAGE) bash -c '\
	  set -e; \
	  export DEBIAN_FRONTEND=noninteractive; \
	  apt-get update -qq >/dev/null; \
	  apt-get install -y -qq build-essential libffi-dev pkg-config >/dev/null; \
	  make clean >/dev/null; \
	  make host && make test'

# Verify Amiga test results (after FS-UAE run)
verify-amiga:
	@if [ ! -f build/amiga/test-results.log ]; then \
		echo "No test results found. Run FS-UAE first."; \
		exit 1; \
	fi
	@echo "=== Amiga Test Results ==="
	@cat build/amiga/test-results.log
	@echo ""
	@if grep -q "ALL TESTS PASSED" build/amiga/test-results.log; then \
		echo "=== Amiga verification PASSED ==="; \
	else \
		echo "=== Amiga verification FAILED ==="; \
		exit 1; \
	fi

# Pre-compile boot files to FASL for faster startup
# Regenerate the raw AmigaOS/MorphOS API bindings (lib/amiga/raw/) from the
# AmigaOS 3.2 NDK (NDK=<unpacked NDK 3.2>, default tools/aos32-ndk; falls
# back to the copy inside the cross toolchain), when MOS_SDK points at a
# copy of the MorphOS SDK's os-include (fd/ + clib/), the MorphOS tables,
# and from the MUI 3.8 developer kit (MUI_SDK=<copy of MUI:Developer>,
# default tools/mui-sdk) the muimaster module's constants; the MUI 5 SDK
# (MUI5_SDK=tools/mui5-sdk) and the MorphOS SDK's libraries/mui.h add the
# post-3.8 muimaster surface; the AHI developer kit (AHI_SDK=tools/ahi-sdk,
# the unpacked Developer/ drawer of Aminet's m68k-amigaos-ahidev.lha) makes
# ahi.device an AmigaOS module carrying devices/ahi.h's tags and structs.
# The output is
# committed; CI checks it (tests/test_amiga_bindgen.sh) but never needs the
# SDKs.  See README "Raw OS bindings".
gen-amiga-bindings: $(HOST_BIN)
	NDK="$(NDK)" MOS_SDK="$(MOS_SDK)" MUI_SDK="$(MUI_SDK)" MUI5_SDK="$(MUI5_SDK)" AHI_SDK="$(AHI_SDK)" sh scripts/gen-amiga-bindings.sh

# Both FASL targets compile with CLAMIGA_FASL_PORTABLE=1: the output is loaded
# by the byte-string m68k AmigaOS binaries (and MorphOS), so a non-ASCII
# string literal in lib/ fails here with the source line instead of on the
# Amiga (tests/test_lib_fasl_portable.sh).
fasl: $(HOST_BIN)
	@echo "=== Compiling boot.lisp → lib/boot.fasl ==="
	CLAMIGA_FASL_PORTABLE=1 $(HOST_BIN) --no-userinit --heap 24M \
		--eval '(compile-file "lib/boot.lisp" :output-file "lib/boot.fasl")' \
		--eval '(quit)'
	@echo "=== Compiling clos.lisp → lib/clos.fasl ==="
	CLAMIGA_FASL_PORTABLE=1 $(HOST_BIN) --no-userinit --heap 24M \
		--eval '(compile-file "lib/clos.lisp" :output-file "lib/clos.fasl")' \
		--eval '(quit)'

# Precompile lib/amiga/** (the curated modules, AMIGA.REACTION and the
# generated raw OS bindings) next to their sources, so an Amiga run -- the
# FS-UAE suite mounts this tree, and so does a real machine with the repo on
# it -- loads lib/amiga/raw/intuition.fasl instead of compiling ~4k forms on a
# 68020 at first REQUIRE.  Optional for development (the Amiga's faslcache
# does the same lazily) and gitignored; REQUIRE ignores a FASL that is older
# than its source or was written by another CL_FASL_VERSION.  The binary
# release runs the same script into its staging tree.
fasl-amiga: $(HOST_BIN)
	sh scripts/compile-lib-fasls.sh -o . -b $(HOST_BIN) --no-docstrings

clean-fasl-amiga:
	find lib/amiga -name '*.fasl' -delete

QL_LOCAL_PROJECTS ?= $(HOME)/quicklisp/local-projects

# RETIRED: the `swank` stub and `cl+ssl` facade now live in lib/shims/ and
# are registered on ASDF:*CENTRAL-REGISTRY* automatically when lib/asdf.lisp
# loads — searched before the Quicklisp and ocicl searchers, so they shadow
# any package-manager copy without touching the filesystem, work from the
# binary release, and stay invisible to other implementations sharing the
# same quicklisp tree.  Opt out with CLAMIGA_NO_SHIMS=1.
#
# This target remains only to clean up symlinks created by the old scheme
# (they dangle now that contrib/shims/ moved to lib/shims/).
install-shims:
	@echo "=> shims are auto-registered by lib/asdf.lisp (from lib/shims/);"
	@echo "   no local-projects installation is needed anymore."
	@for shim in swank cl+ssl; do \
	  dst="$(QL_LOCAL_PROJECTS)/$$shim"; \
	  if [ -L "$$dst" ] && ! [ -e "$$dst" ]; then \
	    case "$$(readlink "$$dst")" in \
	      */contrib/shims/$$shim) \
	        rm "$$dst" && echo "=> removed stale symlink $$dst";; \
	    esac; \
	  fi; \
	done

# Activate the auto-review git hook for this clone. Sets a RELATIVE core.hooksPath
# (githooks) so it survives the repo being moved and works the same in every clone.
# Run once after cloning. See scripts/review/README.md.
install-hooks:
	@git config core.hooksPath githooks
	@chmod +x githooks/* 2>/dev/null || true
	@echo "=> auto-review hook activated (core.hooksPath=githooks)"
	@echo "   bypass one commit with 'git commit --no-verify'; disable with CLAUDE_AUTO_REVIEW=0"

# Bare-boot heap image for `make install` (specs/image-save-load.md, README
# "Heap images"): the host counterpart of Makefile.cross `image-amiga` and
# Makefile.mos `image`.  Images are per-build, so the freshly built binary
# writes its own from lib/boot.fasl + clos.fasl, then restarts from it to
# prove the result (scripts/save-boot-image.lisp / verify-boot-image.lisp).
#
# It lands in $(IMAGE_DIR), deliberately OFF the startup discovery path
# (cwd, beside the binary, <prefix>/lib/amilisp/): an image beside
# build/host/amilisp would be restored by every test run from then on, and
# since the fingerprint keys on version/format rather than the binary's
# bytes, a leftover would outlive a rebuild that changed lib/boot.lisp and
# the gates would run the stale boot.  The verify step names it with --image
# for the same reason.  CLAMIGA_HOME is cleared so the dump comes from THIS
# tree's lib/, the one `install` copies, not from an installed one.
IMAGE_DIR = $(BUILDDIR)/image

image: $(HOST_BIN)
	@mkdir -p $(IMAGE_DIR)
	rm -f $(IMAGE_DIR)/amilisp.img
	cd $(IMAGE_DIR) && CLAMIGA_HOME= ../amilisp$(EXE) --no-userinit --no-image --non-interactive --boot-log --load $(CURDIR)/scripts/save-boot-image.lisp
	CLAMIGA_HOME= $(HOST_BIN) --no-userinit --non-interactive --boot-log --image $(IMAGE_DIR)/amilisp.img --load scripts/verify-boot-image.lisp

# Binary + lib/ + heap image under a prefix, SBCL-style: <prefix>/bin/amilisp,
# <prefix>/lib/amilisp/ (the runtime library) and, in there, the amilisp.img
# startup restores boot + CLOS from — the third executable-relative place
# discovery looks (src/main.c discover_image), so an installed amilisp starts
# the way the binary release does.  The image is regenerated first so it is
# always the just-built binary's own, dumped from the FASLs being installed;
# the two recipe-level sub-makes sequence fasl -> image -> copy under -j.
install: host fasl
	@$(MAKE) --no-print-directory image
	@$(MAKE) --no-print-directory install-layout

# The copying alone, once binary, FASLs and image are built — split out so
# tests/test_install_layout.sh can lay the tree out into a DESTDIR without
# recompiling lib/.  Only <prefix>/lib is created up front: `cp -R lib/ X`
# must find X absent to create it as a copy of lib/ on both BSD and GNU cp.
install-layout:
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib
	cp -pR lib/ $(DESTDIR)$(PREFIX)/lib/amilisp
	install -m755 $(BUILDDIR)/amilisp $(DESTDIR)$(PREFIX)/bin
	install -m644 $(IMAGE_DIR)/amilisp.img $(DESTDIR)$(PREFIX)/lib/amilisp/amilisp.img

# lib/amilisp/ goes as a whole, image included.
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/amilisp
	rm -rf $(DESTDIR)$(PREFIX)/lib/amilisp

clean:
	rm -rf build
	find lib/amiga -name '*.fasl' -delete
