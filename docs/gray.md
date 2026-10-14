# `GRAY` — Gray Streams Protocol

The Gray-streams protocol lets you define your own stream classes in Lisp by
subclassing the `fundamental-*-stream` classes and specializing the
`stream-*` generic functions. AmiLisp's built-in stream functions (`read-char`,
`write-string`, `read-line`, …) dispatch to these generics for user-defined
streams. This is the foundation several ported libraries rely on — e.g. the
**chipz** decompressing stream and **drakma**/**Hunchentoot** flexi-streams — and
it is loaded by the SLY backend.

- **Package:** `GRAY` (nickname `...`), matching ECL/CLISP.
- **Load it:** `(require "gray-streams")` (resolves `lib/gray-streams.lisp`).
  The `GRAY` package must exist at read time before any `gray:`-qualified symbol
  is read, so load it in its own top-level form.

```lisp
(require "gray-streams")

(defclass uppercase-out (gray:fundamental-character-output-stream)
  ((target :initarg :target :reader target)))

(defmethod gray:stream-write-char ((s uppercase-out) ch)
  (write-char (char-upcase ch) (target s)))

(let ((s (make-instance 'uppercase-out :target *standard-output*)))
  (write-string "hello" s))     ; prints HELLO
```

## Fundamental stream classes

Subclass these to define a stream. `fundamental-stream` is the root.

| Class | Specialize for |
|-------|----------------|
| `fundamental-stream` | any stream |
| `fundamental-input-stream` / `fundamental-output-stream` | direction |
| `fundamental-character-stream` | character streams |
| `fundamental-character-input-stream` / `fundamental-character-output-stream` | character I/O |
| `fundamental-binary-stream` | binary streams |
| `fundamental-binary-input-stream` / `fundamental-binary-output-stream` | binary I/O |

## Generic functions to specialize

| Input | Output | Query / control |
|-------|--------|-----------------|
| `(stream-read-char stream)` | `(stream-write-char stream character)` | `(stream-line-column stream)` |
| `(stream-unread-char stream character)` | `(stream-write-string stream string &optional start end)` | `(stream-start-line-p stream)` |
| `(stream-read-char-no-hang stream)` | `(stream-write-byte stream byte)` | `(stream-listen stream)` |
| `(stream-peek-char stream)` | `(stream-terpri stream)` | `(stream-clear-input stream)` |
| `(stream-read-line stream)` | `(stream-fresh-line stream)` | `(stream-clear-output stream)` |
| `(stream-read-byte stream)` | `(stream-finish-output stream)` | `(stream-advance-to-column stream column)` |
| `(stream-read-sequence stream sequence start end &key &allow-other-keys)` | `(stream-force-output stream)` | |
| | `(stream-write-sequence stream sequence start end &key &allow-other-keys)` | |

## Source of truth

`lib/gray-streams.lisp` is the implementation. The
`(typep gray-stream 'stream)` regression and a worked Gray-stream subclass live
in `tests/amiga/run-tests.lisp` (the "Gray streams" block). The
**chipz** AmiLisp fork's `stream.lisp` `#+cl-amiga` branch is a real-world usage
example (see [Library forks](../README.md#library-forks-amilisp-backends)).
