# `MP` — Multiprocessing (Threads)

Kernel threads with per-thread dynamic bindings, locks, and condition variables.
On POSIX this is pthreads (`pthread_rwlock`, `__sync_*` atomics); on AmigaOS it is
`CreateNewProc()`, shared/exclusive `SignalSemaphore`s, custom condition variables
over signal bits, and `Forbid()`/`Permit()` for atomicity. GC is stop-the-world
with safepoints across all live threads.

- **Package:** `MP` (uses `CL`)
- **Inherited by:** `COMMON-LISP-USER`.
- This package backs the **bordeaux-threads** AmiLisp fork (BT v1 and v2), which
  is what most concurrent libraries (Sento, lparallel, …) build on.

```lisp
(let ((lock (mp:make-lock "counter"))
      (n 0))
  (let ((threads (loop repeat 4 collect
                   (mp:make-thread
                     (lambda ()
                       (dotimes (i 1000)
                         (mp:with-lock-held (lock) (incf n))))))))
    (mapc #'mp:join-thread threads)
    n))            ; => 4000
```

## Threads

| Signature | Kind | Description |
|-----------|------|-------------|
| `(make-thread function &key name)` | function | Spawn a thread running a function |
| `(join-thread thread)` | function | Block until a thread finishes, returning its result |
| `(current-thread)` | function | The thread object for the calling thread |
| `(all-threads)` | function | List of all live threads |
| `(thread-name thread)` | function | A thread's name |
| `(thread-alive-p thread)` | function | Whether a thread is still running |
| `(threadp object)` | function | Type predicate for thread objects |
| `(thread-yield)` | function | Hint the scheduler to run other threads |
| `(interrupt-thread thread function)` | function | Run a function in the context of another thread |
| `(destroy-thread thread)` | function | Forcibly terminate a thread |
| `(dump-thread-waits)` | function | Debug: print what each thread is waiting on |
| `*thread-death-hooks*` | variable | Functions called when an unhandled error ends a thread (below) |

### Hearing of a thread that died

A thread that an unhandled error ends is otherwise just gone: `thread-alive-p`
says so to whoever asks, but nobody is told. And an exhausted heap, the error
that ends most worker threads in practice, is not signalled as a condition --
no `handler-case` and no `unwind-protect` in the thread's own code sees it.

`mp:*thread-death-hooks*` is a list of function designators. Each is called
with the thread object and the error's message, on the dying thread, after
its stacks were dropped -- so what the thread held is garbage again and the
hook has room to allocate, to send a message or to start a replacement:

```lisp
(push (lambda (thread message)
        (format *error-output* "~&; ~a died: ~a~%" (mp:thread-name thread) message))
      mp:*thread-death-hooks*)
```

The hooks are not called for a thread whose function returned, that took
its `abort` restart, or that `destroy-thread` ended. A hook that fails is
dropped without a word (`AMILISP_THREAD_ERRORS=1` in the environment reports
it, as it reports every thread an error ended) and the next one still runs.
A lock the thread held when it died stays held: no cleanup ran for it. See
`tests/test_mt_thread_death_hook.sh`.

## Locks

| Signature | Kind | Description |
|-----------|------|-------------|
| `(make-lock &optional name)` | function | Create a mutex |
| `(make-recursive-lock &optional name)` | function | Create a recursive (re-entrant) lock |
| `(acquire-lock lock &optional (wait-p t) timeout)` | function | Acquire a lock; with `wait-p` `nil`, try once without blocking; `timeout` (seconds) bounds a blocking acquire — `nil` on timeout |
| `(release-lock lock)` | function | Release a held lock (an error unless the caller holds it) |
| `(with-lock-held (lock &key timeout) &body body)` | macro | Acquire/release around a body; with a `timeout` (seconds) that elapses first, the body does not run and the form returns `nil` |
| `(with-recursive-lock-held (lock &key timeout) &body body)` | macro | As above for a recursive lock |
| `(lockp object)` | function | Type predicate for locks |
| `(lock-name lock)` | function | A lock's name |

## Condition variables

| Signature | Kind | Description |
|-----------|------|-------------|
| `(make-condition-variable &optional name)` | function | Create a named condition variable |
| `(condition-wait condition-variable lock &optional timeout)` | function | Atomically release the lock and wait; `timeout` in seconds |
| `(condition-notify condition-variable)` | function | Wake one waiter |
| `(condition-broadcast condition-variable)` | function | Wake all waiters |
| `(condition-variable-p object)` | function | Type predicate |
| `(condition-name condition-variable)` | function | A condition variable's name |

## Memory barriers

| Signature | Kind | Description |
|-----------|------|-------------|
| `(read-memory-barrier)` | function | Acquire/read fence |
| `(write-memory-barrier)` | function | Release/write fence |

Locks and condition variables are plain heap objects with no OS primitive
behind them, so there is no limit on how many can be live at once and a
dropped lock is ordinary garbage.  A plain lock is not re-entrant: acquiring
one the calling thread already holds signals an error (use
`make-recursive-lock`).  Releasing a lock the caller does not hold signals
an error and leaves the lock as it was.  A recursive lock held at depth
*n* across `condition-wait` is re-acquired at depth *n*.  Timeouts on
AmigaOS resolve in 20–50 ms steps.

> `%make-recursive-lock` is the internal primitive behind `make-recursive-lock`.
> Not yet covered: semaphores and `with-timeout` — see
> [Known Limitations](../README.md#known-limitations-and-future-work).

## Atomic operations

| Signature | Kind | Description |
|-----------|------|-------------|
| `(compare-and-swap place old new)` | macro | Atomically store `new` in `place` if it holds `old` (under `eq`). Returns the value `place` held when the comparison was made — `eq` to `old` exactly when the swap happened |
| `(cas place old new)` | macro | Alias for `compare-and-swap` |
| `(atomic-incf place &optional (delta 1))` | macro | Atomically add `delta` to the fixnum in `place`; returns the new value |
| `(atomic-decf place &optional (delta 1))` | macro | Atomically subtract `delta` from the fixnum in `place`; returns the new value |

Supported places: `(car c)` `(cdr c)` `(first c)` `(rest c)`, `(svref v i)` /
`(aref v i)` on a simple-vector, `(symbol-value s)` or a special variable
(the calling thread's dynamic binding when one is in effect), `(slot-value o 's)`
on a standard-object or structure, defstruct slot accessors, and macros,
symbol-macros or `(the type ...)` wrapping one of these.  The place's subforms
are evaluated once, left to right, before `old` and `new` (or `delta`).  An
unsupported place is an error at macroexpansion time.

`atomic-incf` / `atomic-decf` require the current value, the delta and the
result to be fixnums; anything else signals an error and leaves the place
unchanged — a counter never silently turns into a bignum.

The primitive is real on every target: a native compare-exchange on the host,
and a `Forbid()`/`Permit()` window on AmigaOS and MorphOS, where the single
core makes disabling task switching a complete atomicity guarantee.  These
operations are atomic with respect to each other across all threads; a plain
`setf` of the same cell is not part of the protocol.  A library backend maps
onto them directly — e.g. `atomics:cas` as
`(eq old (mp:compare-and-swap place old new))`.

## Source of truth

`tests/test_threads.c`, `tests/test_dynbind_threaded.c`,
`tests/test_gc_threaded.c`, `tests/test_atomics.c`, and the threading and
atomics blocks in `tests/amiga/run-tests.lisp`.
