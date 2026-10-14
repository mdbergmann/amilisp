# MP locks and condition variables as heap words

Status: IMPLEMENTED (2026-09-10, branch `mp-heap-locks`; see "Implementation notes")
Date: 2026-09-10
Origin: [docs/sento-bench-results-0.10.md](../docs/sento-bench-results-0.10.md),
"shared/ask and the lock table" — the 0.10 runtime fills the 16384-slot
lock table on a standard sento cell.

## Problem

An `MP:LOCK` is a heap object that owns an OS mutex. Because a heap object
cannot hold a host pointer (every struct field is 32-bit; the arena is
offset-addressed), the object carries a 32-bit *id* and four fixed side
tables map that id to the primitive and its bookkeeping:

| Table | Size | Written by |
| --- | --- | --- |
| `cl_lock_table[id]` → `void *` mutex | `CL_MAX_LOCKS` (16384 host / 256 Amiga) | make-lock, GC finalizer |
| `cl_lock_held[id]` → holder `CL_Thread *` | same | every acquire / release |
| `cl_lock_depth[id]` → recursion depth | same | every acquire / release |
| `cl_condvar_table[id]` → `void *` condvar | `CL_MAX_CONDVARS` | make-condition-variable, finalizer |

The consequences, all measured or read from the code in the 0.10 entry:

- **A hard cap.** `MP:MAKE-LOCK` fails with "lock table full" once 16384
  lock objects are reachable. Sento's asynchronous `ask` keeps three locks
  per in-flight message (the waiting actor's message box, its queue, the
  future), so ~5,400 asks in flight is the ceiling, and the 0.10 sender
  side reaches it at the bench's default queue cap. Any actor program with
  a deep backlog hits the same wall.
- **A global mutex and a 16k-entry scan on every allocation.** The scan is
  ~3 µs at 15k live locks (not the limiter today, but O(n)), and the mutex
  serialises every thread that conses a lock.
- **Cross-thread cache-line traffic on every acquire and release.** The
  holder and depth arrays are indexed by id; locks allocated together get
  adjacent ids, so unrelated locks owned by different threads share lines.
  This is the class of cost the 0.8 regression bisect found in the
  Ctrl-C poll counter (docs/sento-bench-results-0.8.md, root cause 1).
- **A shared parking condvar that broadcasts.** A contended acquire past
  the 256-yield spin parks on one global condvar; every release with
  registered waiters wakes *all* of them (thundering herd) on a 100 ms
  timed backstop.
- **OS resources that must be destroyed by hand.** The GC finalizer
  destroys a dead lock's mutex, except when a thread still holds it, in
  which case the mutex is leaked with a warning (destroying a held
  pthread mutex is undefined). A shutdown sweep walks all four tables
  because AmigaOS never reclaims a `SignalSemaphore`. Image restore
  re-creates every primitive at its recorded id.

Fixing the cap alone (a growable table) keeps every other line of this
list. This spec removes the tables.

## Design

**A lock is data.** The lock object holds its own state word and the
runtime never allocates an OS primitive per lock. Blocking goes through
one park/unpark primitive *per thread*, which already exists in spirit
(the STW barrier parks threads; thread join on the Amiga waits on a
per-task signal bit).

### Objects

```c
typedef struct {
    CL_Header hdr;
    volatile uint32_t state;   /* 0 = free; else owner_serial << 1 | CONTENDED */
    uint32_t depth;            /* nested acquires by the owner; 1 when held once */
    uint32_t flags;            /* CL_LOCK_FLAG_RECURSIVE, as today */
    CL_Obj   name;
} CL_Lock;

typedef struct {
    CL_Header hdr;
    volatile uint32_t waiters; /* registered waiters; 0 lets notify return at once */
    CL_Obj   name;
} CL_CondVar;
```

Four 32-bit fields, no pointers, no ids. `CL_LOCK_CONTENDED` is bit 0 of
`state`; the owner is the thread's process-unique *serial* (never reused,
main = 1, `CL_Thread.serial`) shifted up one, so that a free lock is the
zero word and a lock left held by an exited thread can never read as owned
by a later thread that inherited its table slot. Both layouts are 32-bit
clean on every target.

### Per-thread parking

Each `CL_Thread` gets, **at the end of the struct** (the hot-field layout
trap, see Tier-4 phase 1 notes in docs/benchmarks.md):

```c
    void  *park;            /* platform park handle (token semantics) */
    CL_Obj wait_obj;        /* lock or condvar this thread is parked on, or NIL */
    CL_Obj wait_lock;       /* the lock to re-acquire after a condvar wait */
```

`wait_kind` (already present: running / condwait / condwait-timeout /
lock-acquire / GC-STW-wait) keeps its meaning; `wait_cv_id` and
`wait_lock_id` are replaced by the two objects above.

The platform primitive has *token* semantics, like a binary semaphore:

```c
int  platform_park_init(void **handle);
void platform_park_destroy(void *handle);
int  platform_park(void *handle, uint32_t timeout_ms);   /* 0 = unparked, 1 = timed out; 0 ms = forever */
void platform_unpark(void *handle);
```

An `unpark` that arrives before the matching `park` is not lost: the next
`park` returns immediately and consumes the token. This is what closes the
register-then-park window without the 100 ms backstop.

- **POSIX / Windows (winpthreads)**: a mutex, a condvar and a `permit`
  flag per thread. `park` waits while `permit == 0`, then clears it;
  `unpark` sets it under the mutex and signals.
- **AmigaOS / MorphOS**: one signal bit allocated per thread at thread
  creation and freed at exit. `park` is `Wait()` on the bit (a delivered
  `Signal()` stays set until consumed — token semantics for free);
  `unpark` is `Signal(task, bit)`. A timed park keeps the current
  `Delay()`-step poll of the condvar code for the first version (see Open
  questions for timer.device). This also retires the per-wait
  `AllocSignal(-1)` whose exhaustion produced the degraded spurious-wakeup
  paths in `platform_thread_amiga.c`.

The runtime's own C-level mutexes (`gc_mutex`, `cl_thread_list_lock`,
stream and table locks, rwlocks) are **not** touched by this spec; only
the Lisp-visible MP objects change.

### Acquire

```
fast path:   CAS(state, 0 → me)            → depth = 1, return T
             owner == me, recursive lock   → depth++, return T
             owner == me, plain lock       → error "recursive acquire of a non-recursive lock"
             :wait NIL                     → return NIL
spin:        a short trylock/yield phase (host: as today, 256 yields;
             Amiga: a handful — the only CPU is the one holding the lock)
slow path:   loop
               s = state
               if s == 0 and CAS(0 → me | CONTENDED)  → break
               register (under cl_thread_list_lock): wait_obj = lock, wait_kind = LOCK-ACQUIRE
               loop                                     ; mark whoever holds the lock NOW
                 s = state
                 if s == 0: break                       ; a release slipped in: do not park
                 if s & CONTENDED, or CAS(s → s | CONTENDED) succeeds: break
               if s == 0: deregister, continue
               enter safe region; platform_park(park, diag ? diag_ms : 0); leave safe region
               deregister
               if interrupt_pending: hand the lock on (lock_abandon_wait), cl_thread_handle_interrupt
```

The order *register, then mark the current holder, then park* is the
whole protocol (the first cut marked first and re-checked only "free"
after registering, which lost a wakeup — see the implementation
notes): the holder whose word carries the bit cannot release without
scanning, its scan runs after the store of 0 and therefore after the
registration, and a holder that took the lock between the mark and the
registration is re-marked before the park.  It is `futex_wait(&val,
2)`'s atomic "still 2?" check, done by hand against a registration
list.

A waiter that comes off the slow path acquires with the `CONTENDED` bit
*kept set*, even when it cannot know whether other waiters remain. That is
the "futex mutex, version 2" rule: it makes the next release scan for
waiters once (finding none is cheap), which is what keeps a second waiter
from being stranded after the first is woken. The uncontended path never
scans anything.

Barging is allowed: a spinning newcomer may take the lock before the woken
waiter, which then re-parks. This is the same fairness as today's trylock
loop and what bordeaux-threads promises (nothing).

### Release

```
depth--; if depth > 0 return NIL
owner != me                  → error "release of a lock held by another thread"
s = state
if !(s & CONTENDED) and CAS(s → 0)   → return NIL        ; one CAS, no scan
release fence; state = 0; full fence                    ; the plain store publishes the
                                                        ; critical section's writes and
                                                        ; must be ordered before the scan
under cl_thread_list_lock: find one thread with wait_kind = LOCK-ACQUIRE
                           and wait_obj == this lock; platform_unpark it
```

Today a non-owner release is undefined behaviour in pthreads and a plain
`ReleaseSemaphore` on the Amiga; the spec turns both misuse cases into
errors, the way SBCL does.

### Condition wait

```
owner != me                              → error "condition-wait without holding the lock"
register (under cl_thread_list_lock):     wait_obj = cv, wait_lock = lock, wait_kind = CONDWAIT[-TIMEOUT];
                                          cv->waiters++
pre-park interrupt check                  (as today: consume here, not at "the next safepoint")
saved = depth; release the lock fully     (the contended-release path above wakes a lock waiter if any)
enter safe region; r = platform_park(park, timeout_ms); leave safe region
under cl_thread_list_lock:
    if wait_kind is still CONDWAIT*:      we timed out (or were unparked by an interrupt):
                                          deregister, cv->waiters--, result = NIL
    else:                                 a notifier picked us (it already deregistered us and
                                          decremented waiters), result = T
re-acquire the lock (slow path above); depth = saved
post-wake interrupt check                 (as today, with the lock held)
return result
```

Resolving "timed out" against "notified" under the same lock the notifier
uses is what makes a notify that lands at the timeout instant count as a
notify, never both or neither. Restoring the saved depth replaces today's
"the wait always re-locks at depth 1" (the platform condvar knows nothing
about recursion); a recursive lock held at depth 2 across a wait comes
back at depth 2.

### Notify and broadcast

```
if cv->waiters == 0: return              ; fast path, no lock
under cl_thread_list_lock:
    for each thread with wait_kind = CONDWAIT* and wait_obj == cv (one / all):
        wait_kind = 0; cv->waiters--; platform_unpark(thread->park)
```

One waiter wakes per notify, on its own token. The `waiters` fast path
keeps the common "notify with nobody waiting" free, which is most of
sento's notifies on the reply path.

### Interrupt and destroy of a parked thread

The publisher already sets `interrupt_pending` under `cl_thread_list_lock`
and has the target's `CL_Thread *`. It now simply `platform_unpark`s the
target. The target's park returns, it deregisters, and consumes the
interrupt (for a condvar wait: after re-acquiring the lock, exactly as
today, so a destroy longjmps out with the lock held and the caller's
`unwind-protect` releases it). This replaces `wake_interrupted_waiter`'s
lock-then-broadcast dance and its 20 × 10 ms trylock loop.

A thread destroyed while *holding* a lock leaves `state` pointing at a
dead thread id, as today's leaked mutex does; later acquirers park until
the process ends. The `AMILISP_LOCK_DIAG` report already says "held by an
already-exited thread"; that stays. (An exit-path release of held locks
would be a behaviour change and is out of scope.)

### GC

- `wait_obj` and `wait_lock` are roots: marked and forwarded next to
  `t->result` (mem.c, mark and update passes). A thread parked on a lock
  therefore keeps the lock alive, and after a compaction the record holds
  the forwarded object.
- Identity comparisons in notify and release run in mutator code outside
  a safe region, so no compaction can be in progress while they compare;
  the parked thread's record was forwarded during the STW. Both sides see
  the same object.
- `TYPE_LOCK` and `TYPE_CONDVAR` lose their finalizer cases: a dead lock
  is plain garbage, held or not. The shutdown sweep of the four tables
  goes away with the tables.
- Generational collector: nothing special. The objects are ordinary heap
  objects; `state` is mutated in place by C, which is not a pointer store
  and needs no dirty-page treatment.

### Images and FASLs

- `CL_IMAGE_VERSION` is bumped: the layouts change. Restore zeroes
  `state`, `depth` and `waiters` for every lock and condvar in the image
  (a saved image cannot meaningfully contain a held lock; today's restore
  also re-creates every primitive fresh). `cl_lock_table_install_at` and
  its condvar twin are deleted.
- The FASL wire format for `FASL_TAG_LOCK` (tag, flags, name) does not
  change; the reader allocates a fresh, free lock. No `CL_FASL_VERSION`
  bump.

### Diagnostics

- `AMILISP_LOCK_DIAG` reports read the owner and depth from the object
  and use a *timed* park (the threshold, then the repeat cadence) only
  when the variable is set; otherwise parks are untimed.
- `MP:DUMP-THREAD-WAITS` prints the names of `wait_obj` / `wait_lock`
  instead of ids.
- `MP:ACQUIRE-LOCK` gains an optional third argument, a timeout in
  seconds (a timed park on the slow path), which bordeaux-threads v2's
  `acquire-lock :timeout` can map to. Cheap once parking is timed.

### Expected cost

Uncontended acquire: type check, one CAS, one store. Release: one CAS.
Today: type check, a table load, a pthread trylock (a CAS plus its own
bookkeeping), two stores into shared arrays, a barrier and a waiters read.
Contended: one token wakeup instead of a broadcast to every parked
thread. `MAKE-LOCK`: a 20-byte allocation instead of a mutex allocation,
a 16k scan under a global mutex, and a finalizer.

Measured before merging (the bench methodology of the Tier-4 entries:
same-session worktree base, minimum of 5): the lock rows of
`trunk/bench-prims.lisp`, the `mt.*` rows of `trunk/bench-opt.lisp`, and
the sento matrix — the shared/ask cell at the *default* queue cap is the
acceptance cell (it must run, and read at or above the cap-2000 figure of
28.7k msg/s).

## Implementation phases

1. **Park primitive, POSIX.** `platform_park_*` in `platform_thread_posix.c`
   (covers Windows through winpthreads), a handle created in thread
   start-up and destroyed on exit, `tests/test_thread_platform.c` cases:
   unpark-before-park is consumed by the next park, timed park times out,
   two unparks are one token.
2. **Locks.** New `CL_Lock` layout, acquire/release/trylock as above, the
   wait record and its GC roots, the diag on the object, deletion of
   `cl_lock_table`, `cl_lock_held`, `cl_lock_depth`, the parking-lot
   globals, the `TYPE_LOCK` finalizer, the shutdown sweep, the image
   install-at, the `CL_MAX_LOCKS` checks. `CL_IMAGE_VERSION` bump. Host
   gates green.
3. **Condition variables.** New `CL_CondVar` layout, wait/notify/broadcast
   with the timeout protocol, interrupt delivery by direct unpark, deletion
   of `cl_condvar_table` and its finalizer.
4. **Amiga and MorphOS park primitive.** The per-thread signal bit in
   `platform_thread_amiga.c`; the FS-UAE suite, then the real machines
   (Vampire, MorphOS) via their MCP runners.
5. **Validation and numbers.** The full gate list below, the benchmark
   rows, and a "Implementation notes" section appended to this spec with
   the measured deltas, as the generational-GC spec does.

Phases 1–3 leave the Amiga build compiling against a stub park (a
`Delay()`-poll fallback) so `master` never has a broken cross build
between phases.

## Tests

Existing suites that must stay green: `make test` (`tests/test_threads.c`
lock and condvar cases), `make test-gc-stress`, `make test-memleak`, the
`tests/test_mt_*.sh` set — in particular `test_mt_interrupt_parked.sh`
(interrupt and destroy of parked threads), `test_mt_lock_contention_throughput.sh`
(handoff is wakeup-driven, not sleep-polled), `test_mt_gc_compact_hang.sh`
and `test_mt_gc_regression.sh` — and the "MP" block of
`tests/amiga/run-tests.lisp`.

New, each a regression test for one clause of the design:

- **Cap gone**: 100,000 live locks in a vector, then release and collect
  (the sento shared/ask failure in miniature); host and Amiga (a smaller
  count on the 8 MB target).
- **Misuse errors**: release by a non-owner, re-acquire of a plain lock by
  its owner, condition-wait without the lock — each signals, none hangs.
- **Recursive depth across a wait**: hold at depth 2, wait, get notified,
  observe depth 2 and two releases needed.
- **One wakeup per release**: N parked waiters, N sequential releases,
  exactly N acquisitions and no waiter woken more than once (counts on
  both sides).
- **Lost-wakeup soak**: 8 threads ping-pong through a lock and a condvar
  with randomised notify timing for 10 s under the harness watchdog; a
  hang is the failure.
- **Barging race, deterministic** (`tests/test_mt_lock_barge_race.sh`,
  against the `DEBUG_THREAD_RACE_HOOKS` binary of
  `make test-mt-thread-exit-race`): the window between a waiter's
  registration and its CONTENDED mark is held open for 300 ms while the
  holder releases and re-acquires plain inside it; the waiter must still
  be woken by the final release.  See "Implementation notes" for the
  lost wakeup this pins.
- **Timeout racing notify**: a waiter with a 1 ms timeout and a notifier
  firing at the same instant, 10,000 rounds; invariant: every notify is
  either returned as T by that waiter or consumed by the next one.
- **Interrupt and destroy while parked** on a lock and on a condvar, timed
  and untimed (extend `test_mt_interrupt_parked.sh`): delivery within a
  bounded time, no backstop to hide behind.
- **Compaction while parked** (gc-stress): threads parked on a lock and a
  condvar while the main thread forces compactions with
  `AMILISP_GC_STRESS=1`; after the wake the woken thread's lock is the
  forwarded object and the waiter count is zero.
- **Off-heap**: one million `make-lock` / `make-condition-variable` calls
  leave nothing outstanding in `tests/test_memleak_tracked.sh`.
- **Image round trip**: an image saved with a recursive lock held at
  depth 2 and a condvar that has been waited on and notified restores
  with the lock free, zero waiters, and both usable.
- **Amiga**: the same lock/condvar block in `run-tests.lisp`, plus a
  parked-then-destroyed worker; and the per-thread signal bit is freed at
  thread exit (`AvailMem` / signal-bit count stable across 1,000 thread
  create-exit cycles).

## Risks

- **Lost wakeups.** The whole design rests on the token semantics of the
  park primitive plus the re-check after registration. Both are stated
  above as invariants and each has a test; the review should read the
  acquire and wait pseudo-code against the futex literature ("Futexes Are
  Tricky", mutex 2) rather than trust the prose.
- **Timeout vs notify**: resolved under `cl_thread_list_lock`; the test
  above is the guard.
- **Lock-ordering with `cl_thread_list_lock`.** Registration, the
  release scan and notify take it; nothing may park or block on the heap
  while holding it (today's rule, kept). The STW request loop also takes
  it briefly and releases it before parking on `gc_condvar`, so no new
  ordering is introduced.
- **The interrupt path** loses its backstop. Direct unpark is stronger,
  but a target that has published `interrupt_pending` and is between its
  pre-park check and the park must not lose the token: the publisher's
  unpark is ordered after the target's registration (both under the list
  lock), and an early token is consumed by the park. Test: the extended
  `test_mt_interrupt_parked.sh`.
- **Layout sensitivity of `CL_Thread`.** New fields go at the end;
  bench-prims call rows are the detector (phase-1 notes).
- **Amiga timed waits** keep 50 ms `Delay()` granularity; this is not a
  regression (today's condvar timed wait has the same), but it caps how
  precise `condition-wait :timeout` and `acquire-lock` timeouts are there.
- **Semantic changes**: errors on non-owner release and on re-acquiring a
  plain lock, and depth restoration across a wait. All three replace
  undefined behaviour or a platform accident; each is called out in
  README's MP section when the change lands.

## Decisions (were: open questions)

1. **Spin length.** Host keeps 256 yields for this change so the
   measured delta isolates the table removal, but the spin's *shape*
   changed: it reads the state word and only CASes when it reads free,
   so spinners do not bounce the lock's cache line (today's trylock is a
   CAS per iteration).  AmigaOS / MorphOS spin zero times: a
   `Forbid()`/`Permit()` yield only reschedules when a task switch is
   already pending, so spinning cannot hand the single CPU to the
   holder.  SBCL's futex path spins zero times too.  Revisit 64 vs 256
   on the host with the contention test's hammer probe if a profile
   ever shows the spin.
2. **timer.device.** Deferred.  Sento's throughput path takes no timed
   wait (its message-box and queue waits are untimed, so they block in
   `Wait()` and wake on the `Signal()` itself); only `ask-s` with a
   timeout, remoting and `with-timeout` hit the `Delay()` poll, whose
   real cost is a notify seen up to 50 ms late — acceptable on a timeout
   path.  If a workload needs it: a MsgPort plus timerequest per thread,
   opened lazily on the first timed park, `Wait(parkbit | portbit)`,
   closed at thread exit (the memleak gate applies).
3. **Exit-path lock release.** No, and no follow-up.  Destroy already
   unwinds through `unwind-protect`, so a lock taken by the normal idiom
   is released when its thread is destroyed; only a raw acquire with no
   cleanup in a thread that exits is affected, and that is a program
   bug.  Releasing at exit would hand the lock to a waiter while the
   protected state is half-updated — silent corruption is worse than a
   hang the lock diagnostic explains ("held by an already-exited
   thread"), which is why POSIX robust mutexes report the dead owner
   instead of unlocking.  And with the tables gone there is no record of
   which locks a thread holds, so it would cost a per-thread held list
   on every acquire.
4. **`acquire-lock` timeout.** `(mp:acquire-lock lock &optional (wait-p
   t) timeout)`, timeout a non-negative real in seconds — the unit and
   conversion `condition-wait` already uses.  NIL waits forever, 0 makes
   one attempt, a timeout with `wait-p` NIL is ignored (bordeaux-threads'
   contract), NIL is returned on timeout.  The fork's `%acquire-lock`
   takes `(lock waitp timeout)` positionally and maps one to one; its
   four `mark-not-implemented` entries and the README's known-limitation
   line go away with this change.

Two corrections made while implementing: ECL, not SBCL, is the
implementation that errors on both misuse cases (SBCL errors on a
recursive attempt but by default silently ignores a non-owner release);
and a saved image cannot carry a registered condvar waiter at all, since
`SAVE-IMAGE` refuses while worker threads run — the image test saves a
held recursive lock and a condvar that has been waited on and notified.

The prior art, read from the sources before deciding: SBCL's mutex is a
struct with a 32-bit state word (free / taken / contended; newer builds
pack the owner tid into it), Drepper's "mutex 2" on a futex keyed by the
word's address with the object pinned during the wait; Linux, FreeBSD,
macOS and Windows have the futex backend, and where none exists SBCL
falls back to a CAS on the owner plus a spin/yield/sleep poll with a
linked list of waiting threads per condvar.  ECL had this design from
2012 (CAS mutexes and user-space wait queues) and replaced it in
September 2020 with a raw `pthread_mutex_t` embedded inline in the heap
object plus a Boehm finalizer — possible only because Boehm never moves
objects.  That model is closed to a compacting arena (a locked pthread
mutex has waiters queued on its address; an Amiga `SignalSemaphore`
carries a linked list of waiting tasks inside itself), which is why the
id table existed and why this spec follows SBCL's shape — the non-futex
one, since AmigaOS has no address-keyed wait.

## Implementation notes (2026-09-10)

Landed in one change on top of `f4cbcb41`, phases 1–4 together (the
Amiga park primitive shipped with the rest; no Delay()-poll stub was
ever on master).

**What the code does that the design text did not spell out.**

- Registration comes *before* the CONTENDED mark, and the mark goes on
  whoever holds the lock at that moment.  The first version marked
  first, registered second, and re-checked only `state == 0` before
  parking.  That lost a wakeup in three steps: the marked holder
  released and scanned before the waiter was listed (nobody to wake), a
  spinning newcomer took the free lock with a plain word, the waiter —
  now registered — saw "held" and parked, and the newcomer's release,
  seeing no bit, never scanned.  A thread was left parked on a *free*
  lock.  Every host gate, the 8-thread soaks and the Amiga suite passed
  that version; only the sento shared/ask-s cell hung, thirty seconds
  in, at 0% CPU.  The fix is the loop in `lock_acquire_slow`: after
  registering, read the state; free → do not park; held without the bit
  → CAS it on (retry on failure); then park.  That is
  `futex_wait(&val, 2)`'s own re-check, done by hand.  Pinned by
  `tests/test_mt_lock_barge_race.sh` through a race hook
  (`AMILISP_RACE_LOCK_MARK_DELAY_MS`, `DEBUG_THREAD_RACE_HOOKS` build);
  ordinary timing soaks from Lisp did not reproduce it in minutes.
- The contended release stores 0 with a release fence *before* the
  store (and the fence after it that orders the store against the
  waiter scan).  The first version had only the second fence; on the
  ARM64 host the plain store could become visible before the critical
  section's own stores, and the next owner — whose CAS is an acquire —
  read stale data.  The barging soak in `tests/test_mp_heap_locks.sh`
  lost exactly one protected `incf` in 320,000 that way, once; it now
  also stamps a shared cell on entry and checks it before release.  The
  uncontended release is a CAS and had the fence built in.
- A waiter that abandons a contended acquire (timeout, or an interrupt
  about to unwind it) hands the lock on: if the lock is free it wakes
  another registered waiter, if it is held without the bit it sets the
  bit, so the release that woke it cannot strand the others
  (`lock_abandon_wait`).
- The spin phase reaches a safepoint (`cl_gc_safepoint` when
  `gc_requested`) so a peer's stop-the-world does not wait out 256
  yields; the parked phase is a safe region as designed.
- The owner identity in `state` is a process-unique thread *serial*
  (`CL_Thread.serial`, main = 1), not the table slot `id`, so a lock
  left held by an exited thread can never read as owned by a later
  thread that got its slot.
- Timed condition waits loop on a deadline: a stale park token (an
  unpark that raced a timeout) returns the park early and the loop
  re-parks for the remainder, so a token never shortens a timeout.
- `SAVE-IMAGE` refuses while workers run, so an image cannot carry a
  registered waiter; the image test saves a recursive lock held at
  depth 2 and a condvar that was waited on and notified.
- `AMILISP_LOCK_DIAG` reports name the lock (no id any more) and find the
  holder by serial; `MP:DUMP-THREAD-WAITS` prints wait-object names.
- Test hooks `mp::%condvar-waiters` and `mp::%lock-held-p` (internal).
- The bordeaux-threads fork's `%acquire-lock`, `%acquire-recursive-lock`
  and both `%with-lock` macros pass the timeout through
  (`~/quicklisp/local-projects/bordeaux-threads/apiv2/impl-clamiga.lisp`,
  edited alongside this change).

**Measured** (host: Apple M-series, 28 cores, same-session A/B against a
detached worktree of `f4cbcb41`; the base binary's `ACQUIRE-LOCK` takes
two arguments, so its sento leg loads the fork's committed
bordeaux-threads via `ATOMICS_DIR`).

`trunk/bench-prims.lisp`, ns per iteration, minimum of 5 runs:

| row | base | new |
| --- | ---: | ---: |
| lock-acquire-release | 55 | 52 |
| lock+condvar-notify | 67 | 62 |
| empty-loop | 17 | 16 |

No call row moved by more than a nanosecond; `mt.call-x8` and
`mt.dynbind-x8` (`trunk/bench-opt.lisp`, minimum of 3) read 35 ms and
36 ms before and after — the new `CL_Thread` fields at the end of the
struct cost nothing.

Sento matrix (`trunk/sento-bench-matrix.lisp`, sento 3.4.5, cold
caches, speed 3, the driver's default queue cap of 10000), messages per
second (trivial-benchmark average):

| cell | base | new | Δ |
| --- | ---: | ---: | ---: |
| pinned/tell | 243,587 | 299,260 | +22.9% |
| pinned/ask-s | 108,365 | 121,118 | +11.8% |
| pinned/ask | 55,995 | 55,311 | −1.2% |
| shared/tell | 41,791 | 176,126 | +321% |
| shared/ask-s | 68,005 | 58,086 | −14.6% |
| shared/ask | aborts: lock table full | 45,424 | runs |

The acceptance cell runs at the default cap and reads well above the
28.7k the 0.10 entry got at cap 2000.  shared/tell is the thundering
herd gone: eight workers on one queue lock used to be broadcast awake
on every release.  The two cells that read down were re-measured in two
interleaved rounds on a quieter machine (warm caches): pinned/ask
57,933 → 61,085 and 53,280 → 53,938 (noise), shared/ask-s 68,211 →
65,531 and 70,647 → 66,422 (−4 to −6%, real).  shared/ask-s is the
timed-`ask-s` path on the shared dispatcher: every wait and notify now
takes `cl_thread_list_lock` (registration, wake resolution, the notify
scan), a process-wide mutex that sixteen threads bounce per message,
where the old code touched only the per-object pthread condvar.
Follow-up if it matters: a per-object waiter chain of thread serials
(head in the object, `next` in `CL_Thread`) would take the notify scan
and the registration off the global lock — SBCL's non-futex waitqueue
shape.

Amiga (FS-UAE, `make -f Makefile.cross test-amiga`): the suite passes
except the known FS-UAE-only audio miss; the thirteen new MP checks
pass, including sixty thread create/exit cycles handing their signal
bit back.  Soft-float and FPU=1 binaries build clean.  MorphOS shares
`platform_thread_amiga.c` and was not run.

Pre-existing on this machine, base and branch alike: `make
test-mt-thread-exit-race`'s start-up self-test reports "worker never
registered"; it is unrelated to this change (the race binary now
honours `AMILISP_RACE_SELFTEST=0` so it can run the barge test).

