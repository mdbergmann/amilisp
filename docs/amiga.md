# AmigaOS Packages — `AMIGA`, `AMIGA.FFI`, `AMIGA.EXEC`, `AMIGA.INTUITION`, `AMIGA.GFX`, `AMIGA.GADTOOLS`, `AMIGA.BOOPSI`, `AMIGA.REACTION`, `AMIGA.MUI`, `AMIGA.AUDIO`, `AMIGA.AHI`, `AMIGA.ASYNCIO`, `AMIGA.AREXX`

The AmigaOS-native bindings. These exist **only on the AmigaOS build** — on the
POSIX host the packages are not present. `AMIGA` is a C-level package (raw
register-based library calls); the rest are Lisp libraries loaded on demand via
`require`, with zero binary-size cost until used.

| Package | Load with | Provides |
|---------|-----------|----------|
| `AMIGA` | `(require "ffi")` | Raw library open/close and register-based library calls |
| `AMIGA.FFI` | `(require "amiga/ffi")` | Tag lists, `defcfun`, `with-library` |
| `AMIGA.EXEC` | `(require "amiga/exec")` | AvailMem memory introspection, chip-RAM upload helper |
| `AMIGA.INTUITION` | `(require "amiga/intuition")` | Windows, screens, IDCMP events, public screens |
| `AMIGA.GFX` | `(require "amiga/graphics")` | Drawing: lines, rectangles, ellipses, text, pens, fonts, bitmaps, blits |
| `AMIGA.GADTOOLS` | `(require "amiga/gadtools")` | GadTools gadgets, menus, bevel boxes, VisualInfo |
| `AMIGA.BOOPSI` | `(require "amiga/boopsi")` | Toolkit-neutral BOOPSI helpers: the foreign pool, tag lists from Lisp values, `do-method`, `get-attr` / `set-attrs`, exec label lists — shared by `AMIGA.REACTION` and `AMIGA.MUI` |
| `AMIGA.REACTION` | `(require "amiga/reaction")` | ReAction helpers over the generated class modules: object creation, the window.class event loop, requesters; re-exports `AMIGA.BOOPSI` |
| `AMIGA.MUI` | `(require "amiga/mui")` | MUI helpers over muimaster.library: objects by class name, `MUI_MakeObject`, `MUIM_Notify`, the Application event loop, `MUI_Request`, custom classes and layout hooks with Lisp dispatchers; re-exports `AMIGA.BOOPSI` |
| `AMIGA.AUDIO` | `(require "amiga/audio")` | Non-blocking 8-bit sample playback through audio.device |
| `AMIGA.AHI` | `(require "amiga/ahi")` | Opt-in AHI (ahi.device) playback: 8/16-bit mono/stereo samples at any rate from any memory, gapless double buffering, the audio mode database, AHI's mixer through the hook-free low-level API |
| `AMIGA.ASYNCIO` | `(require "amiga/asyncio")` | Double-buffered asynchronous file I/O over DOS packets (the NDK AsynchIO package as Lisp) |
| `AMIGA.IFF` | `(require "amiga/iff")` | IFF file parsing and writing over iffparse.library, with the NDK `sift` chunk lister |

`COMMON-LISP-USER` `:use`s `AMIGA` on AmigaOS, so its symbols are available
unqualified. The `AMIGA.*` libraries are referenced by their package prefix
(e.g. `amiga.intuition:`, `amiga.gfx:`).

---

## `AMIGA` — Raw library calls

Open an AmigaOS library and call any function by its negative vector offset and a
register spec. This is the lowest level; higher-level packages are built on it.

```lisp
(require "ffi")
(let ((dos (amiga:open-library "dos.library" 36)))
  ;; Delay(ticks) — dos.library offset -198, d1 = ticks
  (amiga:call-library dos -198 (list :d1 50))
  (amiga:close-library dos))
```

| Signature | Kind | Description |
|-----------|------|-------------|
| `(open-library name &optional version)` | function | Open library `name` at minimum `version` (default 0 = any); returns a base pointer, or `NIL` on failure |
| `(close-library base)` | function | Close a library base |
| `(call-library base offset reg-spec)` | function | Call `base` at `offset` with a register-spec plist (`(:d1 x :a0 ptr …)`); returns the d0 result as an integer |
| `(call-library-fast base offset regspec &rest values)` | function | Fast path: `regspec` is a fixnum of nibbles (low to high), one register index per value (`d0`..`d7` = 0..7, `a0`..`a5` = 8..13); up to 7 register args |
| `(alloc-chip size)` | function | Allocate `size` bytes of Chip RAM; returns a foreign pointer |
| `(free-chip pointer)` | function | Free Chip RAM from `alloc-chip` |

A register value is an integer (a negative `LONG` wraps to its two's-complement
longword), a foreign pointer (its address), `nil` (0: NULL / FALSE) or `t` (1:
TRUE) — the same for `call-library`, `call-library-fast` and every `defcfun`
binding.  Anything else signals a `type-error` naming the argument, its register
and the value.

ARexx host-port transport. These are the raw primitives; ordinary use goes
through `AMIGA.AREXX` below, which builds the handler thread on top of them.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(arexx-open &optional basename)` | function | Create the public port, claiming `basename` (upcased, default `"AMILISP"`) or the first free `BASENAME.<n>`; returns the name claimed. Must be called from the thread that will wait on it |
| `(arexx-close)` | function | Remove the port, replying to anything still queued |
| `(arexx-port-name)` | function | Name the port is registered under, or `NIL` |
| `(arexx-wait)` | function | Block until a command arrives; returns the command string, or `NIL` when woken by `arexx-request-stop` |
| `(arexx-reply rc &optional result)` | function | Answer the message returned by the last `arexx-wait` |
| `(arexx-request-stop)` | function | Wake the waiting thread so it can shut down; safe from any thread |
| `(arexx-send port command &optional result-size)` | function | Send `command` to a public ARexx port and wait for the reply; returns `(values rc result-string)` |

Waiting for exec signals:

| Signature | Kind | Description |
|-----------|------|-------------|
| `(wait-signals mask)` | function | exec `Wait(mask)`, returning the signals received. Unlike the raw `amiga.raw.exec:wait`, the sleep is a GC safe region: a stop-the-world collection started by another Lisp thread does not wait for this task's next signal. The MUI and ReAction event loops wait with it; use it yourself wherever a task parks on a signal a worker thread raises |

---

## `AMIGA.FFI` — Tag lists & `defcfun`

Conveniences over `AMIGA`/`FFI` for the AmigaOS calling conventions.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(make-tag-list pairs)` | function | Build an AmigaOS TagItem array from `pairs`, a flat list of tag/value pairs; caller frees with `ffi:free-foreign` |
| `(with-tag-list (var &rest pairs) &body body)` | macro | Build a tag list from `pairs`, bind it to `var`, run `body`, free it after |
| `(with-library (var name &optional (version 0)) &body body)` | macro | Open library `name`, bind the base to `var`, run `body`, close it after |
| `(open-library-or-die name &optional (version 0))` | function | `OpenLibrary` that signals a descriptive error (naming the library) instead of returning NIL |
| `(library-version base)` | function | The `lib_Version` of an open library base (the OS revision actually running) |
| `(define-library-variable name init-form &optional doc)` | macro | `defvar` for a value the running system decides — a library base, its version, `NIL` for a device base the program sets itself — whose `init-form` runs again after a heap-image restore, before `~/.amilisprc`, in definition order (a restore zeroes every foreign pointer).  Every library module declares its base this way, so a saved image can call the OS right away; a form that fails then leaves the variable `NIL` and says so.  `tests/test_image.sh` and `tests/amiga/image-verify.lisp` show the contract |
| `(make-hook function &key data)` | function | A `struct Hook` whose entry calls `function` with the hook, the object and the message (foreign pointers — a0, a2, a1 of the hook convention); the result is what the OS gets in d0 (`callback-ulong`: integer, pointer, `T`/`NIL`); `data` goes into `h_Data`.  Returns the hook as a foreign pointer.  The function runs on the OS's stack: an unhandled error or a non-local exit to outside the hook is caught there, the hook returns 0, and the condition is re-signaled after the library call that invoked it returns |
| `(free-hook hook)` | function | Release a hook (after every object holding it is gone); `NIL` is ignored |
| `(hook-entry hook)`, `(hook-data hook)` | function | The callback behind `h_Entry` (on the host a C function of `(hook, object, message)`), and `h_Data` as an unsigned integer (`setf`-able) |
| `(make-dispatcher function &key methods)` | function | A BOOPSI class dispatcher for `MakeClass` / `MUI_CreateCustomClass`: `function` gets the class, the object and the message and returns the method's result.  With `methods`, a list of method ids, `function` is called for those methods only and every other message goes to the superclass natively, without entering Lisp |
| `(dispatcher-stats dispatcher)` | function | For a dispatcher made with `methods`: messages received, messages passed to the superclass natively, method ids listed (three values); `nil` otherwise |
| `(free-dispatcher dispatcher)` | function | Release it once the class is removed |
| `(callback-ulong value who)` | function | The ULONG coercion the hook / dispatcher results go through: integers (wrapped to 32 bits), foreign pointers, `T` (1), `NIL` (0) |

Under `make-hook` sits `ffi:make-callback`'s `regs` argument — `'(:a0 :a2 :a1)` — which
also serves any other register-convention entry the OS may call.  On the target a
callback returns a 32-bit integer or pointer in d0 and takes no `:float` / `:double`
arguments; a callback invoked from a task that is not a Lisp thread (intuition calling a
gadget class from input.device) returns 0 and is reported with a warning.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(defcfun name library-base offset (&rest reg-spec) &key void result doc)` | macro | Define `name` as a binding for the library function at `offset` in `library-base`; `reg-spec` alternates register keywords and parameter names (`(:a1 rastport :d0 x …)`); `:result` chooses how d0 comes back — `:unsigned` (default), `:void`, `:pointer` (foreign pointer, NIL for NULL), `:signed`, `:bool`, `:u16`/`:i16`/`:u8`/`:i8` (`:void t` is the legacy spelling of `:result :void`); more than seven registers fall back to a `defun` over `amiga:call-library`; `:doc` is the docstring.  The function cell receives an *FFI stub* (a compact binding descriptor — a function for every CL purpose; `(ffi::%ffi-stub-info #'name)` lists its fields); a direct call compiles to the library-call opcode in the caller |
| `*defcfun-docstrings*` | variable | When NIL, `defcfun` drops its `:doc` string (default T); `scripts/compile-lib-fasls.sh --no-docstrings` — i.e. `make fasl-amiga` and the binary release — binds it for the Amiga FASLs to save heap on the target |
| `(define-binding-table package (&key base version) &body rows)` | macro | A whole module's bindings as one packed table attached to `package`, materialised on first reference: `(:const "NAME" value)`, `(:var "NAME" value)` (`value` an integer of any size or an ASCII string of at most 255 characters — `mui.h`'s `MUIC_Window "Window.mui"`), `(:fn "NAME" lvo (:a0 …) :result [:not-morphos\|:morphos] [min-version])` (a `defcfun`), `(:struct "NAME" size ("FIELD" type offset) …)` (a `defcstruct`), `(:field "NAME" type offset)`, `(:name "NAME")` (export only); `base`/`version` name the library-base and `lib_Version` variables the `:fn` rows and version guards use.  Names are strings taken literally.  `(amilisp::%binding-table-info pkg)` / `(amilisp::%binding-table-entries pkg)` inspect a table |

The generated modules under `lib/amiga/raw/` (`(require "amiga/raw/<lib>")`,
packages `AMIGA.RAW.<LIB>`) are built on these: one `define-binding-table`
per module holding every OS function, constant and struct of the NDK
includes, so `(require "amiga/raw/intuition")` costs the table bytes plus
the names a program actually touches — see *Raw OS bindings* in the main
README.

---

## `AMIGA.EXEC` — Memory introspection, chip RAM, device I/O

```lisp
(require "amiga/exec")
(amiga.exec:avail-mem amiga.exec:+memf-chip+)   ; free chip RAM in bytes
```

| Signature | Kind | Description |
|-----------|------|-------------|
| `(avail-mem &optional (requirements +memf-any+))` | function | Free system memory in bytes — exec.library AvailMem; `requirements` is a `MEMF_*` mask (`+memf-largest+` ORed in asks for the largest single free block, `+memf-total+` for the pool's total size) |
| `(alloc-chip-bytes bytes)` | function | Copy the `(unsigned-byte 8)` vector `bytes` into a fresh chip-RAM allocation (blitter masks, sprites, audio samples); returns the chip foreign pointer, freed with `amiga:free-chip` |
| `*exec-base*` | variable | ExecBase foreign pointer (read from absolute address 4) |
| `(create-msg-port)` / `(delete-msg-port port)` | function | A reply port as a foreign pointer (or `NIL`) / free it |
| `(create-io-request port size)` / `(delete-io-request io)` | function | A zeroed `size`-byte I/O request replying to `port` (or `NIL`) / free it |
| `(open-device name unit io &optional (flags 0))` / `(close-device io)` | function | OpenDevice by name (a Lisp string) on the request; returns the `io_Error` code, 0 on success / CloseDevice |
| `(send-io io)` / `(check-io io)` / `(wait-io io)` / `(abort-io io)` / `(do-io io)` | function | The asynchronous quartet — start a request, true once it completed, take it back (returns `io_Error`), ask the device to abort it — and the synchronous DoIO |
| `(io-request-device io)` / `(io-request-error io)` | function | The device base OpenDevice stored in the request (a foreign pointer — ahi.device's functions are called through it) / its `io_Error` byte |

`AMIGA.AUDIO` and `AMIGA.AHI` are the device-I/O calls in use, end to end.

- **Constants:** `MEMF_*` requirement/option flags `+memf-any+`, `+memf-public+`,
  `+memf-chip+`, `+memf-fast+`, `+memf-clear+`, `+memf-largest+`, `+memf-total+`.

---

## `AMIGA.INTUITION` — Windows, screens, events

```lisp
(require "amiga/intuition")
(require "amiga/graphics")

(amiga.intuition:with-window (win :title "Hello Amiga"
                                  :width 320 :height 200
                                  :idcmp amiga.intuition:+idcmp-closewindow+)
  (let ((rp (amiga.intuition:window-rastport win)))
    (amiga.gfx:set-a-pen rp 1)
    (amiga.gfx:move-to rp 20 40)
    (amiga.gfx:gfx-text rp "Hello from AmiLisp!"))
  (amiga.intuition:event-loop win
    (#.amiga.intuition:+idcmp-closewindow+ (msg) (return))))
```

### Windows

| Signature | Kind | Description |
|-----------|------|-------------|
| `(open-window &key (title "AmiLisp") (left 0) (top 0) (width 320) (height 200) screen (idcmp +idcmp-closewindow+) flags)` | function | Open an Intuition window via OpenWindowTagList; `title` `NIL` opens an untitled window; `screen` is a custom screen pointer; `flags` defaults to close gadget + drag bar + depth gadget + size gadget + activate |
| `(close-window window)` | function | Close a window from `open-window` |
| `(with-window (var &rest args) &body body)` | macro | Open a window (`args` as for `open-window`), bind to `var`, close on exit |
| `(window-rastport window)` | function | RastPort pointer of the window |
| `(window-width window)` | function | Window width in pixels |
| `(window-height window)` | function | Window height in pixels |
| `(window-left window)` | function | Window left edge |
| `(window-top window)` | function | Window top edge |
| `(window-title window)` | function | Window title pointer |
| `(window-user-port window)` | function | The UserPort (IDCMP message port) of the window |
| `(window-border-left window)` | function | Pixels reserved at the left border |
| `(window-border-top window)` | function | Pixels reserved at the top border (incl. title bar) |
| `(window-border-right window)` | function | Pixels reserved at the right border |
| `(window-border-bottom window)` | function | Pixels reserved at the bottom border |
| `(window-gzz-width window)` | function | Inner width (valid with `+wflg-gimmezerozero+`) |
| `(window-gzz-height window)` | function | Inner height (valid with `+wflg-gimmezerozero+`) |

### Screens

| Signature | Kind | Description |
|-----------|------|-------------|
| `(open-screen &key (width 640) (height 256) (depth 2) (title "AmiLisp") mode-id show-title)` | function | Open a custom screen via OpenScreenTagList; `mode-id` non-`NIL` requests that display mode (get one RTG-safely from `amiga.gfx:best-mode-id`); `show-title` non-`NIL` shows the title bar |
| `(close-screen screen)` | function | Close a screen from `open-screen` |
| `(with-screen (var &rest args) &body body)` | macro | Open a screen (`args` as for `open-screen`), bind to `var`, close on exit |
| `(show-title screen show-it)` | function | ShowTitle: put the screen's title bar in front of (`show-it` non-`NIL`) or behind (`NIL`) backdrop windows |
| `(screen-width screen)` | function | Screen width in pixels |
| `(screen-height screen)` | function | Screen height in pixels |
| `(screen-bar-height screen)` | function | Height of the screen's title bar |
| `(screen-viewport screen)` | function | Pointer to the screen's embedded ViewPort — what `amiga.gfx:set-rgb4` wants |

### Public screens

| Signature | Kind | Description |
|-----------|------|-------------|
| `(lock-pub-screen &optional name)` | function | Lock a public screen by name (`NIL` = default/Workbench); returns a Screen pointer or `NIL` |
| `(unlock-pub-screen screen &optional name)` | function | Unlock a previously locked public screen |
| `(with-pub-screen (var &optional name) &body body)` | macro | Lock a public screen, bind to `var`, unlock on exit |

### Display dimensions

How large a display actually is belongs to the display *mode*, not to
the screen you ask for: PAL and NTSC differ by 56 rows, and an RTG mode
is whatever its driver says.  Ask, and a program can open a screen the
size of the machine it landed on instead of a size it guessed — which
is what keeps a fixed-layout program from being letterboxed or
rescaled.  Pair these with `amiga.gfx:best-mode-id`.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(query-overscan mode-id &optional oscan-type)` | function | `QueryOverscan`: the overscan rectangle of display mode `mode-id` as `(values min-x min-y max-x max-y)`; `NIL` if the display database does not know the mode |
| `(display-mode-height mode-id &optional oscan-type)` | function | Rows that mode can show — 256 on PAL, 200 on NTSC, the driver's own on RTG; `NIL` for an unknown mode |
| `+oscan-text+` | constant | Region every monitor of that mode is guaranteed to show (the default) |
| `+oscan-standard+` | constant | The mode's nominal display clip |
| `+oscan-max+` | constant | As much as the mode can show |
| `+oscan-video+` | constant | Beyond the visible edges |

```lisp
(let* ((mode (amiga.gfx:best-mode-id :width 320 :height 200 :depth 5))
       (rows (and mode (amiga.intuition:display-mode-height mode))))
  ;; rows is 256 on a PAL machine, 200 on NTSC
  (amiga.intuition:with-screen (scr :width 320 :height (or rows 200)
                                    :depth 5 :mode-id mode)
    ...))
```

### IDCMP events

| Signature | Kind | Description |
|-----------|------|-------------|
| `(get-msg port)` | function | Next message from `port`, or `NIL` if none available |
| `(reply-msg msg)` | function | Reply to a received message |
| `(wait-port port)` | function | Block until a message arrives on `port` |
| `(msg-class msg)` | function | IDCMP class from an IntuiMessage |
| `(msg-code msg)` | function | Code field from an IntuiMessage |
| `(msg-qualifier msg)` | function | Qualifier field from an IntuiMessage (`+iequalifier-*+` bits) |
| `(msg-mouse-x msg)` | function | MouseX from an IntuiMessage |
| `(msg-mouse-y msg)` | function | MouseY from an IntuiMessage |
| `(event-loop window &body clauses)` | macro | Process IDCMP messages for `window` until a clause calls `(return)`; each clause is `(idcmp-class (msg) &body body)` |
| `*event-loop-max-waits*` | variable | When non-`NIL`, bound `event-loop`'s wait to this many `get-msg` polls instead of blocking forever (for unattended test runs) |

### Mouse pointer

| Signature | Kind | Description |
|-----------|------|-------------|
| `(set-pointer window sprite-data height width x-offset y-offset)` | function | SetPointer: show a custom pointer while `window` is active; `sprite-data` is chip RAM in hardware sprite layout, `width` at most 16, `x-offset`/`y-offset` place the hot spot |
| `(clear-pointer window)` | function | Restore the window's default mouse pointer |
| `(make-pointer-sprite rows)` | function | Build the chip-RAM sprite data `set-pointer` wants from `rows`, a list of `(word-a word-b)` pairs, one per pointer line; returns `(values chip height)`; caller frees with `amiga:free-chip` |

### Menus / gadgets on a window

| Signature | Kind | Description |
|-----------|------|-------------|
| `(set-menu-strip window menu)` | function | Attach a menu strip to a window |
| `(clear-menu-strip window)` | function | Remove the menu strip from a window |
| `(add-gadget-list window gadget-list)` | function | Add a gadget list to a window; returns position |
| `(refresh-gadget-list window gadget-list)` | function | Refresh all gadgets in the list |

- **Library base:** `*intuition-base*`.
- **Constants:** IDCMP class flags `+idcmp-closewindow+`, `+idcmp-gadgetup+`,
  `+idcmp-gadgetdown+`, `+idcmp-mousebuttons+`, `+idcmp-mousemove+`,
  `+idcmp-rawkey+`, `+idcmp-menupick+`, `+idcmp-refreshwindow+`,
  `+idcmp-newsize+`, `+idcmp-vanillakey+`, `+idcmp-activewindow+`,
  `+idcmp-inactivewindow+`, `+idcmp-intuiticks+`; mouse button codes
  `+selectdown+`, `+selectup+`, `+menudown+`, `+menuup+`
  (`+idcmp-mousebuttons+` `msg-code` values); input qualifier bits
  `+iequalifier-lshift+`, `+iequalifier-rshift+`, `+iequalifier-capslock+`
  (`msg-qualifier` values); window flags `+wflg-*+`
  (`+wflg-closegadget+`, `+wflg-dragbar+`, `+wflg-depthgadget+`,
  `+wflg-sizegadget+`, `+wflg-activate+`, `+wflg-smart-refresh+`,
  `+wflg-simple-refresh+`, `+wflg-backdrop+`, `+wflg-borderless+`,
  `+wflg-gimmezerozero+`, `+wflg-reportmouse+`, `+wflg-rmbtrap+`); window tags
  `+wa-*+` (`+wa-left+`, `+wa-top+`, `+wa-width+`, `+wa-height+`, `+wa-title+`,
  `+wa-idcmp+`, `+wa-flags+`, `+wa-customscreen+`, `+wa-gadgets+`).

---

## `AMIGA.GFX` — Drawing

### Drawing primitives

| Signature | Kind | Description |
|-----------|------|-------------|
| `(move-to rastport x y)` | function | Move the pen to `(x,y)` |
| `(draw-to rastport x y)` | function | Draw a line from the current pen position to `(x,y)` |
| `(draw-line rastport x1 y1 x2 y2)` | function | Draw a line from `(x1,y1)` to `(x2,y2)` |
| `(rect-fill rastport x-min y-min x-max y-max)` | function | Fill the rectangle between the two corners |
| `(draw-ellipse rastport cx cy rx ry)` | function | Draw an ellipse centered at `(cx,cy)` with radii `rx`/`ry` |
| `(set-a-pen rastport pen)` | function | Set the foreground (A) pen |
| `(set-b-pen rastport pen)` | function | Set the background (B) pen |
| `(set-drmd rastport mode)` | function | Set the draw mode (`+jam1+`, `+jam2+`, `+complement+`, `+inversvid+`) |

### Text & fonts

| Signature | Kind | Description |
|-----------|------|-------------|
| `(gfx-text rastport string)` | function | Render `string` at the current pen position |
| `(text-length rastport string)` | function | Pixel width of `string` in the rastport's font |
| `(open-font name ysize)` | function | OpenFont: the ROM font `name` (e.g. `"topaz.font"`) at `ysize` pixels; returns a TextFont pointer or `NIL`; close with `close-font` |
| `(close-font font)` | function | Close a font from `open-font` |
| `(set-font rastport font)` | function | Select `font` in `rastport` |

### Display database & palette

| Signature | Kind | Description |
|-----------|------|-------------|
| `(best-mode-id &key (width 640) (height 256) (depth 2))` | function | Ask the display database (BestModeIDA) for the mode that best fits — the RTG-safe way to pick a screen mode; returns the mode ID or `NIL` |
| `(set-rgb4 viewport index red green blue)` | function | Set palette entry `index` (4-bit color components) |
| `(get-rgb4 colormap entry)` | function | Read a palette entry as packed nibbles `#x0RGB`; -1 for an entry outside the map |
| `(viewport-color-map viewport)` | function | Pointer to the viewport's ColorMap — what `get-rgb4` wants |

### Bitmaps & blits (RTG-safe)

All through OS calls: bitmaps from AllocBitMap (pass a friend bitmap so RTG
systems allocate their native format), chunky pixels via WriteChunkyPixels
(V40+; WritePixel fallback on V39), copies via BltBitMapRastPort.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(gfx-version)` | function | graphics.library version; WriteChunkyPixels needs 40+ |
| `(alloc-bitmap width height depth &key (flags +bmf-clear+) friend)` | function | AllocBitMap: an offscreen bitmap, cleared by default; pass the screen's or window's bitmap as `friend` for RTG-native format; signals on failure |
| `(free-bitmap bitmap)` | function | Free a bitmap from `alloc-bitmap` |
| `(with-bitmap (var width height depth &rest keys) &body body)` | macro | Allocate a bitmap (`keys` as for `alloc-bitmap`), bind to `var`, free on exit |
| `(get-bitmap-attr bitmap attribute)` | function | GetBitMapAttr: query `+bma-width+`/`+bma-height+`/`+bma-depth+`/`+bma-flags+` |
| `(init-rastport rastport)` | function | InitRastPort on a raw RastPort allocation |
| `(with-bitmap-rastport (var bitmap) &body body)` | macro | A scratch RastPort rendering into `bitmap`: allocated, InitRastPort'd and pointed at the bitmap; freed on exit |
| `(rastport-bitmap rastport)` | function | Pointer to the BitMap the rastport renders into (rp_BitMap) — the natural `alloc-bitmap` `:friend` |
| `(write-chunky rastport x y width height pens)` | function | Write the `(unsigned-byte 8)` vector `pens` (row-major `width` x `height` pen indices) into `rastport` at `(x,y)` |
| `(write-pixel rastport x y)` | function | WritePixel with the foreground pen |
| `(read-pixel rastport x y)` | function | ReadPixel: the pen number at `(x,y)` |
| `(blt-bitmap-rastport src-bitmap src-x src-y dest-rastport dest-x dest-y width height &optional (minterm +minterm-copy+))` | function | BltBitMapRastPort: copy a `width` x `height` region into the destination rastport |
| `(blt-mask-bitmap-rastport src-bitmap src-x src-y dest-rastport dest-x dest-y width height mask &optional (minterm +minterm-cookie+))` | function | BltMaskBitMapRastPort: like above but cookie-cut through `mask`, a single interleaved bitplane in chip RAM with a 1 bit per pixel to copy |
| `*write-chunky-force-fallback*` | variable | Non-`NIL` forces `write-chunky` onto the V39 per-pixel WritePixel path (for exercising the fallback) |

### Planar bitmap access

Legal only on a standard planar BitMap: one from `alloc-bitmap` with no
`:friend` and without `+bmf-displayable+`. Pour plane rows in with
`write-planes`, then `blt-bitmap-rastport` into the friend-format destination.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(bitmap-bytes-per-row bitmap)` | function | struct BitMap BytesPerRow |
| `(bitmap-rows bitmap)` | function | struct BitMap Rows |
| `(bitmap-flags bitmap)` | function | struct BitMap Flags |
| `(bitmap-depth bitmap)` | function | struct BitMap Depth |
| `(bitmap-plane bitmap n)` | function | Pointer to the bitmap's `n`th bitplane (bm_Planes[n]) |
| `(write-planes bitmap planes src-row-bytes height)` | function | Copy planar pixel data into `bitmap`: `planes` is a list of `(unsigned-byte 8)` vectors, one per bitplane, each `height` rows of `src-row-bytes` bytes (ILBM BODY plane layout) |

### RastPort accessors

| Signature | Kind | Description |
|-----------|------|-------------|
| `(rastport-fgpen rastport)` | function | Current foreground pen |
| `(rastport-bgpen rastport)` | function | Current background pen |
| `(rastport-cp-x rastport)` | function | Current pen X position |
| `(rastport-cp-y rastport)` | function | Current pen Y position |
| `(rastport-tx-height rastport)` | function | Current font height in pixels (rp_TxHeight) |
| `(rastport-tx-baseline rastport)` | function | Baseline offset from glyph top (rp_TxBaseline) |

- **Library base:** `*gfx-base*`.
- **Constants:** draw modes `+jam1+`, `+jam2+`, `+complement+`, `+inversvid+`;
  bitmap flags `+bmf-clear+`, `+bmf-displayable+`, `+bmf-interleaved+`,
  `+bmf-standard+`, `+bmf-minplanes+`; GetBitMapAttr attributes `+bma-width+`,
  `+bma-height+`, `+bma-depth+`, `+bma-flags+`; blit minterms `+minterm-copy+`,
  `+minterm-cookie+`.

---

## `AMIGA.GADTOOLS` — Gadgets & menus

```lisp
(require "amiga/gadtools")

(amiga.intuition:with-pub-screen (scr)
  (amiga.gadtools:with-visual-info (vi scr)
    (amiga.gadtools:with-gadgets (glist ctx vi)
      (amiga.gadtools:create-gadget
        amiga.gadtools:+button-kind+ ctx vi
        :left 20 :top 30 :width 120 :height 16
        :text "Click Me" :gadget-id 1)
      ;; … open a window, add the gadget list, run an event loop …
      )))
```

### VisualInfo

| Signature | Kind | Description |
|-----------|------|-------------|
| `(get-visual-info screen &rest tags)` | function | Get VisualInfo for a screen; returns a foreign pointer |
| `(free-visual-info vi)` | function | Free a VisualInfo from `get-visual-info` |
| `(with-visual-info (var screen) &body body)` | macro | Get VisualInfo for `screen`, bind to `var`, free on exit |

### Gadgets

| Signature | Kind | Description |
|-----------|------|-------------|
| `(create-context glist-ptr)` | function | Create a gadget context (dummy head gadget); `glist-ptr` is a foreign pointer to a zero-initialized Gadget* |
| `(create-gadget kind previous-gadget visual-info &key (left 0) (top 0) (width 80) (height 14) text text-attr (gadget-id 0) (flags +placetext-in+) (user-data 0) tags)` | function | Create a GadTools gadget of `kind` (e.g. `+button-kind+`); `tags` is an optional flat list of additional tag/value pairs |
| `(free-gadgets gadget-list)` | function | Free all gadgets in a gadget list |
| `(with-gadgets (glist-var context-var visual-info) &body body)` | macro | Create a gadget context, bind list pointer to `glist-var` and context to `context-var`, free all gadgets on exit |
| `(set-gadget-attrs gadget window &rest tags)` | function | Modify gadget attributes; `tags` is a flat list of tag/value pairs |

### Menus

| Signature | Kind | Description |
|-----------|------|-------------|
| `(make-new-menu-array entries)` | function | Build a foreign NewMenu array from menu specs — each entry is `(type label &key commkey flags userdata)` or `:bar` for a separator; returns `(values pointer strings)` (caller frees both) |
| `(create-menus new-menu-array &rest tags)` | function | Create a menu strip from a NewMenu array; returns a Menu pointer |
| `(layout-menus menu visual-info &rest tags)` | function | Lay out menus for display; call after `create-menus` |
| `(free-menus menu)` | function | Free a menu strip |
| `(with-menus (var entries visual-info &optional window) &body body)` | macro | Create menus from `entries`, lay out with `visual-info`, optionally attach to `window`, clean up on exit |

### Messages & refresh

| Signature | Kind | Description |
|-----------|------|-------------|
| `(gt-get-msg port)` | function | Next IDCMP message via GadTools (handles gadget-specific processing), or `NIL` |
| `(gt-reply-msg msg)` | function | Reply to a GadTools-processed message |
| `(gt-refresh-window window)` | function | Refresh GadTools gadgets after window resize/reveal |
| `(gt-begin-refresh window)` | function | Begin optimized refresh |
| `(gt-end-refresh window &optional (complete t))` | function | End optimized refresh |

### Bevel box

| Signature | Kind | Description |
|-----------|------|-------------|
| `(draw-bevel-box rastport left top width height &key visual-info recessed)` | function | Draw a bevel box on a RastPort |

- **Library base:** `*gadtools-base*`.
- **Gadget kinds:** `+button-kind+`, `+checkbox-kind+`, `+integer-kind+`,
  `+listview-kind+`, `+mx-kind+`, `+number-kind+`, `+cycle-kind+`,
  `+palette-kind+`, `+scroller-kind+`, `+slider-kind+`, `+string-kind+`,
  `+text-kind+`.
- **NewGadget flags:** `+placetext-left/right/above/below/in+`, `+ng-highlabel+`.
- **Per-gadget tags:** the `+gtst-*+`, `+gtin-*+`, `+gtcb-*+`, `+gtcy-*+`,
  `+gtlv-*+`, `+gtsl-*+`, `+gtsc-*+`, `+gtmx-*+`, `+gttx-*+`, `+gtnm-*+`,
  `+gtbb-recessed+`, `+gtmn-new-look-menus+`, `+gt-visual-info+`,
  `+gt-underscore+` families (see the `:export` list in
  `lib/amiga/gadtools.lisp` for the full set).
- **NewMenu constants:** `+nm-end+`, `+nm-title+`, `+nm-item+`, `+nm-sub+`,
  `+nm-barlabel+`.
- **Per-kind IDCMP masks:** `+buttonidcmp+`, `+checkboxidcmp+`, `+integeridcmp+`,
  `+stringidcmp+`, `+cycleidcmp+`, `+mxidcmp+`, `+listviewidcmp+`,
  `+scrolleridcmp+`, `+slideridcmp+`.

---

## `AMIGA.AUDIO` — Sample playback

Allocates a Paula channel through audio.device and plays 8-bit signed samples
from chip RAM. Playback is strictly non-blocking: no call here stalls the
caller for the duration of a sample. See `tests/amiga/test-audio.lisp` for
usage end-to-end.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(open-audio &key (precedence 0))` | function | Allocate one Paula channel and return an audio handle, or `NIL` if no channel is available; `precedence` (-128..127) is the allocation priority |
| `(close-audio audio)` | function | Stop playback, free the channel and all request plumbing |
| `(with-audio (var &rest open-args) &body body)` | macro | Open an audio channel (`open-args` as for `open-audio`), bind the handle to `var`, close on exit; signals when no channel is available |
| `(audio-channel-mask audio)` | function | The Paula channel mask (1, 2, 4 or 8) OpenDevice allocated |
| `(play-sample audio chip-data length &key (period 443) (volume +max-volume+) (cycles 1))` | function | Start `length` bytes of signed 8-bit sample data at `chip-data` (a chip-RAM pointer, e.g. from `amiga.exec:alloc-chip-bytes`); `period` is the Paula period (see `period-for-rate`), `volume` 0..64, `cycles` the repeat count (0 loops until `stop-sample`); returns immediately, `T` when queued |
| `(stop-sample audio)` | function | Silence the channel: abort any in-flight write |
| `(playing-p audio)` | function | True while the last `play-sample` is still sounding |
| `(period-for-rate rate)` | function | Paula period for a sample `rate` in Hz (PAL clock) |

- **Constants:** `+max-volume+` (64), `+max-sample-bytes+` (131072), `+max-precedence+` (127 — the highest allocation precedence; a channel held at it cannot be stolen, and on m68k `open-audio` always allocates at it).

---

## `AMIGA.AHI` — Sound through AHI (opt-in)

Playback through ahi.device, the AHI audio system (an Aminet install on
AmigaOS 3.x, built into MorphOS): 8- or 16-bit, mono or stereo samples at
any sample rate from any memory (no chip RAM), mixed onto whatever the
user configured — Paula, a sound card, a PPC machine's audio.  Strictly
**opt-in**: only `(require "amiga/ahi")` touches ahi.device, and
`AMIGA.AUDIO` stays the Paula module, independent of this one.  Two tiers:
the *device interface* (a handle on one of the user's AHI units, one sound
at a time, non-blocking, double-buffered) and the *low-level API* (AHI's
own mixer, several sounds on several channels, driven from the program —
never through AHI's SoundFunc/PlayerFunc hooks, which run in interrupt
context).  See `tests/amiga/test-ahi.lisp` for usage end-to-end and
`examples/amiga/audio/ahi-play.lisp` for a program.

The device's functions are called library-style through the base
`OpenDevice` leaves in the request: `open-ahi` stores it in
`amiga.raw.ahi:*ahi-base*`, arming the generated `AMIGA.RAW.AHI` table
until the last handle closes.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(open-ahi &key (unit +ahi-default-unit+) (version 4))` | function | Open ahi.device and return a handle, or `NIL` when AHI is absent or the unit will not open; `unit` 0..3 (the user's AHI preferences units) or `:none` for the function interface alone (no playback) |
| `(close-ahi ahi)` | function | Stop playback, close the unit, free the requests and the port; the last close disarms the raw table |
| `(with-ahi (var &rest open-args) &body body)` | macro | `open-ahi`, bind the handle, close on exit; signals when it cannot open |
| `(ahi-open-p ahi)` / `(ahi-unit ahi)` | function | Whether the handle is open / the unit number it opened |
| `(play-sample ahi data length &key (type :mono8) (rate 8000) (volume 1.0) (pan 0.5) (priority 0))` | function | Start `length` bytes at `data` (a foreign pointer, see `make-sample-buffer`) on the unit, cutting off what it was playing; `type` `:mono8` `:mono16` `:stereo8` `:stereo16`, `rate` Hz, `volume` 0..1, `pan` 0 (left)..1 (right); returns immediately, `T` when the device took the request |
| `(queue-sample ahi data length &key …)` | function | Like `play-sample` but *after* the sound in flight, gapless (linked through `ahir_Link`); at most two in flight — `NIL` when both buffers are busy |
| `(stop-sample ahi)` | function | Abort what is in flight (queued too) and take the requests back |
| `(playing-p ahi)` | function | True while a sample is still sounding; reclaims finished requests |
| `(sample-bytes samples &key (bits 8))` | function | Pack a vector of signed sample values (8-bit -128..127, or 16-bit -32768..32767 big-endian; stereo interleaved by the caller) into the `(unsigned-byte 8)` bytes AHI plays |
| `(make-sample-buffer bytes)` | function | Copy such a byte vector into a fresh foreign buffer; free with `ffi:free-foreign` once nothing refers to it |
| `(sample-frame-size type)` / `(sample-type-code type)` | function | Bytes per frame (1, 2, 2, 4) / the `AHIST_*` code of a sample type |
| `(audio-modes)` | function | Every audio mode ID in AHI's database |
| `(audio-mode-name id)` / `(audio-mode-info id)` | function | A mode's name / a plist: `:name :driver :bits :max-channels :min-mix-freq :max-mix-freq :stereo :panning :hifi :volume :realtime :record` |
| `(best-audio-mode &key stereo panning hifi volume record (realtime t) bits max-channels)` | function | The mode `AHI_BestAudioIDA` picks for the requirements (a boolean key given demands or forbids the feature), or `NIL` |
| `(alloc-audio &key (audio-id +ahi-default-id+) mix-freq (channels 4) (sounds 8))` | function | AHI's mixer on a mode: an `AHIAudioCtrl` foreign pointer, or `NIL`; silent until `start-playback`.  From a handle opened with `:unit :none` — a unit open through the device interface holds the hardware (on Paula all of it) and the allocation then fails |
| `(free-audio ctrl)` / `(with-audio-ctrl (var &rest alloc-args) &body body)` | function / macro | Release the mixer / allocate, bind, free on exit |
| `(load-sound ctrl sound data frames &key (type :mono8))` | function | Make the `frames` sample frames at `data` sound number `sound`; `T`, or `NIL` and the `AHIE_*` code |
| `(unload-sound ctrl sound)` | function | Forget a sound |
| `(start-playback ctrl)` / `(stop-playback ctrl)` | function | Start / stop the mixer (`AHIC_Play`) |
| `(play ctrl channel &key sound offset length frequency volume pan loop-sound loop-offset loop-length loop-frequency loop-volume loop-pan)` | function | `AHI_PlayA`: set a channel up in one go — `sound` (`NIL` silences it) from `offset` for `length` frames at `frequency` Hz, `volume` / `pan`, and what loops after it |
| `(set-volume ctrl channel volume &key (pan 0.5) immediate)` / `(set-frequency ctrl channel hz &key immediate)` / `(set-sound ctrl channel sound &key offset length immediate)` | function | The single-attribute calls (`AHI_SetVol` / `AHI_SetFreq` / `AHI_SetSound`) |
| `(to-fixed x)` / `(from-fixed n)` | function | AHI's 16.16 `Fixed` from / to a real |

- **Constants:** `+ahi-default-unit+` (0), `+ahi-no-unit+` (255).

---

## `AMIGA.BOOPSI` — toolkit-neutral BOOPSI helpers

What every BOOPSI object needs from the Lisp side regardless of the
toolkit that created it — ReAction's class libraries, MUI's classes, or
intuition's built-in `buttongclass` / `propgclass` / `icclass` that every
AmigaOS since 2.0 has: memory that lives as long as the objects holding
pointers to it, tag lists from Lisp values, `DoMethod()`, `GetAttr` /
`SetAttrsA`, `NewList()`.  Object creation and disposal belong to the
toolkit modules (`AMIGA.REACTION`, `AMIGA.MUI`), which `:use` this package
and re-export its names (`AMIGA.MUI` all but the exec-list pair, which
mean nothing to MUI), so `amiga.reaction:with-foreign-pool`,
`amiga.mui:with-foreign-pool` and `amiga.boopsi:with-foreign-pool` are one
symbol.  The module loads on
every system (the host included); `tests/amiga/test-boopsi.lisp` drives it
against the built-in intuition classes with no toolkit installed, and
`tests/test_amiga_boopsi.sh` checks the portable half on the host.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(with-foreign-pool () &body body)` | macro | Run `body` with a foreign pool; every `pool-alloc`/`pool-string`/`pool-hook`/`new-list` made inside — also the string tag values of `with-tags`, `set-attrs` and the toolkit's `new-object`, `set-gadget-attrs`, `open-requester` — is freed on exit, normally or not, in reverse order of creation.  Wrap the life of a GUI in one and dispose the objects inside `body`: what the pool holds must outlive them |
| `(pool-alloc size)` | function | Zeroed foreign memory living until the pool exits |
| `(pool-string string)` | function | A NUL-terminated foreign copy of `string` living until the pool exits |
| `(pool-hook function &key data)` | function | `amiga.ffi:make-hook` with the pool's lifetime — what a `MUIA_List_DisplayHook`, `MUIM_CallHook`, `LISTBROWSER_*Hook` value should be; freed after `body` has disposed the objects |
| `(pool-finalizer function)` | function | Call `function` (no arguments) when the pool exits, in its turn among the entries — for GUI-lifetime resources that are not plain memory (`create-custom-class` uses it) |
| `(new-list)` | function | A fresh, initialised exec `struct List` (pooled) — the label list of a chooser / clicktab / listbrowser, filled with `amiga.raw.exec:add-tail` |
| `(free-list-nodes list free-node)` | function | `RemHead` every node of `list` and call `free-node` on it (e.g. `amiga.raw.gadgets.chooser:free-chooser-node`); returns the count |
| `(with-tags (var &rest tags) &body body)` | macro | Bind `var` to a TagItem array built from the `(tag value ...)` plist for `body` — for library functions that take a tag list themselves (`alloc-list-browser-node-a`, `alloc-chooser-node-a`, `new-object-a` of a built-in class, …); values may be integers, foreign pointers, `T`/`NIL` or strings (pooled, since the receiver keeps the pointer); the array is freed on exit |
| `(object-class object)` | function | `OCLASS(object)`: the object's IClass as a foreign pointer |
| `(do-method object method-id &rest args)` | function | `IDoMethodA`: invoke a method on a BOOPSI object — `CallHookPkt` on the object's class dispatcher; `args` are the message longwords after the MethodID (integers, foreign pointers, `T`/`NIL`), so `(do-method obj +om-get+ attr storage)` is an `opGet`; returns d0 as an unsigned integer |
| `(get-attr attribute object)` | function | `GetAttr`: the value as an unsigned integer, `NIL` if the object does not know the attribute |
| `(get-attr-pointer attribute object)` | function | `get-attr` for a pointer-valued attribute (`WINDOW_Window`, `LISTBROWSER_SelectedNode` …): a foreign pointer or `NIL` |
| `(set-attrs object &rest tags)` | function | `SetAttrsA` with `with-tags`' value rules; returns the class's result |

---

## `AMIGA.REACTION` — ReAction helpers

What reaction.lib and the `RA_*` macros give a C ReAction program on top
of the generated class modules (`amiga/raw/classes/window`,
`amiga/raw/gadgets/*`, `amiga/raw/images/*`, `amiga/raw/classes/requester`),
which supply the tags, method IDs and class functions, and of
`AMIGA.BOOPSI` above, whose names this package re-exports (`do-method`,
`object-class`, `with-foreign-pool`, `pool-alloc`, `pool-string`,
`new-list`, `free-list-nodes`, `with-tags`, `get-attr`, `get-attr-pointer`,
`set-attrs` — see that table).  The module loads on every system (the host
included); the classes themselves exist on AmigaOS 3.5+/3.2 and MorphOS.
See the [ReAction](../README.md#reaction-amigaos-3532-morphos) section of
the main README, the ports of the NDK examples under
`examples/amiga/reaction/`, and `tests/amiga/test-reaction.lisp` /
`tests/test_amiga_reaction.sh` for usage end to end.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(available-p)` | function | True when the ReAction classes can be opened here (window.class opens); `NIL` on the host and on an AmigaOS without ReAction |
| `(new-object class &rest tags)` | function | `NewObjectA(class, NULL, tags)`: `class` is what a class module's `xxx-get-class` returns; tag values may be integers, foreign pointers, `T`/`NIL` or strings (pooled, the object keeps the pointer).  Returns the object; signals when the class returns NULL |
| `(dispose-object object)` | function | `DisposeObject` — a window or layout object takes everything attached to it along; `NIL` is ignored |
| `(set-gadget-attrs gadget window &rest tags)` | function | `SetGadgetAttrsA(gadget, window, NULL, tags)` — change and refresh a displayed gadget (`window` may be `NIL`) |
| `(open-window window-object)` | function | `RA_OpenWindow` (`WM_OPEN`): the `struct Window` as a foreign pointer, or `NIL` |
| `(close-window window-object)` | function | `RA_CloseWindow` (`WM_CLOSE`) — the object survives |
| `(iconify window-object)` | function | `RA_Iconify` (`WM_ICONIFY`); needs `WINDOW_IconifyGadget` / `WINDOW_AppPort`; true on success |
| `(handle-input window-object)` | function | `RA_HandleInput` (`WM_HANDLEINPUT`): two values, the `WMHI_*` result (class in `WMHI_CLASSMASK`, gadget id in `WMHI_GADGETMASK`, `WMHI_LASTMSG` = 0 when nothing is pending) and the message Code word |
| `(window-signal-mask window-object)` | function | `WINDOW_SigMask`, 0 while the window is closed |
| `(do-window-events ((result code) window-object &key timeout signals) &body body)` | macro | The event loop: `Wait()` on the window's signals (+ `SIGBREAKF_CTRL_C` + `signals`), run `body` per message with `result`/`code` as `handle-input` returns them; `(return)` in `body` or Ctrl-C leaves.  With `timeout` (seconds, default `*event-loop-timeout*`) it polls and returns when the time is up |
| `*event-loop-timeout*` | variable | Default `:timeout` of `do-window-events`; `NIL` = interactive.  Set it before loading a GUI program to run it unattended |
| `(open-requester requester window &rest tags)` | function | requester.class `RM_OPENREQ` over `window` (or `NIL`) with the `REQ_*`/`REQS_*`/`REQI_*` attributes in `tags`; returns the gadget number chosen (1 = leftmost, 0 = rightmost / cancel) |

---

## `AMIGA.MUI` — MUI helpers

What `mui.h`'s shortcut macros and amiga.lib give a C MUI program on top
of muimaster.library (MUI 3.8+ on AmigaOS 3.x, built into MorphOS) and
of the generated `amiga/raw/muimaster` module, which supplies the 26
library functions and every `MUIA_` / `MUIM_` / `MUIV_` / `MUII_` /
`MUIC_` / `MUIX_` constant of `libraries/mui.h`.  The toolkit-neutral
half is `AMIGA.BOOPSI` above, re-exported (`do-method`, `object-class`,
`with-foreign-pool`, `pool-alloc`, `pool-string`, `with-tags`, `get-attr`,
`get-attr-pointer`, `set-attrs` — see that table).  The module loads on
every system and opens muimaster.library on first use; the raw module
needs it at `require` time.  See the [MUI](../README.md#mui-amigaos-3x-with-mui-38-morphos)
section of the main README, `examples/amiga/mui/`, and
`tests/amiga/test-mui.lisp` / `tests/test_amiga_mui.sh` for usage end to
end.  Hooks are `pool-hook` (above, re-exported); custom classes are
below.  Both run Lisp on MUI's stack: an error there is caught at the
callback, the method or hook returns 0, and the condition is re-signaled
when the call into MUI that dispatched it returns (`new-object`,
`set-attrs`, `application-input` …).

| Signature | Kind | Description |
|-----------|------|-------------|
| `(available-p)` | function | True when `muimaster.library` ≥ `MUIMASTER_VMIN` (11, MUI 3.8) opens here — and leaves it open; `NIL` on the host and on an Amiga without MUI |
| `(class-id class)` | function | The `MUIC_*` name string for a class designator: a string as it is, a keyword by the rule "hyphens dropped, first letter upper case, `.mui` appended" — `:window` → `"Window.mui"`, `:numeric-button` → `"Numericbutton.mui"` (every class of `mui.h` and the newer MUIs) |
| `(new-object class &rest tags)` | function | `MUI_NewObjectA(class-id, tags)`: `class` a keyword or name string (a `struct IClass *` goes through intuition's `NewObjectA`); tag values may be integers, foreign pointers (child objects), `T`/`NIL` or strings (pooled, the object keeps the pointer).  Returns the object; signals, naming the class, when MUI returns NULL |
| `(make-object type &rest params)` | function | `MUI_MakeObjectA`: `type` is `:label` / `:button` / `:checkmark` / `:cycle` / `:radio` / `:slider` / `:string` / `:pop-button` / `:h-space` / `:v-space` / `:h-bar` / `:v-bar` / `:menustrip-nm` / `:menuitem` / `:bar-title` / `:numeric-button` (or the `MUIO_*` code), `params` the type's parameters as `mui.h` lists them — strings pooled, a list of strings (cycle / radio entries) as a pooled `STRPTR[]` |
| `(dispose-object object)` | function | `MUI_DisposeObject` — the application object takes its windows and their children along; `NIL` is ignored |
| `(get-attr-string attribute object)` | function | `get-attr` for a `STRPTR` attribute (`MUIA_String_Contents`, `MUIA_Text_Contents`): a Lisp string, or `NIL` |
| `(pool-string-array strings)` | function | A NULL-terminated `STRPTR[]` of pooled copies — `MUIA_Cycle_Entries`, `MUIA_Radio_Entries` |
| `(notify object attribute trigger dest method &rest params)` | function | `MUIM_Notify`: when `attribute` becomes `trigger` (a value, `T`/`NIL`, or `:every-time`), invoke `method` with `params` on `dest` — an object or `:self` / `:window` / `:application` / `:parent`; `:trigger-value` / `:not-trigger-value` in `params`; strings pooled; `FollowParams` computed |
| `(return-id application id)` | function | `MUIM_Application_ReturnID`: queue `id` for `application-input` |
| `(application-input application)` | function | `MUIM_Application_NewInput`: two values — the next return ID (`:quit` for `MUIV_Application_ReturnID_Quit`, `NIL` when none) and the signal mask to `Wait()` on (0 = call again at once) |
| `(do-application-events ((id) application &key timeout signals) &body body)` | macro | The event loop: `application-input`, `body` per return ID, `Wait()` on MUI's mask (+ `SIGBREAKF_CTRL_C` + `signals`) until `:quit`; `(return)` in `body` or Ctrl-C leaves.  With `timeout` (seconds, default `*event-loop-timeout*`) it polls and returns when the time is up |
| `*event-loop-timeout*` | variable | Default `:timeout` of `do-application-events`; `NIL` = interactive.  Set it before loading a GUI program to run it unattended |
| `(request application window title gadgets format &rest params)` | function | `MUI_RequestA`: `application` / `window` objects or `NIL`, `gadgets` as `"_Save\|_Use\|*_Cancel"`, a printf-style `format` whose `params` are longwords (`%ld`) and strings (`%s`, copied for the call); returns the gadget number (1 = leftmost, 0 = rightmost) |
| `(make-id string)` | function | `MAKE_ID('M','A','I','N')` from a four-character string — `MUIA_Window_ID` |
| `(window-size-minmax p)`, `(window-size-visible p)`, `(window-size-screen p)`, `(window-edge-delta p)` | function | The `MUIV_Window_{Width,Height,AltWidth,AltHeight}_{MinMax,Visible,Screen}(p)` and `MUIV_Window_{TopEdge,AltTopEdge}_Delta(p)` macros |
| `(create-custom-class superclass function &key data-size methods)` | function | `MUI_CreateCustomClass` over `superclass` (a class designator or another custom class) with a Lisp dispatcher: `function` gets the class, the object and the message and returns the method's result — for the methods it does not handle, `(do-super-method class object message)`.  `data-size` bytes of zeroed instance data per object.  `methods`, a list of method ids, limits `function` to those methods: the rest go to the superclass natively (list what the class handles when its objects are on an interactive path — MUI sends an object many methods per repaint and keystroke).  Returns the `struct MUI_CustomClass`; deleted, and the dispatcher released, when the enclosing `with-foreign-pool` exits |
| `(custom-class-stats mcc)` | function | For a class made with `methods`: messages its dispatcher received, messages passed to the superclass natively, method ids listed (three values); `nil` otherwise |
| `(custom-class-class mcc)` | function | Its `struct IClass` (`mcc_Class`) — what `new-object` takes to make an object of the class |
| `(do-super-method class object message)` | function | `DoSuperMethodA`: the message on to the superclass, from inside a dispatcher |
| `(method-id message)` | function | The `MethodID` of a method message |
| `(inst-data class object)` | function | `INST_DATA(class, object)`: the object's instance data as a foreign pointer |
| `(min-max-info message)`, `(add-min-max message &key min-width min-height max-width max-height def-width def-height)` | function | The `MUI_MinMax` of a `MUIM_AskMinMax` message, and adding to its six fields after `do-super-method` — what every `AskMinMax` method does |
| `(set-min-max info &key min-width min-height max-width max-height def-width def-height)` | function | Set (not add) the fields of a `MUI_MinMax` — `min-max-info`'s, or `layout-msg-min-max`'s; `NIL` leaves a field alone.  What a custom layout hook answers `MUILM_MINMAX` with |
| `(draw-flags message)` | function | The flags of a `MUIM_Draw` message (`MADF_DRAWOBJECT` …) |
| `(request-idcmp object flags)`, `(reject-idcmp object flags)` | function | `MUI_RequestIDCMP` / `MUI_RejectIDCMP`: ask the object's window for IDCMP classes in `MUIM_Setup`, give them back in `MUIM_Cleanup` — what a class answering `MUIM_HandleInput` does |
| `(area-rastport object)`, `(area-window object)`, `(area-draw-info object)`, `(area-screen object)`, `(area-pens object)`, `(area-font object)`, `(area-render-info object)`, `(area-flags object)` | function | `_rp(obj)`, `_window(obj)`, `_dri(obj)`, `_screen(obj)`, `_pens(obj)`, `_font(obj)`, `muiRenderInfo(obj)`, `mad_Flags` — valid when mui.h says they are (render info between `MUIM_Setup` / `MUIM_Cleanup`, window and rastport between `MUIM_Show` / `MUIM_Hide`) |
| `(area-left object)` … `(area-bottom object)`, `(area-mleft object)` … `(area-mbottom object)` | function | `_left` / `_top` / `_width` / `_height` / `_right` / `_bottom` and `_mleft` / `_mtop` / `_mwidth` / `_mheight` / `_mright` / `_mbottom` — the object's box and the box inside its frame, valid during `MUIM_Draw` |
| `(area-min-width object)`, `(area-min-height object)` | function | `_minwidth(obj)` / `_minheight(obj)` — the size the object settled on, valid between `MUIM_Show` and `MUIM_Hide`; what a layout hook reads from each child |
| `(layout-msg-type message)` | function | `lm_Type` of a `MUI_LayoutMsg` (a `MUIA_Group_LayoutHook`'s message): `MUILM_MINMAX` or `MUILM_LAYOUT` — anything else the hook answers with `MUILM_UNKNOWN` |
| `(layout-msg-min-max message)` | function | The `MUI_MinMax` embedded in the message (not a pointer to one, as in `MUIP_AskMinMax`) — fill it with `set-min-max` |
| `(layout-msg-width message)`, `(layout-msg-height message)` | function | `lm_Layout.Width` / `.Height`: the rectangle MUI gives the group for a `MUILM_LAYOUT`.  `setf`-able — a virtual group writes back what it really needs |
| `(layout-children message)` | function | The group's children as a list of objects — the `NextObject()` walk of `lm_Children` |
| `(layout-child child left top width height &optional flags)` | function | `MUI_Layout`: place one child inside the group's rectangle, in coordinates relative to it.  `T` when MUI accepted it |
| `(make-string-key-hook app object method entries)` | function | A `struct Hook` for a `String`'s `MUIA_String_EditHook` that takes key presses away from the gadget without running Lisp — MUI 3.8 edits an active `String` on input.device's task, where a Lisp hook is answered 0.  `entries` is a list of `(code qual-mask qual-value value)`: a press whose raw code is `code` and whose qualifiers, masked with `qual-mask`, equal `qual-value` (the right Shift and Alt count as the left ones) becomes a no-op for the gadget and `value` is queued to `object` as `method`'s one argument through `MUIM_Application_PushMethod`, so a custom class's Lisp dispatcher gets `(DoMethod object method value)` on the application's task.  Every other key stays the gadget's.  The hook must outlive the `String` |
| `(free-string-key-hook hook)` | function | Release it, once no `String` holds it (`NIL` is ignored) |
| `(string-key-hook-entry hook)` | function | Its `h_Entry` as a foreign pointer — exact on the host too, where it is a C function of `(hook sgwork message)` that tests call |
| `(string-key-hook-stats hook)` | function | Three values: calls, keys taken, entries.  "Never called" and "never matched" are different bugs |

---

## `AMIGA.ASYNCIO` — Asynchronous file I/O over DOS packets

The NDK 3.1 AsynchIO package (`asyncio.c`, Amiga Developer CD) as Common
Lisp: two buffers alternate, and while the program consumes or fills one,
an `ACTION_READ` / `ACTION_WRITE` packet for the other is in flight at the
filesystem handler — sent with `PutMsg`, collected with `WaitPort` over an
embedded reply port using the classic `PA_IGNORE` / `SIGB_SINGLE` trick.
Buffers are rounded to the device's block size and 16-byte aligned for
DMA.  The module loads on every system; `available-p` (and every open)
needs AmigaOS/MorphOS.  An async file must be used from the thread that
opened it, and file names cannot be interactive streams (`CON:`, `RAW:`,
`*`; `NIL:` works).  I/O errors signal a Lisp error carrying the DOS
error code.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(available-p)` | function | True when DOS packets can be spoken here: AmigaOS/MorphOS with dos.library; `NIL` on the host |
| `(open-async name mode &key buffer-size)` | function | Open `name` for `:read` (read-ahead starts before this returns), `:write` (create, replacing) or `:append`.  `buffer-size` (default 8192) is the total for both buffers, rounded up to twice the device block size.  Returns an `ASYNC-FILE`; signals on failure |
| `(close-async file)` | function | Flush buffered writes, close, free the OS memory.  Idempotent; returns `T` |
| `(with-async-file (var name mode &rest args) &body body)` | macro | `open-async` + `unwind-protect`ed `close-async` |
| `(read-async file dest &optional n)` | function | Read up to `n` bytes into `dest` — a foreign pointer, an integer address, or a `(unsigned-byte 8)` vector (defaults `n` to its length).  Returns bytes read; short only at EOF, `0` at EOF proper |
| `(write-async file src &optional n)` | function | Write `n` bytes from `src` (same kinds as `read-async`); a full buffer goes out asynchronously while the other fills |
| `(seek-async file position whence)` | function | `whence` is `:start` / `:current` / `:end`; returns the previous logical position, like DOS `Seek`.  `(seek-async f 0 :current)` is a position probe |
| `(read-byte-async file)` | function | Next byte as an integer, `NIL` at EOF — a single `peek-u8` on the fast path |
| `(read-char-async file)` | function | Next byte as a `CHARACTER`, `NIL` at EOF |
| `(read-line-async file)` | function | Next line without its newline, `NIL` at EOF |
| `(write-byte-async file byte)` | function | Write one byte (0..255) |
| `(write-char-async file char)` | function | Write one 8-bit character |
| `(write-string-async file string)` | function | Write a string's bytes |
| `(write-line-async file string)` | function | `write-string-async` plus a newline |

`examples/amiga/asyncio/copyfile.lisp` copies and verifies a 256 KB file
with it, timed against plain synchronous streams;
`tests/amiga/test-asyncio.lisp` is the executable specification
(`tests/test_amiga_asyncio.sh` load-checks module and example on the
host).

---

## `AMIGA.IFF` — IFF parsing and writing over iffparse.library

The NDK 3.1 `sift` example (`sift.c`, Amiga Developer CD) as Common
Lisp, grown into a module: `sift` prints the IFFCheck-like chunk
listing of any IFF file or of the clipboard (the C's `-c`), and the
machinery it needs — `ParseIFF` stepping, chunk inspection, the
matching write side — is exported for programs reading or writing IFF
themselves (ILBM, 8SVX, FTXT, own formats).  Chunk identifiers travel
as integers; `id-string` / `string-id` convert (`"FORM"` ⇄
`#x464F524D`) and work on any host, as does the module itself — only
`available-p` (and every open) needs AmigaOS/MorphOS.  Parse and write
errors signal a Lisp error carrying the iffparse `IFFERR_#?` text.

| Signature | Kind | Description |
|-----------|------|-------------|
| `(available-p)` | function | True when iffparse.library is open: AmigaOS/MorphOS; `NIL` on the host |
| `(open-iff source &key direction clipboard-unit)` | function | `source` is a file name or `:clipboard` (unit `clipboard-unit`, default 0); `direction` `:read` (default) or `:write` (create, replacing).  Returns an `IFF-FILE`; signals on failure |
| `(close-iff file)` | function | `CloseIFF`, close the stream underneath, `FreeIFF`.  Idempotent; returns `T` |
| `(with-iff (var source &rest args) &body body)` | macro | `open-iff` + `unwind-protect`ed `close-iff` |
| `(parse-step file &optional mode)` | function | One `ParseIFF` step; `mode` `:rawstep` (default), `:step` or `:scan`.  Returns `:chunk` on entering a context, `:end-of-chunk` before leaving one, `:eof` at the end |
| `(current-chunk file)` | function | The current context node as `(values id type size scan)`, integers; `NIL` when there is none |
| `(iff-depth file)` | function | Chunk nesting depth (`iff_Depth`): 1 at the top-level `FORM` (the default outer context counts) |
| `(read-chunk-bytes file vector &optional n)` | function | Current chunk data into a `(unsigned-byte 8)` vector; clips at the chunk boundary, returns bytes read |
| `(push-chunk file type id &optional size)` | function | Write side: open a chunk.  `type`/`id` are 4-character strings or integers, `type` `NIL` for plain data chunks; default `size` `IFFSIZE_UNKNOWN` is back-patched by `pop-chunk` |
| `(pop-chunk file)` | function | Close the innermost chunk: pad byte if odd, size patched |
| `(write-chunk-bytes file data &optional n)` | function | Write a byte vector's or 8-bit string's bytes into the current chunk |
| `(map-chunks function source &key clipboard-unit)` | function | Parse `source` and call `function` with `(id type size depth)` per chunk entered; returns the chunk count |
| `(sift source &key stream clipboard-unit)` | function | The `sift` program: print the indented chunk listing to `stream`; returns the chunk count |
| `(id-string id)` / `(string-id string)` | function | Integer identifier ⇄ 4-character string, e.g. `#x464F524D` ⇄ `"FORM"` |
| `(error-string code)` | function | The text for an iffparse `IFFERR_#?` code, matching the C sift's message table |

`examples/amiga/iff/sift.lisp` builds a nested CAT-of-FORMs file with
the write side, lists it, reads chunk data back and tries the
clipboard; `tests/amiga/test-iff.lisp` is the executable specification
(`tests/test_amiga_iff.sh` covers the pure-Lisp half and load-checks
module and example on the host).

---

## `AMIGA.AREXX` — ARexx development port

An ARexx host port that lets a native Amiga editor drive the running Lisp:
load the file you are editing, get its compile diagnostics back, evaluate a
form in the live image. Served by its own thread, so it answers while the
REPL is busy.

```lisp
(require "amiga/arexx")
(amiga.arexx:start)          ; => "AMILISP"
```

| Signature | Kind | Description |
|-----------|------|-------------|
| `(start &key name stack-size vm-frames)` | function | Open the port and start its handler thread; returns the port name claimed. `stack-size`/`vm-frames` size the thread that compiles your code |
| `(stop)` | function | Remove the port and stop the handler thread |
| `(running-p)` | function | Whether the port is open and its thread alive |
| `(port-name)` | function | Name the port is registered under, or `NIL` |
| `(send port command &key result-size)` | function | Drive another application's ARexx port (or our own) — returns `(values rc result-string)` |
| `*default-port-name*` | variable | Base name `START` claims (`"AMILISP"`) |
| `*handler-thread*` | variable | The `MP` thread serving the port |

The commands themselves (`PING`, `VERSION`, `LOAD`, `COMPILE-FILE`, `EVAL`,
`IN-PACKAGE`, `LASTRESULT`, the introspection commands and the `REPL-*`
commands behind an editor's listener window), the return-code ladder, and the
`RESULT`/`FAILAT` protocol notes are documented in the
[ARexx port](../README.md#arexx-port-amigaos--morphos) section of the main
README. They are implemented by the portable `EXT.DEV` package
(`lib/dev-commands.lisp`), which loads and runs on the host as well —
`(ext.dev:handle-command "LOAD foo.lisp")` returns `(values rc text)` with no
Amiga in sight, which is how the command layer is tested.

The same package loads ASDF systems for an editor:
`(ext.dev:load-asd-system "Work:src/foo/foo.asd")` loads ASDF on first use,
the definition file when ASDF does not have its system yet, and the system
named after it (or the one a second argument names among the file's
systems, `"foo/tests"`) -- incrementally, as `asdf:load-system` does; a
`.asd` that changed on disk ASDF reloads by itself.
`(ext.dev:test-asd-system ...)` runs `asdf:test-system` on it the same way.

## Source of truth

`tests/amiga/test-gui.lisp` exercises the Intuition/Graphics/GadTools path on
AmigaOS via FS-UAE; `tests/amiga/test-boopsi.lisp` (with
`tests/test_amiga_boopsi.sh` on the host) covers `AMIGA.BOOPSI` against the
built-in intuition classes; `tests/amiga/test-reaction.lisp` (with
`tests/test_amiga_reaction.sh` on the host) covers `AMIGA.REACTION`, and
`examples/amiga/reaction/` are its worked examples; `tests/amiga/test-mui.lisp`
(with `tests/test_amiga_mui.sh` on the host) covers `AMIGA.MUI` against a
real Application / Window tree, `examples/amiga/mui/` being its worked
examples — all run and photographed
unattended by `verify/realamiga/run-examples.sh`, together with the
graphics examples (`tests/amiga/test-gfx-examples.lisp` runs those in the
suite, `tests/test_amiga_gfx_examples.sh` load-checks them on the host);
`tests/amiga/test-audio.lisp` covers `AMIGA.AUDIO`;
`tests/amiga/test-ahi.lisp` covers `AMIGA.AHI` (skipping the device
checks where AHI is absent), `examples/amiga/audio/ahi-play.lisp` being
its worked example;
`tests/amiga/test-asyncio.lisp` (with `tests/test_amiga_asyncio.sh` on
the host) covers `AMIGA.ASYNCIO`; the
`tests/amiga/arexx-tests.lisp` drives `AMIGA.AREXX` end to end
over the real host protocol, and `tests/test_dev_commands.sh` is the
host-side specification for the command layer;
`examples/amiga/gfx/` holds the runnable graphics demos (bouncing lines,
the NDK double-buffering example, the RKM hardware-sprite example, a
screen grabber that saves any screen as a PPM file). See the
[AmigaOS and MorphOS APIs](../README.md#amigaos-and-morphos-apis) and
[Raw FFI Access](../README.md#raw-ffi-access) sections of the main README.

> The GUI bindings cover common cases (windows, drawing, gadgets, menus) but not
> the full API surface — see
> [Known Limitations](../README.md#known-limitations-and-future-work).
