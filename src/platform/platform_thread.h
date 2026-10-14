#ifndef CL_PLATFORM_THREAD_H
#define CL_PLATFORM_THREAD_H

/*
 * Platform threading abstraction for AmiLisp.
 *
 * Host (POSIX): pthreads, __sync_* atomics, pthread_key_t TLS
 * Amiga (OS3+): Exec Tasks, SignalSemaphore, Forbid/Permit atomics, tc_UserData TLS
 *
 * Implementations: platform_thread_posix.c, platform_thread_amiga.c
 */

#include <stdint.h>

/* ---- Thread ---- */
int  platform_thread_create(void **handle, void *(*func)(void *), void *arg,
                            uint32_t stack_size);
int  platform_thread_join(void *handle, void **result);
/* Release OS-side resources without joining.  Caller must guarantee the
 * worker has already finished (status >= 2) so no further use of `handle`
 * will occur.  After this call, `handle` is invalid. */
void platform_thread_detach(void *handle);
void platform_thread_yield(void);

/* OS threads created by platform_thread_create that are still executing
 * amilisp code.  A worker leaves the MP registry (cl_thread_count) a few
 * steps BEFORE its OS thread is gone; this count drops only at the very end
 * of the platform entry wrapper.  On AmigaOS that gap is fatal at process
 * exit: the process's code is unloaded when main returns, and nothing kills
 * a straggling Exec task — it runs on into freed memory (MorphOS: "68k
 * exception" in CL-Thread). */
uint32_t platform_thread_live_count(void);
/* Wait (bounded, polling) until at most `*keep` OS threads are live.  `keep`
 * is re-read on every poll (not snapshotted once at the start) so a caller
 * whose "how many to keep" figure can itself change while draining — e.g.
 * cl_thread_count, when a worker being waited on unregisters mid-wait —
 * tracks the current value instead of one that is already stale.  Returns
 * how many beyond the final `*keep` are still live when it gives up (0 =
 * drained). */
uint32_t platform_thread_drain(const volatile uint32_t *keep, uint32_t timeout_ms);
/* Test hook: stall every worker this many ms after its function returned,
 * before it counts itself gone — widens the exit window above so tests can
 * hit it deterministically.  Returns the previous value. */
uint32_t platform_thread_set_exit_delay(uint32_t ms);
/* Last resort at process exit, after workers were asked to stop and did not:
 * on AmigaOS remove every worker task still live (RemTask under Forbid) so
 * none of them runs on into this program's unloaded code — their memory is
 * reclaimed with the task, any DOS or exec resource they held is lost.
 * Elsewhere the process exit ends every thread and this does nothing.
 * Returns how many tasks were removed. */
uint32_t platform_thread_remove_stragglers(void);

/* ---- Mutex ---- */
int  platform_mutex_init(void **handle);
int  platform_mutex_init_recursive(void **handle);
void platform_mutex_destroy(void *handle);
void platform_mutex_lock(void *handle);
void platform_mutex_unlock(void *handle);
int  platform_mutex_trylock(void *handle);   /* 0 = acquired, non-zero = busy */

/* ---- Read-Write Lock ---- */
int  platform_rwlock_init(void **handle);
void platform_rwlock_destroy(void *handle);
void platform_rwlock_rdlock(void *handle);
void platform_rwlock_wrlock(void *handle);
void platform_rwlock_unlock(void *handle);

/* ---- Condition variable ---- */
int  platform_condvar_init(void **handle);
void platform_condvar_destroy(void *handle);
void platform_condvar_wait(void *handle, void *mutex);
int  platform_condvar_wait_timeout(void *handle, void *mutex, uint32_t ms);
void platform_condvar_signal(void *handle);
void platform_condvar_broadcast(void *handle);

/* ---- Per-thread parking (token semantics) ----
 *
 * The blocking primitive behind the Lisp-visible MP locks and condition
 * variables (specs/mp-locks-heap-words.md).  A handle belongs to ONE
 * thread — the one that parks on it — and behaves like a binary
 * semaphore: `unpark` deposits a token, `park` blocks until a token is
 * present and consumes it.  An unpark that arrives BEFORE the matching
 * park is therefore never lost (the next park returns at once), which is
 * what lets a waiter register itself, re-check its condition and then
 * park without a timed backstop.  Two unparks are one token.
 *
 * park returns 0 when a token was consumed, 1 when timeout_ms elapsed
 * without one; timeout_ms == 0 waits forever.  Any thread may unpark;
 * only the owner parks, and only the owner may init/destroy its handle
 * (on AmigaOS the handle is a signal bit of the owning task).
 *
 * POSIX / Windows: a mutex, a condvar and a permit flag.  AmigaOS /
 * MorphOS: one AllocSignal() bit per thread; Wait() is the park,
 * Signal() the unpark, and a delivered signal stays set until consumed. */
int  platform_park_init(void **handle);
void platform_park_destroy(void *handle);
int  platform_park(void *handle, uint32_t timeout_ms);
void platform_unpark(void *handle);

/* ---- Atomics ---- */
uint32_t platform_atomic_inc(volatile uint32_t *ptr);
uint32_t platform_atomic_dec(volatile uint32_t *ptr);
int      platform_atomic_cas(volatile uint32_t *ptr, uint32_t expected,
                             uint32_t desired);  /* 1 = swapped, 0 = failed */
/* Full memory barrier for lock-free flag/payload pairs read outside any
 * mutex (e.g. interrupt_func published before interrupt_pending, consumed
 * at safepoints without taking a lock).  Both sides need it: the writer
 * orders payload-before-flag, the reader orders flag-before-payload — on
 * weakly-ordered hosts (ARM64) either reorder loses the payload.  On the
 * single-core 68k this reduces to a compiler barrier. */
void     platform_memory_barrier(void);

/* ---- TLS (for cl_current_thread) ----
 *
 * On POSIX, the per-thread CL_Thread* is held in a `__thread` variable so
 * the hot `cl_get_current_thread()` path is a single TLS load (e.g. an
 * `mrs tpidr_el0`-based access on ARM64) instead of a `pthread_getspecific`
 * libcall.  Profiling sento showed `pthread_getspecific` dominating the real
 * CPU work in multi-threaded mode (every `call_builtin`, `cl_gc_push_root`,
 * `cl_gc_pop_roots`, `cl_dynbind_restore_to`, `cl_alloc`, ... pays it).
 *
 * On Amiga, `tc_UserData` already serves this role with zero overhead, so
 * there `platform_tls_get` stays a tiny function call.
 */
void  platform_tls_init(void);
void  platform_tls_set(void *value);

#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)
extern __thread void *cl_tls_thread_ptr;
static inline void *platform_tls_get(void) { return cl_tls_thread_ptr; }
#else
void *platform_tls_get(void);
#endif

#endif /* CL_PLATFORM_THREAD_H */
