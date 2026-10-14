# Heap images: EXT:SAVE-IMAGE / --image

Status: IMPLEMENTED (802f5c1, 2026-08-21 — all M1-M4 gates green:
host fast tier + test-plus + gc-stress, both FS-UAE legs incl. the
restored-boot full-suite gate; open questions resolved as leaned:
rc runs guarded by EXT:*IMAGE-RESTORED-P*, warn-at-save late roots,
hooks-only MP restart, compression deferred)
Date: 2026-08-20

## Problem

Every amilisp session re-executes its entire startup: boot.fasl, clos.fasl,
then (on the host, via ~/.amilisprc) asdf + quicklisp-compat + gray-streams,
then whatever the user loads.  Measured on the host (warm FASL cache):

| boot                            | live after | consed  | time   |
|---------------------------------|-----------:|--------:|-------:|
| bare (`--no-userinit`)          |     542 KB |  542 KB |   7 ms |
| + asdf + quicklisp (.amilisprc) |     3.1 MB | 10.9 MB | 163 ms |

10.9 MB consed to end at 3.1 MB live — ~4x of boot work is transient, and
the ratio is far worse on a 14MHz 68020 reading from a slow disk, where a
quicklisp-capable boot takes minutes, not milliseconds.  FASLs already cut
the compile cost; an image cuts the *execution and consing* cost too: a
session restore becomes `read(3MB)` + one linear fixup walk.

Target user stories:

- `(ext:save-image "work:devel/mysession.img")` after loading a big system
  on the Amiga; next day `amilisp --image mysession.img` is at the REPL with
  everything loaded in seconds.
- A game/app ships as `amilisp` + `app.img` and starts instantly.
- CI/test scripts skip the quicklisp warm-up leg.

## Why this heap makes it cheap

Two properties of the existing design remove the classic pain points:

1. **Heap references are arena-relative byte offsets, not pointers**
   (`types.h` CL_OBJ_TO_PTR).  A raw dump of `arena[0..bump)` reloads at
   *any* base address — no relocation pass, no fixed-mapping requirement,
   no ASLR interaction.  It also reloads into a **larger** arena: save with
   `--heap 8M`, restore with `--heap 48M`, only `arena_size' >= bump` is
   required.
2. **EQ/EQL identity hashing hashes the offset** (`hash_obj` /
   `cl_hashtable_hash_key`, keys compared with `a == b`).  Because offsets
   are preserved byte-for-byte, no post-restore rehash of identity-keyed
   tables is needed — the entire bug class that compaction has
   (`test_gc_rehash.c`) does not exist for image restore.

### The load-bearing invariant: restore set == GC root set

The compacting GC already enforces that **every C location holding a
`CL_Obj` across a collection is registered**: `global_roots[]`
(cl_gc_register_root, ~463 boot-time registrations), the named shared
globals marked explicitly in `gc_mark` (package registry, the six
compiler tables, CLOS/struct/condition tables), the readtable pool, and
per-thread stacks.  Anything outside that set is *already* a
memory-corruption bug under compaction (see the "stale static tables"
regression class).

Therefore: the set of C-side locations an image must dump and restore is
**exactly the GC root enumeration in `gc_mark` +
`gc_mark_thread_roots` (mem.c)**.  Restore = overwrite the arena, then
overwrite every registered root from the image.  The virgin boot's objects
become unreachable orphans under the copied image bytes; nothing can point
at them because nothing outside the root set may hold a heap reference.
Any future root added to the marker must be added to the image writer — the
spec-level rule is: *the image writer and `gc_mark` must enumerate
the same state*, and a debug audit (below) enforces it.

## Scope

**Images are per-build.**  An image is valid only for the exact binary
(or byte-compatible build) that wrote it, enforced by a fingerprint.
Host images for host, Amiga images for Amiga, FPU= and WIDE= variants
each their own.  This delivers every user story above — the Amiga saves
its own image after its first slow boot.

Portable/cross-built images (build on host, ship for m68k) are a
**non-goal** — see the note near the end for why, in case it ever
resurfaces.

## Image file format

Big-endian on Amiga, native on host — images are per-build, so
"native byte order, recorded in the header" is sufficient; the reader
refuses a mismatch.

```
Header:
  magic       4  "CLIM"
  version     2  CL_IMAGE_VERSION (own counter, starts at 1)
  flags       2  bit 0: wide-strings, bit 1: fpu, bit 2: little-endian
  fingerprint 32 build fingerprint (below)
  arena_size  4  arena size at save (informational)
  bump        4  bytes of arena payload that follow the sections
  n_roots     4  entries in the root section
  n_blobs     4  entries in the side-blob section
  boot_roots  4  n_global_roots at end-of-boot in the saving process

Sections, in order:
  1. ROOTS      n_roots CL_Obj values (see "Root section")
  2. READTABLES cl_readtable_alloc_mask + the full cl_readtable_pool
                (syntax bytes, case modes, macro_fn/dispatch_fn CL_Obj
                arrays — offsets, valid as-is)
  3. THREAD0    main-thread persistent state: current_package, the TLV
                table entries (sym,value pairs), the main thread's
                Lisp wrapper object (CL_Obj)
  4. SOURCES    every distinct source-file name once (see below)
  5. BLOBS      per-bytecode side buffers (see below)
  6. ARENA      bump bytes, raw
```

### Build fingerprint

An image written by a different build must be rejected *before* any bytes
are interpreted.  The fingerprint is a hash (FNV-1a is fine) over:

- `CL_VERSION_STRING_FULL` and `CL_FASL_VERSION` (the opcode set and
  bytecode encoding share fate with FASL — CLAUDE.md already mandates a
  FASL bump for any opcode/encoding change, so this one number keys the
  in-memory bytecode format too)
- `CL_ALIGN`, `sizeof(void *)`, endianness
- `sizeof()` of every heap struct: CL_Cons, CL_Symbol, CL_String,
  CL_Function, CL_Bytecode, CL_Closure, CL_Vector, CL_Package,
  CL_Hashtable, CL_Condition, CL_Struct, CL_Bignum, CL_Ratio, CL_Complex,
  CL_Stream, CL_RandomState, CL_BitVector, CL_Pathname, CL_Cell,
  CL_ThreadObj, CL_Lock, CL_CondVar, CL_ForeignPtr, CL_Restart,
  CL_ByteVector (+ CL_WideString when built)
- `CL_TYPE_MAX` and the feature flags (wide/fpu/gengc/tlab/platform)

Additionally `boot_roots` is checked against the restoring process's
`n_global_roots` at restore time (see below).  A mismatch of either is a
clean "image was written by a different build of amilisp — delete it or
rebuild it" error, never a crash.

### Root section

Roots are dumped **in registration order** — boot is single-threaded and
deterministic, so the Nth `cl_gc_register_root` call in the saving process
and in the restoring process (same build) registers the same C variable.
The section layout:

1. `boot_roots` values for `global_roots[0..boot_roots)` — restored by
   index after the restoring process finishes its own C init.  Restore
   *requires* `n_global_roots == boot_roots` at that point; anything else
   is a fingerprint-level mismatch.
2. The named shared globals, in a fixed order that mirrors
   `gc_mark`: `cl_package_registry`, `macro_table`, `setf_table`,
   `setf_fn_table`, `setf_expander_table`, `type_table`,
   `compiler_macro_table`, `cl_clos_class_table`, `struct_table`,
   `condition_hierarchy`, `condition_slot_table`,
   `condition_default_initargs`, `condition_slot_initforms`.

**Late-registered roots** (registered lazily after boot, `index >=
boot_roots` in the saving session) are deliberately **dropped**: in the
restoring process the corresponding static is still `CL_NIL` and
unregistered, its lazy-init pattern re-derives the value (they are cached
interned symbols) against the restored package table on first use.  This
is safe *only* for re-derivable caches — the save path warns (listing
names under `DEBUG_GC`, which records file:line per root) whenever
`n_global_roots > boot_roots` at save, so a stateful late root is caught
in development, and the coding rule is: **stateful roots must be
registered during init**, as `trace_list`, `SYM_EXIT_HOOKS` etc. already
are.

### Side-blob section (`CL_Bytecode` external buffers)

`CL_Bytecode` is the one heap type that owns raw `platform_alloc`
pointers: `code`, `constants`, `key_syms`, `key_slots`,
`key_suppliedp_slots`, `line_map`, `source_file` — plus the JIT pair
`native_code`/`native_relocs`.  The arena dump preserves the (dangling)
pointer bytes but they are meaningless in the restoring process.

For every live `TYPE_BYTECODE` object, keyed by its arena offset, the
writer emits:

```
  bc_offset            4   arena offset of the CL_Bytecode object
  code_len             4 + code bytes
  n_constants          2 + n CL_Obj values      (arena offsets, valid as-is)
  n_keys               1 + key_syms CL_Obj[] + key_slots u8[] + suppliedp u8[]
  line_map_count       2 + CL_LineEntry[]
  source_file          2   1-based index into SOURCES (0 = none)
```

`native_code`/`native_relocs` are **not** dumped: the restore walk NULLs
those fields and the lazy JIT recompiles exactly as it does after a FASL
load.  `constants` values and `source_lambda_list` are ordinary heap
references — nothing to do.

`source_file` points into the interned source-file pool
(`cl_intern_source_file`), so every function of a file shares one string.
The image keeps that sharing (since v5): the SOURCES section before the
blobs is `n_sources` (4) followed by `len` (2) + path bytes per distinct
name, collected by pointer in the same arena walk that counts the
bytecodes, and a blob names its file by index.  Before v5 each blob
carried its own copy of the path — in the editor's image 2,019 copies of
46 paths, 130 KB of the file.  Restore interns each name once and hands
the pointers out by index; the pre-arena scan rejects an index past the
table (and a table larger than a u16 index reaches) as a corrupt image.
A path the table cannot take (65,535 names, out of memory) leaves that
function without a source file, and SAVE-IMAGE says how many.

A second, small blob class: **string-output-stream buffers**.  A live
output string stream's `out_buf_handle` indexes the growable C outbuf side
table (stream.c).  The writer dumps `(handle, length, bytes)` for every
outbuf slot referenced by a live stream; restore recreates the slots at
the same handles (the segmented table grows on demand, so any handle can
be re-materialized).  Everything else about streams is handled by the
relink walk below.

## Saving

### API and execution point

```lisp
(ext:save-image pathname &key (quit nil))
```

`SAVE-IMAGE` does not dump from inside the builtin frame.  It validates
preconditions, records a pending request, and the dump executes at the
**existing GC-compaction safe point** (`cl_gc_compact_if_pending` call
sites: REPL top level / between top-level forms in load and script
drivers), where by construction no C locals hold `CL_Obj` values and the
main thread's transient stacks (VM stack, frames, dyn/NLX/handler/restart
stacks, gc_root stack) are empty.  This reuses the pending-compaction
mechanism verbatim and eliminates the entire "save from arbitrary C depth"
problem.  Consequence to document: the image is written *after* the
enclosing top-level form finishes, before the next prompt/form.  With
`:quit t` the process exits after writing (running `EXT:*EXIT-HOOKS*` as
usual) — the recommended mode for build scripts.

### Preconditions (checked in the builtin, error with a clear message)

1. **No live worker threads** (`cl_thread_list` is main only).  Same
   doctrine as SBCL's save-lisp-and-die.  Message tells the user to
   JOIN/DESTROY first.
2. **No open file or socket streams.**  One arena walk lists offenders by
   pathname/peer.  Console, string, synonym, two-way, broadcast,
   concatenated and closed streams are fine.  (Sockets can never survive
   a process; refusing beats silently breaking a server object.)
3. **No live FFI state**: no open libraries, no owned foreign pointers
   (`CL_FPTR_FLAG_OWNED`).  Non-owned foreign pointers are permitted but
   invalidated at restore (below).  `EXT:*SAVE-HOOKS*` exists precisely so
   libraries can tear these down (and `*RESTORE-HOOKS*` to rebuild them).
4. **No active FASL readers/writers/MLF pre-pass**
   (`cl_fasl_reader_save_count() == 0` etc.) — guaranteed anyway at a
   top-level safe point, asserted for defense.

### Dump sequence (at the safe point)

1. Run `EXT:*SAVE-HOOKS*` (most recent first, each in its own CL_CATCH,
   like exit hooks).  Hooks run *before* the precondition re-check so a
   hook can close the streams that would otherwise abort the save.
2. Re-verify preconditions (hooks ran user code).
3. `cl_gc()` then `cl_gc_compact()` — dump the minimal, hole-free heap.
   Compaction also guarantees `free_list == 0` and a dense `[0, bump)`
   payload, so the ARENA section needs no free-block awareness.
4. Clear `cl_srcloc_table` (offset-keyed reader diagnostics — stale by
   definition in the next process; also cleared at restore, belt and
   braces).
5. Write header + sections to `pathname` via a temp file + rename
   (delete-before-write on Amiga — see the FS-UAE `.uaem` stale-datestamp
   trap).  On any error: delete temp, signal FILE-ERROR, session
   continues undamaged.

## Restoring

### CLI and discovery

- `amilisp --image FILE` — explicit.
- Auto-discovery, mirroring the `lib/boot.fasl` search in repl.c
  (cwd → CLAMIGA_HOME → exedir → exedir/../lib/amilisp → exedir/../..):
  `amilisp.img` next to `lib/` is used when present.  `--no-image`
  bypasses it.

### Sequence in main()

```
platform_init … cl_thread_init … cl_error_init … cl_mem_init(max(heap_arg,
image bump + slack)) … cl_package_init … cl_symbol_init … cl_reader_init …
cl_printer_init … cl_compiler_init … cl_jit_init … cl_vm_init …
cl_stream_init … cl_builtins_init … cl_debugger_init
    → this is the FULL C init: every root registered, every builtin's
      name→CFunc known, all pure-C side state (mutexes, tables) alive.
cl_image_restore(file)             # instead of cl_repl_init's boot load
    1. read + verify header: magic, version, fingerprint, endianness,
       boot_roots == n_global_roots, bump <= arena_size'
    2. memcpy ARENA payload over arena[0..bump), set cl_heap.bump,
       free_list = 0, reset alloc counters
    3. overwrite all registered roots + named globals from ROOTS
    4. restore READTABLES pool + alloc mask
    5. restore THREAD0: main thread's current_package, TLV entries,
       re-point the main-thread Lisp wrapper (fix its thread_id/table_gen
       to the current main slot)
    6. re-attach BLOBS: intern the SOURCES names once; for each entry,
       find the CL_Bytecode at bc_offset (verify CL_HDR_TYPE ==
       TYPE_BYTECODE — corrupt image → clean error), platform_alloc +
       copy each buffer, point source_file at its interned name,
       NULL the native_code fields; recreate outbuf slots
    7. relink walk (next section)
    8. clear cl_srcloc_table
    9. re-derive process state that came back from the SAVING process:
       *DEFAULT-PATHNAME-DEFAULTS* from this process's cwd
       (cl_pathname_default_from_cwd — otherwise a shipped amilisp.img
       merges every relative LOAD/REQUIRE against a build-machine
       directory while the existence checks use the real cwd), and a
       freshly seeded *RANDOM-STATE* (otherwise every restore replays the
       saver's sequence)
skip boot.lisp; run ~/.amilisprc unless --no-userinit; run
EXT:*RESTORE-HOOKS*; enter REPL/actions
```

Every failure between 1 and 7 is fatal-but-clean: print what mismatched
and exit — never fall through to a half-restored heap.  (Falling back to
a normal boot after step 2 has begun is impossible; falling back before
step 2 is allowed and used for "image not found / wrong fingerprint" when
the image came from auto-discovery rather than an explicit `--image`.)

### The relink walk

One linear pass `ptr = arena+CL_ALIGN … arena+bump`, dispatching on
`CL_HDR_TYPE(ptr)` — the same walk shape as sweep.  Per type:

| type | action at restore |
|---|---|
| CONS, STRING, WIDE_STRING, VECTOR, BIT_VECTOR, BYTE_VECTOR, HASHTABLE, PACKAGE, SYMBOL, STRUCT, CONDITION, RESTART, BIGNUM, RATIO, COMPLEX, SINGLE/DOUBLE_FLOAT, PATHNAME, CELL, RANDOM_STATE, CLOSURE | nothing — fully arena-contained (hashtable bucket_vec is an arena vector; EQ/EQL hashes are offset-based and offsets are preserved; symbol `hash` is content-based) |
| FUNCTION | relink `func` through the builtin registry by (package, name); unknown → point at `bi_stale_builtin` stub that signals a descriptive error naming the function |
| BYTECODE | re-attach side blobs (step 6); if no blob recorded for a live bytecode → corrupt image, clean error.  NULL native_code/native_relocs/native_len/native_reloc_count |
| STREAM (console) | keep — console I/O dispatches on stream_type, no OS handle involved |
| STREAM (string in/out) | keep; out_buf_handle re-materialized from BLOBS |
| STREAM (file/socket) | cannot exist (save precondition).  Defensive: clear OPEN, set EOF, handle_id = 0 |
| STREAM (synonym/two-way/broadcast/concatenated) | keep — children are arena references |
| STREAM (cbuf) | clear OPEN + EOF (load-time C-buffer streams never live past their load) |
| LOCK | plain heap data since specs/mp-locks-heap-words.md (no OS primitive, no table): zero `state` and `depth` — every restored lock is free; `flags` and `name` are kept |
| CONDVAR | same: zero `waiters` (no thread of the restoring process is registered on it) |
| THREAD | if it is the saved main-thread wrapper (matched via THREAD0): re-bind to the live main thread.  Any other wrapper: force `table_gen` mismatch so JOIN/INTERRUPT report "thread no longer exists" — exactly the existing stale-wrapper semantics |
| FOREIGN_POINTER | invalidate: address = 0, size = 0, flags = 0.  FFI deref/call paths already reject a null address; audit and tighten messages ("foreign pointer from a restored image — recreate it via *RESTORE-HOOKS*") |

The walk is also the integrity check: any header whose type exceeds
`CL_TYPE_MAX`, whose size is 0/unaligned, or that walks past `bump`
aborts the restore with offset + header bytes in the message (maximum
diagnostic visibility per the project doctrine).

### Builtin relink registry

`cl_register_builtin` (builtins.c) is the single choke point through which
all ~586 builtin registrations flow (510 defun + 76 table entries).  It
gains a side registry `(package-name, symbol-name) → CL_CFunc` (plain C
hash, off-arena, built during init at zero marginal cost).  The relink
walk resolves each TYPE_FUNCTION's `name` symbol to (home package name,
symbol name) and looks up the current process's CFunc.  Functions created
outside `cl_register_builtin` (audit `cl_make_function` callers) must
either go through the registry or be reachable only from state that is
rebuilt at init anyway; the stale-builtin stub catches anything missed and
names it, so a gap is a loud runtime error, not corruption.

### Collector interactions

- **GenGC (host)**: after restore the entire `[0, bump)` payload is *old*
  space: set the old-space watermark to `bump`, empty nursery above,
  clear the survivor/finalizable side lists, re-arm dirty-page protection.
  Note `cl_mem_init` must allocate the arena with `platform_alloc_pages`
  before the image size is known — read the image *header* before
  `cl_mem_init` to size the arena (header read needs no heap).
- **Classic (Amiga)**: nothing special — `bump` set, `free_list = 0`,
  first GC after restore marks/sweeps normally.
- **TLAB**: main thread's TLAB is virgin post-init; restore invalidates
  any carved chunk (none exists yet at that point in main()).
- **JIT**: nothing to restore; `jit_pinned` and friends are per-collection
  state.  First execution of each hot function recompiles lazily.

## User surface

- `EXT:SAVE-IMAGE pathname &key quit` — as above.  Returns the truename;
  with `:quit t` does not return.
- `EXT:*SAVE-HOOKS*` / `EXT:*RESTORE-HOOKS*` — function-designator lists,
  most recent first, each hook in its own CL_CATCH; the natural siblings
  of `EXT:*EXIT-HOOKS*`.  This is the documented path for anything holding
  OS resources (sockets, FFI libraries, AmiSSL) to survive an image:
  close in the save hook, reopen in the restore hook.
- `EXT:*IMAGE-RESTORED-P*` — T when the session came from `--image`
  (lets .amilisprc skip redundant loads).
- `--image FILE`, `--no-image`, auto-discovery of `amilisp.img`.
- README section: what it is, how to use it, limits (per-build images,
  thread/stream/FFI preconditions), pointing at the test files per the
  documentation policy.

## Delivered executables (added 2026-10-05)

`(ext:save-image path :executable t [:toplevel fn] [:heap-size bytes])`
writes the runtime and the image as one file:

```
[ the running executable, without an image it carries itself ]
[ 68k AmigaOS only: HUNK_DEBUG id, length in longwords       ]
[ the image, exactly as SAVE-IMAGE writes it to a file, +0..3 ]
[ trailer: u32 image offset, u32 image length, u32 heap size,
           8 bytes "CLAMIEXE"                                  ]
```

- ELF, Mach-O and PE loaders ignore bytes behind the last section, so
  the file starts as the runtime does.  **AmigaOS LoadSeg does not**: it
  reads hunk after hunk until the file ends and fails on anything else
  (ERROR_BAD_HUNK / ERROR_FILE_NOT_OBJECT, measured in FS-UAE on 3.1).
  The 68k build therefore wraps image + trailer in one `HUNK_DEBUG`
  (id + longword count in front, patched after the write), which LoadSeg
  skips; the image is padded so the trailer starts on a longword.
- `main` looks at the last 20 bytes of its own file (`platform_executable_
  path`) **before it parses the command line**.  With a valid trailer the
  image is staged from that offset, no option is parsed (argv[1..] is
  `EXT:*COMMAND-LINE-ARGS*`), the user init file is skipped, and a
  trailer whose offsets do not add up to the file is fatal — a delivered
  program never falls back to a REPL.  The image format itself is
  unchanged; staging and the payload read take an offset.
- `:toplevel` is stored as the value of the internal
  `EXT::*IMAGE-TOPLEVEL*` (heap state, so no format change) and honoured
  only by a start from an embedded image: `cl_image_run_toplevel` after
  the restore hooks, then shutdown.  Unhandled error = exit status 1.
- `:heap-size` lives in the trailer because the arena is sized before the
  heap exists.
- Because the tail is read before anything else, the early exits of the
  option parser (`--help`, usage errors) release the platform file table
  (`early_exit` in main.c).
- A Workbench-started process is no CLI and has no program name:
  `platform_executable_path` falls back to the tool name of the WBStartup
  message.

Tests: `tests/test_executable.sh` (+ gc-stress leg), the executable
scenarios of `tests/test_memleak_tracked.sh`, and the Amiga legs
`tests/amiga/exe-save.lisp` (Shell and Workbench start).

## Non-goal: portable images

Cross-built images (host-built, Amiga-run) are not planned.  Recorded
only so the cost is known if the question ever comes back: it would
require (1) replacing `CL_Bytecode`'s nine raw pointers with u32 handles
into a side-blob table, (2) unifying `CL_ALIGN` (8 on 64-bit hosts vs 4
on m68k shifts every offset; `double` fields then need memcpy access),
and (3) a type-driven byte-swap pass.  All three are invasive
(compiler/VM/FASL/GC/JIT), and the per-build design already covers the
actual use cases — the Amiga writes its own image after its first slow
boot, and releases ship FASLs as today.

## Testing

Per the project rules — tests are the spec, every path host + Amiga:

1. `tests/test_image.c` (host units): header/fingerprint round-trip and
   rejection (wrong magic/version/fingerprint/root-count/endianness/
   truncation — each a distinct clean error), root-section index mapping,
   blob serialize/deserialize, relink of each stream kind, builtin
   registry lookup incl. the stale stub, refusal paths (worker thread
   live, open file stream, owned foreign pointer, active FASL reader).
   Uses the existing shutdown/re-init harness for in-process
   save→restore cycles (the "second life in one process" machinery),
   minding the stale-static-table re-init hazards it already documents.
2. Shell tests (`verify/`): save in one process, restore in a second,
   assert state fidelity — functions (incl. closures over cells), macros,
   CLOS classes/methods/instances + dispatch (IC caches rebuilt), structs,
   EQ/EQL/EQUAL hashtables keyed by symbols/conses saved earlier,
   readtable customizations (set-macro-character survives), packages/
   nicknames/shadowing, special bindings, defconstant/CONSTANTP,
   string-output-stream with buffered content, pending pathname state.
   Save with small heap, restore with larger `--heap`.
3. **Restored-boot full-suite gate**: boot from an image and run the
   entire existing Amiga test suite (`tests/amiga/run-tests.lisp`) — the
   strongest equivalence check available, on both FS-UAE legs (soft-float
   and FPU=1) and host.
4. gc-stress (`make test-gc-stress`): the relink walk and blob re-attach
   allocate (interned source paths don't, but restored-session first-GC
   does) — run a save→restore→workload cycle under CLAMIGA_GC_STRESS=1 so
   the first compaction after restore exercises forwarding over a
   restored heap; plus save-under-stress.
5. Root audit: debug check comparing the writer's enumeration against
   `cl_gc_audit_roots` machinery; a DEBUG_GC-build save prints late roots
   (registered after boot) by file:line so a stateful late root is caught
   in CI.

## Open questions

1. **`.amilisprc` semantics with `--image`**: run it (current spec: yes,
   guarded by `EXT:*IMAGE-RESTORED-P*`) or skip unless `--userinit`?
   Leaning: run it — an rc that double-loads quicklisp is idempotent and
   the variable lets users skip explicitly.
2. **Late-root doctrine**: is warn-at-save enough, or should
   `cl_gc_register_root` gain a `stateful` flag that makes a post-boot
   stateful registration a hard error?  Start with the warning; tighten
   if a real bug slips through.
3. **Image + `MP` restart**: should restore re-create a worker pool
   automatically via *RESTORE-HOOKS* convention only, or offer a
   declarative `EXT:*RESTORE-THREADS*` list?  Hooks only, for now.
4. **Compression**: Amiga disks are slow; a trivial RLE/LZ4-class
   compressor could halve read time.  Defer — measure first on the
   Vampire and A1200.

## Milestones

1. **M1 — writer + format** (~500 lines: image.c writer half, header/
   fingerprint, pending-save plumbing at the compact-if-pending sites,
   preconditions, save hooks).  Testable alone: dump + verify tool.
2. **M2 — loader + relink** (~700 lines: header verify, arena restore,
   root overwrite, readtable/thread0 sections, blob re-attach, relink
   walk, builtin registry, GenGC old-space arming).  The risk
   concentrates here; build it under DEBUG_GC with the walk's integrity
   checks from day one.
3. **M3 — surface** (~200 lines: EXT:SAVE-IMAGE builtin, --image/
   --no-image/discovery in main.c+repl.c, hooks, *IMAGE-RESTORED-P*,
   README).
4. **M4 — test suites** as above; gates: `make test`, `make test-plus`,
   `make test-gc-stress`, both `test-amiga` legs, plus the new
   restored-boot suite gate.

Total estimate ~1500–2500 lines of C plus tests.  No FASL version bump
required (nothing about FASL serialization changes); the image format
carries its own `CL_IMAGE_VERSION` with the same bump-on-any-change rule.
