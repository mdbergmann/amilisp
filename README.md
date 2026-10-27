# AmiLisp

[![CI](https://github.com/mdbergmann/amilisp/actions/workflows/ci.yml/badge.svg)](https://github.com/mdbergmann/amilisp/actions/workflows/ci.yml)

A Common Lisp implementation for AmigaOS 3+ (68020+) and, as a fully native PPC build, MorphOS — with AmigaOS 4 in reach on the same path — but also macOS, Linux and Windows.

> **Alpha software** — AmiLisp is under active development. The core language is functional and can run real-world CL libraries, but ANSI CL compliance is incomplete and APIs may change. See [Known Limitations](#known-limitations-and-future-work) for details.

AmiLisp is a bytecode-compiled Common Lisp environment written in C (C89/C99). It aims for ANSI Common Lisp compatibility and runs on classic Amiga hardware (or emulators like FS-UAE) as well as modern hosts (macOS, Linux, and Windows via MSYS2), where most development happens.

**Contents**

- [Getting started](#getting-started)
- [Why AmiLisp?](#why-amilisp)
- [Status](#status)
- [Building](#building)
- [Usage](#usage)
- [Libraries](#libraries-asdf-quicklisp-and-ocicl)
- [Editors and IDEs](#editors-and-ides)
- [Language and runtime](#language-and-runtime)
- [AmigaOS and MorphOS APIs](#amigaos-and-morphos-apis)
- [Documentation](#documentation)
- [Architecture](#architecture)
- [Known limitations](#known-limitations-and-future-work)
- [Project structure](#project-structure)
- [How it compares](#how-it-compares)

## Getting started

Five minutes from nothing to a running Lisp.  This is the host path (macOS
or Linux); Amiga and MorphOS users can skip the build and take the
ready-made binaries from Aminet instead — see
[Binary release](#binary-release-amigaos--morphos).

**1. Build it.**  You need a C compiler (gcc or clang), GNU make and
libffi (found through `pkg-config`; `brew install libffi` on macOS,
`apt install libffi-dev` on Debian/Ubuntu).  No submodules are needed for
this:

```
git clone https://github.com/mdbergmann/amilisp.git
cd amilisp
make host
```

The binary is `build/host/amilisp`.  (Windows: see
[Host (Windows)](#host-windows).)

**2. Say hello.**  Evaluate one form and exit:

```
./build/host/amilisp --no-userinit --non-interactive --eval '(format t "Hello World~%")'
```

prints `Hello World`.  `--no-userinit` skips your `~/.amilisprc` (you
don't have one yet, but it keeps the command reproducible) and
`--non-interactive` exits instead of dropping into the REPL afterwards.

**3. Start the REPL.**

```
./build/host/amilisp
```

The banner names the build you are running (CPU and float variant, heap
size, collector, JIT state) and whether it booted from a heap image or
from `lib/`.

```
COMMON-LISP-USER> (defun fib (n) (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))
FIB
COMMON-LISP-USER> (fib 20)
6765
COMMON-LISP-USER> (quit)
```

**4. Run a file.**  Put the `fib` definition and a `(format t "~a~%" (fib 20))`
into `fib.lisp`, then:

```
./build/host/amilisp --script fib.lisp     # load it and exit
./build/host/amilisp fib.lisp              # load it, then stay in the REPL
```

From here: [Usage](#usage) lists every command-line option,
[Libraries](#libraries-asdf-quicklisp-and-ocicl) gets you Quicklisp, and
[Editors and IDEs](#editors-and-ides) connects Emacs (SLY) or the bundled
[Clamacs](#clamacs-native-editor--ide) editor.  To run the result on a
real Amiga or in FS-UAE, see
[AmigaOS (cross-compile)](#amigaos-cross-compile-with-m68k-amigaos-gcc).

**New to Common Lisp?**  Start with Peter Seibel's
[Practical Common Lisp](https://gigamonkeys.com/book/) and the
[Common Lisp Cookbook](https://lispcookbook.github.io/cl-cookbook); the
[HyperSpec](https://www.lispworks.com/documentation/HyperSpec/Front/) is
the language reference.  [This blog post](https://nnamgreb.de/blog/Clamiga+-+Common+Lisp+for+the+Amiga)
introduces the project under its earlier name, Clamiga.

## Why AmiLisp?

There are already excellent Common Lisp implementations — SBCL, CCL, ECL, Clasp, CLISP — so why another one?

**Because none of them run on the Amiga** — neither the classic 68k machines nor the PPC-based next-gen systems (MorphOS, AmigaOS 4). The high-performance implementations (SBCL, CCL) are native-code compilers tied to modern architectures — x86-64, ARM, PPC — with no 68k backend and a memory footprint measured in tens of megabytes. Clasp is built on LLVM and targets C++ interop. CLISP, the closest in spirit — a compact bytecode interpreter in C — hasn't had a maintained AmigaOS build in decades.

AmiLisp is built for the constraint the others ignore: **a 68020 at 14 MHz with 4 MB of RAM (or even less).** It's a self-contained bytecode VM in portable C89/C99 with no external runtime dependencies on the Amiga — no libffi (there's a hand-written 68k trampoline), no LLVM, no C compiler needed at runtime. Values are 32-bit tagged words and heap pointers are arena-relative offsets, keeping the whole object model 32-bit-clean; the Amiga and MorphOS binaries run an Immix-style mark-region collector, so allocation is always a bump and a collection never sweeps the arena (the host gets a generational collector on top), and there's an optional native JIT on 68k hardware and on arm64 hosts. Yet it's ambitious enough on the language side to load ASDF, run Quicklisp, and pass the self-tests of real libraries (Alexandria, FSet, fiveam, Sento) — and it runs identically on a modern macOS, Linux or Windows host, where most development actually happens.

Because execution is bytecode, the object model is **architecture-agnostic**: the same compiled Lisp runs unchanged on 68k and PowerPC. Two targets are fully working today: classic **AmigaOS 3+** on 68020+ and a **fully native MorphOS (PPC)** build — including threading, FFI, GUI, and audio; even the compiled FASL files are compatible between the two (the MorphOS build has full Unicode strings, and its writer downgrades all-ASCII strings to the byte format the 68k build reads — only FASLs with non-ASCII string literals are PPC-side only). **AmigaOS 4** — the other PPC-based next-gen system — is a natural target on the same path. So while the design's tightest constraint is the classic Amiga, the aim is the whole Amiga family, not just the 68k machines.

In short: it exists to bring a modern, ANSI-aiming, library-capable Common Lisp to hardware every other implementation left behind — without giving up comfortable development on a fast host.

A side-by-side with SBCL, ECL, CCL, Clasp and CLISP is at the end of this
document: [How it compares](#how-it-compares).

## Status

AmiLisp can load **ASDF**, install and run **Quicklisp**, and successfully quickload libraries including **Alexandria**, **fiveam**, **FSet**, **Trivia**, **str** and **Sento** — their `asdf:test-system` suites pass end-to-end. Sento pulls in **lparallel**, **serapeum**, **bordeaux-threads**, **log4cl** and friends along the way. On the host, the **drakma** HTTP(S) client and the **Hunchentoot** web server run their own test suites too (see [Integration test scripts](#integration-test-scripts)).

On the Amiga side it drives the OS natively: Intuition, Graphics, GadTools, **ReAction** and **MUI** GUIs (custom MUI classes included, with Lisp dispatchers), ARexx, audio.device and AHI, IFF and asynchronous DOS I/O, plus generated 1:1 bindings for every OS 3.2 library. [Clamacs](#clamacs-native-editor--ide), an Emacs-flavoured MUI editor/IDE written in Common Lisp, ships with it.

**ANSI conformance** — the Paul Dietz ANSI test suite (`third_party/ansi-test/`) is the working spec. A bootstrap in `trunk/` runs it on host and Amiga:

- **CONS, SYMBOLS, NUMBERS, and SEQUENCES** (`load-and-test-ansi.lisp`) — passing.

A broad test suite covers the implementation, including threading, CLOS, conditions, the full numeric tower, FFI, both JITs, both collectors, heap images, and the AmigaOS GUI modules (Intuition, Graphics, GadTools, ReAction, MUI); the Amiga half runs unattended in FS-UAE.

### Screenshots

| AmigaOS 3 | MorphOS |
|---|---|
| ![AmiLisp booting and running the bouncing-lines GFX example on AmigaOS 3](docs/scrshts/amilisp-bounce.jpg) | ![AmiLisp booting and running Hello World on MorphOS](docs/scrshts/amilisp-mos.png) |

ReAction GUIs from Lisp — four of the [`examples/amiga/reaction/`](examples/amiga/reaction/) ports of the NDK examples, running on AmigaOS 3.2:

![The checkbox, fuelgauge, listbrowser and clicktab ReAction examples](docs/scrshts/amilisp-reaction.png)

MUI GUIs from Lisp — four of the [`examples/amiga/mui/`](examples/amiga/mui/) ports of the MUI SDK examples (layout, slidorama, pages, requester), running on AmigaOS 3.9 with MUI 3.8.  `layout`'s group places its children from a Lisp `MUIA_Group_LayoutHook`, and every levelmeter and slider in `slidorama` is an instance of a MUI custom class whose dispatcher is a Lisp function:

![The layout, slidorama, pages and requester MUI examples](docs/scrshts/amilisp-mui.png)

The same programs on MorphOS 3.20 (MUI 4, built into the OS) — layout, slidorama, pages, and `class1`, a custom class whose `MUIM_Draw` method is a Lisp function:

![The layout, slidorama, pages and class1 MUI examples on MorphOS](docs/scrshts/amilisp-mui-mos.png)

## Building

Ready-made AmigaOS 3 and MorphOS binaries are on Aminet as
[dev/lang/amilisp.lha](https://aminet.net/package/dev/lang/amilisp); everything
else is built from source.  A plain clone is enough for the host build
([Getting started](#getting-started) has the three commands).  Two things
are git submodules and are fetched on demand: the editor
([Clamacs](#clamacs-native-editor--ide)), which the binary release needs, and
the m68k cross toolchain, which the AmigaOS build needs:

```
git submodule update --init clamacs      # the editor (binary release)
```

### Host (macOS / Linux)

A C compiler (gcc or clang), GNU make and libffi (found through
`pkg-config`) are all the host build needs.  OpenSSL is optional and loaded at
runtime, for [TLS](#networking-tcp-tls-udp).

```
make host          # Build for host -> build/host/amilisp
make test          # Fast test tier (C unit + shell tests)
make test-plus     # Fast tier + host-cold-test (sento cold-load smoke test)
make test-extra    # Heavyweight trunk integration scripts
make test-gc-stress  # Test suite with a compacting GC on every allocation
make test-memleak  # Off-heap allocation tracer: assert nothing leaks at exit
make host GC=immix # The Immix-style collector instead (build/host-immix/); same gates with GC=immix
make fasl          # Regenerate the bundled lib/boot.fasl and lib/clos.fasl
make image         # Save + verify a bare-boot amilisp.img (build/host/image/) for `make install`
make guide         # Build the AmigaGuide documentation into build/guide/
make clean         # Remove build artifacts
```

On arm64 macOS and Linux the build includes the AArch64
[JIT](#jit-m68k-and-aarch64); `make host JIT=0` leaves it out, and
`make test-jit-eager` runs the test suite with every function compiled to
native code.

`make install` (and `make uninstall`) lays the binary, its `lib/` and a
bare-boot heap image out under an install prefix, the same way SBCL does —
`<prefix>/bin/amilisp` plus `<prefix>/lib/amilisp/` (with `amilisp.img` inside,
so the installed amilisp starts from an image like the binary release; see
[Heap images](#heap-images)). The prefix is `/usr/local` unless you say otherwise:

```
make install PREFIX=/opt/amilisp
```

An installed amilisp finds its library and its image there on its own, from
any working directory and with no environment setup (`tests/test_lib_search_cwd.sh`
covers the full resolution order, `tests/test_install_layout.sh` the installed
layout end to end).

### Host (Windows)

`make host` also builds a **native Windows executable** — a real `.exe` with no
msys2 runtime DLL behind it — from an [MSYS2](https://www.msys2.org/) mingw
shell (CLANGARM64, UCRT64 or MINGW64):

```
pacman -S mingw-w64-clang-aarch64-{clang,libffi,make}   # CLANGARM64; adjust prefix
make host CC_HOST=clang       # -> build/host/amilisp.exe
make test CC_HOST=clang
```

The Makefile detects the mingw environment from `uname -s` and switches the
platform layer to `src/platform/platform_win32.c` (Winsock, the console API,
`VirtualAlloc`, `LoadLibrary`), so the same targets work as on macOS/Linux.
Everything the runtime offers on a POSIX host is available: threads (MP),
sockets and TLS, the FFI, the generational GC, the debugger and the REPL.

Three Windows-specific notes:

- **Features.** `*features*` carries `:windows` next to `:unix` and `:posix`;
  use `#+windows` for what differs.
- **Paths.** Namestrings use `/`; a drive letter appears as the pathname's
  device, so `(truename "Makefile")` prints as `#P"C:/Users/you/amilisp/Makefile"`.
- **OpenSSL** is loaded at runtime, as on every other host. It is found by DLL
  name (`libssl-3-arm64.dll`, `libssl-3-x64.dll`, …) on `PATH`; point
  `AMILISP_LIBSSL` / `AMILISP_LIBCRYPTO` at specific files to override.
  `(ext:tls-available-p)` reports what was found.

### AmigaOS (cross-compile with m68k-amigaos-gcc)

The AmigaOS binary is cross-compiled on a POSIX host with `m68k-amigaos-gcc`
(gcc 6.5.0b from the AmigaPorts toolchain, pinned as a submodule).

First, install the `m68k-amigaos-gcc` cross toolchain:

```
./tools/setup-toolchain.sh          # auto-pick: download on macOS arm64, build elsewhere
./tools/setup-toolchain.sh --build   # force build-from-source on any host
./tools/setup-toolchain.sh --help    # all options
```

The toolchain itself is tracked as a git submodule
(`tools/m68k-amigaos-gcc` → [AmigaPorts/m68k-amigaos-gcc](https://github.com/AmigaPorts/m68k-amigaos-gcc),
pinned). On macOS arm64 the script downloads a prebuilt `prefix/` tarball
from the AmiLisp release; on every other host it runs `git submodule
update --init` and invokes the upstream `make all` (host build deps —
`gmp`, `mpfr`, `mpc`, `wget`, etc. — see `tools/m68k-amigaos-gcc/README.md`).

Then build AmiLisp:

```
make -f Makefile.cross amiga        # Cross-compile with m68k-amigaos-gcc
make -f Makefile.cross test-amiga   # Build, deploy to FS-UAE, run Amiga tests (68040 config)
make -f Makefile.cross test-amiga-lowend # The same on the 68020 baseline config
make -f Makefile.cross examples-amiga # Run + photograph the GUI examples (gfx/, reaction/, mui/) in FS-UAE (build/amiga/shots/)
make -f Makefile.cross image-amiga  # Save + verify a bare-boot amilisp.img beside the cross binary in FS-UAE (composes with FPU=1)
make -f Makefile.cross clean        # Remove cross-build artifacts
```

Adding `FPU=1` to any of these builds the hard-float variant (to
`build/cross-fpu/`): double arithmetic compiles to native 68881/68882
instructions instead of soft-float library calls — much faster on machines
that have an FPU (68881/68882 boards, 68040/68060, Vampire/PiStorm), but the
binary requires one.  `make -f Makefile.cross test-amiga FPU=1` runs the
Amiga test suite against the hard-float binary in FS-UAE's 68040 config.

`test-amiga` runs fully unattended: FS-UAE quits by itself when the suite
finishes, a host-side watchdog stops a hung run, and the results are checked
on the host.  The binaries are linked stripped; `STRIP=0` keeps the symbols
for debuggers and crash reports.

Adding `WIDE=1` builds the wide-string variant (to `build/cross-wide/`, or
`build/cross-fpu-wide/` combined with `FPU=1`): `CHAR-CODE-LIMIT` rises
above 65533, matching the host and MorphOS builds, which is what libraries
like flexi-streams and drakma require to load.  String representation stays
adaptive (8-bit for Latin-1 text, UTF-32 only for strings that actually
contain wider characters), so ASCII workloads cost the same as the default
build.  The released binaries stay narrow (8-bit) to keep the 68020/8MB
baseline lean — build with `WIDE=1` on big-RAM machines (Vampire, PiStorm)
if you want the Quicklisp HTTP stack.

The m68k binaries use the Immix-style collector (see
[Garbage collection](#garbage-collection)); adding `GC=classic` builds
one with the classic mark-sweep collector instead (to
`build/cross-classic/`, composing with `FPU=1` and `WIDE=1` the same
way).  `make -f Makefile.cross test-amiga GC=classic` runs the suite
against it, and `verify/realamiga/run-gc-phase0.sh BINDIR` measures a
collection cycle of either binary on the emulated machine
(`docs/gc-phase0-results.md` has the numbers for both collectors).

### MorphOS (native PPC build)

The MorphOS binary is built natively *under* MorphOS with the MorphOS SDK's GCC:

```
make -f Makefile.mos                # build build/morphos/amilisp
make -f Makefile.mos GC=classic     # the same with the classic collector (build/morphos-classic/)
make -f Makefile.mos image          # save + verify build/morphos/amilisp.img (bare-boot heap image)
make -f Makefile.mos clean
```

This is a fully native PowerPC build, not a 68k binary running under
emulation. Threading, sockets, and the whole `AMIGA` FFI/GUI/audio stack
work as on classic AmigaOS — Amiga library calls are dispatched from PPC
code to the (68k-ABI) library bases through MorphOS's ABox emulation layer.
PPC is 32-bit and big-endian like m68k, so FASL files compiled on AmigaOS
and MorphOS are byte-compatible. The one thing the MorphOS build omits is
the native JIT, which has no PPC backend — it runs the portable bytecode VM,
like a host build without one.

### Binary release (AmigaOS + MorphOS)

`scripts/make-binary-release.sh` packages a ready-to-run release for both
Amiga targets under `build/release/`:

```
MOS_BIN=./amilisp-mos scripts/make-binary-release.sh
```

It cross-compiles both AmigaOS 3 binaries — soft-float (`bin/aos3/`, runs
on any 68020+) and hard-float (`bin/aos3-fpu/`, requires an FPU) — takes a
natively built MorphOS binary (`MOS_BIN`, default `./amilisp-mos`), compiles
[Clamacs](#clamacs-native-editor--ide) from the `clamacs/` submodule into
`lib/clamacs/` and saves its heap image beside each binary (the MorphOS
one comes in as `CLAMACS_MOS_IMG`, default `./clamacs-mos.img`), and
assembles `amilisp-<version>/` with `bin/aos3/`, `bin/aos3-fpu/`, `bin/mos/` (a
`clamacs.img` next to each `amilisp` and its `amilisp.img`), `lib/` (precompiled
FASLs where portable — the core library, all of `lib/amiga/` including
the raw OS bindings, and the editor, with the sources alongside for reference —
and Lisp sources where compilation must happen on the target, i.e. asdf and
quicklisp), the package API reference under `docs/`, `examples/`, the
[AmigaGuide documentation](#amigaguide) (`README-FIRST.guide`,
`amilisp.guide` and `clamacs.guide` in the package root, the reference
guides under `docs/`, every guide with an icon that opens it in MultiView),
and six Workbench icons in the package root, one pair per binary
(`AmiLisp` / `Clamacs`, `AmiLisp-FPU` / `Clamacs-FPU`, `AmiLisp-MOS` /
`Clamacs-MOS`: IconX launchers from `icons/`, drawn by
`scripts/make-icons.py` like the guide icons, that start amilisp or the
editor from `bin/aos3`, `bin/aos3-fpu` or `bin/mos` on a double-click, with
no platform detection in the scripts; a snapshot without `bin/mos` ships
without the MOS pair). The root icons carry fixed positions (a 3 x 3 grid:
amilisp launchers, Clamacs launchers and the three guides as the columns,
soft-float, FPU and MorphOS as the rows) and the archives carry the
package drawer's own icon (`amilisp-<version>.info` beside the drawer),
whose window is sized for that grid and shows
only files with icons — then it smoke-tests the deployed layout and
produces `.zip` and `.lha` archives. The binaries find `lib/` relative to themselves, so the extracted
tree runs from any directory without assigns or environment variables.

The release is published on Aminet as
[dev/lang/amilisp.lha](https://aminet.net/package/dev/lang/amilisp).
`scripts/aminet-upload.sh` takes the `.lha` the release script produced,
writes the accompanying `amilisp.readme` from `scripts/aminet-readme.in`
(version, uploader and the release notes filled in — the annotated tag's
message by default, or `--notes FILE`), checks the pair against Aminet's
rules (readme fields, 40-character `Short:`, 78-column ASCII lines,
30-character file names, archive integrity) and uploads both by
anonymous FTP to `main.aminet.net/new/`, where the Aminet moderators pick
them up:

```
AMINET_UPLOADER="you@example.org (Your Name)" scripts/aminet-upload.sh --dry-run   # stage + check only
AMINET_UPLOADER="you@example.org (Your Name)" scripts/aminet-upload.sh             # ... and upload
```

Aminet updates a package by an upload under the same file name, so the
archive goes up as `amilisp.lha` with the version in the readme.  The
first upload under that name adds `--replaces dev/lang/clamiga.lha`, which
retires the package the project had as CL-Amiga.  The script runs from
macOS or Linux (not from MSYS2 on Windows).
`tests/test_aminet_upload.sh` is the executable specification of the
rules the script enforces.

### Pre-commit hook (auto-review + tests)

Optional. A `pre-commit` hook reviews staged changes with a headless `claude`
(auto-fixing issues and re-staging), then runs the fast test tier
(`make test-fast` — no sento) and blocks the commit on failure. Activate once
per clone:

```
make install-hooks
```

Bypass a single commit with `git commit --no-verify`. See
[`scripts/review/README.md`](scripts/review/README.md) for the full flow,
toggles, and safety guarantees.

## Usage

```
amilisp                              # Start the REPL
amilisp --load hello.lisp            # Load a file, then start the REPL
amilisp hello.lisp                   # The same: a bare argument is loaded
amilisp --eval '(print 42)'          # Evaluate a form, then start the REPL
amilisp --script hello.lisp          # Load a file and exit
amilisp --heap 8M                    # Start with an 8 MB heap
amilisp --image app.img              # Start from a saved heap image
amilisp --boot-log                   # Print boot phase timings ("; [boot] ...")
```

`--help` lists all options; `--load` and `--eval` may be repeated and run in
order, `--non-interactive` exits after them instead of starting the REPL, and
`--no-jit` keeps every function bytecode-only.  At startup amilisp loads the
user init file `~/.amilisprc` (AmigaOS: `S:.amilisprc`) unless
`--no-userinit` is given; a `.clamigarc` from before the rename is still
read when no `.amilisprc` exists.  `--boot-log` is handy on slow Amiga hardware
(shows progress during the multi-second boot) and for spotting startup-time
regressions; see `tests/test_boot_log.sh` for the exact behavior.

The interactive REPL colours its prompt, values, warnings and errors;
`--no-color` turns that off and `--color` forces it on (for a batch or
non-interactive run whose output goes to a terminal).  On AmigaOS and
MorphOS the console has no ANSI colours but pens, so there amilisp draws
with the Workbench's own text and highlight pens, bold and inverse video
(the prompt in bold blue, `WARNING:` and `ERROR:` as badges), which read on
a grey Shell and on a black one alike; see `tests/test_color.sh` and
`tests/test_color.c`.

### Program arguments

A bare argument is loaded like `--load`; everything after `--` belongs to
the program instead:

```
amilisp --load app.lisp -- input.txt "two words"
```

`ext:*command-line-args*` is then `("input.txt" "two words")` — verbatim,
never loaded, whatever it looks like — and it is already set when
`~/.amilisprc` and `ext:*restore-hooks*` run, so an application image
started as `amilisp --image app.img -- file` finds its files there.
`tests/test_command_line_args.sh` shows the whole contract.

### Starting from Workbench (AmigaOS)

amilisp can be started from an icon: its own tool icon, or a project icon
(a `.lisp` file, say) whose default tool is amilisp — double-clicked, or
shift-selected together with the tool.  There is no command line then;
two tool types stand in for it, read from the tool's icon and then from
each project's (a project's options come later and win where one wins):

- `ARGS=...` — the options, exactly as on a command line, e.g.
  `ARGS=--heap 8M --load Work:app.lisp`; a double-quoted stretch is one
  argument.
- `WINDOW=...` — the console, e.g. `WINDOW=CON:0/20/640/200/My Lisp/CLOSE`,
  or `NIL:` for none.  Without it amilisp gets an `AUTO` window that only
  appears once something is printed and stays until it is closed.

Every project is one argument after `--`, as a full path, so an
application takes them from `ext:*command-line-args*` — they are never
loaded on its behalf.  `ext:*workbench-started-p*` is `T`, the current
directory is the tool's drawer, and the run gets its 128 KB stack by
itself (see [Heap and stack sizing](#heap-and-stack-sizing)).
`tests/amiga/wb-check.lisp` is the runnable example: the Amiga suite starts
amilisp through `verify/realamiga/wbrun.c`, which sends the same
`WBStartup` message Workbench does.

### Where amilisp finds its library

amilisp finds `lib/` in three ways, in order: relative to the current working directory (so running it from the source root just works), under **`$AMILISP_HOME`**, and relative to the amilisp executable itself. The executable-relative lookup tries three layouts — `lib/` next to the binary (binary release), `../lib/amilisp/` (an install prefix: `<prefix>/bin/amilisp` + `<prefix>/lib/amilisp/`, the same layout SBCL uses), and `../../lib/` above it (the in-repo `build/host/amilisp`) — so an installed amilisp, or a symlink to the in-repo binary on `$PATH`, locates the bundled `lib/` from any directory with no environment setup at all. The same three locations are searched for a `amilisp.img` [heap image](#heap-images). On AmigaOS the executable-relative lookup is `PROGDIR:`. `AMILISP_HOME` is only needed for a binary that sits in none of those layouts (e.g. a bare copy of the executable); see `tests/test_lib_search_cwd.sh` for the exact resolution behavior.

### Heap and stack sizing

The default heap is **4 MB**. On the Amiga, plain amilisp — without Quicklisp and ASDF — gets by with as little as **`--heap 1M`** for writing simple programs: the full Common Lisp core boots in about **0.5 MB** (`(room)` on a fresh 1 MB-heap session reports ~51% used). Larger workloads need more:

| Use case                                   | Heap             |
|--------------------------------------------|------------------|
| Simple programs (Amiga, no Quicklisp/ASDF) | `--heap 1M`      |
| REPL / small programs                      | 4M (default)     |
| Loading ASDF                               | `--heap 11M`     |
| Quicklisp + quickload libraries (FSet, fiveam, ...) | `--heap 24M` |
| SLY / ICL sessions (SLYNK loaded)          | `--heap 96M`     |
| Sento with its whole dependency tree, cold | `--heap 96M` to `192M` |

On AmigaOS 3 the run always has at least **128 KB** of stack: when the
Shell's `stack` setting (64K by default) or a Workbench icon gives less,
amilisp switches to a 128 KB stack of its own for the duration.  So `stack`
matters only when a workload needs more than that — the Quicklisp-based
test scripts on real hardware, say:

```
stack 800000
amilisp --heap 24M
```

(MorphOS sizes its native stack itself, 1 MB.)  If the stack is too small
for a deeply nested form, amilisp signals a clean `C stack nearly exhausted`
error telling you to raise it — it never corrupts the session.

### REPL results

The REPL prints **every** value a form returns, one per line, starting on a
fresh line even when the form's own output didn't end with a newline — a form
that returns no values prints nothing at all. Input may span lines: an open
paren or an unfinished string literal makes the REPL prompt for the rest of
the form instead of erroring.

```lisp
CL-USER> (floor 7 2)
3
1
CL-USER> (values)
CL-USER> (let ((x (floor 7 2))) x)   ; one value, so one line
3
```

The standard history variables (CLHS 25.1.1) are bound after each form:
`*`, `**`, `***` hold the last three *primary* values, `/`, `//`, `///` the
last three value *lists*, and `+`, `++`, `+++` the last three forms
(`-` is the form currently being evaluated).

```lisp
CL-USER> (floor 7 2)
3
1
CL-USER> (list * /)
(3 (3 1))
```

See `tests/test_repl_values.sh` (interactive loop) and `tests/test_batch.sh`
(`--batch` loop) for the exact behavior.

### Debugging and introspection

**Ctrl-C interrupts running code** — pressing Ctrl-C (SIGINT on the host,
the shell break signal on AmigaOS/MorphOS) while Lisp code is running
enters the interactive debugger with a backtrace of the interrupted
computation and a `CONTINUE` restart that resumes it in place. In
non-interactive runs the interrupt aborts to top level, printing the
backtrace; a second Ctrl-C before the first is handled force-exits.
See `tests/test_break_diag.sh`.

**Interactive debugger** — an unhandled error in the REPL opens a `Debug>`
prompt offering the available restarts by number, `:q` to return to top
level, and any Lisp expression for inspection. Ctrl-D (EOF) at the prompt
acts like `:q`: it leaves the debugger and returns to the top-level REPL
(a second Ctrl-D there exits the session). The backtrace shown on entry
is capped at 20 frames; `:bt <n>` re-renders it at whatever depth you ask
for and `:bt all` shows every frame, so the `... N more frames` tail is never
the end of the story. Expressions you evaluate at the prompt run *on top of*
the error-time stack, so `(ext:backtrace)` and `(ext:frame-locals <n>)` there
report the frames of the error you are debugging. Frames are named after the
function they run: `defun`s, `defmethod` bodies (shown under the generic
function's name), and `flet`/`labels` locals under the name they were
declared with. `<anonymous>` means a genuinely unnamed `lambda`.
See `tests/test_debugger_backtrace.sh` and `tests/test_backtrace.c`.

**Interactive inspector** — `(inspect obj)` opens an `Inspect>` prompt that
numbers the object's components and lets you walk into them: `<n>` descends,
`u` goes back up, `r` returns to the root, `d` describes, `p` prints,
`e <expr>` evaluates, `q` leaves. Ctrl-D (EOF) at the prompt acts like `q`,
the same contract the `Debug>` prompt keeps: it leaves the inspector and
returns to the caller, with the session still running.
See `tests/test_inspect.c` and `tests/test_inspect_eof.sh`.

**Documentation and introspection** — docstrings are kept: `(documentation
'foo 'function)` answers for `defun`/`defmacro`/`defgeneric`, `'variable`
for `defvar`/`defparameter`/`defconstant`, `'type` for `deftype`/`defclass`/
`define-condition`, `'structure` for `defstruct`, and they survive
`compile-file` (the record is a load-time call in the FASL).
`(describe 'foo)` shows the lambda list as written, the documentation and
the source `file:line`; `(apropos "map" "CL")` / `apropos-list` search by
substring.  Binding `ext:*capture-documentation*` to `nil` around a compile
drops the strings for a lean image (`scripts/compile-lib-fasls.sh
--no-docstrings` does that for the release's Amiga modules); the bundled
`boot.fasl`/`clos.fasl` keep theirs, about 66 KB of heap.
See `tests/test_documentation.c`.

**Disassembly** — `(disassemble 'foo)` lists a function's bytecode and
`(jitexpand ...)` its native code; see [Disassembly](#disassembly).

**Diagnostic switches** — a few environment variables make amilisp report on
itself when something hangs or leaks: `AMILISP_GC_DIAG=1` prints one stderr
line per collection (kind, pause, heap occupancy) — useful for telling a GC
storm from a hang elsewhere (`tests/test_break_diag.sh`); `AMILISP_IO_DIAG=1`
traces every platform file operation (op, path, handle) as it is entered, so a
process stuck inside an OS file call names the operation in its last trace
line (`tests/test_io_diag.sh`); `AMILISP_LOCK_DIAG` and `AMILISP_SOCK_DIAG`
do the same for lock waits and sockets (see [Threads](#threads-mp) and
[Networking](#networking-tcp-tls-udp)), and `AMILISP_MEM_DIAG` for memory
handed back at exit (see [Memory outside the heap](#memory-outside-the-heap)).
On AmigaOS, set them with `SetEnv`.

### Version

From Lisp, on any platform:

```lisp
(lisp-implementation-type)     ; => "AmiLisp"
(lisp-implementation-version)  ; => "0.12.0"
```

On AmigaOS the binary also carries a standard `$VER:` cookie, so the Shell's
`Version` command works without starting the REPL:

```
1> Version amilisp
amilisp 0.12 (08.10.2026)
```

See `tests/test_version.c` for the full contract.

### Loading source and FASL files

AmiLisp ships a bytecode VM, so `compile-file` writes a `.fasl` and `load` can take either a `.lisp` source or a precompiled `.fasl`.

| Call                       | Behaviour                                                                                                                                                                                  |
|----------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `(load "x.lisp")`          | Looks up a cached FASL in the per-user cache (see below) and loads it if its mtime ≥ the source. Otherwise loads the source and **auto-writes** a fresh FASL to the cache for next time.   |
| `(load "x.fasl")`          | Loads that exact file. The per-user cache is **not** consulted — `.fasl` inputs are already-compiled artifacts.                                                                            |
| `(require "name")`         | Searches `lib/name.fasl` and `lib/name.lisp` (and `PROGDIR:lib/...` on Amiga) and picks the FASL when its mtime ≥ source. The name is a string designator — `(require :asdf)` and `(require 'asdf)` work too: the search retries the lowercase spelling, and a module already in `*modules*` under any case is not loaded again. Used internally for `clos`, `asdf`, etc. |
| `(compile-file "x.lisp")`  | Writes to the cache path (= what `compile-file-pathname` returns). `:output-file "x.fasl"` overrides.                                                                                      |

When a literal object reachable from compiled code is an instance of a class that defines a `make-load-form` method (CLHS 7.6), `compile-file` serializes the object as that method's creation + initialization forms and reconstructs it via those forms at load time, instead of dumping it slot-for-slot. `make-load-form-saving-slots` is provided, and the reconstructed object preserves a slot that points back at itself (the circular self-reference). Plain structures with no method keep the built-in fast path. See `tests/test_make_load_form.sh` (host) and the MAKE-LOAD-FORM cases in `tests/amiga/run-tests.lisp` (Amiga) for runnable examples.

**AmigaOS paths** follow AmigaDOS: only a volume or assign (`Work:`, `PROGDIR:`) makes a path absolute, and a leading `/` or an empty component (`a//b`) means the parent directory. So `(load "//src/main.lisp")` resolves the files `main.lisp` loads relative to that grandparent directory, just as the Shell would. Such a component appears as `:UP` in `pathname-directory` (see the AmigaDOS cases in `tests/test_pathname.c`).

**Per-user cache locations** (keyed by `amilisp` version + FASL format version, so a version bump invalidates everything automatically):

- POSIX: `~/.cache/common-lisp/amilisp-<version>-fasl<n>/<source-path>.fasl`
- AmigaOS: `S:amilisp/faslcache/<version>-fasl<n>/<source-path>.fasl`

`AMILISP_FASL_CACHE_DIR` (POSIX, Windows) moves the cache root. `--no-fasl-cache`, or `AMILISP_FASL_CACHE=0` in the environment, switches the implicit cache off: every `(load "x.lisp")` compiles the source, and nothing is read from or written to the cache.

**Structure layouts are checked.** Code that calls a `defstruct` accessor or keyword constructor is compiled against that structure's layout, and the `defstruct` usually lives in another file. Every FASL therefore records the layouts it depends on, and they are compared before the file runs: when a `defstruct` has changed since, `LOAD` recompiles the cached file that uses it (even though that file itself is untouched), and a FASL loaded by name signals an error naming the structure instead of reading the wrong slots. Macros and inline functions from other files are not tracked — after changing one, touch the files that use it, or load with `--no-fasl-cache`. See `tests/test_fasl_struct_deps.sh` for runnable examples.

Pre-built `lib/boot.fasl` and `lib/clos.fasl` ship with the binary; on the lower-end 020 baseline this cuts cold boot from ~92 s to ~9 s. `make fasl` regenerates them after editing `lib/boot.lisp`/`lib/clos.lisp`.

`make fasl-amiga` does the same for everything under `lib/amiga/` (the curated modules, `AMIGA.REACTION` and the generated raw OS bindings), writing `lib/amiga/**/*.fasl` next to the sources: an Amiga run of the tree — the FS-UAE suite, or a real machine with the repo on it — then loads e.g. `lib/amiga/raw/intuition.fasl` instead of compiling ~4k forms on a 68020 at the first `(require "amiga/raw/intuition")`. This is optional for development (the Amiga's faslcache does the same lazily on first use), the files are gitignored, and `REQUIRE` ignores a FASL that is older than its source or was written by another FASL format version. The binary release ships these FASLs (see below). The host-compiled FASLs are portable because the FASL format is arch/endian-neutral and the `lib/amiga` sources have no reader conditionals — all platform variance is decided at load time. See `tests/test_lib_fasl_portable.sh`.

Note: string literals in the `lib/` modules that ship as FASLs must stay ASCII-only — the m68k Amiga build is compiled without `CL_WIDE_STRINGS` to save RAM and cannot read FASLs that contain `FASL_TAG_WIDE_STRING`. The host and MorphOS builds have `CL_WIDE_STRINGS` (full Unicode, `CHAR-CODE-LIMIT` 1114112 — required by e.g. flexi-streams/drakma); their writers auto-downgrade all-ASCII wide strings to byte strings, so the shared `lib/` FASLs stay readable everywhere. `make fasl`, `make fasl-amiga` and the release script compile with `AMILISP_FASL_PORTABLE=1`, which makes the writer refuse such a literal on the host — the diagnostic names the file, source line and code point (`U+2014` for the usual em dash) — instead of the Amiga failing the whole module with a `BAD_TAG` deserialize error at load time. Comments are unaffected; docstrings are string literals like any other (they are recorded for `documentation`, see below), so the rule applies to them too.

### Exit hooks

`ext:*exit-hooks*` holds functions amilisp calls on its way out — after
`(quit)`, at the end of a `--script` / `--non-interactive` run, and when the
REPL reaches end of input. They run before any runtime teardown, so a hook can
still print, write files and stop threads. `(quit)` unwinds without running
`unwind-protect` cleanups, so this is the only place user code sees process
exit.

```lisp
(ext:add-exit-hook (lambda () (save-state "work.dat")))
(ext:add-exit-hook 'shutdown-server)      ; symbol resolved when the hook runs
(ext:remove-exit-hook 'shutdown-server)   ; => T if it was registered
```

Hooks run most recently added first; one that signals an error is reported and
skipped, and the rest still run. See [docs/ext.md](docs/ext.md#exit-hooks) and
the runnable examples in `tests/test_exit_hooks.c` / `tests/test_exit_hooks.sh`.

### Heap images

`(ext:save-image "mysession.img")` snapshots the entire session — everything
loaded, defined and computed — to one file, and `amilisp --image mysession.img`
is back at that exact state in a single read, skipping boot and all loads.  On
a 14MHz Amiga that turns a minutes-long quicklisp warm-up into a near-instant
start; a game or app can ship as `amilisp` + `app.img`, or as a single
[standalone executable](#standalone-executables).

```lisp
(load "my-big-system.lisp")
(ext:save-image "mysession.img" :quit t)   ; write the image and exit
```

```
amilisp --image mysession.img              ; next day: instantly back
```

A file named `amilisp.img` in the current directory (or next to the binary, or
in an install prefix's `lib/amilisp/`) is restored automatically at startup;
`--no-image` skips that.  Images are strictly per-build — a fingerprint makes
any other amilisp build (or platform/variant) refuse them cleanly — and can be
restored into a larger `--heap` than they were saved with.  Worker threads and open file/socket streams must be closed before
saving; `ext:*save-hooks*` / `ext:*restore-hooks*` exist to tear down and
rebuild such OS state around the snapshot, and `ext:*image-restored-p*` lets
`~/.amilisprc` skip loads the image already contains.  The refusal names
what is in the way — `SAVE-IMAGE: 1 worker thread(s) are still running
("arexx-port") ...` is the [ARexx port](#arexx-port-amigaos--morphos) that
`S:.amilisprc` opens for Clamacs — so the clean way to save an image or an
executable is a amilisp started with `--no-userinit`: load the project, save,
quit.  Process state is
re-derived on restore, not carried over: `*default-pathname-defaults*` names
the restoring process's directory, `*random-state*` is freshly seeded, and
the libraries the `AMIGA.*` modules opened are opened again before
`~/.amilisprc` runs (`amiga.ffi:define-library-variable` does it for a
module of your own).

The binary release starts this way itself: each shipped binary has a
bare-boot `amilisp.img` beside it (`bin/aos3/amilisp.img` and so on), so
startup restores boot and CLOS in one read instead of loading `lib/boot.fasl`
and `lib/clos.fasl`; `--no-image` boots from the FASLs, and `--boot-log`
prints the phase timings either way.  Images are per-build, so each is
written by its own binary: `make image` saves and verifies one for the host
build (`make install` puts it in `<prefix>/lib/amilisp/`), `make -f
Makefile.cross image-amiga` for a cross build unattended in FS-UAE, `make -f
Makefile.mos image` natively on MorphOS, and `scripts/make-binary-release.sh`
does it for the staged release (see `scripts/save-boot-image.lisp` /
`verify-boot-image.lisp`, exercised by `tests/test_boot_image_scripts.sh` and
`tests/test_install_layout.sh`).

For a shipped application image, `:shake-bindings t` additionally drops the
demand-interned binding tables of the raw OS modules (~150 KB for the four
common ones).  Names the program referenced before the save keep working;
names it never referenced stop existing — the delivery trade, described in
[docs/ext.md](docs/ext.md#shipping-an-image-shake-bindings).

See [docs/ext.md](docs/ext.md#heap-images) and the runnable examples in
`tests/test_image.sh` / `tests/test_image.c` (host) and
`tests/amiga/image-save.lisp` / `image-verify.lisp` (Amiga).

### Standalone executables

`:executable t` turns the image into a program of its own — one file, a
copy of amilisp with the image appended, that needs no `lib/`, no image
file and no launcher beside it:

```
amilisp --no-userinit
```

```lisp
(load "app.lisp")
(ext:save-image "app" :executable t :toplevel 'app:main :quit t)
```

```
app input.txt --verbose
```

(`--no-userinit` because the saving session must have no worker threads
and no open file or socket streams, and an rc file that opens the ARexx
port — or a `REPL-ATTACH`ed Clamacs — has started one; the refusal names
the thread.)

The whole command line belongs to the program (`ext:*command-line-args*`;
no amilisp option is parsed), `:toplevel` is the function it runs — its
return ends the process with status 0, `(quit n)` with `n`, an unhandled
error with 1 — and `:heap-size` sets the heap it starts with, in bytes.
Without `:toplevel` the executable starts at the REPL with the program
loaded.  It starts from a Workbench icon as well as from a Shell.  An
executable runs on the kind of machine that saved it, so each platform
builds its own.  [docs/ext.md](docs/ext.md#delivering-a-program-executable)
has the details; `tests/test_executable.sh` (host) and
`tests/amiga/exe-save.lisp` (Amiga) are the runnable examples.

## Libraries: ASDF, Quicklisp and ocicl

### Quicklisp

Quicklisp runs on AmiLisp, but the stock client doesn't recognise this implementation and pulls in libraries that assume features we don't have yet. So the project ships a small compat layer, a set of **library backends** — maintained forks of a few systems that now carry first-class AmiLisp support behind `#+amilisp` branches — and a tiny `swank` stub, and keeps the bootstrap entirely on its own side. The library forks are deliberately minimal and exist to be upstreamed once the remaining API gaps close.

**Installing Quicklisp on a fresh system** (where `~/quicklisp/` — Amiga: `S:quicklisp/` — does not exist yet). Do this once:

```lisp
(require "asdf")
(load "lib/quicklisp-install.lisp")
(amilisp-ql:install)                 ; downloads + installs the QL client, patches networking
```

`amilisp-ql:install` runs the standard `quicklisp-quickstart:install`, catches the network error it raises (AmiLisp isn't a registered `ql-impl` yet), loads the compat shim so networking works, and retries the dist install. The bundled shim systems in `lib/shims/` — the `swank` stub and the `cl+ssl` facade (which routes drakma/Hunchentoot TLS through AmiLisp's native `ext:socket-start-tls` instead of the CFFI-based original) — need no installation: loading ASDF registers them on `asdf:*central-registry*`, which is searched ahead of the Quicklisp and ocicl searchers, so they shadow the stock dist copies automatically. To opt out of the auto-registration, set `AMILISP_NO_SHIMS` to `1` in the environment before starting amilisp — host: `AMILISP_NO_SHIMS=1 amilisp`, Amiga shell: `SetEnv AMILISP_NO_SHIMS 1` (any other value, or unsetting it, re-enables the shims). One thing still needs to be on disk in `~/quicklisp/local-projects` (Amiga: `S:quicklisp/local-projects`): the AmiLisp **library forks** (listed below), which you install by cloning them into that directory — Quicklisp's local-projects searcher then resolves them ahead of the stock dist releases.

**Using Quicklisp** in any later session, once it is installed:

```lisp
(load #P"~/quicklisp/setup.lisp")
(load "lib/quicklisp-compat.lisp")
(ql:quickload "alexandria")
```

**What we patch** (the local changes shipped with the project):

- `lib/quicklisp-compat.lisp` — routes Quicklisp's networking through `ext:open-tcp-stream` and plain CL stream ops (working around generic-function dispatch limits in the stock `ql-network` interface), and adapts `directory-entries` to AmiLisp's `directory`. All Quicklisp downloads (installer and dist fetches alike) run with socket timeouts armed — a dead or stalled connection signals `ext:socket-timeout` instead of hanging the image; see `tests/test_ql_socket_timeouts.sh`.
- **AmiLisp library forks** (cloned into `~/quicklisp/local-projects`) — maintained forks that carry first-class AmiLisp support behind `#+amilisp` branches, so a stock `quickload` resolves them like any other implementation backend rather than needing a replacement package. See the [Library forks (AmiLisp backends)](#library-forks-amilisp-backends) table below for the full list.
- `lib/shims/swank` — a tiny stub package: several libraries such as clack name the `swank` system only to reach a couple of symbols for an optional remote-debug server they never start. It stays a shim (there is no upstream to fork) and is auto-registered on `asdf:*central-registry*` when ASDF loads.
- `lib/asdf.lisp` — `#+amilisp` adaptations: real binary FASL compile/load for cross-session persistence, AmigaOS path/device handling, and `*asdf-session*` NULL-safety.

Libraries confirmed working via `quickload` + `asdf:test-system` (`trunk/run-load-and-test-all.sh`) include **fiveam**, **FSet**, **cl-spark**, **str**, **closer-mop**, **CFFI**, **chipi** (cl-hab), and **Sento** — plus, on the host, the **drakma** HTTP/HTTPS client and the **Hunchentoot** web server (these two need a TCP/IP stack; see [Integration test scripts](#integration-test-scripts)). Loading these pulls in and exercises a much wider dependency graph along the way — **alexandria, serapeum, lparallel, log4cl, bordeaux-threads, cl+ssl, usocket, chipz, cl-who** and friends. Sento cold-compiles its full dependency tree, so give it ~96–128M of heap (more for a cold cache) and `stack 800000` on Amiga.

### Ocicl

[Ocicl](https://github.com/ocicl/ocicl) is an alternative to Quicklisp that distributes ASDF systems as OCI artifacts pulled from a container registry. It has two halves: the `ocicl` command-line tool, which fetches systems over HTTPS into a project-local `systems/` directory, and a runtime hook that teaches ASDF where to find them. AmiLisp consumes the second half directly — once the systems are on disk, an `ocicl`-managed `systems/` tree is just `.asd` files plus sources, which AmiLisp's ASDF loads like any other source registry.

**Install the systems with the host `ocicl` tool.** Run the upstream CLI (on the macOS/Linux host, or anywhere you have it) from your project directory to vendor the systems you need:

```sh
cd my-project
ocicl install alexandria       # downloads into ./systems/ and records ./systems.csv
```

This populates a `systems/` subdirectory and a `systems.csv` manifest inside the project.

**Point ASDF at the tree from `.amilisprc`.** AmiLisp reads `~/.amilisprc` (Amiga: `S:.amilisprc`) at startup, so add an `asdf:initialize-source-registry` form there to register the project's `systems/` directory as a search tree. ASDF requires `:tree`/`:directory` pathnames to be **absolute**, so merge the relative subdirectory against `*default-pathname-defaults*` (seeded with the current working directory) rather than passing a bare relative `#P"systems/"`:

```lisp
(require "asdf")
(asdf:initialize-source-registry
  `(:source-registry
    (:tree ,(merge-pathnames "systems/" *default-pathname-defaults*))
    :inherit-configuration))
```

(Note the backquote and `,` — the form is built with a computed absolute pathname, not quoted literally.) If the project doesn't live at the directory amilisp is launched from, use the `:home` token (resolved against your home directory regardless of the cwd) or a plain absolute pathname instead:

```lisp
(asdf:initialize-source-registry
  '(:source-registry
    (:tree (:home "my-project/systems/"))
    :inherit-configuration))
```

`ocicl` sometimes vendors into `ocicl/` rather than `systems/`; list both as separate `:tree` entries (earlier entries win on name clashes) so either layout is picked up:

```lisp
(asdf:initialize-source-registry
  `(:source-registry
    (:tree ,(merge-pathnames "systems/" *default-pathname-defaults*))
    (:tree ,(merge-pathnames "ocicl/"   *default-pathname-defaults*))
    :inherit-configuration))
```

After that, `(asdf:load-system "alexandria")` resolves against the `ocicl`-installed copy. Because this is plain ASDF, the AmiLisp library forks listed below still apply — clone any needed `*-amilisp.lisp` fork into a directory covered by the source registry so it takes precedence over the stock system. The bundled shims need no such step: `asdf:*central-registry*` (where `lib/shims/` registers) is searched before the source registry, so an `ocicl`-vendored `cl+ssl` or `swank` is shadowed by the AmiLisp facade/stub automatically.

> AmiLisp does not yet run the `ocicl` fetcher natively (that needs an on-Amiga OCI-registry client); install on the host and copy or share the `systems/` tree to the Amiga side.

### Library forks (AmiLisp backends)

Several third-party libraries don't recognise AmiLisp and either lack a porting
layer for it or assume features of other implementations. For these, the project
maintains forks that add an AmiLisp backend (a new `*-amilisp.lisp` file) or a
small `#+amilisp` adaptation. Clone them into
`~/quicklisp/local-projects/` (Amiga: `S:quicklisp/local-projects/`) so
Quicklisp's local-projects searcher picks them up ahead of the stock dist
versions. The goal is to upstream each one as the remaining API gaps close.

| Library | Fork repository | What the AmiLisp support adds |
|---------|-----------------|--------------------------------|
| **usocket** | https://github.com/mdbergmann/usocket | `backend/amilisp.lisp` — a usocket backend that wraps AmiLisp's `EXT`-package TCP sockets/streams. The networking foundation for drakma and Hunchentoot. |
| **bordeaux-threads** | https://github.com/mdbergmann/bordeaux-threads | `apiv1/impl-amilisp.lisp` + `apiv2/impl-amilisp.lisp` — maps the BT v1 and v2 thread/lock/condition-variable surface onto AmiLisp's `MP` package. Pulled in by Sento, lparallel, and most concurrent libraries. |
| **cffi** | https://github.com/mdbergmann/cffi | `src/cffi-amilisp.lisp` — a CFFI-SYS backend built on AmiLisp's `FFI` package (fully functional on the POSIX host; AmigaOS uses the library-vector model). Lets CFFI-dependent systems load. |
| **trivial-features** | https://github.com/mdbergmann/trivial-features | `src/tf-amilisp.lisp` — populates `*features*` with AmiLisp's OS/CPU/endianness keywords. Required by CFFI and cl+ssl for platform detection. |
| **closer-mop** | https://codeberg.org/mdbergmann/closer-mop | `#+amilisp` package definition plus a `closer-amilisp.lisp` backend that re-export AmiLisp's native AMOP subset under the CLOSER-MOP / C2MOP / C2CL names. |
| **trivial-cltl2** | https://github.com/mdbergmann/trivial-cltl2 | `amilisp.lisp` backend supplying the CLtL2 functions serapeum/trivia call (`declaration-information`, `variable-information`, `function-information`, `compiler-let`, `parse-macro` / `enclose`). |
| **introspect-environment** | https://github.com/mdbergmann/introspect-environment | `#+amilisp` `typexpand` / `typexpand-1` delegating to AmiLisp's native `amilisp:typexpand` / `amilisp:typexpand-1` (see `docs/amilisp.md`), so callers like serapeum's `explode-type` can resolve user `deftype` aliases. |
| **trivial-garbage** | https://github.com/mdbergmann/trivial-garbage | `#+amilisp` finalizers and weak pointers, with weak hash-tables falling back to ordinary (strong) tables. |
| **trivial-gray-streams** | https://github.com/mdbergmann/trivial-gray-streams | `#+amilisp` branch importing AmiLisp's native `GRAY` package (same package name as ECL/CLISP) and bridging the `stream-read-sequence` / `stream-write-sequence` generics. The portability layer most Gray-stream users (flexi-streams, chipz, drakma, Hunchentoot) build on. |
| **chipz** | https://github.com/mdbergmann/chipz | `#+amilisp` Gray-stream branch in `stream.lisp` — makes `make-decompressing-stream` work. Enables drakma's gzip/deflate `:decode-content`. |
| **float-features** | https://codeberg.org/mdbergmann/float-features | `#+amilisp` branch using AmiLisp's IEEE float-bits builtins (`amilisp:single-float-bits`, …). Needed by jzon to serialize floats (e.g. chipi-api's SSE JSON). |
| **rfc2388** | https://github.com/mdbergmann/rfc2388 | `#+amilisp` MIME multipart parsing using a `:latin-1` external format. Used by Hunchentoot for multipart form/file uploads. |
| **cl-fad** | https://github.com/mdbergmann/cl-fad | `#+amilisp` directory/pathname/file utilities (`list-directory`, `file-exists-p`, …) mapped onto AmiLisp's `directory`/`probe-file`. Used by Hunchentoot. |
| **hunchentoot** | https://github.com/mdbergmann/hunchentoot | `#+amilisp` web-server adaptations (e.g. `set-timeouts` over the usocket amilisp backend). Runs AmiLisp as an HTTP server. |
| **atomics** | https://codeberg.org/mdbergmann/atomics | `#+amilisp` branch in `atomics.lisp` mapping `cas` / `atomic-incf` / `atomic-decf` onto `mp:compare-and-swap` / `mp:atomic-incf` / `mp:atomic-decf`. Backs bordeaux-threads v2's atomic API. |
| **fset** | https://github.com/mdbergmann/fset | `#+amilisp` branches in `Code/port.lisp` (lock/memory-barrier stubs onto the `MP` package, a `make-char` helper). The functional-collections library; its own suite passes 17/17 on AmiLisp. |

> **fset dependency:** fset 2.4.x requires `misc-extensions` ≥ 4.2.4, which is
> newer than the version in the bundled Quicklisp dist. Clone the upstream
> [slburson/misc-extensions](https://github.com/slburson/misc-extensions) (≥ 4.2.4)
> into `~/quicklisp/local-projects/` as well — it needs no AmiLisp patch and
> loads as-is, but the local-projects copy must take precedence over the older dist
> release for fset to build.

### Integration test scripts

Reusable Lisp loaders in `trunk/` that load and exercise third-party libraries on both host and Amiga:

```
./build/host/amilisp --heap 24M  --load trunk/load-and-test-alexandria.lisp     # Alexandria (250/250)
./build/host/amilisp --heap 24M  --load trunk/load-and-test-5am.lisp            # Fiveam
./build/host/amilisp --heap 24M  --load trunk/load-and-test-fset.lisp           # FSet
./build/host/amilisp --heap 64M  --load trunk/load-and-test-trivia.lisp         # Trivia pattern matcher (490/490)
./build/host/amilisp --heap 24M  --load trunk/load-and-test-cl-spark.lisp       # cl-spark (sparklines, 68/68)
./build/host/amilisp --heap 64M  --load trunk/load-and-test-str.lisp            # str
./build/host/amilisp --heap 192M --load trunk/load-and-test-sento-system.lisp   # Sento (cold cache)
./build/host/amilisp --heap 192M --load trunk/load-and-test-knx-conn.lisp       # knx-conn KNXnet/IP (fiveam)
./build/host/amilisp --heap 96M  --load trunk/load-and-test-ansi.lisp           # ANSI cons + symbols + numbers
./build/host/amilisp --heap 256M --load trunk/load-and-test-cffi.lisp           # CFFI backend
./build/host/amilisp --heap 256M --load trunk/load-and-test-drakma.lisp         # drakma HTTP/HTTPS (host only)
./build/host/amilisp --heap 256M --load trunk/load-and-test-hunchentoot.lisp    # Hunchentoot server (host only)
./build/host/amilisp --heap 256M --load trunk/load-and-test-hunchentoot-ssl.lisp # Hunchentoot HTTPS server (host only)
./build/host/amilisp --heap 256M --load trunk/load-and-test-chipi-api.lisp      # chipi web API tests (host only)
./build/host/amilisp --heap 256M --load trunk/load-and-test-chipi-ui.lisp       # chipi-ui CLOG UI tests (host only)
```

`load-and-test-drakma.lisp` drives **drakma** as an HTTP/HTTPS **client** and
runs drakma's own test suite: plain HTTP and HTTPS, GET and POST, streamed and
gzip-decoded responses, and certificate verification. It loads over the
**usocket** AmiLisp backend, with HTTPS through the bundled **cl+ssl facade**
over the native TLS layer (see [TLS](docs/ext.md#tls)) and the **chipz** fork
for decompression.

`load-and-test-hunchentoot.lisp` runs AmiLisp itself as a web **server**: it
starts a Hunchentoot `easy-acceptor` and runs Hunchentoot's built-in confidence
suite against it (driving drakma over loopback through cookies, sessions,
multipart parameters, redirection and basic auth), rendering HTML with
**cl-who**.

`load-and-test-hunchentoot-ssl.lisp` is the HTTPS variant: a Hunchentoot
`easy-ssl-acceptor` serves the same confidence suite over TLS on loopback,
with drakma as the HTTPS client — AmiLisp is both ends of every encrypted
connection.

The drakma, Hunchentoot and chipi scripts are **host-only** — they need a TCP/IP stack and network access;
the same TLS stack on Amiga is covered by `tests/amiga/tls-tests.lisp` in the
FS-UAE suite (with AmiSSL installed in the emulated Workbench).

## Editors and IDEs

### Emacs (SLY) integration

AmiLisp speaks the SLYNK protocol, so you can drive it from Emacs with [SLY](https://github.com/joaotavora/sly) — REPL, completion, `M-.`, the inspector, and the SLDB debugger. This targets the **host** build (`build/host/amilisp`) and needs a SLY checkout whose `slynk/backend/` includes the AmiLisp backend (`clamiga.lisp`) — [this SLY fork](https://github.com/mdbergmann/sly) ships it.

amilisp comes up exactly like every other implementation — there is no amilisp-specific Lisp startup file or init form. The backend (`slynk/backend/clamiga.lisp`) pulls in amilisp's Gray streams itself via `(require "gray-streams")`, which needs to locate the bundled `lib/` (see [Where amilisp finds its library](#where-amilisp-finds-its-library)).


> **Heap sizing:** the 4 MB default thrashes the GC once SLYNK and its contribs load. Use **`--heap 96M` as a practical minimum** — that also carries a real application's dependency graph (e.g. `(asdf:load-system :sento)`). Give more headroom (`512M`) if you can.

#### Method A — auto-start with `M-x sly` (recommended)

Add a `amilisp` entry to `sly-lisp-implementations`. Don't set SLY's `:directory` — the executable-relative `lib/` lookup (or `AMILISP_HOME`) already lets amilisp start from anywhere, so the connection's working directory stays free to follow the buffer you start from:

```elisp
(defvar my/amilisp-root "/path/to/amilisp")
(defvar my/amilisp-bin  (expand-file-name "build/host/amilisp" my/amilisp-root))

;; Optional: only needed when the binary lives outside the AmiLisp tree —
;; build/host/amilisp finds its lib/ by itself via the executable location.
(setenv "AMILISP_HOME" my/amilisp-root)

(with-eval-after-load 'sly
  (add-to-list 'sly-lisp-implementations
               `(amilisp (,my/amilisp-bin "--heap" "512M"))))
```

(If you prefer the old behaviour of pinning the working directory to the source root, drop the `setenv` and add `:directory ,my/amilisp-root` back to the entry instead.)

Then `M-x sly` and pick `amilisp` (or `C-u M-x sly` to choose). SLY starts a server on an OS-assigned port (via ASDF + `slynk.asd`, which includes the AmiLisp backend) and connects automatically.

#### Method B — external server + `M-x sly-connect`

Start a server in a terminal, then connect to it (useful to keep the image alive across reconnects). A launcher ships with AmiLisp:

```sh
# From the AmiLisp repo root:
SLY_SLYNK_DIR=/path/to/sly/slynk \
  ./tools/sly/amilisp-slynk.sh                 # defaults: port 4005, heap 96M
# AMILISP_PORT=4006 and a trailing `--heap 192M' etc. also work.
```

It runs amilisp from the source root (so Gray streams resolve), loads `slynk-loader`, starts a server on the chosen port, and holds stdin open with `tail -f /dev/null` (otherwise the REPL reads EOF and exits, taking the server thread with it). Then in Emacs:

```
M-x sly-connect RET 127.0.0.1 RET 4005 RET
```

The equivalent by hand, without the script:

```sh
cd /path/to/amilisp
tail -f /dev/null | ./build/host/amilisp --heap 96M \
    --eval '(load "/path/to/sly/slynk/slynk-loader.lisp")' \
    --eval '(funcall (read-from-string "slynk-loader:init"))' \
    --eval '(funcall (read-from-string "slynk:create-server") :port 4005 :dont-close t)'
```

### ICL integration

[ICL](https://github.com/atgreen/icl) (Interactive Common Lisp) is a terminal/browser REPL frontend that drives an inferior Lisp over the SLYNK protocol — the same protocol AmiLisp already speaks for SLY, so amilisp works as an ICL backend. You need two things:

1. **A SLYNK with the AmiLisp backend** — the same [SLY fork](https://github.com/mdbergmann/sly) the Emacs integration uses (it ships `slynk/backend/clamiga.lisp`). ICL is pointed at it via `ICL_SLYNK_PATH` so it loads this SLYNK instead of its bundled upstream copy.
2. **ICL itself.** Stock ICL (≤ 1.23.10) crashes with `malformed property list` when `icl:configure-lisp` registers an implementation that isn't in its built-in table; use [this fork](https://github.com/mdbergmann/icl), which carries the fix until it lands upstream.

Register amilisp in `~/.iclrc`:

```lisp
;; Let amilisp's (require ...) find its bundled lib/ from any directory.
(setf (uiop:getenv "AMILISP_HOME") "/path/to/amilisp")

;; ICL must load a SLYNK whose slynk/backend/ includes clamiga.lisp —
;; point it at your SLY checkout (trailing slash required).
(setf (uiop:getenv "ICL_SLYNK_PATH") "/path/to/sly/slynk/")

;; The 4 MB default heap thrashes the GC once SLYNK loads;
;; 96M is a practical minimum.
(icl:configure-lisp :amilisp
  :program "/path/to/amilisp/build/host/amilisp"
  :args '("--heap" "96M")
  :eval-arg "--eval")

;; Optional: make amilisp the default for a plain `icl`.
;; (setf icl:*default-lisp* :amilisp)
```

Then run `icl --lisp amilisp`. ICL spawns amilisp, loads SLYNK via ASDF, and connects; evaluation, completion, `,doc`, the inspector, and the browser UI all run against the amilisp image. If something goes wrong at startup, `icl --verbose --lisp amilisp --eval '(+ 1 2)'` shows the spawn command and wire traffic.

### Clamacs (native editor / IDE)

![The Clamacs icon: a lambda on a blue card, the icon of Clamacs.app on a Mac and the lambda card of the Workbench icons](docs/scrshts/clamacs-icon.png)

[Clamacs](https://github.com/mdbergmann/clamacs) is an Emacs-flavoured Common Lisp editor and IDE for AmigaOS 3 and MorphOS, with a host variant for macOS and Linux (below): on the Amiga a native MUI application (a subclass of `TextEditor.mcc` with Emacs key handling, minibuffer, kill ring, Lisp indentation and sexp navigation) that drives a running amilisp over the [ARexx port](#arexx-port-amigaos--morphos) below — load, compile and evaluate from the buffer with clickable diagnostics, arglists, completion, jump to definition, describe and apropos, a REPL window (`C-c C-z`) fed by a REPL thread in amilisp, a debugger window (restarts, backtrace, locals, eval in a frame) and an inspector (`C-c I`). It is the on-Amiga counterpart to the SLY setup above: two processes, so a GC pause or a crash in one never freezes the other.

![Clamacs on a Vampire V4: the editor, the REPL window and the debugger window](https://raw.githubusercontent.com/mdbergmann/clamacs/master/docs/screenshots/debugger.png)

More screenshots (the inspector, the diagnostics window) are in the submodule's [README](https://github.com/mdbergmann/clamacs#screenshots).

Clamacs is written in Common Lisp and runs as a amilisp instance of its own. It lives in this repository as the `clamacs/` submodule and ships in the [binary release](#binary-release-amigaos--morphos) as a heap image next to each `amilisp` binary (`bin/aos3/clamacs.img`, `bin/mos/clamacs.img`, saved like `amilisp.img`) plus its sources and FASLs under `lib/clamacs/`. It needs MUI 3.8+ and `TextEditor.mcc` 15.29+ installed (MorphOS ships it). Start the editor:

```
bin/aos3/amilisp --image bin/aos3/clamacs.img --non-interactive --eval "(clamacs::run)" -- file.lisp
```

(the `Clamacs` Workbench icon runs that line; from a checkout, `amilisp --heap 8M --non-interactive --load clamacs/lisp/clamacs.lisp -- file.lisp` loads it from source). `S:.clamacsrc` is loaded before the first window opens and can define commands and bind keys (see `clamacs.guide`). The editor connects to a amilisp whose ARexx port is open, or starts one itself (**Clamiga > Start amilisp**, offered by the first command that needs it): that amilisp runs on the same binary with your `S:.amilisprc` loaded and is told to open its port, so no rc setup is needed for it — `(setq *amilisp-options* '("--no-userinit"))` in `S:.clamacsrc` makes it a bare one. To have the editor connect to a amilisp you start yourself, let `S:.amilisprc` open the port:

```lisp
;; S:.amilisprc
(require "amiga/arexx")
(amiga.arexx:start)
```

**On the host.** The same editor runs on macOS and Linux, in one native window with the buffers as tabs (its menus on the screen's menu bar on a Mac): `clamacs/host/run.sh file.lisp` starts it from a checkout (`make host-app` in the submodule builds `Clamacs.app` on a Mac), and the Lisp behind it is a separate amilisp reached over the [TCP development port](#tcp-development-port-all-platforms) below, started by the editor itself or found through `CLAMACS_AMILISP=host:port`. The submodule's README has the details under [On the host](https://github.com/mdbergmann/clamacs#on-the-host-macos-linux).

The editor's own suites and design notes are in the submodule (`clamacs/README.md`, `clamacs/CLAUDE.md`, `clamacs/specs/clamacs-lisp.md`); the Lisp-side commands it speaks are `EXT.DEV` (`lib/dev-commands.lisp`, `tests/test_dev_commands.sh`). `make -f Makefile.cross editor-image-amiga` saves and verifies a `clamacs.img` beside a cross build in FS-UAE, `make -f Makefile.mos editor-image` does the same natively on MorphOS.

### ARexx port (AmigaOS / MorphOS)

Native Amiga editors talk to a running amilisp over an ARexx port: trigger a load from CygnusEd or GoldED, get the compile diagnostics back, evaluate a form in the live image. This is the on-Amiga counterpart to the SLY setup above — no Emacs, no TCP, no host machine involved.

Start it from inside amilisp (put these two lines in `S:.amilisprc` to have every session offer the port):

```lisp
(require "amiga/arexx")
(amiga.arexx:start)          ; => "AMILISP"
```

The port is served by its own thread, so it answers even while the REPL is busy. A second amilisp claims `AMILISP.1`, `AMILISP.2`, and so on; `(amiga.arexx:port-name)` reports the name, `(amiga.arexx:stop)` shuts it down. `start` also registers `stop` as an exit hook, so a `(quit)` or the end of a `--non-interactive` run takes the port down with the process instead of leaving a dead task on it.

From an editor macro:

```rexx
OPTIONS RESULTS
OPTIONS FAILAT 21                          /* see the note below */
ADDRESS AMILISP 'LOAD Work:src/foo.lisp'
IF RC = 0 THEN SAY RESULT
ELSE DO
    ADDRESS AMILISP 'LASTRESULT'
    SAY RESULT
END
```

A failing load answers with every diagnostic in the file, not just the first:

```
; loading Work:src/foo.lisp
Work:src/foo.lisp:12: ERROR: Undefined function: RENDER-TILE
Work:src/foo.lisp:40: ERROR: Too many arguments to DRAW: expected 2, got 3
2 error(s), 0 warning(s)
```

| Command | Does |
|---------|------|
| `PING` | Liveness check; answers `PONG` |
| `VERSION` | Implementation, version, OS and CPU |
| `LOAD <file>` | Load a file, answer with its diagnostics |
| `COMPILE-FILE <file>` | Compile a file to a FASL, answer with its diagnostics |
| `EVAL <form>` | Evaluate one or more forms, answer with the printed values |
| `IN-PACKAGE <pkg>` | Set the package used by `EVAL`, `LOAD` and the commands below |
| `LASTRESULT` | Re-fetch the previous reply (see below) |
| `ARGLIST <symbol>` | The lambda list, on one line, in lower case: a function's as written, a macro's, a generic function's, a builtin's with `arg0 arg1 ...` placeholders, a special operator's from a table |
| `COMPLETE <prefix> [<pkg>]` | One candidate per line, exported symbols first, each group sorted, at most `ext.dev:*max-completions*` (200); `cl:map`, `pkg::name` and `:key` prefixes are honoured and kept; when no name starts with the text, the names that contain it |
| `DESCRIBE <symbol>` | `describe` output (lambda list, docstring, source file:line included) |
| `APROPOS <string> [<pkg>]` | One matching symbol per line with its kinds: `function`, `macro`, `special-operator`, `variable`, `class`; the symbols accessible in `<pkg>`, or in the command package |
| `SOURCE-LOCATION <symbol>` | `<file>:<line>` of the definition; rc 10 when none was recorded |
| `MACROEXPAND <form>`, `MACROEXPAND-1 <form>` | The expansion, laid out as code (body forms indented, arguments aligned) rather than filled to the margin; lines break at column `ext.dev:*pretty-margin*` (72) |
| `REPL-ATTACH <port> [DEBUG]` | Start the REPL thread, which talks back to the editor's ARexx port `<port>` (see below); answers with the current package. With `DEBUG`, an unhandled error opens the debugger protocol instead of ending the form |
| `REPL-EVAL <forms>` | Hand the forms to the REPL thread; answers at once (rc 10 while a form is still running), the values arrive with `RESULT` |
| `REPL-LOAD <file>` | Hand a `LOAD` of the file to the REPL thread (an editor's C-c C-k); answers at once. The file's output streams as `OUTPUT` while it loads, its errors and warnings are rows LOAD goes on past -- what the port's `LOAD` reports, never the debugger -- and `RESULT` carries the rows and the summary |
| `REPL-INPUT <line>` | Answer an outstanding `READLINE` |
| `REPL-INTERRUPT` | Abort the running form (also from inside the debugger) |
| `REPL-DETACH` | Stop the REPL thread |
| `BACKTRACE` | In the debugger: the frames of the current level, `<n>: <name>  <file>:<line>` |
| `RESTARTS` | In the debugger: `level <n>: <condition>`, then the current level's restarts as `DEBUGGER` listed them |
| `FRAME <n>` | In the debugger: the locals of frame `<n>`, `<name> = <value>` per line |
| `FRAME-EVAL <n> <forms>` | In the debugger: evaluate with frame `<n>`'s locals bound under their names (`ARG0`, `LOCAL3`, ...); answers at once, the values arrive as `OUTPUT`, an error opens a nested level |
| `RESTART <n>`, `ABORT`, `CONTINUE` | In the debugger: invoke restart `<n>` of the current level (interactively -- one that asks reads through `READLINE`), or the innermost `ABORT` / `CONTINUE` restart |
| `INSPECT <form>` | Evaluate the form and inspect its value: a header `<TYPE> <depth> <part-count>`, the object, then `<n>: <label> = <value>` per part (at most `ext.dev:*max-inspect-parts*`, 200). `*`, `**` and `***` are the REPL thread's |
| `PART <n>`, `POP` | Descend into part `<n>` of the object being inspected, or come back up; each answers like `INSPECT` |

A command string starting with `(` is evaluated directly, so `ADDRESS AMILISP '(room)'` works too. The introspection commands resolve names in the command package without interning: an unknown symbol or package is rc 10 with the reason.

**The REPL** is for an editor with a listener window. `EVAL` answers with everything a form printed, after the fact; a REPL wants the output as it happens, wants a `READ-LINE` to ask the editor, and must not tie up the port while a form runs. So `REPL-ATTACH` starts a second thread and the conversation becomes two-way: the port answers the editor's commands at once, and the REPL thread sends commands to the editor's port — `OUTPUT <text>` for each line (or 1 KB) of output, `READLINE` when the form reads standard input (the editor answers with `REPL-INPUT`), and `RESULT <rc> <package>` followed by the printed values, one per line, or the error text, when the form is done. The listener's `*`, `+` and friends are kept, `IN-PACKAGE` at either end is seen by the other, and arglist and completion keep working while a form runs. A form that exhausts the heap ends the REPL thread, which no handler can prevent; `RESULT` still arrives, with rc 10 and the error, and a new REPL thread has taken the old one's place by then. The REPL support (`lib/dev-repl.lisp`) is loaded on first use; `ext.dev:*repl-send*` is the function that delivers to the editor, which the host tests replace with a Lisp function playing the editor.

**The debugger** is the same shape once more. With `REPL-ATTACH <port> DEBUG`, an unhandled error does not end the form: the REPL thread stays on the erring stack, sends `DEBUGGER <level> <package>` followed by the condition and one restart per line (`<n>: <NAME> <report>`), and takes its next steps from the port -- `BACKTRACE`, `FRAME`, `FRAME-EVAL`, `RESTART`, `ABORT`, `CONTINUE` above. Every REPL form has an `ABORT` restart ("Return to the REPL"), an error inside a `FRAME-EVAL` is a nested level whose `ABORT` returns to the level below, and leaving a level re-announces the one underneath (`DEBUGGER 0` once none is left) before `RESULT` ends the form. `INSPECT`, `PART` and `POP` are the non-interactive face of the inspector (`ext:inspect-parts` lists an object's parts as `(label . value)` pairs) and answer at once on the port's thread.

**Return codes** follow the ARexx severity ladder: `0` success, `5` warnings, `10` errors, `20` unusable command. Two consequences worth knowing, both forced by the ARexx protocol rather than chosen:

- ARexx only transmits `RESULT` when the return code is **0**, so a failing command's diagnostics arrive via **`LASTRESULT`** — which is exactly why that command exists.
- ARexx aborts a macro once a return code reaches `FAILAT`, which **defaults to 10** — the code for "your file has errors". Editor macros want `OPTIONS FAILAT 21` so they survive to report the problem. (Warnings are `5` precisely so they never trip the default.)

Replies are capped at `ext.dev:*max-result-length*` (8 KB) and truncated on a line boundary.

**ASDF systems** are loaded by their definition file: `(ext.dev:load-asd-system "Work:src/foo/foo.asd")` loads ASDF if it is not there yet, then the file, then the system named after it with everything it depends on -- or, with a second argument (`"foo/tests"`), that one of the file's systems; `(ext.dev:test-asd-system ...)` runs `asdf:test-system` on it the same way. Both are functions rather than commands, meant for `REPL-EVAL`: a system takes long enough to want its output as it happens, an interrupt and the debugger.

Runnable macros are in [`examples/amiga/arexx/`](examples/amiga/arexx/): `amilisp.rexx` (a shell client — `rx amilisp.rexx LOAD Work:src/foo.lisp`) and `load-current-file.ced` (save-and-load bound to a CygnusEd key). `AMIGA.AREXX:SEND` drives *other* applications' ARexx ports from Lisp with the same protocol.

The command layer is portable Lisp (`lib/dev-commands.lisp`, package `EXT.DEV`) and runs on the host too, so `(ext.dev:handle-command "LOAD foo.lisp")` is testable without an Amiga; see `tests/test_dev_commands.sh` for the executable specification and `tests/amiga/arexx-tests.lisp` for the end-to-end port test. Your own verbs go in with `ext.dev:define-command`; a verb whose argument is text rather than syntax (an editor taking the REPL thread's `OUTPUT <chunk>`) uses `ext.dev:define-raw-command` and receives it verbatim, blanks and newlines included.

### TCP development port (all platforms)

The same commands over TCP: what the host Clamacs (macOS) talks to, and what lets a Clamacs on a Mac drive a amilisp on an Amiga across the LAN. Start it from inside amilisp:

```lisp
(require "dev-tcp")
(ext.dev.tcp:start)                          ; 127.0.0.1:4005, token drawn and printed once
(ext.dev.tcp:start :port 4005 :token "...")  ; a token of your own (an Amiga has no /dev/urandom)
(ext.dev.tcp:start :host "192.168.1.5")      ; reachable from the LAN -- on this one address only
```

Every connection must authenticate before anything is served, the port binds loopback unless `:host` names one address of the machine (never a wildcard), and `start` refuses to listen without a token -- one it drew from the OS entropy source (printed to amilisp's own output, once) or the `:token` it was given. `(ext.dev.tcp:stop)` takes it down, and so does the process exit; `(ext.dev.tcp:wait)` blocks until then, which is what a `--load` file whose only job is to serve the port ends with. A port serves at most 8 connections at once, 3 of them before their `AUTH` (`ext.dev.tcp:*max-connections*` / `*max-unauthenticated*`); one beyond that is closed unanswered. On a trusted network the token authenticates but nothing is encrypted.

The protocol is a length-framed line in each direction -- a request is `<n>\n` followed by `n` characters of command line, a reply `<rc> <n>\n` followed by the text, `<n>` counting characters with the bytes in UTF-8 -- and the first request on a connection is `AUTH <token>`, answered `0 2\nOK`. Every command of the table above works unchanged; the REPL attaches with `REPL-ATTACH tcp:<host>:<port>/<token>`, naming the *editor's* port and token, and amilisp's REPL thread opens a connection back to it for `OUTPUT`, `READLINE`, `RESULT` and `DEBUGGER`. `ext.dev.tcp:connect` / `ext.dev.tcp:request` are the client side in Lisp. `tests/test_dev_tcp.sh` is the executable specification (the refusals included), `tests/amiga/dev-tcp-tests.lisp` the same server on the Amiga's TCP stack.

## Language and runtime

### Declarations (`declaim` / `proclaim` / `declare`)

AmiLisp accepts the full ANSI declaration syntax so that portable code compiles
without error, but only a subset of declarations currently changes behavior. The
rest are parsed and accepted as conforming no-ops.

- **`declaim`** processes its specifiers at compile time *and* emits a
  `proclaim` call that runs whenever the form is executed — REPL, source load,
  or compiled-FASL load (per CLHS it behaves like `eval-when` with
  `:compile-toplevel :load-toplevel :execute`). So a library's
  `(declaim (optimize ...))` or `(declaim (special ...))` still takes global
  effect when its cached FASL is loaded, not only when it was compiled.
  **`proclaim`** is the plain runtime function form for the same specifiers —
  both apply globally. **`declare`** handles leading declarations in
  a body; `special` and `optimize` declarations are lexically scoped to that body
  (CLHS 3.3.4) and stop applying when it ends.
- **`special`** — *honored.* Marks the variable as dynamically bound (per-thread
  dynamic bindings / TLV). `declaim`/`proclaim` make it globally special; a local
  `(declare (special x))` is scoped to its binding form.
- **`optimize`** — *honored.* The qualities `speed`, `safety`, `space`, and
  `debug` are parsed (bare `quality` ≡ level 3; values clamped to 0–3; all
  default to 1). A `declaim`/`proclaim` sets the global baseline; a body
  `(declare (optimize ...))` overrides it for that body only.
  - At **`speed ≥ 1`** (the default) the compiler folds calls to pure fixnum
    builtins with constant arguments (`+ - * 1+ 1- ash logand logior logxor
    not null = < > <= >=`) into a single constant load, and eliminates the
    dead branch of an `if` whose test is a compile-time constant (this also
    collapses `when`/`unless`/`and`/`or` with constant operands). Folding is
    value-transparent: anything that would overflow fixnum range or involve
    floats/ratios is left to the runtime. `(optimize (speed 0))` disables both.
  - At **`safety 0`** the `(the type value)` runtime check (`OP_ASSERT_TYPE`)
    and the `destructuring-bind` guards — too-few/too-many arity and the
    unknown-keyword check of a `&key` section — are not emitted; at
    `safety ≥ 1` they are.
  - At **`speed ≥ 1`** (the default) a bytecode **peephole post-pass** runs
    over each compiled function: it removes store-then-reload round trips,
    discarded pure values, the dead store of a function's result slot and
    the jump to a shared return, fuses `(not ...)` tests into inverted
    branches, threads jump-to-jump chains, deletes unreachable code, and
    then fuses the most frequent adjacent opcode pairs into
    **superinstructions** (a local followed by a global call, a struct
    slot read of a local, a local or special variable used as an `if`
    test, an `eq` test that branches, and so on — fourteen shapes, chosen
    from an opcode-pair profile of a message-passing workload — plus a
    numeric or `char=` comparison that branches). Both
    JITs compile the optimized stream for free. The rewrite is
    semantics-preserving: type errors from discarded values (e.g.
    `(car 5)`), multiple-values state, and non-local exits all behave
    exactly as at `speed 0`, which is the only level that keeps the
    bytecode as emitted.
  - `compilation-speed` is interned (so libraries can name
    `cl:compilation-speed`) but ignored, and any non-standard quality — e.g.
    `security` — is silently accepted and ignored.
- **`inline` / `notinline`** — `notinline` is *honored*: it suppresses
  compiler-macro expansion, constant folding, the builtin-to-opcode
  inlining, and the local-function inlining below for the named functions,
  forcing real out-of-line calls.  `inline` sets a flag on the function
  symbol (visible via `describe`) but does not yet force inlining of
  global user functions.
- **Character and vector access is inlined.** A two-argument `aref`,
  `svref`, `char` or `schar`, and a two-argument `char=`, compile to one
  opcode each instead of a call (same type checks and error messages as
  the functions); a comparison under `if`/`when`/`unless`/`dotimes`
  branches directly; `push`/`pop` on a local variable and `incf`/`decf`
  of a local by a constant are one or three opcodes. A character scan such
  as an editor's paren matcher runs 3 to 7 times faster on a 68040-class
  machine for it. See `tests/test_scan_opcodes.sh` for what is inlined and
  what stays a call.
- **Local functions are inlined automatically.** A `flet`/`labels`
  function that never escapes — it is not `#'`-referenced, not called
  from inside a `lambda` or another local function, not recursive — is
  compiled into each of its call sites instead of a closure, with its free
  variables still resolving where the function was defined. Such a call
  costs what its body costs (no frame, no closure allocation when the
  `flet` is entered). Functions with `&optional`/`&rest`/`&key`, special
  parameters, a `load-time-value` in the body (its value stays one object
  per function, not one per call site), or a large body called from
  several places keep a closure, as does everything under
  `(optimize (space 2))`, `(debug 3)`, or a `notinline` declaration; an
  inlined call has no frame of its own in a backtrace. `AMILISP_NO_LOCAL_INLINE=1` turns it off process-wide.
  See `tests/test_local_inline.sh` for what is and is not inlined.
- **`type`, `ftype`, `ignore`, `ignorable`, `dynamic-extent`** — accepted but
  currently no-ops (no type propagation, unused-variable warnings, or
  stack-allocation).

See `tests/test_optimize.c` and the "Optimize declarations" section of
`tests/amiga/run-tests.lisp` for runnable examples of the folding,
dead-branch, scoping, and check-elision behavior; the implementation lives in
`try_fold_constant`/`compile_if`/`compile_call` (`src/core/compiler.c`) and
`cl_process_declaration_specifier` (`src/core/compiler_extra.c`).

### The peephole post-pass in practice

**How it works**: when a function finishes compiling with an effective
`speed ≥ 1`, its bytecode is decoded into an instruction list, rewritten
(store-reload elimination, discarded-pure-value removal, dead result-slot
stores, returns in place of jumps to a return, `(not ...)` branch fusion,
jump threading, dead code), fused into superinstructions, and re-encoded
with all jump and non-local-exit offsets recomputed. The pass is
fail-safe: anything it does not fully understand makes it leave the
bytecode untouched, so it can never miscompile — only miss an
optimization. `disassemble` shows the result; a fused opcode is named
after its members (`LOAD_CALL_GLOBAL`, `GLOAD_JNIL`, `STORE_POP`, ...).

**When it applies**: at **compile time only** — inside `defun`, `compile`,
`compile-file`, and source `load` — whenever the function's effective
`speed` is ≥ 1, i.e. always unless a `declaim`/`proclaim` baseline or the
`AMILISP_FORCE_SPEED=0` environment variable (which pins the effective
`speed` for the whole process, overriding declarations — handy for A/B
testing any workload) sets it to 0. `AMILISP_NO_FUSE=1` keeps the
rewrites but leaves the opcode pairs unfused, for the same A/B purpose.

**FASL caches**: the optimization is baked into the compiled bytecode, so a
`.fasl` stays optimized for everyone who loads it, regardless of their
current settings. The flip side: **loading a cached FASL never re-runs the
compiler**, so code compiled at `speed 0` stays as emitted until it is
actually recompiled — changing `speed` (or `AMILISP_FORCE_SPEED`)
afterwards has no effect on a warm cache. To push an already-compiled
library through the pass, clear its FASL cache first (for ASDF/Quicklisp
systems on the host: `~/.cache/common-lisp/amilisp-*`) and reload.

`tests/test_peephole.c` demonstrates every rewrite and fusion pattern and
guard, `tests/test_tier4_phase3.sh` pins the fused shapes through
`disassemble`, and `tests/peephole-corpus.lisp` + `tests/test_peephole_diff.sh`
run the same code with the pass forced off and on (`AMILISP_FORCE_SPEED=0`
vs `1`, `2`, `3`) and require identical output. The design rationale and
rewrite-soundness arguments live in the `src/core/peephole.c` header
comment and `specs/performance.md` §1.8 and §4.3.

### Disassembly

Two disassemblers, one per execution tier: `disassemble` for the bytecode the
compiler emits, and `%jit-disassemble` (or the `jitexpand` macro) for the
native m68k or AArch64 code the [JIT](#jit-m68k-and-aarch64) emits from it.

**Bytecode — `(disassemble fn)`** (host and Amiga). Takes a symbol, a function,
or a closure, and prints the lambda-list shape, local/upvalue/size counts, the
instruction listing, and the constant pool:

```lisp
CL-USER> (defun add1 (x) (+ x 1))
CL-USER> (disassemble 'add1)
Disassembly of ADD1:
  1 required, 0 optional, 0 key
  2 locals, 0 upvalues
  6 bytes, 1 constants

  0000: LOAD_CONST   0 0    ; 1
  0004: ADD
  0005: RET

Constants:
  0: 1
```

Jump and non-local-exit operands are resolved to their target offsets,
constant-pool references are annotated with the printed constant (the `; 1`
above), and `OP_CLOSURE` lists one line per captured variable. Built-in
functions have no bytecode, so they report `Built-in function: CAR` instead.
The listing goes to `*standard-output*` like any other output, so
`(with-output-to-string (*standard-output*) (disassemble 'add1))` captures it —
including over a SLY connection.

Because bytecode is what the optimizer rewrites, `disassemble` is also how you
check whether a `declaim`/`declare` actually took effect: compile the same
function at `speed 1` and `speed 3` and compare the listings (see [the peephole
post-pass](#the-peephole-post-pass-in-practice) for why a warm FASL cache can
make that comparison lie).

**Native code — `(jitexpand form)`** (AmigaOS on 68020+, and arm64 macOS/Linux
hosts). Prints one line of m68k or AArch64 assembly per instruction, with the
raw bytes alongside. The macro takes a
`defun`, a `lambda`, or any expression — an expression is wrapped in a thunk
that is never called, so free variables need not be bound:

```lisp
(jitexpand (defun add1 (x) (+ x 1)))   ; defines, then disassembles
(jitexpand (lambda (x) (car x)))
(jitexpand (+ x 1))
```

`amilisp::%jit-disassemble` is the underlying function if you already have the
function object. A function the JIT declined to translate prints `(no native
code — function runs through the bytecode interpreter)`, which makes this the
quickest way to find out whether the JIT took a given definition (see
[JIT](#jit-m68k-and-aarch64) for what it covers). This is a targeted disassembler,
not a general one: it decodes the instruction forms the JIT emits and
falls back to `.word` for anything else, so the raw word is still
visible. On builds without a JIT it compiles to a no-op.

For runnable examples of the bytecode `disassemble` builtin, see
`tests/test_disassemble_stream.c` and the "Disassemble" sections of
`tests/amiga/run-tests.lisp`; `%jit-disassemble` is exercised by
`tests/amiga/test-jit.lisp` (m68k) and `tests/test_jit_a64_walk.sh` (AArch64).

### JIT (m68k and AArch64)

AmiLisp has two JIT backends that translate bytecode functions to native machine code once they turn hot: **m68k** on the AmigaOS build (68020+) and **AArch64** on arm64 macOS and Linux hosts. Other builds — x86-64 hosts, Windows and MorphOS — run the bytecode VM.

On AmigaOS the m68k backend translates a function once it turns hot. The VM dispatcher jumps straight into the native body instead of interpreting bytecode. The translator (a single-pass bytecode walker) covers a broad core of the instruction set: integer arithmetic and comparisons (with fixnum fast paths), branches, `cons`/`car`/`cdr`/`rplaca`/`rplacd`/list building, struct slot access, function calls and tail calls (a chain of tail calls between functions runs in constant stack, as interpreted), closures, multiple-value flow, non-local exits (`block`/`return-from`, `catch`/`throw`, `unwind-protect`, `tagbody`/`go`, handlers/restarts), dynamic binding, `&optional`, `&rest` and `&key` parameters, and AmigaOS FFI (`amiga-call`). Opcodes it doesn't handle yet — and frames too large for a 16-bit displacement — fall back to the interpreter transparently. Native functions push an ordinary call frame, so backtraces and error messages list them like interpreted ones, at the line of the call they are in; their arguments are not always shown.

A function is compiled on its 8th call, or on its first when it contains a loop. Code compiled under `(optimize (speed 3))` is compiled at definition. Code that runs once, like most of what runs while a program loads, stays bytecode and costs no native-code memory. Functions restored from a heap image are compiled again as they turn hot. `(amilisp::%jit-set-hot-threshold n)` sets the call count (it returns the previous one), and `--jit-eager` (threshold 0) compiles every function at definition.

The JIT is on by default. Pass `--no-jit` to keep functions bytecode-only (useful for A/B benchmarks or isolating a bug). At runtime, `(amilisp::%jit-set-active nil|t)` toggles the JIT around individual `defun`s; a function defined while it is off stays bytecode. Builds without a backend — x86-64 hosts, Windows and MorphOS — compile the JIT out entirely and run the bytecode VM; its entry points become inline no-ops.

**arm64 macOS and Linux hosts** build a second backend, a template JIT for AArch64 ([specs/native-backend-a64.md](specs/native-backend-a64.md)), with the same compile-when-hot policy and switches. Fixnum arithmetic and comparisons, `car`/`cdr`, structure slots and special variables run as inline machine code, everything else through the interpreter's own helpers, so results are the interpreter's: a fixnum loop runs about 14× faster than bytecode, and a native function calls another native one directly, about 6× faster than bytecode ([docs/benchmarks.md](docs/benchmarks.md)). `amilisp::%jit-set-direct-calls nil` (or `AMILISP_JIT_DIRECT=0`) sends every call through the helper path instead. Every lambda list compiles, `&optional`, `&rest` and `&key` included. Native functions keep an ordinary call frame, so backtraces, error locations and `frame` show them like interpreted ones. `make host JIT=0` builds without it; `make test-jit-eager` runs the test suite with every function compiled. Its tests are `tests/test_jit_a64_walk.sh`, which also runs the behavioural checks of `tests/amiga/test-jit.lisp`.

To see the machine code for a definition — or to find out whether the JIT translated it at all — use `(jitexpand ...)`; see [Disassembly](#disassembly).

#### Performance

Measured on the high-end FS-UAE config (A4000 / 68040 / Picasso96). The A/B microbenchmarks in `trunk/bench-jit-loop.lisp` run identical function bodies with the JIT toggled via `%jit-set-active`, so only the dispatch path differs:

| Benchmark     | Shape                          | Bytecode |   JIT  | Speedup |
|---------------|--------------------------------|---------:|-------:|--------:|
| `sum-to`      | `tagbody`/`go` fixnum loop     |   400 ms |  20 ms |  20.0×  |
| `struct-loop` | 2× struct-slot read per iter   |   260 ms |  20 ms |  13.0×  |
| `arith-chain` | chained binary ops             |   300 ms |  40 ms |   7.5×  |

Compute-bound code sees the largest wins. A call from native code to another native function whose lambda list the call fits jumps straight into that function's code. Builtins and FFI stubs are dispatched directly from native code as well, so call-heavy generic code also runs ahead of the interpreter. `trunk/bench-jit-call.lisp` is the per-call A/B suite: on the FS-UAE 68040 config, a loop iteration that calls a native function takes about 1 us, against 13 us in the interpreter (an `&optional` callee 1.3 us against 17 us, an `&key` callee 3 us against 20 us). Only calls into functions the JIT declined stay near interpreter cost. `AMILISP_JIT_DIRECT=0` turns the direct calls off for A/B runs (see [docs/amilisp.md](docs/amilisp.md)). On the real-world `examples/amiga/gfx/bouncing-lines.lisp` demo (FFI-dominated — five lines drawn through `graphics.library` each frame), the JIT reaches **~615 FPS** versus **~500 FPS** on the bytecode VM — a smaller lead, since the frame time is mostly FFI calls rather than arithmetic. The remaining gap to compiled ACE BASIC (~1900 FPS through the same ROM graphics calls) is the structural cost of a dynamic, GC'd, tagged-value language — per-argument unboxing, dispatch and symbol lookup per call, GC safepoints — not codegen.

The Amiga test suite passes on the JIT config; per-opcode JIT coverage (counter-bump, value-correctness, and unwind-recovery assertions) lives in `tests/amiga/test-jit.lisp`.

Point-in-time benchmark results (sento actor throughput on host, Amiga JIT call loop) are logged with environment and reproduction commands in [docs/benchmarks.md](docs/benchmarks.md). Two general-purpose suites live in `trunk/`: `trunk/bench.lisp` compares JIT vs. bytecode across common Lisp constructs, and `trunk/bench-opt.lisp` tracks the optimization targets from [specs/performance.md](specs/performance.md) with deterministic, result-verified micro-benchmarks (`./build/host/amilisp --heap 64M --load trunk/bench-opt.lisp`). Two more are portable and run unchanged on amilisp, ECL and SBCL, so every row carries a native-compiler reference point: `trunk/bench-general.lisp` (general workloads: sorting, hashing, strings, reader/printer, CLOS, bignums, ...) and `trunk/bench-prims.lisp` (per-primitive costs); the usage lines are in each file's header and the measured tables in [docs/benchmarks.md](docs/benchmarks.md).

### Threads (MP)

The `MP` package provides kernel threads, per-thread dynamic bindings (TLV), locks, named condition variables, thread interruption/destruction and type predicates, with stop-the-world GC at safepoints — POSIX pthreads (with `__thread`-backed TLS) on the hosts, exec processes on AmigaOS and MorphOS.

Locks and condition variables are plain heap objects — no OS mutex per
lock, no table, no limit on how many are live at once (an actor system
with a deep backlog of in-flight asks is just allocation).  A contended
acquire parks the waiting thread on its own handle (a signal bit on
AmigaOS) and a release wakes exactly one waiter.  `(mp:acquire-lock lock
&optional (wait-p t) timeout)` takes the timeout in seconds and returns
`nil` when it elapses (`nil` timeout = wait forever, `0` = one attempt);
`(mp:with-lock-held (lock :timeout secs) ...)` skips its body and returns
`nil` on the same condition, and bordeaux-threads' `:timeout` on
`acquire-lock` / `with-lock-held` maps onto both.  Misuse signals instead
of hanging or corrupting: acquiring a
plain lock the thread already holds (use `mp:make-recursive-lock`),
releasing a lock the thread does not hold, and `condition-wait` without
holding the lock are all errors.  A recursive lock held at depth *n*
across `condition-wait` comes back at depth *n*.  See
`tests/test_mp_heap_locks.sh` and [docs/mp.md](docs/mp.md).

`mp:make-thread` accepts per-thread size keywords — `:stack-size` (C stack,
bytes), `:vm-stack-size` (operand-stack entries), `:vm-frames` (call-frame
budget), `:nlx-frames` (catch/unwind budget). Each is a *minimum*: values
below the platform default are raised to it, so a worker can only be grown.
This matters on AmigaOS, where the compact worker defaults (64 KB C stack,
256 call frames) are far below the main task's — a worker that runs deep
call chains, nested `catch`es, or `load`s from source should request larger
budgets, e.g. `(mp:make-thread #'game-loop :stack-size 200000 :vm-frames
1024)`. With the m68k JIT enabled, natively compiled code nests on the C
stack and books one call frame per level, as interpreted code does, so
budget both `:stack-size` and `:vm-frames` for the deepest call chain the
thread will run. The main task's own
catch/unwind budget is also platform-sized: 68k AmigaOS reserves 512
nested `catch`/`block`/`handler-case` frames and 128 nested
`unwind-protect`s (MorphOS and the host: 2048 / 256); going past either
signals a catchable "stack overflow" error rather than crashing. On
AmigaOS a worker also inherits the creator's console, so
`*standard-output*` reaches the shell window (or a worker can open its own
`CON:` window via `open`). See the size-keyword tests in
`tests/test_threads.c` / `tests/amiga/run-tests.lisp` for usage.

Atomic operations: `mp:compare-and-swap` (alias `mp:cas`) on `car`/`cdr`,
`svref`, `symbol-value` / special variables, `slot-value` and defstruct
accessors — returns the value the place held, `eq` to the old value exactly
when it swapped — plus `mp:atomic-incf` / `mp:atomic-decf` for fixnum
counters. A native compare-exchange on the host, a `Forbid()`/`Permit()`
window on the single-core Amiga targets. Library backends map onto these
directly (the `atomics` fork does). See `tests/test_atomics.c` /
`tests/amiga/run-tests.lisp` and [docs/mp.md](docs/mp.md).

Two built-in hang-triage diagnostics: `(mp:dump-thread-waits)` prints every
live thread's current wait state (which lock/condvar it is blocked on), and
setting `AMILISP_LOCK_DIAG=<ms>` in the environment makes any blocking
`mp:acquire-lock` that waits past the threshold report the contended lock by
name, the current holder thread and what *it* is blocked on, and the total
wait once the lock is finally acquired (`AMILISP_LOCK_DIAG=1` selects the
1000 ms default). See `tests/test_lock_diag.sh` for the exact output format.

`mp:*thread-death-hooks*` is a list of functions called with the thread and
the error's message when an unhandled error ends a thread -- an exhausted
heap included, which no handler in the thread's own code sees. See
[docs/mp.md](docs/mp.md) and `tests/test_mt_thread_death_hook.sh`.

### Networking (TCP, TLS, UDP)

**TCP networking** — BSD sockets (POSIX) and bsdsocket.library (AmigaOS). On the
POSIX host the socket table grows on demand, so a server can hold thousands of
simultaneous connections (readiness waits use `poll`, which has no `FD_SETSIZE`
ceiling); on AmigaOS the table is a fixed 64 slots, bounded by bsdsocket.library's
per-task descriptor table. Socket streams support per-connection read/write timeouts:
`(setf (ext:socket-stream-timeout stream :input) seconds)` (also `:output`) arms a
`poll`/`WaitSelect` deadline so a read/write that stalls past the timeout signals
`ext:socket-timeout` (a subtype of `stream-error`) instead of blocking forever; the
value is in seconds (fractional allowed), `nil` clears it, and reading the place back
returns the current setting. See `tests/test_stream.c`
(`platform_socket_table_grows_many_connections`, `socket_read_timeout_*`,
`eval_socket_stream_timeout_*`) and `tests/amiga/run-tests.lisp` for usage.

On AmigaOS/MorphOS all socket I/O runs through a dedicated reactor process;
setting `AMILISP_SOCK_DIAG=1` in the environment (`SetEnv AMILISP_SOCK_DIAG 1`)
traces every request through the client↔reactor handshake on stderr — posted,
received, parked, resumed, replied, reply received, plus DNS lookups — so a
hanging socket operation's last trace line names the handoff that was lost.

**TLS** — `(ext:socket-start-tls stream ...)` upgrades a connected TCP socket
stream to TLS **in place** (client or server, with SNI, certificate and
hostname verification, and peer-certificate introspection). The provider is
loaded at runtime and optional — OpenSSL 1.1.1/3.x on the host, AmiSSL v5 on
AmigaOS — with `(ext:tls-available-p)` as the capability gate. drakma and
Hunchentoot get HTTPS through the bundled cl+ssl facade
(`lib/shims/cl+ssl`, auto-registered on `asdf:*central-registry*` when
ASDF loads, shadowing any Quicklisp/ocicl-installed cl+ssl; opt out with
`AMILISP_NO_SHIMS=1` — Amiga: `SetEnv AMILISP_NO_SHIMS 1` — e.g. to run
the real cl+ssl on the host, where its CFFI stack works). See
[docs/ext.md](docs/ext.md#tls) and the runnable examples in
`tests/tls-loopback.lisp` / `tests/amiga/tls-tests.lisp` /
`trunk/load-and-test-hunchentoot-ssl.lisp`.

**UDP networking** — connected datagram sockets:
`(ext:open-udp-stream host port)` returns a UDP socket stream;
`(ext:udp-stream-send stream buffer &optional length)` sends one datagram,
`(ext:udp-stream-receive stream buffer &optional max-length)` blocks for one
(honoring the same `ext:socket-stream-timeout` places), and
`(ext:socket-stream-local-endpoint stream)` returns the local dotted-quad
address and port (getsockname — TCP streams too). The usocket fork maps
`:datagram` sockets onto these, which is what KNXnet/IP tunneling (knx-conn)
uses. See `tests/test_stream.c` (`eval_udp_stream_*`) and
`tests/amiga/run-tests.lisp` for usage.

### Host FFI (dlopen + libffi + CFFI)

The `FFI` package provides foreign pointers and typed peek/poke on **all**
platforms (on AmigaOS the `AMIGA` package adds register-based library calls — see
[Raw FFI Access](#raw-ffi-access)). On the POSIX dev host the `FFI` package
additionally provides a real, general-purpose foreign-function engine — dynamic
library loading (`ffi:load-library`/`ffi:symbol-pointer` via `dlopen`/`dlsym`),
arbitrary C calls with full argument/return marshaling (`ffi:call-foreign`,
libffi-backed, incl. variadics), Lisp-as-C callbacks (`ffi:make-callback`, libffi
closures), and typed memory access
(`ffi:peek-i8/i16/i32/u64/i64/single/double/pointer` and the matching `poke-*`).
`ffi:make-callback` exists on AmigaOS/MorphOS as well — there it builds a
68k entry point, and its optional `regs` list names the register each
argument arrives in (`'(:a0 :a2 :a1)` is the `struct Hook` convention),
see [MUI](#mui-amigaos-3x-with-mui-38-morphos).  On every platform a
callback is a *boundary*: Lisp runs on the foreign caller's stack, so an
unhandled error inside it — or a `throw` / `return-from` to a target
outside it — does not unwind through the C frames; the callback returns
0 / NULL and the condition is re-signaled once the foreign call that
invoked it returns, where a `handler-case` around `ffi:call-foreign` (or
the library call) catches it.  `ext:*callback-error-policy*` (`:defer`,
the default) can be set to `:debug` on the host to enter the debugger
inside the callback instead.

```lisp
;; Resolve and call libc directly
(ffi:call-foreign (ffi:symbol-pointer "pow") :double '(:double :double) '(2d0 10d0))
;; => 1024.0d0
```

On top of this engine AmiLisp ships a **CFFI** backend (`cffi-amilisp.lisp`,
in the CFFI source tree), so the standard CFFI API — `defcfun`,
`foreign-funcall`, `mem-ref`, `defcallback`, `defcstruct`, foreign strings —
works on the host. This is what lets CFFI-dependent Quicklisp systems load.
Calls into arbitrary C functions (`ffi:call-foreign`, `dlopen`) are host-only;
on AmigaOS and MorphOS, library calls go through the library-vector model
(`AMIGA.FFI`, see [Raw FFI Access](#raw-ffi-access)) instead. See `tests/test_ffi.c` and
`trunk/load-and-test-cffi.lisp` for runnable end-to-end examples.

### Garbage collection

On macOS/Linux hosts AmiLisp runs a **generational collector**: all
allocation is a lock-free bump in a nursery region, and most collections are
cheap minor cycles that only trace the young objects (old→young references
are tracked by hardware page protection, so there is no write-barrier cost
in compiled code).  Full compacting collections still run when old space
fills.  The generational machinery needs an MMU and is compiled out on
AmigaOS and MorphOS: those binaries run the Immix-style collector
described below.

- `AMILISP_GENGC=0` (environment) selects the classic collector on the host
  (useful for A/B measurements; behavior is identical, only pause costs
  differ).
- `AMILISP_HDR_INDEX=0` (environment) drops the block-start index the classic
  collector keeps for the m68k JIT's native-stack scan; collections taken
  inside JIT'd code then pay a full arena walk in the mark phase (an A/B
  switch, behavior is identical).  `(ext:%gc-audit-hdr-index)` checks the
  index against the heap (0 = clean).
- `(ext:gc)` forces a full collection, `(ext:%gc-minor)` a minor cycle.
- A compaction needs a small off-heap block for the duration of the
  collection — under 5% of the heap in use (some 390 KB for an 8 MB heap;
  `(ext:%gc-fwd-bytes)` tells the exact figure).  When even that cannot be
  allocated the collector says so once and keeps sweeping without moving
  anything.
- `(ext:%gc-time-stats)` and `(ext:%gengc-stats)` expose collector telemetry
  (per-phase times, minor counts, promoted bytes, dirty-page counts);
  `(ext:%heap-stats)` the allocator's side (bump front, live bytes,
  free-list probe steps and free-list shape).  `trunk/gc-phase0.lisp`
  reads all three to profile a collection cycle on a given machine
  (`verify/realamiga/run-gc-phase0.sh` runs it in FS-UAE).
- `(room)` ends with the collector's own lines: the free list's shape and
  probe count for the classic collector, the line map, holes claimed,
  cycles and policy compactions for Immix.

See `tests/test_gengc.c` and `tests/test_gengc_watch.c` for the behavioral
contract, and `specs/generational-gc.md` for the design.

The collector is chosen at build time, one per binary.  The m68k AmigaOS
and the MorphOS binaries run an **Immix-style mark-region collector**: the heap is viewed
as 128-byte lines, a collection marks the live objects and the lines they
touch and then sweeps only the line map, and allocation is always a bump
into the next run of free lines.  Nothing in a cycle is proportional to
the amount of garbage, which is what the classic sweep and its free-list
probing cost on a 68020: the same allocation-heavy program runs 3–4×
faster on a 68040 and a 68020 in FS-UAE, 3× on a Vampire and 3–5× on a
MorphOS machine, than with the classic collector, with the collection
pauses unchanged or shorter (`docs/gc-phase0-results.md`).  It needs no write barrier, so it fits a
machine without an MMU; the sliding compactor is still there as the rare
defragmentation, and `(ext:gc)`, `(ext:gc-compact)`, heap images and the
telemetry above work unchanged.  `make -f Makefile.cross amiga GC=classic`
builds the classic collector instead; on the host, `make host GC=immix`
builds a second binary (`build/host-immix/amilisp`) that is the
development and A/B vehicle (`make test GC=immix`, `make test-gc-stress
GC=immix`, `make test-memleak GC=immix` run the gates against it), and
`make -f Makefile.mos GC=classic` is the classic MorphOS build
(`build/morphos-classic/`).  See `tests/test_gc_immix.c` for the behavioral
contract and `specs/gc-backend-immix.md` for the design.

### Memory outside the heap

Not everything AmiLisp allocates lives in the GC arena, so not everything
shows up in `ROOM`.  Each compiled function owns a bytecode body, a constants
pool, `&key` arrays, a line map and (on m68k) its JIT code; the compiler keeps
eight working blocks of about 80 KB for the whole run, whose bytecode and
constants buffers grow only as far as the functions being compiled need; the
struct, condition and type registries keep hash indexes.  All of that is
ordinary system memory, and AmiLisp returns it — during a run when the collector proves a function dead, and at
exit for everything still live.

This matters more on AmigaOS than anywhere else: **AmigaOS does not reclaim a
program's memory when it exits**, so anything a program fails to hand back is
gone from the system pool until the machine is rebooted.

Two diagnostics report on it:

- `AMILISP_MEM_DIAG=1` (environment) prints, at exit, how much off-heap memory
  each shutdown stage handed back and — on AmigaOS — how much free system
  memory changed across the whole run.  Compare it with `Avail` before and
  after.  A small, constant residue is the shell's own bookkeeping; a figure
  that grows with what the program did, or grows run over run, is a leak.
- `(ext:%bytecode-offheap-stats)` returns `(bytes objects)` — how much
  compiled-code memory has been returned to the system since startup.  It must
  grow after a GC that follows redefining functions, `compile`, or reloading a
  file.
- `(ext:%compiler-pool-stats)` returns `(parked-blocks block-bytes
  buffer-bytes largest-parked-buffer largest-code-bytes largest-constants)` —
  what the compiler holds between compiles, and how far the largest compile so
  far had to grow its buffers.  A single function is limited to 262144 bytes
  of bytecode and 8192 distinct constants; past that the compiler signals an
  error.  See `tests/test_compiler_buffers.sh`.
- `(ext:%fasl-registry-stats)` returns `(readers writers)` — the FASL loads
  and cache writes in flight on the calling thread.  Outside of `load` /
  `compile-file` both are 0; both are back to 0 as well after a FASL load
  (or a source `load`'s auto-cache write) was left by `handler-case`,
  `return-from` or `throw`.  See `tests/test_fasl_reader_unwind.sh`.

`make test-memleak` builds with `-DDEBUG_MEM_TRACK`, which tags every off-heap
allocation with the source line that made it and asserts a run ends with zero
bytes outstanding — naming the file and line if not.  See
`tests/test_memleak_tracked.sh` and `tests/test_shutdown_leak.sh`.

### Exact float printing and reading

Float literals and printed floats convert between decimal text and IEEE
bits using AmiLisp's own integer-only conversion (`src/core/float_dtoa.c`)
— never the platform's `printf`/`strtod`.  The printer emits the shortest
digit string that reads back to exactly the same float (Steele-White),
and the reader rounds every literal correctly to its target format,
singles included.  Results are bit-identical on every platform and immune
to FPU quality — FPGA-accelerated Amigas (Vampire/Apollo) divide with only
~42 mantissa bits, which used to make `strtod` misparse literals by
several ulps.  `(prin1 pi)` → `3.141592653589793d0` reads back to the
same 64 bits everywhere, so FASLs and source files with float constants
compile identically on host and Amiga.

See `tests/test_float_dtoa.c` for the contract (shortest-form vectors,
correctly rounded halfway/tie cases, fuzz round-trips against libc) and
the "Exact FPU-independent float reading/printing" block in
`tests/amiga/run-tests.lisp` for the bit-exact checks that run on real
hardware.

### Packed byte vectors

`(make-array n :element-type '(unsigned-byte 8))` — and any element type that
upgrades to `(unsigned-byte 8)`, `(signed-byte 8)`, `(unsigned-byte 16)` or
`(signed-byte 16)`, like `(mod 256)`, `(integer -5 5)` or `(integer 0 1000)`
— builds a **packed byte vector**: 1 byte (8-bit kinds) or 2 bytes (16-bit
kinds) per element instead of a 4-byte tagged value, and the GC never scans
its contents.  Integer ranges upgrade to the narrowest kind that holds them.
On an 8MB Amiga that makes I/O buffers, graphics plane data and 16-bit audio
samples or coordinate tables 4× (or 2×) smaller and essentially free to
collect.  `aref`/`elt`, fill pointers with
`vector-push`/`vector-pop`, `adjust-array`, the common sequence functions
(`fill`, `subseq`, `copy-seq`, `sort`, `remove`/`delete`, `replace`, `map`,
`coerce`, …),
`equalp` (including `:test 'equalp` hash-table keys), `typep`/`type-of`,
and FASL literals all work; out-of-range stores signal a catchable
`type-error`.  `typep` and `subtypep` compare array element types by their
*upgraded* class and expand user `deftype` aliases, so idioms like
flexi-streams' `(deftype octet () '(unsigned-byte 8))` followed by
`(the (array octet *) v)` behave as on other Lisps.  Arrays made with
`:adjustable t` keep the growable general-vector representation (so
`vector-push-extend`/`adjust-array` grow them in place, as e.g. drakma's
HTTP buffers require) while still reporting the byte element type — only
non-adjustable byte arrays are packed.  One caveat: `:displaced-to` a packed
byte vector produces a *copy* of the requested window rather than a live
view (the packed bytes cannot back one), so later writes to the target are
not visible through the displaced array.

See `tests/test_byte_vector.c` and the byte-vector section of
`tests/amiga/run-tests.lisp` for the full behavioral contract.

### Bulk sequence I/O and RLE decoding

`read-sequence` and `write-sequence` are C builtins: an `(unsigned-byte 8)`
vector against a binary file stream moves whole chunks per platform call
instead of one VM round-trip per byte — on a 14MHz 68020 that turns loading
a 20KB asset file from seconds into file-I/O speed.  Every other
sequence/stream combination (strings, lists, string streams, Gray streams)
keeps its standard element-wise semantics.  `replace` between byte vectors
of the same element type is a single `memmove`, `map-into` folding byte
vectors through `#'logior`/`#'logand`/`#'logxor` runs as a C loop — the
idiomatic way to OR bitplanes into a mask — and `count` of a fixnum under
the default `eql` test runs as a C loop over the packed elements.

`(ext:unpack-byterun1 src pos end dst dst-len &optional dst-start)` decodes
ByteRun1/PackBits RLE data — the compression used by IFF ILBM `BODY`
chunks (and TIFF/MacPaint) — from the byte vector `src` into `dst` at C
speed, returning the new source position and signalling a clear error on
truncated or overlong runs.

`(ext:copy-rows dst src count chunk dst-start dst-stride src-start
src-stride)` copies `count` rows of `chunk` bytes each with independent
strides on both sides — the gather/scatter step for interleaved binary
formats, e.g. pulling one bitplane's rows out of an ILBM `BODY` decoded in
a single piece (see `docs/ext.md`).

See the READ-SEQUENCE/WRITE-SEQUENCE tests in `tests/test_stream.c`, the
fast-path tests in `tests/test_byte_vector.c`, and the corresponding
sections of `tests/amiga/run-tests.lisp` for usage examples.

### CPU store self-test

Some FPGA CPU cores lose memory stores: the Apollo 68080 of the Vampire V4
with core 10760 drops the last of a short run of stores after a
memory-to-memory move from an absolute address — a sequence gcc emits for
ordinary function prologues — so a C local can read back as whatever its
stack slot held before, and any program fails at random ("not a function",
"too few arguments", wrong values), differently on every launch.  Nothing in
software avoids it: the data cache, superscalar mode and compiler flags make
no difference.  Core 11350 no longer shows it; updating the core is the fix.

So the m68k build checks at startup.  It replays that store sequence a few
thousand times (milliseconds) and, if a store is lost, prints a warning
naming the defect and puts `:cpu-lost-stores` on `*features*` so scripts
and test suites can see it.  A sound CPU prints nothing.  Whether the core
loses the store depends on where the program's data landed in memory, which
changes from launch to launch, so on an affected machine the warning shows
on some launches and not on others — a quiet launch is not a clean bill of
health.
`(ext:%cpu-store-selftest &optional rounds)` repeats the test and returns
the number of lost stores; `AMILISP_CPU_CHECK=0` in the environment skips
the startup check, and `AMILISP_CPU_CHECK=<n>` (a positive number) makes any
CPU report `n` lost stores — a marked, simulated warning, for trying
`#+cpu-lost-stores` code on a sound machine.  A session started from a heap
image gets the verdict of the machine it runs on, not of the one that saved
the image.  `docs/vampire-68080-lost-stores.md` has the defect's record:
the instruction sequence, what decides it, the machines checked and what
to report to the Apollo team.
See `tests/test_cpu_selftest.sh` and the `cpu-store-selftest` block in
`tests/amiga/run-tests.lisp`.

## AmigaOS and MorphOS APIs

AmiLisp talks to the OS through Lisp modules loaded on demand via `require`, at no cost to the binary's size. Two layers: **generated raw bindings** for every library, device and BOOPSI class of the AmigaOS 3.2 API (and MUI, AHI and the MorphOS additions), one Lisp function per C function — see [Raw OS bindings](#raw-os-bindings-generated) — and **hand-written modules** with a Lisp-shaped API on top: Intuition, Graphics and GadTools, ReAction and MUI GUIs, ARexx, audio.device and AHI, IFF and asynchronous DOS I/O. A generic FFI layer (`FFI` package) provides foreign memory access on all platforms; the `AMIGA` package adds register-based library call dispatch via a 68k assembly trampoline. The same modules load on MorphOS.

### Available Amiga Modules

| Module | Package | Description |
|--------|---------|-------------|
| `(require "ffi")` | `FFI` | Foreign pointers, typed peek/poke, defcstruct, callbacks (all platforms); dlopen/libffi calls (host) |
| `(require "amiga/ffi")` | `AMIGA.FFI` | Tag lists, defcfun, with-library, open-library-or-die, library-version, `make-hook` / `make-dispatcher` — `struct Hook`s and BOOPSI dispatchers that call Lisp (AmigaOS) |
| `(require "amiga/raw/<lib>")` | `AMIGA.RAW.<LIB>` | Generated 1:1 bindings for every OS library (`exec`, `dos`, `intuition`, `graphics`, `utility`, `asl`, `locale`, `iffparse`, `datatypes`, `rexxsyslib`, …), the ReAction classes (`gadgets/button`, `gadgets/layout`, `images/bevel`, `classes/window`, …), device/resource tables (`timer`, `cia`, …), header-only constant/struct modules (`devices/audio`, `hardware/custom`, `reaction/reaction`, …) and the MUI custom-class headers (`mui/tron`, …) — see above |
| `(require "amiga/exec")` | `AMIGA.EXEC` | AvailMem/MEMF_* memory introspection, chip-RAM upload helper, the exec device-I/O calls (`open-device`, `create-io-request`, `send-io` / `check-io` / `wait-io` / `abort-io`, `do-io`) |
| `(require "amiga/intuition")` | `AMIGA.INTUITION` | Windows, screens, IDCMP events, public screens, pointer sprites |
| `(require "amiga/graphics")` | `AMIGA.GFX` | Drawing, text, fonts, offscreen bitmaps and blits, planar upload |
| `(require "amiga/gadtools")` | `AMIGA.GADTOOLS` | Gadgets, menus, bevel boxes, VisualInfo |
| `(require "amiga/boopsi")` | `AMIGA.BOOPSI` | Toolkit-neutral BOOPSI helpers for any object — ReAction, MUI or intuition's built-in classes: `with-foreign-pool` / `pool-string` / `pool-hook`, `with-tags`, `do-method`, `object-class`, `get-attr` / `set-attrs`, exec label lists |
| `(require "amiga/reaction")` | `AMIGA.REACTION` | ReAction helpers over the raw class modules: `new-object`, `set-gadget-attrs`, `open-window` / `do-window-events`, `open-requester`; re-exports `AMIGA.BOOPSI` (AmigaOS 3.5+/3.2, MorphOS) |
| `(require "amiga/mui")` | `AMIGA.MUI` | MUI helpers over `muimaster.library`: `new-object` by class name, `make-object`, `notify`, `do-application-events`, `request`, custom classes with Lisp dispatchers (`create-custom-class`, `do-super-method`, the `_mleft(obj)` shortcuts); re-exports `AMIGA.BOOPSI` (AmigaOS 3.x with MUI 3.8+, MorphOS) |
| `(require "amiga/arexx")` | `AMIGA.AREXX` | amilisp's own ARexx port (`start` / `stop`, see [ARexx port](#arexx-port-amigaos--morphos)) and `send` to drive other applications' ports |
| `(require "amiga/asyncio")` | `AMIGA.ASYNCIO` | Double-buffered file I/O with DOS packets (`with-async-file`, `read-async` / `write-async`), see [Async file I/O](#async-file-io-dos-packets) |
| `(require "amiga/iff")` | `AMIGA.IFF` | Reading and writing IFF files through iffparse.library (`with-iff`, `map-chunks`, `sift`), see [IFF files](#iff-files-iffparselibrary) |
| `(require "amiga/audio")` | `AMIGA.AUDIO` | audio.device channel allocation, non-blocking 8-bit sample playback from chip RAM |
| `(require "amiga/ahi")` | `AMIGA.AHI` | **Opt-in** AHI (ahi.device) playback: 8/16-bit mono/stereo samples at any rate from any memory on the user's AHI unit (`play-sample`, gapless double-buffered `queue-sample`), the audio mode database, and AHI's mixer through the hook-free low-level API (`alloc-audio`, `load-sound`, `play`, `set-volume`, …); AmigaOS with AHI installed, or MorphOS |

The GUI modules are exercised end-to-end by `tests/amiga/test-gui.lisp`
(run by the Amiga test suite) — use it as the reference for working
examples of every export.  `AMIGA.AUDIO` is exercised the same way by
`tests/amiga/test-audio.lisp`: open a channel with `open-audio` (or
`with-audio`), upload a signed 8-bit sample with
`amiga.exec:alloc-chip-bytes`, start it with `play-sample`
(`period-for-rate` converts a Hz sample rate to a Paula period), poll
with `playing-p`, cut it off with `stop-sample`.  Playback never
blocks: requests go out via `SendIO` and are reclaimed with
`CheckIO`/`AbortIO`.

`AMIGA.AHI` is the same shape over AHI, and strictly opt-in: nothing
loads or probes ahi.device unless a program `(require "amiga/ahi")`s
it, and `AMIGA.AUDIO` stays the Paula module — no detection, no
fallback between the two (both can be loaded; on a Paula machine
AHI's own driver takes its channels through audio.device, so used at
the same time they contend for the four).  `open-ahi` opens the unit
the user configured in the AHI preferences (unit 0 by default), then
`play-sample` takes a foreign buffer of 8- or 16-bit, mono or stereo
samples at any rate — `sample-bytes` packs signed sample values,
`make-sample-buffer` uploads them, no chip RAM involved — and
`queue-sample` links a second request behind the first for gapless
streaming from two buffers.  The low-level tier (`alloc-audio`,
`load-sound`, `start-playback`, `play`, `set-volume` / `set-frequency` /
`set-sound`) is AHI's mixer driven from the program on a mode picked
with `best-audio-mode` or listed by `audio-modes` / `audio-mode-info`;
it never installs an AHI hook (those run in interrupt context).
`tests/amiga/test-ahi.lisp` is the executable specification — it skips
its device checks where AHI is absent — and
`examples/amiga/audio/ahi-play.lisp` a program using both tiers.

### Opening a Window

```lisp
(require "amiga/intuition")
(require "amiga/graphics")

(amiga.intuition:with-window (win :title "Hello Amiga"
                                   :width 320 :height 200
                                   :idcmp (logior amiga.intuition:+idcmp-closewindow+
                                                  amiga.intuition:+idcmp-vanillakey+))
  (let ((rp (amiga.intuition:window-rastport win)))
    (amiga.gfx:set-a-pen rp 1)
    (amiga.gfx:move-to rp 20 40)
    (amiga.gfx:gfx-text rp "Hello from AmiLisp!")
    (amiga.gfx:draw-line rp 20 50 300 50)
    ;; Wait for close gadget
    (amiga.intuition:event-loop win
      (#.amiga.intuition:+idcmp-closewindow+ (msg) (return)))))
```

### Opening a Custom Screen

Applications that want the whole display (games, demos) open their own
screen instead of a window on Workbench.  Pick the display mode
RTG-safely with `best-mode-id` (graphics.library `BestModeIDA` — on
Picasso96/CyberGraphX/MorphOS it returns a suitable RTG mode, on a
chipset Amiga a native one), then cover the screen with a borderless
backdrop window for input and menus:

```lisp
(require "amiga/intuition")
(require "amiga/graphics")

(amiga.intuition:with-screen
    (scr :width 640 :height 256 :depth 2 :title "My Screen"
         :mode-id (amiga.gfx:best-mode-id :width 640 :height 256 :depth 2))
  ;; screen palette: entry 0 black, entry 1 white
  (let ((vp (amiga.intuition:screen-viewport scr)))
    (amiga.gfx:set-rgb4 vp 0 0 0 0)
    (amiga.gfx:set-rgb4 vp 1 15 15 15))
  (amiga.intuition:with-window
      (win :left 0 :top 0
           :width (amiga.intuition:screen-width scr)
           :height (amiga.intuition:screen-height scr)
           :screen scr
           :flags (logior amiga.intuition:+wflg-borderless+
                          amiga.intuition:+wflg-backdrop+
                          amiga.intuition:+wflg-activate+)
           :idcmp amiga.intuition:+idcmp-vanillakey+)
    (amiga.intuition:event-loop win
      (#.amiga.intuition:+idcmp-vanillakey+ (msg) (return)))))
```

The Lambda's Tale engine (its own repo, run with `:display :screen`)
exercises this path end-to-end; its Amiga test suite covers it.

### Offscreen Bitmaps and Blits

RTG-safe sprite/tile rendering: allocate bitmaps through the OS (pass
the window's or screen's bitmap as `:friend` so Picasso96 / CyberGraphX
/ MorphOS put them in the display's native format), fill them with
chunky pen bytes, and composite with the blitter — no planar layout or
chip-ram assumptions anywhere:

```lisp
(require "amiga/graphics")

(amiga.gfx:with-bitmap (bm 32 32 2)          ; AllocBitMap/FreeBitMap
  (amiga.gfx:with-bitmap-rastport (brp bm)   ; scratch RastPort on it
    ;; row-major pen indices; WriteChunkyPixels on V40+,
    ;; per-pixel fallback on V39
    (amiga.gfx:write-chunky brp 0 0 4 2 #(0 1 2 3 3 2 1 0))
    (amiga.gfx:read-pixel brp 1 0))          ; => 1
  ;; blit into any window/screen rastport
  (amiga.gfx:blt-bitmap-rastport bm 0 0 window-rp 10 20 32 32))
```

`get-bitmap-attr` (`+bma-width+`/`+bma-height+`/`+bma-depth+`) inspects
what was really allocated.  See `tests/amiga/test-gui.lisp` for
runnable examples; the Lambda's Tale engine's blitted wall graphics
(its own repo, M3) are the end-to-end user.

### GadTools Gadgets

```lisp
(require "amiga/gadtools")

(amiga.intuition:with-pub-screen (scr)
  (amiga.gadtools:with-visual-info (vi scr)
    (amiga.gadtools:with-gadgets (glist ctx vi)
      (amiga.gadtools:create-gadget
        amiga.gadtools:+button-kind+ ctx vi
        :left 20 :top 30 :width 120 :height 16
        :text "Click Me" :gadget-id 1)
      (amiga.intuition:with-window (win :title "GadTools Demo"
                                         :width 320 :height 100
                                         :idcmp (logior amiga.intuition:+idcmp-closewindow+
                                                        amiga.gadtools:+buttonidcmp+))
        (amiga.intuition:add-gadget-list win
          (ffi:make-foreign-pointer (ffi:peek-u32 glist)))
        (amiga.gadtools:gt-refresh-window win)
        (amiga.intuition:event-loop win
          (#.amiga.intuition:+idcmp-closewindow+ (msg) (return))
          (#.amiga.intuition:+idcmp-gadgetup+ (msg)
            (format t "Button clicked!~%")))))))
```

### ReAction (AmigaOS 3.5+/3.2, MorphOS)

The ReAction classes — `window.class`, `gadgets/layout.gadget`,
`gadgets/button.gadget`, `listbrowser`, `chooser`, `requester.class` … —
are BOOPSI class libraries driven through `NewObjectA` / `SetAttrsA` /
`GetAttr` and object methods.  Their tags, method IDs and functions come
from the generated raw modules (`amiga/raw/classes/window`,
`amiga/raw/gadgets/button`, …, see below); what a C program gets from
amiga.lib / reaction.lib on top — `DoMethod()`, the `RA_OpenWindow` /
`RA_HandleInput` macros, `NewList()`, string literals that outlive the
objects using them — is `(require "amiga/reaction")`, package
`AMIGA.REACTION`:

```lisp
(require "amiga/reaction")
(require "amiga/raw/intuition")
(require "amiga/raw/classes/window")
(require "amiga/raw/gadgets/layout")
(require "amiga/raw/gadgets/button")

(defpackage "HELLO-REACTION"
  (:use "CL")
  (:local-nicknames ("RA" "AMIGA.REACTION") ("INTUI" "AMIGA.RAW.INTUITION")
                    ("WIN" "AMIGA.RAW.CLASSES.WINDOW")
                    ("LAYOUT" "AMIGA.RAW.GADGETS.LAYOUT")
                    ("BUTTON" "AMIGA.RAW.GADGETS.BUTTON")))
(in-package "HELLO-REACTION")

(ra:with-foreign-pool ()                      ; strings live as long as the objects
  (let ((win (ra:new-object (win:window-get-class)
               intui:+wa-title+ "Hello ReAction"
               intui:+wa-close-gadget+ t
               intui:+wa-drag-bar+ t
               win:+window-parent-group+
               (ra:new-object (layout:layout-get-class)        ; VGroupObject
                 layout:+layout-orientation+ layout:+layout-vertical+
                 layout:+layout-add-child+
                 (ra:new-object (button:button-get-class)      ; ButtonObject
                   intui:+ga-id+ 1
                   intui:+ga-rel-verify+ t
                   intui:+ga-text+ "_Quit")))))
    (unwind-protect
         (progn
           (ra:open-window win)                                ; RA_OpenWindow
           (ra:do-window-events ((result code) win)            ; Wait + RA_HandleInput
             (let ((class (logand result win:+wmhi-classmask+)))
               (when (or (= class win:+wmhi-closewindow+)
                         (= class win:+wmhi-gadgetup+))       ; gadget id: (logand result win:+wmhi-gadgetmask+)
                 (return)))))
      (ra:dispose-object win))))
```

`new-object` takes tag/value pairs with integers, foreign pointers,
`T`/`NIL` and strings; `get-attr`, `set-attrs`, `set-gadget-attrs`,
`do-method` (any method — `CallHookPkt` on the object's class
dispatcher, which is what amiga.lib's `DoMethodA` is), `open-requester`
(requester.class), `iconify`, `new-list` / `free-list-nodes` for the
label lists of chooser / clicktab / listbrowser, and `with-tags` for the
class functions that take a tag list themselves.  Setting
`amiga.reaction:*event-loop-timeout*` makes `do-window-events` return
after that many seconds — how the examples run unattended.
`available-p` tells whether the classes can be opened (OS 3.5+/3.2,
MorphOS); the module itself loads everywhere.

The toolkit-neutral half — the foreign pool, `with-tags`, `do-method`,
`object-class`, `get-attr` / `get-attr-pointer` / `set-attrs`, the exec
label lists — is its own module, `(require "amiga/boopsi")`, package
`AMIGA.BOOPSI`, which `AMIGA.REACTION` re-exports (same symbols).  It
works on any BOOPSI object, including intuition's built-in classes on a
bare OS 3.1 with no toolkit installed; `tests/amiga/test-boopsi.lisp`
drives a `propgclass` object through it, and `docs/amiga.md` has the
table.

[`examples/amiga/reaction/`](examples/amiga/reaction/) ports the NDK 3.2
ReAction examples — `buttons`, `checkbox`, `chooser`, `clicktab`,
`fuelgauge`, `integer`, `listbrowser`, `requester` — and is the
reference for the classes' use; `tests/amiga/test-reaction.lisp` /
`tests/test_amiga_reaction.sh` are the module's executable
specification, and `verify/realamiga/run-examples.sh` runs and
photographs every example in FS-UAE.

### MUI (AmigaOS 3.x with MUI 3.8+, MorphOS)

MUI — the Magic User Interface, 3.8 on classic AmigaOS and built into
MorphOS — is a BOOPSI toolkit whose classes are created *by name*
through `muimaster.library` (`MUI_NewObjectA("Window.mui", tags)`) and
driven with `SetAttrs` / `GetAttr` / `DoMethod` like any BOOPSI object.
Every function of `muimaster.library` and every `MUIA_` / `MUIM_` /
`MUIV_` / `MUII_` / `MUIC_` constant of `libraries/mui.h` is in the
generated raw module `amiga/raw/muimaster` (the `MUIC_*` class names
and the `MUIX_*` text-style escapes are string constants); what a C
program gets from the `mui.h` shortcut macros and amiga.lib on top —
`ApplicationObject` / `WindowObject` / `SimpleButton`, `DoMethod`,
the `MUIM_Notify` idiom, the `MUIM_Application_NewInput` loop,
`MUI_Request` — is `(require "amiga/mui")`, package `AMIGA.MUI`:

```lisp
(require "amiga/mui")
(require "amiga/raw/muimaster")

(defpackage "HELLO-MUI"
  (:use "CL")
  (:local-nicknames ("MUI" "AMIGA.MUI") ("M" "AMIGA.RAW.MUIMASTER")))
(in-package "HELLO-MUI")

(mui:with-foreign-pool ()                          ; strings live as long as the objects
  (let* ((ok  (mui:make-object :button "_OK"))     ; SimpleButton("_OK")
         (win (mui:new-object :window               ; WindowObject, ... End
                m:+muia-window-title+ "Hello from Lisp"
                m:+muia-window-id+    (mui:make-id "HELO")
                m:+muia-window-root-object+
                (mui:new-object :group              ; VGroup
                  m:+muia-group-child+
                  (mui:new-object :text m:+muia-text-contents+
                                  (concatenate 'string m:+muix-c+ "Hello, MUI!"))
                  m:+muia-group-child+ ok)))
         (app (mui:new-object :application          ; ApplicationObject
                m:+muia-application-title+  "HelloMUI"
                m:+muia-application-base+   "HELLOMUI"
                m:+muia-application-window+ win)))
    (unwind-protect
         (progn
           ;; DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
           ;;          MUIV_Notify_Application, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit)
           (mui:notify win m:+muia-window-close-request+ t
                       :application m:+muim-application-return-id+
                       m:+muiv-application-return-id-quit+)
           (mui:notify ok m:+muia-pressed+ nil
                       :application m:+muim-application-return-id+ 1)
           (mui:set-attrs win m:+muia-window-open+ t)          ; set(win, MUIA_Window_Open, TRUE)
           (mui:do-application-events ((id) app)               ; NewInput / Wait until Quit
             (case id (1 (format t "OK pressed~%")))))
      (mui:dispose-object app))))                   ; takes the window and its children along
```

`new-object` takes a class keyword (`:window` is `"Window.mui"`,
`:numeric-button` `"Numericbutton.mui"` — every class of `mui.h` and of
the newer MUIs, see `class-id`) or the name string, plus tag/value
pairs with integers, foreign pointers (child objects), `T`/`NIL` and
strings; `make-object` is `MUI_MakeObject`'s builtin collection
(`:button`, `:checkmark`, `:cycle`, `:radio`, `:slider`, `:string`, …);
`notify` takes `:every-time`, the `:self` / `:window` / `:application` /
`:parent` destinations and `:trigger-value` in the parameters; `get-attr`,
`get-attr-string`, `set-attrs`, `do-method` (any `MUIM_*` method),
`return-id` / `application-input` for the return-ID queue, `request`
(`MUI_RequestA`), `pool-string-array` for `MUIA_Cycle_Entries`, `make-id`
and the `MUIV_Window_*` size macros complete it.  Setting
`amiga.mui:*event-loop-timeout*` makes `do-application-events` return
after that many seconds — how the examples run unattended.  The loop
waits with `amiga:wait-signals`, exec `Wait()` as a GC safe region, so a
worker thread (`mp:make-thread`) beside a GUI can allocate and collect
while the loop sleeps; a program that parks its own task on a signal
another thread raises should wait the same way (`docs/amiga.md`).  `available-p`
tells whether `muimaster.library` opens; the module itself loads
everywhere (the raw module, by contrast, needs the library at `require`
time).  The toolkit-neutral half is `AMIGA.BOOPSI`, re-exported (same
symbols as `AMIGA.REACTION`'s).

**Hooks and custom classes** are Lisp functions too.  `pool-hook` makes
a `struct Hook` whose entry is a Lisp function of `(hook object message)`
— a `MUIA_List_DisplayHook`, the target of a `MUIM_CallHook`
notification, a `MUIA_Group_LayoutHook` — and `create-custom-class` is
`MUI_CreateCustomClass` with a Lisp dispatcher of `(class object
message)`, with `do-super-method`, `method-id`, `inst-data`,
`add-min-max` / `set-min-max`, `request-idcmp` / `reject-idcmp` for a
class that answers `MUIM_HandleInput`, and the `_rp(obj)` / `_mleft(obj)`
/ `_minwidth(obj)` shortcuts (`area-rastport`, `area-mleft`,
`area-min-width`, …) for writing its methods; both live in the foreign
pool and are released after the objects.  A class whose objects sit on
an interactive path should name the methods it handles
(`create-custom-class … :methods (list id …)`): MUI sends an object many
methods per repaint and keystroke, and the ones not listed go to the
superclass natively instead of calling into Lisp.  One hook cannot be Lisp: MUI
3.8 edits an active `String` on input.device's task, where no Lisp can
run, so a `MUIA_String_EditHook` that takes keys away from the gadget
(TAB for completion, C-g, …) is `make-string-key-hook` — native, with a
table of raw keys filled from the keymap in Lisp — which queues each
taken key to an object as a method through `MUIM_Application_PushMethod`,
back on the application's task; `tests/amiga/test-mui.lisp` drives it
as Intuition does.  A group that lays itself out
reads its `struct MUI_LayoutMsg` with `layout-msg-type`,
`layout-msg-width` / `-height`, `layout-msg-min-max` and
`layout-children` (the `NextObject()` walk), and places each child with
`layout-child` (`MUI_Layout`).  The function runs on MUI's stack,
so nothing may unwind through MUI: an unhandled error (or a `throw` to a
catch outside the hook) is caught at the callback, the hook returns 0,
and the condition is re-signaled in Lisp when the call into MUI that
invoked it returns — a `handler-case` around `set-attrs` or the event
loop catches it there, the debugger shows it there.

[`examples/amiga/mui/`](examples/amiga/mui/) holds the MUI examples:
`hello` (above), the ports of the MUI 3.8 developer kit's demos —
`balancing`, `pages`, `menus`, `showhide`, `virtual`, `class1` (the
kit's simplest custom class, a subclass of `Area.mui` drawing a fan of
lines from `MUIM_Draw`), `layout` (a custom layout hook placing the
group's children itself) and `slidorama` (four custom classes: a
`Levelmeter.mui` that follows the mouse through `MUIM_HandleInput`, and
`MUIM_Numeric_Stringify` overrides on `Slider.mui` and
`Numericbutton.mui`) — plus `group-layout`, `numeric`, `requester` and
`hooks` on the group attributes, the numeric classes, `MUI_Request`, and a
list display hook with `MUIM_CallHook` buttons; `tests/amiga/test-mui.lisp` /
`tests/test_amiga_mui.sh` are the module's executable specification —
the Amiga side drives a real Application / Window / String tree, fires
`MUIM_Notify` from Lisp, has MUI dispatch to a Lisp custom class and call
a Lisp display hook, and reads the return IDs back without a user — and
`docs/amiga.md` has the table.

### Graphics examples (double-buffering, sprites, screenshots)

[`examples/amiga/gfx/`](examples/amiga/gfx/) holds the graphics demos:
`bouncing-lines.lisp` (a window on the Workbench screen, drawn through
the curated `AMIGA.GFX`), `doublebuffer.lisp` — the NDK 3.1
`intuition/doublebuffer.c` example: a face bouncing on a HIRES custom
screen at the frame rate via `AllocScreenBuffer` / `ChangeScreenBuffer`
and the `dbi_SafeMessage` protocol, with an attached control screen of
GadTools sliders and `LendMenus` — `sprite.lisp`, the RKM "Simple
Sprite" listing: a hardware sprite claimed with `GetSprite`, coloured,
and walked across a LORES screen with `MoveSprite` under `WaitTOF` —
and `screenshot.lisp`, which saves the front screen (or the Workbench)
as a PPM file: RTG screens through cybergraphics.library / Picasso96
`ReadPixelArray`, chipset screens straight from their bitplanes and the
ViewPort's palette — the technique the FS-UAE harness uses to
photograph the other examples.  The ports are written against the
generated raw bindings (`AMIGA.RAW.GRAPHICS` / `AMIGA.RAW.INTUITION`)
and are the reference for those parts of the API; setting
`amiga.intuition:*event-loop-timeout*` runs any of them unattended for
that many seconds.  `tests/amiga/test-gfx-examples.lisp` runs them in
the FS-UAE suite, `tests/test_amiga_gfx_examples.sh` load-checks them
on the host, and `make -f Makefile.cross examples-amiga` photographs
them.

### Async file I/O (DOS packets)

`(require "amiga/asyncio")` — package `AMIGA.ASYNCIO` — is a Common Lisp
port of the NDK's AsynchIO package: double-buffered file I/O that sends
`ACTION_READ` / `ACTION_WRITE` packets straight to the filesystem
handler, so the next buffer load is transferred **while your Lisp code
runs**.  Buffers are rounded to the device's block size and 16-byte
aligned for DMA.

```lisp
(require "amiga/asyncio")

(amiga.asyncio:with-async-file (f "data.bin" :read)   ; read-ahead starts here
  (loop for b = (amiga.asyncio:read-byte-async f)
        while b do (process b)))                      ; next block already in flight
```

`open-async` (`:read` / `:write` / `:append`), `read-async` /
`write-async` (bulk, to a foreign pointer or a Lisp byte vector),
`seek-async`, byte / char / line convenience functions, and
`with-async-file`.  I/O errors signal a Lisp error carrying the DOS
error code.  [`examples/amiga/asyncio/copyfile.lisp`](examples/amiga/asyncio/copyfile.lisp)
copies and verifies a file with it, timed against plain streams;
`tests/amiga/test-asyncio.lisp` is the executable specification.

### IFF files (iffparse.library)

`(require "amiga/iff")` — package `AMIGA.IFF` — reads and writes IFF
files through `iffparse.library`, grown from the NDK's `sift` example:
`sift` prints the IFFCheck-like chunk listing of any IFF file, or of
the clipboard (the C program's `-c`).

```lisp
(require "amiga/iff")

(amiga.iff:sift "work:picture.ilbm")     ; . FORM 3120 ILBM
                                         ; . . BMHD 20 ILBM
                                         ; . . BODY 2986 ILBM ...
(amiga.iff:sift :clipboard)
```

Under it: `with-iff` (a file or `:clipboard`, `:read` or `:write`),
`parse-step` / `current-chunk` / `read-chunk-bytes` for walking chunks,
`push-chunk` / `write-chunk-bytes` / `pop-chunk` (sizes back-patched,
odd chunks padded) for writing, `map-chunks` over a whole file, and
`id-string` / `string-id` for the `"FORM"` ⇄ integer identifiers.
Parse errors signal a Lisp error carrying the iffparse error text.
[`examples/amiga/iff/sift.lisp`](examples/amiga/iff/sift.lisp) builds,
lists and re-reads a nested IFF; `tests/amiga/test-iff.lisp` is the
executable specification.

### Raw OS bindings (generated)

Every public function, constant and structure of the AmigaOS 3.2 API is
available as a **generated** binding module under `lib/amiga/raw/` — one
module per library / NDK include subsystem, loaded on demand.  The
ReAction class libraries sit under their kind directory, the way the OS
stores them: `(require "amiga/raw/gadgets/button")` →
`AMIGA.RAW.GADGETS.BUTTON` (`button-get-class`, `+button-text-pen+` …),
`images/bevel`, `classes/window` (`+wmhi-closewindow+`, `+wa-…+` tags),
`gadgets/layout`, `gadgets/listbrowser` …

```lisp
(require "amiga/raw/intuition")
(require "amiga/raw/graphics")

;; C: OpenWindowTagList(newWindow, tagList) -> struct Window *
(amiga.ffi:with-tag-list (tags amiga.raw.intuition:+wa-width+ 320
                               amiga.raw.intuition:+wa-height+ 200
                               amiga.raw.intuition:+wa-idcmp+
                               amiga.raw.intuition:+idcmp-closewindow+)
  (let* ((win (amiga.raw.intuition:open-window-tag-list nil tags))  ; foreign pointer, NIL on failure
         (rp  (amiga.raw.intuition:window-rport win)))               ; struct Window field
    (amiga.raw.graphics:set-a-pen rp 1)
    (amiga.raw.graphics:move rp 10 20)
    (ffi:with-foreign-string (s "Hello from the raw API")
      (amiga.raw.graphics:text rp s 22))
    (amiga.raw.intuition:close-window win)))
```

What a module contains, all derived mechanically from the SDK files — as
one `amiga.ffi:define-binding-table` form whose names are built on first
use (see *Footprint* below):

- **Functions** — a binding for every public library function (LVO,
  registers and arity from the NDK's `*_lib.sfd`), with the d0 result
  converted from the C return type: `struct X *`/`APTR`/`STRPTR` come back
  as a foreign pointer (`NIL` for NULL), `LONG` is signed, `BOOL` is
  `T`/`NIL`, `VOID` returns `NIL`, `ULONG`/`BPTR`/`Tag` stay integers.
  Arguments are integers or foreign pointers (`NIL` = NULL); strings go
  through `ffi:with-foreign-string`.  The C prototype sits next to each
  row as a comment.  A dozen functions with more than seven register
  arguments (`BltBitMap`, `ClipBlit`, …) go through `amiga:call-library`;
  the few that return a `DOUBLE`, pass register pairs or use A5 are listed
  as `;; skipped` comments in the module.
- **Constants** — every `EQU`, `ENUM`/`EITEM` and `BITDEF` of the matching
  assembler includes as `+name+`: `+idcmp-closewindow+`, `+wa-left+`,
  `+memf-chip+`, `+mode-newfile+`, `+adcmd-allocate+` … Where the NDK has
  no assembler include for a C header (the ReAction tags in
  `gadgets/*.h`, `images/*.h`, `classes/*.h`, `reaction/*.h`, the
  `RAWKEY_*` codes of `libraries/keymap.h`, `devices/trackfile.h`, and
  the MUI 3.8 developer kit's `libraries/mui.h` — every `MUIA_`/`MUIM_`/
  `MUIV_`/`MUII_`/`MUIO_` tag, method and value), its integer `#define`s
  and enumerators are read instead: `+button-justification+`,
  `+layout-add-child+`, `+wmhi-closewindow+`, `+reqimage-warning+`,
  `+rawkey-f1+`, `+muia-window-close-request+`, `+muim-notify+` … A
  `#define` whose body is a string literal becomes a **string constant**:
  `amiga.raw.muimaster:+muic-window+` is `"Window.mui"`, `+muix-c+` the
  `"\033c"` centering escape, `+trackfilename+` `"trackfile.device"`.
- **Structures** — `ffi:defcstruct` layouts for every `STRUCTURE` of the
  assembler includes, with the NDK's own offsets and sizes:
  `(amiga.raw.intuition:window-width w)`,
  `(setf (amiga.raw.gadtools:new-gadget-left-edge ng) 10)`,
  `amiga.raw.exec:*io-std-req-size*` — and for every `struct` a C header
  without an assembler twin defines, laid out by the 68k rules (2-byte
  alignment; the MorphOS headers pack to the same layout): `mui.h`'s
  `MUI_MinMax`, `MUI_AreaData`, `MUI_RenderInfo` and the `MUIP_*` method
  messages (`(amiga.raw.muimaster:muip-draw-flags msg)`,
  `*muip-ask-min-max-size*`), `listbrowser.h`'s `ColumnInfo`
  (`*column-info-size*`, `(setf (column-info-title ci) …)`).  Pointer
  fields (`APTR`, `struct X *`) read as foreign pointers; an embedded
  struct, a `char[n]` buffer and an array of structs as a pointer to the
  field; an array of scalars by index (`(mui-pub-screen-desc-system-pens
  d 3)`); the members of a nested union or struct are flattened next to
  it (`mui-input-handler-node-millis`).  A struct the reader cannot lay
  out (a bitfield) is a `;; skipped struct` comment naming the reason.
- **MUI custom classes** — the header of an MCC (`<Name>_mcc.h`: the
  kit's `ExtClasses/`, and `MCC_HEADERS=<dir>` for the NList / TextEditor
  / BetterString headers you have) becomes `amiga/raw/mui/<name>`:
  `(require "amiga/raw/mui/tron")` → `amiga.raw.mui.tron:+muic-tron+`,
  `+muim-tron-demo+`, `*muip-tron-demo-size*`.  Constants and method
  structs only — the class opens by name through `MUI_NewObjectA`, there
  is no library base.
- **The library base** — `*intuition-base*` etc., opened at `require` time
  (a missing library fails there, with its name); `*intuition-version*`
  is the running `lib_Version`.  Device and resource tables
  (`timer`, `cia`, …) have no `OpenLibrary`; their base variable starts
  `NIL` for you to set.

Naming is mechanical: CamelCase splits into words, `_` becomes `-`,
everything is lowercased — `OpenWindowTagList` → `open-window-tag-list`,
`ModifyIDCMP` → `modify-idcmp`, `SetAPen` → `set-a-pen`, `GA_Left` →
`+ga-left+`, `wd_RPort` → `window-rport` (only `RastPort`, `RPort` and
`BitMap` stay one word: `rastport`, `bitmap`).  Names that collide with
`CL` (`dos.library`'s `Open`, `Close`, `Read`, `Write`, `Format`;
`exec.library`'s `Signal`, `Remove`) are shadowed, so
`amiga.raw.dos:open` is its own symbol and `cl:open` is untouched.

**Platforms.** The same modules load on AmigaOS 3.x and MorphOS.  The
generator merges the MorphOS SDK's function tables with the NDK's: the
1100+ functions both provide at the same vector are unconditional; the
OS 3.2 additions MorphOS lacks (or places something else at — `ShowWindow`,
`ErrorOutput`, `NewMinList`, the diskfont outline API …) exist only when
`:morphos` is absent, MorphOS' own extensions (`AllocVecPooled`,
`GetMonitorList`, `MUI_GetRGBColor`, plus the MorphOS-only libraries on
the generator's allowlist — `cybergraphics` by default, function
tables only) only when it is present, and functions newer than OS 3.0
are also gated on the running library's version — so a call that would
jump into a wrong or missing vector is an "undefined function" error
instead.  `muimaster` (MUI) is generated from the MUI 3.8 developer kit
the same way an NDK library is from the NDK: its 25 functions and the
`mui.h` constants are unconditional (MUI 3.8 on AmigaOS, MorphOS's
built-in MUI, MUI 5 all keep them), MorphOS's additions `:morphos`.  On
top of that baseline the post-3.8 MUI surface is merged in additively:
the MUI 5 SDK contributes its extra library functions (`MUI_Show` … —
bound only when the running muimaster reports version 20+) and both the
MUI 5 and the MorphOS `mui.h` contribute their newer `MUIA_`/`MUIM_`/
`MUIV_` tags and `MUIC_` class names, unguarded — a tag unknown to an
older MUI is simply ignored, a missing class makes `MUI_NewObjectA`
return `NULL`.  Values always stay the 3.8 baseline's; the generator
refuses to run if a newer header disagrees on an existing name.  `ahi`
(ahi.device, the AHI audio system — an Aminet install on AmigaOS, built
into MorphOS) is generated from the AHI developer kit the same way:
its 20 functions and every tag, sample type, error code and struct of
`devices/ahi.h`, with the MorphOS SDK's table as the cross-check.  A
device has no `OpenLibrary`, so its base variable starts `NIL` and is
armed by whoever opens it — `AMIGA.AHI` does, from the request's
`io_Device`.  On
the host (macOS/Linux/Windows) the modules load too — the base stays
`NIL` and any call reports that — which is how they are unit-tested.

The modules are committed; regenerate them with `make gen-amiga-bindings`
from an unpacked AmigaOS 3.2 NDK (`NDK=<dir>`, default `tools/aos32-ndk`;
the copy inside the cross toolchain is the fallback) — add
`MOS_SDK=/path/to/morphos/os-include` (the copy with `fd/` and `clib/`)
to merge MorphOS, and a copy of `MUI:Developer` from the MUI 3.8
distribution (`MUI_SDK=<dir>`, default `tools/mui-sdk`; `FD/` and
`C/Include/`) for `muimaster`; without them the output is AmigaOS-only
and `muimaster` a MorphOS function table without constants.  The
post-3.8 MUI surface comes from two more optional inputs: the MUI 5 SDK
for AmigaOS (`MUI5_SDK=<dir>`, default `tools/mui5-sdk` — the unpacked
`SDK/MUI` of the muidev.de os3 release) and the MorphOS SDK's
`libraries/mui.h` placed into `$MOS_SDK/libraries/` (it is
`gg:os-include/libraries/mui.h` on a MorphOS box).  The
`<Name>_mcc.h` headers of the kit's `ExtClasses/` become the
`amiga/raw/mui/` modules; `MCC_HEADERS=<dir>` adds the headers of the
custom classes you have.  The AHI developer kit (`AHI_SDK=<dir>`,
default `tools/ahi-sdk` — the unpacked `Developer/` drawer of Aminet's
`driver/audio/m68k-amigaos-ahidev.lha`) makes `ahi` an AmigaOS module
with its constants and structs; without it `ahi` is a bare MorphOS
function table.  None of the SDKs is redistributed; only the
output is.  The generator is
`scripts/gen-amiga-bindings.lisp` (runs on the host build of amilisp) and
cross-checks every LVO against the NDK's own `lvo/*.i` before writing
anything.  Its executable specification is `tests/test_amiga_bindgen.sh`
(fixture SDK + checks of the committed output); the Amiga-side calls are
exercised by `tests/amiga/test-raw-bindings.lisp`.

The modules are also the reference for the hand-written ones: every
constant, `+lvo-…+` offset and `defcfun` register assignment in
`AMIGA.INTUITION`, `AMIGA.GFX`, `AMIGA.GADTOOLS`, `AMIGA.EXEC`,
`AMIGA.AUDIO` and `AMIGA.AHI` is checked against the generated bindings by
`tests/test_amiga_curated_vs_raw.sh` in `make test` — and so is every
struct offset or size a hand-written module spells as
`+<accessor>-offset+` / `+<struct>-size+` (`AMIGA.MUI`'s
`+mui-area-data-flags-offset+` against `mui-area-data-flags`, its
`+iclass-super-offset+` against intuition's `iclass-super`).

**Footprint.** A module costs what a program uses, not what it defines.
Its bindings ship as one packed table (`amiga.ffi:define-binding-table`,
~47 KB for intuition's ~1500 names) attached to the package; a name
becomes a symbol — with its value or FFI stub, exported — the first time
anything looks it up (the reader, `find-symbol`, `intern`, a FASL).
`(require "amiga/raw/intuition")` is ~50 KB of heap on the host, a
program that touches 150 of its names adds ~15 KB more, and the FASL is
one byte-vector unit instead of thousands of definitions.  Everything
stays ordinary CL: `do-symbols`, `do-external-symbols`, `unintern` and friends build
the whole table on demand (the package is then a plain package), so
nothing observable changes except memory.  `(amilisp::%binding-table-info
"AMIGA.RAW.INTUITION")` reports entries, table bytes and symbols built so
far.  A first `require` from source packs the table once, which a 68020
feels; `make fasl-amiga` precompiles all of `lib/amiga/` on the host (see
*Loading source and FASL files*), and the binary release ships those FASLs.

### Raw FFI Access

Underneath the generated modules sits `amiga.ffi:defcfun` — a register
spec plus an LVO, compiled to a dedicated bytecode op — and
`ffi:defcstruct`; both are available for hand-written bindings.  Neither
creates a wrapper function: the name's function cell receives a small
*FFI stub* (a 20-byte binding descriptor the runtime calls directly — a
function for every purpose: `#'`, `funcall`, `apply`, `trace`,
`describe`, `ext:function-arglist`), which is what keeps a module with
thousands of bindings affordable on an 8 MB machine.  A direct call to a
`defcfun` name compiles to the library-call opcode in the caller;
`(ffi::%ffi-stub-info #'name)` shows a stub's fields.  A whole library's
worth of bindings goes into one `amiga.ffi:define-binding-table` — rows
`(:const "NAME" value)`, `(:fn "NAME" lvo (:a0 …) :result)`, `(:struct
"NAME" size ("FIELD" type offset) …)` — packed at compile time and
materialised name by name on first reference; that is what the generated
modules are (see `docs/amiga.md` for the row syntax).  `defcfun`'s `:doc`
strings are kept on the host and dropped from the FASLs built by `make
fasl-amiga` and the binary release (`amiga.ffi:*defcfun-docstrings*`,
`scripts/compile-lib-fasls.sh --no-docstrings`) to save heap on the target.

```lisp
(require "amiga/ffi")

;; WritePixel(rp, x, y) -> LONG, graphics.library LVO -324
(defvar *gfx* (amiga.ffi:open-library-or-die "graphics.library" 39))
(amiga.ffi:defcfun write-pixel *gfx* -324 (:a1 rp :d0 x :d1 y)
  :result :signed)          ; :unsigned (default) :void :pointer :bool :u16 :i16 :u8 :i8

;; struct layout with explicit offsets; :fptr = foreign pointer (NIL for NULL),
;; (:struct n) = embedded struct, (:array type n) = inline array
(ffi:defcstruct (rect :size 8)
  (min-x :i16 0) (min-y :i16 2) (max-x :i16 4) (max-y :i16 6))

;; or call by offset and register plist without any definition
(let ((dos (amiga:open-library "dos.library" 36)))
  ;; Delay(ticks) — dos.library offset -198, d1 = ticks
  (amiga:call-library dos -198 (list :d1 50))
  (amiga:close-library dos))
```

A register argument — for `defcfun` bindings, the generated modules and
`call-library` alike — is an integer (negative values wrap to the
two's-complement longword), a foreign pointer, `nil` (NULL / FALSE) or `t`
(TRUE, 1), the same coercion tag values get; a `BOOL` parameter such as
`RethinkLayout`'s `refresh` takes a Lisp boolean.  Anything else is a
`type-error` naming the argument, its register and the value.

(For the host's general-purpose foreign-function engine, see [Host FFI](#host-ffi-dlopen--libffi--cffi) above.)

## Documentation

### Package reference


Beyond `COMMON-LISP` / `COMMON-LISP-USER`, AmiLisp ships several packages for
platform extensions, threading, FFI, the Gray-streams protocol, the CLOS
Metaobject Protocol, and the AmigaOS GUI. `COMMON-LISP-USER` already `:use`s most
of them, so their symbols are usually available unqualified at the REPL. Each has
its own reference page under [`docs/`](docs/README.md):

| Package | What it provides | Doc |
|---------|------------------|-----|
| `EXT` | TCP sockets, GC control, environment access, exit hooks, terminal raw mode (TUIs), debug/introspection | [docs/ext.md](docs/ext.md) |
| `MP` | Threads, locks, condition variables, memory barriers | [docs/mp.md](docs/mp.md) |
| `FFI` | Foreign pointers, typed peek/poke, libffi calls & callbacks | [docs/ffi.md](docs/ffi.md) |
| `GRAY` | Gray-streams protocol (define stream classes in Lisp) | [docs/gray.md](docs/gray.md) |
| `MOP` | CLOS Metaobject Protocol (AMOP / closer-mop subset) | [docs/mop.md](docs/mop.md) |
| `AMILISP` | IEEE float bits, package-local nicknames, JIT/trace toggles | [docs/amilisp.md](docs/amilisp.md) |
| `AMIGA`, `AMIGA.*` | Raw library calls, FFI tag lists, the generated OS bindings, Intuition, Graphics, GadTools, ReAction, MUI, ARexx, audio | [docs/amiga.md](docs/amiga.md) |
| `EXT.DEV`, `EXT.DEV.TCP` | The development-command layer behind the ARexx and TCP ports (what Clamacs speaks) | [ARexx port](#arexx-port-amigaos--morphos), [TCP port](#tcp-development-port-all-platforms) |

The symbol lists in those pages are kept honest by `make docs-check`, which
diffs the real package exports against a committed snapshot; run
`make docs-update` after changing a package's exports. See
[docs/README.md](docs/README.md#keeping-the-lists-in-sync).

### AmigaGuide

The binary release ships its documentation as AmigaGuide files, readable
on the Amiga itself with MultiView, each with a Workbench icon: in the
package root `README-FIRST.guide` (the getting-started page,
[README-FIRST.md](README-FIRST.md): what is where, the binaries, the heap
image, and how Clamacs connects to amilisp's ARexx port), this README as
`amilisp.guide` and the Clamacs README as `clamacs.guide`; the package
reference under `docs/` next to its Markdown. Every heading is a node and
every link between the pages works, across the drawers.
`make guide` builds the same layout into `build/guide/` with
`tools/docs/md2guide.lisp`, a small converter written in Lisp and run by
amilisp (on the host or on the Amiga). It supports the Markdown these pages
use and rejects anything else with a `file:line:` diagnostic, so `make test`
fails on a construct it cannot render or on a dangling link.
`tests/md2guide/fixture.md` and its `fixture.guide` are the executable
example of the mapping.

## Architecture

- **Single-pass compiler** from S-expressions to bytecode, executed by a
  stack-based VM.  A [peephole pass](#the-peephole-post-pass-in-practice)
  rewrites and fuses the bytecode; on 68020+ AmigaOS and arm64 hosts a
  [JIT](#jit-m68k-and-aarch64) translates hot functions to native code.
- **Tagged 32-bit values** (`CL_Obj = uint32_t`) — heap pointers are
  arena-relative byte offsets, so the object model is the same 32-bit layout
  on every platform.
- **Memory-efficient** — one arena and, on AmigaOS and MorphOS, an
  Immix-style mark-region collector: allocation bumps into 128-byte-line
  holes, a collection sweeps only the line map, and a sliding compactor
  runs on measured fragmentation; designed for a 68020 @ 14 MHz with 8 MB
  RAM.  Hosts run the classic mark-sweep collector with a generational
  mode on top (see [Garbage collection](#garbage-collection)),
  and each thread there allocates from a private chunk (TLAB) refilled from
  the shared heap, so concurrent allocation doesn't serialize on a global lock
  (`AMILISP_TLAB_CHUNK=<bytes>` tunes the chunk size, `0` disables; see
  `tests/test_gc_threaded.c`).
- **Threads** — kernel threads with a VM each, per-thread dynamic bindings and
  stop-the-world collection at safepoints (pthreads on the hosts, exec
  processes on AmigaOS/MorphOS); see [Threads (MP)](#threads-mp).
- **Platform abstraction** — all OS calls go through `platform.h`, implemented
  for POSIX (`platform_posix.c`), Windows (`platform_win32.c`) and
  AmigaOS/MorphOS (`platform_amiga.c`).
- **FFI** — a generic foreign pointer type with peek/poke on all platforms,
  libffi calls on the hosts, and a 68k assembly trampoline for AmigaOS
  register-based library calls.
- **FASLs and heap images** — `compile-file` writes an architecture- and
  endian-neutral FASL format, and `ext:save-image` snapshots the whole heap
  (see [Loading source and FASL files](#loading-source-and-fasl-files) and
  [Heap images](#heap-images)).

Design notes for the larger subsystems are in [`specs/`](specs/) — the JIT
backends, the Immix collector and the generational GC, heap images, the MOP,
and the performance plan.

## Known Limitations and Future Work

- **Alpha status / ANSI CL gaps** — the major subsystems work (CLOS, conditions, packages, the full numeric tower, arrays, pathnames, streams, `loop`, `format`) and real CL libraries load, but corners of the spec remain unimplemented. Concretely, these standard operators are `fboundp` but signal a "not yet implemented" error when called: `y-or-n-p` / `yes-or-no-p`, `pprint-tab` / `pprint-tabular`, `print-not-readable-object`, `invalid-method-error` / `method-combination-error`, and logical pathnames (`logical-pathname`, `load-logical-pathname-translations`; `(typep x 'logical-pathname)` is always `nil`). `defstruct` supports `(:type list)` / `(:type vector)` but ignores `:named` and `:initial-offset`. The metaobject protocol is a working AMOP subset rather than the complete MOP — see [docs/mop.md](docs/mop.md) for what is covered.
- **Amiga OS coverage** — every OS library is callable 1:1 through the generated raw bindings under `lib/amiga/raw/` (`asl`, `layers`, `commodities`, `datatypes`, `locale`, … — see [Raw OS bindings](#raw-os-bindings-generated)), but the idiomatic Lisp layer on top of them covers only Intuition, Graphics, GadTools, ReAction, MUI, ARexx, audio.device, AHI (opt-in), IFF and async DOS I/O (see [Available Amiga Modules](#available-amiga-modules)). Everything else — ASL requesters, Layers, Commodities, Datatypes, Locale, … — is raw-binding-only for now: fully usable, but with C-shaped structures and tag lists rather than a Lisp-shaped API.
- **Callbacks on AmigaOS/MorphOS** — `ffi:make-callback` builds real entry points on the target too (a 68k stub; on MorphOS a 68k stub into a PPC gate), with the AmigaOS register conventions through its `regs` argument, so `struct Hook`s and BOOPSI dispatchers re-enter Lisp (`amiga.ffi:make-hook` / `make-dispatcher`, `amiga.boopsi:pool-hook`, `amiga.mui:create-custom-class`). Two limits: `:float` / `:double` arguments and 64-bit or floating-point results are refused on the target (the stub returns d0 only), and a callback invoked from a task that is not a Lisp thread — intuition calling a `gadgetclass` method from input.device — returns 0 with a warning instead of running Lisp, so custom *gadget* classes stay out of reach (custom MUI classes are fine: MUI dispatches from the application's own task).
- **Composite streams** — `make-two-way-stream`, `make-broadcast-stream`, `make-concatenated-stream`, and `make-echo-stream` are implemented with their component accessors (see the composite-stream tests in `tests/test_stream.c` / `tests/amiga/run-tests.lisp` for usage). Broadcast, concatenated, and echo streams accept native stream components only — a Gray stream component is rejected with a type error (a two-way stream may wrap Gray streams)
- **Stream external formats** — character streams default to UTF-8; `(open … :external-format :latin-1)` (also `:iso-8859-1`) selects an 8-bit-transparent stream where each code point 0–255 maps to a single raw byte with no transcoding, for byte-faithful I/O over a character stream (e.g. an `rfc2388` multipart upload written to a temp file). `stream-external-format` reports `:latin-1` / `:default`. Other named encodings are not yet selectable — an unrecognised `:external-format` is silently treated as the default. See `tests/test_stream.c` (`open_latin1_*`) and `tests/amiga/run-tests.lisp` for usage.
- **Threading** — `MP` package covers the core bordeaux-threads surface (threads with `interrupt`/`destroy`, mutex + recursive locks, named condition variables with timeout, `with-lock-held` / `with-recursive-lock-held`, `compare-and-swap` / `atomic-incf` / `atomic-decf`, type predicates). `(ql:quickload :bordeaux-threads)` resolves to the AmiLisp [bordeaux-threads fork](#library-forks-amilisp-backends) cloned into `~/quicklisp/local-projects` (its `impl-amilisp.lisp` backends map the BT v1/v2 API onto `MP`); Quicklisp itself relies on `lib/quicklisp-compat.lisp` (network/HTTP adaptations) and the auto-registered `swank` stub from `lib/shims/` (see [Quicklisp](#quicklisp)). The plan is to upstream these once the remaining API gaps close. Not yet covered: semaphores and `with-timeout` (the fork marks these as not implemented, so BT signals its `not-implemented` condition rather than blocking)
- **CPU time on AmigaOS** — `get-internal-run-time` (and the "cpu" figure that `time` prints) measures real process CPU time on POSIX hosts via `getrusage`, but falls back to wall-clock time on AmigaOS because exec has no per-task CPU accounting — there, run time and real time report the same value.
- **Socket timeout clock on AmigaOS** — the socket read/write timeout deadlines are measured with a `DateStamp`-based millisecond clock, which resets at midnight. A timeout window that straddles 00:00 can therefore fire early or late by up to the elapsed-since-midnight amount — a once-a-day edge that is harmless for the typical second-scale timeouts but not exact. Switching the Amiga deadline source to a monotonic `timer.device` clock would remove it. (POSIX is unaffected.)
- **Socket write timeouts over loopback** — a `:output` timeout fires only when the send genuinely cannot make progress (the peer's receive window and the local send buffer are both full). On a `127.0.0.1` connection the host kernel may buffer the data effectively without bound — macOS, in particular, keeps a loopback socket writable no matter how much unread data is queued — so a write timeout will not trigger there even against a peer that never reads. This is a host-buffering property, not a AmiLisp limit; write timeouts behave normally against real remote peers and on AmigaOS. (Read timeouts are unaffected and fire reliably everywhere.) Because of this, the write-timeout path is exercised by the success-path test (a timed write to a draining peer) rather than a loopback saturation test; it shares the same readiness-wait/deadline mechanism as the read path (`poll` on POSIX, `WaitSelect` on AmigaOS).

## TODO

- **Upstream `bordeaux-threads` and Quicklisp patches** — close the remaining `MP`/BT v2 API gaps (semaphores, `with-timeout`, and distinct lock types so `native-lock-p` / `native-recursive-lock-p` / `recursive-lock-p` can tell them apart) so the fork's `impl-amilisp.lisp` backends can merge upstream; same for the Quicklisp network/HTTP adaptations currently in `lib/quicklisp-compat.lisp`
- **Native AmigaOS 4 version** — the other PPC-based next-gen system; a natural next step on the MorphOS build's path
- **Bignum performance** — optional GMP backend for faster arbitrary-precision arithmetic

## Project Structure

```
src/
  main.c          Entry point and REPL
  core/           Compiler, VM, builtins, GC, types, reader, printer, conditions
    builtins_*.c      Builtin functions, split by domain (arith, array, lists,
                      stream, format, hashtable, thread, pathname, ...)
    builtins_ffi.c    FFI package (platform-independent)
    builtins_amiga.c  AMIGA package (AmigaOS/MorphOS only)
    vm.c / compiler*.c  S-expr -> bytecode compiler and stack VM
    peephole.c        Bytecode peephole pass and superinstructions
    mem.c             Arena, allocation fast path, object constructors, roots
    gc_trace.c        GC mark phase (shared by every collector)
    gc_backend.h      The collector seam: one backend per binary (GC=classic|immix)
    gc_classic.c      Classic collector: sweep, free list, generational mode
    gc_immix.c        Immix-style mark-region collector (line map, hole allocator)
    gc_compact.c      Sliding compactor (defragmentation, image save)
    gc_jitscan.c      Conservative JIT-stack scan and its block-start index
    gc_verify.c       Heap verifiers and audits (ext:%heap-verify)
    fasl.c / image.c  FASL reader/writer, heap images
    debugger.c        Interactive debugger and backtraces
  jit/            JIT -- bytecode -> native translators (m68k; AArch64 on arm64 macOS/Linux)
    jit_common.c      When to compile (hot-call policy), switches, counters
    jit_m68k.c        m68k walker: bytecode -> m68k machine code
    jit_a64.c         AArch64 walker: bytecode -> AArch64 machine code
    asm_m68k.c / asm_a64.c  Instruction encoders
    codebuf.c         Code buffer; codeheap.c executable memory (AArch64)
    runtime*.c        Helpers the native code calls (slow paths, NLX, calls)
  platform/       OS abstraction (platform.h)
    platform_posix.c / platform_win32.c / platform_amiga.c   Files, I/O, time, sockets
    platform_thread_posix.c / _amiga.c                       Threads, locks, atomics, TLS
    platform_amiga_rexx.c                                    The ARexx port
    tls_openssl.c                                            TLS provider (OpenSSL / AmiSSL)
    ffi_dispatch_m68k.s                                      68k asm trampoline for library calls
include/
  amilisp.h       Public embedding header
lib/
  boot.lisp       Standard library bootstrap (+ prebuilt boot.fasl)
  clos.lisp       CLOS implementation (+ prebuilt clos.fasl)
  ffi.lisp        FFI utilities (defcstruct, with-foreign-alloc)
  gray-streams.lisp   Gray streams protocol
  dev-*.lisp      EXT.DEV: the development commands behind the ARexx and TCP ports
  asdf.lisp       ASDF (Another System Definition Facility, with AmiLisp adaptations)
  quicklisp*.lisp Quicklisp install + compatibility layer
  shims/          swank stub and cl+ssl facade, auto-registered with ASDF
  amiga/          AmigaOS/MorphOS Lisp modules (loaded on demand): ffi, exec,
                  intuition, graphics, gadtools, boopsi, reaction, mui, arexx,
                  audio, ahi, asyncio, iff
    raw/            GENERATED 1:1 OS bindings, one module per library /
                    include subsystem (scripts/gen-amiga-bindings.lisp)
examples/
  amiga/          AmigaOS / MorphOS examples
    arexx/          ARexx client + CygnusEd macro (amilisp.rexx, load-current-file.ced)
    asyncio/        Async file copy
    audio/          AHI playback
    gfx/            Graphics demos (bouncing-lines, doublebuffer, sprite, screenshot)
    iff/            IFF writer/lister (sift)
    mui/            MUI examples (hello, the MUI 3.8 SDK demos, custom classes, hooks)
    reaction/       ReAction GUI examples ported from the NDK 3.2 (buttons, checkbox, chooser, ...)
clamacs/          Clamacs, the native MUI editor/IDE, with a macOS/Linux host variant (git submodule, shipped in the binary release)
docs/             Package reference (EXT, MP, FFI, GRAY, MOP, AMILISP, AMIGA), benchmark logs, screenshots
README-FIRST.md   Getting-started page of the binary release
icons/            Workbench icons and IconX launchers of the binary release
tests/
  test_*.c        Host test suites (C)
  test_*.sh       Host shell tests
  amiga/          Amiga test suite (Lisp)
trunk/            Integration test scripts and benchmarks (ANSI, Sento, FSet, fiveam, str, ...)
third_party/
  ansi-test/      Paul Dietz ANSI CL conformance test suite
specs/            Design notes (JIT, MOP, generational GC, heap images, performance, ...)
scripts/          Binary release, Aminet upload, FASL/image builders, binding generator
  review/         Pre-commit auto-review + test hook
githooks/         Git hooks installed by `make install-hooks`
tools/
  setup-toolchain.sh   m68k-amigaos-gcc cross toolchain installer
  m68k-amigaos-gcc/    Cross toolchain (git submodule)
  docs/                md2guide, the Markdown -> AmigaGuide converter
  sly/                 SLY/SLYNK launcher script
verify/
  realamiga/      FS-UAE configurations, AmigaOS system image, run/verify scripts
```

## How it compares

| Implementation | Approach | Amiga family (68k / PPC)? | Footprint | Notes |
|---|---|---|---|---|
| **AmiLisp** | Bytecode VM in C, optional m68k/AArch64 JIT, Immix-style GC | **Yes** — its whole reason to exist (68k + native MorphOS/PPC now; OS4 next) | Tiny (core runs in a 1 MB heap) | Alpha; ANSI coverage incomplete |
| **ECL** | Lisp → C, bytecode fallback | No | Medium | Very portable/embeddable on modern hosts |
| **CCL** | Native compiler | No (x86-64/ARM/PPC only) | Large | Fast and mature; no 68k backend |
| **Clasp** | LLVM-based, C++ interop | No | Very large (needs LLVM) | Best for C++/scientific interop |
| **SBCL** | Native compiler | No | Large | Fastest mainstream CL; modern arch only |
| **CLISP** | Bytecode interpreter in C | Historically, now unmaintained | Small | Closest in spirit; no current Amiga build |

**Pros:** runs where nothing else does; tiny and dependency-free; identical behavior on host and Amiga; small, readable C you can actually hack on.
**Cons:** alpha-quality ANSI coverage; a bytecode VM with a light JIT won't match a native compiler's raw speed; the object model is 32-bit throughout, so even on a 64-bit host the heap is capped at 4 GB (a deliberate trade for a compact, Amiga-faithful representation); the ecosystem is (so far) an ecosystem of one.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
