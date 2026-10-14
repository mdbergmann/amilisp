/* This file DEFINES platform_alloc / platform_free, so it must opt out of
 * the DEBUG_MEM_TRACK macros in platform.h that redirect those names to the
 * leak tracer — otherwise the definitions themselves get renamed.  It calls
 * the allocator nowhere else, so nothing here goes untracked. */
#define CL_MEM_TRACK_IMPL 1

#include "platform.h"
#include "platform_thread.h"
#include "tls_openssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <poll.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#if defined(JIT_A64) && defined(__APPLE__)
#include <pthread.h>              /* pthread_jit_write_protect_np */
#include <libkern/OSCacheControl.h> /* sys_icache_invalidate */
#endif
#if defined(JIT_A64) && defined(__linux__)
#include <sys/syscall.h>          /* SYS_memfd_create */
#endif
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <glob.h>
#include <limits.h>
#include <dlfcn.h>
#include <pthread.h>
#include <ffi.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>   /* _NSGetExecutablePath (platform_executable_prefix) */
#endif

/* GC stop-the-world cooperation (defined in core/thread.c).  Forward-declared
 * here rather than #including core/thread.h so the platform layer stays free of
 * core/VM type dependencies.  A thread parked in a blocking socket syscall
 * cannot reach a GC safepoint; bracketing the syscall with these marks the
 * thread as "stopped" for the duration so a concurrent stop-the-world GC does
 * not deadlock waiting on it.  Both are no-ops on threads not registered with
 * the MP subsystem (e.g. the main thread before any mp:make-thread). */
extern void cl_gc_enter_safe_region(void);
extern void cl_gc_leave_safe_region(void);

void *platform_alloc(unsigned long size)
{
    void *p = malloc((size_t)size);
    if (p) {
        memset(p, 0, (size_t)size);
    }
    return p;
}

/* --- Page write-watch (generational GC dirty tracking) --------------- */

#include <sys/mman.h>
#include <signal.h>

uint32_t platform_page_size(void)
{
    long ps = sysconf(_SC_PAGESIZE);
    return (ps > 0) ? (uint32_t)ps : 4096u;
}

void *platform_alloc_pages(uint32_t size)
{
    void *p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;  /* mmap memory is zero-filled */
}

void platform_free_pages(void *ptr, uint32_t size)
{
    if (ptr)
        munmap(ptr, (size_t)size);
}

int platform_page_protect(uint8_t *addr, uint32_t len, int readonly)
{
    return mprotect(addr, (size_t)len,
                    readonly ? PROT_READ : (PROT_READ | PROT_WRITE));
}

/* Watch state.  Written only with the watch uninstalled or under the GC's
 * stop-the-world (install/remove happen at heap init/shutdown); read by
 * the fault handler on any thread. */
static uint8_t *volatile ww_base = NULL;
static uint32_t ww_len = 0;
static volatile uint8_t *ww_bitmap = NULL;
static uint32_t ww_pagesize = 0;
static int ww_installed = 0;
static struct sigaction ww_prev_segv, ww_prev_bus;

/* Chain a fault that is not ours to the previous disposition.  Restoring
 * the previous handler and re-raising from the faulting context makes the
 * default action (crash with the real si_addr) take effect. */
static void ww_chain(int sig, siginfo_t *si, void *uctx)
{
    struct sigaction *prev = (sig == SIGBUS) ? &ww_prev_bus : &ww_prev_segv;
    if ((prev->sa_flags & SA_SIGINFO) && prev->sa_sigaction) {
        prev->sa_sigaction(sig, si, uctx);
        return;
    }
    if (prev->sa_handler != SIG_DFL && prev->sa_handler != SIG_IGN &&
        prev->sa_handler) {
        prev->sa_handler(sig);
        return;
    }
    sigaction(sig, prev, NULL);
    raise(sig);
}

static void ww_handler(int sig, siginfo_t *si, void *uctx)
{
    uint8_t *addr = (uint8_t *)si->si_addr;
    uint8_t *base = ww_base;
    if (base && addr >= base && (uint32_t)(addr - base) < ww_len) {
        uint32_t page = (uint32_t)(addr - base) / ww_pagesize;
        /* Atomic: peer threads can fault on pages sharing the bitmap byte. */
        __sync_or_and_fetch((uint8_t *)&ww_bitmap[page >> 3],
                            (uint8_t)(1u << (page & 7u)));
        if (mprotect(base + (size_t)page * ww_pagesize, ww_pagesize,
                     PROT_READ | PROT_WRITE) == 0)
            return;  /* store retries and succeeds */
        /* mprotect failed inside a fault handler — nothing sane left. */
    }
    ww_chain(sig, si, uctx);
}

int platform_write_watch_install(uint8_t *base, uint32_t len,
                                 volatile uint8_t *dirty_bitmap)
{
    struct sigaction sa;

    ww_pagesize = platform_page_size();
    ww_bitmap = dirty_bitmap;
    ww_len = len;
    ww_base = base;

    if (!ww_installed) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = ww_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        /* Both signals: macOS reports write-protection faults as SIGBUS,
         * Linux as SIGSEGV. */
        if (sigaction(SIGSEGV, &sa, &ww_prev_segv) != 0) {
            ww_base = NULL;
            return -1;
        }
        if (sigaction(SIGBUS, &sa, &ww_prev_bus) != 0) {
            sigaction(SIGSEGV, &ww_prev_segv, NULL);
            ww_base = NULL;
            return -1;
        }
        ww_installed = 1;
    }
    return 0;
}

void platform_write_watch_remove(void)
{
    if (ww_installed) {
        sigaction(SIGSEGV, &ww_prev_segv, NULL);
        sigaction(SIGBUS, &ww_prev_bus, NULL);
        ww_installed = 0;
    }
    ww_base = NULL;
    ww_bitmap = NULL;
    ww_len = 0;
}

void platform_free(void *ptr)
{
    free(ptr);
}

void platform_write_string(const char *str)
{
    fputs(str, stdout);
    fflush(stdout);
}

void platform_flush_output(void)
{
    fflush(stdout);
}

int platform_read_line(char *buf, int bufsize)
{
    char *r;
    /* Blocking stdin read — bracket with a GC safe region exactly like the
     * socket syscalls below.  cl_repl() (and the SLDB/inspect prompts) park
     * here in fgets, often the main thread under a `tail -f /dev/null | amilisp`
     * launcher.  Without the bracket, a stop-the-world GC fired by any other
     * thread (e.g. a :spawn worker printing a backtrace into SLDB) waits
     * forever for this thread to reach a safepoint it can never reach. */
    cl_gc_enter_safe_region();
    r = fgets(buf, bufsize, stdin);
    cl_gc_leave_safe_region();
    if (!r)
        return 0;
    /* Strip trailing newline */
    {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
            buf[len - 1] = '\0';
    }
    return 1;
}

void platform_clear_stdin_eof(void)
{
    clearerr(stdin);
}

/* --- TTY control (raw mode / size / input availability) ---------------
 *
 * Raw mode switches console reads from stdio (getchar) to direct read(2)
 * with a private one-byte pushback.  This is what makes the LISTEN /
 * READ-CHAR-NO-HANG select() probe exact: getchar() reads ahead into
 * stdio's buffer, so select() on fd 0 would claim "nothing pending" while
 * the tail of an escape sequence sits in that buffer — stalling a TUI's
 * input decoder.  In cooked mode reads stay on stdio (no behavior change
 * for the REPL / piped-stdin paths, which mix fgets and getchar).
 *
 * Single-reader assumption: like the pre-existing getchar path, console
 * input is not safe for concurrent readers; a TUI reads keys from one
 * thread. */

static struct termios tty_saved_termios;
static int tty_saved_valid = 0;
static int tty_raw_active = 0;
static int tty_pushback = -1;

/* Crash insurance: never leave the user's shell in raw mode if the process
 * exits (cl_error longjmp escapes, ext:quit, crash-to-exit) while a TUI is
 * up.  Registered once on first raw-mode entry. */
static void tty_restore_at_exit(void)
{
    if (tty_raw_active && tty_saved_valid) {
        tcsetattr(STDIN_FILENO, TCSANOW, &tty_saved_termios);
        tty_raw_active = 0;
    }
}

int platform_tty_raw(int enable)
{
    static int atexit_registered = 0;
    struct termios t;

    if (!isatty(STDIN_FILENO))
        return -1;

    if (enable) {
        if (tty_raw_active)
            return 0;
        if (tcgetattr(STDIN_FILENO, &tty_saved_termios) != 0)
            return -1;
        tty_saved_valid = 1;
        t = tty_saved_termios;
        /* cfmakeraw-style, except OPOST stays on so '\n' still expands to
         * CR+LF — warnings and backtraces printed mid-TUI stay readable. */
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ECHOE | ECHOK | ECHONL |
                                 ISIG | IEXTEN);
        t.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                                 INLCR | IGNCR | ICRNL | IXON);
        t.c_cflag = (t.c_cflag & ~(tcflag_t)(CSIZE | PARENB)) | CS8;
        t.c_cc[VMIN] = 1;    /* block until at least one byte */
        t.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0)
            return -1;
        tty_raw_active = 1;
        if (!atexit_registered) {
            atexit(tty_restore_at_exit);
            atexit_registered = 1;
        }
        return 0;
    }

    if (!tty_raw_active)
        return 0;
    if (tty_saved_valid && tcsetattr(STDIN_FILENO, TCSANOW, &tty_saved_termios) != 0)
        return -1;
    tty_raw_active = 0;
    return 0;
}

int platform_tty_raw_active(void)
{
    return tty_raw_active;
}

/* Bytes stdio has already pulled off fd 0 but not yet handed out — the
 * cooked-mode reads (fgets / getchar) go through stdio, and select() on the
 * fd cannot see that buffer.  A pipe or file read slurps everything
 * available in one go, so after platform_read_line returned the first line
 * of a pasted multi-line form the rest sits exactly here.  (A tty in
 * canonical mode hands out one line per read, so there the type-ahead
 * stays in the kernel and select() finds it.)
 *
 * The FILE layout is libc-specific; the two we build on export it:
 * glibc's struct _IO_FILE (_IO_read_ptr/_IO_read_end) and the BSD/macOS
 * __sFILE (_r = bytes left for getc, ungetc included).  Elsewhere answer
 * "nothing", the conservative pre-existing behavior. */
static int stdin_stdio_buffered(void)
{
#if defined(__GLIBC__)
    return stdin->_IO_read_ptr < stdin->_IO_read_end;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
      defined(__OpenBSD__) || defined(__DragonFly__)
    return stdin->_r > 0;
#else
    return 0;
#endif
}

int platform_tty_char_avail(void)
{
    fd_set rfds;
    struct timeval tv;
    int r;
    if (tty_pushback != -1)
        return 1;
    /* Raw-mode reads bypass stdio, so its buffer is not what the next read
     * returns — only consult it in cooked mode. */
    if (!tty_raw_active && stdin_stdio_buffered())
        return 1;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    do {
        r = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    } while (r == -1 && errno == EINTR);
    return (r > 0) ? 1 : 0;
}

int platform_tty_size(int *cols, int *rows)
{
    struct winsize ws;
    /* stdout first (a TUI's frames go there), then stdin, then the
     * controlling terminal — stdio may be partially redirected. */
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 ||
         ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
    {
        int fd = open("/dev/tty", O_RDONLY);
        if (fd >= 0) {
            int ok = (ioctl(fd, TIOCGWINSZ, &ws) == 0 &&
                      ws.ws_col > 0 && ws.ws_row > 0);
            close(fd);
            if (ok) {
                *cols = ws.ws_col;
                *rows = ws.ws_row;
                return 0;
            }
        }
    }
    return -1;
}

int platform_getchar(void)
{
    int c;
    if (tty_pushback != -1) {
        c = tty_pushback;
        tty_pushback = -1;
        return c;
    }
    if (tty_raw_active) {
        /* Raw TUI regime: bypass stdio so the platform_tty_char_avail()
         * select() probe and this read agree on what is pending. */
        unsigned char b;
        ssize_t r;
        cl_gc_enter_safe_region();
        do {
            r = read(STDIN_FILENO, &b, 1);
        } while (r == -1 && errno == EINTR);
        cl_gc_leave_safe_region();
        return (r == 1) ? (int)b : -1;
    }
    /* Same blocking-stdin rationale as platform_read_line: the CONSOLE stream's
     * read-char parks here, so bracket it as a GC safe region. */
    cl_gc_enter_safe_region();
    c = getchar();
    cl_gc_leave_safe_region();
    return c;
}

void platform_ungetchar(int ch)
{
    if (tty_raw_active) {
        /* Raw reads bypass stdio, so ungetc()'s buffer would be invisible;
         * park the char in the platform pushback instead (getchar checks it
         * first in either mode, so a later cooked read still sees it). */
        tty_pushback = ch;
        return;
    }
    ungetc(ch, stdin);
}

void platform_drain_input(void)
{
    /* No-op on POSIX — stdin doesn't have residual CLI data */
}

int platform_stdin_is_interactive(void)
{
    return isatty(fileno(stdin)) ? 1 : 0;
}

char *platform_file_read(const char *path, unsigned long *size_out)
{
    FILE *f;
    long fsize;
    char *buf;

    *size_out = 0;
    f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) { fclose(f); return NULL; }

    buf = (char *)malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return NULL; }

    if (fread(buf, 1, (size_t)fsize, f) != (size_t)fsize) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[fsize] = '\0';
    *size_out = (unsigned long)fsize;
    fclose(f);
    return buf;
}

/* --- Handle-based file I/O --- */

#define PLATFORM_FILE_TABLE_SIZE 64

static FILE *file_table[PLATFORM_FILE_TABLE_SIZE];
static int file_table_init = 0;

/* Serialises slot claim (open) and slot release (close) so two threads can
 * never race a claim onto the same index or double-fclose one slot (mirrors
 * socket_table_mutex).  Per-byte read/write paths on a caller-owned slot run
 * unlocked, like the socket table.  Initialised on first use, which is
 * single-threaded (boot loads files before any worker thread exists). */
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
        for (i = 0; i < PLATFORM_FILE_TABLE_SIZE; i++)
            file_table[i] = NULL;
        platform_mutex_init(&file_table_mutex);
        file_table_init = 1;
    }
}

/* --- I/O buffer for sockets --- */

#define PLATFORM_IOBUF_SIZE 4096

typedef struct {
    char *rbuf;     /* read buffer (malloc'd) */
    int   rpos;     /* current read position */
    int   rlen;     /* valid bytes in read buffer */
    char *wbuf;     /* write buffer (malloc'd) */
    int   wlen;     /* pending bytes in write buffer */
} IOBuf;

static IOBuf *iobuf_alloc(void)
{
    IOBuf *b = (IOBuf *)malloc(sizeof(IOBuf));
    if (!b) return NULL;
    b->rbuf = (char *)malloc(PLATFORM_IOBUF_SIZE);
    b->wbuf = (char *)malloc(PLATFORM_IOBUF_SIZE);
    if (!b->rbuf || !b->wbuf) {
        free(b->rbuf);
        free(b->wbuf);
        free(b);
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
        free(b->rbuf);
        free(b->wbuf);
        free(b);
    }
}

/* ---- Platform file-I/O trace (AMILISP_IO_DIAG) ----
 * One stderr line as each file operation is ENTERED (before the blocking
 * call), so a hang inside the OS still shows which op/path/handle entered
 * it.  Mirrors the AmigaOS implementation (platform_amiga.c), where this
 * exists to catch stale-handle/garbage-path DOS waits; kept in lockstep
 * here so the diagnostic (and its test) runs on the host too.  Zero cost
 * when the env var is unset. */
#include <stdarg.h>
static volatile int32_t io_diag_cached = -2;  /* -2 unread; 0 off; 1 on */

static int io_diag_on(void)
{
    if (io_diag_cached == -2)
        io_diag_cached = (platform_getenv_s("AMILISP_IO_DIAG") &&
                          *platform_getenv_s("AMILISP_IO_DIAG")) ? 1 : 0;
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

PlatformFile platform_file_open(const char *path, int mode)
{
    FILE *f;
    const char *fmode;
    int i;

    file_table_ensure_init();
    io_diag("open \"%s\" mode=%d", path, mode);

    switch (mode) {
    case PLATFORM_FILE_READ:   fmode = "r";  break;
    case PLATFORM_FILE_WRITE:  fmode = "w";  break;
    case PLATFORM_FILE_APPEND: fmode = "a";  break;
    default: return PLATFORM_FILE_INVALID;
    }

    /* Copy the path to C memory, then bracket the fopen in a GC safe
     * region: open can block (network FS, spinning disk wake-up), and a
     * peer's stop-the-world GC would otherwise stall for its whole
     * duration.  The copy is mandatory — `path` may point into the
     * moving Lisp arena, and inside the safe region a peer compaction
     * can relocate it mid-syscall.  Oversized paths (>1023 bytes) fall
     * back to the unbracketed direct call (accepted STW stall). */
    {
        char pathbuf[1024];
        size_t plen = strlen(path);
        if (plen < sizeof(pathbuf)) {
            memcpy(pathbuf, path, plen + 1);
            cl_gc_enter_safe_region();
            f = fopen(pathbuf, fmode);
            cl_gc_leave_safe_region();
        } else {
            f = fopen(path, fmode);
        }
    }
    if (!f) return PLATFORM_FILE_INVALID;

    /* Find free slot (slot 0 is reserved as INVALID) — claim under the
     * table mutex so two concurrent opens never claim the same index. */
    file_table_lock();
    for (i = 1; i < PLATFORM_FILE_TABLE_SIZE; i++) {
        if (file_table[i] == NULL) {
            file_table[i] = f;
            file_table_unlock();
            io_diag("open -> fh=%d", i);
            return (PlatformFile)i;
        }
    }
    file_table_unlock();

    /* No free slots */
    fclose(f);
    return PLATFORM_FILE_INVALID;
}

void platform_file_close(PlatformFile fh)
{
    FILE *f = NULL;
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE) {
        /* Detach under the mutex (double-close protection: only one caller
         * observes the non-NULL slot), fclose outside it — the flush inside
         * fclose can block, and holding the table lock across it would
         * stall every concurrent open/close. */
        file_table_lock();
        f = file_table[fh];
        file_table[fh] = NULL;
        file_table_unlock();
    }
    if (f) {
        io_diag("close fh=%d", (int)fh);
        fclose(f);
    }
}

/* Per-character read/write and small buffered writes are deliberately NOT
 * bracketed in GC safe regions: they are hot paths (LOAD reads source
 * character by character) and stdio buffers them, so the underlying
 * syscall runs only every ~4KB and completes quickly on local disks.
 * The enter/leave pair costs a gc_mutex lock + condvar broadcast each —
 * per character that would dwarf the I/O itself.  A peer STW GC stalls
 * for at most one short buffered-syscall; accepted (mirrors the FFI
 * decision in the tier-4 audit). */
int platform_file_getchar(PlatformFile fh)
{
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh])
        return fgetc(file_table[fh]);
    return -1;
}

int platform_file_read_buf(PlatformFile fh, char *buf, uint32_t len)
{
    /* Bulk read for READ-SEQUENCE.  `buf` MUST be C memory (the stream layer
     * chunks arena-backed vectors through a C stack buffer): a large read
     * blocks for real, so it is bracketed in a GC safe region where a peer
     * compaction may run.  Small reads stay unbracketed like the per-char
     * path above (stdio buffers them). */
    size_t got;
    FILE *f;
    if (!(fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh]))
        return -1;
    f = file_table[fh];
    io_diag("read fh=%d len=%lu", (int)fh, (unsigned long)len);
    if (len <= 4096)
        return (int)fread(buf, 1, (size_t)len, f);
    cl_gc_enter_safe_region();
    got = fread(buf, 1, (size_t)len, f);
    cl_gc_leave_safe_region();
    return (int)got;
}

int platform_file_write_string(PlatformFile fh, const char *str)
{
    return platform_file_write_buf(fh, str, (uint32_t)strlen(str));
}

int platform_file_write_char(PlatformFile fh, int ch)
{
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh])
        return fputc(ch, file_table[fh]) != EOF ? 0 : -1;
    return -1;
}

int platform_file_write_buf(PlatformFile fh, const char *buf, uint32_t len)
{
    /* Large writes (FASL emission, WRITE-SEQUENCE) can block for real —
     * bracket them.  `buf` may point into the moving arena, so each
     * chunk is copied to the C stack BEFORE entering the safe region
     * (inside it a peer compaction can move the source).  Small writes
     * (< one chunk) stay unbracketed like the per-char path above. */
    char chunk[4096];
    uint32_t pos = 0;
    FILE *f;
    if (!(fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh]))
        return -1;
    f = file_table[fh];
    io_diag("write fh=%d len=%lu", (int)fh, (unsigned long)len);
    if (len <= sizeof(chunk))
        return (fwrite(buf, 1, (size_t)len, f) == (size_t)len) ? 0 : -1;
    while (pos < len) {
        uint32_t nb = len - pos;
        size_t written;
        if (nb > sizeof(chunk)) nb = (uint32_t)sizeof(chunk);
        memcpy(chunk, buf + pos, nb);
        cl_gc_enter_safe_region();
        written = fwrite(chunk, 1, (size_t)nb, f);
        cl_gc_leave_safe_region();
        if (written != (size_t)nb) return -1;
        pos += nb;
    }
    /* NOTE: `buf` is read across safe regions above, so a >chunk-sized
     * write requires C-memory input.  That holds for every current
     * caller: the FASL writer passes a platform_alloc'd buffer, and the
     * stream layer chunks arena-backed strings through rooted CL_Objs
     * (cl_stream_write_lisp_string) before reaching this level. */
    return 0;
}

int platform_file_flush(PlatformFile fh)
{
    int rc;
    if (!(fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh]))
        return -1;
    /* Flush pushes up to a full stdio buffer to disk — bracket it. */
    cl_gc_enter_safe_region();
    rc = (fflush(file_table[fh]) == 0) ? 0 : -1;
    cl_gc_leave_safe_region();
    return rc;
}

int platform_file_eof(PlatformFile fh)
{
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh])
        return feof(file_table[fh]) ? 1 : 0;
    return 1;
}

long platform_file_position(PlatformFile fh)
{
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh])
        return ftell(file_table[fh]);
    return -1;
}

int platform_file_set_position(PlatformFile fh, long pos)
{
    int rc;
    if (!(fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh]))
        return -1;
    /* A seek can flush buffered writes to disk — bracket it (no arena
     * data is touched inside). */
    cl_gc_enter_safe_region();
    rc = fseek(file_table[fh], pos, SEEK_SET) == 0 ? 0 : -1;
    cl_gc_leave_safe_region();
    return rc;
}

long platform_file_length(PlatformFile fh)
{
    if (fh > 0 && fh < PLATFORM_FILE_TABLE_SIZE && file_table[fh]) {
        long cur, end;
        FILE *f = file_table[fh];
        cl_gc_enter_safe_region();
        cur = ftell(f);
        fseek(f, 0, SEEK_END);
        end = ftell(f);
        fseek(f, cur, SEEK_SET);
        cl_gc_leave_safe_region();
        return end;
    }
    return -1;
}

uint32_t platform_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
}

uint64_t platform_time_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

uint32_t platform_run_time_ms(void)
{
    /* getrusage over clock(): clock() wraps after ~72 minutes of CPU at
     * CLOCKS_PER_SEC=1000000 on 32-bit. */
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) == 0) {
        return (uint32_t)((ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000
                          + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000);
    }
    return platform_time_ms();
}

void platform_sleep_ms(uint32_t milliseconds)
{
    if (milliseconds > 0) {
        /* A sleeping thread cannot reach a safepoint — without the safe
         * region every peer's stop-the-world GC stalls for the whole
         * nap (SLEEP naps in 100ms chunks; MP polling loops in 10ms).
         * Safe regions nest (safe_region_depth), so callers already
         * inside one are fine. */
        cl_gc_enter_safe_region();
        usleep(milliseconds * 1000);
        cl_gc_leave_safe_region();
    }
}

uint32_t platform_universal_time(void)
{
    /* CL universal time: seconds since 1900-01-01 00:00:00 UTC
     * Unix epoch: 1970-01-01 00:00:00 UTC
     * Difference: 70 years = 2208988800 seconds */
    time_t t = time(NULL);
    return (uint32_t)((unsigned long)t + 2208988800UL);
}

int platform_file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

int platform_file_is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int platform_file_delete(const char *path)
{
    return (unlink(path) == 0) ? 0 : -1;
}

int platform_file_rename(const char *oldpath, const char *newpath)
{
    return (rename(oldpath, newpath) == 0) ? 0 : -1;
}

int platform_file_make_executable(const char *path)
{
    /* x wherever the file is readable: what the umask allowed at creation
     * decides who may run it, as cc does for its output. */
    struct stat st;
    mode_t mode;
    if (stat(path, &st) != 0)
        return -1;
    mode = st.st_mode & 07777;
    mode |= (mode & 0444) >> 2;
    return chmod(path, mode) == 0 ? 0 : -1;
}

uint32_t platform_file_mtime(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    /* Convert Unix time_t to CL universal time */
    return (uint32_t)((unsigned long)st.st_mtime + 2208988800UL);
}

int platform_mkdir(const char *path)
{
    return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

const char *platform_getenv_raw(const char *name, char *buf, int bufsize)
{
    (void)buf; (void)bufsize;
    return getenv(name);
}

/* No system-wide figure worth reporting: the kernel reclaims a process's
 * memory at exit, so an unfreed block here is a bug for valgrind/ASan to
 * find, not RAM the machine loses.  See platform.h. */
unsigned long platform_mem_available(void)
{
    return 0;
}

/* Nothing to take back: the kernel closes every descriptor and reclaims the
 * whole address space when the process ends.  See platform.h. */
void platform_release_resources(void)
{
}

/* The executable's resolved absolute path into `resolved` (PATH_MAX bytes);
 * 0 when the platform cannot tell. */
static int executable_resolved(char *resolved)
{
#if defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t rawsz = (uint32_t)sizeof(raw);
    if (_NSGetExecutablePath(raw, &rawsz) != 0)
        return 0;
    if (!realpath(raw, resolved))
        return 0;
    return 1;
#elif defined(__linux__)
    ssize_t n = readlink("/proc/self/exe", resolved, PATH_MAX - 1);
    if (n <= 0)
        return 0;
    resolved[n] = '\0';
    return 1;
#else
    /* No portable way to find the executable on other POSIX systems —
     * callers fall back to cwd-relative lookup and $AMILISP_HOME. */
    (void)resolved;
    return 0;
#endif
}

const char *platform_executable_prefix(char *buf, int bufsize)
{
    char resolved[PATH_MAX];
    if (!executable_resolved(resolved))
        return NULL;
    /* Strip the executable name, keep the trailing slash */
    {
        char *slash = strrchr(resolved, '/');
        if (!slash)
            return NULL;
        slash[1] = '\0';
    }
    if ((int)strlen(resolved) >= bufsize)
        return NULL;
    strcpy(buf, resolved);
    return buf;
}

const char *platform_executable_path(char *buf, int bufsize)
{
    char resolved[PATH_MAX];
    if (!executable_resolved(resolved))
        return NULL;
    if ((int)strlen(resolved) >= bufsize)
        return NULL;
    strcpy(buf, resolved);
    return buf;
}

uint32_t platform_cpu_store_selftest(uint32_t rounds)
{
    (void)rounds;
    return 0;                           /* the m68k build has the test */
}

long platform_stack_headroom(void)
{
    /* No portable exact answer on POSIX; the generic 3MB usage budget in
     * cl_check_c_stack plus the OS guard pages cover the host. */
    return -1;
}

int platform_executable_ancestor_prefix(int levels, char *buf, int bufsize)
{
    int i;
    if (!platform_executable_prefix(buf, bufsize))
        return 0;
    for (i = 0; i < levels; i++) {
        if ((int)(strlen(buf) + 3) >= bufsize)
            return 0;
        strcat(buf, "../");
    }
    return 1;
}

int platform_getcwd(char *buf, int bufsize)
{
    if (getcwd(buf, (size_t)bufsize) != NULL)
        return (int)strlen(buf);
    return 0;
}

int platform_startup_args(int *argc, char ***argv)
{
    /* A POSIX process always has its command line. */
    (void)argc; (void)argv;
    return 0;
}

int platform_run_main(int (*fn)(int, char **), int argc, char **argv)
{
    /* The main thread's stack is 8 MB or more here. */
    return fn(argc, argv);
}

void platform_process_exit(int code)
{
    fflush(NULL);
    _exit(code);
}

int platform_system(const char *command)
{
    int status;
    /* `command` typically points straight into a Lisp string's arena data
     * (EXT:SYSTEM-COMMAND passes s->data), and system() blocks for the
     * child's whole runtime — both reasons to copy to C memory FIRST and
     * only then enter the safe region (inside it a peer compaction can
     * relocate the source).  OOM fallback: run unbracketed from the
     * original pointer (accepted STW stall, no relocation risk). */
    size_t clen = strlen(command);
    char *cmdbuf = (char *)malloc(clen + 1);
    if (cmdbuf) {
        memcpy(cmdbuf, command, clen + 1);
        cl_gc_enter_safe_region();
        status = system(cmdbuf);
        cl_gc_leave_safe_region();
        free(cmdbuf);
    } else {
        status = system(command);
    }
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

char **platform_directory(const char *pattern, int *count_out)
{
    glob_t g;
    char **result;
    int i;
    char patbuf[1024];
    size_t plen;
    int rc;

    *count_out = 0;
    /* Copy the pattern to C memory and bracket the glob: directory
     * enumeration touches the disk (possibly a network FS) and the
     * pattern may point into the moving arena.  Oversized patterns run
     * unbracketed from the original pointer (accepted STW stall). */
    plen = strlen(pattern);
    /* GLOB_MARK appends '/' to directory entries so callers can
       distinguish directories from files (needed for CL DIRECTORY). */
    if (plen < sizeof(patbuf)) {
        memcpy(patbuf, pattern, plen + 1);
        cl_gc_enter_safe_region();
        rc = glob(patbuf, GLOB_MARK, NULL, &g);
        cl_gc_leave_safe_region();
    } else {
        rc = glob(pattern, GLOB_MARK, NULL, &g);
    }
    if (rc != 0) {
        return NULL;
    }
    result = (char **)malloc(((size_t)g.gl_pathc + 1) * sizeof(char *));
    if (!result) { globfree(&g); return NULL; }
    for (i = 0; i < (int)g.gl_pathc; i++) {
        result[i] = strdup(g.gl_pathv[i]);
    }
    result[g.gl_pathc] = NULL;
    *count_out = (int)g.gl_pathc;
    globfree(&g);
    return result;
}

const char *platform_realpath(const char *path, char *buf, int bufsize)
{
    char resolved[PATH_MAX];
    if (!realpath(path, resolved)) return NULL;
    strncpy(buf, resolved, (size_t)bufsize - 1);
    buf[bufsize - 1] = '\0';
    return buf;
}

const char *platform_expand_home(const char *path, char *buf, int bufsize)
{
    const char *home;
    size_t hlen, plen;

    if (!path || path[0] != '~') return path;
    /* Only expand ~/... or bare ~ (not ~user) */
    if (path[1] != '\0' && path[1] != '/') return path;

    home = getenv("HOME");
    if (!home) home = "/";
    hlen = strlen(home);
    plen = strlen(path + 1); /* everything after ~ */

    if ((int)(hlen + plen + 1) > bufsize) return path;
    memcpy(buf, home, hlen);
    /* path+1 is either "" or "/rest..." — copy including NUL */
    memcpy(buf + hlen, path + 1, plen + 1);
    return buf;
}

/* --- TCP Socket I/O ---
 *
 * Unlike the Amiga reactor (a fixed 64-slot table bounded by bsdsocket's
 * descriptor table), the POSIX host has no such ceiling — a busy server can
 * hold thousands of connections — so the socket table grows on demand.
 *
 * A PlatformSocket handle IS a slot index handed to the Lisp layer, so a slot
 * must never move or be renumbered once allocated.  That rules out a realloc'd
 * flat array (it would move the backing store out from under the lock-free
 * read/write paths).  Instead this is a segmented (two-level) table: a fixed
 * directory of block pointers, each block lazily allocated and published once.
 * Resolving a handle is two indexed loads of immutable data, so the hot
 * read/write path stays lock-free; only slot claim/free take the mutex.
 * Slot 0 is the reserved INVALID handle. */
typedef struct {
    int    fd;          /* -1 = free */
    IOBuf *buf;         /* NULL for listeners and the no-buffer fallback */
    int    rtimeout;    /* read timeout in ms; 0 = block indefinitely */
    int    wtimeout;    /* write timeout in ms; 0 = block indefinitely */
    int    next_free;   /* free-list link; valid only while fd < 0, 0 = end */
    TLSConn *tls;       /* non-NULL once platform_tls_start upgraded the
                         * connection: refill/drain/probe route through the
                         * TLS record layer instead of raw read()/send() */
} SockSlot;

#define SOCK_BLOCK_SHIFT 6
#define SOCK_BLOCK_SIZE  (1 << SOCK_BLOCK_SHIFT)   /* 64 slots per block */
#define SOCK_BLOCK_MASK  (SOCK_BLOCK_SIZE - 1)
#define SOCK_MAX_BLOCKS  1024                       /* up to 65536 sockets */

static SockSlot * volatile socket_dir[SOCK_MAX_BLOCKS]; /* NULL until allocated */
static int       socket_nblocks   = 0;              /* blocks allocated so far */
static int       socket_free_head = 0;              /* free-list head, 0 = empty */
static int       socket_table_init = 0;

/* Serialises slot claim (connect/listen/accept), slot free (close), and block
 * growth so a threaded server — e.g. an accept loop on one thread while
 * another connects — can never race two claims onto the same index.  Only the
 * table mutation is guarded; the blocking syscalls (connect/accept/flush/close)
 * and the per-byte read/write paths (each on its own caller-owned slot) run
 * unlocked.  Initialised on first use, which is single-threaded. */
static void *socket_table_mutex = NULL;

static void socket_table_lock(void)
{
    if (socket_table_mutex) platform_mutex_lock(socket_table_mutex);
}

static void socket_table_unlock(void)
{
    if (socket_table_mutex) platform_mutex_unlock(socket_table_mutex);
}

/* Resolve a handle to its slot, or NULL if it names a never-allocated slot.
 * Reads only published (immutable) directory pointers — the block pointer is
 * stored last in socket_grow_locked, after the block is fully initialised, and
 * is never freed or changed — so this is safe to call from the lock-free
 * read/write paths. */
static SockSlot *sock_slot(PlatformSocket sh)
{
    unsigned blk = (unsigned)sh >> SOCK_BLOCK_SHIFT;
    SockSlot *b;
    if (blk >= (unsigned)SOCK_MAX_BLOCKS) return NULL;
    b = socket_dir[blk];
    if (!b) return NULL;
    return &b[(unsigned)sh & SOCK_BLOCK_MASK];
}

/* Allocate one more block and push its slots onto the free-list (in ascending
 * order so low indices are handed out first).  Slot 0 — the INVALID handle — is
 * reserved and never linked.  Caller holds the lock.  Returns 1 on success, 0
 * if the directory is full or out of memory. */
static int socket_grow_locked(void)
{
    SockSlot *blk;
    int base, start, i;
    if (socket_nblocks >= SOCK_MAX_BLOCKS) return 0;
    blk = (SockSlot *)calloc(SOCK_BLOCK_SIZE, sizeof(SockSlot));
    if (!blk) return 0;
    base  = socket_nblocks << SOCK_BLOCK_SHIFT;
    start = (base == 0) ? 1 : 0;          /* reserve global slot 0 */
    if (base == 0) blk[0].fd = -1;        /* slot 0: never claimed, mark invalid */
    for (i = SOCK_BLOCK_SIZE - 1; i >= start; i--) {
        blk[i].fd = -1;
        blk[i].next_free = socket_free_head;
        socket_free_head = base + i;
    }
    /* Publish the fully-initialised block last so a lock-free reader never
     * observes a directory pointer to uninitialised slots. */
    socket_dir[socket_nblocks] = blk;
    socket_nblocks++;
    return 1;
}

/* Claim a free slot for fd, allocating an IOBuf when want_buf (NULL for
 * listeners).  Caller holds the lock.  Returns the slot index, or 0 (INVALID)
 * if the table can't grow or the IOBuf alloc fails. */
static PlatformSocket socket_claim_locked(int fd, int want_buf)
{
    int idx;
    SockSlot *s;
    if (socket_free_head == 0 && !socket_grow_locked())
        return PLATFORM_SOCKET_INVALID;
    idx = socket_free_head;
    s = sock_slot((PlatformSocket)idx);
    if (want_buf) {
        IOBuf *b = iobuf_alloc();
        if (!b) return PLATFORM_SOCKET_INVALID;   /* leave slot on free-list */
        s->buf = b;
    } else {
        s->buf = NULL;
    }
    socket_free_head = s->next_free;   /* pop only after the buf alloc succeeds */
    s->fd = fd;
    s->rtimeout = 0;
    s->wtimeout = 0;
    s->next_free = 0;
    s->tls = NULL;
    return (PlatformSocket)idx;
}

static void socket_table_ensure_init(void)
{
    if (!socket_table_init) {
        platform_mutex_init(&socket_table_mutex);
        socket_table_init = 1;
    }
}

/* Defined below; used here to bound a non-blocking connect by its deadline. */
static int socket_wait_ready(int fd, int timeout_ms, int want_write);

PlatformSocket platform_socket_connect(const char *host, int port, int connect_ms)
{
    struct hostent *he;
    struct sockaddr_in addr;
    int fd;
    char host_buf[256];

    socket_table_ensure_init();

    /* Copy the hostname to the C stack first: host may point into the
     * Lisp arena, and the safe region below lets a peer thread's
     * compaction move it while we're parked in DNS — resolving a garbage
     * hostname.  (The Amiga implementation stack-copies for exactly this
     * reason.) */
    {
        size_t hl = strlen(host);
        if (hl >= sizeof(host_buf)) return PLATFORM_SOCKET_INVALID;
        memcpy(host_buf, host, hl + 1);
    }

    /* DNS resolution can block on the network — stay GC-cooperative. */
    cl_gc_enter_safe_region();
    he = gethostbyname(host_buf);
    cl_gc_leave_safe_region();
    if (!he) return PLATFORM_SOCKET_INVALID;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return PLATFORM_SOCKET_INVALID;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect_ms > 0) {
        /* Bounded connect: go non-blocking, fire the handshake, then poll for
         * writability up to connect_ms.  Writable + SO_ERROR==0 means connected;
         * a timeout or a connect error both fail fast.  Restore blocking mode so
         * the rest of the stream code (which read()s blocking) is unaffected. */
        int flags = fcntl(fd, F_GETFL, 0);
        int rc, ok = 0;
        if (flags < 0) { close(fd); return PLATFORM_SOCKET_INVALID; }
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        cl_gc_enter_safe_region();
        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (rc == 0) {
            ok = 1;                                  /* connected immediately */
        } else if (errno == EINPROGRESS) {
            if (socket_wait_ready(fd, connect_ms, 1) == 1) {
                int err = 0;
                socklen_t elen = (socklen_t)sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0)
                    ok = 1;
            }
        }
        cl_gc_leave_safe_region();
        fcntl(fd, F_SETFL, flags);
        if (!ok) {
            close(fd);
            return PLATFORM_SOCKET_INVALID;
        }
    } else {
        int rc;
        cl_gc_enter_safe_region();
        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        cl_gc_leave_safe_region();
        if (rc < 0) {
            close(fd);
            return PLATFORM_SOCKET_INVALID;
        }
    }

    /* Claim a slot (slot 0 reserved as INVALID); the table grows on demand. */
    {
        PlatformSocket sh;
        socket_table_lock();
        sh = socket_claim_locked(fd, 1);
        socket_table_unlock();
        if (sh == PLATFORM_SOCKET_INVALID) {
            close(fd);
            return PLATFORM_SOCKET_INVALID;
        }
        return sh;
    }
}

/* Flush socket write buffer to the wire */
/* Forward declaration: readiness wait shared by read and write paths. */
static int socket_wait_ready(int fd, int timeout_ms, int want_write);

static int socket_flush_wbuf(PlatformSocket sh)
{
    IOBuf *b;
    int fd, wtimeout;
    SockSlot *s = sock_slot(sh);
    ssize_t total = 0;
    if (sh == 0 || !s) return -1;
    b = s->buf;
    if (!b || b->wlen == 0) return 0;
    fd = s->fd;
    wtimeout = s->wtimeout;
    if (s->tls) {
        /* TLS drain: tls_conn_write handles its own poll()-based waits and
         * deadline; it parks, so bracket it as a GC safe region like the
         * plain send() loop below. */
        int wr;
        TLSConn *t = s->tls;
        cl_gc_enter_safe_region();
        wr = tls_conn_write(t, b->wbuf, (uint32_t)b->wlen, wtimeout);
        cl_gc_leave_safe_region();
        if (wr != 0) return wr;         /* -1 error / -2 timeout */
        b->wlen = 0;
        return 0;
    }
    /* write() can block when the peer's receive window is full — bracket the
     * whole drain loop so a slow reader cannot stall a stop-the-world GC. */
    cl_gc_enter_safe_region();
    if (wtimeout > 0) {
        /* Timed drain.  A blocking write() would stall until ALL bytes fit even
         * after select() reports only a sliver of space, so use non-blocking
         * send() + select() instead.  All waits share ONE absolute deadline:
         * select() can spuriously report a loopback socket writable while
         * send() still returns EAGAIN, so a per-call timeout could spin forever
         * — the shared deadline guarantees the loop terminates within wtimeout. */
        uint32_t deadline = platform_time_ms() + (uint32_t)wtimeout;
        while (total < b->wlen) {
            ssize_t n = send(fd, b->wbuf + total, (size_t)(b->wlen - total), MSG_DONTWAIT);
            if (n > 0) { total += n; continue; }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                int32_t rem = (int32_t)(deadline - platform_time_ms());
                int rr;
                if (rem <= 0) { cl_gc_leave_safe_region(); return PLATFORM_SOCKET_TIMEOUT; }
                rr = socket_wait_ready(fd, rem, 1);
                if (rr < 0)  { cl_gc_leave_safe_region(); return -1; }
                if (rr == 0) { cl_gc_leave_safe_region(); return PLATFORM_SOCKET_TIMEOUT; }
                continue;
            }
            cl_gc_leave_safe_region();
            return -1;   /* n == 0 or a real send() error */
        }
    } else {
        while (total < b->wlen) {
            ssize_t n = write(fd, b->wbuf + total, (size_t)(b->wlen - total));
            if (n <= 0) { cl_gc_leave_safe_region(); return -1; }
            total += n;
        }
    }
    cl_gc_leave_safe_region();
    b->wlen = 0;
    return 0;
}

void platform_socket_close(PlatformSocket sh)
{
    SockSlot *s = sock_slot(sh);
    if (sh > 0 && s && s->fd >= 0) {
        int fd;
        IOBuf *buf;
        TLSConn *tls;
        socket_flush_wbuf(sh);
        /* Detach the slot and return it to the free-list under the lock, then
         * do the blocking close() and free() outside it. */
        socket_table_lock();
        fd = s->fd;
        buf = s->buf;
        tls = s->tls;
        s->fd = -1;
        s->buf = NULL;
        s->tls = NULL;
        s->rtimeout = 0;
        s->wtimeout = 0;
        s->next_free = socket_free_head;
        socket_free_head = (int)sh;
        socket_table_unlock();
        /* close_notify before close(fd) — the shutdown record still needs
         * the fd; the attempt is non-blocking and best-effort. */
        if (tls) tls_conn_close(tls, 1);
        if (fd >= 0) close(fd);
        iobuf_free(buf);
    }
}

void platform_socket_close_gc(PlatformSocket sh)
{
    /* As platform_socket_close, but no socket_flush_wbuf — its drain loop
     * brackets a GC safe region, which must not run during the sweep.  A
     * plain close() is fast and safe-region-free.  The TLS teardown skips
     * the close_notify for the same reason (no I/O during the sweep). */
    SockSlot *s = sock_slot(sh);
    if (sh > 0 && s && s->fd >= 0) {
        int fd;
        IOBuf *buf;
        TLSConn *tls;
        socket_table_lock();
        fd = s->fd;
        buf = s->buf;
        tls = s->tls;
        s->fd = -1;
        s->buf = NULL;
        s->tls = NULL;
        s->rtimeout = 0;
        s->wtimeout = 0;
        s->next_free = socket_free_head;
        socket_free_head = (int)sh;
        socket_table_unlock();
        if (tls) tls_conn_close(tls, 0);
        if (fd >= 0) close(fd);
        iobuf_free(buf);
    }
}

void platform_socket_set_timeout(PlatformSocket sh, int read_ms, int write_ms)
{
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s) return;
    socket_table_lock();
    s->rtimeout = (read_ms  > 0) ? read_ms  : 0;
    s->wtimeout = (write_ms > 0) ? write_ms : 0;
    socket_table_unlock();
}

/* Wait until `fd` is readable (want_write==0) or writable (want_write==1),
 * up to timeout_ms.  Returns 1 ready, 0 timed out, -1 error/invalid.  The
 * select() blocks, so the caller must already be in a GC safe region.
 * An absolute deadline is computed from timeout_ms so that EINTR retries
 * consume elapsed time rather than resetting the full timeout each iteration. */
static int socket_wait_ready(int fd, int timeout_ms, int want_write)
{
    struct pollfd pfd;
    int r;
    short want;
    uint32_t deadline;
    int32_t rem;
    /* poll() has no FD_SETSIZE ceiling, so a host server holding thousands of
     * connections (fd numbers well above 1024) still works — select() does not. */
    if (fd < 0) return -1;
    want = want_write ? POLLOUT : POLLIN;
    deadline = platform_time_ms() + (uint32_t)timeout_ms;
    for (;;) {
        rem = (int32_t)(deadline - platform_time_ms());
        if (rem <= 0) return 0;             /* deadline elapsed */
        pfd.fd = fd;
        pfd.events = want;
        pfd.revents = 0;
        r = poll(&pfd, 1, rem);
        if (r < 0) {
            if (errno == EINTR) continue;   /* retry, deadline unchanged */
            return -1;
        }
        if (r == 0) return 0;               /* timed out */
        /* Report ready on the wanted event, or on an error/hangup condition so
         * the caller's send()/recv() surfaces the real result. */
        return (pfd.revents & (want | POLLERR | POLLHUP | POLLNVAL)) ? 1 : 0;
    }
}

int platform_socket_read(PlatformSocket sh)
{
    IOBuf *b;
    int rtimeout;
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd < 0)
        return -1;
    b = s->buf;
    rtimeout = s->rtimeout;
    if (b) {
        if (b->rpos < b->rlen)
            return (unsigned char)b->rbuf[b->rpos++];
        /* TLS refill: tls_conn_read waits, decrypts, and honors the read
         * timeout itself (-2).  It parks in poll(), so bracket it as a GC
         * safe region like the plain read() below. */
        if (s->tls) {
            TLSConn *t = s->tls;
            int n;
            cl_gc_enter_safe_region();
            n = tls_conn_read(t, b->rbuf, PLATFORM_IOBUF_SIZE, rtimeout);
            cl_gc_leave_safe_region();
            if (n == -2) return PLATFORM_SOCKET_TIMEOUT;
            if (n <= 0) return -1;      /* clean close_notify or error: EOF */
            b->rpos = 1;
            b->rlen = n;
            return (unsigned char)b->rbuf[0];
        }
        /* Refill read buffer.  The read() blocks until data arrives, so bracket
         * it as a GC safe region; capture the fd first since a concurrent close
         * could clear the slot while we are parked. */
        {
            int fd = s->fd;
            ssize_t n;
            cl_gc_enter_safe_region();
            if (rtimeout > 0) {
                int rr = socket_wait_ready(fd, rtimeout, 0);
                if (rr <= 0) {
                    cl_gc_leave_safe_region();
                    return (rr == 0) ? PLATFORM_SOCKET_TIMEOUT : -1;
                }
            }
            n = read(fd, b->rbuf, PLATFORM_IOBUF_SIZE);
            cl_gc_leave_safe_region();
            if (n <= 0) return -1;
            b->rpos = 1;
            b->rlen = (int)n;
            return (unsigned char)b->rbuf[0];
        }
    }
    /* Fallback: no buffer */
    {
        int fd = s->fd;
        unsigned char byte;
        ssize_t n;
        if (s->tls) {
            TLSConn *t = s->tls;
            int tn;
            cl_gc_enter_safe_region();
            tn = tls_conn_read(t, (char *)&byte, 1, rtimeout);
            cl_gc_leave_safe_region();
            if (tn == -2) return PLATFORM_SOCKET_TIMEOUT;
            if (tn <= 0) return -1;
            return (int)byte;
        }
        cl_gc_enter_safe_region();
        if (rtimeout > 0) {
            int rr = socket_wait_ready(fd, rtimeout, 0);
            if (rr <= 0) {
                cl_gc_leave_safe_region();
                return (rr == 0) ? PLATFORM_SOCKET_TIMEOUT : -1;
            }
        }
        n = read(fd, &byte, 1);
        cl_gc_leave_safe_region();
        if (n <= 0) return -1;
        return (int)byte;
    }
}

/* Non-blocking readiness probe, used to back CL:LISTEN on socket streams.
 * Returns:
 *    1  data is available — a read returns a byte now (or, for a listener
 *       slot, a client connection is pending and accept() won't block);
 *    0  a read/accept would block (nothing ready yet);
 *    2  at end of file — the peer closed and a read returns EOF immediately;
 *   -1  invalid handle.
 * Uses select() with a zero timeout (never blocks, no GC safe region needed).
 * select() reports a half-closed peer as "readable", so for a connection
 * socket we MSG_PEEK one byte to tell real data (1) from EOF (2); a listener
 * has no IOBuf and is never peeked (recv on a listen fd is invalid) — a
 * readable listener always means a pending connection. */
int platform_socket_data_available(PlatformSocket sh)
{
    IOBuf *b;
    int fd;
    struct pollfd pfd;
    int r;
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd < 0)
        return -1;
    b = s->buf;
    if (b && b->rpos < b->rlen)
        return 1;                       /* already-buffered bytes */
    /* TLS: decrypted bytes may already sit inside the record layer with the
     * fd itself idle — check before polling.  A readable fd is then only a
     * hint (the bytes could be handshake traffic or a partial record), so
     * a non-blocking SSL_peek gives the real LISTEN answer. */
    if (s->tls) {
        TLSConn *t = s->tls;
        if (tls_conn_pending(t) > 0)
            return 1;
        fd = s->fd;
        if (fd < 0) return -1;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        r = poll(&pfd, 1, 0);
        if (r < 0) return (errno == EINTR) ? 0 : -1;
        if (r == 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
            return 0;
        return tls_conn_probe(t);       /* 1 data / 0 not yet / 2 EOF */
    }
    fd = s->fd;
    if (fd < 0)
        return -1;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, 0);               /* zero timeout: never blocks */
    if (r < 0) {
        if (errno == EINTR) return 0;   /* treat as "not ready yet" */
        return -1;
    }
    if (r == 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
        return 0;                       /* would block */
    if (!b)
        return 1;                       /* listener: connection pending */
    /* Connection socket is readable: distinguish data from a closed peer. */
    {
        char peek;
        ssize_t pn = recv(fd, &peek, 1, MSG_PEEK);
        if (pn > 0)  return 1;          /* real data waiting */
        if (pn == 0) return 2;          /* peer closed => EOF */
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)
            return 0;                   /* spurious wakeup */
        return 2;                       /* error => report as EOF-ish */
    }
}

int platform_socket_write(PlatformSocket sh, int byte)
{
    IOBuf *b;
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd < 0)
        return -1;
    b = s->buf;
    if (b) {
        b->wbuf[b->wlen++] = (char)byte;
        if (b->wlen >= PLATFORM_IOBUF_SIZE)
            return socket_flush_wbuf(sh);
        return 0;
    }
    /* Fallback: no buffer */
    {
        int fd = s->fd;
        unsigned char bb = (unsigned char)byte;
        ssize_t n;
        if (s->tls) {
            TLSConn *t = s->tls;
            int wr;
            cl_gc_enter_safe_region();
            wr = tls_conn_write(t, (const char *)&bb, 1, s->wtimeout);
            cl_gc_leave_safe_region();
            return wr;
        }
        cl_gc_enter_safe_region();
        n = write(fd, &bb, 1);
        cl_gc_leave_safe_region();
        return (n == 1) ? 0 : -1;
    }
}

int platform_socket_write_buf(PlatformSocket sh, const char *buf, uint32_t len)
{
    IOBuf *b;
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd < 0)
        return -1;
    b = s->buf;
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
                int fr = socket_flush_wbuf(sh);
                if (fr != 0) return fr;     /* propagate -1 (error) or -2 (timeout) */
            }
        }
        return 0;
    }
    /* Fallback: direct write */
    {
        ssize_t total = 0;
        int fd = s->fd;
        if (s->tls) {
            TLSConn *t = s->tls;
            int wr;
            cl_gc_enter_safe_region();
            wr = tls_conn_write(t, buf, len, s->wtimeout);
            cl_gc_leave_safe_region();
            return wr;
        }
        cl_gc_enter_safe_region();
        while ((uint32_t)total < len) {
            ssize_t n = write(fd, buf + total, (size_t)(len - (uint32_t)total));
            if (n <= 0) { cl_gc_leave_safe_region(); return -1; }
            total += n;
        }
        cl_gc_leave_safe_region();
        return 0;
    }
}

int platform_socket_flush(PlatformSocket sh)
{
    return socket_flush_wbuf(sh);
}

/* "a.b.c.d" -> the address in host byte order; 0 for anything else.  A
 * parser of our own rather than inet_addr(): the same 15 lines on every
 * platform, and no dependence on which of inet_aton / inet_pton the C
 * library of the day provides. */
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

PlatformSocket platform_socket_listen(int port, int loopback, int *actual_port)
{
    return platform_socket_listen_addr(port, loopback ? "127.0.0.1" : NULL, actual_port);
}

PlatformSocket platform_socket_listen_addr(int port, const char *bind_addr, int *actual_port)
{
    struct sockaddr_in addr;
    int fd, on = 1;
    uint32_t ip = INADDR_ANY;

    if (bind_addr && !parse_ipv4(bind_addr, &ip))
        return PLATFORM_SOCKET_INVALID;

    socket_table_ensure_init();

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return PLATFORM_SOCKET_INVALID;

    /* Allow immediate rebind after the server restarts (TIME_WAIT). */
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(ip);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return PLATFORM_SOCKET_INVALID;
    }
    if (listen(fd, 4) < 0) {
        close(fd);
        return PLATFORM_SOCKET_INVALID;
    }
    if (actual_port) {
        socklen_t alen = sizeof(addr);
        if (getsockname(fd, (struct sockaddr *)&addr, &alen) == 0)
            *actual_port = ntohs(addr.sin_port);
        else
            *actual_port = port;
    }

    /* Listener occupies a slot but needs no IOBuf — it is never read/written. */
    {
        PlatformSocket sh;
        socket_table_lock();
        sh = socket_claim_locked(fd, 0);
        socket_table_unlock();
        if (sh == PLATFORM_SOCKET_INVALID) {
            close(fd);
            return PLATFORM_SOCKET_INVALID;
        }
        return sh;
    }
}

PlatformSocket platform_socket_accept(PlatformSocket listener)
{
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int fd;
    SockSlot *ls = sock_slot(listener);

    if (listener == 0 || !ls || ls->fd < 0)
        return PLATFORM_SOCKET_INVALID;

    /* accept() blocks — must run outside the table lock, and as a GC safe
     * region so a thread parked here waiting for a client does not stall a
     * concurrent stop-the-world GC.  This is the SLY read-loop deadlock. */
    {
        int lfd = ls->fd;
        cl_gc_enter_safe_region();
        fd = accept(lfd, (struct sockaddr *)&caddr, &clen);
        cl_gc_leave_safe_region();
    }
    if (fd < 0) return PLATFORM_SOCKET_INVALID;

    {
        PlatformSocket sh;
        socket_table_lock();
        sh = socket_claim_locked(fd, 1);
        socket_table_unlock();
        if (sh == PLATFORM_SOCKET_INVALID) {
            close(fd);
            return PLATFORM_SOCKET_INVALID;
        }
        return sh;
    }
}

/* --- UDP (datagram) sockets ---
 *
 * Connected UDP: socket(SOCK_DGRAM) + connect() fixes the peer so plain
 * send()/recv() work and the OS filters foreign datagrams.  Handles share
 * the TCP slot table (claimed without an IOBuf — UDP I/O is message-based,
 * never byte-buffered), so close / set_timeout / data_available just work. */

PlatformSocket platform_udp_connect(const char *host, int port)
{
    struct hostent *he;
    struct sockaddr_in addr;
    int fd;
    char host_buf[256];

    socket_table_ensure_init();

    /* Stack-copy the hostname: host may point into the Lisp arena and the
     * DNS safe region below lets a peer thread's compaction move it. */
    {
        size_t hl = strlen(host);
        if (hl >= sizeof(host_buf)) return PLATFORM_SOCKET_INVALID;
        memcpy(host_buf, host, hl + 1);
    }

    cl_gc_enter_safe_region();
    he = gethostbyname(host_buf);
    cl_gc_leave_safe_region();
    if (!he) return PLATFORM_SOCKET_INVALID;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return PLATFORM_SOCKET_INVALID;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* connect() on a datagram socket only records the peer — no handshake,
     * no blocking — so no timeout/safe-region dance is needed. */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return PLATFORM_SOCKET_INVALID;
    }

    {
        PlatformSocket sh;
        socket_table_lock();
        sh = socket_claim_locked(fd, 0);   /* no IOBuf: message I/O only */
        socket_table_unlock();
        if (sh == PLATFORM_SOCKET_INVALID) {
            close(fd);
            return PLATFORM_SOCKET_INVALID;
        }
        return sh;
    }
}

int platform_udp_send(PlatformSocket sh, const uint8_t *buf, uint32_t len)
{
    SockSlot *s = sock_slot(sh);
    int fd;
    ssize_t n;
    if (sh == 0 || !s || s->fd < 0) return -1;
    fd = s->fd;
    if (s->wtimeout > 0) {
        int r = socket_wait_ready(fd, s->wtimeout, 1);
        if (r == 0) return PLATFORM_SOCKET_TIMEOUT;
        if (r < 0) return -1;
    }
    cl_gc_enter_safe_region();
    n = send(fd, buf, (size_t)len, 0);
    cl_gc_leave_safe_region();
    return (n == (ssize_t)len) ? 0 : -1;
}

int platform_udp_recv(PlatformSocket sh, uint8_t *buf, uint32_t maxlen)
{
    SockSlot *s = sock_slot(sh);
    int fd;
    ssize_t n;
    if (sh == 0 || !s || s->fd < 0) return -1;
    fd = s->fd;
    if (s->rtimeout > 0) {
        int r = socket_wait_ready(fd, s->rtimeout, 0);
        if (r == 0) return PLATFORM_SOCKET_TIMEOUT;
        if (r < 0) return -1;
    }
    /* recv blocks until a datagram arrives — stay GC-cooperative. */
    cl_gc_enter_safe_region();
    n = recv(fd, buf, (size_t)maxlen, 0);
    cl_gc_leave_safe_region();
    if (n < 0) return -1;
    return (int)n;
}

int platform_socket_local_endpoint(PlatformSocket sh, char *ip_out, int *port_out)
{
    SockSlot *s = sock_slot(sh);
    struct sockaddr_in addr;
    socklen_t alen = (socklen_t)sizeof(addr);
    const unsigned char *b;
    if (sh == 0 || !s || s->fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    if (getsockname(s->fd, (struct sockaddr *)&addr, &alen) < 0) return -1;
    b = (const unsigned char *)&addr.sin_addr;
    snprintf(ip_out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    if (port_out) *port_out = (int)ntohs(addr.sin_port);
    return 0;
}

/* --- TLS public entry points (contract in platform.h; OpenSSL backend in
 * tls_openssl.c).  A successful start stores the connection on the slot,
 * after which the ordinary read/flush/probe/close paths above route through
 * the record layer transparently. --- */

int platform_tls_available(void)
{
    return tls_openssl_available(NULL, 0) == 0 ? 1 : 0;
}

const char *platform_tls_version(void)
{
    return tls_openssl_version();
}

int platform_tls_start(PlatformSocket sh, const PlatformTLSParams *params,
                       char *err, uint32_t errlen)
{
    SockSlot *s = sock_slot(sh);
    TLSConn *t;
    if (err && errlen > 0) err[0] = '\0';
    if (sh == 0 || !s || s->fd < 0) {
        if (err && errlen > 0)
            snprintf(err, errlen, "invalid or closed socket");
        return -1;
    }
    if (!s->buf) {
        /* Listener and UDP slots have no IOBuf; neither can carry TLS. */
        if (err && errlen > 0)
            snprintf(err, errlen, "TLS requires a connected TCP stream socket");
        return -1;
    }
    if (s->tls) {
        if (err && errlen > 0)
            snprintf(err, errlen, "socket is already TLS-upgraded");
        return -1;
    }
    /* Buffered plaintext output must reach the wire before handshake bytes
     * (relevant for STARTTLS-style flows). */
    if (socket_flush_wbuf(sh) != 0) {
        if (err && errlen > 0)
            snprintf(err, errlen, "flushing pending output before the "
                     "TLS handshake failed");
        return -1;
    }
    /* The handshake parks in poll(); params strings are the caller's C
     * buffers (never Lisp-arena pointers), so a safe region is fine. */
    cl_gc_enter_safe_region();
    t = tls_conn_start(s->fd, params, err, errlen);
    cl_gc_leave_safe_region();
    if (!t) return -1;
    s->tls = t;
    return 0;
}

int platform_tls_active(PlatformSocket sh)
{
    SockSlot *s = sock_slot(sh);
    return (sh > 0 && s && s->fd >= 0 && s->tls) ? 1 : 0;
}

int platform_tls_peer_cert_field(PlatformSocket sh, int field,
                                 char *out, uint32_t outlen)
{
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd < 0 || !s->tls) return -1;
    return tls_conn_peer_cert_field(s->tls, field, out, outlen);
}

/* ---- Ctrl-C break-in (see platform_break_pending in platform.h) ----
 * The handler only sets a flag; the VM polls it at safepoints and enters
 * the debugger (CL:BREAK) from well-defined interpreter state.  A second
 * SIGINT while the first is still unconsumed force-exits — the escape
 * hatch when the runtime is wedged somewhere the poll can't reach (a hung
 * C loop or blocking syscall). */
static volatile sig_atomic_t break_requested = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    if (break_requested)
        _exit(130);
    break_requested = 1;
}

int platform_break_pending(void)
{
    if (!break_requested)
        return 0;
    break_requested = 0;
    return 1;
}

void platform_fpu_setup(void)
{
    /* IEEE double semantics are the hardware default on POSIX targets;
     * only the hard-float m68k build needs FPU control setup. */
}

void platform_init(void)
{
    /* SA_RESTART: blocking reads (REPL stdin) resume after the handler so
     * Ctrl-C never turns into a spurious EOF; the break is delivered by the
     * VM poll instead. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
}

void platform_shutdown(void)
{
    /* Restore cooked mode if a TUI died without cleaning up (the atexit
     * handler also covers this, but shutdown runs earlier and matters for
     * the AmigaOS ordering — keep both platforms symmetric). */
    if (tty_raw_active)
        platform_tty_raw(0);
}

void platform_cache_clear(void *addr, uint32_t len)
{
    (void)addr; (void)len;
    /* POSIX hosts don't execute JIT-emitted m68k code; the AArch64 JIT
     * flushes through platform_jit_flush. */
}

#ifdef JIT_A64
#if !defined(__aarch64__) || !(defined(__APPLE__) || defined(__linux__))
#error "JIT_A64 is implemented for arm64 macOS and arm64 Linux only"
#endif
#ifdef __APPLE__
void *platform_jit_map(uint32_t bytes, void **writable)
{
    /* MAP_JIT: the one way to get writable+executable memory under the
     * macOS arm64 code-signing rules.  An unsigned or ad-hoc-signed build
     * needs no entitlement; a hardened-runtime one would need
     * com.apple.security.cs.allow-jit. */
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return NULL;
    *writable = p;
    return p;
}

void platform_jit_unmap(void *exec, void *writable, uint32_t bytes)
{
    (void)writable;   /* the same mapping */
    if (exec) munmap(exec, bytes);
}

void platform_jit_write_begin(void)
{
    pthread_jit_write_protect_np(0);
}

void platform_jit_write_end(void)
{
    pthread_jit_write_protect_np(1);
}

void platform_jit_flush(void *addr, uint32_t len)
{
    sys_icache_invalidate(addr, len);
}
#else /* __linux__ */
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
void *platform_jit_map(uint32_t bytes, void **writable)
{
    /* One memfd mapped twice: code is written through the read+write view
     * and runs from the read+execute view.  Flipping mprotect on one view
     * instead is not safe while another thread executes from its pages,
     * and kernels or policies that refuse W|X mappings accept this.  The
     * fd is not needed once both views exist.  The raw syscall, because
     * the memfd_create wrapper only came with glibc 2.27. */
    void *rw, *rx;
    int fd = (int)syscall(SYS_memfd_create, "amilisp-jit", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); return NULL; }
    rw = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rw == MAP_FAILED) { close(fd); return NULL; }
    rx = mmap(NULL, bytes, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    close(fd);
    if (rx == MAP_FAILED) { munmap(rw, bytes); return NULL; }
    *writable = rw;
    return rx;
}

void platform_jit_unmap(void *exec, void *writable, uint32_t bytes)
{
    if (exec) munmap(exec, bytes);
    if (writable) munmap(writable, bytes);
}

void platform_jit_write_begin(void) {}
void platform_jit_write_end(void) {}

void platform_jit_flush(void *addr, uint32_t len)
{
    /* DC CVAU + IC IVAU over the executable view's addresses: the data
     * written through the other view is the same physical memory. */
    __builtin___clear_cache((char *)addr, (char *)addr + len);
}
#endif
#endif /* JIT_A64 */

/* =============================================================
 * Generic FFI: foreign memory (POSIX implementation)
 *
 * On 64-bit POSIX, pointers don't fit in uint32_t, so we use a
 * side table that maps handles (1-based indices) to real pointers.
 * ============================================================= */

/*
 * On 64-bit POSIX a real pointer doesn't fit in the 32-bit CL_ForeignPtr
 * address field, so foreign pointers carry a 1-based handle into this side
 * table.  The table holds both memory we allocated (OWNED — freed by
 * platform_ffi_free) and externally-owned pointers we merely reference
 * (registered via platform_ffi_register — released, never freed, by
 * platform_ffi_release).
 *
 * The table grows on demand and recycles slots through a free-list so heavy
 * pointer arithmetic (CFFI inc-pointer / mem-aref) doesn't exhaust it.  A
 * mutex guards concurrent access from MP threads; the GC finalizer runs
 * world-stopped so it could skip the lock, but taking it there is harmless.
 */
typedef struct {
    void    *ptr;        /* real pointer; NULL when the slot is free */
    uint32_t size;       /* allocation size for OWNED memory, else 0 */
    uint32_t next_free;  /* free-list link (1-based), 0 = end; valid when free */
} FFIMemEntry;

static FFIMemEntry *ffi_mem_table   = NULL;
static uint32_t     ffi_mem_cap     = 0;   /* allocated slot count */
static uint32_t     ffi_mem_hi      = 0;   /* high-water: slots ever handed out */
static uint32_t     ffi_mem_free    = 0;   /* free-list head (1-based), 0 = empty */
static pthread_mutex_t ffi_mem_lock = PTHREAD_MUTEX_INITIALIZER;

/* Insert PTR (size for owned, 0 for external) and return a fresh 1-based
 * handle, or 0 on out-of-memory.  Caller holds ffi_mem_lock. */
static uint32_t ffi_table_insert_locked(void *ptr, uint32_t size)
{
    uint32_t h;
    if (ffi_mem_free != 0) {
        h = ffi_mem_free;
        ffi_mem_free = ffi_mem_table[h - 1].next_free;
    } else {
        if (ffi_mem_hi >= ffi_mem_cap) {
            uint32_t newcap = ffi_mem_cap ? ffi_mem_cap * 2 : 1024;
            FFIMemEntry *t = (FFIMemEntry *)realloc(ffi_mem_table,
                                                    (size_t)newcap * sizeof(FFIMemEntry));
            if (!t) return 0;
            ffi_mem_table = t;
            ffi_mem_cap = newcap;
        }
        h = ++ffi_mem_hi;  /* 1-based */
    }
    ffi_mem_table[h - 1].ptr = ptr;
    ffi_mem_table[h - 1].size = size;
    ffi_mem_table[h - 1].next_free = 0;
    return h;
}

/* Return a slot to the free-list (does NOT free ptr).  Caller holds lock. */
static void ffi_table_release_locked(uint32_t handle)
{
    if (handle == 0 || handle > ffi_mem_hi) return;
    if (ffi_mem_table[handle - 1].ptr == NULL) return;  /* already free */
    ffi_mem_table[handle - 1].ptr = NULL;
    ffi_mem_table[handle - 1].size = 0;
    ffi_mem_table[handle - 1].next_free = ffi_mem_free;
    ffi_mem_free = handle;
}

uint32_t platform_ffi_alloc(uint32_t size)
{
    void *p;
    uint32_t h;
    if (size == 0) return 0;
    p = malloc((size_t)size);
    if (!p) return 0;
    memset(p, 0, (size_t)size);
    pthread_mutex_lock(&ffi_mem_lock);
    h = ffi_table_insert_locked(p, size);
    pthread_mutex_unlock(&ffi_mem_lock);
    if (h == 0) free(p);  /* table grow failed */
    return h;
}

uint32_t platform_ffi_register(void *ptr)
{
    uint32_t h;
    if (ptr == NULL) return 0;  /* canonical null pointer */
    pthread_mutex_lock(&ffi_mem_lock);
    h = ffi_table_insert_locked(ptr, 0);
    pthread_mutex_unlock(&ffi_mem_lock);
    return h;
}

void platform_ffi_free(uint32_t handle, uint32_t size)
{
    void *p = NULL;
    (void)size;
    if (handle == 0) return;
    pthread_mutex_lock(&ffi_mem_lock);
    if (handle <= ffi_mem_hi)
        p = ffi_mem_table[handle - 1].ptr;
    ffi_table_release_locked(handle);
    pthread_mutex_unlock(&ffi_mem_lock);
    if (p) free(p);
}

void platform_ffi_release(uint32_t handle)
{
    pthread_mutex_lock(&ffi_mem_lock);
    ffi_table_release_locked(handle);
    pthread_mutex_unlock(&ffi_mem_lock);
}

void *platform_ffi_resolve(uint32_t handle)
{
    void *p;
    if (handle == 0) return NULL;
    pthread_mutex_lock(&ffi_mem_lock);
    p = (handle <= ffi_mem_hi) ? ffi_mem_table[handle - 1].ptr : NULL;
    pthread_mutex_unlock(&ffi_mem_lock);
    return p;
}

uint32_t platform_ffi_peek32(uint32_t handle, uint32_t offset)
{
    void *base = platform_ffi_resolve(handle);
    if (!base) return 0;
    return *(uint32_t *)((uint8_t *)base + offset);
}

uint16_t platform_ffi_peek16(uint32_t handle, uint32_t offset)
{
    void *base = platform_ffi_resolve(handle);
    if (!base) return 0;
    return *(uint16_t *)((uint8_t *)base + offset);
}

uint8_t platform_ffi_peek8(uint32_t handle, uint32_t offset)
{
    void *base = platform_ffi_resolve(handle);
    if (!base) return 0;
    return *(uint8_t *)((uint8_t *)base + offset);
}

void platform_ffi_poke32(uint32_t handle, uint32_t offset, uint32_t val)
{
    void *base = platform_ffi_resolve(handle);
    if (!base) return;
    *(uint32_t *)((uint8_t *)base + offset) = val;
}

void platform_ffi_poke16(uint32_t handle, uint32_t offset, uint16_t val)
{
    void *base = platform_ffi_resolve(handle);
    if (!base) return;
    *(uint16_t *)((uint8_t *)base + offset) = val;
}

void platform_ffi_poke8(uint32_t handle, uint32_t offset, uint8_t val)
{
    void *base = platform_ffi_resolve(handle);
    if (!base) return;
    *(uint8_t *)((uint8_t *)base + offset) = val;
}

/* =============================================================
 * Dynamic libraries + foreign function calls (POSIX: dlopen + libffi)
 * ============================================================= */

uint32_t platform_ffi_dlopen(const char *name)
{
    /* RTLD_GLOBAL so symbols become visible to subsequent default-namespace
     * lookups (matches how CFFI expects use-foreign-library to behave). */
    void *h = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!h) return 0;
    return platform_ffi_register(h);
}

uint32_t platform_ffi_dlsym(uint32_t lib_handle, const char *name)
{
    void *sym;
    void *lib = (lib_handle == 0) ? RTLD_DEFAULT : platform_ffi_resolve(lib_handle);
    if (lib_handle != 0 && lib == NULL) return 0;
    dlerror();  /* clear */
    sym = dlsym(lib, name);
    if (sym == NULL && dlerror() != NULL) return 0;
    return platform_ffi_register(sym);
}

void platform_ffi_dlclose(uint32_t lib_handle)
{
    void *lib = platform_ffi_resolve(lib_handle);
    if (lib) dlclose(lib);
    platform_ffi_release(lib_handle);
}

/* Map a CLFFIType to the corresponding libffi ffi_type. */
static ffi_type *ffi_type_for(CLFFIType t)
{
    switch (t) {
    case CL_FFI_VOID:    return &ffi_type_void;
    case CL_FFI_I8:      return &ffi_type_sint8;
    case CL_FFI_U8:      return &ffi_type_uint8;
    case CL_FFI_I16:     return &ffi_type_sint16;
    case CL_FFI_U16:     return &ffi_type_uint16;
    case CL_FFI_I32:     return &ffi_type_sint32;
    case CL_FFI_U32:     return &ffi_type_uint32;
    case CL_FFI_I64:     return &ffi_type_sint64;
    case CL_FFI_U64:     return &ffi_type_uint64;
    case CL_FFI_FLOAT:   return &ffi_type_float;
    case CL_FFI_DOUBLE:  return &ffi_type_double;
    case CL_FFI_POINTER: return &ffi_type_pointer;
    }
    return &ffi_type_void;
}

int platform_ffi_call(void *fn, CLFFIType ret_type, CLFFIValue *ret_val,
                      int nargs, int nfixed,
                      const CLFFIType *arg_types, const CLFFIValue *arg_vals)
{
    ffi_cif cif;
    ffi_type *atypes[CL_FFI_MAX_ARGS];
    void *avalues[CL_FFI_MAX_ARGS];
    /* libffi widens any integral return narrower than ffi_arg to ffi_arg and
     * writes that many bytes, so the return buffer must be at least that big. */
    union { CLFFIValue v; ffi_arg pad; } rc;
    ffi_status st;
    int i;

    if (!fn) return -1;
    if (nargs < 0 || nargs > CL_FFI_MAX_ARGS) return -1;

    for (i = 0; i < nargs; i++) {
        atypes[i] = ffi_type_for(arg_types[i]);
        /* arg_vals is const; libffi wants a non-const pointer to each value
         * but never writes through it for arguments. */
        avalues[i] = (void *)&arg_vals[i];
    }

    if (nfixed >= 0 && nfixed < nargs) {
        st = ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, (unsigned)nfixed,
                              (unsigned)nargs, ffi_type_for(ret_type), atypes);
    } else {
        st = ffi_prep_cif(&cif, FFI_DEFAULT_ABI, (unsigned)nargs,
                          ffi_type_for(ret_type), atypes);
    }
    if (st != FFI_OK) return -1;

    rc.pad = 0;
    ffi_call(&cif, FFI_FN(fn), &rc, avalues);

    if (ret_val) {
        /* For small integer returns libffi stored an ffi_arg; narrow it. */
        switch (ret_type) {
        case CL_FFI_VOID:    break;
        case CL_FFI_I8:      ret_val->i8  = (int8_t)(ffi_sarg)rc.pad; break;
        case CL_FFI_U8:      ret_val->u8  = (uint8_t)(ffi_arg)rc.pad; break;
        case CL_FFI_I16:     ret_val->i16 = (int16_t)(ffi_sarg)rc.pad; break;
        case CL_FFI_U16:     ret_val->u16 = (uint16_t)(ffi_arg)rc.pad; break;
        case CL_FFI_I32:     ret_val->i32 = (int32_t)(ffi_sarg)rc.pad; break;
        case CL_FFI_U32:     ret_val->u32 = (uint32_t)(ffi_arg)rc.pad; break;
        case CL_FFI_I64:     ret_val->i64 = rc.v.i64; break;
        case CL_FFI_U64:     ret_val->u64 = rc.v.u64; break;
        case CL_FFI_FLOAT:   ret_val->f   = rc.v.f; break;
        case CL_FFI_DOUBLE:  ret_val->d   = rc.v.d; break;
        case CL_FFI_POINTER: ret_val->p   = rc.v.p; break;
        }
    }
    return 0;
}

/* --- Callbacks: Lisp functions exposed as C function pointers --- */

typedef struct {
    ffi_cif    cif;
    ffi_type  *atypes[CL_FFI_MAX_ARGS];
    CLFFIType  ret_type;
    CLFFIType  arg_types[CL_FFI_MAX_ARGS];
    int        nargs;
    platform_ffi_cb_handler handler;
    void      *user_data;
    ffi_closure *closure;
    void      *code;
} PosixClosure;

/* libffi entry point: decode raw C args, hand them to the generic handler,
 * then write the handler's result back where libffi expects it. */
static void posix_closure_tramp(ffi_cif *cif, void *ret, void **args, void *ud)
{
    PosixClosure *pc = (PosixClosure *)ud;
    CLFFIValue cargs[CL_FFI_MAX_ARGS];
    CLFFIValue cret;
    int i;
    (void)cif;

    for (i = 0; i < pc->nargs; i++) {
        switch (pc->arg_types[i]) {
        case CL_FFI_I8:      cargs[i].i8  = *(int8_t  *)args[i]; break;
        case CL_FFI_U8:      cargs[i].u8  = *(uint8_t *)args[i]; break;
        case CL_FFI_I16:     cargs[i].i16 = *(int16_t *)args[i]; break;
        case CL_FFI_U16:     cargs[i].u16 = *(uint16_t*)args[i]; break;
        case CL_FFI_I32:     cargs[i].i32 = *(int32_t *)args[i]; break;
        case CL_FFI_U32:     cargs[i].u32 = *(uint32_t*)args[i]; break;
        case CL_FFI_I64:     cargs[i].i64 = *(int64_t *)args[i]; break;
        case CL_FFI_U64:     cargs[i].u64 = *(uint64_t*)args[i]; break;
        case CL_FFI_FLOAT:   cargs[i].f   = *(float   *)args[i]; break;
        case CL_FFI_DOUBLE:  cargs[i].d   = *(double  *)args[i]; break;
        case CL_FFI_POINTER: cargs[i].p   = *(void   **)args[i]; break;
        case CL_FFI_VOID:    break;
        }
    }

    memset(&cret, 0, sizeof(cret));
    pc->handler(pc->user_data, cargs, &cret);

    /* libffi widens integral returns to ffi_arg. */
    switch (pc->ret_type) {
    case CL_FFI_VOID:    break;
    case CL_FFI_I8:      *(ffi_arg *)ret = (ffi_arg)(ffi_sarg)cret.i8;  break;
    case CL_FFI_U8:      *(ffi_arg *)ret = (ffi_arg)cret.u8;            break;
    case CL_FFI_I16:     *(ffi_arg *)ret = (ffi_arg)(ffi_sarg)cret.i16; break;
    case CL_FFI_U16:     *(ffi_arg *)ret = (ffi_arg)cret.u16;           break;
    case CL_FFI_I32:     *(ffi_arg *)ret = (ffi_arg)(ffi_sarg)cret.i32; break;
    case CL_FFI_U32:     *(ffi_arg *)ret = (ffi_arg)cret.u32;           break;
    case CL_FFI_I64:     *(int64_t  *)ret = cret.i64; break;
    case CL_FFI_U64:     *(uint64_t *)ret = cret.u64; break;
    case CL_FFI_FLOAT:   *(float    *)ret = cret.f;   break;
    case CL_FFI_DOUBLE:  *(double   *)ret = cret.d;   break;
    case CL_FFI_POINTER: *(void    **)ret = cret.p;   break;
    }
}

void *platform_ffi_make_closure(CLFFIType ret_type, int nargs,
                                const CLFFIType *arg_types,
                                const int8_t *arg_regs,
                                platform_ffi_cb_handler handler,
                                void *user_data, void **out_closure)
{
    PosixClosure *pc;
    int i;
    (void)arg_regs;   /* C ABI only: every argument is a stack/register C arg */
    if (nargs < 0 || nargs > CL_FFI_MAX_ARGS) return NULL;
    pc = (PosixClosure *)calloc(1, sizeof(PosixClosure));
    if (!pc) return NULL;
    pc->ret_type = ret_type;
    pc->nargs = nargs;
    pc->handler = handler;
    pc->user_data = user_data;
    for (i = 0; i < nargs; i++) {
        pc->arg_types[i] = arg_types[i];
        pc->atypes[i] = ffi_type_for(arg_types[i]);
    }
    pc->closure = (ffi_closure *)ffi_closure_alloc(sizeof(ffi_closure), &pc->code);
    if (!pc->closure) { free(pc); return NULL; }
    if (ffi_prep_cif(&pc->cif, FFI_DEFAULT_ABI, (unsigned)nargs,
                     ffi_type_for(ret_type), pc->atypes) != FFI_OK) {
        ffi_closure_free(pc->closure);
        free(pc);
        return NULL;
    }
    if (ffi_prep_closure_loc(pc->closure, &pc->cif, posix_closure_tramp,
                             pc, pc->code) != FFI_OK) {
        ffi_closure_free(pc->closure);
        free(pc);
        return NULL;
    }
    if (out_closure) *out_closure = pc;
    return pc->code;
}

void platform_ffi_free_closure(void *closure)
{
    PosixClosure *pc = (PosixClosure *)closure;
    if (!pc) return;
    if (pc->closure) ffi_closure_free(pc->closure);
    free(pc);
}

/* =============================================================
 * Amiga-specific FFI stubs (not available on POSIX)
 * ============================================================= */

uint32_t platform_amiga_open_library(const char *name, uint32_t version)
{
    (void)name; (void)version;
    return 0;  /* Not available on POSIX */
}

void platform_amiga_close_library(uint32_t lib_base)
{
    (void)lib_base;
}

uint32_t platform_amiga_call(uint32_t lib_base, int16_t offset,
                              uint32_t *regs, uint16_t reg_mask)
{
    (void)lib_base; (void)offset; (void)regs; (void)reg_mask;
    return 0;  /* Not available on POSIX */
}

uint32_t platform_amiga_alloc_chip(uint32_t size)
{
    /* On POSIX, chip memory is just regular memory */
    return platform_ffi_alloc(size);
}

void platform_amiga_free_chip(uint32_t addr, uint32_t size)
{
    platform_ffi_free(addr, size);
}

/* No MUI on POSIX: the push is recorded for tests/test_amiga_mui.lisp. */
static uint32_t amiga_last_push[5];
static int      amiga_last_push_valid = 0;

void platform_amiga_push_method(uint32_t app, uint32_t push_method,
                                uint32_t object, uint32_t method,
                                uint32_t value)
{
    amiga_last_push[0] = app;
    amiga_last_push[1] = push_method;
    amiga_last_push[2] = object;
    amiga_last_push[3] = method;
    amiga_last_push[4] = value;
    amiga_last_push_valid = 1;
}

int platform_amiga_last_pushed_method(uint32_t out[5])
{
    int i;
    if (!amiga_last_push_valid) return 0;
    for (i = 0; i < 5; i++) out[i] = amiga_last_push[i];
    amiga_last_push_valid = 0;
    return 1;
}

int platform_amiga_push_method_prepare(void)
{
    return 1;
}

uint32_t platform_amiga_do_super_method(void *cl, void *object, void *message)
{
    void *super = NULL, *entry = NULL;
    if (cl == NULL) return 0;
    memcpy(&super, (const uint8_t *)cl + 24, sizeof super);
    if (super == NULL) return 0;
    memcpy(&entry, (const uint8_t *)super + 8, sizeof entry);
    if (entry == NULL) return 0;
    return ((uint32_t (*)(void *, void *, void *))entry)(super, object, message);
}
