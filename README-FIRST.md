# AmiLisp -- read me first

Common Lisp for AmigaOS 3+ and MorphOS.  This drawer is the binary
release: everything runs from here, from any current directory, with no
assigns and no environment variables.  Unpack it, then double-click an
icon or read on.  (This page is `README-FIRST.guide` on the Amiga and
`README-FIRST.md` everywhere else; the two are the same text.)

## What is where

| File | Description |
|------|-------------|
| `AmiLisp`, `AmiLisp-FPU`, `AmiLisp-MOS`, `Clamacs`, `Clamacs-FPU`, `Clamacs-MOS` | Workbench icons, one per binary: a double-click starts amilisp or the Clamacs editor from `bin/aos3`, `bin/aos3-fpu` or `bin/mos`, see Workbench below |
| `README-FIRST.guide` | this page |
| `amilisp.guide` | the manual: features, usage, the GUI libraries, the ARexx port, known limitations (the project's README) |
| `clamacs.guide` | Clamacs, the editor/IDE: keys, windows, the REPL and debugger windows |
| `bin/aos3/amilisp` | AmigaOS 3.x, 68020 or better -- runs on any CPU |
| `bin/aos3-fpu/amilisp` | AmigaOS 3.x, hard-float build -- REQUIRES an FPU |
| `bin/mos/amilisp` | MorphOS (PowerPC, native) |
| `bin/*/amilisp.img` | the heap image of the bare boot, one per binary (see Startup) |
| `bin/*/clamacs.img` | Clamacs, the editor/IDE, as a heap image beside each binary: the `Clamacs` icon starts `amilisp --image clamacs.img` |
| `lib/` | the runtime library: precompiled FASLs plus the Lisp sources; `lib/clamacs/` is the editor's |
| `docs/` | the package reference, as AmigaGuide and Markdown; `docs/README.guide` is its index |
| `examples/` | example programs, as Lisp source |
| `README.md`, `LICENSE` | the manual as Markdown, and the Apache-2.0 license |

## Workbench

Open this drawer on Workbench (or Ambient on MorphOS).  The icons come
in two columns -- amilisp on the left, the Clamacs editor on the right --
and one row per binary: `AmiLisp` / `Clamacs` are the soft-float AmigaOS 3
build that runs on every 68020+, `AmiLisp-FPU` / `Clamacs-FPU` the
hard-float build (FPU required, see below; the editor and the amilisp it
starts for its REPL are then both hard-float), `AmiLisp-MOS` /
`Clamacs-MOS` the native MorphOS build.  Pick the row for your machine;
the icons do no detection, so an AmigaOS icon on MorphOS (or the other
way round) only fails to start.  Each icon runs the small launcher script
of the same name through IconX, from this drawer, with a 128K stack --
the same as the shell quick start below.  The console's size and title
are the WINDOW tool type of the AmiLisp icons (Icons > Information);
closing that window ends amilisp.

Every `.guide` file has an icon too: a double-click opens it in
MultiView.

## Which AmigaOS binary?

`bin/aos3` does float math in software (mathieeedoubbas.library) and runs
on every 68020+ machine, FPU or not.  `bin/aos3-fpu` is compiled for the
68881/68882 FPU: float arithmetic runs directly on the FPU and is much
faster.  Use it if your machine has one -- 68881/68882 boards, 68040/68060
(with the standard 68040/68060.library installed), Vampire/Apollo,
PiStorm.  On a machine without an FPU it will crash; when in doubt, start
with `bin/aos3`.

Both binaries print and read floats identically (conversion is exact
integer arithmetic, independent of the FPU), so FASLs and float-heavy
source files are fully interchangeable between them.

## Quick start from a shell

```
stack 131072
cd <this drawer>
bin/aos3/amilisp
```

On MorphOS use `bin/mos/amilisp`, on FPU machines `bin/aos3-fpu/amilisp`.

The binary finds `lib/` on its own: it looks in the current directory, in
`PROGDIR:lib`, and two directory levels above the executable -- which is
exactly where `lib/` sits in this layout.  No assigns or environment
variables are needed; you can run it from any current directory.

A stack of 128K (`stack 131072`) is recommended.  The AmigaOS default of
64K is enough for the core, but deeply nested source (the GUI libraries,
Quicklisp systems) needs more -- with too little stack you get a clean
"C stack nearly exhausted" error instead of a crash.

For bigger programs raise the heap, e.g. `bin/aos3/amilisp --heap 16M`.

## Startup

Each binary starts from the `amilisp.img` beside it: a snapshot of the
booted runtime (boot + CLOS), restored in one read instead of loading
`lib/boot.fasl` and `lib/clos.fasl` form by form.  Keep the image next to
its binary -- it is tied to that exact build and refused by any other.
`amilisp --no-image` boots from the FASLs instead, and `--boot-log` prints
the startup phase timings either way.  See [Heap images](docs/ext.md#heap-images)
in the EXT reference for saving images of your own.

amilisp runs `S:.amilisprc` at startup when the file exists (Lisp forms,
evaluated one after the other -- the place for `(require ...)` lines and
your own settings; `--no-userinit` skips it).  An `S:.clamigarc` from an
earlier release is still read when there is no `S:.amilisprc`.

## Clamacs and the ARexx port

Clamacs is an Emacs-flavoured Lisp editor and IDE: a MUI application,
written in Lisp and running as a amilisp of its own, that talks to a
running amilisp over amilisp's ARexx port --
load, compile and evaluate from the buffer, arglists and completion, jump
to definition, a REPL window (`C-c C-z`), a debugger window with restarts
and backtrace, and an inspector (`C-c I`).  It needs MUI 3.8 or newer
(muimaster.library 19+) and TextEditor.mcc 15.29 or newer in
`MUI:Libs/mui/` (MorphOS ships it; on AmigaOS 3 install both from
Aminet).  The editor checks for them at startup.

There are two ways to pair the editor with a amilisp.

**Start Clamacs alone.**  The first command that needs amilisp asks "No
amilisp ARexx port was found.  Start amilisp in its own console window?";
Start launches the `amilisp` the editor itself runs on (`bin/aos3/amilisp`,
`bin/aos3-fpu/amilisp` or `bin/mos/amilisp`, the one beside its
`clamacs.img`) with a 128K stack, your `S:.amilisprc` loaded, and tells it
to open its ARexx port -- no rc file is needed for this.  For a bare helper
without your rc put `(setq *clamiga-options* '("--no-userinit"))` into
`S:.clamacsrc`; `*clamiga-heap*` sets its heap (see clamacs.guide).

**Start amilisp first, Clamacs second.**  The editor connects to a amilisp
that already has its port open.  amilisp does not open the port by itself,
so put these two lines into `S:.amilisprc` (create the file if you have
none):

```lisp
(require "amiga/arexx")
(amiga.arexx:start)
```

Every amilisp started from then on -- from the `AmiLisp` icon or a shell --
opens the port `CLAMIGA` (a second instance takes `CLAMIGA.1`, and so on);
for the current session only, type the same two forms at the REPL.  Note
that a session with the port open cannot `EXT:SAVE-IMAGE` -- the port runs
on a thread -- so save images and executables from a amilisp started with
`--no-userinit`.

From a shell, files to edit go after `--`:
`bin/aos3/amilisp --image bin/aos3/clamacs.img --non-interactive --eval "(clamacs::run)" -- Work:my.lisp`
(that is what the `Clamacs` icon runs, without files; `Clamacs-FPU` and
`Clamacs-MOS` run the same from `bin/aos3-fpu/` and `bin/mos/`).
`S:.clamacsrc`, if
you have one, is loaded before the first window opens.

The editor's keys, windows and menus are in [clamacs.guide](clamacs/README.md);
the port's commands (`LOAD`, `EVAL`, `COMPLETE`, ...) are in the manual's
ARexx section ([amilisp.guide](README.md#arexx-port-amigaos--morphos)).

## Libraries

```lisp
(require "asdf")             ; ASDF system loader
(require "quicklisp")        ; Quicklisp client
(require "amiga/intuition")  ; windows, screens, IDCMP events
(require "amiga/graphics")   ; drawing primitives
(require "amiga/gadtools")   ; GadTools gadgets and menus
(require "amiga/reaction")   ; ReAction helpers (OS 3.5+/3.2, MorphOS)
(require "amiga/mui")        ; MUI (MUI 3.8+, MorphOS)
(require "amiga/raw/<lib>")  ; generated 1:1 OS bindings, every library/class
(require "amiga/exec")       ; memory introspection, chip RAM
(require "amiga/audio")      ; audio.device sample playback
(require "amiga/arexx")      ; the ARexx port (see above)
```

The core library and everything under `lib/amiga/` (including the
generated raw OS bindings) ship precompiled (`*.fasl`, loaded directly --
the sources sit next to them for reference); the remaining modules
(asdf, quicklisp) are Lisp sources that compile on your machine the first
time they are required and are cached under `S:cl-amiga/faslcache/`, so
later loads are fast.

## Documentation

Every page ships twice: as AmigaGuide (`*.guide`, each with an icon --
double-click it, or `MultiView amilisp.guide` from a shell) and as
Markdown (`*.md`).  Every heading is a node, and the links between the
pages work, across the drawers.

- [README-FIRST.guide](README-FIRST.md) -- this page
- [amilisp.guide](README.md) -- the manual: features, the REPL and the
  command line, the GUI libraries, the ARexx port, SLY, known limitations
- [clamacs.guide](clamacs/README.md) -- the Clamacs editor/IDE
- [docs/README.guide](docs/README.md) -- the index of the package
  reference: EXT (sockets, TLS, GC, images, introspection), MP (threads),
  FFI, GRAY (Gray streams), MOP, CLAMIGA, and the AMIGA.* GUI bindings --
  every function documented with its call signature

## Examples

```
bin/aos3/amilisp --load examples/amiga/gfx/bouncing-lines.lisp
bin/aos3/amilisp --load examples/amiga/reaction/listbrowser.lisp
```

`examples/amiga/README.md` lists them all: ReAction and MUI GUIs,
graphics, audio, IFF, ARexx.

## Project

Source code, documentation and issue tracker:
https://github.com/mdbergmann/amilisp -- Clamacs:
https://github.com/mdbergmann/clamacs.  License: Apache 2.0.
