# `EXT` — AmiLisp Extensions

Non-standard utilities that don't belong in `COMMON-LISP`: TCP networking, GC
control, environment/host access, and runtime introspection used by the SLY
backend.

- **Package:** `EXT` (uses `CL`)
- **Inherited by:** `COMMON-LISP-USER`, so `ext:` symbols are usually available
  unqualified at the REPL.

## TCP networking

The socket layer is BSD sockets on POSIX and `bsdsocket.library` on AmigaOS. A
client connects with `open-tcp-stream`; a server listens with `socket-listen`,
then `socket-accept`s connections. Socket streams support per-connection
read/write deadlines via `socket-stream-timeout` — a timed-out operation signals
`ext:socket-timeout` (a subtype of `stream-error`) instead of blocking forever.

```lisp
;; Client
(let ((s (ext:open-tcp-stream "example.com" 80)))
  (format s "GET / HTTP/1.0~c~c~c~c" #\Return #\Linefeed #\Return #\Linefeed)
  (force-output s)
  (read-line s))

;; Per-connection read timeout (seconds; nil clears it)
(setf (ext:socket-stream-timeout s :input) 5)
```

| Signature | Kind | Description |
|-----------|------|-------------|
| `(open-tcp-stream host port &optional connect-timeout)` | function | Connect to `host` `port`, return a bidirectional character stream |
| `(socket-listen port &optional address)` | function | Open a listening server socket on a port (port `0` picks a free one). `address` `t` binds 127.0.0.1 only, `nil` every interface, and a string names one dotted-quad IPv4 address of this machine (`"192.168.1.5"`) to bind alone |
| `(socket-accept listener)` | function | Accept one incoming connection, returning its stream |
| `(socket-local-port listener)` | function | The local port a listening/connected socket is bound to |
| `(socket-stream-timeout stream direction)` | function | `setf`-able place: read/write deadline for a socket stream (`direction` is `:input` / `:output`, value in seconds): `(setf (socket-stream-timeout stream direction) seconds)` |
| `(%set-socket-stream-timeout stream direction seconds)` | function | Internal setter behind the `socket-stream-timeout` `setf` expander |
| `socket-timeout` | condition | Signaled when a socket read/write exceeds its deadline (subtype of `stream-error`) |
| `(open-udp-stream host port)` | function | Create a UDP (datagram) socket aimed at `host` `port` |
| `(udp-stream-send stream buffer &optional length)` | function | Send one datagram from byte vector `buffer` (`length` defaults to its full length) |
| `(udp-stream-receive stream buffer &optional max-length)` | function | Receive one datagram into byte vector `buffer`; returns the received length |
| `(socket-stream-local-endpoint stream)` | function | Local `(address . port)` a socket is bound to |

## TLS

A connected TCP socket stream can be upgraded to TLS **in place**: after
`socket-start-tls` the very same stream object carries the encrypted
connection, so any wrapper already holding it (flexi-streams, chunga,
usocket) keeps working.  The provider is loaded lazily at runtime and is
optional — OpenSSL 1.1.1/3.x via dlopen on the host, AmiSSL v5 on
AmigaOS (also the MorphOS route for now), so `tls-available-p` is the
honest capability gate.  drakma and Hunchentoot consume this layer through
the bundled cl+ssl facade (`lib/shims/cl+ssl`, auto-registered on
`asdf:*central-registry*` when ASDF loads so it shadows any
Quicklisp/ocicl-installed cl+ssl).  Setting `CLAMIGA_NO_SHIMS` to `1` in
the environment before starting amilisp disables the registration — host:
`CLAMIGA_NO_SHIMS=1 amilisp`, Amiga shell: `SetEnv CLAMIGA_NO_SHIMS 1` —
e.g. to run the real cl+ssl on the host, where its CFFI stack works.
End-to-end examples: `tests/tls-loopback.lisp`
(host), `tests/amiga/tls-tests.lisp` (Amiga), `tests/test_tls.c`
(platform layer).

```lisp
;; HTTPS-style client with certificate + hostname verification
(let ((s (ext:open-tcp-stream "example.com" 443 10)))
  (ext:socket-start-tls s :hostname "example.com")   ; :verify t is the default
  (write-string "GET / HTTP/1.0 ..." s)
  ...)

;; Server side (certificate required; key defaults to the same file)
(ext:socket-start-tls conn :server t
                           :certificate "certs/server.pem"
                           :key "certs/server-key.pem")
```

| Signature | Kind | Description |
|-----------|------|-------------|
| `(tls-available-p)` | function | True when a TLS provider is present (loads it on first call) |
| `(tls-version)` | function | Provider identification string (`"OpenSSL 3.x ..."`, `"AmiSSL 5.27"`), or `nil` |
| `(socket-start-tls stream &key server hostname (verify t) certificate key key-password ca-file ca-path (timeout 30))` | function | Upgrade a connected TCP socket stream to TLS in place and return it. Client by default (`hostname` sets SNI and, with `verify`, hostname checking; `ca-file`/`ca-path` name trust anchors, defaulting to the provider's store). `:server t` accepts instead — then `certificate` is required. Handshake failure (including certificate rejection) signals an error with the provider's diagnostic |
| `(socket-tls-p stream)` | function | True when `stream` is already TLS-upgraded |
| `(tls-peer-certificate stream)` | function | Peer certificate as a plist `(:subject :issuer :not-before :not-after)`, or `nil` |
| `(%socket-start-tls stream server hostname verify cert key password ca-file ca-path timeout)` | function | Positional core behind `socket-start-tls` |

## GC, environment, host

| Signature | Kind | Description |
|-----------|------|-------------|
| `(gc)` | function | Force a garbage collection (and compaction when fragmented) |
| `(getenv name)` | function | Read an environment variable |
| `(executable-path)` | function | The running amilisp as a path another process can start it by (`/usr/local/bin/amilisp`, `PROGDIR:clamiga`), or `nil` |
| `(getcwd)` | function | Current working directory |
| `(system-command command)` | function | Run a host/AmigaOS shell command |
| `(defglobal name value &optional doc)` | macro | Define a global (non-dynamic) variable |

## Program arguments

What follows `--` on the command line is the program's, never loaded
(README "Program arguments"); a Workbench start on AmigaOS turns the
icons' `ARGS`/`WINDOW` tool types into the options and every project icon
into one argument (README "Starting from Workbench").  Both variables are
set before `~/.amilisprc` and `*restore-hooks*` run, and a restored image
reports the restoring process's arguments.

| Signature | Kind | Description |
|-----------|------|-------------|
| `*command-line-args*` | variable | The arguments after `--`, as a list of strings, verbatim (`NIL` without); on a Workbench start the full path of every project icon |
| `*workbench-started-p*` | variable | `T` when this process was started from a Workbench icon (AmigaOS), `NIL` from a Shell or on a host |

Runnable examples: `tests/test_command_line_args.sh` (host) and
`tests/amiga/wb-check.lisp` (Amiga, the Workbench start).

## Exit hooks

`ext:*exit-hooks*` is a list of function designators amilisp funcalls with no
arguments on its way out — after `(quit)` / `(exit)`, at the end of a
`--script` or `--non-interactive` run, and when the REPL reaches end of input.
They run *before* any runtime teardown, so a hook can still print, write files,
flush caches and stop threads.

This is the only place user code observes process exit: `(quit)` unwinds
without running `unwind-protect` cleanups, so a cleanup form never sees it.

```lisp
(ext:add-exit-hook (lambda () (save-state "work.dat")))

;; A symbol is resolved when the hook RUNS, so it may be registered before the
;; function exists and always picks up the latest definition.
(ext:add-exit-hook 'shutdown-server)
(ext:remove-exit-hook 'shutdown-server)   ; => T if it was registered
```

- Hooks run **most recently added first**, like `atexit` and nested
  `unwind-protect` cleanups.
- `add-exit-hook` is idempotent under `eql`, so re-loading a file that
  registers its cleanup does not queue the hook twice.
- A hook that signals an error is reported on `*error-output*` and skipped —
  the remaining hooks still run and the process still exits with its intended
  code. A hook that calls `(quit n)` ends the sequence, and its exit code wins.
- The list is taken and cleared before the first hook runs, so a hook
  registered from inside a hook is not run.

| Signature | Kind | Description |
|-----------|------|-------------|
| `*exit-hooks*` | variable | List of function designators run at shutdown, most recently added first |
| `(add-exit-hook function)` | function | Push a function or symbol onto `*exit-hooks*` (no-op if already there); returns its argument |
| `(remove-exit-hook function)` | function | Remove it from `*exit-hooks*`; `T` if it was there, `NIL` otherwise |

## Heap images

`ext:save-image` snapshots the whole session — every function, macro, class,
instance, hash table, package and variable — to a single file that
`amilisp --image FILE` restores in one read, skipping the entire boot and
load sequence.  On a slow Amiga this turns a minutes-long quicklisp warm-up
into a near-instant start.

```lisp
(ext:save-image "work:devel/mysession.img")            ; keep working after
(ext:save-image "work:devel/mysession.img" :quit t)    ; write and exit
```

The image is written at the next top-level prompt (after the enclosing
form finishes), not from inside the call.  With `:quit t` amilisp exits
after writing, running `ext:*exit-hooks*` as usual — the recommended mode
for build scripts.

Restore explicitly with `amilisp --image mysession.img`, or implicitly: a
file named `amilisp.img` in the current directory (or next to the binary,
under `$CLAMIGA_HOME`, or in an install prefix's `lib/amilisp/`) is
auto-discovered at startup; `--no-image` skips that.  An image saved with
a small `--heap` restores fine into a larger one.

Rules and limits:

- **Images are per-build.**  A fingerprint ties each image to the exact
  amilisp build that wrote it; any other build refuses it cleanly.  Host
  images for the host, Amiga images for the Amiga, FPU/WIDE variants each
  their own.
- **OS resources cannot survive a process.**  Saving refuses while worker
  threads are running or file/socket streams are open, and restored
  foreign pointers are invalidated.  `ext:*save-hooks*` (run before the
  dump) and `ext:*restore-hooks*` (run after a restore, most recent
  first) are the supported way to tear such state down and rebuild it.
  The refusal names the threads (`... still running ("arexx-port") ...`):
  the ARexx port `S:.amilisprc` opens for Clamacs, a `REPL-ATTACH`ed
  editor's REPL thread and the TCP development port all run on worker
  threads, so save from a amilisp started with `--no-userinit` -- load
  the project, save, quit -- or `(amiga.arexx:stop)` first.
  The Amiga library bases of the `AMIGA.*` modules need neither: they are
  reopened before `~/.amilisprc` runs (`amiga.ffi:define-library-variable`,
  [amiga.md](amiga.md)).
- `ext:*image-restored-p*` is `T` in a restored session — it is already
  set when `~/.amilisprc` runs, so an rc file can skip loads the image
  already contains.
- **Process state is re-derived, not restored.**  `*default-pathname-defaults*`
  names the restoring process's current directory (not the one the image
  was saved from), and `*random-state*` is freshly seeded, exactly as on a
  normal boot.

### Shipping an image: `:shake-bindings`

A generated OS binding module (`amiga/raw/…`) ships one packed *binding
table* and builds a symbol only when something names it, so a module costs
what a program uses rather than what it defines.  In an application image
the set of referenced names is already closed, and those tables — ~150 KB
for the four common modules — are dead weight.

```lisp
(ext:save-image "closure.img" :quit t :shake-bindings t)
```

drops them just before the dump.  Names referenced before the save are
ordinary symbols and keep working (value, `#'`, `funcall`, `(setf (field
…))`); names never referenced **stop existing** — `find-symbol` returns
`NIL`, and reading `pkg:name` signals an error that says the table was
shed.  So reference everything the program needs before saving, including
names it would otherwise reach through `intern`/`read` at run time; and
because platform and version guards are resolved when a name is built,
build the image on the oldest configuration you support.  The shed is
permanent for the running session too, not only the image — loading the
module's FASL again re-attaches a table if you need it back.

Use it for delivery, not for a development snapshot.

### Delivering a program: `:executable`

```lisp
(load "app.lisp")
(ext:save-image "app" :executable t :toplevel 'app:main :quit t)
```

writes **one file that is the program**: a copy of the running amilisp
with the image appended.  It needs nothing beside it — no `lib/`, no image
file, no launcher script — and starts like any other program, from a Shell
or from a Workbench icon:

```
app input.txt --verbose
```

- **The command line is the program's.**  Every argument arrives in
  `ext:*command-line-args*` verbatim; none is a amilisp option, so
  `--help` or `--heap` mean whatever the program says they mean.  Started
  from Workbench, the icon's `ARGS` tool type and the project icons arrive
  the same way (see [Program arguments](#program-arguments)).
- **`:toplevel`** names the function the program is — a symbol or a
  function, called with no arguments once the restore hooks ran.  When it
  returns the process exits with status 0; `(quit n)` exits with `n`; an
  error nothing handled is printed and exits with 1.  `ext:*exit-hooks*`
  run in every case.  Without `:toplevel` the executable is a amilisp that
  starts at the REPL with the program loaded.
- **`:heap-size`** is the heap the program starts with, in bytes — the
  executable has no `--heap` option to give later.  Left out, it gets the
  default heap, or as much as the image needs if that is more.
- `~/.amilisprc` is not loaded and no banner is printed: nothing of the
  development environment shows through.
- Everything said about images above holds: threads and open streams must
  be gone before the save (start the saving amilisp with `--no-userinit`
  so an rc file's ARexx port is not among them), `ext:*restore-hooks*`
  rebuild OS state at start, and `:shake-bindings t` trims the image.
- The executable is for the machine type that saved it — it *contains*
  that amilisp.  Build the AmigaOS program on AmigaOS (or in an emulator),
  the MorphOS one on MorphOS, the host one on the host.  A delivered
  executable can itself save the next one.
- On Windows give the file its `.exe` name; elsewhere the name is free.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(save-image pathname &key quit shake-bindings executable toplevel heap-size)` | function | Arm a heap-image dump; it executes at the next top-level safe point.  With `:quit t` the process exits after writing; with `:shake-bindings t` the demand-interned binding tables are shed first; with `:executable t` the file is a self-contained program (see above) that runs `:toplevel` with a heap of `:heap-size` bytes |
| `*save-hooks*` | variable | Functions funcalled (most recent first) right before the dump — close streams / tear down FFI state here |
| `*restore-hooks*` | variable | Functions funcalled (most recent first) after a `--image` restore, following `~/.amilisprc` |
| `*image-restored-p*` | variable | `T` when this session came from `--image` |

Runnable end-to-end examples: `tests/test_image.sh` and
`tests/test_image.c` (host), `tests/amiga/image-save.lisp` /
`image-verify.lisp` (Amiga); for delivered executables
`tests/test_executable.sh` (host) and `tests/amiga/exe-save.lisp` (Amiga,
Shell and Workbench start).

## Bulk byte-vector operations

C-speed loops over `(unsigned-byte 8)` vectors for binary file formats —
per-byte Lisp loops cost a VM round-trip per byte, which is prohibitive on a
14MHz 68020 (decoding one IFF ILBM image took seconds).  Standard functions
`read-sequence` / `write-sequence` / `replace` / `map-into` already take C
fast paths on byte vectors; these two cover the decode and reshuffle steps
that have no standard equivalent:

```lisp
;; ByteRun1/PackBits RLE decode (IFF ILBM BODY, TIFF, MacPaint):
;; decode from SRC[pos..end) until DST-LEN bytes land in DST at DST-START;
;; returns the new source position.  Signals on truncated/overlong data.
(ext:unpack-byterun1 src pos end dst dst-len &optional (dst-start 0))

;; Strided row copy — the gather/scatter step for interleaved formats:
;; row I goes from SRC[src-start + I*src-stride ...) to
;; DST[dst-start + I*dst-stride ...), CHUNK bytes per row.  Returns DST.
(ext:copy-rows dst src count chunk dst-start dst-stride src-start src-stride)

;; Together they decode a whole interleaved ILBM BODY in one call and pull
;; each bitplane out with one call per plane:
(ext:unpack-byterun1 body 0 (length body) buf (length buf))
(dotimes (p depth)
  (ext:copy-rows (aref planes p) buf height row-bytes
                 0 row-bytes (* p row-bytes) (* n-planes row-bytes)))
```

| Signature | Kind | Description |
|-----------|------|-------------|
| `(unpack-byterun1 src pos end dst dst-len &optional dst-start)` | function | Decode ByteRun1/PackBits RLE data between byte vectors; returns the new source position |
| `(copy-rows dst src count chunk dst-start dst-stride src-start src-stride)` | function | Copy `count` rows of `chunk` bytes with independent source/destination strides; returns `dst` |

## Terminal control (TUI raw mode)

Primitives for full-screen terminal applications (e.g. the
[cl-tuition](https://github.com/atgreen/cl-tuition) TUI library): raw mode
turns off echo and line buffering so single keypresses arrive immediately,
and while it is active `listen` / `read-char-no-hang` on the console report
input availability exactly — a TUI input loop can poll without blocking.
POSIX hosts use termios; AmigaOS uses the console handler's raw mode.
See `tests/test_tty.c` for a complete usage example.

```lisp
(when (ext:tty-p)
  (ext:tty-raw-mode t)                    ; no echo, keys arrive as typed
  (unwind-protect
       (loop for ch = (read-char-no-hang) ; NIL until a key is pressed
             until (eql ch #\q))
    (ext:tty-raw-mode nil)))              ; always restore cooked mode

(ext:tty-size)                            ; => (cols . rows), or NIL
```

| Signature | Kind | Description |
|-----------|------|-------------|
| `(tty-p)` | function | `T` iff stdin is an interactive terminal/console |
| `(tty-raw-mode enable)` | function | Enable (`t`) / disable (`nil`) raw mode; returns `T` on success |
| `(tty-size)` | function | Terminal size as `(cols . rows)`, or `NIL` when unknown |

## Introspection / debugging (SLY backend)

| Signature | Kind | Description |
|-----------|------|-------------|
| `(backtrace &optional max-frames)` | function | Capture the current call stack (used by the debugger / SLDB); `max-frames` limits the depth |
| `(frame-locals frame-index)` | function | Local variable bindings of a stack frame |
| `(function-arglist fn)` | function | Lambda list of a function, for completion/arglist display |
| `(function-source-location fn)` | function | Source file/position of a definition (`M-.`) |

## Source of truth

- Bulk byte-vector operations: `tests/test_byte_vector.c` (the
  `unpack_byterun1_*` and `copy_rows_*` tests) and the matching blocks in
  `tests/amiga/run-tests.lisp`; the ILBM loader in the Lambda's Tale
  engine repo (`src/ilbm.lisp`) is the worked example.
- TCP sockets & timeouts: `tests/test_stream.c`
  (`platform_socket_table_grows_many_connections`, `socket_read_timeout_*`,
  `eval_socket_stream_timeout_*`) and `tests/amiga/run-tests.lisp` (the
  `socket-listen` / `socket-accept` / `socket-local-port` block).
- Exit hooks: `tests/test_exit_hooks.c` (list semantics and the hook runner)
  and `tests/test_exit_hooks.sh` (real process exits through `(quit)`,
  `--script`, `--non-interactive` and REPL EOF); the Amiga leg is the exit-hook
  block in `tests/amiga/run-tests.lisp` plus the `EXIT-HOOK-RAN` marker
  `Makefile.cross`'s `verify-amiga` requires in the results log.
- Introspection: `tests/test_backtrace.c`, `tests/test_arglist.c`,
  `tests/test_srcloc.c`, and the `ext:backtrace` probes in
  `tests/amiga/run-tests.lisp`.

See also the [Emacs (SLY) integration](../README.md#emacs-sly-integration) and
[Networking](../README.md#networking-tcp-tls-udp) sections of the main README.
