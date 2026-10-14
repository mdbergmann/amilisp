# CL-Amiga

Common Lisp environment for AmigaOS 3+ with bytecode VM, written in C (C89/C99).

## Target Platforms

- **Primary target**: AmigaOS 3+, 68020+ CPU
- **Development host**: macOS / Linux

## Build

```
make host          # Build for host (gcc)
make test          # Fast test tier: C unit tests + shell tests (mandatory — must pass before committing)
make test-plus     # Fast tier + host-cold-test (sento cold-load smoke test)
make test-extra    # Heavyweight trunk integration scripts (quicklisp/ansi-tests)
make test-memleak  # Off-heap allocation tracer: assert zero bytes leak at exit
make clean         # Remove build artifacts
```

**These gates cover the clamiga runtime — the C code and the Lisp
library it ships.** The Lambda's Tale engine and the Closure game,
formerly subprojects under `examples/games/`, are **their own repos**
since 2026-07-24 (`../../lambda-tale` and `../../closure-tale` in
development; Closure vendors this repo and the engine as submodules)
with their own suites and their own `CLAUDE.md`.  Work over there
never gates on this repo — and work that leads back into the runtime
(a compiler bug, a missing CL function, an FFI gap found by playing
the game) is a commit *here*, with every gate above applying to it in
full.  Keep the two changes, and their gates, apart.

**Clamacs, the MUI editor/IDE, is the `clamacs/` submodule**
(https://github.com/mdbergmann/clamacs, since 2026-09-12).  It is written
in Common Lisp (`clamacs/lisp/`, `clamacs/specs/clamacs-lisp.md`; the C
editor under `clamacs/src/` is its frozen predecessor) and runs as a
clamiga instance of its own.  It is released *with* clamiga —
`scripts/make-binary-release.sh` compiles it into `lib/clamacs/` (FASLs
beside the sources) and saves a `clamacs.img` heap image next to each
`clamiga` binary in FS-UAE (`verify/realamiga/make-editor-image.sh`;
`make -f Makefile.cross editor-image-amiga` does it for a cross build,
`make -f Makefile.mos editor-image` natively on MorphOS), which the
`Clamacs` launcher starts with `clamiga --image clamacs.img --eval
"(clamacs::run)"` — but it keeps its own suites and its own `CLAUDE.md`.
The same rule applies: editor work is a commit in the submodule (pin
bumped here afterwards, `git add clamacs`), and anything it needs from
the runtime (an `EXT.DEV` command in `lib/dev-commands.lisp`, a compiler
or JIT fix) is a commit here under every gate above.  `make test` never
runs the editor's suites; `git submodule update --init clamacs` is all
the release build needs (no toolchain: the host binary compiles the
editor's FASLs).

`make host` builds a **native Windows .exe** from an MSYS2 mingw shell
(CLANGARM64/UCRT64/MINGW64) — `make host CC_HOST=clang`, `make test
CC_HOST=clang`.  The Makefile detects the environment from `uname -s` and
swaps in `src/platform/platform_win32.c` (Winsock, the console API,
VirtualAlloc, LoadLibrary) plus `win32_compat.c`; `platform_thread_posix.c`
is shared, since mingw ships winpthreads.  Two things to keep in mind when
touching Windows-facing code:

- **Paths**: everything the platform layer hands back is normalised to `/`,
  and a drive letter is the pathname's device.  A Windows path embedded in a
  Lisp string must not carry backslashes — the reader eats them as escapes.
- **Tests**: the shell tests get `TMPDIR` in Windows spelling from the
  Makefile, because only command-line arguments are path-converted on the way
  from the MSYS shell to a native process — env vars and paths written into
  generated `.lisp` files are not.  `tests/shpath.sh` has `native_path` for
  paths compared against clamiga's own output.  A shell script that decides
  whether a path is absolute must accept `C:/...` next to `/...`.
- **Line endings**: `.gitattributes` forces LF in the working tree (`* text=auto
  eol=lf`) because Git for Windows — and GitHub's Windows runners — default to
  `core.autocrlf=true`.  Without it a string literal that spans lines carries
  `\r\n` on Windows only, and a test comparing it against clamiga's own output
  fails there and nowhere else.

Cross-compile for Amiga and test via FS-UAE:
```
make -f Makefile.cross amiga        # Cross-compile to build/cross/clamiga (m68k-amigaos-gcc)
make -f Makefile.cross amiga FPU=1  # Hard-float variant to build/cross-fpu/clamiga (requires FPU)
make -f Makefile.cross amiga WIDE=1 # Wide-string variant (CHAR-CODE-LIMIT > 65533, for flexi-streams/drakma); composes with FPU=1; releases stay narrow
make -f Makefile.cross amiga GC=classic # Classic mark-sweep collector variant to build/cross-classic (the m68k default is Immix, specs/gc-backend-immix.md); composes with FPU=1/WIDE=1; Makefile.mos takes the same knob with the same default (GC=classic -> build/morphos-classic)
make -f Makefile.cross test-amiga   # Cross-compile, copy binary, launch FS-UAE, verify results
make -f Makefile.cross examples-amiga # Run the GUI examples (gfx/ + reaction/) unattended in FS-UAE, screenshots to build/amiga/shots/
make -f Makefile.cross image-amiga  # Save + verify a bare-boot clamiga.img beside the cross binary in FS-UAE (images are per-build; composes with FPU=1)
make -f Makefile.cross clean        # Remove cross-build artifacts
```
- Uses `m68k-amigaos-gcc` toolchain from `tools/m68k-amigaos-gcc/prefix`
- Binaries are linked stripped (`-s`); add `STRIP=0` to keep the symbol hunks when you need function names in MuForce/SegTracker hit reports or `m68k-amigaos-gdb`
- Only `test-amiga`/`test-amiga-lowend`/`run-examples.sh` copy the binary into `build/amiga/` (what FS-UAE runs) — a plain `amiga` build never touches it, so experimental builds with another `BUILDDIR=` are safe
- The only supported way to build the AmigaOS binary (there is no native vbcc build)
- `test-amiga` places the binary in `build/amiga/`, boots FS-UAE, runs the Amiga test suite, and verifies results
- Runs fully unattended: FS-UAE auto-quits (`C:UAEquit`) when the suite finishes, and a host-side watchdog (`verify/realamiga/run-fs-uae.sh`) kills it if clamiga hangs; results are checked automatically

## Release Process

Releases are tag-only (no GitHub release artifacts). Follow the `v0.4`/`v0.5` precedent:

For a downloadable **binary release** (AmigaOS 3 + MorphOS), run `scripts/make-binary-release.sh` after tagging — it cross-builds the aos3 binaries (soft-float and hard-float `FPU=1`), saves + verifies a bare-boot `clamiga.img` beside each of them in FS-UAE (heap images are per-build, so the staged m68k binaries write their own from the staged layout — needs the `test-amiga` emulator setup), packages a natively built MorphOS binary and its image (`MOS_BIN=...` / `MOS_IMG=...`, built with `Makefile.mos` and `make -f Makefile.mos image` on MorphOS), assembles `bin/aos3` + `bin/aos3-fpu` + `bin/mos` + `lib/` (FASLs where portable — the header comment documents which files must ship as source and why) + `docs/` + `examples/` + the three root Workbench icons with their IconX launcher scripts (`icons/`, drawn by `scripts/make-icons.py`; re-run it and commit the `.info` files after an art change) under `build/release/`, smoke-tests the layout (including that it starts from the image and that `--no-image` still boots from the FASLs), and emits `.zip`/`.lha`.

1. **Gates green first**: `make test-plus`, `make test-gc-stress`, `make test-extra`, `make -f Makefile.cross test-amiga`, and `make -f Makefile.cross test-amiga FPU=1` must all pass (`pkill fs-uae` first if an emulator is lingering). Record the Amiga-suite and test-extra pass counts — they go in the tag message.
2. **Bump the version** — exactly two files:
   - `src/core/types.h`: the `CL_VERSION_MAJOR/MINOR/PATCH` block + `CL_VERSION_DATE` (DD.MM.YYYY). Everything derives from this block (banner, `LISP-IMPLEMENTATION-VERSION`, `$VER:` cookie, FASL cache key — stale caches invalidate themselves; `tests/test_version.c` enforces the contract).
   - `README.md`: the two hardcoded examples in the "Version" section (`lisp-implementation-version` and `Version clamiga` output).
   - `CL_FASL_VERSION` is independent — bump it only if serialization actually changed (see FASL Versioning below).
3. **Commit** as `chore(release): bump version to X.Y` touching only those two files, with a headline paragraph summarizing the cycle (see `git show v0.4` for the format). Re-run `make test` on the bumped tree.
4. **Annotated tag**: `git tag -a vX.Y -m "CL-Amiga X.Y — <headlines>; Amiga suite N/N, test-extra N/0"` with the real numbers from step 1.
5. **Push**: `git push origin master vX.Y`.
6. **Aminet** (after `scripts/make-binary-release.sh`): `AMINET_UPLOADER="you@example.org (Name)" scripts/aminet-upload.sh --dry-run` stages `clamiga.lha` + `clamiga.readme` under `build/release/aminet/` from `scripts/aminet-readme.in` (release notes = the tag message, or `--notes FILE`) and checks them against Aminet's rules; without `--dry-run` it uploads by anonymous FTP to `main.aminet.net/new/` after a confirmation. The archive is uploaded under the *same name* as the existing package (that is how Aminet updates work; the version lives in the readme). Aminet asks for at most one update per week.

**Deriving the headlines — do not use `vPREV..HEAD`.** Master's history was rewritten (`git filter-repo`, 2026-07-24), so tags created before that point are no longer ancestors of master: `git log v0.4..HEAD` returns the *entire* rewritten history (786 commits), not the release delta, and silently yields headlines from cycles long past. Check with `git merge-base --is-ancestor vPREV HEAD`; when it fails, find the previous release's *rewritten* bump commit instead and diff from that:

```
git log --oneline --all --grep="bump version to X.Y"   # find the rewritten bump commit
git log <that-commit>..HEAD --oneline                  # the true delta
```

For 0.5 the correct base was `95bda65` (89 commits), not the `v0.4` tag.

## Architecture

- `CL_Obj` = `uint32_t` tagged value; heap pointers are **arena-relative byte offsets** (not raw pointers)
- Single arena, bump allocator with free-list fallback, mark-and-sweep GC with compaction (moving) when fragmented — see GC Safety below
- Single-pass compiler: S-expressions → bytecode; stack-based VM
- All OS calls go through `platform.h` (`platform_posix.c` / `platform_amiga.c`)
- **Threading** (MP package): kernel threads with per-thread VM, TLV dynamic bindings, stop-the-world GC coordination, locks, condition variables
  - POSIX: pthreads, pthread_rwlock, `__sync_*` atomics
  - AmigaOS: `CreateNewProc()`, `SignalSemaphore` (shared/exclusive), custom condvars via signal bits, `Forbid()`/`Permit()` atomics, `tc_UserData` TLS

## Coding Guidelines

- **All Common Lisp code in the runtime and compiler must conform to the HyperSpec** — when implementing or modifying CL functions, macros, or special forms, consult the [HyperSpec](https://www.lispworks.com/documentation/HyperSpec/Front/) as the authoritative reference
- **Tests must be written and verified against the HyperSpec** — test cases should validate behavior as specified by the standard, not just observed behavior
- **Memory/CPU efficiency is critical** — target is 68020 @ 14MHz with 8MB RAM
- All structs must work at 32-bit — no `size_t` or pointer-sized fields in heap objects
- Use `uint32_t`/`int32_t` explicitly, not `int` or `long` for sized data
- C89/C99 compatible — no C11+ features
- keep it simple, try not to overdo it in complexity
- **The VM hot path is fragile in two specific ways** (both bisected in the 0.8
  cycle, see `docs/sento-bench-results-0.8.md`): (1) anything written on every
  opcode/call must be per-thread — a process-wide static there (the Ctrl-C
  poll counter was one) makes all threads bounce one cache line and costs
  25%+ under MT; `trunk/bench-opt.lisp`'s `mt.*` rows catch it. (2) `vm.o` is
  deliberately built **without LTO** (`Makefile`, `OBJ_CFLAGS_EXTRA`); the
  giant `cl_vm_run` is sensitive to code added anywhere in it, so after
  touching `vm.c` compare the `vm.*` rows of bench-opt against
  `docs/benchmarks.md` before committing.

### FASL Versioning (Critical)

Whenever you change FASL serialization — or make any change that results in a different on-disk FASL byte layout — you **must** bump `CL_FASL_VERSION` in `src/core/fasl.h`.

- This covers: adding/removing/reordering FASL tags, changing how any object type is serialized, adding new bytecode opcodes or changing operand encoding, changing the bytecode/closure/struct wire format, or anything else that alters the bytes written by the FASL writer.
- Increment the version number and update the trailing comment to describe what changed (see the existing `v10:` comment as the format to follow).
- **Why it matters**: the version is checked on load; bumping it forces stale `.fasl` files to be rejected/recompiled instead of being misread as the new format, which would silently corrupt the VM. Forgetting to bump it is a memory-corruption bug, not a cosmetic one.
- After bumping, run `make fasl` to regenerate the bundled FASLs and run the full test suite (host + Amiga) to confirm load/recompile works.

### GC Safety (Critical)

Any C code that holds `CL_Obj` values across allocating calls **must** GC-protect them:

- **Allocating functions**: `cl_alloc`, `cl_cons`, `cl_make_string`, `cl_make_vector`, `cl_make_struct`, `cl_make_symbol`, and any function that calls these (including `cl_vm_apply`)
- **The pattern to watch for**: iterative list building with `result`/`tail` local variables and `cl_cons()` in a loop — the partially-built list is invisible to GC unless protected
- **Fix**: wrap with `CL_GC_PROTECT(var)` before the loop and `CL_GC_UNPROTECT(n)` after
- **Why it matters**: this is a **compacting (moving) GC** — when fragmentation forces compaction (which can be triggered inside any allocating call), live objects are relocated and arena-relative offsets are rewritten. A `CL_Obj` C local that isn't GC-protected will (a) be swept/freed if unreachable, or (b) hold a stale offset after objects move — either way silently corrupting memory. `CL_GC_PROTECT` registers the variable's address so the compactor forwards it.
- **Note**: values on the VM stack (`cl_vm.stack`) and in `args[]` (builtin function arguments) are already GC-rooted — no need to protect those
- **Host runs a generational collector** (`CL_GENGC`, see `specs/generational-gc.md`): minor collections MOVE young objects and `cl_gc()` itself is a moving (compacting) collection under it — never assume an explicit GC call is non-moving. The protection discipline above is exactly sufficient; `CLAMIGA_GENGC=0` selects the classic collector (always on Amiga).

## Debugging & Troubleshooting

- **Debug instrumentation** should be guarded by preprocessor flags (e.g. `#ifdef DEBUG_GC`, `#ifdef DEBUG_COMPILER`, `#ifdef DEBUG_VM`) — never leave unconditional debug output in the code
- Activate debug instrumentation via make flags: `make host DEBUG_FLAGS="-DDEBUG_GC -DDEBUG_COMPILER"` (or any combination of flags)
- The Makefile passes `$(DEBUG_FLAGS)` to `CFLAGS`, so any `-D` defines can be added without editing the Makefile
- Keep debug output behind these guards so it compiles to nothing in normal builds — zero overhead when not debugging
- **When fixing bugs and troubleshooting**, maximize the diagnostic and bug-source visibility built into clamiga itself — add clear error messages, runtime checks, assertions, and debug instrumentation so that problems can be diagnosed from clamiga's own output rather than requiring external tools or guesswork

## Tests

- **Tests are our specification** — every new feature or bugfix must have both host and Amiga tests
- **Every bug fix must include a regression test** that reproduces the bug and verifies the fix
- Host tests: `tests/test_*.c` using framework in `tests/test.h`; `make test` must pass before any commit
- Amiga tests: `tests/amiga/run-tests.lisp` — Lisp-based test suite run on AmigaOS via FS-UAE
- **Tests must be tight on production code** — test the exact behavior, not just the happy path; cover edge cases, boundary conditions, and error paths thoroughly
- **Target 90% test coverage** — aim for at least 90% coverage across the codebase
- **The gc-stress test suite is critical for the stability and resiliency of clamiga** (`make test-gc-stress`, forces compaction every alloc via `DEBUG_GC_STRESS`/`CLAMIGA_GC_STRESS=1` to make unprotected-`CL_Obj` GC bugs deterministic). When changing or adding any code, make sure there is coverage not only in the unit tests but **also in the gc-stress suite** — exercise new allocating paths under GC stress so compaction/relocation bugs surface in CI rather than in the field.

### Off-heap memory must be handed back (AmigaOS has no exit reclaim)

`platform_alloc` memory lives **outside** the GC arena, so no arena statistic
sees it and a host build cannot observe it leaking — the kernel reclaims the
address space at exit. **AmigaOS does not.** A block never freed is Fast RAM
gone from the system pool until reboot, so an exit-path leak is a per-launch
cost to the user, not a hygiene issue. (Measured: clamiga lost 3,743,160 bytes
on *every* launch on a Vampire until the 0.8 shutdown paths were fixed.)

- Run **`make test-memleak`** after touching any shutdown path, the compiler
  pool, the FASL reader/writer, or anything that parks `platform_alloc`
  memory in a static. It builds with `-DDEBUG_MEM_TRACK`, tags every
  allocation with its `__FILE__`/`__LINE__`, and fails with the exact call
  site if a run ends with anything outstanding.
- A new long-lived off-heap allocation needs a matching release in one of the
  shutdown functions `main()` calls, and a scenario in
  `tests/test_memleak_tracked.sh`.
- **Order matters**: teardown that takes away *shared* state (module mutexes,
  the compiler pool, stream tables, open files) must run only *after* the
  `cl_thread_count > 0` check in `main()`. Destroying a console mutex or a
  compiler lock while a worker thread is still live hangs the process at exit
  — `test_mt_print_stress` and `test_mt_gc_regression` catch it.
- `CLAMIGA_MEM_DIAG=1` makes each stage report what it handed back, and on
  AmigaOS prints the whole-process `AvailMem` delta. Pair it with `Avail`.

## Documentation

- **When adding or changing a feature, check `README.md` for outdated or missing content** and update it as part of the change — the README must not drift from what the code actually does.
- **Keep README changes user-facing and high-level.** Document what a feature is and how to use it. Do **not** add changelog-style notes, fixed-bug descriptions, internal implementation details, root-cause analyses, or other low-level information that isn't relevant to someone using the project. Those belong in commit messages, tests, or memory — not the README.
- For a new feature, add a short section describing how it works. Keep prose minimal: the best documentation is a comprehensive, runnable test file that demonstrates the feature end-to-end. Create such a test file if one doesn't already exist, then have the README point to it (e.g. "see `tests/test_ffi.c` / `tests/amiga/ffi-tests.lisp` for usage examples").
- Prefer linking to authoritative, executable examples over long explanatory text — the test file is the source of truth and stays current because `make test` runs it.

## Usability

- **Clear error, warning, and info messages from the compiler and runtime are very important**
- Strive for helpful, precise diagnostics that guide the user to the problem and solution
- When loading Lisp code and fixing bugs, **improve compiler error messages** — include source location (file, line), the offending form, and actionable guidance where possible

## Running clamiga

- When running `clamiga` to capture output (e.g. for debugging or verification), use **small timeouts (10 seconds)** and check periodically — the process may hang or run indefinitely on certain inputs

## Amiga Stack Requirements

- The reader and compiler recurse once per nesting level of the source. Since 2026-07 the per-level frames are small (pending-jump chains in the compiler, hoisted reader buffers) and both `read_expr` and `compile_expr_step` call `cl_check_recursion_guards` — a too-small stack now produces a clean, catchable "C stack nearly exhausted" error instead of silent memory corruption.
- **64K** (AmigaOS default) — sufficient for the core runtime and most of the test suite, but NOT for the GUI load path: `(require "amiga/gadtools")` or the Lambda's Tale UI nests `bi_load` frames (~5K each) on top of deep reader recursion. Fails cleanly with the guard error.
- **128K** — verified sufficient for a full from-source compile of the GUI/game path (test runner baseline) and for Quicklisp/FSet/fiveam (deep CLOS dispatch chains).

Since 2026-09-18 the m68k binary gives itself that 128K when it was started with less (`platform_run_main` swaps to a stack of its own, and `platform_process_exit` hands it back — every exit goes through that function, never a bare `_exit`): a Shell at the 64K default and a Workbench icon's 8000 bytes both work.  The `stack` CLI command still matters above 128K (the Quicklisp scripts on real hardware use `stack 800000`).  A Workbench start (argc 0) is turned into a command line by `platform_startup_args` from the icons' `ARGS`/`WINDOW` tool types; `tests/amiga/wb-check.lisp` + `verify/realamiga/wbrun.c` are the unattended leg.

## Integration Test Scripts

Reusable Lisp scripts in `trunk/` for loading and testing third-party libraries:

```
./build/host/clamiga --heap 24M --load trunk/load-and-test-5am.lisp    # Fiveam (57/57 tests)
./build/host/clamiga --heap 24M --load trunk/load-and-test-fset.lisp   # FSet (17/17 tests)
./build/host/clamiga --heap 64M --load trunk/load-and-test-str.lisp    # str (400/400 tests)
```

- These scripts work on both host and Amiga (use `#+amigaos`/`#-amigaos` for platform differences)
- On Amiga, use `--heap 48M` and `stack 800000` for quicklisp-based tests

## Reference

- **Common Lisp HyperSpec**: https://www.lispworks.com/documentation/HyperSpec/Front/
