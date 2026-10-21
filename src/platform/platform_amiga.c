/* This file DEFINES platform_alloc / platform_free, so it must opt out of
 * the DEBUG_MEM_TRACK macros in platform.h that redirect those names to the
 * leak tracer — otherwise the definitions themselves get renamed.  It calls
 * the allocator nowhere else, so nothing here goes untracked. */
#define CL_MEM_TRACK_IMPL 1

#include "platform.h"
#include "platform_thread.h"

#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>  /* struct FileHandle (drain of RunCommand's arg-line stuffing) */
#include <string.h>
#include <stdlib.h>   /* malloc/realloc (platform_directory) */
#include <stdio.h>    /* AMILISP_SOCK_DIAG / AMILISP_IO_DIAG traces (fprintf/stderr) */
#include <stdarg.h>   /* io_diag varargs */

/* GC stop-the-world cooperation (defined in core/thread.c).  Forward-declared
 * here rather than #including core/thread.h so the platform layer stays free of
 * core/VM type dependencies.  A thread parked in a blocking socket syscall
 * cannot reach a GC safepoint; bracketing the syscall with these marks the
 * thread as "stopped" for the duration so a concurrent stop-the-world GC does
 * not deadlock waiting on it.  Both are no-ops on threads not registered with
 * the MP subsystem (e.g. the main thread before any mp:make-thread). */
extern void cl_gc_enter_safe_region(void);
extern void cl_gc_leave_safe_region(void);
extern void cl_write_cstring_to_stdout(const char *s);

void *platform_alloc(unsigned long size)
{
    void *p = AllocVec(size, MEMF_CLEAR);
    return p;
}

void platform_free(void *ptr)
{
    if (ptr) {
        FreeVec(ptr);
    }
}

void platform_write_string(const char *str)
{
    BPTR out = Output();
    if (out) {
        Write(out, (APTR)str, strlen(str));
    }
}

/* Write() above is the unbuffered DOS primitive — it bypasses the FileHandle's
 * fh_Buf layer (that buffer serves FGetC/FPutC/FPuts), so every string is
 * already one packet to the console or to the shell's `>file` redirect handle.
 * Flush() therefore has nothing of ours to push, and is here so the console
 * branch of FINISH-OUTPUT is not a lie: should anything ever put buffered DOS
 * writes on this handle, the flush is already wired through. */
void platform_flush_output(void)
{
    BPTR out = Output();
    if (out) {
        Flush(out);
    }
}

uint32_t platform_wait_signals(uint32_t mask)
{
    ULONG got;
    /* Same bracket as platform_read_line and the ARexx port's Wait: a task
     * asleep in Wait() counts as stopped for a peer's stop-the-world GC. */
    cl_gc_enter_safe_region();
    got = Wait((ULONG)mask);
    cl_gc_leave_safe_region();
    return (uint32_t)got;
}

int platform_read_line(char *buf, int bufsize)
{
    BPTR in = Input();
    char *r;
    if (!in)
        return 0;
    /* Blocking stdin read — bracket with a GC safe region exactly like the
     * socket syscalls.  cl_repl() parks here in FGets; without the bracket a
     * stop-the-world GC from a :spawn worker waits forever for this thread to
     * reach a safepoint it can never reach (the SLY REPL hang). */
    cl_gc_enter_safe_region();
    r = FGets(in, buf, bufsize);
    cl_gc_leave_safe_region();
    if (!r)
        return 0;
    /* Strip trailing newline */
    {
        int len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
            buf[len - 1] = '\0';
    }
    return 1;
}

void platform_clear_stdin_eof(void)
{
    /* No-op: console EOF (Ctrl-\) on AmigaOS is per-read — dos.library has
     * no sticky stdio-style EOF flag, and the next FGets on Input() simply
     * blocks for fresh input. */
}

/* --- TTY control (raw mode / size / input availability) ---------------
 *
 * Raw mode = SetMode(Input(), 1): the console handler delivers characters
 * as typed, with no echo and no line editing.  While raw is active,
 * console reads bypass the buffered FGetC and use Read() directly with a
 * private one-byte pushback, so the WaitForChar() availability probe and
 * the read agree on what is pending (FGetC would buffer ahead and make
 * WaitForChar lie about an escape sequence's tail).
 *
 * Single-reader assumption: like the pre-existing FGetC path, console
 * input is not safe for concurrent readers; a TUI reads keys from one
 * thread. */

static int tty_raw_active = 0;
static int tty_pushback = -1;

int platform_tty_raw(int enable)
{
    BPTR in = Input();
    if (!in || !IsInteractive(in))
        return -1;
    if ((enable != 0) == (tty_raw_active != 0))
        return 0;
    if (!SetMode(in, enable ? 1 : 0))
        return -1;
    tty_raw_active = enable ? 1 : 0;
    return 0;
}

int platform_tty_raw_active(void)
{
    return tty_raw_active;
}

int platform_tty_char_avail(void)
{
    BPTR in = Input();
    if (tty_pushback != -1)
        return 1;
    if (!in)
        return 0;
    if (!IsInteractive(in))
        return 1;   /* file/pipe input: data (or EOF) is always "ready" */
    /* Cooked-mode reads go through FGets/FGetC, whose read-ahead lives in
     * the handle's fh_Buf where WaitForChar() (a console-handler query)
     * cannot see it — the tail of a pasted multi-line form sits there after
     * FGets returned its first line.  Same probe platform_drain_input uses.
     * Raw-mode reads bypass that buffer, so only consult it in cooked mode. */
    if (!tty_raw_active) {
        struct FileHandle *fh = (struct FileHandle *)BADDR(in);
        if (fh->fh_Buf != 0 && fh->fh_Pos >= 0 && fh->fh_Pos < fh->fh_End)
            return 1;
    }
    return WaitForChar(in, 1) ? 1 : 0;   /* 1 microsecond: poll, don't wait */
}

int platform_tty_size(int *cols, int *rows)
{
    BPTR in = Input();
    BPTR out = Output();
    int was_raw = tty_raw_active;
    char buf[64];
    int len = 0;

    if (!in || !out || !IsInteractive(in) || !IsInteractive(out))
        return -1;
    /* The report below is only readable unbuffered/unechoed in raw mode. */
    if (!was_raw && platform_tty_raw(1) != 0)
        return -1;

    /* CSI "0 q" = WINDOW STATUS REQUEST; the console handler answers
     * CSI "1;1;<rows>;<cols> r" (bottom/right in character cells). */
    Write(out, (APTR)"\x9b" "0 q", 4);
    while (len < (int)sizeof(buf) - 1) {
        char c;
        if (!WaitForChar(in, 250000))   /* 250ms guard: never wedge */
            break;
        if (Read(in, &c, 1) != 1)
            break;
        buf[len++] = c;
        if (c == 'r')
            break;
    }
    if (!was_raw)
        platform_tty_raw(0);

    /* Parse the four ';'-separated numbers of the report.  Typed-ahead
     * input can precede the CSI, so scan rather than anchor at buf[0]. */
    if (len > 0 && buf[len - 1] == 'r') {
        int vals[4];
        int nv = 0;
        int i = 0;
        while (i < len && nv < 4) {
            if (buf[i] >= '0' && buf[i] <= '9') {
                int v = 0;
                while (i < len && buf[i] >= '0' && buf[i] <= '9') {
                    v = v * 10 + (buf[i] - '0');
                    i++;
                }
                vals[nv++] = v;
            } else {
                i++;
            }
        }
        if (nv == 4 && vals[2] > 0 && vals[3] > 0) {
            *rows = vals[2];
            *cols = vals[3];
            return 0;
        }
    }
    return -1;
}

int platform_getchar(void)
{
    BPTR in = Input();
    int c;
    if (tty_pushback != -1) {
        c = tty_pushback;
        tty_pushback = -1;
        return c;
    }
    if (!in)
        return -1;
    if (tty_raw_active) {
        /* Raw TUI regime: bypass FGetC's buffer so the WaitForChar()
         * availability probe and this read agree on what is pending. */
        UBYTE b;
        LONG r;
        cl_gc_enter_safe_region();
        r = Read(in, &b, 1);
        cl_gc_leave_safe_region();
        return (r == 1) ? (int)b : -1;
    }
    /* Same blocking-stdin rationale as platform_read_line: the CONSOLE stream's
     * read-char parks here, so bracket it as a GC safe region. */
    cl_gc_enter_safe_region();
    c = FGetC(in);
    cl_gc_leave_safe_region();
    return c;
}

void platform_ungetchar(int ch)
{
    if (tty_raw_active) {
        /* Raw reads bypass the buffered I/O layer, so UnGetC's buffer would
         * be invisible; park the char in the platform pushback instead
         * (getchar checks it first in either mode). */
        tty_pushback = ch;
        return;
    }
    {
        BPTR in = Input();
        if (in) {
            UnGetC(in, ch);
        }
    }
}

void platform_drain_input(void)
{
    /* AmigaOS CLI leaks command line text to Input() (stdin).
     * Drain any pending chars before the interactive REPL starts.
     *
     * The leak has TWO layers, and both must be drained:
     *
     * 1. The Shell's RunCommand() stuffs the command-line tail (arguments +
     *    terminating '\n' — just "\n" when amilisp is started with no
     *    arguments) into the input FileHandle's internal buffer so that
     *    ReadArgs()-style commands can parse their arguments from Input().
     *    amilisp takes its arguments from argv instead, so that text is
     *    still sitting unread in fh_Buf when the REPL starts.
     *    WaitForChar() asks the console HANDLER for pending characters and
     *    never sees handle-level buffered bytes, so the loop below missed
     *    them: the REPL's first FGets() returned the leaked line
     *    immediately and the empty-line skip re-printed the prompt — the
     *    "double prompt" seen at startup on AmigaOS and MorphOS.
     *    Consume the buffered bytes directly (the fh_Pos < fh_End check
     *    means FGetC serves them from the buffer and cannot block).
     *
     * 2. Type-ahead queued in the console handler itself — the
     *    WaitForChar() poll below covers that.
     *
     * Both layers are gated on IsInteractive(in): the RunCommand() fh_Buf
     * stuffing is a Shell/console-only behavior, but a redirected file or
     * pipe handle can equally arrive with DOS read-ahead already sitting in
     * fh_Buf, so draining layer 1 unconditionally could eat real piped
     * input. Gate both loops so file/pipe stdin is never touched.
     *
     * No automated regression test: the leak requires a real interactive
     * console launched through the Shell's RunCommand() path, which the
     * FS-UAE test harness (--non-interactive, redirected I/O) never
     * exercises — verify interactively on AmigaOS/MorphOS. */
    BPTR in = Input();
    struct FileHandle *fh;
    if (!in) return;
    if (!IsInteractive(in)) return;
    fh = (struct FileHandle *)BADDR(in);
    while (fh->fh_Buf != 0 && fh->fh_Pos >= 0 && fh->fh_Pos < fh->fh_End) {
        if (FGetC(in) < 0) break;
    }
    while (WaitForChar(in, 1000)) {  /* 1ms timeout per char */
        if (FGetC(in) < 0) break;
    }
}

int platform_stdin_is_interactive(void)
{
    /* IsInteractive() returns TRUE only for a real console/CON: handle, FALSE
     * for files, pipes and NIL: — exactly the gate the debugger needs. */
    BPTR in = Input();
    if (!in) return 0;
    return IsInteractive(in) ? 1 : 0;
}

/* ---- Platform file-I/O trace (AMILISP_IO_DIAG) ----
 *
 * Setting the AMILISP_IO_DIAG environment variable (any non-empty value)
 * prints one stderr line as each potentially-blocking DOS file call
 * (Open/Read/Write/Close/Lock/Delete/Rename) is ENTERED — deliberately
 * before the call, so a process found hanging in WAIT state inside
 * dos.library still shows which operation, path, and handle it entered
 * with.  A corrupted path or handle (e.g. a stale object reference after
 * a compacting GC) is then visible in the last trace line — a garbage
 * path in Open/Lock also explains a silent "please insert volume"
 * requester wait.  Runtime diagnostic, zero cost when unset (same
 * pattern as AMILISP_SOCK_DIAG / AMILISP_GC_DIAG). */
static int32_t io_diag_cached = -2;   /* -2 = env not read; 0 = off; 1 = on */

static int io_diag_on(void)
{
    if (io_diag_cached == -2) {
        char envbuf[16];
        const char *s = platform_getenv("AMILISP_IO_DIAG", envbuf,
                                        (int)sizeof(envbuf));
        io_diag_cached = (s && *s) ? 1 : 0;
    }
    return io_diag_cached;
}

static void io_diag(const char *fmt, ...)
{
    va_list ap;
    if (!io_diag_on())
        return;
    fprintf(stderr, "[IO] %lums ", (unsigned long)platform_time_ms());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

char *platform_file_read(const char *path, unsigned long *size_out)
{
    io_diag("read-file \"%s\"", path);
    BPTR fh;
    LONG fsize, nread;
    char *buf;

    *size_out = 0;
    fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) return NULL;

    /* Seek to end to get size */
    Seek(fh, 0, OFFSET_END);
    fsize = Seek(fh, 0, OFFSET_BEGINNING);

    if (fsize <= 0) { Close(fh); return NULL; }

    buf = (char *)AllocVec(fsize + 1, MEMF_CLEAR);
    if (!buf) { Close(fh); return NULL; }

    nread = Read(fh, buf, fsize);
    Close(fh);

    if (nread != fsize) {
        FreeVec(buf);
        return NULL;
    }
    buf[fsize] = '\0';
    *size_out = (unsigned long)fsize;
    return buf;
}

/* --- I/O buffer --- */

#define PLATFORM_IOBUF_SIZE 4096

typedef struct {
    char *rbuf;     /* read buffer (AllocVec'd) */
    int   rpos;     /* current read position */
    int   rlen;     /* valid bytes in read buffer */
    char *wbuf;     /* write buffer (AllocVec'd) */
    int   wlen;     /* pending bytes in write buffer */
} IOBuf;

static IOBuf *iobuf_alloc(void)
{
    IOBuf *b = (IOBuf *)AllocVec(sizeof(IOBuf), MEMF_CLEAR);
    if (!b) return NULL;
    b->rbuf = (char *)AllocVec(PLATFORM_IOBUF_SIZE, 0);
    b->wbuf = (char *)AllocVec(PLATFORM_IOBUF_SIZE, 0);
    if (!b->rbuf || !b->wbuf) {
        if (b->rbuf) FreeVec(b->rbuf);
        if (b->wbuf) FreeVec(b->wbuf);
        FreeVec(b);
        return NULL;
    }
    b->rpos = 0;
    b->rlen = 0;
    b->wlen = 0;
    return b;
}

static void iobuf_free(IOBuf *b)
{
    if (b) {
        if (b->rbuf) FreeVec(b->rbuf);
        if (b->wbuf) FreeVec(b->wbuf);
        FreeVec(b);
    }
}

/* --- Handle-based file I/O --- */

/* On Amiga, BPTR is a LONG (32-bit). We store them directly as uint32_t.
 * Slot 0 is reserved as PLATFORM_FILE_INVALID. */

#define PLATFORM_FILE_TABLE_SIZE 64

static BPTR file_table[PLATFORM_FILE_TABLE_SIZE];
static IOBuf *file_buf[PLATFORM_FILE_TABLE_SIZE];
static int file_table_init = 0;

/* Serialises slot claim (open) and slot release (close) so two threads can
 * never race a claim onto the same index or double-Close one slot (mirrors
 * the POSIX file_table_mutex / socket_table_mutex).  Per-byte read/write
 * paths on a caller-owned slot run unlocked.  Initialised on first use,
 * which is single-threaded (boot loads files before any worker exists). */
static void *file_table_mutex = NULL;

static void file_table_lock(void)
{
    if (file_table_mutex) platform_mutex_lock(file_table_mutex);
}

static void file_table_unlock(void)
{
    if (file_table_mutex) platform_mutex_unlock(file_table_mutex);
}

static void file_table_ensure_init(void)
{
    if (!file_table_init) {
        int i;
        for (i = 0; i < PLATFORM_FILE_TABLE_SIZE; i++) {
            file_table[i] = 0;
            file_buf[i] = NULL;
        }
        platform_mutex_init(&file_table_mutex);
        file_table_init = 1;
    }
}

PlatformFile platform_file_open(const char *path, int mode)
{
    BPTR fh;
    LONG amode;
    int i;

    file_table_ensure_init();

    switch (mode) {
    case PLATFORM_FILE_READ:   amode = MODE_OLDFILE;  break;
    case PLATFORM_FILE_WRITE:  amode = MODE_NEWFILE;  break;
    case PLATFORM_FILE_APPEND: amode = MODE_READWRITE; break;
    default: return PLATFORM_FILE_INVALID;
    }

    io_diag("open \"%s\" mode=%d", path, mode);

    /* Copy the path to C memory, then bracket the Open in a GC safe
     * region: Open on floppy/slow media blocks for real, and a peer's
     * stop-the-world GC would stall for its whole duration.  The copy is
     * mandatory — `path` may point into the moving Lisp arena, and a
     * peer compaction inside the safe region can relocate it.  Oversized
     * paths run unbracketed from the original pointer. */
    {
        char pathbuf[1024];
        size_t plen = strlen(path);
        if (plen < sizeof(pathbuf)) {
            memcpy(pathbuf, path, plen + 1);
            cl_gc_enter_safe_region();
            fh = Open((STRPTR)pathbuf, amode);
            if (fh && mode == PLATFORM_FILE_APPEND)
                Seek(fh, 0, OFFSET_END);
            cl_gc_leave_safe_region();
        } else {
            fh = Open((STRPTR)path, amode);
            if (fh && mode == PLATFORM_FILE_APPEND)
                Seek(fh, 0, OFFSET_END);
        }
    }
    if (!fh) return PLATFORM_FILE_INVALID;

    /* Find free slot (slot 0 reserved) — claim under the table mutex so
     * two concurrent opens never claim the same index. */
    file_table_lock();
    for (i = 1; i < PLATFORM_FILE_TABLE_SIZE; i++) {
        if (file_table[i] == 0) {
            file_table[i] = fh;
            file_buf[i] = iobuf_alloc();
            file_table_unlock();
            io_diag("open -> fh=%d bptr=0x%lx", i, (unsigned long)fh);
            return (PlatformFile)i;
        }
    }
    file_table_unlock();

    Close(fh);
    return PLATFORM_FILE_INVALID;
}

/* Flush file write buffer to disk */
static int file_flush_wbuf(PlatformFile fh)
{
    IOBuf *b;
    LONG written;
    BPTR h;
    if (fh == 0 || fh >= PLATFORM_FILE_TABLE_SIZE) return -1;
    b = file_buf[fh];
    if (!b || b->wlen == 0) return 0;
    h = file_table[fh];
    io_diag("write-flush fh=%d len=%d bptr=0x%lx", (int)fh, b->wlen,
            (unsigned long)h);
    /* The 4KB Write to floppy/slow media blocks; wbuf is C memory, so
     * bracketing is safe (a peer compaction cannot move it). */
    cl_gc_enter_safe_region();
    written = Write(h, (APTR)b->wbuf, (LONG)b->wlen);
    cl_gc_leave_safe_region();
    if (written != (LONG)b->wlen) return -1;
    b->wlen = 0;
    return 0;
}

void platform_file_close(PlatformFile fh)
{
    BPTR h = 0;
    IOBuf *b = NULL;
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE) {
        /* Detach under the mutex (double-close protection: only one caller
         * observes the non-zero slot); flush + Close outside it — DOS
         * Close/Write can block, and holding the table lock across them
         * would stall every concurrent open/close. */
        file_table_lock();
        h = file_table[fh];
        b = file_buf[fh];
        file_table[fh] = 0;
        file_buf[fh] = NULL;
        file_table_unlock();
    }
    if (h) {
        io_diag("close fh=%d bptr=0x%lx wpend=%d", (int)fh,
                (unsigned long)h, b ? b->wlen : 0);
        if (b && b->wlen > 0)
            Write(h, (APTR)b->wbuf, (LONG)b->wlen);
        Close(h);
    }
    iobuf_free(b);
}

int platform_file_getchar(PlatformFile fh)
{
    IOBuf *b;
    if (fh == 0 || fh >= PLATFORM_FILE_TABLE_SIZE || !file_table[fh])
        return -1;
    b = file_buf[fh];
    if (b) {
        if (b->rpos < b->rlen)
            return (unsigned char)b->rbuf[b->rpos++];
        /* Refill read buffer.  The 4KB Read from floppy/slow media
         * blocks; rbuf is C memory, so bracketing is safe.  The per-char
         * fast path above stays bracket-free (hot LOAD path). */
        {
            BPTR h = file_table[fh];
            LONG n;
            io_diag("read-refill fh=%d bptr=0x%lx", (int)fh,
                    (unsigned long)h);
            cl_gc_enter_safe_region();
            n = Read(h, (APTR)b->rbuf, PLATFORM_IOBUF_SIZE);
            cl_gc_leave_safe_region();
            if (n <= 0) return -1;
            b->rpos = 1;
            b->rlen = (int)n;
            return (unsigned char)b->rbuf[0];
        }
    }
    return FGetC(file_table[fh]);
}

int platform_file_read_buf(PlatformFile fh, char *buf, uint32_t len)
{
    /* Bulk read for READ-SEQUENCE.  Drain the IOBuf's buffered bytes first
     * (they were read ahead by the per-char path), then Read() the remainder
     * directly into `buf` — one DOS call instead of one per 4KB refill.
     * `buf` MUST be C memory: the Read blocks on floppy/slow media, so it is
     * bracketed in a GC safe region where a peer compaction may run. */
    IOBuf *b;
    uint32_t got = 0;
    if (fh == 0 || fh >= PLATFORM_FILE_TABLE_SIZE || !file_table[fh])
        return -1;
    b = file_buf[fh];
    if (b && b->rpos < b->rlen) {
        uint32_t avail = (uint32_t)(b->rlen - b->rpos);
        if (avail > len) avail = len;
        memcpy(buf, b->rbuf + b->rpos, avail);
        b->rpos += (int)avail;
        got = avail;
    }
    if (got < len) {
        BPTR h = file_table[fh];
        LONG n;
        io_diag("read fh=%d len=%lu bptr=0x%lx", (int)fh,
                (unsigned long)(len - got), (unsigned long)h);
        cl_gc_enter_safe_region();
        n = Read(h, (APTR)(buf + got), (LONG)(len - got));
        cl_gc_leave_safe_region();
        if (n > 0) got += (uint32_t)n;
        else if (got == 0 && n < 0) return -1;
    }
    return (int)got;
}

int platform_file_write_string(PlatformFile fh, const char *str)
{
    LONG len;
    if (fh == 0 || fh >= PLATFORM_FILE_TABLE_SIZE || !file_table[fh])
        return -1;
    len = strlen(str);
    return platform_file_write_buf(fh, str, (uint32_t)len);
}

int platform_file_write_char(PlatformFile fh, int ch)
{
    IOBuf *b;
    if (fh == 0 || fh >= PLATFORM_FILE_TABLE_SIZE || !file_table[fh])
        return -1;
    b = file_buf[fh];
    if (b) {
        b->wbuf[b->wlen++] = (char)ch;
        if (b->wlen >= PLATFORM_IOBUF_SIZE)
            return file_flush_wbuf(fh);
        return 0;
    }
    return (FPutC(file_table[fh], ch) != -1) ? 0 : -1;
}

int platform_file_write_buf(PlatformFile fh, const char *buf, uint32_t len)
{
    IOBuf *b;
    if (fh == 0 || fh >= PLATFORM_FILE_TABLE_SIZE || !file_table[fh])
        return -1;
    b = file_buf[fh];
    if (b) {
        uint32_t pos = 0;
        while (pos < len) {
            int avail = PLATFORM_IOBUF_SIZE - b->wlen;
            int chunk = (int)(len - pos);
            if (chunk > avail) chunk = avail;
            memcpy(b->wbuf + b->wlen, buf + pos, (size_t)chunk);
            b->wlen += chunk;
            pos += (uint32_t)chunk;
            if (b->wlen >= PLATFORM_IOBUF_SIZE) {
                if (file_flush_wbuf(fh) != 0) return -1;
            }
        }
        return 0;
    }
    /* Fallback (no IOBuf — allocation failed at open): chunk through a
     * C stack buffer so the blocking Write can be bracketed.  Each copy
     * happens OUTSIDE the safe region; the syscall inside it reads only
     * the C copy.  NOTE: `buf` is re-read across earlier chunks' safe
     * regions, so multi-chunk writes require C-memory input — which
     * holds for every current caller (the stream layer chunks arena
     * strings through rooted CL_Objs before reaching platform level;
     * the FASL writer passes platform memory). */
    {
        char chunk[512];
        uint32_t pos = 0;
        BPTR h = file_table[fh];
        io_diag("write fh=%d len=%lu bptr=0x%lx", (int)fh,
                (unsigned long)len, (unsigned long)h);
        while (pos < len) {
            uint32_t nb = len - pos;
            LONG written;
            if (nb > (uint32_t)sizeof(chunk)) nb = (uint32_t)sizeof(chunk);
            memcpy(chunk, buf + pos, nb);
            cl_gc_enter_safe_region();
            written = Write(h, (APTR)chunk, (LONG)nb);
            cl_gc_leave_safe_region();
            if (written != (LONG)nb) return -1;
            pos += nb;
        }
        return 0;
    }
}

int platform_file_flush(PlatformFile fh)
{
    return file_flush_wbuf(fh);
}

int platform_file_eof(PlatformFile fh)
{
    /* Amiga: no direct feof equivalent; we detect EOF via FGetC returning -1 */
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh])
        return 0;  /* Can't know without reading */
    return 1;
}

long platform_file_position(PlatformFile fh)
{
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh]) {
        long pos = (long)Seek(file_table[fh], 0, OFFSET_CURRENT);
        /* Adjust for buffered but unread data */
        IOBuf *b = file_buf[fh];
        if (b && b->rlen > 0)
            pos -= (long)(b->rlen - b->rpos);
        /* Adjust for buffered but unflushed writes */
        if (b && b->wlen > 0)
            pos += (long)b->wlen;
        return pos;
    }
    return -1;
}

int platform_file_set_position(PlatformFile fh, long pos)
{
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh]) {
        IOBuf *b = file_buf[fh];
        BPTR h = file_table[fh];
        int rc;
        /* Flush writes and invalidate read buffer on seek */
        if (b) {
            file_flush_wbuf(fh);
            b->rpos = 0;
            b->rlen = 0;
        }
        /* The Seek hits the disk on AmigaDOS — bracket it. */
        cl_gc_enter_safe_region();
        rc = Seek(h, pos, OFFSET_BEGINNING) >= 0 ? 0 : -1;
        cl_gc_leave_safe_region();
        return rc;
    }
    return -1;
}

long platform_file_length(PlatformFile fh)
{
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh]) {
        BPTR h = file_table[fh];
        long cur, end;
        cl_gc_enter_safe_region();
        cur = Seek(h, 0, OFFSET_CURRENT);
        Seek(h, 0, OFFSET_END);
        end = Seek(h, 0, OFFSET_CURRENT);
        Seek(h, cur, OFFSET_BEGINNING);
        cl_gc_leave_safe_region();
        return end;
    }
    return -1;
}

uint32_t platform_time_ms(void)
{
    struct DateStamp ds;
    DateStamp(&ds);
    /* ds_Minute: minutes since midnight, ds_Tick: 1/50s ticks since last minute */
    return (uint32_t)(ds.ds_Minute * 60000UL + ds.ds_Tick * 20UL);
}

uint32_t platform_run_time_ms(void)
{
    /* AmigaOS exec has no per-task CPU accounting — wall-clock time is the
     * best available approximation (and close to exact on a machine where
     * amilisp is the only busy task). */
    return platform_time_ms();
}

uint64_t platform_time_us(void)
{
    /* DateStamp ticks are 1/50s — microsecond resolution is not available
     * without opening timer.device; scale the ms clock instead. */
    return (uint64_t)platform_time_ms() * 1000u;
}

void platform_sleep_ms(uint32_t milliseconds)
{
    /* Delay() takes ticks (1/50s = 20ms each). Round up. */
    LONG ticks = (LONG)((milliseconds + 19) / 20);
    if (ticks > 0) {
        /* A sleeping task cannot reach a safepoint — without the safe
         * region every peer's stop-the-world GC stalls for the whole
         * nap.  Safe regions nest (safe_region_depth). */
        cl_gc_enter_safe_region();
        Delay(ticks);
        cl_gc_leave_safe_region();
    }
}

uint32_t platform_universal_time(void)
{
    /* AmigaOS epoch: 1978-01-01 00:00:00 UTC
     * CL universal time epoch: 1900-01-01 00:00:00 UTC
     * Difference: 78 years = 2461449600 seconds
     *   (1900-1978: 78 years, 19 leap years: 78*365 + 19 = 28489 days * 86400) */
    struct DateStamp ds;
    uint32_t amiga_secs;
    DateStamp(&ds);
    amiga_secs = (uint32_t)(ds.ds_Days * 86400UL + ds.ds_Minute * 60UL + ds.ds_Tick / 50UL);
    return amiga_secs + 2461449600UL;
}

int platform_file_exists(const char *path)
{
    BPTR lock;
    io_diag("probe \"%s\"", path);
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    return 0;
}

int platform_file_is_directory(const char *path)
{
    BPTR lock;
    io_diag("probe-dir \"%s\"", path);
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock) {
        struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR);
        int result = 0;
        if (fib) {
            if (Examine(lock, fib)) {
                result = (fib->fib_DirEntryType > 0) ? 1 : 0;
            }
            FreeVec(fib);
        }
        UnLock(lock);
        return result;
    }
    return 0;
}

int platform_file_delete(const char *path)
{
    io_diag("delete \"%s\"", path);
    return DeleteFile((STRPTR)path) ? 0 : -1;
}

int platform_file_rename(const char *oldpath, const char *newpath)
{
    /* AmigaOS Rename() fails if target exists; delete target first to match
       POSIX rename() semantics (atomic overwrite) */
    BPTR lock;
    io_diag("rename \"%s\" -> \"%s\"", oldpath, newpath);
    lock = Lock((STRPTR)newpath, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        DeleteFile((STRPTR)newpath);
    }
    return Rename((STRPTR)oldpath, (STRPTR)newpath) ? 0 : -1;
}

int platform_file_make_executable(const char *path)
{
    /* All of rwed (the bits are "denied" flags): what a new file has
     * anyway on a native filesystem, said explicitly for the ones that
     * derive the bits from somewhere else (a host directory under UAE). */
    return SetProtection((STRPTR)path, 0) ? 0 : -1;
}

uint32_t platform_file_mtime(const char *path)
{
    BPTR lock;
    io_diag("probe-mtime \"%s\"", path);
    lock = Lock((STRPTR)path, ACCESS_READ);
    if (lock) {
        struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocVec(sizeof(struct FileInfoBlock), MEMF_CLEAR);
        uint32_t result = 0;
        if (fib) {
            if (Examine(lock, fib)) {
                /* fib_Date is a DateStamp relative to Amiga epoch (1978-01-01) */
                uint32_t amiga_secs = (uint32_t)(fib->fib_Date.ds_Days * 86400UL
                    + fib->fib_Date.ds_Minute * 60UL
                    + fib->fib_Date.ds_Tick / 50UL);
                result = amiga_secs + 2461449600UL;
            }
            FreeVec(fib);
        }
        UnLock(lock);
        return result;
    }
    return 0;
}

int platform_mkdir(const char *path)
{
    BPTR lock;
    char clean[256];
    int len;

    /* Strip trailing slash — AmigaOS CreateDir fails with trailing '/' */
    len = strlen(path);
    if (len > 0 && len < (int)sizeof(clean)) {
        memcpy(clean, path, (size_t)len + 1);
        if (len > 1 && clean[len - 1] == '/')
            clean[len - 1] = '\0';
    } else {
        /* Path too long or empty */
        return -1;
    }

    /* Check if it already exists */
    lock = Lock((STRPTR)clean, ACCESS_READ);
    if (lock) {
        UnLock(lock);
        return 0; /* Already exists */
    }
    lock = CreateDir((STRPTR)clean);
    if (lock) {
        UnLock(lock);
        return 0;
    }
    return -1;
}

const char *platform_getenv_raw(const char *name, char *buf, int bufsize)
{
    LONG len = GetVar((STRPTR)name, buf, bufsize, 0);
    if (len < 0) return NULL;
    return buf;
}

unsigned long platform_mem_available(void)
{
    return (unsigned long)AvailMem(MEMF_ANY);
}

int platform_break_pending(void)
{
    /* The console/shell delivers Ctrl-C as SIGBREAKF_CTRL_C on the running
     * task; SetSignal(0, mask) reads-and-clears it atomically.  Each thread
     * (AmigaOS process) polls its own task signal. */
    return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

const char *platform_executable_prefix(char *buf, int bufsize)
{
    /* PROGDIR: is the executable's directory; as a device-style prefix it
     * concatenates directly with a relative path (PROGDIR:lib/...). */
    if (bufsize < (int)sizeof("PROGDIR:"))
        return NULL;
    strcpy(buf, "PROGDIR:");
    return buf;
}

/* The tool's file name out of the WBStartup message (platform_startup_args):
 * a Workbench-started process is no CLI, so GetProgramName has nothing. */
static const char *wb_tool_name = NULL;

const char *platform_executable_path(char *buf, int bufsize)
{
    /* PROGDIR: plus the program's own file name (dos.library 36+ keeps the
     * name for the process): what a Shell can start, whatever its cwd.
     * GetProgramName hands back the command as the Shell found it, which
     * can be a full path (Work:apps/amilisp) -- PROGDIR: already stands for
     * its directory, so only the last component goes after it. */
    char name[128];
    const char *file;
    if (wb_tool_name) {
        strncpy(name, wb_tool_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    } else if (!GetProgramName((STRPTR)name, (LONG)sizeof(name)) ||
               name[0] == '\0')
        return NULL;
    file = (const char *)FilePart((STRPTR)name);
    if (file[0] == '\0')
        return NULL;
    if (bufsize < (int)(sizeof("PROGDIR:") + strlen(file)))
        return NULL;
    strcpy(buf, "PROGDIR:");
    strcat(buf, file);
    return buf;
}

long platform_stack_headroom(void)
{
#ifdef PLATFORM_MORPHOS
    /* MorphOS: native PPC code runs on the task's *PPC* stack, which is
     * separate from the 68k stack that tc_SPLower/tc_SPUpper describe —
     * measuring &probe (a PPC-stack address) against tc_SPLower yields
     * garbage.  The PPC stack bounds live in the ETask (exec/tasks.h:
     * PPCSPLower/PPCSPUpper), present for every MorphOS task.
     *
     * Sanity-gate on &probe actually lying INSIDE those bounds: code can
     * run on a stack the ETask does not describe (a libnix __stack swap,
     * StackSwap, or a foreign callback context), and measuring against
     * the wrong stack's bounds yields a garbage difference — a bogus
     * small-positive reading here made the C-stack guard kill the boot
     * load on the first MorphOS run.  Out of bounds → -1 (unknown), which
     * disables the guard rather than misfiring it. */
    struct Task *t = FindTask(NULL);
    char probe;
    char *lower, *upper;
    if (!t || !t->tc_ETask)
        return -1;
    lower = (char *)t->tc_ETask->PPCSPLower;
    upper = (char *)t->tc_ETask->PPCSPUpper;
    if (!lower || !upper || &probe < lower || &probe >= upper)
        return -1;
    /* PPC stacks grow down too: headroom = SP - lower bound. */
    return (long)(&probe - lower);
#else
    struct Task *t = FindTask(NULL);
    char probe;
    if (!t || !t->tc_SPLower)
        return -1;
    /* m68k stacks grow down: headroom = SP - lower bound. */
    return (long)(&probe - (char *)t->tc_SPLower);
#endif
}

#ifndef PLATFORM_MORPHOS
/* The replay loop is assembly (cpu_store_probe_m68k.s): the chain must
 * reach the CPU exactly as gcc's prologue emits it, with nothing the
 * compiler might store nearby -- the defect eats stores, and a lost loop
 * counter never ends.  One absolute source only: a sweep over several
 * froze the Vampire (the file says why in full). */
extern uint32_t platform_cpu_store_probe_m68k(uint32_t replays);
#endif

uint32_t platform_cpu_store_selftest(uint32_t rounds)
{
#ifdef PLATFORM_MORPHOS
    (void)rounds;
    return 0;                           /* PPC: no such defect known */
#else
    return platform_cpu_store_probe_m68k(rounds);
#endif
}

int platform_executable_ancestor_prefix(int levels, char *buf, int bufsize)
{
    BPTR lock = Lock((STRPTR)"PROGDIR:", ACCESS_READ);
    int i;
    size_t n;
    if (!lock)
        return 0;
    for (i = 0; i < levels; i++) {
        BPTR parent = ParentDir(lock);
        UnLock(lock);
        if (!parent)
            return 0;
        lock = parent;
    }
    if (!NameFromLock(lock, (STRPTR)buf, bufsize - 1)) {
        UnLock(lock);
        return 0;
    }
    UnLock(lock);
    /* NameFromLock yields "Volume:" for a root or "Volume:dir/sub" for a
     * directory — append '/' in the latter case so a relative file path
     * concatenates directly. */
    n = strlen(buf);
    if (n == 0)
        return 0;
    if (buf[n - 1] != ':' && buf[n - 1] != '/') {
        if ((int)n + 1 >= bufsize)
            return 0;
        buf[n] = '/';
        buf[n + 1] = '\0';
    }
    return 1;
}

int platform_getcwd(char *buf, int bufsize)
{
    BPTR lock = Lock("", ACCESS_READ);
    if (lock) {
        if (NameFromLock(lock, (STRPTR)buf, bufsize)) {
            UnLock(lock);
            return (int)strlen(buf);
        }
        UnLock(lock);
    }
    return 0;
}

int platform_system(const char *command)
{
    LONG rc;
    /* `command` typically points into a Lisp string's arena data
     * (EXT:SYSTEM-COMMAND passes s->data) and SystemTagList blocks for
     * the child's whole runtime — copy to C memory FIRST, then bracket
     * (a peer compaction inside the safe region can move the source).
     * OOM fallback: run unbracketed from the original pointer. */
    size_t clen = strlen(command);
    char *cmdbuf = (char *)malloc(clen + 1);
    if (cmdbuf) {
        memcpy(cmdbuf, command, clen + 1);
        cl_gc_enter_safe_region();
        rc = SystemTagList((STRPTR)cmdbuf, NULL);
        cl_gc_leave_safe_region();
        free(cmdbuf);
    } else {
        rc = SystemTagList((STRPTR)command, NULL);
    }
    return (int)rc;
}

/* Convert Unix glob pattern to AmigaDOS pattern.
   '*' -> '#?', rest passed through. */
static void unix_to_amiga_pattern(const char *src, char *dst, int dstsize)
{
    int si = 0, di = 0;
    while (src[si] && di < dstsize - 3) {
        if (src[si] == '*') {
            dst[di++] = '#';
            dst[di++] = '?';
            si++;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
}

char **platform_directory(const char *pattern, int *count_out)
{
    struct AnchorPath *ap;
    char amiga_pat[512];
    char **result = NULL;
    int count = 0;
    int capacity = 16;
    LONG rc;

    *count_out = 0;

    unix_to_amiga_pattern(pattern, amiga_pat, (int)sizeof(amiga_pat));

    /* AnchorPath must be longword-aligned and zeroed */
    ap = (struct AnchorPath *)AllocVec(sizeof(struct AnchorPath) + 256,
                                       MEMF_CLEAR);
    if (!ap) return NULL;
    ap->ap_Strlen = 256; /* space for full path */

    result = (char **)malloc((size_t)(capacity + 1) * sizeof(char *));
    if (!result) { FreeVec(ap); return NULL; }

    /* Directory enumeration hits the disk per entry; everything touched
     * inside (amiga_pat, ap, result — all C memory) is compaction-proof,
     * so bracket the whole walk.  MatchFirst/MatchNext do the blocking. */
    cl_gc_enter_safe_region();
    rc = MatchFirst((STRPTR)amiga_pat, ap);
    while (rc == 0) {
        if (count >= capacity) {
            capacity *= 2;
            result = (char **)realloc(result,
                         (size_t)(capacity + 1) * sizeof(char *));
            if (!result) break;
        }
        if (ap->ap_Info.fib_DirEntryType > 0) {
            /* Directory entry — append '/' so parser creates directory pathname */
            size_t slen = strlen((char *)ap->ap_Buf);
            char *dname = (char *)malloc(slen + 2);
            if (dname) {
                memcpy(dname, ap->ap_Buf, slen);
                dname[slen] = '/';
                dname[slen + 1] = '\0';
                result[count] = dname;
                count++;
            }
        } else {
            result[count] = strdup((char *)ap->ap_Buf);
            count++;
        }
        rc = MatchNext(ap);
    }
    MatchEnd(ap);
    cl_gc_leave_safe_region();
    FreeVec(ap);

    if (result) {
        result[count] = NULL;
        *count_out = count;
    }
    return result;
}

const char *platform_realpath(const char *path, char *buf, int bufsize)
{
    BPTR lock;
    int has_volume = 0;
    const char *p;

    /* AmigaOS path syntax: an absolute path begins with a volume or assign
     * name followed by ':' (e.g. "T:foo", "Ram Disk:bar", "DH0:lisp/x").
     * If the user already supplied such a prefix, preserve it instead of
     * canonicalizing through NameFromLock — otherwise PROBE-FILE,
     * RENAME-FILE etc. return paths the user can't recognize
     * (e.g. "T:foo" → "Ram Disk:T/foo"). */
    for (p = path; *p; p++) {
        if (*p == ':') { has_volume = 1; break; }
        if (*p == '/') break;  /* relative path with subdir component */
    }

    lock = Lock((STRPTR)path, ACCESS_READ);
    if (!lock) return NULL;

    if (has_volume) {
        size_t plen = strlen(path);
        UnLock(lock);
        if ((int)(plen + 1) > bufsize) return NULL;
        memcpy(buf, path, plen + 1);
        return buf;
    }

    if (!NameFromLock(lock, (STRPTR)buf, (LONG)bufsize)) {
        UnLock(lock);
        return NULL;
    }
    UnLock(lock);
    return buf;
}

const char *platform_expand_home(const char *path, char *buf, int bufsize)
{
    size_t plen;

    if (!path || path[0] != '~') return path;
    /* Only expand ~/... or bare ~ (not ~user) */
    if (path[1] != '\0' && path[1] != '/') return path;

    /* ~ maps to PROGDIR: on AmigaOS */
    if (path[1] == '\0') {
        /* bare ~ -> "PROGDIR:" */
        if (bufsize < 9) return path;
        memcpy(buf, "PROGDIR:", 9);
        return buf;
    }
    /* ~/rest -> PROGDIR:rest */
    plen = strlen(path + 2); /* skip ~/ */
    if ((int)(8 + plen + 1) > bufsize) return path;
    memcpy(buf, "PROGDIR:", 8);
    memcpy(buf + 8, path + 2, plen + 1);
    return buf;
}

/* --- TCP Socket I/O via bsdsocket.library — single-owner reactor model ---
 *
 * AmigaOS bsdsocket.library is task-specific: the library base AND every
 * socket fd are owned by the task that created them, so a socket cannot be
 * used from a different task.  amilisp threads are separate AmigaOS processes
 * (CreateNewProc), so naive cross-thread socket I/O blocks forever.
 *
 * To give POSIX-like semantics (any thread may use any socket, including one
 * thread reading while another writes the same socket), ALL bsdsocket calls
 * are funnelled to a single dedicated "reactor" process that exclusively owns
 * SocketBase, the socket table, and every fd.  Other threads never touch
 * bsdsocket: their platform_socket_* calls marshal a request to the reactor
 * over an Exec message port and block on the reply (inside a GC safe region).
 * The reactor multiplexes all sockets with WaitSelect so one slow/blocked
 * socket never stalls the others.  AmigaOS's single shared address space lets
 * the reactor recv/send directly into the caller's IOBuf by pointer — no data
 * copy between tasks.
 */

#if PLATFORM_MORPHOS
#include <proto/socket.h>
/* MorphOS's proto/socket.h brings in sockaddr_in but not struct hostent;
 * gethostbyname() needs <netdb.h> for the full definition. */
#include <netdb.h>
#else
#include <proto/bsdsocket.h>
/* GCC 15+ rejects the Roadshow-era inline stub for SetErrnoPtr that
 * amiga-netinclude's inline/bsdsocket.h provides: it binds the two
 * arguments to a0/d0 as register INPUTS and lists a0/d0 among the
 * clobbers ("'asm' specifier for variable conflicts with 'asm' clobber
 * list").  The library call does trash them, and a void function has no
 * d0 result that would make the clobber legal, so bind them as in-out
 * operands instead.  LVO -168: SetErrnoPtr(errno_ptr/a0, size/d0). */
#undef SetErrnoPtr
#define SetErrnoPtr(errno_ptr, size) do { \
    register struct Library *__sep_base __asm("a6") = SocketBase; \
    register APTR __sep_ptr __asm("a0") = (APTR)(errno_ptr); \
    register LONG __sep_size __asm("d0") = (LONG)(size); \
    __asm volatile ("jsr a6@(-168:W)" \
        : "+r"(__sep_ptr), "+r"(__sep_size) \
        : "r"(__sep_base) \
        : "d1", "a1", "fp0", "fp1", "cc", "memory"); \
} while (0)
#endif
#include <exec/ports.h>
#include <exec/tasks.h>
#include <dos/dostags.h>
#include <sys/time.h>   /* struct timeval for WaitSelect zero-timeout poll */
#include "platform_amiga_ppc.h"   /* NP_Entry gate for the MorphOS PPC build */

/* Address-length type for accept/getsockname/getsockopt.  The MorphOS
 * socket.library API takes LONG* (and does not declare socklen_t), whereas
 * Roadshow/bsdsocket on classic AmigaOS provides socklen_t.  Use a portable
 * alias so the same call sites compile cleanly on both. */
#if PLATFORM_MORPHOS
typedef LONG cl_socklen_t;
#else
typedef socklen_t cl_socklen_t;
#endif

/* Roadshow's <netinet/in.h> defines INADDR_ANY but leaves INADDR_LOOPBACK to
 * <arpa/inet.h>, which we don't pull in.  Provide the standard value. */
#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((uint32_t)0x7f000001UL)
#endif
/* The -noixemul SDK headers omit these — supply the BSD values bsdsocket uses. */
#ifndef EWOULDBLOCK
#define EWOULDBLOCK 35
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 36
#endif
#ifndef FIONBIO
#define FIONBIO 0x8004667EUL   /* _IOW('f', 126, int) — set non-blocking */
#endif

struct Library *SocketBase = NULL;   /* opened by, and only used from, the reactor */
static LONG socket_errno = 0;

/* ---- TLS provider (AmiSSL v5 = the OpenSSL 3.x API as an Amiga shared
 * library).  Compiled in when the vendored AmiSSL SDK headers are present
 * (include/amissl-sdk -> Makefile.cross/-mos -DCL_HAVE_AMISSL); the
 * library itself is opened lazily at runtime, so a machine without AmiSSL
 * still runs — TLS just reports unavailable.
 *
 * AmiSSL is task-bound exactly like bsdsocket (per-task InitAmiSSL, needs
 * the owning task's SocketBase for SSL_set_fd I/O), so the reactor owns it
 * outright: every TLS call below runs in the reactor task, and client
 * threads reach TLS through REQ_TLS_* messages like any other socket op.
 * The gcc build calls AmiSSL through the SDK's inline headers (register-
 * mapped calls via AmiSSLBase) — no link library involved. */
#ifdef CL_HAVE_AMISSL
#define CL_REACTOR_TLS 1
#include <amissl/amissl.h>
#include <libraries/amisslmaster.h>
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
struct Library *AmiSSLMasterBase = NULL;  /* reactor-owned */
struct Library *AmiSSLBase = NULL;        /* referenced by the inline headers */
struct Library *AmiSSLExtBase = NULL;     /* referenced by <proto/amissl.h> */
#endif

/* Fixed cap: bounded by bsdsocket.library's per-task descriptor table (Roadshow
 * default ~64).  Going higher would require SocketBaseTags(SBTC_DTABLESIZE).
 * The host (platform_posix.c) has no such ceiling and grows its table on demand. */
#define PLATFORM_SOCKET_TABLE_SIZE 64

/* Reactor-owned: fd per slot (-1 = free), IOBuf per slot (NULL for listeners).
 * Slot 0 is reserved as the INVALID handle.  Only the reactor mutates fds; the
 * IOBuf bytes are shared with the requesting thread, but never concurrently —
 * the request/reply handshake serialises access. */
static LONG   socket_table[PLATFORM_SOCKET_TABLE_SIZE];
static IOBuf *socket_buf[PLATFORM_SOCKET_TABLE_SIZE];
/* Per-socket read/write timeouts in milliseconds (0 = none).  Written by
 * client threads via platform_socket_set_timeout and read by the client stubs
 * to stamp each request; stream.c serialises per socket+direction. */
static int    socket_rtimeout[PLATFORM_SOCKET_TABLE_SIZE];
static int    socket_wtimeout[PLATFORM_SOCKET_TABLE_SIZE];

/* ---- Minimal fd_set (the toolchain headers don't provide one) ----
 * Standard BSD layout: bit n in word n/32, which is what WaitSelect expects. */
#define CL_FDSET_WORDS 8   /* up to 256 fds */
typedef struct { uint32_t bits[CL_FDSET_WORDS]; } CL_fdset;
#define CL_FD_ZERO(s)    memset((s), 0, sizeof(*(s)))
#define CL_FD_SET(n,s)   ((s)->bits[(unsigned)(n) >> 5] |= (1UL << ((unsigned)(n) & 31)))
#define CL_FD_ISSET(n,s) (((s)->bits[(unsigned)(n) >> 5] >> ((unsigned)(n) & 31)) & 1U)

/* ---- Reactor request protocol ---- */
enum {
    REQ_CONNECT = 1, REQ_LISTEN, REQ_ACCEPT,
    REQ_READFILL, REQ_WRITE, REQ_CLOSE, REQ_SHUTDOWN, REQ_POLL,
    /* UDP: connect a datagram socket.  Datagram I/O itself reuses
     * REQ_READFILL (one recv = one datagram into the caller's buffer) and
     * REQ_WRITE (UDP send never partial-sends, so pend_wpos stays 0). */
    REQ_UDP_CONNECT,
    REQ_ENDPOINT,    /* getsockname: dotted-quad into req->buf, port into out_port */
    /* TLS (CL_REACTOR_TLS builds; otherwise answered with -1):
     * TLS_START runs the handshake on an established connection (params in
     * tls_params, diagnostic into buf/len); once it succeeds the slot's
     * READFILL/WRITE/POLL ops transparently move ciphertext.  TLS_PEERCERT
     * copies one certificate field (selector in `port`) into buf. */
    REQ_TLS_START,
    REQ_TLS_PEERCERT
};

typedef struct SockReq {
    struct Message msg;          /* mn_ReplyPort = caller's stack reply port */
    int            op;
    PlatformSocket slot;         /* target slot (read/write/close; listener for accept) */
    const char    *host;         /* connect */
    int            port;         /* connect / listen */
    int            loopback;     /* listen: 127.0.0.1 only */
    uint32_t       bind_addr;    /* listen: the one address to bind, host
                                    order; 0 (with loopback 0) = every one */
    char          *buf;          /* readfill destination / write source */
    uint32_t       len;          /* readfill capacity / write length */
    int            timeout_ms;   /* readfill/write: 0 = block forever, else deadline */
    volatile int            result;    /* readfill: bytes (0=EOF, -2=timeout); else 0=ok/-1=err/-2=timeout */
    volatile PlatformSocket out_slot;  /* connect/listen/accept: new slot */
    volatile int            out_port;  /* listen: bound port */
    uint32_t       seq;          /* trace id (AMILISP_SOCK_DIAG); 0 when tracing is off */
    const PlatformTLSParams *tls_params; /* TLS_START: handshake parameters
                                          * (caller-stack struct; valid for the
                                          * whole call since the caller blocks) */
} SockReq;

/* ---- Reactor request trace (AMILISP_SOCK_DIAG) ----
 *
 * Setting the AMILISP_SOCK_DIAG environment variable (any non-empty value)
 * traces every request through the client<->reactor handshake on stderr,
 * one line per handoff:
 *
 *   [SOCK] 12345ms #17 > CONNECT slot=0 len=0 to=30000 task=0x...   client PutMsg
 *   [SOCK] 12345ms #17 rx CONNECT slot=0                            reactor received
 *   [SOCK] 12345ms #17 dns "beta.quicklisp.org"                     blocking DNS start
 *   [SOCK] 12395ms #17 dns ok                                       DNS returned
 *   [SOCK] 12395ms #17 park-w slot=3                                parked, awaiting fd
 *   [SOCK] 12400ms #17 resume-w slot=3                              WaitSelect readiness
 *   [SOCK] 12400ms #17 tx CONNECT result=0 out=3                    reactor replied
 *   [SOCK] 12400ms #17 < CONNECT result=0 out=3                     client woke
 *
 * A hang's LAST trace line names the lost handoff:
 *   ">" without "rx"          — request posted, reactor never received it
 *                               (reactor dead, or WaitSelect missed the port signal)
 *   "dns" without "dns ok"    — reactor stalled inside gethostbyname (blocks
 *                               ALL socket traffic behind it)
 *   "park" without "resume"/"expire" — reactor waiting in WaitSelect for
 *                               readiness that never signalled
 *   "tx" without "<"          — reply posted, client never woke (lost reply signal)
 *
 * Runtime diagnostic, not DEBUG-flag instrumentation: always compiled, zero
 * cost when the env var is unset (same pattern as AMILISP_STW_DIAG /
 * AMILISP_LOCK_DIAG).  Reactor- and client-side lines can interleave; each
 * line is a single fprintf so it stays whole. */
static int32_t sock_diag_cached = -2;   /* -2 = env not read; 0 = off; 1 = on */
static volatile uint32_t sock_diag_seq = 0;

static int sock_diag_on(void)
{
    if (sock_diag_cached == -2) {
        char envbuf[16];
        const char *s = platform_getenv("AMILISP_SOCK_DIAG", envbuf,
                                        (int)sizeof(envbuf));
        sock_diag_cached = (s && *s) ? 1 : 0;
    }
    return sock_diag_cached;
}

static const char *sock_op_name(int op)
{
    switch (op) {
    case REQ_CONNECT:     return "CONNECT";
    case REQ_LISTEN:      return "LISTEN";
    case REQ_ACCEPT:      return "ACCEPT";
    case REQ_READFILL:    return "READFILL";
    case REQ_WRITE:       return "WRITE";
    case REQ_CLOSE:       return "CLOSE";
    case REQ_SHUTDOWN:    return "SHUTDOWN";
    case REQ_POLL:        return "POLL";
    case REQ_UDP_CONNECT: return "UDP-CONNECT";
    case REQ_ENDPOINT:    return "ENDPOINT";
    case REQ_TLS_START:   return "TLS-START";
    case REQ_TLS_PEERCERT: return "TLS-PEERCERT";
    default:              return "?";
    }
}

/* One trace line: "[SOCK] <time>ms #<seq> <event>" + optional detail. */
static void sock_diag_line(const SockReq *req, const char *event,
                           const char *detail)
{
    fprintf(stderr, "[SOCK] %lums #%lu %s%s%s\n",
            (unsigned long)platform_time_ms(),
            (unsigned long)req->seq, event,
            detail ? " " : "", detail ? detail : "");
    fflush(stderr);
}

/* ---- Reactor state (all touched only by the reactor task) ---- */
static struct Process *reactor_proc = NULL;
static struct MsgPort *reactor_port = NULL;   /* request port, owned by reactor */
static struct Task    *reactor_boot_task = NULL;
static BYTE            reactor_boot_sig = -1;
static void           *reactor_init_mutex = NULL;

/* Parked op per slot+direction (stream.c serialises per socket+direction, so
 * at most one outstanding op each way).  pend_wpos tracks bytes already sent
 * for a partially-completed write. */
static SockReq *pend_read[PLATFORM_SOCKET_TABLE_SIZE];
static SockReq *pend_write[PLATFORM_SOCKET_TABLE_SIZE];
static uint32_t pend_wpos[PLATFORM_SOCKET_TABLE_SIZE];
/* TLS can invert a parked op's fd interest: the record layer may need the fd
 * WRITABLE to finish a read (renegotiation) or READABLE to finish a write /
 * handshake step.  These flags pick the WaitSelect set the parked op waits
 * in; plain sockets never set them, so behavior there is unchanged. */
static uint8_t  pend_read_wants_write[PLATFORM_SOCKET_TABLE_SIZE];
static uint8_t  pend_write_wants_read[PLATFORM_SOCKET_TABLE_SIZE];
#ifdef CL_REACTOR_TLS
/* Per-slot TLS connection state (reactor-owned; clients only ever READ the
 * socket_ssl pointer, an aligned 32-bit load, for platform_tls_active). */
static void *socket_ssl[PLATFORM_SOCKET_TABLE_SIZE];      /* SSL* */
static void *socket_ssl_ctx[PLATFORM_SOCKET_TABLE_SIZE];  /* SSL_CTX* */
#endif
/* Absolute deadline (platform_time_ms) for a parked read/write; valid only
 * when the matching pend_*_has_deadline flag is set.  Lets the reactor time a
 * parked op out instead of waiting on it forever. */
static uint32_t pend_read_deadline[PLATFORM_SOCKET_TABLE_SIZE];
static uint32_t pend_write_deadline[PLATFORM_SOCKET_TABLE_SIZE];
static int      pend_read_has_deadline[PLATFORM_SOCKET_TABLE_SIZE];
static int      pend_write_has_deadline[PLATFORM_SOCKET_TABLE_SIZE];

/* Stamp a parked op's deadline from its requested timeout (ms). */
static void reactor_arm_deadline(int slot, int timeout_ms, int is_write)
{
    if (timeout_ms > 0) {
        uint32_t dl = platform_time_ms() + (uint32_t)timeout_ms;
        if (is_write) { pend_write_deadline[slot] = dl; pend_write_has_deadline[slot] = 1; }
        else          { pend_read_deadline[slot]  = dl; pend_read_has_deadline[slot]  = 1; }
    } else {
        if (is_write) pend_write_has_deadline[slot] = 0;
        else          pend_read_has_deadline[slot]  = 0;
    }
}

/* ===== Reactor-side helpers (run in the reactor task; may call bsdsocket) ===== */

static void reactor_set_nonblock(LONG fd)
{
    LONG one = 1;
    IoctlSocket(fd, FIONBIO, (char *)&one);
}

/* Claim a free slot for fd; allocate an IOBuf unless with_buf==0 (listeners). */
static int reactor_alloc_slot(LONG fd, int with_buf)
{
    int i;
    for (i = 1; i < PLATFORM_SOCKET_TABLE_SIZE; i++) {
        if (socket_table[i] == -1) {
            socket_buf[i] = with_buf ? iobuf_alloc() : NULL;
            if (with_buf && !socket_buf[i]) return -1;
            socket_table[i] = fd;
            return i;
        }
    }
    return -1;
}

#ifdef CL_REACTOR_TLS
static void reactor_tls_drop(int slot, int notify);
#endif

static void reactor_free_slot(int slot)
{
#ifdef CL_REACTOR_TLS
    reactor_tls_drop(slot, 0);
#endif
    if (socket_buf[slot]) { iobuf_free(socket_buf[slot]); socket_buf[slot] = NULL; }
    socket_table[slot] = -1;
    pend_wpos[slot] = 0;
    pend_read_has_deadline[slot] = 0;
    pend_write_has_deadline[slot] = 0;
    pend_read_wants_write[slot] = 0;
    pend_write_wants_read[slot] = 0;
    socket_rtimeout[slot] = 0;
    socket_wtimeout[slot] = 0;
}

static void reactor_reply(SockReq *req)
{
    if (sock_diag_on()) {
        char detail[96];
        sprintf(detail, "%s result=%d out=%d", sock_op_name(req->op),
                (int)req->result, (int)req->out_slot);
        sock_diag_line(req, "tx", detail);
    }
    ReplyMsg(&req->msg);
}

#ifdef CL_REACTOR_TLS
/* ===== TLS via AmiSSL (reactor task only) =====
 *
 * AmiSSL v5 exposes the OpenSSL 3.x API, so this mirrors the host backend
 * (tls_openssl.c) with the poll loop replaced by the reactor's park/resume
 * machinery: an SSL_* call answering WANT_READ/WANT_WRITE parks the request
 * on the slot with the fd interest recorded in pend_*_wants_* and is retried
 * when WaitSelect signals readiness — so a slow TLS handshake never stalls
 * the other sockets. */

static int amissl_state = 0;   /* 0 = untried, 1 = ready, -1 = failed */

/* Bounded copy of a diagnostic into the request's error buffer. */
static void reactor_tls_err(SockReq *req, const char *msg)
{
    uint32_t n;
    if (!req->buf || req->len == 0) return;
    n = (uint32_t)strlen(msg);
    if (n >= req->len) n = req->len - 1;
    memcpy(req->buf, msg, n);
    req->buf[n] = '\0';
}

/* Compose "<what>: <openssl detail> (certificate verification: ...)". */
static void reactor_tls_err_ssl(SockReq *req, void *ssl, const char *what)
{
    char tmp[320];
    char detail[160];
    unsigned long e;
    long vr = X509_V_OK;
    detail[0] = '\0';
    e = ERR_get_error();
    if (e) ERR_error_string_n(e, detail, sizeof(detail));
    else strcpy(detail, "TLS protocol error");
    if (ssl) vr = SSL_get_verify_result((SSL *)ssl);
    if (vr != X509_V_OK)
        sprintf(tmp, "%s: %.140s (certificate verification: %.80s)",
                what, detail, X509_verify_cert_error_string(vr));
    else
        sprintf(tmp, "%s: %.140s", what, detail);
    reactor_tls_err(req, tmp);
}

/* Lazy provider bring-up, once per reactor lifetime.  All of AmiSSL is
 * owned by this task: InitAmiSSL is per-task and binds our SocketBase so
 * SSL_set_fd I/O uses the reactor's descriptor table. */
static int reactor_tls_provider_init(SockReq *req)
{
    if (amissl_state == 1) return 0;
    if (amissl_state == -1) {
        reactor_tls_err(req, "AmiSSL initialisation failed earlier "
                        "(see the first TLS error this session)");
        return -1;
    }
    amissl_state = -1;
    AmiSSLMasterBase = OpenLibrary("amisslmaster.library",
                                   AMISSLMASTER_MIN_VERSION);
    if (!AmiSSLMasterBase) {
        reactor_tls_err(req, "amisslmaster.library v5 not found - "
                        "install AmiSSL 5 (aminet.net/util/libs)");
        return -1;
    }
    if (!InitAmiSSLMaster(AMISSL_CURRENT_VERSION, TRUE)) {
        reactor_tls_err(req, "installed AmiSSL is too old for this build - "
                        "update to AmiSSL 5.27 or newer");
        return -1;
    }
    AmiSSLBase = OpenAmiSSL();
    if (!AmiSSLBase) {
        reactor_tls_err(req, "OpenAmiSSL() failed - AmiSSL installation "
                        "is incomplete");
        return -1;
    }
    if (InitAmiSSL(AmiSSL_ErrNoPtr, (ULONG)&socket_errno,
                   AmiSSL_SocketBase, (ULONG)SocketBase,
                   TAG_DONE) != 0) {
        reactor_tls_err(req, "InitAmiSSL() failed");
        return -1;
    }
    amissl_state = 1;
    return 0;
}

/* Reactor-exit teardown (after reactor_loop; sockets are already closed). */
static void reactor_tls_provider_cleanup(void)
{
    if (amissl_state == 1)
        CleanupAmiSSLA(NULL);
    if (AmiSSLBase) { CloseAmiSSL(); AmiSSLBase = NULL; }
    if (AmiSSLMasterBase) { CloseLibrary(AmiSSLMasterBase); AmiSSLMasterBase = NULL; }
    amissl_state = 0;
}

/* Free a slot's TLS state.  notify sends one non-blocking close_notify
 * (the fd is non-blocking, so SSL_shutdown can never park). */
static void reactor_tls_drop(int slot, int notify)
{
    if (socket_ssl[slot]) {
        if (notify) SSL_shutdown((SSL *)socket_ssl[slot]);
        SSL_free((SSL *)socket_ssl[slot]);
        socket_ssl[slot] = NULL;
    }
    if (socket_ssl_ctx[slot]) {
        SSL_CTX_free((SSL_CTX *)socket_ssl_ctx[slot]);
        socket_ssl_ctx[slot] = NULL;
    }
}

/* Passphrase callback for encrypted PEM keys (userdata = passphrase). */
static int reactor_tls_passwd_cb(char *buf, int size, int rwflag, void *userdata)
{
    const char *pw = (const char *)userdata;
    int len;
    (void)rwflag;
    if (!pw) return 0;
    len = (int)strlen(pw);
    if (len > size) len = size;
    memcpy(buf, pw, (size_t)len);
    return len;
}

/* Park a TLS op on its slot with the fd interest the record layer asked
 * for.  is_write selects the pend slot (and deadline bank), matching how
 * the op re-enters via reactor_resume_read/_write. */
static void reactor_tls_park(SockReq *req, int slot, int sslerr, int is_write)
{
    if (is_write) {
        pend_write[slot] = req;
        pend_write_wants_read[slot] = (sslerr == SSL_ERROR_WANT_READ);
        reactor_arm_deadline(slot, req->timeout_ms, 1);
    } else {
        pend_read[slot] = req;
        pend_read_wants_write[slot] = (sslerr == SSL_ERROR_WANT_WRITE);
        reactor_arm_deadline(slot, req->timeout_ms, 0);
    }
    if (sock_diag_on()) {
        char d[48];
        sprintf(d, "slot=%d want=%s", slot,
                sslerr == SSL_ERROR_WANT_WRITE ? "w" : "r");
        sock_diag_line(req, is_write ? "park-w" : "park-r", d);
    }
}

/* REQ_TLS_START — build the context on first entry, then drive the
 * handshake; parks (in pend_read) and is re-entered on fd readiness. */
static void reactor_try_tls_start(SockReq *req)
{
    int slot = (int)req->slot;
    LONG fd = socket_table[slot];
    const PlatformTLSParams *p = req->tls_params;
    SSL *ssl;
    int n, sslerr;

    if (fd < 0) {
        reactor_tls_err(req, "socket is closed");
        req->result = -1; reactor_reply(req); return;
    }

    if (!socket_ssl[slot]) {
        SSL_CTX *ctx;
        if (reactor_tls_provider_init(req) != 0) {
            req->result = -1; reactor_reply(req); return;
        }
        ctx = SSL_CTX_new(p->server ? TLS_server_method() : TLS_client_method());
        if (!ctx) {
            reactor_tls_err_ssl(req, NULL, "TLS context creation");
            req->result = -1; reactor_reply(req); return;
        }
        /* Trust anchors: explicit locations win; else AmiSSL's default
         * store (AmiSSL:Certs) when verifying. */
        if (p->ca_file || p->ca_path) {
            if (SSL_CTX_load_verify_locations(ctx, p->ca_file, p->ca_path) != 1) {
                reactor_tls_err_ssl(req, NULL, "loading CA locations");
                SSL_CTX_free(ctx);
                req->result = -1; reactor_reply(req); return;
            }
        } else if (!p->server && p->verify) {
            SSL_CTX_set_default_verify_paths(ctx);
        }
        /* Own certificate/key (server: required). */
        if (p->cert_file) {
            const char *key = p->key_file ? p->key_file : p->cert_file;
            if (p->key_password) {
#if PLATFORM_MORPHOS
                /* The passphrase callback is a C function pointer the 68k
                 * AmiSSL library calls back into — from native PPC code
                 * that needs an EmulLibEntry gate with manual argument
                 * extraction, which is not implemented yet.  Encrypted
                 * keys fail cleanly instead of jumping into PPC code. */
                reactor_tls_err(req, "encrypted private keys are not "
                                "supported on MorphOS yet - decrypt the "
                                "key file (openssl rsa -in enc.pem -out "
                                "plain.pem)");
                SSL_CTX_free(ctx);
                req->result = -1; reactor_reply(req); return;
#else
                SSL_CTX_set_default_passwd_cb(ctx, reactor_tls_passwd_cb);
                SSL_CTX_set_default_passwd_cb_userdata(ctx, (void *)p->key_password);
#endif
            }
            if (SSL_CTX_use_certificate_chain_file(ctx, p->cert_file) != 1 ||
                SSL_CTX_use_PrivateKey_file(ctx, key, SSL_FILETYPE_PEM) != 1 ||
                SSL_CTX_check_private_key(ctx) != 1) {
                reactor_tls_err_ssl(req, NULL, "loading certificate/key");
                SSL_CTX_free(ctx);
                req->result = -1; reactor_reply(req); return;
            }
            SSL_CTX_set_default_passwd_cb(ctx, NULL);
            SSL_CTX_set_default_passwd_cb_userdata(ctx, NULL);
        } else if (p->server) {
            reactor_tls_err(req, "server-side TLS requires a certificate file");
            SSL_CTX_free(ctx);
            req->result = -1; reactor_reply(req); return;
        }
        ssl = SSL_new(ctx);
        if (!ssl || SSL_set_fd(ssl, fd) != 1) {
            reactor_tls_err_ssl(req, NULL, "TLS connection creation");
            if (ssl) SSL_free(ssl);
            SSL_CTX_free(ctx);
            req->result = -1; reactor_reply(req); return;
        }
        if (!p->server && p->verify)
            SSL_set_verify(ssl, SSL_VERIFY_PEER, NULL);
        if (!p->server && p->hostname && p->hostname[0]) {
            SSL_set_tlsext_host_name(ssl, (char *)p->hostname);   /* SNI */
            if (p->verify && SSL_set1_host(ssl, p->hostname) != 1) {
                reactor_tls_err_ssl(req, NULL, "setting verification hostname");
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                req->result = -1; reactor_reply(req); return;
            }
        }
        socket_ssl_ctx[slot] = ctx;
        socket_ssl[slot] = ssl;
    }

    ssl = (SSL *)socket_ssl[slot];
    ERR_clear_error();
    n = p->server ? SSL_accept(ssl) : SSL_connect(ssl);
    if (n > 0) {
        req->result = 0; reactor_reply(req); return;
    }
    sslerr = SSL_get_error(ssl, n);
    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) {
        reactor_tls_park(req, slot, sslerr, 0);
        return;
    }
    reactor_tls_err_ssl(req, ssl,
                        p->server ? "TLS handshake (accept)"
                                  : "TLS handshake (connect)");
    /* A failed handshake leaves the connection unusable; drop the TLS
     * state so the close path doesn't try to close_notify over garbage. */
    reactor_tls_drop(slot, 0);
    req->result = -1; reactor_reply(req);
}

/* TLS leg of reactor_try_read: decrypt into the caller's buffer. */
static void reactor_try_tls_read(SockReq *req)
{
    int slot = (int)req->slot;
    SSL *ssl = (SSL *)socket_ssl[slot];
    int n, sslerr;
    ERR_clear_error();
    n = SSL_read(ssl, req->buf, (int)req->len);
    if (n > 0)  { req->result = n; reactor_reply(req); return; }
    sslerr = SSL_get_error(ssl, n);
    if (sslerr == SSL_ERROR_ZERO_RETURN) {         /* clean close_notify */
        req->result = 0; reactor_reply(req); return;
    }
    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) {
        reactor_tls_park(req, slot, sslerr, 0);
        return;
    }
    req->result = -1; reactor_reply(req);
}

/* TLS leg of reactor_try_write.  Without partial-write mode SSL_write
 * answers all-or-WANT, but a short success is still handled by advancing
 * pend_wpos and re-entering (a fresh SSL_write call, which is legal). */
static void reactor_try_tls_write(SockReq *req)
{
    int slot = (int)req->slot;
    SSL *ssl = (SSL *)socket_ssl[slot];
    uint32_t off = pend_wpos[slot];
    int n, sslerr;
    ERR_clear_error();
    n = SSL_write(ssl, req->buf + off, (int)(req->len - off));
    if (n > 0) {
        off += (uint32_t)n;
        pend_wpos[slot] = off;
        if (off >= req->len) {
            pend_wpos[slot] = 0;
            req->result = 0; reactor_reply(req);
        } else {
            reactor_tls_park(req, slot, SSL_ERROR_WANT_WRITE, 1);
        }
        return;
    }
    sslerr = SSL_get_error(ssl, n);
    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE) {
        reactor_tls_park(req, slot, sslerr, 1);
        return;
    }
    pend_wpos[slot] = 0;
    req->result = -1; reactor_reply(req);
}

/* TLS leg of reactor_try_poll: buffered plaintext counts as ready; a
 * readable fd is only a hint (could be a partial record), so SSL_peek
 * gives the real LISTEN answer.  result: 1 data, 0 not yet, 2 EOF. */
static void reactor_try_tls_poll(SockReq *req)
{
    int slot = (int)req->slot;
    LONG fd = socket_table[slot];
    SSL *ssl = (SSL *)socket_ssl[slot];
    CL_fdset rset;
    struct timeval tv;
    LONG r;
    char peek;
    int n, sslerr;
    if (SSL_pending(ssl) > 0) { req->result = 1; reactor_reply(req); return; }
    CL_FD_ZERO(&rset);
    CL_FD_SET(fd, &rset);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    r = WaitSelect(fd + 1, &rset, NULL, NULL, &tv, NULL);
    if (r <= 0 || !CL_FD_ISSET(fd, &rset)) { req->result = 0; reactor_reply(req); return; }
    ERR_clear_error();
    n = SSL_peek(ssl, &peek, 1);
    if (n > 0) { req->result = 1; reactor_reply(req); return; }
    sslerr = SSL_get_error(ssl, n);
    if (sslerr == SSL_ERROR_WANT_READ || sslerr == SSL_ERROR_WANT_WRITE)
        req->result = 0;               /* nothing decodable yet */
    else
        req->result = 2;               /* close_notify / hard EOF / error */
    reactor_reply(req);
}

/* REQ_TLS_PEERCERT — copy one certificate field (selector in req->port,
 * PLATFORM_TLS_CERT_*) into the caller's buffer. */
static void reactor_tls_peercert(SockReq *req)
{
    int slot = (int)req->slot;
    SSL *ssl = (SSL *)socket_ssl[slot];
    X509 *x;
    int ok = -1;
    if (!ssl || !req->buf || req->len == 0) {
        req->result = -1; reactor_reply(req); return;
    }
    req->buf[0] = '\0';
    x = SSL_get1_peer_certificate(ssl);
    if (x) {
        switch (req->port) {
        case PLATFORM_TLS_CERT_SUBJECT:
        case PLATFORM_TLS_CERT_ISSUER: {
            X509_NAME *name = (req->port == PLATFORM_TLS_CERT_SUBJECT)
                              ? X509_get_subject_name(x)
                              : X509_get_issuer_name(x);
            if (name && X509_NAME_oneline(name, req->buf, (int)req->len))
                ok = 0;
            break;
        }
        case PLATFORM_TLS_CERT_NOT_BEFORE:
        case PLATFORM_TLS_CERT_NOT_AFTER: {
            const ASN1_TIME *t = (req->port == PLATFORM_TLS_CERT_NOT_BEFORE)
                                 ? X509_get0_notBefore(x)
                                 : X509_get0_notAfter(x);
            struct tm tm;
            memset(&tm, 0, sizeof(tm));
            if (t && req->len >= 24 && ASN1_TIME_to_tm(t, &tm) == 1) {
                sprintf(req->buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                        tm.tm_hour, tm.tm_min, tm.tm_sec);
                ok = 0;
            }
            break;
        }
        default:
            break;
        }
        X509_free(x);
    }
    req->result = ok;
    reactor_reply(req);
}
#endif /* CL_REACTOR_TLS */

/* recv into the caller's read buffer; complete or park.  result>0 = bytes,
 * 0 = EOF, -1 = error. */
static void reactor_try_read(SockReq *req)
{
    int slot = (int)req->slot;
    LONG fd = socket_table[slot];
    LONG n;
    if (fd < 0) { req->result = -1; reactor_reply(req); return; }
#ifdef CL_REACTOR_TLS
    if (socket_ssl[slot]) { reactor_try_tls_read(req); return; }
#endif
    n = recv(fd, req->buf, (LONG)req->len, 0);
    if (n > 0)                       { req->result = (int)n; reactor_reply(req); }
    else if (n == 0)                 { req->result = 0;      reactor_reply(req); } /* EOF */
    else if (Errno() == EWOULDBLOCK) { pend_read[slot] = req;                      /* park */
                                       reactor_arm_deadline(slot, req->timeout_ms, 0);
                                       if (sock_diag_on()) {
                                           char d[32];
                                           sprintf(d, "slot=%d", slot);
                                           sock_diag_line(req, "park-r", d);
                                       } }
    else                             { req->result = -1;     reactor_reply(req); }
}

/* Non-blocking readiness probe (REQ_POLL), backing CL:LISTEN on socket streams.
 * Buffered data is checked caller-side; here we probe only the fd with a
 * zero-timeout WaitSelect.  result: 1 = readable now (data, or for a listener a
 * pending connection), 0 = would block, 2 = EOF (peer closed), -1 = invalid.
 * A readable CONNECTION socket is MSG_PEEK'd to tell data from EOF; a listener
 * has no IOBuf and is never peeked (recv on a listen fd is invalid). */
static void reactor_try_poll(SockReq *req)
{
    int slot = (int)req->slot;
    LONG fd = socket_table[slot];
    CL_fdset rset;
    struct timeval tv;
    LONG r;
    if (fd < 0) { req->result = -1; reactor_reply(req); return; }
#ifdef CL_REACTOR_TLS
    if (socket_ssl[slot]) { reactor_try_tls_poll(req); return; }
#endif
    CL_FD_ZERO(&rset);
    CL_FD_SET(fd, &rset);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    r = WaitSelect(fd + 1, &rset, NULL, NULL, &tv, NULL);
    if (r <= 0 || !CL_FD_ISSET(fd, &rset)) { req->result = 0; reactor_reply(req); return; }
    if (!socket_buf[slot]) { req->result = 1; reactor_reply(req); return; }  /* listener: pending conn */
    {
        char peek;
        LONG pn = recv(fd, &peek, 1, MSG_PEEK);
        if (pn > 0)                      req->result = 1;   /* real data */
        else if (pn == 0)                req->result = 2;   /* EOF */
        else if (Errno() == EWOULDBLOCK) req->result = 0;   /* spurious */
        else                             req->result = 2;
    }
    reactor_reply(req);
}

/* send from the caller's write buffer (continuing at pend_wpos); complete or park. */
static void reactor_try_write(SockReq *req)
{
    int slot = (int)req->slot;
    LONG fd = socket_table[slot];
    LONG n;
    uint32_t off;
    if (fd < 0) { req->result = -1; pend_wpos[slot] = 0; reactor_reply(req); return; }
#ifdef CL_REACTOR_TLS
    if (socket_ssl[slot]) { reactor_try_tls_write(req); return; }
#endif
    off = pend_wpos[slot];
    n = send(fd, req->buf + off, (LONG)(req->len - off), 0);
    if (n >= 0) {
        off += (uint32_t)n;
        pend_wpos[slot] = off;
        if (off >= req->len) { pend_wpos[slot] = 0; req->result = 0; reactor_reply(req); }
        else                 { pend_write[slot] = req;     /* more to send — park */
                               reactor_arm_deadline(slot, req->timeout_ms, 1);
                               if (sock_diag_on()) {
                                   char d[48];
                                   sprintf(d, "slot=%d sent=%lu/%lu", slot,
                                           (unsigned long)off, (unsigned long)req->len);
                                   sock_diag_line(req, "park-w", d);
                               } }
    } else if (Errno() == EWOULDBLOCK) {
        pend_write[slot] = req;                            /* park */
        reactor_arm_deadline(slot, req->timeout_ms, 1);
        if (sock_diag_on()) {
            char d[32];
            sprintf(d, "slot=%d", slot);
            sock_diag_line(req, "park-w", d);
        }
    } else {
        pend_wpos[slot] = 0; req->result = -1; reactor_reply(req);
    }
}

static void reactor_try_accept(SockReq *req)
{
    int slot = (int)req->slot;            /* listener slot */
    LONG lfd = socket_table[slot];
    struct sockaddr_in caddr;
    cl_socklen_t clen = sizeof(caddr);
    LONG fd;
    if (lfd < 0) { req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return; }
    fd = accept(lfd, (struct sockaddr *)&caddr, &clen);
    if (fd >= 0) {
        int ns;
        reactor_set_nonblock(fd);
        ns = reactor_alloc_slot(fd, 1);
        if (ns < 0) { CloseSocket(fd); req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; }
        else        { req->result = 0;  req->out_slot = (PlatformSocket)ns; }
        reactor_reply(req);
    } else if (Errno() == EWOULDBLOCK) {
        pend_read[slot] = req;                              /* park on listener readable */
        if (sock_diag_on()) {
            char d[32];
            sprintf(d, "slot=%d", slot);
            sock_diag_line(req, "park-r", d);
        }
    } else {
        req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req);
    }
}

static void reactor_finish_connect(SockReq *req)
{
    int slot = (int)req->slot;
    LONG err = 0;
    cl_socklen_t elen = sizeof(err);
    getsockopt(socket_table[slot], SOL_SOCKET, SO_ERROR, (char *)&err, &elen);
    if (err == 0) {
        req->result = 0; req->out_slot = (PlatformSocket)slot;
    } else {
        CloseSocket(socket_table[slot]);
        reactor_free_slot(slot);
        req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID;
    }
    reactor_reply(req);
}

static void reactor_start_connect(SockReq *req)
{
    struct hostent *he;
    struct sockaddr_in addr;
    LONG fd, rc;
    int slot;

    /* DNS: dotted-quad (e.g. loopback) resolves locally and does not block;
     * a real hostname lookup can briefly stall the reactor — acceptable. */
    if (sock_diag_on()) {
        char d[80];
        sprintf(d, "\"%.64s\"", req->host);
        sock_diag_line(req, "dns", d);
    }
    he = gethostbyname((STRPTR)req->host);
    if (sock_diag_on()) sock_diag_line(req, he ? "dns ok" : "dns FAIL", NULL);
    if (!he) { req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return; }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return; }
    reactor_set_nonblock(fd);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)req->port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc == 0) {                                          /* connected immediately */
        slot = reactor_alloc_slot(fd, 1);
        if (slot < 0) { CloseSocket(fd); req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; }
        else          { req->result = 0;  req->out_slot = (PlatformSocket)slot; }
        reactor_reply(req);
    } else if (Errno() == EINPROGRESS || Errno() == EWOULDBLOCK) {
        slot = reactor_alloc_slot(fd, 1);                   /* reserve slot, park */
        if (slot < 0) { CloseSocket(fd); req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return; }
        req->slot = (PlatformSocket)slot;
        pend_write[slot] = req;
        /* Bound the handshake when a connect timeout was requested; the reactor
         * reaps it in reactor_expire_deadlines if the peer never replies. */
        reactor_arm_deadline(slot, req->timeout_ms, 1);
        if (sock_diag_on()) {
            char d[32];
            sprintf(d, "slot=%d", slot);
            sock_diag_line(req, "park-w", d);
        }
    } else {
        CloseSocket(fd); req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req);
    }
}

static void reactor_do_listen(SockReq *req)
{
    struct sockaddr_in addr;
    LONG fd, on = 1;
    int slot;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));
    reactor_set_nonblock(fd);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)req->port);
    addr.sin_addr.s_addr = htonl(req->loopback ? INADDR_LOOPBACK
                                 : req->bind_addr ? req->bind_addr : INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        CloseSocket(fd); req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return;
    }
    {
        cl_socklen_t alen = sizeof(addr);
        if (getsockname(fd, (struct sockaddr *)&addr, &alen) == 0)
            req->out_port = ntohs(addr.sin_port);
        else
            req->out_port = req->port;
    }
    slot = reactor_alloc_slot(fd, 0);                       /* listener: no IOBuf */
    if (slot < 0) { CloseSocket(fd); req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; }
    else          { req->result = 0;  req->out_slot = (PlatformSocket)slot; }
    reactor_reply(req);
}

/* Connected UDP socket: socket(SOCK_DGRAM) + connect() — the connect only
 * records the peer (no handshake), so it completes immediately.  No IOBuf:
 * datagram I/O goes straight between the caller's buffer and the fd. */
static void reactor_udp_connect(SockReq *req)
{
    struct hostent *he;
    struct sockaddr_in addr;
    LONG fd;
    int slot;

    if (sock_diag_on()) {
        char d[80];
        sprintf(d, "\"%.64s\"", req->host);
        sock_diag_line(req, "dns", d);
    }
    he = gethostbyname((STRPTR)req->host);
    if (sock_diag_on()) sock_diag_line(req, he ? "dns ok" : "dns FAIL", NULL);
    if (!he) { req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return; }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return; }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)req->port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        CloseSocket(fd); req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; reactor_reply(req); return;
    }
    reactor_set_nonblock(fd);
    slot = reactor_alloc_slot(fd, 0);   /* no IOBuf: message I/O only */
    if (slot < 0) { CloseSocket(fd); req->result = -1; req->out_slot = PLATFORM_SOCKET_INVALID; }
    else          { req->result = 0;  req->out_slot = (PlatformSocket)slot; }
    reactor_reply(req);
}

/* getsockname: dotted-quad local address into req->buf (>= 16 bytes, a C
 * stack buffer of the calling thread — shared address space), port into
 * out_port. */
static void reactor_do_endpoint(SockReq *req)
{
    int slot = (int)req->slot;
    LONG fd = socket_table[slot];
    struct sockaddr_in addr;
    cl_socklen_t alen = sizeof(addr);
    if (fd < 0) { req->result = -1; reactor_reply(req); return; }
    memset(&addr, 0, sizeof(addr));
    if (getsockname(fd, (struct sockaddr *)&addr, &alen) < 0) {
        req->result = -1; reactor_reply(req); return;
    }
    {
        const unsigned char *b = (const unsigned char *)&addr.sin_addr;
        sprintf(req->buf, "%u.%u.%u.%u",
                (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3]);
        req->out_port = ntohs(addr.sin_port);
    }
    req->result = 0;
    reactor_reply(req);
}

static void reactor_do_close(SockReq *req)
{
    int slot = (int)req->slot;
    /* Cancel any op parked on this slot — the waiting thread gets -1 and must
     * not touch the IOBuf afterwards (it is about to be freed). */
    if (pend_read[slot])  { pend_read[slot]->result  = -1; reactor_reply(pend_read[slot]);  pend_read[slot]  = NULL; }
    if (pend_write[slot]) { pend_write[slot]->result = -1; reactor_reply(pend_write[slot]); pend_write[slot] = NULL; }
    if (socket_table[slot] >= 0) {
#ifdef CL_REACTOR_TLS
        /* close_notify before CloseSocket — the shutdown record still
         * needs the fd; non-blocking, best-effort. */
        reactor_tls_drop(slot, 1);
#endif
        CloseSocket(socket_table[slot]);
        reactor_free_slot(slot);
    }
    req->result = 0;
    reactor_reply(req);
}

static void reactor_close_all(void)
{
    int i;
    for (i = 1; i < PLATFORM_SOCKET_TABLE_SIZE; i++) {
        if (pend_read[i])  { pend_read[i]->result  = -1; reactor_reply(pend_read[i]);  pend_read[i]  = NULL; }
        if (pend_write[i]) { pend_write[i]->result = -1; reactor_reply(pend_write[i]); pend_write[i] = NULL; }
        if (socket_table[i] >= 0) { CloseSocket(socket_table[i]); reactor_free_slot(i); }
    }
}

static void reactor_handle(SockReq *req)
{
    if (sock_diag_on()) {
        char d[64];
        sprintf(d, "%s slot=%d", sock_op_name(req->op), (int)req->slot);
        sock_diag_line(req, "rx", d);
    }
    switch (req->op) {
    case REQ_CONNECT:  reactor_start_connect(req); break;
    case REQ_LISTEN:   reactor_do_listen(req);     break;
    case REQ_ACCEPT:   reactor_try_accept(req);    break;
    case REQ_READFILL: reactor_try_read(req);      break;
    case REQ_WRITE:    reactor_try_write(req);     break;
    case REQ_POLL:     reactor_try_poll(req);      break;
    case REQ_UDP_CONNECT: reactor_udp_connect(req); break;
    case REQ_ENDPOINT: reactor_do_endpoint(req);   break;
    case REQ_CLOSE:    reactor_do_close(req);      break;
#ifdef CL_REACTOR_TLS
    case REQ_TLS_START:    reactor_try_tls_start(req); break;
    case REQ_TLS_PEERCERT: reactor_tls_peercert(req);  break;
#endif
    default:           req->result = -1; reactor_reply(req); break;
    }
}

static void reactor_resume_read(int slot)
{
    SockReq *req = pend_read[slot];
    pend_read[slot] = NULL;
    pend_read_has_deadline[slot] = 0;
    pend_read_wants_write[slot] = 0;
    if (sock_diag_on()) {
        char d[32];
        sprintf(d, "slot=%d", slot);
        sock_diag_line(req, "resume-r", d);
    }
    if (req->op == REQ_ACCEPT) { reactor_try_accept(req); return; }
#ifdef CL_REACTOR_TLS
    if (req->op == REQ_TLS_START) { reactor_try_tls_start(req); return; }
#endif
    reactor_try_read(req);
}

static void reactor_resume_write(int slot)
{
    SockReq *req = pend_write[slot];
    pend_write[slot] = NULL;
    pend_write_has_deadline[slot] = 0;
    pend_write_wants_read[slot] = 0;
    if (sock_diag_on()) {
        char d[32];
        sprintf(d, "slot=%d", slot);
        sock_diag_line(req, "resume-w", d);
    }
    if (req->op == REQ_CONNECT) reactor_finish_connect(req);
    else                        reactor_try_write(req);
}

/* Time out any parked op whose deadline has elapsed: hand the waiting thread
 * PLATFORM_SOCKET_TIMEOUT and clear the park slot. */
static void reactor_expire_deadlines(uint32_t now)
{
    int i;
    for (i = 1; i < PLATFORM_SOCKET_TABLE_SIZE; i++) {
        if (pend_read[i] && pend_read_has_deadline[i] &&
            (int32_t)(pend_read_deadline[i] - now) <= 0) {
            SockReq *req = pend_read[i];
            pend_read[i] = NULL; pend_read_has_deadline[i] = 0;
            pend_read_wants_write[i] = 0;
            if (sock_diag_on()) {
                char d[32];
                sprintf(d, "slot=%d", i);
                sock_diag_line(req, "expire-r", d);
            }
            if (req->op == REQ_TLS_START) {
                /* Handshake never completed: drop the half-established SSL
                 * state so the slot doesn't look TLS-upgraded afterward
                 * (platform_tls_active) and a retried EXT:SOCKET-START-TLS
                 * can proceed on the still-plain socket, matching the
                 * REQ_CONNECT cleanup in the pend_write branch below. */
                reactor_tls_drop(i, 0);
            }
            req->result = PLATFORM_SOCKET_TIMEOUT;
            req->out_slot = PLATFORM_SOCKET_INVALID;   /* in case it was an accept */
            reactor_reply(req);
        }
        if (pend_write[i] && pend_write_has_deadline[i] &&
            (int32_t)(pend_write_deadline[i] - now) <= 0) {
            SockReq *req = pend_write[i];
            pend_write[i] = NULL; pend_write_has_deadline[i] = 0;
            pend_write_wants_read[i] = 0;
            pend_wpos[i] = 0;
            if (sock_diag_on()) {
                char d[32];
                sprintf(d, "slot=%d", i);
                sock_diag_line(req, "expire-w", d);
            }
            if (req->op == REQ_CONNECT) {
                /* Handshake never completed: tear down the reserved socket and
                 * report failure (out_slot INVALID), matching the error path of
                 * reactor_finish_connect. */
                if (socket_table[i] >= 0) { CloseSocket(socket_table[i]); reactor_free_slot(i); }
                req->result = -1;
                req->out_slot = PLATFORM_SOCKET_INVALID;
            } else {
                req->result = PLATFORM_SOCKET_TIMEOUT;
            }
            reactor_reply(req);
        }
    }
}

static void reactor_loop(void)
{
    ULONG portsig = 1UL << reactor_port->mp_SigBit;
    int running = 1;

    while (running) {
        CL_fdset rset, wset;
        int maxfd = -1, i;
        ULONG sigs = portsig;
        int have_deadline = 0;
        uint32_t earliest = 0;

        CL_FD_ZERO(&rset);
        CL_FD_ZERO(&wset);
        for (i = 1; i < PLATFORM_SOCKET_TABLE_SIZE; i++) {
            LONG fd = socket_table[i];
            if (fd < 0) continue;
            /* A parked op normally waits for its own direction; TLS can
             * invert that (pend_*_wants_* — see the flag declarations). */
            if (pend_read[i]) {
                CL_FD_SET(fd, pend_read_wants_write[i] ? &wset : &rset);
                if (fd > maxfd) maxfd = fd;
            }
            if (pend_write[i]) {
                CL_FD_SET(fd, pend_write_wants_read[i] ? &rset : &wset);
                if (fd > maxfd) maxfd = fd;
            }
            /* Track the soonest deadline so WaitSelect wakes to expire it. */
            if (pend_read[i] && pend_read_has_deadline[i]) {
                if (!have_deadline || (int32_t)(pend_read_deadline[i] - earliest) < 0)
                    { earliest = pend_read_deadline[i]; have_deadline = 1; }
            }
            if (pend_write[i] && pend_write_has_deadline[i]) {
                if (!have_deadline || (int32_t)(pend_write_deadline[i] - earliest) < 0)
                    { earliest = pend_write_deadline[i]; have_deadline = 1; }
            }
        }

        if (maxfd < 0) {
            /* No parked socket ops — just wait for the next request. */
            Wait(portsig);
        } else if (have_deadline) {
            /* Bound the wait by the soonest deadline; clamp to >= 0. */
            uint32_t now = platform_time_ms();
            int32_t rem = (int32_t)(earliest - now);
            struct timeval tv;
            if (rem < 0) rem = 0;
            tv.tv_sec  = rem / 1000;
            tv.tv_usec = (rem % 1000) * 1000;
            WaitSelect(maxfd + 1, &rset, &wset, NULL, &tv, &sigs);
            for (i = 1; i < PLATFORM_SOCKET_TABLE_SIZE; i++) {
                LONG fd = socket_table[i];
                if (fd < 0) continue;
                if (pend_read[i] &&
                    CL_FD_ISSET(fd, pend_read_wants_write[i] ? &wset : &rset))
                    reactor_resume_read(i);
                if (pend_write[i] &&
                    CL_FD_ISSET(fd, pend_write_wants_read[i] ? &rset : &wset))
                    reactor_resume_write(i);
            }
            reactor_expire_deadlines(platform_time_ms());
        } else {
            WaitSelect(maxfd + 1, &rset, &wset, NULL, NULL, &sigs);
            for (i = 1; i < PLATFORM_SOCKET_TABLE_SIZE; i++) {
                LONG fd = socket_table[i];
                if (fd < 0) continue;
                if (pend_read[i] &&
                    CL_FD_ISSET(fd, pend_read_wants_write[i] ? &wset : &rset))
                    reactor_resume_read(i);
                if (pend_write[i] &&
                    CL_FD_ISSET(fd, pend_write_wants_read[i] ? &rset : &wset))
                    reactor_resume_write(i);
            }
        }

        /* Drain new requests (the port signal may or may not be set in sigs
         * after WaitSelect; always poll the port to be safe). */
        {
            struct Message *m;
            while ((m = GetMsg(reactor_port)) != NULL) {
                SockReq *req = (SockReq *)m;
                if (req->op == REQ_SHUTDOWN) {
                    reactor_close_all();
                    running = 0;
                    reactor_reply(req);
                } else {
                    reactor_handle(req);
                }
            }
        }
    }
}

/* Reactor process entry: open SocketBase + request port (owned here), signal
 * the booting thread, then run the loop until REQ_SHUTDOWN. */
static void reactor_entry(void)
{
    /* Capture the boot task/signal into locals as the very first thing this
     * process does — reactor_boot_task/reactor_boot_sig are globals that the
     * booting caller may reset (and FreeSignal) once its bounded wait times
     * out (see reactor_wait_boot / reactor_ensure).  Reading the globals
     * again at Signal() time below would race that reset: the shift could
     * see reactor_boot_sig == -1 (UB), or a bit number already reallocated
     * to an unrelated AllocSignal() caller by then. */
    struct Task    *boot_task = reactor_boot_task;
    BYTE            boot_sig  = reactor_boot_sig;
    struct MsgPort *port = CreateMsgPort();
    int ok = 0;
    int i;

    for (i = 0; i < PLATFORM_SOCKET_TABLE_SIZE; i++) {
        socket_table[i] = -1; socket_buf[i] = NULL;
        pend_read[i] = NULL; pend_write[i] = NULL; pend_wpos[i] = 0;
        pend_read_has_deadline[i] = 0; pend_write_has_deadline[i] = 0;
        pend_read_wants_write[i] = 0; pend_write_wants_read[i] = 0;
        socket_rtimeout[i] = 0; socket_wtimeout[i] = 0;
#ifdef CL_REACTOR_TLS
        socket_ssl[i] = NULL; socket_ssl_ctx[i] = NULL;
#endif
    }

    if (port) {
        SocketBase = OpenLibrary("bsdsocket.library", 3);
        if (SocketBase) {
            SetErrnoPtr(&socket_errno, sizeof(socket_errno));
            reactor_port = port;   /* publish to clients */
            ok = 1;
        } else {
            DeleteMsgPort(port);
        }
    }

    Signal(boot_task, 1UL << boot_sig);  /* boot handshake done */
    if (!ok) return;

    reactor_loop();

#ifdef CL_REACTOR_TLS
    reactor_tls_provider_cleanup();   /* before SocketBase goes away */
#endif
    CloseLibrary(SocketBase);
    SocketBase = NULL;
    DeleteMsgPort(reactor_port);
    reactor_port = NULL;
}

/* NP_Entry is entered as m68k code; on MorphOS the PPC entry needs a trap
 * gate or the new process dies in the emulator (see platform_amiga_ppc.h). */
CL_PROC_ENTRY_GATE(reactor_entry_gate, reactor_entry);

/* Wait for the reactor's boot handshake, but BOUNDED.  A plain Wait() here
 * deadlocks the whole runtime if the child process dies before it can signal
 * — which is exactly what happens when the entry point is entered with the
 * wrong code type (see platform_amiga_ppc.h) or when the process cannot get
 * its stack.  Polling SetSignal() between Delay() ticks leaves the pending
 * signal untouched, so the normal path costs at most one tick of latency and
 * a broken one surfaces as a clean error instead of a frozen console. */
#define REACTOR_BOOT_TICKS 500   /* 500 * 1/50s = ~10 seconds */

/* Returns 1 if the boot signal arrived, 0 if the wait timed out. */
static int reactor_wait_boot(void)
{
    ULONG mask = 1UL << reactor_boot_sig;
    LONG ticks = 0;
    while (!(SetSignal(0, 0) & mask)) {
        if (ticks++ >= REACTOR_BOOT_TICKS)
            return 0;                /* gave up: reactor_port stays NULL */
        Delay(1);
    }
    SetSignal(0, mask);             /* consume the boot signal */
    return 1;
}

/* Sticky latch: once the reactor process has failed to boot, don't retry —
 * every socket call would otherwise pay the full CreateNewProcTags() +
 * REACTOR_BOOT_TICKS poll-and-give-up cost again (up to ~10s), and leak
 * another reactor_proc handle each time. */
static int reactor_init_failed = 0;

/* Lazily spin up the reactor.  Guarded by a mutex so concurrent first-uses
 * from different threads race safely.  Returns 1 if the reactor is ready. */
static int reactor_ensure(void)
{
    if (reactor_port) return 1;
    if (reactor_init_failed) return 0;
    platform_mutex_lock(reactor_init_mutex);
    if (!reactor_port && !reactor_init_failed) {
        reactor_boot_task = FindTask(NULL);
        reactor_boot_sig = AllocSignal(-1);
        if (reactor_boot_sig >= 0) {
            reactor_proc = CreateNewProcTags(
                NP_Entry,     CL_PROC_ENTRY(reactor_entry_gate, reactor_entry),
#ifdef CL_REACTOR_TLS
                /* TLS handshakes run on this stack (AmiSSL is reactor-
                 * owned); give the crypto code real headroom. */
                CL_PROC_STACK_TAGS(65536),
#else
                CL_PROC_STACK_TAGS(32768),
#endif
                NP_Name,      (ULONG)"amilisp_sockets",
                TAG_DONE);
            if (reactor_proc) {
                /* On timeout the reactor process may still be starting up
                 * and will Signal() this bit later using the copy it
                 * captured for itself at entry (see reactor_entry) — do
                 * NOT FreeSignal() it here, or that bit could be handed to
                 * an unrelated AllocSignal() caller before the reactor
                 * fires it, corrupting that caller's wait. */
                if (reactor_wait_boot())
                    FreeSignal(reactor_boot_sig);
            } else {
                FreeSignal(reactor_boot_sig);
            }
            reactor_boot_sig = -1;
        }
        if (!reactor_port) {
            reactor_init_failed = 1;
            cl_write_cstring_to_stdout(
                "amilisp: socket reactor process failed to start - "
                "sockets are unavailable.\n");
        }
    }
    platform_mutex_unlock(reactor_init_mutex);
    return reactor_port != NULL;
}

/* ===== Client side (any thread): marshal a request and block on the reply ===== */

static void sock_call_impl(SockReq *req, int use_safe_region)
{
    struct MsgPort rp;
    BYTE sig;

    req->result = -1;
    req->out_slot = PLATFORM_SOCKET_INVALID;
    if (!reactor_ensure()) return;

    sig = AllocSignal(-1);
    if (sig < 0) return;

    /* Stack-local reply port — valid for the whole call since we block until
     * the reactor replies. */
    rp.mp_Node.ln_Type = NT_MSGPORT;
    rp.mp_Node.ln_Pri  = 0;
    rp.mp_Node.ln_Name = NULL;
    rp.mp_Flags        = PA_SIGNAL;
    rp.mp_SigBit       = sig;
    rp.mp_SigTask      = FindTask(NULL);
    rp.mp_MsgList.lh_Head     = (struct Node *)&rp.mp_MsgList.lh_Tail;
    rp.mp_MsgList.lh_Tail     = NULL;
    rp.mp_MsgList.lh_TailPred = (struct Node *)&rp.mp_MsgList.lh_Head;

    req->msg.mn_Node.ln_Type = NT_MESSAGE;
    req->msg.mn_Length       = sizeof(*req);
    req->msg.mn_ReplyPort    = &rp;

    if (sock_diag_on()) {
        char d[96];
        req->seq = platform_atomic_inc(&sock_diag_seq);
        sprintf(d, "%s slot=%d len=%lu to=%d task=%p",
                sock_op_name(req->op), (int)req->slot,
                (unsigned long)req->len, req->timeout_ms,
                (void *)rp.mp_SigTask);
        sock_diag_line(req, ">", d);
    }
    PutMsg(reactor_port, &req->msg);
    /* The GC-sweep close path must not enter a safe region (the sweeping
     * thread owns the collection); the reactor is a plain exec Task, not a
     * stopped Lisp thread, so the Wait completes under STW regardless. */
    if (use_safe_region) cl_gc_enter_safe_region();
    /* Wait for the reply, but also listen for Ctrl-C: a task blocked here
     * is in WAIT state where the VM's break poll can never run, so without
     * this a wedged reactor makes Ctrl-C appear dead.  On break, REPORT
     * what we are blocked on (op/slot) and keep waiting — the reply may
     * still arrive, and repeated presses re-report. */
    {
        ULONG rpsig = 1UL << sig;
        for (;;) {
            ULONG got = Wait(rpsig | SIGBREAKF_CTRL_C);
            if (got & SIGBREAKF_CTRL_C) {
                fprintf(stderr, "[SOCK] Ctrl-C while blocked awaiting reactor "
                        "reply: op=%s slot=%d - reactor stuck or reply lost\n",
                        sock_op_name(req->op), (int)req->slot);
                fflush(stderr);
            }
            if (got & rpsig)
                break;
        }
    }
    if (use_safe_region) cl_gc_leave_safe_region();
    GetMsg(&rp);
    FreeSignal(sig);
    if (sock_diag_on()) {
        char d[64];
        sprintf(d, "%s result=%d out=%d", sock_op_name(req->op),
                (int)req->result, (int)req->out_slot);
        sock_diag_line(req, "<", d);
    }
}

static void sock_call(SockReq *req)
{
    sock_call_impl(req, 1);
}

/* Flush the slot's pending write buffer to the wire via the reactor. */
static int sock_flush(PlatformSocket sh)
{
    IOBuf *b = socket_buf[sh];
    SockReq req;
    if (!b || b->wlen == 0) return 0;
    memset(&req, 0, sizeof(req));
    req.op = REQ_WRITE; req.slot = sh; req.buf = b->wbuf; req.len = (uint32_t)b->wlen;
    req.timeout_ms = socket_wtimeout[sh];
    sock_call(&req);
    /* On timeout the bytes were not fully sent; drop the buffer either way (the
     * stream is being torn down) and report the distinct timeout code. */
    b->wlen = 0;
    if (req.result == PLATFORM_SOCKET_TIMEOUT) return PLATFORM_SOCKET_TIMEOUT;
    return req.result;
}

void platform_socket_set_timeout(PlatformSocket sh, int read_ms, int write_ms)
{
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return;
    socket_rtimeout[sh] = (read_ms  > 0) ? read_ms  : 0;
    socket_wtimeout[sh] = (write_ms > 0) ? write_ms : 0;
}

/* All public entry points run on arbitrary client threads.  They never call
 * bsdsocket — they marshal a request to the reactor and block on the reply.
 * Read/write buffering stays caller-side (in the slot's IOBuf), so only a
 * buffer refill/flush costs a reactor round-trip, not every byte. */

PlatformSocket platform_socket_connect(const char *host, int port, int connect_ms)
{
    SockReq req;
    /* Copy hostname onto the stack so sock_call's cl_gc_enter_safe_region()
     * cannot invalidate the pointer if the GC compacts the arena. */
    char host_buf[256];
    strncpy(host_buf, host, sizeof(host_buf) - 1);
    host_buf[sizeof(host_buf) - 1] = '\0';
    memset(&req, 0, sizeof(req));
    req.op = REQ_CONNECT; req.host = host_buf; req.port = port;
    /* connect_ms > 0 arms a deadline on the parked handshake (see
     * reactor_start_connect / reactor_expire_deadlines); 0 waits forever. */
    req.timeout_ms = connect_ms;
    sock_call(&req);
    return req.out_slot;
}

void platform_socket_close(PlatformSocket sh)
{
    SockReq req;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return;
    sock_flush(sh);                       /* push any buffered output first */
    memset(&req, 0, sizeof(req));
    req.op = REQ_CLOSE; req.slot = sh;
    sock_call(&req);
}

PlatformSocket platform_udp_connect(const char *host, int port)
{
    SockReq req;
    /* Stack-copy the hostname — sock_call brackets a GC safe region and a
     * compaction could move an arena-resident string. */
    char host_buf[256];
    strncpy(host_buf, host, sizeof(host_buf) - 1);
    host_buf[sizeof(host_buf) - 1] = '\0';
    memset(&req, 0, sizeof(req));
    req.op = REQ_UDP_CONNECT; req.host = host_buf; req.port = port;
    sock_call(&req);
    return req.out_slot;
}

int platform_udp_send(PlatformSocket sh, const uint8_t *buf, uint32_t len)
{
    /* One datagram per REQ_WRITE.  UDP send() never partial-sends, so the
     * reactor's pend_wpos continuation logic stays at offset 0. */
    SockReq req;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return -1;
    memset(&req, 0, sizeof(req));
    req.op = REQ_WRITE; req.slot = sh;
    req.buf = (char *)buf; req.len = len;
    req.timeout_ms = socket_wtimeout[sh];
    sock_call(&req);
    return req.result;   /* 0 = ok, -1 = error, -2 = timeout */
}

int platform_udp_recv(PlatformSocket sh, uint8_t *buf, uint32_t maxlen)
{
    /* One datagram per REQ_READFILL, received directly into the caller's
     * buffer (shared address space).  Unlike the TCP byte path, 0 here is a
     * valid (empty) datagram, not EOF. */
    SockReq req;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return -1;
    memset(&req, 0, sizeof(req));
    req.op = REQ_READFILL; req.slot = sh;
    req.buf = (char *)buf; req.len = maxlen;
    req.timeout_ms = socket_rtimeout[sh];
    sock_call(&req);
    if (req.result == PLATFORM_SOCKET_TIMEOUT) return PLATFORM_SOCKET_TIMEOUT;
    if (req.result < 0) return -1;
    return req.result;
}

int platform_socket_local_endpoint(PlatformSocket sh, char *ip_out, int *port_out)
{
    SockReq req;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return -1;
    memset(&req, 0, sizeof(req));
    req.op = REQ_ENDPOINT; req.slot = sh;
    req.buf = ip_out; req.len = 16;   /* C stack buffer — safe across the safe region */
    sock_call(&req);
    if (req.result != 0) return -1;
    if (port_out) *port_out = req.out_port;
    return 0;
}

/* --- TLS (platform.h contract) ---
 * CL_REACTOR_TLS builds (AmiSSL SDK installed at compile time) marshal the
 * TLS ops to the reactor, which owns AmiSSL exactly as it owns SocketBase.
 * Builds without the SDK report TLS unavailable; plain sockets are
 * unaffected either way. */

#ifdef CL_REACTOR_TLS

/* Availability probe from any task: amisslmaster.library presence is
 * Exec-safe to test here; the actual InitAmiSSL happens in the reactor on
 * first use.  Cached — AmiSSL doesn't come and go at runtime. */
static int  amissl_probe_state = 0;      /* 0 untried, 1 present, -1 absent */
static char amissl_version_str[32];

int platform_tls_available(void)
{
    if (amissl_probe_state == 0) {
        struct Library *l = OpenLibrary("amisslmaster.library",
                                        AMISSLMASTER_MIN_VERSION);
        if (l) {
            sprintf(amissl_version_str, "AmiSSL %d.%d",
                    (int)l->lib_Version, (int)l->lib_Revision);
            CloseLibrary(l);
            amissl_probe_state = 1;
        } else {
            amissl_probe_state = -1;
        }
    }
    return amissl_probe_state == 1;
}

const char *platform_tls_version(void)
{
    return platform_tls_available() ? amissl_version_str : NULL;
}

int platform_tls_start(PlatformSocket sh, const PlatformTLSParams *params,
                       char *err, uint32_t errlen)
{
    SockReq req;
    if (err && errlen > 0) err[0] = '\0';
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE || socket_table[sh] < 0) {
        if (err && errlen > 0) {
            strncpy(err, "invalid or closed socket", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    if (!socket_buf[sh]) {
        /* Listener and UDP slots have no IOBuf; neither can carry TLS. */
        if (err && errlen > 0) {
            strncpy(err, "TLS requires a connected TCP stream socket", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    if (socket_ssl[sh]) {
        if (err && errlen > 0) {
            strncpy(err, "socket is already TLS-upgraded", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    /* Buffered plaintext output must reach the wire before handshake bytes. */
    if (sock_flush(sh) != 0) {
        if (err && errlen > 0) {
            strncpy(err, "flushing pending output before the TLS handshake "
                    "failed", errlen - 1);
            err[errlen - 1] = '\0';
        }
        return -1;
    }
    memset(&req, 0, sizeof(req));
    req.op = REQ_TLS_START;
    req.slot = sh;
    req.tls_params = params;      /* caller-stack struct; we block below */
    req.buf = err;                /* diagnostic buffer (may be NULL) */
    req.len = err ? errlen : 0;
    req.timeout_ms = params->timeout_ms;
    sock_call(&req);
    if (req.result == 0) return 0;
    if (err && errlen > 0 && err[0] == '\0') {
        strncpy(err, req.result == PLATFORM_SOCKET_TIMEOUT
                ? "TLS handshake timed out" : "TLS handshake failed",
                errlen - 1);
        err[errlen - 1] = '\0';
    }
    return -1;
}

int platform_tls_active(PlatformSocket sh)
{
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return 0;
    return socket_ssl[sh] != NULL;   /* aligned pointer read — benign race */
}

int platform_tls_peer_cert_field(PlatformSocket sh, int field,
                                 char *out, uint32_t outlen)
{
    SockReq req;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE || !socket_ssl[sh])
        return -1;
    memset(&req, 0, sizeof(req));
    req.op = REQ_TLS_PEERCERT;
    req.slot = sh;
    req.port = field;
    req.buf = out;
    req.len = outlen;
    sock_call(&req);
    return req.result;
}

#else /* !CL_REACTOR_TLS — built without the AmiSSL SDK */

int platform_tls_available(void)
{
    return 0;
}

const char *platform_tls_version(void)
{
    return NULL;
}

int platform_tls_start(PlatformSocket sh, const PlatformTLSParams *params,
                       char *err, uint32_t errlen)
{
    (void)sh; (void)params;
    if (err && errlen > 0) {
        strncpy(err, "this build has no TLS support (compiled without the "
                "AmiSSL SDK headers - see include/amissl-sdk)", errlen - 1);
        err[errlen - 1] = '\0';
    }
    return -1;
}

int platform_tls_active(PlatformSocket sh)
{
    (void)sh;
    return 0;
}

int platform_tls_peer_cert_field(PlatformSocket sh, int field,
                                 char *out, uint32_t outlen)
{
    (void)sh; (void)field; (void)out; (void)outlen;
    return -1;
}

#endif /* CL_REACTOR_TLS */

void platform_socket_close_gc(PlatformSocket sh)
{
    /* Sweep-finalizer variant: skip the flush (buffered output on an
     * unreachable stream is forfeit; sock_flush also brackets a safe
     * region) and Wait without entering a safe region. */
    SockReq req;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return;
    memset(&req, 0, sizeof(req));
    req.op = REQ_CLOSE; req.slot = sh;
    sock_call_impl(&req, 0);
}

int platform_socket_read(PlatformSocket sh)
{
    IOBuf *b;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return -1;
    b = socket_buf[sh];
    if (!b) return -1;
    if (b->rpos < b->rlen)
        return (unsigned char)b->rbuf[b->rpos++];
    /* Refill: the reactor recv()s directly into rbuf (shared address space). */
    {
        SockReq req;
        memset(&req, 0, sizeof(req));
        req.op = REQ_READFILL; req.slot = sh;
        req.buf = b->rbuf; req.len = PLATFORM_IOBUF_SIZE;
        req.timeout_ms = socket_rtimeout[sh];
        sock_call(&req);
        if (req.result == PLATFORM_SOCKET_TIMEOUT) return PLATFORM_SOCKET_TIMEOUT;
        if (req.result <= 0) return -1;   /* EOF or error */
        b->rpos = 1;
        b->rlen = req.result;
        return (unsigned char)b->rbuf[0];
    }
}

int platform_socket_data_available(PlatformSocket sh)
{
    IOBuf *b;
    SockReq req;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return -1;
    b = socket_buf[sh];
    if (b && b->rpos < b->rlen)
        return 1;                       /* already-buffered bytes */
    /* Probe the fd via the reactor (it owns the WaitSelect/recv calls). */
    memset(&req, 0, sizeof(req));
    req.op = REQ_POLL; req.slot = sh;
    sock_call(&req);
    return req.result;                  /* 1 ready, 0 would-block, 2 EOF, -1 invalid */
}

int platform_socket_write(PlatformSocket sh, int byte)
{
    IOBuf *b;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return -1;
    b = socket_buf[sh];
    if (!b) return -1;
    b->wbuf[b->wlen++] = (char)byte;
    if (b->wlen >= PLATFORM_IOBUF_SIZE)
        return sock_flush(sh);
    return 0;
}

int platform_socket_write_buf(PlatformSocket sh, const char *buf, uint32_t len)
{
    IOBuf *b;
    uint32_t pos = 0;
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return -1;
    b = socket_buf[sh];
    if (!b) return -1;
    while (pos < len) {
        int avail = PLATFORM_IOBUF_SIZE - b->wlen;
        int chunk = (int)(len - pos);
        if (chunk > avail) chunk = avail;
        memcpy(b->wbuf + b->wlen, buf + pos, (size_t)chunk);
        b->wlen += chunk;
        pos += (uint32_t)chunk;
        if (b->wlen >= PLATFORM_IOBUF_SIZE) {
            int fr = sock_flush(sh);
            if (fr != 0) return fr;     /* propagate -1 (error) or -2 (timeout) */
        }
    }
    return 0;
}

int platform_socket_flush(PlatformSocket sh)
{
    if (sh == 0 || sh >= PLATFORM_SOCKET_TABLE_SIZE) return -1;
    return sock_flush(sh);
}

PlatformSocket platform_socket_listen(int port, int loopback, int *actual_port)
{
    SockReq req;
    memset(&req, 0, sizeof(req));
    req.op = REQ_LISTEN; req.port = port; req.loopback = loopback;
    sock_call(&req);
    if (req.out_slot != PLATFORM_SOCKET_INVALID && actual_port)
        *actual_port = req.out_port;
    return req.out_slot;
}

/* "a.b.c.d" -> the address in host byte order; 0 for anything else.  Parsed
 * here, on the caller's task: inet_addr() is a bsdsocket.library call and
 * the library base belongs to the reactor task. */
static int parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t value = 0;
    int part;
    for (part = 0; part < 4; part++) {
        int digits = 0, octet = 0;
        while (*s >= '0' && *s <= '9' && digits < 3) {
            octet = octet * 10 + (*s - '0');
            s++; digits++;
        }
        if (digits == 0 || octet > 255) return 0;
        value = (value << 8) | (uint32_t)octet;
        if (part < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s != '\0') return 0;
    *out = value;
    return 1;
}

PlatformSocket platform_socket_listen_addr(int port, const char *bind_addr, int *actual_port)
{
    SockReq req;
    uint32_t ip = 0;
    if (bind_addr && !parse_ipv4(bind_addr, &ip))
        return PLATFORM_SOCKET_INVALID;
    memset(&req, 0, sizeof(req));
    req.op = REQ_LISTEN; req.port = port; req.loopback = 0; req.bind_addr = ip;
    sock_call(&req);
    if (req.out_slot != PLATFORM_SOCKET_INVALID && actual_port)
        *actual_port = req.out_port;
    return req.out_slot;
}

PlatformSocket platform_socket_accept(PlatformSocket listener)
{
    SockReq req;
    if (listener == 0 || listener >= PLATFORM_SOCKET_TABLE_SIZE)
        return PLATFORM_SOCKET_INVALID;
    memset(&req, 0, sizeof(req));
    req.op = REQ_ACCEPT; req.slot = listener;
    sock_call(&req);
    return req.out_slot;
}

void platform_fpu_setup(void)
{
#ifdef __HAVE_68881__
    /* Hard-float build only.  The 68881/68882 defaults to extended-precision
     * rounding: every C double operation carries 64 mantissa bits until the
     * value is stored, so chained arithmetic sees double-rounded results
     * that differ from strict IEEE doubles — e.g. (floor x (/ x 7)) reads a
     * quotient a hair under 7 and answers 6 where the host answers 7.  Set
     * FPCR to round-to-nearest / double precision (0x80) so arithmetic
     * matches the host bit for bit on real Motorola FPUs.  Per-task FPU
     * context: amiga_thread_entry() repeats this for every worker thread. */
    __asm__ volatile ("fmove.l %0,fpcr" : : "d" (0x00000080UL));
#endif
}

void platform_init(void)
{
    /* dos.library is auto-opened by startup */
    platform_fpu_setup();
    platform_mutex_init(&reactor_init_mutex);
}

void platform_shutdown(void)
{
    /* Restore the console to cooked mode if a TUI died without cleaning up —
     * a raw-mode CLI window would otherwise stay unusable after exit. */
    if (tty_raw_active)
        platform_tty_raw(0);

    /* Tell the reactor to close every socket, drop SocketBase, and exit.  The
     * reactor replies before tearing down, so this returns once it is done. */
    if (reactor_port) {
        SockReq req;
        memset(&req, 0, sizeof(req));
        req.op = REQ_SHUTDOWN;
        sock_call(&req);
    }
}

/* =============================================================
 * Program arguments, Workbench start, the main stack, process exit
 * ============================================================= */

#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/icon.h>
#include "../core/cmdline.h"

/* <unistd.h> cannot be included this late (the bsdsocket headers above
 * define a getdtablesize macro that breaks it); _exit is all we need. */
extern void _exit(int status);

struct Library *IconBase = NULL;

#ifndef PLATFORM_MORPHOS
/* libnix's Workbench start: with no console of its own, a tool started
 * from an icon gets this window as Input()/Output() (opened before main by
 * libnix's __nocommandline; AUTO = it appears only when something is
 * written or read, so a GUI program never shows it, and WAIT keeps it up
 * until the user closes it once it did appear).  libnix closes it again
 * from its exit list (__exitcommandline), which DOES run on the 68k exit
 * path: in libnix's ncrt0.S `_exit' and `exit' are one entry, so the
 * runtime must not close this handle itself -- a second Close() of a
 * freed FileHandle is a "Program failed" requester at exit (seen 2026-09-18
 * in the suite's Workbench leg).  The same goes for the current-directory
 * lock that start set: libnix's exit unlocks it. */
char *__stdiowin = "CON:0/20/640/200/AmiLisp/AUTO/CLOSE/WAIT";
#endif

static int    wb_started = 0;          /* main() was handed a WBStartup */
static BPTR   wb_stdio_console = 0;    /* libnix's __stdiowin handle (libnix closes it) */
static BPTR   wb_window_console = 0;   /* the WINDOW tool type's, if any (ours) */
static char   wb_argbuf[2048];         /* the synthetic command line ... */
static char  *wb_argv[64];             /* ... and its argv (64 slots) */
static char   wb_pathbuf[512];

/* The main stack swap (platform_run_main / platform_process_exit). */
static struct StackSwapStruct main_stack_swap;
static void *main_stack_mem = NULL;

/* Read the ARGS and WINDOW tool types of the icon NAME in the directory
 * LOCK into the builder / *WINDOW.  Missing icon or library: nothing. */
static void wb_read_icon(BPTR lock, const char *name, CL_ArgvBuilder *b,
                         const char **window)
{
    BPTR old;
    struct DiskObject *dobj;
    if (!IconBase || !name)
        return;
    old = CurrentDir(lock);
    dobj = GetDiskObject((CONST_STRPTR)name);
    if (dobj) {
        if (dobj->do_ToolTypes) {
            const char *v;
            v = (const char *)FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                                           (CONST_STRPTR)"ARGS");
            if (v)
                cl_argv_builder_split(b, v);
            v = (const char *)FindToolType((CONST_STRPTR *)dobj->do_ToolTypes,
                                           (CONST_STRPTR)"WINDOW");
            if (v && *v) {
                /* keep a copy: the DiskObject goes away below */
                static char winbuf[256];
                strncpy(winbuf, v, sizeof(winbuf) - 1);
                winbuf[sizeof(winbuf) - 1] = '\0';
                *window = winbuf;
            }
        }
        FreeDiskObject(dobj);
    }
    CurrentDir(old);
}

int platform_startup_args(int *argc, char ***argv)
{
    struct WBStartup *wbs;
    CL_ArgvBuilder b;
    const char *window = NULL;
    LONG i;

    if (*argc != 0)
        return 0;                       /* a Shell start: argv is the command line */
    wbs = (struct WBStartup *)*argv;
    wb_started = 1;
    wb_stdio_console = Input();         /* libnix's __stdiowin (0 without one) */

    cl_argv_builder_init(&b, wb_argbuf, (int)sizeof(wb_argbuf),
                         wb_argv, (int)(sizeof(wb_argv) / sizeof(wb_argv[0])));
    if (wbs && wbs->sm_NumArgs > 0 && wbs->sm_ArgList[0].wa_Name &&
        wbs->sm_ArgList[0].wa_Name[0])
        wb_tool_name = (const char *)wbs->sm_ArgList[0].wa_Name;
    cl_argv_builder_add(&b, wb_tool_name ? wb_tool_name : "amilisp");

    IconBase = OpenLibrary((CONST_STRPTR)"icon.library", 36);
    if (wbs && wbs->sm_ArgList) {
        /* The tool's icon first, then every project's: a project's ARGS
         * come later on the line, so its options win where one wins. */
        for (i = 0; i < wbs->sm_NumArgs; i++)
            wb_read_icon(wbs->sm_ArgList[i].wa_Lock,
                         (const char *)wbs->sm_ArgList[i].wa_Name, &b, &window);
        /* Every project (a file icon double-clicked with amilisp as its
         * default tool, or shift-selected with the tool's icon) is one
         * argument after --, as a full path: the program decides what to
         * do with it — it is never loaded on the program's behalf. */
        if (wbs->sm_NumArgs > 1)
            cl_argv_builder_add(&b, "--");
        for (i = 1; i < wbs->sm_NumArgs; i++) {
            struct WBArg *a = &wbs->sm_ArgList[i];
            if (!a->wa_Name || !a->wa_Name[0])
                continue;               /* a drawer, not a file */
            wb_pathbuf[0] = '\0';
            if (a->wa_Lock &&
                NameFromLock(a->wa_Lock, (STRPTR)wb_pathbuf, (LONG)sizeof(wb_pathbuf)) &&
                AddPart((STRPTR)wb_pathbuf, (CONST_STRPTR)a->wa_Name, (ULONG)sizeof(wb_pathbuf)))
                cl_argv_builder_add(&b, wb_pathbuf);
            else
                cl_argv_builder_add(&b, (const char *)a->wa_Name);
        }
    }
    if (IconBase) {
        CloseLibrary(IconBase);
        IconBase = NULL;
    }

    /* WINDOW=<console spec>: the console this run reads and writes (the
     * IconX convention; NIL: for none).  Replaces libnix's default window
     * for Input()/Output(); C stdio keeps the default one, which stays
     * unopened unless something is printed through it. */
    if (window) {
        wb_window_console = Open((CONST_STRPTR)window, MODE_OLDFILE);
        if (wb_window_console) {
            SelectInput(wb_window_console);
            SelectOutput(wb_window_console);
        } else {
            platform_write_string("amilisp: cannot open the WINDOW tool type's console: ");
            platform_write_string(window);
            platform_write_string("\n");
        }
    }
    if (b.overflow)
        platform_write_string("amilisp: too many or too long arguments in the "
                              "icons' ARGS tool types and projects; some were dropped\n");

    *argc = b.argc;
    *argv = b.argv;
    return 1;
}

/* Close what THIS runtime opened for a Workbench start: the WINDOW tool
 * type's console, after putting libnix's stdio window back as Input()/
 * Output() (C stdio has been flushed for the last time by then).  libnix's
 * own window and the DupLock'd current directory its startup set are
 * libnix's to close and unlock, from the exit list that _exit runs (see
 * __stdiowin above); closing them here too crashed the process at exit. */
static void wb_teardown(void)
{
    if (!wb_started)
        return;
    SelectInput(wb_stdio_console);
    SelectOutput(wb_stdio_console);
    if (wb_window_console) {
        Close(wb_window_console);
        wb_window_console = 0;
    }
    wb_stdio_console = 0;
    wb_started = 0;
}

int platform_run_main(int (*fn)(int, char **), int argc, char **argv)
{
#ifdef PLATFORM_MORPHOS
    /* Native code runs on the PPC stack, sized by the __stack global the
     * MorphOS startup honours (main.c). */
    return fn(argc, argv);
#else
    struct Task *t = FindTask(NULL);
    ULONG have = (ULONG)((char *)t->tc_SPUpper - (char *)t->tc_SPLower);
    int rc;
    if (have >= PLATFORM_MAIN_STACK_MIN)
        return fn(argc, argv);
    main_stack_mem = AllocVec(PLATFORM_MAIN_STACK_MIN, MEMF_ANY);
    if (!main_stack_mem)
        return fn(argc, argv);          /* out of memory: run with what there is */
    main_stack_swap.stk_Lower = main_stack_mem;
    main_stack_swap.stk_Upper = (ULONG)main_stack_mem + PLATFORM_MAIN_STACK_MIN;
    main_stack_swap.stk_Pointer = (APTR)main_stack_swap.stk_Upper;
    /* The swap itself is assembly (stack_swap_m68k.s): to gcc, StackSwap
     * is an ordinary call, and C code around it may keep sp-relative
     * temporaries or defer a callee's argument pop past the swap back --
     * this function did, unwinding the wrong stack before its rts. */
    rc = platform_stack_call(&main_stack_swap, fn, argc, argv);
    FreeVec(main_stack_mem);
    main_stack_mem = NULL;
    return rc;
#endif
}

void platform_process_exit(int code)
{
    fflush(NULL);
    wb_teardown();
#ifndef PLATFORM_MORPHOS
    if (main_stack_mem) {
        /* Back on the stack we were given, so the task's stack bounds are
         * the original ones again when libnix's exit resets SP to what it
         * saved at _start (a frame on that stack); the frames above the
         * swap point are abandoned.  Swap, FreeVec and _exit are one
         * assembly routine: nothing of ours runs on either stack in
         * between. */
        void *mem = main_stack_mem;
        main_stack_mem = NULL;
        platform_stack_exit(&main_stack_swap, mem, code);
    }
#endif
    /* On 68k libnix this is the same entry as exit(): the exit list runs
     * (libnix's Workbench window and lock, stdio, the libraries it opened)
     * and the WBStartup message is replied; only the C-level atexit work
     * of the runtime is skipped, which is the point. */
    _exit(code);
}

void platform_release_resources(void)
{
    /* Remove our public ARexx port from exec's list.  This is a correctness
     * fix before it is a memory one: a public port left registered outlives
     * the process and points at memory DOS has reclaimed, so the next program
     * that looks the name up finds a corpse. */
    platform_arexx_close();

    /* Close any file left open at exit.  A stream the program never CLOSEd
     * still holds a DOS handle and an 8 KB IOBuf (two 4 KB buffers plus the
     * header); on AmigaOS neither comes back on its own, and the buffered
     * write side would silently lose whatever had not been flushed.
     * platform_file_close does the flush, the Close and the buffer free. */
    {
        int fh;
        for (fh = 1; fh < PLATFORM_FILE_TABLE_SIZE; fh++)
            if (file_table[fh])
                platform_file_close((PlatformFile)fh);
    }

    if (file_table_mutex) {
        platform_mutex_destroy(file_table_mutex);
        file_table_mutex = NULL;
        file_table_init = 0;
    }
    if (reactor_init_mutex) {
        platform_mutex_destroy(reactor_init_mutex);
        reactor_init_mutex = NULL;
    }
}

/* =============================================================
 * Generic FFI: foreign memory (Amiga implementation)
 *
 * On Amiga (32-bit), handles ARE raw addresses — no side table needed.
 * ============================================================= */

uint32_t platform_ffi_alloc(uint32_t size)
{
    void *p;
    if (size == 0) return 0;
    p = AllocVec(size, MEMF_CLEAR);
    return (uint32_t)p;
}

void platform_ffi_free(uint32_t handle, uint32_t size)
{
    (void)size;
    if (handle == 0) return;
    FreeVec((void *)handle);
}

void *platform_ffi_resolve(uint32_t handle)
{
    return (void *)handle;
}

uint32_t platform_ffi_register(void *ptr)
{
    /* Handles are raw addresses on Amiga — no side table. */
    return (uint32_t)ptr;
}

void platform_ffi_release(uint32_t handle)
{
    /* Nothing to reclaim — handles are raw addresses. */
    (void)handle;
}

/* Dynamic-library / generic-call FFI is not available on AmigaOS — the
 * Amiga path uses the library-vector model (platform_amiga_call) instead. */
uint32_t platform_ffi_dlopen(const char *name)
{
    (void)name;
    return 0;
}

uint32_t platform_ffi_dlsym(uint32_t lib_handle, const char *name)
{
    (void)lib_handle; (void)name;
    return 0;
}

void platform_ffi_dlclose(uint32_t lib_handle)
{
    (void)lib_handle;
}

int platform_ffi_call(void *fn, CLFFIType ret_type, CLFFIValue *ret_val,
                      int nargs, int nfixed,
                      const CLFFIType *arg_types, const CLFFIValue *arg_vals)
{
    (void)fn; (void)ret_type; (void)ret_val;
    (void)nargs; (void)nfixed; (void)arg_types; (void)arg_vals;
    return -1;  /* unsupported */
}

/* ================================================================
 * Callbacks — Lisp functions as C function pointers and hook entries
 *
 * A callback is a few words of 68k code in AllocVec'd memory (all RAM is
 * executable on the 68k; the MorphOS emulator runs 68k code from any
 * memory) with the closure descriptor baked in as an immediate, so one
 * shared C entry can tell the callbacks apart.  No libffi needed.
 *
 * m68k (AmigaOS 3):
 *
 *     movem.l d0-d7/a0-a6,-(sp)     ; the caller's registers: frame[0..14]
 *     move.l  sp,-(sp)              ; &frame
 *     pea     <closure>.l
 *     jsr     cl_amiga_callback_entry.l   ; C: (closure, frame) -> d0
 *     lea     12(sp),sp             ; the two args + the saved d0 slot
 *     movem.l (sp)+,d1-d7/a0-a6     ; callee-saved back, d0 = result
 *     rts
 *
 * frame[15] is the caller's return address, frame[16..] its C-stack
 * arguments.  Register arguments (struct Hook: a0/a2/a1; BOOPSI dispatcher:
 * the same) are read from frame[reg].  C-stack arguments follow the ABI of
 * the toolchain that compiles amilisp (m68k-amigaos-gcc, -mcpu=68020):
 * every scalar argument occupies one 4-byte slot — char / short are
 * promoted by the caller (`pea -3.l` for a char -3) — and a 64-bit one two,
 * high word first.  Verified by disassembling a probe (see
 * specs/mui-bindings.md §10.1).
 *
 * MorphOS (PPC): the template is `pea <closure>.l; jsr <gate>.l; addq.l
 * #4,sp; rts`, where <gate> is one shared struct EmulLibEntry (TRAP_LIB)
 * around a native function — the emulator executes the pea/jsr, hits the
 * trap word and calls the PPC function, whose r3 becomes d0 for the `rts`.
 * The PPC side finds the closure at 4(REG_A7) (above the jsr's return
 * address), the C-stack arguments at 12(REG_A7) (above the caller's return
 * address as well), and the registers in the emulator's frame (REG_D0 ..
 * REG_A6) — the same frame platform_amiga_call writes on the way out.
 * MorphOS's own CallHookPkt calls a 68k h_Entry through the emulator, so a
 * native (PPC) MUI invoking a Lisp hook takes the same path.
 *
 * Both entries hand the decoded CLFFIValue[] to the generic handler
 * (builtins_ffi.c ffi_callback_handler), which owns the foreign-callback
 * boundary: it never longjmps out of here.
 * ================================================================ */

typedef struct {
    CLFFIType ret_type;
    int       nargs;
    CLFFIType arg_types[CL_FFI_MAX_ARGS];
    int8_t    arg_regs[CL_FFI_MAX_ARGS];   /* CL_FFI_REG_STACK, 0..7 = d0..d7, 8..14 = a0..a6 */
    platform_ffi_cb_handler handler;
    void     *user_data;
    void     *code;                        /* the AllocVec'd stub */
} AmigaClosure;

/* Decode the arguments from the 68k register image REGS (d0-d7/a0-a6, 15
 * longwords as the caller left them) and the C-stack arguments at STACK,
 * run the handler, return what goes into d0. */
static uint32_t amiga_closure_invoke(AmigaClosure *pc, const uint32_t *regs,
                                     const uint32_t *stack)
{
    CLFFIValue cargs[CL_FFI_MAX_ARGS];
    CLFFIValue cret;
    int i, si = 0;

    for (i = 0; i < pc->nargs; i++) {
        CLFFIType ty = pc->arg_types[i];
        int r = pc->arg_regs[i];
        int wide = (ty == CL_FFI_I64 || ty == CL_FFI_U64);
        uint32_t hi, lo = 0;
        if (r >= 0 && r <= 14) {
            hi = regs[r];
        } else {
            hi = stack[si++];
            if (wide) lo = stack[si++];
        }
        switch (ty) {
        case CL_FFI_I8:      cargs[i].i8  = (int8_t)hi;   break;
        case CL_FFI_U8:      cargs[i].u8  = (uint8_t)hi;  break;
        case CL_FFI_I16:     cargs[i].i16 = (int16_t)hi;  break;
        case CL_FFI_U16:     cargs[i].u16 = (uint16_t)hi; break;
        case CL_FFI_I32:     cargs[i].i32 = (int32_t)hi;  break;
        case CL_FFI_U32:     cargs[i].u32 = hi;           break;
        case CL_FFI_I64:     cargs[i].i64 = (int64_t)(((uint64_t)hi << 32) | lo); break;
        case CL_FFI_U64:     cargs[i].u64 = ((uint64_t)hi << 32) | lo; break;
        case CL_FFI_POINTER: cargs[i].p   = (void *)hi;   break;
        default:             cargs[i].u32 = hi;           break;  /* fp rejected at creation */
        }
    }

    memset(&cret, 0, sizeof(cret));
    pc->handler(pc->user_data, cargs, &cret);

    switch (pc->ret_type) {
    case CL_FFI_I8:      return (uint32_t)(int32_t)cret.i8;
    case CL_FFI_U8:      return cret.u8;
    case CL_FFI_I16:     return (uint32_t)(int32_t)cret.i16;
    case CL_FFI_U16:     return cret.u16;
    case CL_FFI_I32:     return (uint32_t)cret.i32;
    case CL_FFI_U32:     return cret.u32;
    case CL_FFI_POINTER: return (uint32_t)cret.p;
    default:             return 0;   /* void; 64-bit / fp rejected at creation */
    }
}

#ifndef PLATFORM_MORPHOS

/* Entered from the m68k stub (not static: the stub carries its address). */
uint32_t cl_amiga_callback_entry(AmigaClosure *pc, uint32_t *frame)
{
    return amiga_closure_invoke(pc, frame, frame + 16);
}

#define AMIGA_STUB_WORDS 14

static void amiga_write_stub(uint16_t *c, AmigaClosure *pc)
{
    uint32_t entry = (uint32_t)&cl_amiga_callback_entry;
    uint32_t desc  = (uint32_t)pc;
    c[0]  = 0x48E7; c[1]  = 0xFFFE;                   /* movem.l d0-d7/a0-a6,-(sp) */
    c[2]  = 0x2F0F;                                   /* move.l  sp,-(sp)          */
    c[3]  = 0x4879; c[4]  = (uint16_t)(desc >> 16);   /* pea     desc.l            */
                    c[5]  = (uint16_t)desc;
    c[6]  = 0x4EB9; c[7]  = (uint16_t)(entry >> 16);  /* jsr     entry.l           */
                    c[8]  = (uint16_t)entry;
    c[9]  = 0x4FEF; c[10] = 0x000C;                   /* lea     12(sp),sp         */
    c[11] = 0x4CDF; c[12] = 0x7FFE;                   /* movem.l (sp)+,d1-d7/a0-a6 */
    c[13] = 0x4E75;                                   /* rts                       */
}

#else  /* MorphOS */

#include <emul/emulinterface.h>
#include <emul/emulregs.h>

static ULONG cl_mos_callback_gate(void)
{
    uint32_t regs[15];
    const uint32_t *sp = (const uint32_t *)REG_A7;
    AmigaClosure *pc = (AmigaClosure *)sp[1];
    regs[0]  = REG_D0; regs[1]  = REG_D1; regs[2]  = REG_D2; regs[3]  = REG_D3;
    regs[4]  = REG_D4; regs[5]  = REG_D5; regs[6]  = REG_D6; regs[7]  = REG_D7;
    regs[8]  = REG_A0; regs[9]  = REG_A1; regs[10] = REG_A2; regs[11] = REG_A3;
    regs[12] = REG_A4; regs[13] = REG_A5; regs[14] = REG_A6;
    return (ULONG)amiga_closure_invoke(pc, regs, sp + 3);
}

static const struct EmulLibEntry cl_mos_callback_gate_entry = {
    TRAP_LIB, 0, (void (*)(void))cl_mos_callback_gate
};

#define AMIGA_STUB_WORDS 8

static void amiga_write_stub(uint16_t *c, AmigaClosure *pc)
{
    uint32_t gate = (uint32_t)&cl_mos_callback_gate_entry;
    uint32_t desc = (uint32_t)pc;
    c[0] = 0x4879; c[1] = (uint16_t)(desc >> 16); c[2] = (uint16_t)desc;   /* pea desc.l  */
    c[3] = 0x4EB9; c[4] = (uint16_t)(gate >> 16); c[5] = (uint16_t)gate;   /* jsr gate.l  */
    c[6] = 0x588F;                                                         /* addq.l #4,sp */
    c[7] = 0x4E75;                                                         /* rts          */
}

#endif

void *platform_ffi_make_closure(CLFFIType ret_type, int nargs,
                                const CLFFIType *arg_types,
                                const int8_t *arg_regs,
                                platform_ffi_cb_handler handler,
                                void *user_data, void **out_closure)
{
    AmigaClosure *pc;
    uint16_t *code;
    int i;

    if (out_closure) *out_closure = NULL;
    if (nargs < 0 || nargs > CL_FFI_MAX_ARGS) return NULL;
    pc = (AmigaClosure *)AllocVec(sizeof(AmigaClosure), MEMF_PUBLIC | MEMF_CLEAR);
    if (!pc) return NULL;
    code = (uint16_t *)AllocVec(AMIGA_STUB_WORDS * 2, MEMF_PUBLIC | MEMF_CLEAR);
    if (!code) { FreeVec(pc); return NULL; }

    pc->ret_type = ret_type;
    pc->nargs = nargs;
    for (i = 0; i < nargs; i++) {
        pc->arg_types[i] = arg_types[i];
        pc->arg_regs[i] = arg_regs ? arg_regs[i] : (int8_t)CL_FFI_REG_STACK;
    }
    pc->handler = handler;
    pc->user_data = user_data;
    pc->code = code;

    amiga_write_stub(code, pc);
    platform_cache_clear(code, AMIGA_STUB_WORDS * 2);   /* fresh code past the I-cache */

    if (out_closure) *out_closure = pc;
    return code;
}

void platform_ffi_free_closure(void *closure)
{
    AmigaClosure *pc = (AmigaClosure *)closure;
    if (!pc) return;
    if (pc->code) FreeVec(pc->code);
    FreeVec(pc);
}

uint32_t platform_ffi_peek32(uint32_t handle, uint32_t offset)
{
    return *(volatile uint32_t *)((uint8_t *)handle + offset);
}

uint16_t platform_ffi_peek16(uint32_t handle, uint32_t offset)
{
    return *(volatile uint16_t *)((uint8_t *)handle + offset);
}

uint8_t platform_ffi_peek8(uint32_t handle, uint32_t offset)
{
    return *(volatile uint8_t *)((uint8_t *)handle + offset);
}

void platform_ffi_poke32(uint32_t handle, uint32_t offset, uint32_t val)
{
    *(volatile uint32_t *)((uint8_t *)handle + offset) = val;
}

void platform_ffi_poke16(uint32_t handle, uint32_t offset, uint16_t val)
{
    *(volatile uint16_t *)((uint8_t *)handle + offset) = val;
}

void platform_ffi_poke8(uint32_t handle, uint32_t offset, uint8_t val)
{
    *(volatile uint8_t *)((uint8_t *)handle + offset) = val;
}

/* =============================================================
 * Amiga-specific FFI: shared library calls
 * ============================================================= */

uint32_t platform_amiga_open_library(const char *name, uint32_t version)
{
    struct Library *lib = OpenLibrary((CONST_STRPTR)name, (ULONG)version);
    if (!lib) return 0;
    return (uint32_t)lib;
}

void platform_amiga_close_library(uint32_t lib_base)
{
    if (lib_base == 0) return;
    CloseLibrary((struct Library *)lib_base);
}

/* MUIM_Application_PushMethod from a foreign task (platform.h).  The
 * application's class dispatcher is the Hook at the start of its IClass
 * (what amiga.lib's DoMethodA does), invoked through utility.library so
 * that a PPC MUI on MorphOS and a 68k one on AmigaOS are called the same
 * way.  utility.library is opened on the first push and stays open: a
 * library base is a reference count, not memory, and this can run on
 * input.device's task, where a CloseLibrary would be one call too many.
 * OCLASS is intuition/classes.h's (classusr.h only has the Object type):
 * the m68k NDK reaches it through another include, the MorphOS SDK does
 * not, and the PPC link failed on the implicit declaration. */
#include <intuition/classes.h>
#include <proto/utility.h>
struct Library *UtilityBase = NULL;

void platform_amiga_push_method(uint32_t app, uint32_t push_method,
                                uint32_t object, uint32_t method,
                                uint32_t value)
{
    ULONG msg[5];
    struct IClass *cl;

    if (app == 0) return;
    if (UtilityBase == NULL) {
        /* Opened by the Lisp task at hook creation (see
         * platform_amiga_push_method_prepare); a NULL here means that did
         * not happen, and the push is dropped rather than an OpenLibrary
         * attempted from a task that may not own a stack for it. */
        return;
    }
    cl = OCLASS((Object *)app);
    if (cl == NULL) return;
    msg[0] = (ULONG)push_method;
    msg[1] = (ULONG)object;
    msg[2] = 2;
    msg[3] = (ULONG)method;
    msg[4] = (ULONG)value;
    CallHookPkt((struct Hook *)cl, (Object *)app, msg);
}

int platform_amiga_push_method_prepare(void)
{
    if (UtilityBase == NULL)
        UtilityBase = OpenLibrary((CONST_STRPTR)"utility.library", 36);
    return UtilityBase != NULL;
}

int platform_amiga_last_pushed_method(uint32_t out[5])
{
    (void)out;
    return 0;
}

uint32_t platform_amiga_do_super_method(void *cl, void *object, void *message)
{
    struct IClass *super;
    if (cl == NULL || UtilityBase == NULL) return 0;
    super = ((struct IClass *)cl)->cl_Super;
    if (super == NULL) return 0;
    /* cl_Dispatcher is the Hook at the start of the IClass */
    return (uint32_t)CallHookPkt((struct Hook *)super, (Object *)object, message);
}

/* platform_amiga_call() is implemented in ffi_dispatch_m68k.s
 * (68k assembly trampoline for register-based library calls).
 *
 * On MorphOS (PPC) the same call is dispatched through the ABox
 * emulator's per-task 68k register frame — the identical mechanism the
 * SDK's ppcinline LP macros use for every OS call: write the argument
 * values into the frame's Dn[]/An[] images (REG_D0..REG_A5), put the
 * library base in REG_A6, and jump through the library vector at
 * base+offset via EmulCallDirectOS.  Native MorphOS libraries hit their
 * PPC emulgate directly (no 68k emulation on the hot path); real 68k
 * libraries run under emulation — both honour the d0-d7/a0-a5 register
 * convention, so callers can't tell the ports apart.
 *
 * MyEmulHandle lives in r2 and is per-task, so this is MT-safe (every
 * amilisp thread is its own MorphOS process).
 *
 * Like the m68k trampoline, all 13 argument registers are loaded
 * unconditionally from the pre-zeroed regs[] array; reg_mask is accepted
 * for API compatibility but not checked.  Unlike the LP macros — which
 * only ever write scratch registers (d0/d1/a0/a1) plus a6 — this loads
 * the callee-saved images Dn[2..7]/An[2..6] as well, and those slots can
 * hold live state of a 68k frame further up (our thread entries are
 * TRAP_LIB gates called *from* the emulator, whose saved context is this
 * very frame).  The called function preserves them per the 68k ABI, so
 * restoring our snapshot afterwards makes the whole call as ABI-clean as
 * an LP macro invocation. */
#ifdef PLATFORM_MORPHOS
#include <emul/emulregs.h>

uint32_t platform_amiga_call(uint32_t lib_base, int16_t offset,
                             uint32_t *regs, uint16_t reg_mask)
{
    ULONG save_d[6], save_a[5];
    ULONG result;
    int i;
    (void)reg_mask;

    for (i = 0; i < 6; i++) save_d[i] = MyEmulHandle->Dn[2 + i];
    for (i = 0; i < 5; i++) save_a[i] = MyEmulHandle->An[2 + i];

    REG_D0 = regs[0];  REG_D1 = regs[1];
    REG_D2 = regs[2];  REG_D3 = regs[3];
    REG_D4 = regs[4];  REG_D5 = regs[5];
    REG_D6 = regs[6];  REG_D7 = regs[7];
    REG_A0 = regs[8];  REG_A1 = regs[9];
    REG_A2 = regs[10]; REG_A3 = regs[11];
    REG_A4 = regs[12]; REG_A5 = regs[13];
    REG_A6 = (ULONG)lib_base;

    /* offset is the (negative) LVO, exactly what EmulCallDirectOS takes
     * (the LP macros pass -offs for their positive offs). */
    result = (*MyEmulHandle->EmulCallDirectOS)((LONG)offset);

    for (i = 0; i < 6; i++) MyEmulHandle->Dn[2 + i] = save_d[i];
    for (i = 0; i < 5; i++) MyEmulHandle->An[2 + i] = save_a[i];

    return (uint32_t)result;
}
#endif

uint32_t platform_amiga_alloc_chip(uint32_t size)
{
    void *p;
    if (size == 0) return 0;
    p = AllocMem(size, MEMF_CHIP | MEMF_CLEAR);
    return (uint32_t)p;
}

void platform_amiga_free_chip(uint32_t addr, uint32_t size)
{
    if (addr == 0) return;
    FreeMem((void *)addr, size);
}

void platform_cache_clear(void *addr, uint32_t len)
{
    (void)addr; (void)len;
    /* CacheClearU flushes the full I and D caches.  Per-range
     * CacheClearE exists from V37 but takes more code and is no faster
     * on the buffer sizes the JIT produces.  Safe no-op on 68020/030
     * where the I-cache isn't write-back. */
    CacheClearU();
}
