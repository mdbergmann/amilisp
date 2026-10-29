/*
 * stream.c — CL Stream heap type and output buffer management
 */

#include "stream.h"
#include "symbol.h"
#include "mem.h"
#include "error.h"
#include "package.h"
#include "vm.h"
#include "string_utils.h"
#include "../platform/platform.h"
#include "../platform/platform_thread.h"
#include "thread.h"   /* cl_gc_safe_mutex_lock, CL_MT */
#include <string.h>
#ifdef DEBUG_STREAM
#include <stdio.h>
#endif

/* Singleton console streams */
CL_Obj cl_stdin_stream = 0;
CL_Obj cl_stdout_stream = 0;
CL_Obj cl_stderr_stream = 0;
/* Bidirectional console stream backing *TERMINAL-IO* / *DEBUG-IO* /
 * *QUERY-IO* — CLHS specifies all three as bidirectional.  Reading resolves
 * to the same console input as *STANDARD-INPUT* (platform_getchar), writing
 * to the same console output as *STANDARD-OUTPUT*. */
static CL_Obj cl_terminal_io_stream = 0;

/* --- Output buffer side table --- */

typedef struct {
    char    *data;
    uint32_t capacity;
    uint32_t length;
    /* GC mark bit.  The outbuf side table is reclaimed by the GC driven by
     * STREAM LIVENESS, not by finalizing dead corpses: freeing a slot from a
     * dead stream's st->out_buf_handle is unsafe because that handle may
     * already have been reused by a *live* string-output-stream (handles are
     * recycled the moment a slot's data is freed).  Re-finalizing the stale
     * corpse would then free the live stream's buffer, silently truncating
     * its output (observed as chunga READ-LINE* returning "" for a non-empty
     * HTTP header under concurrent GC, desyncing drakma's response framing).
     * Instead, each GC clears these bits, marks the slot of every LIVE output
     * stream, then frees any slot still allocated but unmarked. */
    uint8_t  inuse;
    /* Creation-window pin.  A freshly allocated outbuf is not yet referenced
     * by any stream object, so the liveness-driven reclaim above would free
     * it out from under the stream being built — a window a *preemptive*
     * stop-the-world GC (AmigaOS task suspension) can hit between
     * cl_stream_alloc_outbuf returning the handle and the caller storing it
     * in st->out_buf_handle.  alloc pins the slot; the caller unpins once the
     * live, GC-reachable stream owns it.  Pinned slots are never reclaimed;
     * mark-begin does NOT clear pins. */
    uint8_t  pinned;
} CL_OutBuf;

/* The table grows on demand as a segmented directory of fixed-size blocks
 * (same pattern as cl_sock_lock_dir below): block 0 is static, further blocks
 * are lazily platform_alloc'd and NEVER freed or moved, so the I/O hot paths
 * (putchar/write/data/len) can index a slot without the table mutex.  Growth
 * replaces the old fixed-256-slot behavior of forcing a FULL stop-the-world
 * GC on every allocation once the table saturated — under logging-heavy
 * loads (one transient string-output-stream per printed node) that turned
 * printing into a GC storm (~33 full GCs/sec observed in eta-hab).  Dead
 * unclosed outbufs are still reclaimed by every normal GC via the mark-driven
 * sweep below, so the table only grows to the true peak of simultaneously
 * live + not-yet-collected buffers. */
#define CL_OUTBUF_BLOCK_SHIFT 8   /* log2(CL_STREAM_BUF_TABLE_SIZE) */
#define CL_OUTBUF_BLOCK_SIZE  CL_STREAM_BUF_TABLE_SIZE       /* 256 slots/block */
#define CL_OUTBUF_BLOCK_MASK  (CL_OUTBUF_BLOCK_SIZE - 1)
#define CL_OUTBUF_MAX_BLOCKS  64                    /* up to 16384 handles */
#if CL_STREAM_BUF_TABLE_SIZE != (1 << CL_OUTBUF_BLOCK_SHIFT)
#error "CL_OUTBUF_BLOCK_SHIFT must be log2(CL_STREAM_BUF_TABLE_SIZE)"
#endif

static CL_OutBuf outbuf_block0[CL_OUTBUF_BLOCK_SIZE];
static CL_OutBuf * volatile outbuf_dir[CL_OUTBUF_MAX_BLOCKS] = { outbuf_block0 };
/* Count of allocated blocks.  Grows only under cl_stream_table_mutex; the
 * matching outbuf_dir[] entry is published (fully zero-initialised) BEFORE
 * the count, so an unlocked reader that sees the new count also sees the
 * block pointer. */
static volatile uint32_t outbuf_nblocks = 1;

static int stream_initialized = 0;

/* Resolve a handle to its slot, or NULL for handle 0 / out of range.  Safe
 * without the table mutex: blocks are never freed or moved once published. */
static CL_OutBuf *outbuf_at(uint32_t handle)
{
    uint32_t blk = handle >> CL_OUTBUF_BLOCK_SHIFT;
    CL_OutBuf *b;
    if (handle == 0 || blk >= outbuf_nblocks) return NULL;
    b = outbuf_dir[blk];
    return b ? &b[handle & CL_OUTBUF_BLOCK_MASK] : NULL;
}

/* Occupancy snapshot for diagnostics (ext:%stream-outbuf-stats, DEBUG_STREAM).
 * Caller holds the table mutex for an exact count; without it the count is
 * approximate (fine for diagnostics). */
static void outbuf_stats(uint32_t *used_out, uint32_t *capacity_out)
{
    uint32_t blk, i, used = 0, nb = outbuf_nblocks;
    for (blk = 0; blk < nb; blk++) {
        CL_OutBuf *b = outbuf_dir[blk];
        if (!b) continue;
        for (i = 0; i < CL_OUTBUF_BLOCK_SIZE; i++)
            if (b[i].data) used++;
    }
    if (used_out) *used_out = used;
    if (capacity_out) *capacity_out = nb * CL_OUTBUF_BLOCK_SIZE - 1;
}

/* Mutex protecting outbuf and cbuf slot allocation/deallocation */
static void *cl_stream_table_mutex = NULL;

/* I/O serialisation locks.
 *
 * A single global lock is wrong here: it is held across the *blocking* read()/
 * write() syscalls (a socket read can park indefinitely waiting for the peer),
 * so one parked thread would wedge every other stream operation — including an
 * unrelated thread writing to the console, or the reply being written back to
 * the very socket that is being read.  That is the SLY :spawn deadlock.
 *
 * Locks are therefore split by stream and by direction, so a blocking syscall
 * only ever holds a lock that no independent operation needs:
 *   - console reads vs. writes are independent (a parked stdin read must not
 *     block another thread's stdout output);
 *   - each socket has its own read and write lock, keyed by socket handle, so
 *     reading a socket never blocks writing it (full-duplex), and one client
 *     connection never blocks another;
 *   - files share one lock (regular-file I/O does not park indefinitely).
 * The locks still serialise concurrent same-direction access, preserving
 * UTF-8 decode atomicity and preventing interleaved console output. */
static void *cl_console_read_mutex = NULL;
static void *cl_console_write_mutex = NULL;
static void *cl_file_io_mutex = NULL;

/* Per-socket read/write locks, keyed by socket handle.  The lock MUST be per
 * handle, not striped (handle % N): a thread parked in a blocking read on one
 * socket must never stall I/O on a different socket, and reading a socket must
 * never block writing it (full duplex) — striping would reintroduce exactly the
 * cross-connection blocking this split was built to avoid (the SLY deadlock).
 *
 * The POSIX socket table grows without a fixed ceiling, so this table grows to
 * match it: a segmented directory of lazily-allocated, never-freed blocks of
 * read/write mutex pairs.  Because blocks are never freed and a block pointer is
 * published only once (fully initialised), a looked-up lock stays valid for the
 * life of the process and the I/O hot path reads it without the table mutex.
 * Amiga handles stay within the first two blocks (handles 1..63). */
typedef struct {
    void *read_mutex;
    void *write_mutex;
} SockLockPair;

#define CL_SOCK_LOCK_BLOCK_SHIFT 5
#define CL_SOCK_LOCK_BLOCK_SIZE  (1 << CL_SOCK_LOCK_BLOCK_SHIFT)   /* 32 handles/block */
#define CL_SOCK_LOCK_BLOCK_MASK  (CL_SOCK_LOCK_BLOCK_SIZE - 1)
#define CL_SOCK_LOCK_MAX_BLOCKS  2048                              /* up to 65536 handles */

static SockLockPair * volatile cl_sock_lock_dir[CL_SOCK_LOCK_MAX_BLOCKS];

/* Resolve a socket handle to its lock pair, lazily allocating and initialising
 * the covering block on first use (under the table mutex, so two threads racing
 * the same new block don't double-allocate it).  Returns NULL for the reserved
 * handle 0, an out-of-range handle, or on allocation failure (caller then runs
 * unlocked, as it did before any locks existed). */
static SockLockPair *sock_lock_pair(uint32_t h)
{
    uint32_t blk = h >> CL_SOCK_LOCK_BLOCK_SHIFT;
    SockLockPair *b;
    if (h == 0 || blk >= CL_SOCK_LOCK_MAX_BLOCKS) return NULL;
    b = cl_sock_lock_dir[blk];
    if (!b) {
        platform_mutex_lock(cl_stream_table_mutex);
        b = cl_sock_lock_dir[blk];          /* re-check after acquiring the lock */
        if (!b) {
            b = (SockLockPair *)platform_alloc((uint32_t)(CL_SOCK_LOCK_BLOCK_SIZE *
                                                          sizeof(SockLockPair)));
            if (b) {
                int i;
                for (i = 0; i < CL_SOCK_LOCK_BLOCK_SIZE; i++) {
                    b[i].read_mutex = NULL;
                    b[i].write_mutex = NULL;
                    platform_mutex_init(&b[i].read_mutex);
                    platform_mutex_init(&b[i].write_mutex);
                }
                cl_sock_lock_dir[blk] = b;  /* publish only after full init */
            }
        }
        platform_mutex_unlock(cl_stream_table_mutex);
        if (!b) return NULL;
    }
    return &b[h & CL_SOCK_LOCK_BLOCK_MASK];
}

/* --- C-buffer input stream side table --- */

/* One slot per LOAD / COMPILE-FILE / script file being read, so the table is
 * as deep as LOADs nest.  Same segmented directory as the outbuf table above:
 * block 0 is static (seven slots cover every ordinary run), further blocks
 * are platform_alloc'd on demand and never freed or moved before process
 * exit, so the read paths index a slot without the table mutex.  What bounds
 * LOAD nesting is the per-thread error-frame stack (CL_MAX_ERROR_FRAMES) or
 * the C stack, not this table; the directory's ceiling is for all threads
 * together. */
typedef struct {
    const char *data;
    uint32_t len;
} CL_CBufSlot;

#define CL_CBUF_BLOCK_SHIFT 3
#define CL_CBUF_BLOCK_SIZE  (1 << CL_CBUF_BLOCK_SHIFT)
#define CL_CBUF_BLOCK_MASK  (CL_CBUF_BLOCK_SIZE - 1)
#define CL_CBUF_MAX_BLOCKS  32                      /* up to 255 handles */

static CL_CBufSlot cbuf_block0[CL_CBUF_BLOCK_SIZE];
static CL_CBufSlot * volatile cbuf_dir[CL_CBUF_MAX_BLOCKS] = { cbuf_block0 };
/* Grows only under cl_stream_table_mutex; the block is published (zeroed)
 * before the count, as for outbuf_nblocks. */
static volatile uint32_t cbuf_nblocks = 1;

/* Resolve a handle to its slot, or NULL for handle 0 / out of range. */
static CL_CBufSlot *cbuf_at(uint32_t handle)
{
    uint32_t blk = handle >> CL_CBUF_BLOCK_SHIFT;
    CL_CBufSlot *b;
    if (handle == 0 || blk >= cbuf_nblocks) return NULL;
    b = cbuf_dir[blk];
    return b ? &b[handle & CL_CBUF_BLOCK_MASK] : NULL;
}

void cl_stream_init(void)
{
    int i;
    CL_Symbol *s;

    if (!cl_stream_table_mutex)
        platform_mutex_init(&cl_stream_table_mutex);
    if (!cl_console_read_mutex)
        platform_mutex_init(&cl_console_read_mutex);
    if (!cl_console_write_mutex)
        platform_mutex_init(&cl_console_write_mutex);
    if (!cl_file_io_mutex)
        platform_mutex_init(&cl_file_io_mutex);
    /* Per-socket lock blocks are allocated lazily on first use (sock_lock_pair). */

    /* Reset every slot in every allocated block (re-init keeps the blocks —
     * they are never freed — but must not carry stale entries; see the
     * shared-C-global re-init corruption class). */
    {
        uint32_t blk;
        for (blk = 0; blk < outbuf_nblocks; blk++) {
            CL_OutBuf *b = outbuf_dir[blk];
            if (!b) continue;
            for (i = 0; i < CL_OUTBUF_BLOCK_SIZE; i++) {
                b[i].data = NULL;
                b[i].capacity = 0;
                b[i].length = 0;
                b[i].inuse = 0;
                b[i].pinned = 0;
            }
        }
    }
    stream_initialized = 1;

    /* Create singleton console streams */
    cl_stdin_stream = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CONSOLE);
    CL_GC_PROTECT(cl_stdin_stream);

    cl_stdout_stream = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_CONSOLE);
    CL_GC_PROTECT(cl_stdout_stream);

    cl_stderr_stream = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_CONSOLE);
    CL_GC_PROTECT(cl_stderr_stream);

    /* *TERMINAL-IO* was historically bound to the OUTPUT-only stdout
     * singleton, which made (read-char t) — and any TUI reading from
     * *terminal-io* (cl-tuition) — see instant EOF.  A bidirectional
     * stream fixes that, but it must not be an independent CL_STREAM_CONSOLE
     * object: UNREAD-CHAR pushback (CL_Stream.unread_char) is per-stream-object
     * state, and a separate console object would make a char pushed back via
     * *STANDARD-INPUT* invisible to *TERMINAL-IO* (and vice versa) — silent
     * data loss, and LISTEN/READ-CHAR would then block despite a pending
     * char.  Make it a two-way-stream over the SAME stdin/stdout objects so
     * reads and writes resolve (via resolve_stream) to the identical
     * CL_Stream backing *STANDARD-INPUT* and *STANDARD-OUTPUT* — one shared
     * unread_char, one shared EOF flag, one shared position. */
    {
        CL_Stream *tst;
        cl_terminal_io_stream = cl_make_stream(CL_STREAM_IO, CL_STREAM_TWO_WAY);
        /* cl_stdin_stream/cl_stdout_stream are still CL_GC_PROTECT'd above,
         * so the allocation just above has already forwarded them if it
         * triggered compaction — safe to read the globals now. */
        tst = (CL_Stream *)CL_OBJ_TO_PTR(cl_terminal_io_stream);
        tst->string_buf = cl_stdin_stream;     /* input child */
        tst->element_type = cl_stdout_stream;  /* output child */
    }
    CL_GC_UNPROTECT(3);

    /* The C runtime writes through these cached handles directly
     * (platform console I/O, REPL banner, error output).  The stream
     * objects stay live via the *STANDARD-INPUT* / *STANDARD-OUTPUT* /
     * *ERROR-OUTPUT* symbol values, but the C globals hold raw offsets the compactor
     * only rewrites for registered roots — unregistered, the first
     * compaction that relocates a console stream leaves them pointing
     * into whatever moved there.  Registration is idempotent across
     * re-init. */
    cl_gc_register_root(&cl_stdin_stream);
    cl_gc_register_root(&cl_stdout_stream);
    cl_gc_register_root(&cl_stderr_stream);
    cl_gc_register_root(&cl_terminal_io_stream);

    /* Bind standard stream variables to console streams */
    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STANDARD_INPUT);
    s->value = cl_stdin_stream;

    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_STANDARD_OUTPUT);
    s->value = cl_stdout_stream;

    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_ERROR_OUTPUT);
    s->value = cl_stderr_stream;

    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_TRACE_OUTPUT);
    s->value = cl_stdout_stream;

    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_DEBUG_IO);
    s->value = cl_terminal_io_stream;

    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_QUERY_IO);
    s->value = cl_terminal_io_stream;

    s = (CL_Symbol *)CL_OBJ_TO_PTR(SYM_TERMINAL_IO);
    s->value = cl_terminal_io_stream;
}

void cl_stream_shutdown(void)
{
    uint32_t blk, i;
    /* Free the buffers but keep the directory blocks: they may be indexed by
     * a straggler and are reused verbatim after a re-init. */
    for (blk = 0; blk < outbuf_nblocks; blk++) {
        CL_OutBuf *b = outbuf_dir[blk];
        if (!b) continue;
        for (i = 0; i < CL_OUTBUF_BLOCK_SIZE; i++) {
            if (b[i].data) {
                platform_free(b[i].data);
                b[i].data = NULL;
            }
        }
    }
    stream_initialized = 0;
}

void cl_stream_release_tables(void)
{
    uint32_t blk;
    int i;

    /* Output-buffer directory.  cl_stream_shutdown deliberately keeps these
     * blocks so a re-init can reuse them verbatim; at process exit there is
     * no re-init, and each block is 256 slots (a few KB) of platform_alloc'd
     * memory the OS will not reclaim on AmigaOS.  Block 0 is static BSS —
     * only blocks 1.. were allocated. */
    for (blk = 1; blk < outbuf_nblocks; blk++) {
        CL_OutBuf *b = outbuf_dir[blk];
        if (!b) continue;
        for (i = 0; i < CL_OUTBUF_BLOCK_SIZE; i++)
            if (b[i].data) { platform_free(b[i].data); b[i].data = NULL; }
        outbuf_dir[blk] = NULL;
        platform_free(b);
    }
    outbuf_nblocks = 1;

    /* C-buffer stream blocks past the static one (deeply nested LOADs). */
    for (blk = 1; blk < cbuf_nblocks; blk++) {
        CL_CBufSlot *cb = cbuf_dir[blk];
        if (!cb) continue;
        cbuf_dir[blk] = NULL;
        platform_free(cb);
    }
    cbuf_nblocks = 1;

    /* Per-socket lock blocks: never freed during the run by design (a looked-up
     * lock must stay valid for the life of the process, unlocked readers and
     * all).  Each block carries 2 x 32 OS mutexes — on AmigaOS that is 64
     * SignalSemaphores per block. */
    for (blk = 0; blk < CL_SOCK_LOCK_MAX_BLOCKS; blk++) {
        SockLockPair *b = cl_sock_lock_dir[blk];
        if (!b) continue;
        cl_sock_lock_dir[blk] = NULL;
        for (i = 0; i < CL_SOCK_LOCK_BLOCK_SIZE; i++) {
            if (b[i].read_mutex)  platform_mutex_destroy(b[i].read_mutex);
            if (b[i].write_mutex) platform_mutex_destroy(b[i].write_mutex);
        }
        platform_free(b);
    }

    /* Module locks — all lazily created by cl_stream_init, so NULLing them
     * here leaves the module able to re-initialize cleanly. */
    if (cl_stream_table_mutex)  { platform_mutex_destroy(cl_stream_table_mutex);  cl_stream_table_mutex = NULL; }
    if (cl_console_read_mutex)  { platform_mutex_destroy(cl_console_read_mutex);  cl_console_read_mutex = NULL; }
    if (cl_console_write_mutex) { platform_mutex_destroy(cl_console_write_mutex); cl_console_write_mutex = NULL; }
    if (cl_file_io_mutex)       { platform_mutex_destroy(cl_file_io_mutex);       cl_file_io_mutex = NULL; }
}

CL_Obj cl_make_stream(uint32_t direction, uint32_t stream_type)
{
    CL_Stream *st = (CL_Stream *)cl_alloc(TYPE_STREAM, sizeof(CL_Stream));
    if (!st) return CL_NIL;
    st->direction = direction;
    st->stream_type = stream_type;
    st->flags = CL_STREAM_FLAG_OPEN;
    st->handle_id = 0;
    st->string_buf = CL_NIL;
    st->position = 0;
    st->out_buf_handle = 0;
    st->out_buf_size = 0;
    st->out_buf_len = 0;
    st->unread_char = -1;
    st->element_type = CL_NIL;  /* Will be set to CHARACTER symbol later */
    st->charpos = 0;
    st->line = 1;
    st->read_timeout_ms = 0;
    st->write_timeout_ms = 0;
    return CL_PTR_TO_OBJ(st);
}

uint32_t cl_stream_alloc_outbuf(uint32_t initial_size)
{
    uint32_t blk, i, nb;
    int gc_ran = 0;
    char *data;
    /* Capture the multithread decision ONCE: cl_thread_count can drop from 2
     * to 1 (a peer worker exiting/joining) between the lock below and its
     * matching unlock.  Re-evaluating CL_MT() at each unlock site would then
     * skip the unlock of a mutex this thread locked, leaking cl_stream_table_mutex
     * locked forever — the next FORMAT/string-output-stream deadlocks on it.
     * Mirrors mem.c's `int multi = (cl_thread_count > 1)` idiom. */
    int mt = CL_MT();

    if (!stream_initialized) cl_stream_init();
    if (initial_size == 0) initial_size = 256;

    if (mt) platform_mutex_lock(cl_stream_table_mutex);
    for (;;) {
        nb = outbuf_nblocks;
        for (blk = 0; blk < nb; blk++) {
            CL_OutBuf *b = outbuf_dir[blk];
            /* Handle 0 (block 0, slot 0) is reserved as invalid */
            for (i = (blk == 0 ? 1 : 0); i < CL_OUTBUF_BLOCK_SIZE; i++) {
                if (b[i].data == NULL) {
                    data = (char *)platform_alloc(initial_size);
                    if (!data) {
                        if (mt) platform_mutex_unlock(cl_stream_table_mutex);
                        return 0;
                    }
                    b[i].data = data;
                    b[i].capacity = initial_size;
                    b[i].length = 0;
                    b[i].inuse = 0;
                    b[i].pinned = 1;  /* protect until the owning stream is set */
#ifdef DEBUG_STREAM
                    {
                        char dbg[80];
                        uint32_t used, cap;
                        outbuf_stats(&used, &cap);
                        snprintf(dbg, sizeof(dbg),
                                 "[STREAM] alloc slot %u (%u/%u used)\n",
                                 (unsigned)((blk << CL_OUTBUF_BLOCK_SHIFT) | i),
                                 (unsigned)used, (unsigned)cap);
                        platform_write_string(dbg);
                    }
#endif
                    if (mt) platform_mutex_unlock(cl_stream_table_mutex);
                    return (blk << CL_OUTBUF_BLOCK_SHIFT) | i;
                }
            }
        }
        /* All allocated slots occupied — grow the directory by one block.
         * Growing (instead of forcing a GC, as the fixed-size table did)
         * keeps a saturated table from turning every string-output-stream
         * creation into a full stop-the-world collection; dead unclosed
         * outbufs are still reclaimed by every normal GC. */
        if (nb < CL_OUTBUF_MAX_BLOCKS) {
            CL_OutBuf *b = (CL_OutBuf *)platform_alloc(
                (uint32_t)(CL_OUTBUF_BLOCK_SIZE * sizeof(CL_OutBuf)));
            if (b) {
                for (i = 0; i < CL_OUTBUF_BLOCK_SIZE; i++) {
                    b[i].data = NULL;
                    b[i].capacity = 0;
                    b[i].length = 0;
                    b[i].inuse = 0;
                    b[i].pinned = 0;
                }
                outbuf_dir[nb] = b;       /* publish only after full init */
                outbuf_nblocks = nb + 1;  /* count published after the block */
#ifdef DEBUG_STREAM
                {
                    char dbg[80];
                    snprintf(dbg, sizeof(dbg),
                             "[STREAM] outbuf table grown to %u blocks (%u slots)\n",
                             (unsigned)(nb + 1),
                             (unsigned)((nb + 1) * CL_OUTBUF_BLOCK_SIZE - 1));
                    platform_write_string(dbg);
                }
#endif
                continue;
            }
        }
        /* Directory exhausted (or block allocation failed): GC once to
         * reclaim dead streams' buffers, then rescan.  Only reached at
         * CL_OUTBUF_MAX_BLOCKS * CL_OUTBUF_BLOCK_SIZE simultaneously
         * allocated buffers or under OOM. */
        if (gc_ran) break;
        gc_ran = 1;
        {
            extern void cl_gc(void);
            if (mt) platform_mutex_unlock(cl_stream_table_mutex);
            cl_gc();
            if (mt) platform_mutex_lock(cl_stream_table_mutex);
        }
    }
    if (mt) platform_mutex_unlock(cl_stream_table_mutex);
#ifdef DEBUG_STREAM
    platform_write_string("[STREAM] outbuf table FULL after GC\n");
#endif
    return 0;  /* No free slots even after growth + GC */
}

/* Re-materialize an outbuf slot at a FIXED handle with the given contents —
 * the image-restore path (image.c): a restored string-output-stream's
 * out_buf_handle must resolve to the exact byte state it had at save time.
 * Runs single-threaded before any Lisp executes, so no table mutex.  Grows
 * the block directory as needed (any handle the saving process allocated is
 * reachable — same directory geometry, same build).  Returns 0 on success,
 * -1 on an out-of-range handle, allocation failure, or occupied slot. */
int cl_stream_outbuf_install_at(uint32_t handle, const char *data, uint32_t len)
{
    uint32_t blk = handle >> CL_OUTBUF_BLOCK_SHIFT;
    CL_OutBuf *slot;
    uint32_t cap;

    if (handle == 0 || blk >= CL_OUTBUF_MAX_BLOCKS) return -1;
    while (outbuf_nblocks <= blk) {
        CL_OutBuf *b = (CL_OutBuf *)platform_alloc(
            (uint32_t)(CL_OUTBUF_BLOCK_SIZE * sizeof(CL_OutBuf)));
        uint32_t i;
        if (!b) return -1;
        for (i = 0; i < CL_OUTBUF_BLOCK_SIZE; i++) {
            b[i].data = NULL;
            b[i].capacity = 0;
            b[i].length = 0;
            b[i].inuse = 0;
            b[i].pinned = 0;
        }
        outbuf_dir[outbuf_nblocks] = b;
        outbuf_nblocks = outbuf_nblocks + 1;
    }
    slot = outbuf_at(handle);
    if (!slot || slot->data) return -1;
    cap = len < 256 ? 256 : len;   /* same floor as cl_stream_alloc_outbuf */
    slot->data = (char *)platform_alloc(cap);
    if (!slot->data) return -1;
    if (len) memcpy(slot->data, data, len);
    slot->capacity = cap;
    slot->length = len;
    slot->inuse = 0;
    slot->pinned = 0;
    return 0;
}

void cl_stream_free_outbuf(uint32_t handle)
{
    /* The .data occupancy check must run INSIDE the table mutex: with the
     * old outside-the-lock check, two concurrent frees of the same handle
     * could both see non-NULL and double-platform_free the buffer (worse:
     * the slot may be re-claimed between them, freeing a live stream's
     * buffer). */
    CL_OutBuf *buf = outbuf_at(handle);
    if (buf) {
        /* Capture CL_MT() once — see cl_stream_alloc_outbuf: a live re-eval at
         * the unlock could skip it if a peer thread exits mid-critical-section,
         * leaking the mutex locked. */
        int mt = CL_MT();
        if (mt) platform_mutex_lock(cl_stream_table_mutex);
        if (!buf->data) {
            if (mt) platform_mutex_unlock(cl_stream_table_mutex);
            return;
        }
        platform_free(buf->data);
        buf->data = NULL;
        buf->capacity = 0;
        buf->length = 0;
        buf->pinned = 0;
#ifdef DEBUG_STREAM
        {
            char dbg[80];
            uint32_t used, cap;
            outbuf_stats(&used, &cap);
            snprintf(dbg, sizeof(dbg), "[STREAM] free  slot %u (%u/%u used)\n",
                     (unsigned)handle, (unsigned)used, (unsigned)cap);
            platform_write_string(dbg);
        }
#endif
        if (mt) platform_mutex_unlock(cl_stream_table_mutex);
    }
}

char *cl_stream_outbuf_data(uint32_t handle)
{
    CL_OutBuf *buf = outbuf_at(handle);
    return buf ? buf->data : NULL;
}

/* --- Mark-driven outbuf reclamation (see CL_OutBuf.inuse comment above) --- */

/* Called once at the start of every GC mark phase: clear the in-use bits. */
void cl_stream_outbuf_gc_mark_begin(void)
{
    uint32_t blk, i;
    for (blk = 0; blk < outbuf_nblocks; blk++) {
        CL_OutBuf *b = outbuf_dir[blk];
        if (!b) continue;
        for (i = 0; i < CL_OUTBUF_BLOCK_SIZE; i++)
            b[i].inuse = 0;
    }
}

/* Called while marking each LIVE output stream: pin its outbuf slot. */
void cl_stream_outbuf_gc_mark_use(uint32_t handle)
{
    CL_OutBuf *buf = outbuf_at(handle);
    if (buf) buf->inuse = 1;
}

/* Release the creation-window pin once a live stream owns the slot; from here
 * on the slot is protected by the mark phase (cl_stream_outbuf_gc_mark_use). */
void cl_stream_outbuf_unpin(uint32_t handle)
{
    CL_OutBuf *buf = outbuf_at(handle);
    if (buf) buf->pinned = 0;
}

/* Called after marking completes (world stopped, so no table mutex needed —
 * mirrors gc_finalize_dead): free every allocated slot that no live stream
 * pinned.  These belong to streams that became unreachable this cycle. */
void cl_stream_outbuf_gc_reclaim(void)
{
    uint32_t blk, i;
    for (blk = 0; blk < outbuf_nblocks; blk++) {
        CL_OutBuf *b = outbuf_dir[blk];
        if (!b) continue;
        for (i = (blk == 0 ? 1 : 0); i < CL_OUTBUF_BLOCK_SIZE; i++) {
            if (b[i].data && !b[i].inuse && !b[i].pinned) {
                platform_free(b[i].data);
                b[i].data = NULL;
                b[i].capacity = 0;
                b[i].length = 0;
            }
        }
    }
}

uint32_t cl_stream_outbuf_len(uint32_t handle)
{
    CL_OutBuf *buf = outbuf_at(handle);
    return buf ? buf->length : 0;
}

/* Enumerate allocated outbuf slots for the image writer (image.c): the
 * smallest in-use handle greater than AFTER, or 0 when exhausted.  The
 * writer runs single-threaded at a safe point right after a compaction,
 * whose mark-driven reclaim has already freed every slot no live stream
 * references — so "allocated" here means "referenced by a live stream". */
uint32_t cl_stream_outbuf_next_used(uint32_t after)
{
    uint32_t h;
    uint32_t cap = outbuf_nblocks * CL_OUTBUF_BLOCK_SIZE;
    for (h = after + 1; h < cap; h++) {
        CL_OutBuf *b = outbuf_at(h);
        if (b && b->data) return h;
    }
    return 0;
}

/* Diagnostics hook for ext:%stream-outbuf-stats (builtins_io.c). */
void cl_stream_outbuf_table_stats(uint32_t *used, uint32_t *capacity)
{
    outbuf_stats(used, capacity);
}

static void outbuf_grow(uint32_t handle, uint32_t needed)
{
    CL_OutBuf *buf = outbuf_at(handle);
    uint32_t new_cap;
    char *new_data;

    if (!buf || !buf->data) return;
    new_cap = buf->capacity;

    while (new_cap < needed)
        new_cap *= 2;

    new_data = (char *)platform_alloc(new_cap);
    if (!new_data) return;  /* Allocation failure — silently drop */
    memcpy(new_data, buf->data, buf->length);
    platform_free(buf->data);
    buf->data = new_data;
    buf->capacity = new_cap;
}

void cl_stream_outbuf_putchar(CL_Stream *st, int ch)
{
    uint32_t h = st->out_buf_handle;
    CL_OutBuf *buf = outbuf_at(h);
    if (!buf || !buf->data) return;

    if (buf->length + 1 >= buf->capacity)
        outbuf_grow(h, buf->length + 2);

    buf->data[buf->length++] = (char)ch;
    buf->data[buf->length] = '\0';
    st->out_buf_len = buf->length;
    st->out_buf_size = buf->capacity;
}

void cl_stream_outbuf_write(CL_Stream *st, const char *str)
{
    uint32_t h = st->out_buf_handle;
    uint32_t len;
    CL_OutBuf *buf = outbuf_at(h);
    if (!buf || !buf->data || !str) return;

    len = (uint32_t)strlen(str);
    if (buf->length + len >= buf->capacity)
        outbuf_grow(h, buf->length + len + 1);

    memcpy(buf->data + buf->length, str, len);
    buf->length += len;
    buf->data[buf->length] = '\0';
    st->out_buf_len = buf->length;
    st->out_buf_size = buf->capacity;
}

void cl_stream_outbuf_reset(uint32_t handle)
{
    CL_OutBuf *buf = outbuf_at(handle);
    if (buf && buf->data) {
        buf->length = 0;
        buf->data[0] = '\0';
    }
}

/* --- Synonym / two-way stream resolution --- */

static CL_Obj resolve_synonym(CL_Obj stream)
{
    CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    while (st->stream_type == CL_STREAM_SYNONYM) {
        CL_Obj sym = st->string_buf;  /* symbol stored here */
        stream = cl_symbol_value(sym);
        /* Require a real CL_Stream, like resolve_stream: the synonym's
         * symbol can be bound to anything — e.g. a Gray stream, which is
         * a CLOS instance (CL_Struct).  Treating that as a CL_Stream and
         * writing st-> fields into it corrupts the instance. */
        if (CL_NULL_P(stream) || !CL_STREAM_P(stream))
            return CL_NIL;
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    }
    return stream;
}

/* Resolve synonym and two-way stream wrappers to a concrete stream.
 * `writing` selects the output child of a two-way stream (else input child).
 * An echo stream unwraps to its output child for writing only: direct output
 * goes to that child unechoed (CLHS 21.1), but a READ must stop at the echo
 * wrapper itself — it is the wrapper that copies each element read to the
 * output child, and its own unread_char slot is what keeps a pushed-back
 * char from being echoed twice. */
static CL_Obj resolve_stream(CL_Obj stream, int writing)
{
    CL_Stream *st;
    if (CL_NULL_P(stream) || !CL_STREAM_P(stream)) return CL_NIL;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    for (;;) {
        if (st->stream_type == CL_STREAM_SYNONYM) {
            stream = cl_symbol_value(st->string_buf);
            if (CL_NULL_P(stream) || !CL_STREAM_P(stream))
                return CL_NIL;
            st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        } else if (st->stream_type == CL_STREAM_TWO_WAY ||
                   (st->stream_type == CL_STREAM_ECHO && writing)) {
            stream = writing ? st->element_type : st->string_buf;
            if (CL_NULL_P(stream) || !CL_STREAM_P(stream))
                return CL_NIL;
            st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        } else {
            return stream;
        }
    }
}

/* Public wrapper around resolve_stream for builtins that must inspect
 * low-level CL_Stream state directly (LISTEN, READ-CHAR-NO-HANG,
 * INTERACTIVE-STREAM-P) instead of going through cl_stream_read_char/
 * cl_stream_write_char.  Those builtins must unwrap synonym-stream /
 * two-way-stream designators (e.g. *TERMINAL-IO*, see cl_stream_init) to the
 * concrete backing stream, or they inspect fields (unread_char, flags,
 * stream_type) on the wrapper object instead of the child that actually
 * carries the state.  `writing` selects the output child of a two-way
 * stream (else input child); returns CL_NIL if stream doesn't resolve. */
CL_Obj cl_stream_resolve_backing(CL_Obj stream, int writing)
{
    return resolve_stream(stream, writing);
}

/* --- Stream I/O operations --- */

/* Return the I/O serialisation lock for `st` in the given direction, or NULL
 * when no lock is needed (single-threaded, an in-memory stream type, or an
 * out-of-range socket handle).  `writing` selects the write lock; otherwise the
 * read lock.  See the lock declarations above for why locks are split this way. */
static void *stream_lock_for(CL_Stream *st, int writing)
{
    if (!CL_MT()) return NULL;
    switch (st->stream_type) {
    case CL_STREAM_CONSOLE:
        return writing ? cl_console_write_mutex : cl_console_read_mutex;
    case CL_STREAM_FILE:
        return cl_file_io_mutex;
    case CL_STREAM_SOCKET: {
        SockLockPair *p = sock_lock_pair(st->handle_id);
        if (!p) return NULL;
        return writing ? p->write_mutex : p->read_mutex;
    }
    default:
        return NULL;
    }
}

/* Read a single raw byte from a byte-oriented stream.
 * Caller must hold the I/O mutex if needed.
 * Does NOT handle UTF-8 decoding — use cl_stream_read_char for that. */
static int stream_read_raw_byte(CL_Stream *st)
{
    switch (st->stream_type) {
    case CL_STREAM_CONSOLE:
        return platform_getchar();
    case CL_STREAM_FILE:
        return platform_file_getchar((PlatformFile)st->handle_id);
    case CL_STREAM_CBUF: {
        CL_CBufSlot *cb = cbuf_at(st->handle_id);
        if (!cb || !cb->data)
            return -1;
        if (st->position >= st->out_buf_len)
            return -1;
        return (unsigned char)cb->data[st->position++];
    }
    case CL_STREAM_SOCKET:
        return platform_socket_read((PlatformSocket)st->handle_id);
    default:
        return -1;
    }
}

/* Raise EXT:SOCKET-TIMEOUT when a socket read/write missed its deadline.
 * cl_error longjmps, so callers MUST release any held iolock first.
 * timeout_ms must be captured from st->read/write_timeout_ms BEFORE any
 * platform call that enters a GC safe region, since compaction can move st. */
static CL_NORETURN void stream_raise_timeout(int is_write, uint32_t timeout_ms)
{
    cl_error(CL_ERR_TIMEOUT, "%s on socket stream timed out after %u ms",
             is_write ? "WRITE" : "READ",
             (unsigned)timeout_ms);
}

#ifdef CL_WIDE_STRINGS
/* Decode a UTF-8 character from a byte-oriented stream.
 * first_byte is already read; reads continuation bytes as needed.
 * Takes the stream as a CL_Obj (not a CL_Stream *) because each
 * continuation-byte read may block inside a GC safe region, where a
 * concurrent STW-GC can compact the arena and relocate the stream object —
 * so the pointer is re-derived from the (GC-protected) offset on every read. */
static int stream_decode_utf8(CL_Obj stream, int first_byte)
{
    unsigned char b0 = (unsigned char)first_byte;
    int cp, expected, i;

    if (b0 <= 0x7F)
        return b0;

    if ((b0 & 0xE0) == 0xC0) {
        cp = b0 & 0x1F;
        expected = 2;
    } else if ((b0 & 0xF0) == 0xE0) {
        cp = b0 & 0x0F;
        expected = 3;
    } else if ((b0 & 0xF8) == 0xF0) {
        cp = b0 & 0x07;
        expected = 4;
    } else {
        return 0xFFFD;  /* Invalid lead byte → replacement char */
    }

    for (i = 1; i < expected; i++) {
        int b;
        CL_GC_PROTECT(stream);   /* read may compact; forward the offset */
        b = stream_read_raw_byte((CL_Stream *)CL_OBJ_TO_PTR(stream));
        CL_GC_UNPROTECT(1);
        if (b == -1 || (b & 0xC0) != 0x80) {
            return 0xFFFD;  /* Truncated or invalid continuation */
        }
        cp = (cp << 6) | (b & 0x3F);
    }

    /* Reject overlong encodings */
    if ((expected == 2 && cp < 0x80) ||
        (expected == 3 && cp < 0x800) ||
        (expected == 4 && cp < 0x10000))
        return 0xFFFD;

    /* Reject surrogates and out-of-range */
    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return 0xFFFD;

    return cp;
}
#endif /* CL_WIDE_STRINGS */

int cl_stream_read_char(CL_Obj stream)
{
    CL_Stream *st;
    int ch;
    void *iolock;

    stream = resolve_stream(stream, 0);
    if (CL_NULL_P(stream)) return -1;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    if (!(st->flags & CL_STREAM_FLAG_OPEN))
        return -1;
    if (!(st->direction & CL_STREAM_INPUT))
        return -1;

    iolock = stream_lock_for(st, 0);
    /* cl_gc_safe_mutex_lock's contended path enters a GC safe region, so a
     * peer's STW-GC can compact and relocate st while we're blocked here.
     * Protect the offset across the call and re-derive st before touching
     * any st-> field. */
    CL_GC_PROTECT(stream);
    cl_gc_safe_mutex_lock(iolock);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    CL_GC_UNPROTECT(1);

    /* Check for pushed-back character (already decoded code point).  On an
     * echo stream this slot only ever holds a char that was read through the
     * wrapper (and therefore already echoed) and then UNREAD-CHARed — so
     * handing it back without echoing is exactly the CLHS 21.1 rule that a
     * character unread and re-read is echoed only once. */
    if (st->unread_char != -1) {
        ch = st->unread_char;
        st->unread_char = -1;
        if (iolock) platform_mutex_unlock(iolock);
        return ch;
    }

    /* Echo streams: take the element from the input child and copy it to the
     * output child (CLHS 21.1).  Both the child read and the child write can
     * block inside a GC safe region (console/socket) and let a peer STW-GC
     * compact the arena, so keep the wrapper rooted and re-derive st between
     * the two before reading its child slots. */
    if (st->stream_type == CL_STREAM_ECHO) {
        CL_GC_PROTECT(stream);
        ch = cl_stream_read_char(st->string_buf);
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (ch == -1)
            st->flags |= CL_STREAM_FLAG_EOF;
        else
            cl_stream_write_char(st->element_type, ch);
        CL_GC_UNPROTECT(1);
        if (iolock) platform_mutex_unlock(iolock);
        return ch;
    }

    /* Concatenated streams: read from the first remaining component; on its
     * EOF pop it and continue with the next; EOF only after the last
     * component is exhausted (CLHS 21.1).  The recursive read can block
     * inside a GC safe region (console/socket child) where a peer STW-GC
     * compacts the arena, so keep the wrapper's offset rooted and re-derive
     * st before every st-> access. */
    if (st->stream_type == CL_STREAM_CONCATENATED) {
        CL_GC_PROTECT(stream);
        for (;;) {
            st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
            if (!CL_CONS_P(st->string_buf)) {
                ch = -1;
                break;
            }
            ch = cl_stream_read_char(cl_car(st->string_buf));
            if (ch != -1)
                break;
            st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
            st->string_buf = cl_cdr(st->string_buf);
        }
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (ch == -1)
            st->flags |= CL_STREAM_FLAG_EOF;
        CL_GC_UNPROTECT(1);
        if (iolock) platform_mutex_unlock(iolock);
        return ch;
    }

    /* String streams return code points directly (no UTF-8 layer) */
    if (st->stream_type == CL_STREAM_STRING) {
        if (CL_NULL_P(st->string_buf)) {
            if (iolock) platform_mutex_unlock(iolock);
            return -1;
        }
        if (st->position >= st->out_buf_len) {
            if (iolock) platform_mutex_unlock(iolock);
            return -1;
        }
        ch = cl_string_char_at(st->string_buf, st->position++);
        if (iolock) platform_mutex_unlock(iolock);
        return ch;
    }

    /* Byte-oriented streams: read raw byte, then UTF-8 decode.
     * platform_socket_read enters cl_gc_enter/leave_safe_region, so a
     * concurrent STW-GC may compact the arena DURING the read and relocate
     * this stream object.  `stream` is an unrooted C local, so its offset goes
     * stale too — GC-protect it across the read and re-derive `st` from the
     * (forwarded) offset before touching any st-> field.  Without this the
     * `st->flags |= EOF` write below (hit on every Connection:close EOF) would
     * scribble into whatever object slid into the old location. */
    {
        uint32_t rto = st->read_timeout_ms;  /* capture while st is valid */
        CL_GC_PROTECT(stream);
        ch = stream_read_raw_byte(st);
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);   /* st may have moved */

#ifdef CL_WIDE_STRINGS
        /* Decode UTF-8 multi-byte sequences for byte-oriented streams.  A LATIN-1
         * stream is 8-bit transparent: the raw byte 0..255 IS the code point, so
         * skip UTF-8 decoding entirely. */
        if (ch > 0x7F && ch != -1 && !(st->flags & CL_STREAM_FLAG_LATIN1)) {
            ch = stream_decode_utf8(stream, ch);
            st = (CL_Stream *)CL_OBJ_TO_PTR(stream);   /* decode read more bytes */
        }
#endif
        CL_GC_UNPROTECT(1);

        if (ch == PLATFORM_SOCKET_TIMEOUT) {   /* deadline elapsed, not EOF */
            if (iolock) platform_mutex_unlock(iolock);
            stream_raise_timeout(0, rto);
        }
    }
    if (ch == -1)
        st->flags |= CL_STREAM_FLAG_EOF;
    if (iolock) platform_mutex_unlock(iolock);
    return ch;
}

/* Read one raw byte from stream — no UTF-8 decoding.
 * For binary (unsigned-byte 8) streams. */
int cl_stream_read_byte(CL_Obj stream)
{
    CL_Stream *st;
    int ch;
    void *iolock;

    stream = resolve_stream(stream, 0);
    if (CL_NULL_P(stream)) return -1;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    if (!(st->flags & CL_STREAM_FLAG_OPEN))
        return -1;
    if (!(st->direction & CL_STREAM_INPUT))
        return -1;

    iolock = stream_lock_for(st, 0);
    /* See cl_stream_read_char: the contended path can enter a GC safe region
     * and let a peer compact the arena while we're blocked, so protect the
     * offset and re-derive st before touching any st-> field. */
    CL_GC_PROTECT(stream);
    cl_gc_safe_mutex_lock(iolock);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    CL_GC_UNPROTECT(1);

    /* Check for pushed-back byte */
    if (st->unread_char != -1) {
        ch = st->unread_char;
        st->unread_char = -1;
        if (iolock) platform_mutex_unlock(iolock);
        return ch;
    }

    /* Echo streams: same read-then-echo as cl_stream_read_char, with raw
     * bytes (see the GC hazard note there). */
    if (st->stream_type == CL_STREAM_ECHO) {
        CL_GC_PROTECT(stream);
        ch = cl_stream_read_byte(st->string_buf);
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (ch == -1)
            st->flags |= CL_STREAM_FLAG_EOF;
        else
            cl_stream_write_byte(st->element_type, ch);
        CL_GC_UNPROTECT(1);
        if (iolock) platform_mutex_unlock(iolock);
        return ch;
    }

    /* Concatenated streams: same pop-on-EOF walk as cl_stream_read_char,
     * with raw byte reads (see the GC hazard note there). */
    if (st->stream_type == CL_STREAM_CONCATENATED) {
        CL_GC_PROTECT(stream);
        for (;;) {
            st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
            if (!CL_CONS_P(st->string_buf)) {
                ch = -1;
                break;
            }
            ch = cl_stream_read_byte(cl_car(st->string_buf));
            if (ch != -1)
                break;
            st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
            st->string_buf = cl_cdr(st->string_buf);
        }
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (ch == -1)
            st->flags |= CL_STREAM_FLAG_EOF;
        CL_GC_UNPROTECT(1);
        if (iolock) platform_mutex_unlock(iolock);
        return ch;
    }

    /* String streams: return code point (for string-as-bytes) */
    if (st->stream_type == CL_STREAM_STRING) {
        if (CL_NULL_P(st->string_buf)) {
            if (iolock) platform_mutex_unlock(iolock);
            return -1;
        }
        if (st->position >= st->out_buf_len) {
            if (iolock) platform_mutex_unlock(iolock);
            return -1;
        }
        ch = cl_string_char_at(st->string_buf, st->position++);
        if (iolock) platform_mutex_unlock(iolock);
        return ch;
    }

    /* Raw byte read — no UTF-8 decoding.  Same GC hazard as cl_stream_read_char:
     * the read may compact and relocate st, so protect the offset across it and
     * re-derive st before the EOF-flag write. */
    {
        uint32_t rto = st->read_timeout_ms;  /* capture while st is valid */
        CL_GC_PROTECT(stream);
        ch = stream_read_raw_byte(st);
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);   /* st may have moved */
        CL_GC_UNPROTECT(1);
        if (ch == PLATFORM_SOCKET_TIMEOUT) {   /* deadline elapsed, not EOF */
            if (iolock) platform_mutex_unlock(iolock);
            stream_raise_timeout(0, rto);
        }
    }
    if (ch == -1)
        st->flags |= CL_STREAM_FLAG_EOF;
    if (iolock) platform_mutex_unlock(iolock);
    return ch;
}

int32_t cl_stream_read_bytes(CL_Obj stream, char *buf, uint32_t len)
{
    CL_Stream *st;
    uint32_t got = 0;
    void *iolock;

    stream = resolve_stream(stream, 0);
    if (CL_NULL_P(stream)) return -2;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    if (!(st->flags & CL_STREAM_FLAG_OPEN)) return -2;
    if (!(st->direction & CL_STREAM_INPUT)) return -2;

    /* Only stream kinds with a real bulk path; everything else falls back
     * to the caller's per-element loop (string/console/socket/concatenated
     * keep their existing per-byte semantics, incl. socket timeouts). */
    if (st->stream_type != CL_STREAM_FILE && st->stream_type != CL_STREAM_CBUF)
        return -2;
    /* A pushed-back char above 255 cannot become a byte — let the
     * per-element path handle (and diagnose) it instead of truncating. */
    if (st->unread_char > 255)
        return -2;
    if (len == 0) return 0;

    iolock = stream_lock_for(st, 0);
    /* See cl_stream_read_char: the contended lock path can enter a GC safe
     * region and let a peer compact the arena, so protect the offset and
     * re-derive st before touching any st-> field. */
    CL_GC_PROTECT(stream);
    cl_gc_safe_mutex_lock(iolock);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    /* A pushed-back byte is the first element. */
    if (st->unread_char != -1) {
        buf[got++] = (char)(unsigned char)st->unread_char;
        st->unread_char = -1;
    }

    if (st->stream_type == CL_STREAM_CBUF) {
        CL_CBufSlot *cb = cbuf_at(st->handle_id);
        if (cb && cb->data && st->position < st->out_buf_len) {
            uint32_t avail = st->out_buf_len - st->position;
            if (avail > len - got) avail = len - got;
            memcpy(buf + got, cb->data + st->position, avail);
            st->position += avail;
            got += avail;
        }
        if (got < len)
            st->flags |= CL_STREAM_FLAG_EOF;
    } else {
        /* FILE: bulk platform reads.  `buf` is C memory (the builtin's chunk
         * buffer), so it is safe across the safe region the platform read may
         * enter — but st can move there; re-derive it before the EOF-flag
         * write.  Loop until satisfied: a short read below the request means
         * EOF (regular files), but don't rely on it. */
        PlatformFile fh = (PlatformFile)st->handle_id;
        while (got < len) {
            int n = platform_file_read_buf(fh, buf + got, len - got);
            if (n <= 0) {
                st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
                st->flags |= CL_STREAM_FLAG_EOF;
                break;
            }
            got += (uint32_t)n;
        }
        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    }
    CL_GC_UNPROTECT(1);
    if (iolock) platform_mutex_unlock(iolock);
    return (int32_t)got;
}

int32_t cl_stream_write_bytes(CL_Obj stream, const char *buf, uint32_t len)
{
    CL_Stream *st;
    void *iolock;
    int wr = 0;

    stream = resolve_stream(stream, 1);
    if (CL_NULL_P(stream)) return -2;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    if (!(st->flags & CL_STREAM_FLAG_OPEN)) return -2;
    if (!(st->direction & CL_STREAM_OUTPUT)) return -2;

    /* Bulk path for file and socket streams; broadcast/string/console fall
     * back to the caller's per-element loop. */
    if (st->stream_type != CL_STREAM_FILE && st->stream_type != CL_STREAM_SOCKET)
        return -2;
    if (len == 0) return 0;

    iolock = stream_lock_for(st, 1);
    CL_GC_PROTECT(stream);
    cl_gc_safe_mutex_lock(iolock);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    {
        uint32_t wto = st->write_timeout_ms;  /* capture before safe region */
        if (st->stream_type == CL_STREAM_FILE) {
            /* `buf` is C memory, so the >4KB bracketed path inside
             * platform_file_write_buf is safe. */
            platform_file_write_buf((PlatformFile)st->handle_id, buf, len);
        } else {
            wr = platform_socket_write_buf((PlatformSocket)st->handle_id,
                                           buf, len);
        }
        CL_GC_UNPROTECT(1);
        if (iolock) platform_mutex_unlock(iolock);
        if (wr == PLATFORM_SOCKET_TIMEOUT)
            stream_raise_timeout(1, wto);
    }
    return 0;
}

void cl_stream_write_char(CL_Obj stream, int ch)
{
    CL_Stream *st;
    void *iolock;
    int wr = 0;   /* socket write result, for timeout detection */
#ifdef CL_WIDE_STRINGS
    /* A LATIN-1 stream is 8-bit transparent: write the low byte raw, no UTF-8.
     * `encode` is non-zero only when this char must be UTF-8-encoded. */
    int encode;
#endif

    stream = resolve_stream(stream, 1);
    if (CL_NULL_P(stream)) return;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    if (!(st->flags & CL_STREAM_FLAG_OPEN))
        return;
    if (!(st->direction & CL_STREAM_OUTPUT))
        return;

    /* Broadcast streams: fan the write out to every component (CLHS 21.1);
     * with no components the output is discarded.  A recursive write can
     * block inside a GC safe region (file/socket child) where a peer STW-GC
     * compacts the arena, so keep the list cursor rooted across each call. */
    if (st->stream_type == CL_STREAM_BROADCAST) {
        CL_Obj comps = st->string_buf;
        CL_GC_PROTECT(comps);
        while (CL_CONS_P(comps)) {
            cl_stream_write_char(cl_car(comps), ch);
            comps = cl_cdr(comps);
        }
        CL_GC_UNPROTECT(1);
        return;
    }

#ifdef CL_WIDE_STRINGS
    encode = (ch > 0x7F) && !(st->flags & CL_STREAM_FLAG_LATIN1);
#endif

    iolock = stream_lock_for(st, 1);
    /* See cl_stream_read_char: the contended path can enter a GC safe region
     * and let a peer compact the arena while we're blocked, so protect the
     * offset and re-derive st before touching any st-> field. */
    CL_GC_PROTECT(stream);
    cl_gc_safe_mutex_lock(iolock);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    CL_GC_UNPROTECT(1);

    /* Capture timeout before the socket write: platform_socket_write* may call
     * socket_flush_wbuf which enters a GC safe region; a concurrent STW-GC can
     * compact the arena, making st stale.  Read the field now while st is valid,
     * GC-protect the offset across the write, and re-derive st before the
     * st->charpos update below. */
    {
        uint32_t wto = st->write_timeout_ms;
        CL_GC_PROTECT(stream);

        switch (st->stream_type) {
        case CL_STREAM_CONSOLE: {
#ifdef CL_WIDE_STRINGS
            if (encode) {
                char utf8[5];
                int nb = cl_utf8_encode(ch, utf8);
                if (nb > 0) { utf8[nb] = '\0'; platform_write_string(utf8); }
            } else
#endif
            {
                char buf[2];
                buf[0] = (char)ch;
                buf[1] = '\0';
                platform_write_string(buf);
            }
            break;
        }
        case CL_STREAM_FILE:
#ifdef CL_WIDE_STRINGS
            if (encode) {
                char utf8[4];
                int nb = cl_utf8_encode(ch, utf8);
                if (nb > 0)
                    platform_file_write_buf((PlatformFile)st->handle_id, utf8, (uint32_t)nb);
            } else
#endif
            platform_file_write_char((PlatformFile)st->handle_id, ch);
            break;
        case CL_STREAM_STRING:
            /* String output buffers store raw bytes; encode UTF-8 for non-ASCII */
#ifdef CL_WIDE_STRINGS
            if (encode) {
                char utf8[4];
                int nb = cl_utf8_encode(ch, utf8);
                int j;
                for (j = 0; j < nb; j++)
                    cl_stream_outbuf_putchar(st, (unsigned char)utf8[j]);
            } else
#endif
            cl_stream_outbuf_putchar(st, ch);
            break;
        case CL_STREAM_SOCKET:
#ifdef CL_WIDE_STRINGS
            if (encode) {
                char utf8[4];
                int nb = cl_utf8_encode(ch, utf8);
                if (nb > 0)
                    wr = platform_socket_write_buf((PlatformSocket)st->handle_id, utf8, (uint32_t)nb);
            } else
#endif
            wr = platform_socket_write((PlatformSocket)st->handle_id, ch);
            break;
        }

        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);   /* a socket flush may have moved st */
        CL_GC_UNPROTECT(1);

        if (ch == '\n')
            st->charpos = 0;
        else
            st->charpos++;

        if (iolock) platform_mutex_unlock(iolock);
        if (wr == PLATFORM_SOCKET_TIMEOUT)
            stream_raise_timeout(1, wto);
    }
}

/* Write one raw byte to stream — no UTF-8 encoding.
 * For binary (unsigned-byte 8) streams. */
void cl_stream_write_byte(CL_Obj stream, int byte)
{
    CL_Stream *st;
    void *iolock;
    int wr = 0;   /* socket write result, for timeout detection */

    stream = resolve_stream(stream, 1);
    if (CL_NULL_P(stream)) return;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    if (!(st->flags & CL_STREAM_FLAG_OPEN))
        return;
    if (!(st->direction & CL_STREAM_OUTPUT))
        return;

    /* Broadcast: fan out to every component (see cl_stream_write_char). */
    if (st->stream_type == CL_STREAM_BROADCAST) {
        CL_Obj comps = st->string_buf;
        CL_GC_PROTECT(comps);
        while (CL_CONS_P(comps)) {
            cl_stream_write_byte(cl_car(comps), byte);
            comps = cl_cdr(comps);
        }
        CL_GC_UNPROTECT(1);
        return;
    }

    iolock = stream_lock_for(st, 1);
    /* See cl_stream_read_char: the contended path can enter a GC safe region
     * and let a peer compact the arena while we're blocked, so protect the
     * offset and re-derive st before touching any st-> field. */
    CL_GC_PROTECT(stream);
    cl_gc_safe_mutex_lock(iolock);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    CL_GC_UNPROTECT(1);

    {
        uint32_t wto = st->write_timeout_ms;  /* capture before safe region */

        switch (st->stream_type) {
        case CL_STREAM_CONSOLE: {
            char buf[2];
            buf[0] = (char)(unsigned char)byte;
            buf[1] = '\0';
            platform_write_string(buf);
            break;
        }
        case CL_STREAM_FILE:
            platform_file_write_char((PlatformFile)st->handle_id, byte);
            break;
        case CL_STREAM_STRING:
            cl_stream_outbuf_putchar(st, byte);
            break;
        case CL_STREAM_SOCKET:
            wr = platform_socket_write((PlatformSocket)st->handle_id, byte);
            break;
        }

        if (iolock) platform_mutex_unlock(iolock);
        if (wr == PLATFORM_SOCKET_TIMEOUT)
            stream_raise_timeout(1, wto);
    }
}

void cl_stream_write_string(CL_Obj stream, const char *str, uint32_t len)
{
    CL_Stream *st;
    uint32_t i;
    void *iolock;
    int wr = 0;   /* socket write result, for timeout detection */

    stream = resolve_stream(stream, 1);
    if (CL_NULL_P(stream)) return;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);

    if (!(st->flags & CL_STREAM_FLAG_OPEN))
        return;
    if (!(st->direction & CL_STREAM_OUTPUT))
        return;

    /* Broadcast: fan out to every component (see cl_stream_write_char).
     * str is a caller-owned C buffer (arena-resident sources must already
     * go through cl_stream_write_lisp_string's stack chunking), so it stays
     * valid even if a recursive blocking write lets a peer GC compact. */
    if (st->stream_type == CL_STREAM_BROADCAST) {
        CL_Obj comps = st->string_buf;
        CL_GC_PROTECT(comps);
        while (CL_CONS_P(comps)) {
            cl_stream_write_string(cl_car(comps), str, len);
            comps = cl_cdr(comps);
        }
        CL_GC_UNPROTECT(1);
        return;
    }

    iolock = stream_lock_for(st, 1);
    /* See cl_stream_read_char: the contended path can enter a GC safe region
     * and let a peer compact the arena while we're blocked, so protect the
     * offset and re-derive st before touching any st-> field. */
    CL_GC_PROTECT(stream);
    cl_gc_safe_mutex_lock(iolock);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    CL_GC_UNPROTECT(1);

    {
        uint32_t wto = st->write_timeout_ms;  /* capture before safe region */
        /* Compute the charpos delta BEFORE the write: for blocking file/
         * socket writes a peer compaction can move an arena-interior str
         * while we are parked, so str must not be re-read afterward. */
        uint32_t last_nl = 0xFFFFFFFF;
        for (i = 0; i < len; i++) {
            if (str[i] == '\n')
                last_nl = i;
        }
        CL_GC_PROTECT(stream);   /* socket flush may compact; re-derive st after */

        switch (st->stream_type) {
        case CL_STREAM_CONSOLE: {
            /* Chunk to stack buffer for platform_write_string (needs NUL) */
            char tmp[257];
            uint32_t pos = 0;
            while (pos < len) {
                uint32_t chunk = len - pos;
                if (chunk > 256) chunk = 256;
                memcpy(tmp, str + pos, chunk);
                tmp[chunk] = '\0';
                platform_write_string(tmp);
                pos += chunk;
            }
            break;
        }
        case CL_STREAM_FILE:
            platform_file_write_buf((PlatformFile)st->handle_id, str, len);
            break;
        case CL_STREAM_STRING:
            for (i = 0; i < len; i++)
                cl_stream_outbuf_putchar(st, (unsigned char)str[i]);
            break;
        case CL_STREAM_SOCKET:
            wr = platform_socket_write_buf((PlatformSocket)st->handle_id, str, len);
            break;
        }

        st = (CL_Stream *)CL_OBJ_TO_PTR(stream);   /* a socket flush may have moved st */
        CL_GC_UNPROTECT(1);

        /* Update charpos from the pre-computed newline position */
        if (last_nl != 0xFFFFFFFF)
            st->charpos = len - last_nl - 1;
        else
            st->charpos += len;

        if (iolock) platform_mutex_unlock(iolock);
        if (wr == PLATFORM_SOCKET_TIMEOUT)
            stream_raise_timeout(1, wto);
    }
}

/* Write a Lisp base-string's [start, end) range to a stream, safe for
 * ARENA-resident sources: blocking file/socket writes park in a GC safe
 * region (MT), a peer compaction moves the string, and the platform layer
 * would resume copying from a dangling pointer (garbage bytes on the
 * wire).  Chunk through a C stack buffer, re-deriving the source from the
 * rooted strobj per chunk, so no arena pointer is ever live across a
 * blocking call.  strobj must be a base string (TYPE_STRING). */
void cl_stream_write_lisp_string(CL_Obj stream, CL_Obj strobj,
                                 uint32_t start, uint32_t end)
{
    char chunk[512];
    CL_GC_PROTECT(stream);
    CL_GC_PROTECT(strobj);
    while (start < end) {
        uint32_t nb = end - start;
        /* volatile: m68k-amigaos-gcc at -O2 miscompiles `data + start`
         * folded straight into the copy (dropped the first byte in the
         * old bi_write_string fast path) — force the base pointer through
         * memory before the offset math. */
        const char *volatile src = ((CL_String *)CL_OBJ_TO_PTR(strobj))->data;
        if (nb > (uint32_t)sizeof(chunk)) nb = (uint32_t)sizeof(chunk);
        memcpy(chunk, src + start, nb);
        cl_stream_write_string(stream, chunk, nb);
        start += nb;
    }
    CL_GC_UNPROTECT(2);
}

int cl_stream_peek_char(CL_Obj stream)
{
    stream = resolve_stream(stream, 0);
    if (CL_NULL_P(stream)) return -1;
    /* Echo streams: a peek must not echo (CLHS PEEK-CHAR: the character is
     * not consumed; ansi-test peek-char.17).  A char already pushed back
     * onto the wrapper is what the next read returns, so answer with it;
     * otherwise peek on the INPUT CHILD, which parks the char in the child's
     * own unread slot — the read that later consumes it goes through the
     * wrapper and echoes it then, exactly once. */
    {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
        if (st->stream_type == CL_STREAM_ECHO) {
            if (st->unread_char != -1)
                return st->unread_char;
            return cl_stream_peek_char(st->string_buf);
        }
    }
    {
        /* cl_stream_read_char can block (socket/console) inside a GC safe
         * region, letting a peer compact and relocate the stream.  Root
         * the local so it is forwarded, and derive st only AFTER the read
         * — otherwise the unread_char store scribbles into freed arena. */
        int ch;
        CL_GC_PROTECT(stream);
        ch = cl_stream_read_char(stream);
        if (ch != -1) {
            CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
            st->unread_char = ch;
        }
        CL_GC_UNPROTECT(1);
        return ch;
    }
}

void cl_stream_unread_char(CL_Obj stream, int ch)
{
    CL_Stream *st;
    stream = resolve_stream(stream, 0);
    if (CL_NULL_P(stream)) return;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    st->unread_char = ch;
}

void cl_stream_close(CL_Obj stream)
{
    CL_Stream *st;
    void *iolock;
    stream = resolve_synonym(stream);
    if (CL_NULL_P(stream)) return;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    if (!(st->flags & CL_STREAM_FLAG_OPEN))
        return;

    /* Closing flushes and tears down the write side; take the write lock.  A
     * concurrent reader parked on this socket holds only the read lock, so
     * close never waits on it — the read() returns once the fd is closed. */
    iolock = (st->stream_type == CL_STREAM_FILE ||
              st->stream_type == CL_STREAM_SOCKET)
                 ? stream_lock_for(st, 1) : NULL;
    /* See cl_stream_read_char: the contended path can enter a GC safe region
     * and let a peer compact the arena while we're blocked, so protect the
     * offset and re-derive st before touching any st-> field. */
    CL_GC_PROTECT(stream);
    cl_gc_safe_mutex_lock(iolock);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    CL_GC_UNPROTECT(1);

    /* Claim the close.  The OPEN check at function entry ran BEFORE the
     * lock (TOCTOU): two concurrent CLOSE calls could both pass it, both
     * clear the flag and both run the platform close — the second close
     * hits an fd/handle slot the platform may already have RECYCLED for
     * an unrelated stream, killing a live connection or file.
     *  - FILE/SOCKET: re-check under the iolock just acquired.
     *  - No-iolock types (string/cbuf/console): claim the flag bit with an
     *    atomic CAS; the loser sees OPEN already gone and bails.  The CAS
     *    targets arena memory, which is safe here: no allocation happens
     *    between the re-derive above and the claim, so the stream cannot
     *    move (a peer STW compaction must wait for this running thread). */
    if (iolock) {
        if (!(st->flags & CL_STREAM_FLAG_OPEN)) {
            platform_mutex_unlock(iolock);
            return;
        }
        st->flags &= ~CL_STREAM_FLAG_OPEN;
    } else {
        for (;;) {
            uint32_t old = st->flags;
            if (!(old & CL_STREAM_FLAG_OPEN))
                return;                      /* lost the race — already closed */
            if (!CL_MT()) { st->flags = old & ~CL_STREAM_FLAG_OPEN; break; }
            if (platform_atomic_cas(&st->flags, old,
                                    old & ~CL_STREAM_FLAG_OPEN))
                break;
        }
    }

    /* Capture the handle/type before any teardown call: platform_socket_flush
     * and platform_socket_close (and the file equivalents) enter GC safe
     * regions, so a concurrent STW-GC can compact and relocate st between the
     * flush and the close.  Reading st->handle_id for the close THROUGH a stale
     * st could yield a garbage slot number and close an UNRELATED live socket,
     * corrupting another in-flight connection (intermittent "No status line"). */
    {
        uint32_t stype  = st->stream_type;
        uint32_t handle = st->handle_id;
        uint32_t obh    = st->out_buf_handle;
        uint32_t dir    = st->direction;
        switch (stype) {
        case CL_STREAM_FILE:
            if (handle != 0) {
                platform_file_flush((PlatformFile)handle);
                platform_file_close((PlatformFile)handle);
            }
            break;
        case CL_STREAM_STRING:
            if ((dir & CL_STREAM_OUTPUT) && obh != 0) {
                cl_stream_free_outbuf(obh);
                /* Clear the stream's handle so this (still reachable, now
                 * closed) stream can't pin the slot in the GC mark phase
                 * after the handle is recycled by another live stream —
                 * that pin would keep a genuinely dead stream's buffer from
                 * being reclaimed.  Safe to touch st here: the STRING path
                 * entered no GC safe region since st was re-derived above
                 * (cl_stream_free_outbuf takes only a plain mutex), so the
                 * stream cannot have moved. */
                st->out_buf_handle = 0;
            }
            break;
        case CL_STREAM_CBUF: {
            CL_CBufSlot *cb = cbuf_at(handle);
            if (cb) {
                cb->data = NULL;
                cb->len = 0;
            }
            break;
        }
        case CL_STREAM_SOCKET:
            if (handle != 0) {
                platform_socket_flush((PlatformSocket)handle);
                platform_socket_close((PlatformSocket)handle);
            }
            break;
        default:
            break;
        }
    }

    if (iolock) platform_mutex_unlock(iolock);
}

CL_Obj cl_make_string_input_stream(CL_Obj string, uint32_t start, uint32_t end)
{
    CL_Obj s;
    CL_Stream *st;

    /* GC-protect `string` across cl_make_stream: it allocates the stream
     * object and may compact the heap, relocating the source string. Without
     * protection, `string` holds a stale offset and st->string_buf would point
     * at moved/garbage memory, corrupting every subsequent read from the
     * stream (e.g. read-from-string dropping or mangling list elements). */
    CL_GC_PROTECT(string);
    s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_STRING);
    if (CL_NULL_P(s)) { CL_GC_UNPROTECT(1); return CL_NIL; }

    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->string_buf = string;
    st->position = start;
    st->out_buf_len = end;  /* Reuse as end limit */
    CL_GC_UNPROTECT(1);
    return s;
}

/* Sweep-time release of a C-buffer stream that died without CLOSE.
 *
 * LOAD, COMPILE-FILE and the script runner read their file through one, keep
 * it GC-protected for as long as they run, and close it (then free the file
 * buffer) on their way out.  A non-local exit out of the file — HANDLER-CASE
 * around a LOAD whose form signals — skips that: the stream becomes garbage
 * with its table slot still taken and the file buffer owned by nobody.  Seven
 * of those and every later LOAD / COMPILE-FILE found no slot (and used to
 * read nothing, silently).  The OPEN flag gates it exactly as for the OS
 * handles next door in gc_finalize: a closed stream cleared it together with
 * the slot, and clearing it here makes a re-finalize a no-op.  The slot cannot
 * have been recycled — it stays taken until this runs.  Called under STW. */
void cl_stream_cbuf_gc_release(CL_Stream *st)
{
    CL_CBufSlot *cb;
    if (st->stream_type != CL_STREAM_CBUF || !(st->flags & CL_STREAM_FLAG_OPEN))
        return;
    st->flags &= ~CL_STREAM_FLAG_OPEN;
    cb = cbuf_at(st->handle_id);
    if (!cb || !cb->data)
        return;
    platform_free((void *)cb->data);
    cb->data = NULL;
    cb->len = 0;
}

/* Take a free slot; with `grow`, add a block when every slot is taken.
 * Returns the handle, or 0 when full (and not growing, at the directory's
 * ceiling, or out of memory). */
static uint32_t cbuf_take_slot(const char *data, uint32_t len, int grow)
{
    uint32_t blk, i, found = 0;
    /* Capture CL_MT() once — see cl_stream_alloc_outbuf for why re-evaluating
     * it at the unlock sites can leak cl_stream_table_mutex locked. */
    int mt = CL_MT();
    if (mt) platform_mutex_lock(cl_stream_table_mutex);
    for (blk = 0; blk < cbuf_nblocks && !found; blk++) {
        CL_CBufSlot *b = cbuf_dir[blk];
        if (!b) continue;
        for (i = (blk == 0) ? 1 : 0; i < CL_CBUF_BLOCK_SIZE; i++) {
            if (b[i].data == NULL) {
                found = (blk << CL_CBUF_BLOCK_SHIFT) | i;
                break;
            }
        }
    }
    if (!found && grow && cbuf_nblocks < CL_CBUF_MAX_BLOCKS) {
        uint32_t nb = cbuf_nblocks;
        CL_CBufSlot *b = (CL_CBufSlot *)platform_alloc(
            (uint32_t)(CL_CBUF_BLOCK_SIZE * sizeof(CL_CBufSlot)));
        if (b) {
            memset(b, 0, CL_CBUF_BLOCK_SIZE * sizeof(CL_CBufSlot));
            cbuf_dir[nb] = b;             /* publish only after full init */
            cbuf_nblocks = nb + 1;
            found = nb << CL_CBUF_BLOCK_SHIFT;
        }
    }
    if (found) {
        CL_CBufSlot *cb = cbuf_at(found);
        cb->data = data;
        cb->len = len;
    }
    if (mt) platform_mutex_unlock(cl_stream_table_mutex);
    return found;
}

CL_Obj cl_make_cbuf_input_stream(const char *data, uint32_t len)
{
    CL_Obj s;
    CL_Stream *st;
    uint32_t slot = cbuf_take_slot(data, len, 0);

    if (!slot) {
        /* Full: slots (and file buffers) held by abandoned streams come back
         * with a collection (cl_stream_cbuf_gc_release), so try that before
         * growing.  Nothing of ours is on the heap yet. */
        cl_gc();
        slot = cbuf_take_slot(data, len, 1);
        if (!slot) return CL_NIL;  /* directory ceiling or out of memory */
    }
    s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CBUF);
    if (CL_NULL_P(s)) {
        CL_CBufSlot *cb = cbuf_at(slot);
        cb->data = NULL;
        cb->len = 0;
        return CL_NIL;
    }
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->handle_id = slot;
    st->position = 0;
    st->out_buf_len = len;  /* End limit */
    return s;
}

CL_Obj cl_make_string_output_stream(void)
{
    CL_Obj s = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_STRING);
    CL_Stream *st;
    uint32_t h;
    if (CL_NULL_P(s)) return CL_NIL;

    /* cl_stream_alloc_outbuf runs a GC (sweep + possible compaction) when the
     * outbuf table is full.  `s` is a freshly-built stream not yet reachable
     * from any root, so that GC would sweep it (or compaction would relocate
     * it, leaving this C local with a stale offset) — and `st` derived before
     * the call would dangle.  Protect `s` across the alloc and derive `st`
     * afterward.  This is the deterministic corruption behind the periodic
     * "argument is not a stream" failures when many string-output-streams are
     * created in a loop (e.g. chunga read-line* per HTTP request). */
    CL_GC_PROTECT(s);
    h = cl_stream_alloc_outbuf(256);
    if (h == 0) {
        CL_GC_UNPROTECT(1);
        cl_error(0,
                 "String output stream buffer table exhausted "
                 "(all %d slots in use after growth and GC, or out of memory)",
                 CL_OUTBUF_MAX_BLOCKS * CL_OUTBUF_BLOCK_SIZE - 1);
        return CL_NIL;
    }
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);   /* derive after alloc — s may have moved */
    st->out_buf_handle = h;
    st->out_buf_size = 256;
    /* The (still GC-protected) stream now owns the slot, so the mark phase will
     * keep it alive — release the creation-window pin. */
    cl_stream_outbuf_unpin(h);
    CL_GC_UNPROTECT(1);
    return s;
}

CL_Obj cl_get_output_stream_string(CL_Obj stream)
{
    CL_Stream *st;
    char *data;
    uint32_t len;
    CL_Obj result;

    /* Building the result string ALLOCATES (cl_make_string / cl_utf8_to_cl_string),
     * which can trigger a compacting GC — either this thread's own, or a peer
     * thread's stop-the-world compaction while we are parked at cl_alloc's
     * safepoint.  That relocates the stream object, so the `st` raw pointer
     * (and the `stream` C-local offset) captured before the alloc go stale.
     * Using the stale `st` afterward for cl_stream_outbuf_reset(st->out_buf_handle)
     * and the st->out_buf_len/charpos writes reads a garbage handle and scribbles
     * into whatever slid into the stream's old slot — under multi-thread FORMAT
     * this reset a PEER's live outbuf, so `(format nil …)` returned corrupt/
     * cross-contaminated results.  Protect `stream` so the compactor forwards it,
     * and re-derive `st` after the allocation.  `data` points into the
     * platform-alloc'd (non-arena) outbuf, so it stays valid across the move. */
    CL_GC_PROTECT(stream);
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    data = cl_stream_outbuf_data(st->out_buf_handle);
    len = cl_stream_outbuf_len(st->out_buf_handle);
    if (!data || len == 0)
        result = cl_make_string("", 0);
    else {
#ifdef CL_WIDE_STRINGS
        /* Output buffer contains UTF-8 bytes; decode to CL string */
        result = cl_utf8_to_cl_string(data, len);
#else
        result = cl_make_string(data, len);
#endif
    }

    /* Re-derive st: the alloc above may have relocated the stream object. */
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    cl_stream_outbuf_reset(st->out_buf_handle);
    st->out_buf_len = 0;
    st->charpos = 0;
    CL_GC_UNPROTECT(1);
    return result;
}

CL_Obj cl_finish_string_output_stream(CL_Obj sstream)
{
    /* Collect the accumulated string, then release the temp stream's platform
     * output buffer and clear the handle so it is neither reused nor leaked.
     * Callers that build a transient string-output-stream (printer / format /
     * io / condition report) all repeated this get-string + free-outbuf pair.
     * SSTREAM is GC-protected here because cl_get_output_stream_string
     * allocates and can relocate the stream before the free/clear below. */
    CL_Obj text;
    CL_Stream *st;
    CL_GC_PROTECT(sstream);
    text = cl_get_output_stream_string(sstream);
    st = (CL_Stream *)CL_OBJ_TO_PTR(sstream);
    cl_stream_free_outbuf(st->out_buf_handle);
    st->out_buf_handle = 0;
    CL_GC_UNPROTECT(1);
    return text;
}

CL_Obj cl_make_socket_stream(const char *host, int port, int connect_ms)
{
    PlatformSocket sh = platform_socket_connect(host, port, connect_ms);
    CL_Obj s;
    CL_Stream *st;
    if (sh == PLATFORM_SOCKET_INVALID)
        return CL_NIL;
    s = cl_make_stream(CL_STREAM_IO, CL_STREAM_SOCKET);
    if (CL_NULL_P(s)) {
        platform_socket_close(sh);
        return CL_NIL;
    }
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->handle_id = (uint32_t)sh;
    return s;
}

CL_Obj cl_make_udp_socket_stream(const char *host, int port)
{
    PlatformSocket sh = platform_udp_connect(host, port);
    CL_Obj s;
    CL_Stream *st;
    if (sh == PLATFORM_SOCKET_INVALID)
        return CL_NIL;
    s = cl_make_stream(CL_STREAM_IO, CL_STREAM_SOCKET);
    if (CL_NULL_P(s)) {
        platform_socket_close(sh);
        return CL_NIL;
    }
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->handle_id = (uint32_t)sh;
    st->flags |= CL_STREAM_FLAG_DGRAM;
    return s;
}

CL_Obj cl_make_udp_bound_socket_stream(const char *addr, int port, int family,
                                       int reuse, int *actual_port)
{
    int bound_port = 0;
    PlatformSocket sh = platform_udp_bind(addr, port, family, reuse, &bound_port);
    CL_Obj s;
    CL_Stream *st;
    if (sh == PLATFORM_SOCKET_INVALID)
        return CL_NIL;
    if (actual_port)
        *actual_port = bound_port;
    s = cl_make_stream(CL_STREAM_IO, CL_STREAM_SOCKET);
    if (CL_NULL_P(s)) {
        platform_socket_close(sh);
        return CL_NIL;
    }
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->handle_id = (uint32_t)sh;
    st->flags |= CL_STREAM_FLAG_DGRAM;
    return s;
}

CL_Obj cl_make_listen_stream(int port, int loopback, int *actual_port)
{
    return cl_make_listen_stream_addr(port, loopback ? "127.0.0.1" : NULL, actual_port);
}

CL_Obj cl_make_listen_stream_addr(int port, const char *bind_addr, int *actual_port)
{
    int bound_port = 0;
    PlatformSocket sh = platform_socket_listen_addr(port, bind_addr, &bound_port);
    CL_Obj s;
    CL_Stream *st;
    if (sh == PLATFORM_SOCKET_INVALID)
        return CL_NIL;
    if (actual_port)
        *actual_port = bound_port;
    /* A listener is only ever accept()ed on, never read/written; flag it
     * INPUT so an accidental write-char is rejected as a direction error. */
    s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_SOCKET);
    if (CL_NULL_P(s)) {
        platform_socket_close(sh);
        return CL_NIL;
    }
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->handle_id = (uint32_t)sh;
    /* `position` is unused for socket streams (no read/write cursor); reuse it
     * to record the bound local port so cl_listen_stream_local_port can return
     * it — essential when port==0 lets the OS pick an ephemeral port. */
    st->position = (uint32_t)bound_port;
    return s;
}

int cl_listen_stream_local_port(CL_Obj stream)
{
    CL_Stream *st;
    if (!CL_STREAM_P(stream))
        return -1;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    /* Listeners are created with direction == CL_STREAM_INPUT exactly;
     * connected sockets are CL_STREAM_IO, so this rejects them. */
    if (st->stream_type != CL_STREAM_SOCKET ||
        st->direction != CL_STREAM_INPUT ||
        !(st->flags & CL_STREAM_FLAG_OPEN))
        return -1;
    return (int)st->position;
}

CL_Obj cl_socket_stream_accept(CL_Obj listener)
{
    CL_Stream *lst;
    PlatformSocket conn;
    CL_Obj s;
    CL_Stream *st;

    if (!CL_STREAM_P(listener))
        return CL_NIL;
    lst = (CL_Stream *)CL_OBJ_TO_PTR(listener);
    if (lst->stream_type != CL_STREAM_SOCKET ||
        !(lst->flags & CL_STREAM_FLAG_OPEN))
        return CL_NIL;

    /* Block until a client connects.  conn is a plain integer handle, so the
     * subsequent allocation cannot lose it; `listener` is GC-rooted by the
     * caller's argument slot. */
    conn = platform_socket_accept((PlatformSocket)lst->handle_id);
    if (conn == PLATFORM_SOCKET_INVALID)
        return CL_NIL;

    s = cl_make_stream(CL_STREAM_IO, CL_STREAM_SOCKET);
    if (CL_NULL_P(s)) {
        platform_socket_close(conn);
        return CL_NIL;
    }
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->handle_id = (uint32_t)conn;
    return s;
}

uint32_t cl_socket_stream_get_timeout(CL_Obj stream, int which)
{
    CL_Stream *st;
    stream = resolve_stream(stream, which ? 1 : 0);
    if (CL_NULL_P(stream)) return 0;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    if (st->stream_type != CL_STREAM_SOCKET) return 0;
    return which ? st->write_timeout_ms : st->read_timeout_ms;
}

void cl_socket_stream_set_timeout(CL_Obj stream, int which, uint32_t ms)
{
    CL_Stream *st;
    stream = resolve_stream(stream, which ? 1 : 0);
    if (CL_NULL_P(stream)) return;
    st = (CL_Stream *)CL_OBJ_TO_PTR(stream);
    if (st->stream_type != CL_STREAM_SOCKET) return;
    if (which) st->write_timeout_ms = ms;
    else       st->read_timeout_ms = ms;
    /* Push both current values down to the platform layer in one call. */
    platform_socket_set_timeout((PlatformSocket)st->handle_id,
                                (int)st->read_timeout_ms,
                                (int)st->write_timeout_ms);
}

CL_Obj cl_make_synonym_stream(CL_Obj symbol)
{
    CL_Obj s = cl_make_stream(CL_STREAM_IO, CL_STREAM_SYNONYM);
    CL_Stream *st;
    if (CL_NULL_P(s)) return CL_NIL;
    st = (CL_Stream *)CL_OBJ_TO_PTR(s);
    st->string_buf = symbol;  /* Store the symbol */
    return s;
}

/* Shared constructor for the two wrapper kinds with an input child in
 * string_buf and an output child in element_type.  cl_make_stream allocates
 * and may compact: the two component offsets are C locals (copies of the
 * caller's rooted args[]), so they must be forwarded across it. */
static CL_Obj make_io_pair_stream(uint32_t stream_type,
                                  CL_Obj input_stream, CL_Obj output_stream)
{
    CL_Obj s;
    CL_GC_PROTECT(input_stream);
    CL_GC_PROTECT(output_stream);
    s = cl_make_stream(CL_STREAM_IO, stream_type);
    if (!CL_NULL_P(s)) {
        CL_Stream *st = (CL_Stream *)CL_OBJ_TO_PTR(s);
        st->string_buf = input_stream;    /* input child */
        st->element_type = output_stream; /* output child */
    }
    CL_GC_UNPROTECT(2);
    return s;
}

CL_Obj cl_make_two_way_stream(CL_Obj input_stream, CL_Obj output_stream)
{
    return make_io_pair_stream(CL_STREAM_TWO_WAY, input_stream, output_stream);
}

/* Same slot layout as a two-way stream; the ECHO type is what makes the
 * read paths copy each element to the output child. */
CL_Obj cl_make_echo_stream(CL_Obj input_stream, CL_Obj output_stream)
{
    return make_io_pair_stream(CL_STREAM_ECHO, input_stream, output_stream);
}

/* streams: proper list of component output streams (may be NIL — such a
 * broadcast stream discards all output, CLHS 21.1). */
CL_Obj cl_make_broadcast_stream_list(CL_Obj streams)
{
    CL_Obj s;
    /* cl_make_stream allocates and may compact — keep the list forwarded. */
    CL_GC_PROTECT(streams);
    s = cl_make_stream(CL_STREAM_OUTPUT, CL_STREAM_BROADCAST);
    if (!CL_NULL_P(s))
        ((CL_Stream *)CL_OBJ_TO_PTR(s))->string_buf = streams;
    CL_GC_UNPROTECT(1);
    return s;
}

/* streams: proper list of component input streams (may be NIL — such a
 * concatenated stream is immediately at end of file, CLHS 21.1). */
CL_Obj cl_make_concatenated_stream_list(CL_Obj streams)
{
    CL_Obj s;
    CL_GC_PROTECT(streams);
    s = cl_make_stream(CL_STREAM_INPUT, CL_STREAM_CONCATENATED);
    if (!CL_NULL_P(s))
        ((CL_Stream *)CL_OBJ_TO_PTR(s))->string_buf = streams;
    CL_GC_UNPROTECT(1);
    return s;
}

/* --- Stream-aware write helpers --- */

/* Resolve CL:WRITE-STRING's current function, or CL_NIL if unavailable.
 * gray-streams.lisp shadows WRITE-STRING in the COMMON-LISP package so that,
 * given a Gray stream, it dispatches to GRAY:STREAM-WRITE-STRING.  Looked up
 * fresh each call: the symbol lives in the package (a GC root), but its
 * function object may move under a compacting GC, so we must not cache it. */
static CL_Obj lisp_write_string_fn(void)
{
    CL_Obj pkg = cl_find_package("COMMON-LISP", 11);
    CL_Obj sym;
    CL_Symbol *s;
    if (CL_NULL_P(pkg)) return CL_NIL;
    sym = cl_package_find_symbol("WRITE-STRING", 12, pkg);
    if (CL_NULL_P(sym)) return CL_NIL;
    s = (CL_Symbol *)CL_OBJ_TO_PTR(sym);
    if (s->function == CL_UNBOUND) return CL_NIL;
    return s->function;
}

void cl_write_cstring_to_stream_sym(CL_Obj sym, const char *s)
{
    CL_Obj val;
    if (CL_NULL_P(sym)) {
        platform_write_string(s);
        return;
    }
    val = cl_symbol_value(sym);
    if (CL_STREAM_P(val)) {
        cl_stream_write_string(val, s, (uint32_t)strlen(s));
        return;
    }
    /* Non-native stream value (e.g. a Gray-streams CLOS instance, such as the
     * output stream SLY binds to *STANDARD-OUTPUT*).  The native stream path
     * cannot reach it; route through the Lisp WRITE-STRING wrapper so the
     * stream's STREAM-WRITE-STRING method runs.
     *
     * Guard against CL_UNBOUND: a C-level diagnostic/boot write may run before
     * cl_stream_init has bound the standard stream variables (e.g. the boot
     * timing log fired from cl_repl_init, or any minimal embedding that skips
     * stream init).  An unbound special is neither a stream nor NULL, so
     * without this check it would be handed to WRITE-STRING as a bogus stream
     * and raise "argument is not a stream" — fatal at that early stage.  Fall
     * through to platform_write_string instead. */
    if (!CL_NULL_P(val) && val != CL_UNBOUND) {
        CL_Obj fn = lisp_write_string_fn();
        if (!CL_NULL_P(fn)) {
            CL_Obj args[2];
            CL_GC_PROTECT(val);
            CL_GC_PROTECT(fn);
            args[0] = cl_make_string(s, (uint32_t)strlen(s));
            args[1] = val;
            cl_vm_apply(fn, args, 2);
            CL_GC_UNPROTECT(2);
            return;
        }
    }
    platform_write_string(s);
}

void cl_write_cstring_to_stdout(const char *s)
{
    cl_write_cstring_to_stream_sym(SYM_STANDARD_OUTPUT, s);
}

void cl_write_cstring_to_error(const char *s)
{
    cl_write_cstring_to_stream_sym(SYM_ERROR_OUTPUT, s);
}

void cl_write_cstring_to_debug_io(const char *s)
{
    cl_write_cstring_to_stream_sym(SYM_DEBUG_IO, s);
}

void cl_write_cstring_to_trace(const char *s)
{
    cl_write_cstring_to_stream_sym(SYM_TRACE_OUTPUT, s);
}

/* --- REPL column support (issue #14) --- */

/* FRESH-LINE from C on the stream bound to `sym` (a standard stream
 * variable).  A non-native binding (e.g. a Gray stream under SLY) is left
 * alone — its column is not observable from C, and the SLY REPL does its
 * own result formatting anyway. */
void cl_stream_sym_fresh_line(CL_Obj sym)
{
    CL_Obj val;
    if (CL_NULL_P(sym)) return;
    val = cl_symbol_value(sym);
    if (CL_STREAM_P(val))
        cl_stream_fresh_line(val);
}

/* The interactive REPL calls this after reading an input line: the Enter
 * that delivered the line moved the console cursor to column 0 (via the
 * terminal's input echo, which the stream layer never sees), so re-sync the
 * stream's column tracking.  Without this, the prompt's own width lingers in
 * charpos and the fresh-line before the value echo would emit a spurious
 * blank line after forms that print nothing. */
void cl_stream_sym_note_bol(CL_Obj sym)
{
    CL_Obj val;
    if (CL_NULL_P(sym)) return;
    val = cl_symbol_value(sym);
    if (!CL_STREAM_P(val)) return;
    val = cl_stream_resolve_backing(val, 1);
    if (!CL_STREAM_P(val)) return;
    ((CL_Stream *)CL_OBJ_TO_PTR(val))->charpos = 0;
}
