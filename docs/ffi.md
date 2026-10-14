# `FFI` — Foreign-Function Interface

Foreign pointers and typed memory access on **all** platforms, plus a real,
general-purpose foreign-function engine (dynamic library loading, arbitrary C
calls with full marshaling, and Lisp-as-C callbacks) on the **POSIX host**.

- **Package:** `FFI` (uses `CL`)
- **Inherited by:** `COMMON-LISP-USER`.
- On AmigaOS, foreign *calls* go through the library-vector model in the
  [`AMIGA` / `AMIGA.FFI`](amiga.md) packages instead; the peek/poke and
  foreign-pointer parts of `FFI` work everywhere.
- The standard **CFFI** API is layered on top of this engine via the AmiLisp
  CFFI backend, so CFFI-dependent Quicklisp systems load on the host.

```lisp
;; Resolve and call libc directly (host)
(ffi:call-foreign (ffi:symbol-pointer "pow") :double '(:double :double) '(2d0 10d0))
;; => 1024.0d0

;; Typed memory access (all platforms)
(let ((p (ffi:alloc-foreign 8)))
  (ffi:poke-u32 p 0 #xDEADBEEF)
  (prog1 (ffi:peek-u32 p 0) (ffi:free-foreign p)))
```

## Foreign pointers & allocation

| Signature | Kind | Description |
|-----------|------|-------------|
| `(make-foreign-pointer address &optional size)` | function | Wrap a raw address as a foreign pointer; `size` (bytes) enables bounds checks |
| `(foreign-pointer-address pointer)` | function | The integer address of a foreign pointer |
| `(foreign-pointer-p object)` | function | Type predicate |
| `(null-pointer-p pointer)` | function | Whether a pointer is null |
| `(pointer-eq pointer1 pointer2)` | function | Address equality of two pointers |
| `(pointer+ pointer offset)` | function | Pointer plus a byte offset |
| `(alloc-foreign size)` | function | Allocate `size` bytes of foreign memory |
| `(free-foreign pointer)` | function | Free foreign memory from `alloc-foreign` |

## Typed peek / poke

Every reader is `(peek-TYPE pointer &optional offset)`; every writer is
`(poke-TYPE pointer value &optional offset)` and returns `value`. `offset`
is in bytes and defaults to 0.

| Read | Write | Type |
|------|-------|------|
| `(peek-i8 pointer &optional offset)` / `(peek-u8 pointer &optional offset)` | `(poke-i8 pointer value &optional offset)` / `(poke-u8 pointer value &optional offset)` | 8-bit signed / unsigned |
| `(peek-i16 pointer &optional offset)` / `(peek-u16 pointer &optional offset)` | `(poke-i16 pointer value &optional offset)` / `(poke-u16 pointer value &optional offset)` | 16-bit signed / unsigned |
| `(peek-i32 pointer &optional offset)` / `(peek-u32 pointer &optional offset)` | `(poke-i32 pointer value &optional offset)` / `(poke-u32 pointer value &optional offset)` | 32-bit signed / unsigned |
| `(peek-i64 pointer &optional offset)` / `(peek-u64 pointer &optional offset)` | `(poke-i64 pointer value &optional offset)` / `(poke-u64 pointer value &optional offset)` | 64-bit signed / unsigned |
| `(peek-single pointer &optional offset)` / `(peek-double pointer &optional offset)` | `(poke-single pointer value &optional offset)` / `(poke-double pointer value &optional offset)` | IEEE float / double |
| `(peek-pointer pointer &optional offset)` | `(poke-pointer pointer value &optional offset)` | machine-word pointer |

## Bulk byte transfer

Moving a buffer one `poke-u8` at a time costs a VM dispatch per byte, which
dominates any path that pushes real data across (bitplane rows, chip-RAM
masks, packet buffers). These move a whole span in one call:

| Signature | Description |
|-----------|-------------|
| `(poke-bytes pointer source &optional offset start end)` | Copy `source[start..end)` to `pointer + offset`; returns the byte count |
| `(peek-bytes pointer vector &optional offset start end)` | Fill `vector[start..end)` from `pointer + offset`; returns the byte count |

`source` is a string or a vector of `(integer 0 255)`. A string holds its
bytes contiguously so it copies with `memcpy`; a vector holds one tagged
element per byte — AmiLisp upgrades `(unsigned-byte 8)` to `t`, as
`upgraded-array-element-type` reports — so it unpacks in a C loop instead.
Both beat per-byte `poke-u8`; only the string form reaches `memcpy` speed.

Out-of-range elements, spans past the end of the source, and (when the
pointer's allocation size is known) writes that would overrun the buffer are
all rejected with a diagnostic rather than silently corrupting memory.

## Foreign strings

| Signature | Kind | Description |
|-----------|------|-------------|
| `(foreign-string string)` | function | Copy a Lisp string into a freshly allocated C string |
| `(foreign-to-string pointer &optional max-len)` | function | Read a NUL-terminated C string into a Lisp string: up to its NUL, or at most `max-len` bytes when given |

## Calls, callbacks, libraries (host)

| Signature | Kind | Description |
|-----------|------|-------------|
| `(load-library name)` | function | `dlopen` a shared library; returns a library handle or `nil` |
| `(close-library library)` | function | `dlclose` it |
| `(symbol-pointer name &optional library)` | function | `dlsym` — resolve a symbol to a foreign pointer; `library` defaults to the default namespace |
| `(call-foreign fn-ptr ret-type arg-types arg-values &optional n-fixed)` | function | Call a C function (libffi); `n-fixed` = count of fixed args for variadic calls |
| `(make-callback ret-type arg-types lisp-fn &optional regs)` | function | Create a C-callable callback from a Lisp function — a libffi closure on the host, a 68k stub (into a PPC gate on MorphOS) on the target, where it works too.  `regs`, a list parallel to `arg-types` of `:d0`–`:d7` / `:a0`–`:a6` / `NIL`, names the 68k register each argument arrives in for the AmigaOS register conventions (`'(:a0 :a2 :a1)` = a `struct Hook` entry's hook / object / message; the host ignores it).  On the target the result is a 32-bit integer or pointer in d0 and `:float` / `:double` arguments are refused |
| `(free-callback callback)` | function | Release a callback |
| `ext:*callback-error-policy*` | variable | What an unhandled error inside a callback does: `:defer` (default) — the callback returns 0 / NULL and the condition is re-signaled once the foreign call that invoked it returns, where a `handler-case` around `call-foreign` (or the library call) catches it; `:debug` — enter the debugger inside the callback (host only in practice) |
| `(ext:%ffi-foreign-task-calls)` | function | How many callback invocations so far came from a thread or task that is not a Lisp thread and were answered with 0 without running Lisp (see below) — readable where the warning is not visible, e.g. over an ARexx port from a detached process |

A callback is a *boundary*: the Lisp function runs on the foreign caller's
stack, between its C frames, and no non-local exit may cross them.  A
`throw` / `return-from` / `go` to a target outside the callback, or an
`invoke-restart` of a restart established outside it, is an error (the
message says why) that is handled like any other unhandled error inside the
callback — parked and re-signaled after the foreign call returns.
`handler-case`, `unwind-protect` and `catch` *inside* the callback work as
usual; a callback may itself make foreign calls whose callbacks error.  A
callback invoked from a thread or task that is not a Lisp thread returns 0
without running Lisp, and the next foreign call that returns on a Lisp
thread prints a warning.

## Source of truth

`tests/test_ffi.c` and `trunk/load-and-test-cffi.lisp` (the CFFI backend
end-to-end); the callback boundary is tested there (host) and in
`tests/amiga/test-raw-bindings.lisp` (target, through `CallHookPkt` and a
68k caller snippet).  See also [Host FFI](../README.md#host-ffi-dlopen--libffi--cffi) in
the main README. Higher-level utilities (`defcstruct`, `with-foreign-alloc`) live
in `lib/ffi.lisp`; the AmigaOS hook and dispatcher wrappers in
`lib/amiga/ffi.lisp` (`AMIGA.FFI`, [amiga.md](amiga.md)).
