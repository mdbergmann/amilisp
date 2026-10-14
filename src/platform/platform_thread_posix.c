/*
 * platform_thread_posix.c — POSIX threading primitives for AmiLisp.
 *
 * Thin wrappers around pthreads, GCC __sync_* atomics, and pthread TLS.
 */

#include "platform_thread.h"
#include "platform.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

/* ================================================================
 * Thread
 * ================================================================ */

/* Live OS-thread accounting (see platform_thread.h).  On POSIX a straggler
 * is harmless at exit (the process takes every thread with it), but the
 * count is kept here too so the drain is exercised by the host suites. */
static volatile uint32_t posix_live_threads = 0;
static volatile uint32_t posix_exit_delay_ms = 0;

typedef struct {
    void *(*func)(void *);
    void  *arg;
} PosixThreadStart;

static void *posix_thread_start(void *p)
{
    PosixThreadStart s = *(PosixThreadStart *)p;
    void *result;

    free(p);
    result = s.func(s.arg);
    if (posix_exit_delay_ms)
        platform_sleep_ms(posix_exit_delay_ms);
    __sync_fetch_and_sub(&posix_live_threads, 1);
    return result;
}

int platform_thread_create(void **handle, void *(*func)(void *), void *arg,
                           uint32_t stack_size)
{
    pthread_t *th;
    PosixThreadStart *s;
    pthread_attr_t attr;
    int ret;

    th = (pthread_t *)malloc(sizeof(pthread_t));
    if (!th) return -1;
    s = (PosixThreadStart *)malloc(sizeof(PosixThreadStart));
    if (!s) {
        free(th);
        return -1;
    }
    s->func = func;
    s->arg = arg;

    pthread_attr_init(&attr);
    if (stack_size > 0)
        pthread_attr_setstacksize(&attr, (size_t)stack_size);

    __sync_fetch_and_add(&posix_live_threads, 1);
    ret = pthread_create(th, &attr, posix_thread_start, s);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        __sync_fetch_and_sub(&posix_live_threads, 1);
        free(s);
        free(th);
        return -1;
    }
    *handle = th;
    return 0;
}

uint32_t platform_thread_live_count(void)
{
    return __sync_fetch_and_add(&posix_live_threads, 0);
}

uint32_t platform_thread_drain(const volatile uint32_t *keep, uint32_t timeout_ms)
{
    uint32_t start = platform_time_ms(), now, live;
    for (;;) {
        live = platform_thread_live_count();
        if (live <= *keep) return 0;
        /* Bound by the clock, not by summing the naps: Windows rounds a
         * 5 ms Sleep up to its 15.6 ms scheduler tick, and a counted 2 s
         * bound then held process exit for 6 s (and never reported the
         * straggler, whose tail had ended by then). */
        now = platform_time_ms();
        if (now < start) start = now;          /* clock wrapped */
        if (now - start >= timeout_ms) return live - *keep;
        platform_sleep_ms(5);
    }
}

uint32_t platform_thread_remove_stragglers(void)
{
    return 0;   /* _exit ends every thread of the process */
}

uint32_t platform_thread_set_exit_delay(uint32_t ms)
{
    uint32_t old = posix_exit_delay_ms;
    posix_exit_delay_ms = ms;
    return old;
}

int platform_thread_join(void *handle, void **result)
{
    pthread_t *th = (pthread_t *)handle;
    void *retval = NULL;
    int ret;

    ret = pthread_join(*th, &retval);
    free(th);

    if (result) *result = retval;
    return ret == 0 ? 0 : -1;
}

void platform_thread_detach(void *handle)
{
    pthread_t *th = (pthread_t *)handle;
    if (!th) return;
    /* pthread_detach is safe whether the thread is still running or has
     * already terminated: in the running case it flags the kernel to
     * reclaim resources at exit; in the terminated case it reaps now.
     * Either way no one can pthread_join *th afterwards, so freeing our
     * heap-allocated pthread_t wrapper is sound. */
    pthread_detach(*th);
    free(th);
}

void platform_thread_yield(void)
{
    sched_yield();
}

/* ================================================================
 * Mutex
 * ================================================================ */

int platform_mutex_init(void **handle)
{
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!m) return -1;

    if (pthread_mutex_init(m, NULL) != 0) {
        free(m);
        return -1;
    }
    *handle = m;
    return 0;
}

int platform_mutex_init_recursive(void **handle)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    if (!m) return -1;

    if (pthread_mutexattr_init(&attr) != 0) {
        free(m);
        return -1;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (pthread_mutex_init(m, &attr) != 0) {
        pthread_mutexattr_destroy(&attr);
        free(m);
        return -1;
    }
    pthread_mutexattr_destroy(&attr);
    *handle = m;
    return 0;
}

void platform_mutex_destroy(void *handle)
{
    pthread_mutex_t *m = (pthread_mutex_t *)handle;
    pthread_mutex_destroy(m);
    free(m);
}

void platform_mutex_lock(void *handle)
{
    pthread_mutex_lock((pthread_mutex_t *)handle);
}

void platform_mutex_unlock(void *handle)
{
    pthread_mutex_unlock((pthread_mutex_t *)handle);
}

int platform_mutex_trylock(void *handle)
{
    return pthread_mutex_trylock((pthread_mutex_t *)handle) == 0 ? 0 : 1;
}

/* ================================================================
 * Read-Write Lock
 * ================================================================ */

int platform_rwlock_init(void **handle)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t));
    if (!rw) return -1;

    if (pthread_rwlock_init(rw, NULL) != 0) {
        free(rw);
        return -1;
    }
    *handle = rw;
    return 0;
}

void platform_rwlock_destroy(void *handle)
{
    pthread_rwlock_t *rw = (pthread_rwlock_t *)handle;
    pthread_rwlock_destroy(rw);
    free(rw);
}

void platform_rwlock_rdlock(void *handle)
{
    pthread_rwlock_rdlock((pthread_rwlock_t *)handle);
}

void platform_rwlock_wrlock(void *handle)
{
    pthread_rwlock_wrlock((pthread_rwlock_t *)handle);
}

void platform_rwlock_unlock(void *handle)
{
    pthread_rwlock_unlock((pthread_rwlock_t *)handle);
}

/* ================================================================
 * Condition variable
 * ================================================================ */

int platform_condvar_init(void **handle)
{
    pthread_cond_t *cv = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
    if (!cv) return -1;

    if (pthread_cond_init(cv, NULL) != 0) {
        free(cv);
        return -1;
    }
    *handle = cv;
    return 0;
}

void platform_condvar_destroy(void *handle)
{
    pthread_cond_t *cv = (pthread_cond_t *)handle;
    pthread_cond_destroy(cv);
    free(cv);
}

void platform_condvar_wait(void *handle, void *mutex)
{
    pthread_cond_wait((pthread_cond_t *)handle, (pthread_mutex_t *)mutex);
}

int platform_condvar_wait_timeout(void *handle, void *mutex, uint32_t ms)
{
    struct timespec ts;
    struct timeval tv;
    int ret;

    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + (long)(ms / 1000);
    ts.tv_nsec = tv.tv_usec * 1000L + (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    ret = pthread_cond_timedwait((pthread_cond_t *)handle,
                                (pthread_mutex_t *)mutex, &ts);
    return ret == ETIMEDOUT ? 1 : 0;  /* 1 = timed out, 0 = signaled */
}

void platform_condvar_signal(void *handle)
{
    pthread_cond_signal((pthread_cond_t *)handle);
}

void platform_condvar_broadcast(void *handle)
{
    pthread_cond_broadcast((pthread_cond_t *)handle);
}

/* ================================================================
 * Per-thread parking (see platform_thread.h)
 * ================================================================ */

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    int             permit;   /* 1 = a token is waiting to be consumed */
} PosixPark;

int platform_park_init(void **handle)
{
    PosixPark *p = (PosixPark *)malloc(sizeof(PosixPark));
    if (!p) return -1;
    if (pthread_mutex_init(&p->mutex, NULL) != 0) {
        free(p);
        return -1;
    }
    if (pthread_cond_init(&p->cond, NULL) != 0) {
        pthread_mutex_destroy(&p->mutex);
        free(p);
        return -1;
    }
    p->permit = 0;
    *handle = p;
    return 0;
}

void platform_park_destroy(void *handle)
{
    PosixPark *p = (PosixPark *)handle;
    if (!p) return;
    pthread_cond_destroy(&p->cond);
    pthread_mutex_destroy(&p->mutex);
    free(p);
}

int platform_park(void *handle, uint32_t timeout_ms)
{
    PosixPark *p = (PosixPark *)handle;
    int timed_out = 0;

    pthread_mutex_lock(&p->mutex);
    if (timeout_ms == 0) {
        while (!p->permit)
            pthread_cond_wait(&p->cond, &p->mutex);
    } else {
        struct timespec ts;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec + (long)(timeout_ms / 1000);
        ts.tv_nsec = tv.tv_usec * 1000L + (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        while (!p->permit) {
            if (pthread_cond_timedwait(&p->cond, &p->mutex, &ts) == ETIMEDOUT) {
                /* A token deposited between the timeout and this re-check
                 * still counts: the permit test below decides. */
                timed_out = !p->permit;
                break;
            }
        }
    }
    if (p->permit) {
        p->permit = 0;   /* consume the token */
        timed_out = 0;
    }
    pthread_mutex_unlock(&p->mutex);
    return timed_out;
}

void platform_unpark(void *handle)
{
    PosixPark *p = (PosixPark *)handle;
    pthread_mutex_lock(&p->mutex);
    p->permit = 1;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mutex);
}

/* ================================================================
 * Atomics (GCC __sync builtins — available on all target GCC versions)
 * ================================================================ */

uint32_t platform_atomic_inc(volatile uint32_t *ptr)
{
    return __sync_add_and_fetch(ptr, 1);
}

uint32_t platform_atomic_dec(volatile uint32_t *ptr)
{
    return __sync_sub_and_fetch(ptr, 1);
}

int platform_atomic_cas(volatile uint32_t *ptr, uint32_t expected,
                        uint32_t desired)
{
    return __sync_bool_compare_and_swap(ptr, expected, desired) ? 1 : 0;
}

void platform_memory_barrier(void)
{
    __sync_synchronize();
}

/* ================================================================
 * TLS (Thread-Local Storage for cl_current_thread)
 *
 * Backed by `__thread` so the hot read path inlines to a single
 * TLS-register load.  The header exposes `cl_tls_thread_ptr` and
 * makes `platform_tls_get` a static inline that returns it.
 * ================================================================ */

__thread void *cl_tls_thread_ptr;

void platform_tls_init(void)
{
    /* Nothing to do — `__thread` storage is set up by the runtime. */
}

void platform_tls_set(void *value)
{
    cl_tls_thread_ptr = value;
}
