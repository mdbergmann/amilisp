/*
 * Windows (mingw-w64) platform layer.
 *
 * This is platform_posix.c ported block by block: everything that has a
 * Windows equivalent keeps the POSIX file's structure, comments and — most
 * importantly — its GC safe-region brackets, so the two files can be diffed
 * against each other when either changes.  Only these blocks differ:
 *
 *   page protection / write-watch  VirtualAlloc + a vectored exception handler
 *   TTY control                    the console API (SetConsoleMode & friends)
 *   DIRECTORY                      FindFirstFile instead of glob()
 *   sockets                        Winsock (WSAPoll, closesocket, SOCKET fds)
 *   dlopen / poll                  shimmed in win32_compat.h
 *   Ctrl-C                         SetConsoleCtrlHandler instead of SIGINT
 *
 * Threading is NOT ported: platform_thread_posix.c compiles unchanged here
 * because mingw ships winpthreads.
 *
 * Requires Windows 10 or later (GetCurrentThreadStackLimits) and links
 * against ws2_32.
 */

/* This file DEFINES platform_alloc / platform_free, so it must opt out of
 * the DEBUG_MEM_TRACK macros in platform.h that redirect those names to the
 * leak tracer — otherwise the definitions themselves get renamed.  It calls
 * the allocator nowhere else, so nothing here goes untracked. */
#define CL_MEM_TRACK_IMPL 1

#include "platform.h"
#include "platform_thread.h"
#include "tls_openssl.h"
#include "win32_compat.h"    /* winsock2 + dlopen/poll shims — include first */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <direct.h>
#include <io.h>
#include <limits.h>
#include <pthread.h>
#include <ffi.h>

#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif

/* Rewrite Windows path separators to '/' in place.  Every path that leaves
 * the platform layer for the Lisp side (TRUENAME, DIRECTORY results, the
 * executable prefix used to find lib/) is normalised this way so the CL
 * pathname code sees the one separator it parses.  Nothing needs converting
 * on the way back in: Win32 accepts '/' everywhere. */
static void win_normalize_seps(char *p)
{
    for (; *p; p++)
        if (*p == '\\') *p = '/';
}

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

uint32_t platform_page_size(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? (uint32_t)si.dwPageSize : 4096u;
}

void *platform_alloc_pages(uint32_t size)
{
    /* MEM_COMMIT memory is zero-filled, like MAP_ANON. */
    return VirtualAlloc(NULL, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE,
                        PAGE_READWRITE);
}

void platform_free_pages(void *ptr, uint32_t size)
{
    /* MEM_RELEASE frees the whole reservation and requires a zero size. */
    (void)size;
    if (ptr)
        VirtualFree(ptr, 0, MEM_RELEASE);
}

int platform_page_protect(uint8_t *addr, uint32_t len, int readonly)
{
    DWORD old;
    return VirtualProtect(addr, (SIZE_T)len,
                          readonly ? PAGE_READONLY : PAGE_READWRITE,
                          &old) ? 0 : -1;
}

/* Watch state.  Written only with the watch uninstalled or under the GC's
 * stop-the-world (install/remove happen at heap init/shutdown); read by
 * the fault handler on any thread. */
static uint8_t *volatile ww_base = NULL;
static uint32_t ww_len = 0;
static volatile uint8_t *ww_bitmap = NULL;
static uint32_t ww_pagesize = 0;
static void *ww_veh = NULL;      /* AddVectoredExceptionHandler cookie */

/* Vectored exception handler — the Win32 counterpart of the POSIX SIGSEGV
 * write-protection handler.  A store into a PAGE_READONLY arena page raises
 * EXCEPTION_ACCESS_VIOLATION carrying ExceptionInformation[0] == 1 (write)
 * and [1] == the faulting address: mark the page dirty, make it writable,
 * and resume the faulting instruction, which retries the store.
 *
 * Anything else — a read fault, a write outside the watched arena, any other
 * exception — returns EXCEPTION_CONTINUE_SEARCH, so a genuine crash still
 * reaches the crash handler with its real address (what ww_chain() does on
 * POSIX by restoring and re-raising).
 *
 * Vectored, not SetUnhandledExceptionFilter: vectored handlers run before
 * any frame-based __try/__except, so the retry works regardless of what the
 * faulting code is wrapped in. */
static LONG CALLBACK ww_handler(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint8_t *addr, *base;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION ||
        er->NumberParameters < 2 ||
        er->ExceptionInformation[0] != 1 /* 1 = write access */)
        return EXCEPTION_CONTINUE_SEARCH;

    addr = (uint8_t *)er->ExceptionInformation[1];
    base = ww_base;
    if (base && addr >= base && (uint32_t)(addr - base) < ww_len) {
        uint32_t page = (uint32_t)(addr - base) / ww_pagesize;
        DWORD old;
        /* Atomic: peer threads can fault on pages sharing the bitmap byte. */
        __sync_or_and_fetch((uint8_t *)&ww_bitmap[page >> 3],
                            (uint8_t)(1u << (page & 7u)));
        if (VirtualProtect(base + (size_t)page * ww_pagesize, ww_pagesize,
                           PAGE_READWRITE, &old))
            return EXCEPTION_CONTINUE_EXECUTION;  /* store retries, succeeds */
        /* VirtualProtect failed inside a fault handler — nothing sane left. */
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int platform_write_watch_install(uint8_t *base, uint32_t len,
                                 volatile uint8_t *dirty_bitmap)
{
    ww_pagesize = platform_page_size();
    ww_bitmap = dirty_bitmap;
    ww_len = len;
    ww_base = base;

    if (!ww_veh) {
        /* First == 1: run ahead of any other vectored handler so that a
         * debugger's or the CRT's handler never sees our benign faults. */
        ww_veh = AddVectoredExceptionHandler(1, ww_handler);
        if (!ww_veh) {
            ww_base = NULL;
            return -1;
        }
    }
    return 0;
}

void platform_write_watch_remove(void)
{
    if (ww_veh) {
        RemoveVectoredExceptionHandler(ww_veh);
        ww_veh = NULL;
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
 * Raw mode switches console reads from stdio (getchar) to direct ReadFile
 * on the console handle with a private one-byte pushback.  This is what
 * makes the LISTEN / READ-CHAR-NO-HANG probe exact: getchar() reads ahead
 * into stdio's buffer, so a console-handle probe would claim "nothing
 * pending" while the tail of an escape sequence sits in that buffer —
 * stalling a TUI's input decoder.  In cooked mode reads stay on stdio (no
 * behavior change for the REPL / piped-stdin paths, which mix fgets and
 * getchar).
 *
 * ENABLE_VIRTUAL_TERMINAL_INPUT is part of raw mode: it makes the console
 * deliver arrow keys and friends as the same ANSI escape sequences a POSIX
 * terminal sends, so a TUI's decoder needs no Windows-specific branch.
 *
 * Single-reader assumption: like the pre-existing getchar path, console
 * input is not safe for concurrent readers; a TUI reads keys from one
 * thread. */

static DWORD tty_saved_mode = 0;
static int tty_saved_valid = 0;
static int tty_raw_active = 0;
static int tty_pushback = -1;
static int tty_vt_input = 0;    /* raw mode got ENABLE_VIRTUAL_TERMINAL_INPUT */

static HANDLE tty_in(void)
{
    return GetStdHandle(STD_INPUT_HANDLE);
}

/* True when stdin is a real console (not a pipe or a redirected file) — the
 * Win32 answer to isatty(), which on Windows also says "yes" to NUL and to
 * any other character device. */
static int tty_is_console(void)
{
    DWORD mode;
    HANDLE h = tty_in();
    return (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) ? 1 : 0;
}

/* Crash insurance: never leave the user's console in raw mode if the process
 * exits (cl_error longjmp escapes, ext:quit, crash-to-exit) while a TUI is
 * up.  Registered once on first raw-mode entry. */
static void tty_restore_at_exit(void)
{
    if (tty_raw_active && tty_saved_valid) {
        SetConsoleMode(tty_in(), tty_saved_mode);
        tty_raw_active = 0;
    }
}

int platform_tty_raw(int enable)
{
    static int atexit_registered = 0;
    HANDLE h = tty_in();
    DWORD mode;

    if (!tty_is_console())
        return -1;

    if (enable) {
        if (tty_raw_active)
            return 0;
        if (!GetConsoleMode(h, &tty_saved_mode))
            return -1;
        tty_saved_valid = 1;
        /* The console counterpart of cfmakeraw: no line assembly, no echo,
         * and no Ctrl-C/Ctrl-Break translation (ENABLE_PROCESSED_INPUT), so
         * the TUI sees every keystroke as data. */
        mode = tty_saved_mode & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                                         ENABLE_PROCESSED_INPUT);
        mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        tty_vt_input = 1;
        if (!SetConsoleMode(h, mode)) {
            /* Legacy consoles reject VT input — fall back to raw-without-VT
             * rather than leaving the TUI stuck in cooked mode.  The
             * availability probe has to know which mode won: see
             * platform_tty_char_avail. */
            mode &= ~(DWORD)ENABLE_VIRTUAL_TERMINAL_INPUT;
            tty_vt_input = 0;
            if (!SetConsoleMode(h, mode))
                return -1;
        }
        tty_raw_active = 1;
        if (!atexit_registered) {
            atexit(tty_restore_at_exit);
            atexit_registered = 1;
        }
        return 0;
    }

    if (!tty_raw_active)
        return 0;
    if (tty_saved_valid && !SetConsoleMode(h, tty_saved_mode))
        return -1;
    tty_raw_active = 0;
    tty_vt_input = 0;
    return 0;
}

int platform_tty_raw_active(void)
{
    return tty_raw_active;
}

/* True when a virtual key produces no input bytes at all, so a pending
 * key-down for it must not be reported as "a character is available" — the
 * following read would block on a keypress the user has already made. */
static int tty_vk_is_modifier(WORD vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL ||
           vk == VK_LWIN || vk == VK_RWIN;
}

/* Will this key-down record make ReadFile produce at least one byte?
 *
 * With ENABLE_VIRTUAL_TERMINAL_INPUT the console synthesises an escape
 * sequence for the navigation keys, so an arrow counts even though its
 * INPUT_RECORD carries no character.  On a legacy console that fell back to
 * raw-without-VT it does not: only records with a character yield bytes, and
 * answering "ready" for an arrow there would park the next read on a
 * keypress the user has already made — exactly the guarantee LISTEN and
 * READ-CHAR-NO-HANG must not break. */
static int tty_key_yields_byte(const KEY_EVENT_RECORD *k)
{
    if (!k->bKeyDown)
        return 0;
    if (k->uChar.UnicodeChar != 0)
        return 1;
    return tty_vt_input && !tty_vk_is_modifier(k->wVirtualKeyCode);
}

int platform_tty_char_avail(void)
{
    HANDLE h;
    if (tty_pushback != -1)
        return 1;
    h = tty_in();
    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return 0;
    if (!tty_is_console()) {
        /* Redirected stdin.  A pipe knows how much it has buffered; a disk
         * file (or anything else) never blocks, so a read is always ready. */
        DWORD avail = 0;
        if (GetFileType(h) == FILE_TYPE_PIPE)
            return (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && avail > 0)
                   ? 1 : 0;
        return 1;
    }
    /* Console: the handle also queues key-up, mouse, focus and resize
     * records, none of which yield a byte.  Discard those from the head of
     * the queue and answer on what is left, so a "yes" means exactly "the
     * next read returns without blocking" — the guarantee the POSIX
     * select() probe gives. */
    for (;;) {
        INPUT_RECORD rec;
        DWORD n = 0;
        if (!PeekConsoleInputA(h, &rec, 1, &n) || n == 0)
            return 0;
        if (rec.EventType == KEY_EVENT &&
            tty_key_yields_byte(&rec.Event.KeyEvent))
            return 1;
        if (!ReadConsoleInputA(h, &rec, 1, &n) || n == 0)
            return 0;               /* could not consume it; report not ready */
    }
}

int platform_tty_size(int *cols, int *rows)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    HANDLE outs[2];
    int i;
    /* stdout first (a TUI's frames go there), then stderr, then the console
     * device itself — stdio may be partially redirected. */
    outs[0] = GetStdHandle(STD_OUTPUT_HANDLE);
    outs[1] = GetStdHandle(STD_ERROR_HANDLE);
    for (i = 0; i < 2; i++) {
        if (outs[i] && outs[i] != INVALID_HANDLE_VALUE &&
            GetConsoleScreenBufferInfo(outs[i], &csbi)) {
            int c = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            int r = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            if (c > 0 && r > 0) {
                *cols = c;
                *rows = r;
                return 0;
            }
        }
    }
    {
        HANDLE con = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                 OPEN_EXISTING, 0, NULL);
        if (con != INVALID_HANDLE_VALUE) {
            int ok = GetConsoleScreenBufferInfo(con, &csbi) &&
                     csbi.srWindow.Right > csbi.srWindow.Left &&
                     csbi.srWindow.Bottom > csbi.srWindow.Top;
            if (ok) {
                *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
                *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            }
            CloseHandle(con);
            if (ok) return 0;
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
         * console probe and this read agree on what is pending. */
        unsigned char b;
        DWORD got = 0;
        BOOL ok;
        HANDLE h = tty_in();
        cl_gc_enter_safe_region();
        ok = ReadFile(h, &b, 1, &got, NULL);
        cl_gc_leave_safe_region();
        return (ok && got == 1) ? (int)b : -1;
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
    /* No-op on Windows — stdin doesn't have residual CLI data */
}

int platform_stdin_is_interactive(void)
{
    /* GetConsoleMode, not isatty(): the Windows CRT reports every character
     * device — NUL included — as a tty, which would put the REPL into
     * interactive mode under `amilisp < NUL`. */
    return tty_is_console();
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
    case PLATFORM_FILE_READ:   fmode = "rb"; break;
    case PLATFORM_FILE_WRITE:  fmode = "wb"; break;
    case PLATFORM_FILE_APPEND: fmode = "ab"; break;
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
    /* GetProcessTimes over clock(): clock() on the Windows CRT measures wall
     * time since process start, not CPU time, which is what GET-INTERNAL-
     * RUN-TIME must report.  The kernel/user FILETIMEs are 100ns units. */
    FILETIME creation, exit, kernel, user;
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        ULARGE_INTEGER k, u;
        k.LowPart  = kernel.dwLowDateTime;  k.HighPart  = kernel.dwHighDateTime;
        u.LowPart  = user.dwLowDateTime;    u.HighPart  = user.dwHighDateTime;
        return (uint32_t)((k.QuadPart + u.QuadPart) / 10000ULL);
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
        Sleep((DWORD)milliseconds);
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

int platform_file_make_executable(const char *path)
{
    (void)path;                         /* the .exe name is what counts */
    return 0;
}

int platform_file_rename(const char *oldpath, const char *newpath)
{
    /* NOT rename(3): the Windows CRT's fails when the destination exists,
     * while POSIX rename(2) and the Amiga back end both replace it.  Callers
     * rely on replacing — OPEN :if-exists :rename renames the original out of
     * the way, ignoring the result (builtins_stream.c), so a failure there
     * left the stale backup in place and then truncated the file the backup
     * was supposed to preserve.  MOVEFILE_COPY_ALLOWED additionally lets the
     * rename cross volumes, which POSIX rename cannot do but callers of a
     * "rename this file" primitive reasonably expect. */
    {
        wchar_t wold[PATH_MAX], wnew[PATH_MAX];
        if (!cl_win_utf8_to_wide(oldpath, wold, PATH_MAX) ||
            !cl_win_utf8_to_wide(newpath, wnew, PATH_MAX))
            return -1;
        return MoveFileExW(wold, wnew,
                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)
               ? 0 : -1;
    }
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
    return (_mkdir(path) == 0 || errno == EEXIST) ? 0 : -1;
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

uint32_t platform_cpu_store_selftest(uint32_t rounds)
{
    (void)rounds;
    return 0;                           /* the m68k build has the test */
}

/* Nothing to take back: the kernel closes every descriptor and reclaims the
 * whole address space when the process ends.  See platform.h. */
void platform_release_resources(void)
{
}

/* The executable's path, UTF-8 with `/' separators, into `resolved`
 * (PATH_MAX bytes); 0 when it cannot be determined. */
static int executable_resolved(char *resolved)
{
    wchar_t wresolved[PATH_MAX];
    DWORD n = GetModuleFileNameW(NULL, wresolved, (DWORD)PATH_MAX);
    if (n == 0 || n >= (DWORD)PATH_MAX)
        return 0;               /* failed, or the path was truncated */
    /* Not GetModuleFileNameA: an installation under a user profile whose
     * name the ANSI code page cannot spell would come back with '?' in it,
     * and lib/ would then never be found. */
    if (!cl_win_wide_to_utf8(wresolved, resolved, PATH_MAX))
        return 0;
    win_normalize_seps(resolved);
    return 1;
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

long platform_stack_headroom(void)
{
    /* Windows hands out the real bounds of the current thread's stack, so —
     * unlike POSIX, which has no portable answer and returns -1 — the
     * recursion guard can use an exact number here.  That matters for the
     * reader/compiler recursion on deeply nested source: an exhausted stack
     * becomes a catchable "C stack nearly exhausted" error instead of an
     * access violation in the guard page. */
    ULONG_PTR low = 0, high = 0;
    char probe;
    ULONG_PTR sp = (ULONG_PTR)&probe;
    ULONG_PTR cushion;

    GetCurrentThreadStackLimits(&low, &high);
    (void)high;
    if (low == 0)
        return -1;              /* bounds unavailable: "unknown", not "empty" */
    if (sp <= low)
        return 0;
    /* Keep a few pages back from the guard page: the guard hit itself must
     * not be what the guard reports as "still available". */
    cushion = (ULONG_PTR)4u * platform_page_size();
    if (sp - low <= cushion)
        return 0;
    {
        ULONG_PTR avail = (sp - low) - cushion;
        return (avail > 0x7FFFFFFFUL) ? 0x7FFFFFFFL : (long)avail;
    }
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
    if (getcwd(buf, (size_t)bufsize) != NULL) {
        win_normalize_seps(buf);
        return (int)strlen(buf);
    }
    return 0;
}

int platform_startup_args(int *argc, char ***argv)
{
    /* A Windows process always has its command line. */
    (void)argc; (void)argv;
    return 0;
}

int platform_run_main(int (*fn)(int, char **), int argc, char **argv)
{
    /* The main thread's stack is 1 MB or more here. */
    return fn(argc, argv);
}

/* Defined below; _exit() skips the atexit handler that would normally
 * restore it, so platform_process_exit runs it explicitly. */
static void console_restore_at_exit(void);

void platform_process_exit(int code)
{
    fflush(NULL);
    console_restore_at_exit();
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
    /* Windows has no wait-status encoding: system() returns the child's exit
     * code directly (and -1 only when the command interpreter could not be
     * started), so there is no WIFEXITED/WIFSIGNALED unpacking to do. */
    return status;
}

/* --- DIRECTORY (the glob() replacement) -------------------------------
 *
 * Windows has no glob(3), and FindFirstFile only expands a wildcard in the
 * LAST component, so a wildcard in a middle directory component needs
 * the walk to be done here.  What follows matches POSIX glob() as amilisp uses it: '*' and '?'
 * in any component, no brace expansion and no '**' (glob has neither),
 * GLOB_MARK's trailing '/' on directory results, and "no matches" reported
 * as NULL rather than an empty vector.
 *
 * Matching is case-insensitive because the Windows filesystem is. */

typedef struct {
    char **v;
    int    n;
    int    cap;
} StrVec;

static int sv_push(StrVec *sv, const char *s)
{
    char *dup;
    if (sv->n >= sv->cap) {
        int newcap = sv->cap ? sv->cap * 2 : 32;
        char **nv = (char **)realloc(sv->v, (size_t)newcap * sizeof(char *));
        if (!nv) return 0;
        sv->v = nv;
        sv->cap = newcap;
    }
    dup = _strdup(s);
    if (!dup) return 0;
    sv->v[sv->n++] = dup;
    return 1;
}

/* Wildcard match for ONE path component. */
static int win_wildmatch(const char *p, const char *s)
{
    while (*p) {
        if (*p == '*') {
            p++;
            if (!*p) return 1;              /* trailing '*' takes the rest */
            for (; *s; s++)
                if (win_wildmatch(p, s)) return 1;
            return win_wildmatch(p, s);     /* '*' may also match nothing */
        }
        if (!*s) return 0;
        if (*p != '?' &&
            tolower((unsigned char)*p) != tolower((unsigned char)*s))
            return 0;
        p++;
        s++;
    }
    return *s == '\0';
}

static int win_has_wildcard(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        if (s[i] == '*' || s[i] == '?') return 1;
    return 0;
}

/* Expand `rest` (the remaining '/'-separated pattern) against the directory
 * named by `prefix` (empty, or ending in '/'), appending matches to `out`.
 * Recurses once per pattern component; depth is bounded by the pattern the
 * caller wrote, not by the tree. */
static void win_glob_walk(const char *prefix, const char *rest, StrVec *out)
{
    const char *slash = strchr(rest, '/');
    size_t complen = slash ? (size_t)(slash - rest) : strlen(rest);
    int is_last = (slash == NULL) || (slash[1] == '\0');
    int dir_only = (slash != NULL);          /* something follows: must be a dir */
    char path[PATH_MAX];

    if (complen == 0) {
        /* Leading '/' (drive-relative root) or a doubled separator: carry it
         * into the prefix and continue. */
        if (!slash) return;
        if (snprintf(path, sizeof(path), "%s/", prefix) >= (int)sizeof(path))
            return;
        win_glob_walk(path, slash + 1, out);
        return;
    }

    if (!win_has_wildcard(rest, complen)) {
        /* Literal component: no enumeration needed, just extend the path. */
        DWORD attr;
        if (snprintf(path, sizeof(path), "%.*s%.*s", (int)strlen(prefix), prefix,
                     (int)complen, rest) >= (int)sizeof(path))
            return;
        {
            wchar_t wpath[PATH_MAX];
            if (!cl_win_utf8_to_wide(path, wpath, PATH_MAX)) return;
            attr = GetFileAttributesW(wpath);
        }
        if (attr == INVALID_FILE_ATTRIBUTES) return;
        if (is_last) {
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                char marked[PATH_MAX];
                if (snprintf(marked, sizeof(marked), "%s/", path) <
                    (int)sizeof(marked))
                    sv_push(out, marked);    /* GLOB_MARK */
            } else if (!dir_only) {
                sv_push(out, path);
            }
            return;
        }
        if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
        {
            char nextpref[PATH_MAX];
            if (snprintf(nextpref, sizeof(nextpref), "%s/", path) >=
                (int)sizeof(nextpref))
                return;
            win_glob_walk(nextpref, slash + 1, out);
        }
        return;
    }

    /* Wildcard component: enumerate the prefix directory and match by hand.
     * The search mask is always "*" — FindFirstFile's own matching still
     * carries DOS 8.3 short-name quirks (a "*.lisp" mask also matches
     * "foo.lispx" through its short name), which win_wildmatch does not. */
    {
        /* The WIDE enumeration, not FindFirstFileA: the ANSI one reports any
         * name the process code page cannot spell with '?' substituted, so a
         * directory holding a Japanese filename listed as "???.lisp" — a name
         * that matches nothing and opens nothing. */
        WIN32_FIND_DATAW fd;
        HANDLE h;
        char pat[PATH_MAX];
        char search[PATH_MAX];
        char name[PATH_MAX];
        wchar_t wsearch[PATH_MAX];
        if (complen >= sizeof(pat)) return;
        memcpy(pat, rest, complen);
        pat[complen] = '\0';
        if (snprintf(search, sizeof(search), "%s*", prefix) >= (int)sizeof(search))
            return;
        if (!cl_win_utf8_to_wide(search, wsearch, PATH_MAX)) return;
        h = FindFirstFileW(wsearch, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!cl_win_wide_to_utf8(fd.cFileName, name, (int)sizeof(name)))
                continue;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;
            /* A leading dot is not special on Windows, but glob() hides
             * dotfiles unless the pattern itself starts with one — keep that,
             * so DIRECTORY "*.lisp" does not surface editor backups in a
             * .git/ style directory. */
            if (name[0] == '.' && pat[0] != '.')
                continue;
            if (!win_wildmatch(pat, name))
                continue;
            if (snprintf(path, sizeof(path), "%s%s", prefix, name) >=
                (int)sizeof(path))
                continue;
            if (is_last) {
                if (isdir) {
                    char marked[PATH_MAX];
                    if (snprintf(marked, sizeof(marked), "%s/", path) <
                        (int)sizeof(marked))
                        sv_push(out, marked);        /* GLOB_MARK */
                } else if (!dir_only) {
                    sv_push(out, path);
                }
            } else if (isdir) {
                char nextpref[PATH_MAX];
                if (snprintf(nextpref, sizeof(nextpref), "%s/", path) <
                    (int)sizeof(nextpref))
                    win_glob_walk(nextpref, slash + 1, out);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

char **platform_directory(const char *pattern, int *count_out)
{
    StrVec sv;
    char patbuf[1024];
    size_t plen;

    *count_out = 0;
    sv.v = NULL;
    sv.n = 0;
    sv.cap = 0;

    /* Copy the pattern to C memory and bracket the walk: directory
     * enumeration touches the disk (possibly a network share) and the
     * pattern may point into the moving arena.  Oversized patterns are
     * rejected rather than run unbracketed — unlike glob(), the walk below
     * keeps reading `pattern` across every recursion. */
    plen = strlen(pattern);
    if (plen >= sizeof(patbuf))
        return NULL;
    memcpy(patbuf, pattern, plen + 1);
    win_normalize_seps(patbuf);

    cl_gc_enter_safe_region();
    win_glob_walk("", patbuf, &sv);
    cl_gc_leave_safe_region();

    if (sv.n == 0) {
        free(sv.v);
        return NULL;            /* no matches: glob(3) reports this as failure */
    }
    /* NULL-terminate like glob's gl_pathv, which the caller relies on. */
    if (!sv_push(&sv, "")) {
        int i;
        for (i = 0; i < sv.n; i++) free(sv.v[i]);
        free(sv.v);
        return NULL;
    }
    free(sv.v[sv.n - 1]);
    sv.v[sv.n - 1] = NULL;
    *count_out = sv.n - 1;
    return sv.v;
}

const char *platform_realpath(const char *path, char *buf, int bufsize)
{
    char resolved[PATH_MAX];
    DWORD n;
    /* POSIX realpath() fails for a path that does not exist and callers
     * (TRUENAME) depend on that, but GetFullPathName is pure string
     * arithmetic and would happily canonicalise a missing file — so check
     * existence first.  GetFullPathName does not resolve symlinks or
     * junctions; on Windows that is the documented limit of this call. */
    {
        wchar_t wpath[PATH_MAX], wfull[PATH_MAX];
        if (!cl_win_utf8_to_wide(path, wpath, PATH_MAX))
            return NULL;
        if (GetFileAttributesW(wpath) == INVALID_FILE_ATTRIBUTES)
            return NULL;
        n = GetFullPathNameW(wpath, (DWORD)PATH_MAX, wfull, NULL);
        if (n == 0 || n >= (DWORD)PATH_MAX)
            return NULL;
        if (!cl_win_wide_to_utf8(wfull, resolved, (int)sizeof(resolved)))
            return NULL;
    }
    win_normalize_seps(resolved);
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

    /* $HOME is set by MSYS2/Cygwin shells and by anyone who wants to point
     * amilisp's ~ elsewhere; USERPROFILE is what a plain cmd.exe or
     * PowerShell session has. */
    home = getenv("HOME");
    if (!home || !*home) home = getenv("USERPROFILE");
    if (!home || !*home) home = "/";
    hlen = strlen(home);
    plen = strlen(path + 1); /* everything after ~ */

    if ((int)(hlen + plen + 1) > bufsize) return path;
    memcpy(buf, home, hlen);
    /* path+1 is either "" or "/rest..." — copy including NUL */
    memcpy(buf + hlen, path + 1, plen + 1);
    win_normalize_seps(buf);
    return buf;
}

/* --- TCP Socket I/O ---
 *
 * Unlike the Amiga reactor (a fixed 64-slot table bounded by bsdsocket's
 * descriptor table), the Windows host has no such ceiling — a busy server can
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
    SOCKET fd;          /* INVALID_SOCKET = free */
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
    if (base == 0) blk[0].fd = INVALID_SOCKET;   /* slot 0: never claimed */
    for (i = SOCK_BLOCK_SIZE - 1; i >= start; i--) {
        blk[i].fd = INVALID_SOCKET;
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
static PlatformSocket socket_claim_locked(SOCKET fd, int want_buf)
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
        /* Winsock must be started before the first socket call.  Doing it
         * here rather than only in platform_init() means any entry point
         * into this layer works on its own — which is how the C unit tests
         * drive it, and how a library embedding the runtime would. */
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        platform_mutex_init(&socket_table_mutex);
        socket_table_init = 1;
    }
}

/* Defined below; used here to bound a non-blocking connect by its deadline. */
static int socket_wait_ready(SOCKET fd, int timeout_ms, int want_write);

PlatformSocket platform_socket_connect(const char *host, int port, int connect_ms)
{
    struct hostent *he;
    struct sockaddr_in addr;
    SOCKET fd;
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
    if (fd == INVALID_SOCKET) return PLATFORM_SOCKET_INVALID;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect_ms > 0) {
        /* Bounded connect: go non-blocking, fire the handshake, then wait for
         * writability up to connect_ms.  Writable + SO_ERROR==0 means connected;
         * a timeout or a connect error both fail fast.  Restore blocking mode so
         * the rest of the stream code (which recv()s blocking) is unaffected.
         *
         * select(), not the socket_wait_ready() WSAPoll used everywhere else:
         * WSAPoll famously never reports a FAILED non-blocking connect (no
         * POLLOUT, no POLLERR), so a refused connection would burn the whole
         * connect_ms instead of failing immediately.  select() puts the failure
         * in exceptfds, which is why the wait is spelled out here. */
        u_long nb = 1;
        int rc, ok = 0;
        ioctlsocket(fd, FIONBIO, &nb);
        cl_gc_enter_safe_region();
        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        if (rc == 0) {
            ok = 1;                                  /* connected immediately */
        } else if (sock_errno() == SOCK_EINPROGRESS) {
            fd_set wfds, efds;
            struct timeval tv;
            tv.tv_sec  = connect_ms / 1000;
            tv.tv_usec = (connect_ms % 1000) * 1000;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
            FD_SET(fd, &wfds);
            FD_SET(fd, &efds);
            /* nfds is ignored by Winsock (the sets carry their own count). */
            if (select(0, NULL, &wfds, &efds, &tv) > 0 && FD_ISSET(fd, &wfds)) {
                int err = 0;
                socklen_t elen = (socklen_t)sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err,
                               &elen) == 0 && err == 0)
                    ok = 1;
            }
        }
        cl_gc_leave_safe_region();
        nb = 0;
        ioctlsocket(fd, FIONBIO, &nb);
        if (!ok) {
            closesocket(fd);
            return PLATFORM_SOCKET_INVALID;
        }
    } else {
        int rc;
        cl_gc_enter_safe_region();
        rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
        cl_gc_leave_safe_region();
        if (rc < 0) {
            closesocket(fd);
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
            closesocket(fd);
            return PLATFORM_SOCKET_INVALID;
        }
        return sh;
    }
}

/* Flush socket write buffer to the wire */
/* Forward declaration: readiness wait shared by read and write paths. */
static int socket_wait_ready(SOCKET fd, int timeout_ms, int want_write);

static int socket_flush_wbuf(PlatformSocket sh)
{
    IOBuf *b;
    SOCKET fd;
    int wtimeout;
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
        /* Timed drain.  A blocking send() would stall until ALL bytes fit even
         * after the socket reports only a sliver of space, so switch the
         * socket to non-blocking for the drain and pair it with an explicit
         * readiness wait.  Winsock has no per-call MSG_DONTWAIT — non-blocking
         * is a socket mode — hence the bracketing rather than a send() flag.
         * All waits share ONE absolute deadline: a socket can be reported
         * writable while send() still returns WSAEWOULDBLOCK, so a per-call
         * timeout could spin forever — the shared deadline guarantees the loop
         * terminates within wtimeout. */
        uint32_t deadline = platform_time_ms() + (uint32_t)wtimeout;
        u_long nb = 1;
        int drain_rc = 0;
        ioctlsocket(fd, FIONBIO, &nb);
        while (total < b->wlen) {
            ssize_t n = send(fd, b->wbuf + total, b->wlen - (int)total, 0);
            if (n > 0) { total += n; continue; }
            if (n < 0 && sock_errno() == SOCK_EINTR) continue;
            if (n < 0 && sock_errno() == SOCK_EAGAIN) {
                int32_t rem = (int32_t)(deadline - platform_time_ms());
                int rr;
                if (rem <= 0) { drain_rc = PLATFORM_SOCKET_TIMEOUT; break; }
                rr = socket_wait_ready(fd, rem, 1);
                if (rr < 0)  { drain_rc = -1; break; }
                if (rr == 0) { drain_rc = PLATFORM_SOCKET_TIMEOUT; break; }
                continue;
            }
            drain_rc = -1;   /* n == 0 or a real send() error */
            break;
        }
        /* Back to blocking before returning either way: every other path in
         * this file assumes blocking sockets. */
        nb = 0;
        ioctlsocket(fd, FIONBIO, &nb);
        if (drain_rc != 0) {
            cl_gc_leave_safe_region();
            return drain_rc;
        }
    } else {
        while (total < b->wlen) {
            ssize_t n = send(fd, b->wbuf + total, b->wlen - total, 0);
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
    if (sh > 0 && s && s->fd != INVALID_SOCKET) {
        SOCKET fd;
        IOBuf *buf;
        TLSConn *tls;
        socket_flush_wbuf(sh);
        /* Detach the slot and return it to the free-list under the lock, then
         * do the blocking close() and free() outside it. */
        socket_table_lock();
        fd = s->fd;
        buf = s->buf;
        tls = s->tls;
        s->fd = INVALID_SOCKET;
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
        if (fd != INVALID_SOCKET) closesocket(fd);
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
    if (sh > 0 && s && s->fd != INVALID_SOCKET) {
        SOCKET fd;
        IOBuf *buf;
        TLSConn *tls;
        socket_table_lock();
        fd = s->fd;
        buf = s->buf;
        tls = s->tls;
        s->fd = INVALID_SOCKET;
        s->buf = NULL;
        s->tls = NULL;
        s->rtimeout = 0;
        s->wtimeout = 0;
        s->next_free = socket_free_head;
        socket_free_head = (int)sh;
        socket_table_unlock();
        if (tls) tls_conn_close(tls, 0);
        if (fd != INVALID_SOCKET) closesocket(fd);
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
static int socket_wait_ready(SOCKET fd, int timeout_ms, int want_write)
{
    struct pollfd pfd;
    int r;
    short want;
    uint32_t deadline;
    int32_t rem;
    /* poll() has no FD_SETSIZE ceiling, so a host server holding thousands of
     * connections (fd numbers well above 1024) still works — select() does not. */
    if (fd == INVALID_SOCKET) return -1;
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
            if (sock_errno() == SOCK_EINTR) continue; /* retry, same deadline */
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
    if (sh == 0 || !s || s->fd == INVALID_SOCKET)
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
            SOCKET fd = s->fd;
            ssize_t n;
            cl_gc_enter_safe_region();
            if (rtimeout > 0) {
                int rr = socket_wait_ready(fd, rtimeout, 0);
                if (rr <= 0) {
                    cl_gc_leave_safe_region();
                    return (rr == 0) ? PLATFORM_SOCKET_TIMEOUT : -1;
                }
            }
            n = recv(fd, b->rbuf, PLATFORM_IOBUF_SIZE, 0);
            cl_gc_leave_safe_region();
            if (n <= 0) return -1;
            b->rpos = 1;
            b->rlen = (int)n;
            return (unsigned char)b->rbuf[0];
        }
    }
    /* Fallback: no buffer */
    {
        SOCKET fd = s->fd;
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
        n = recv(fd, (char *)&byte, 1, 0);
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
    SOCKET fd;
    struct pollfd pfd;
    int r;
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd == INVALID_SOCKET)
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
        if (r < 0) return (sock_errno() == SOCK_EINTR) ? 0 : -1;
        if (r == 0 || !(pfd.revents & (POLLIN | POLLHUP | POLLERR)))
            return 0;
        return tls_conn_probe(t);       /* 1 data / 0 not yet / 2 EOF */
    }
    fd = s->fd;
    if (fd == INVALID_SOCKET)
        return -1;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    r = poll(&pfd, 1, 0);               /* zero timeout: never blocks */
    if (r < 0) {
        if (sock_errno() == SOCK_EINTR) return 0;  /* "not ready yet" */
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
        if (sock_errno() == SOCK_EAGAIN || sock_errno() == SOCK_EINTR)
            return 0;                   /* spurious wakeup */
        return 2;                       /* error => report as EOF-ish */
    }
}

int platform_socket_write(PlatformSocket sh, int byte)
{
    IOBuf *b;
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd == INVALID_SOCKET)
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
        SOCKET fd = s->fd;
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
        n = send(fd, (const char *)&bb, 1, 0);
        cl_gc_leave_safe_region();
        return (n == 1) ? 0 : -1;
    }
}

int platform_socket_write_buf(PlatformSocket sh, const char *buf, uint32_t len)
{
    IOBuf *b;
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd == INVALID_SOCKET)
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
        SOCKET fd = s->fd;
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
            ssize_t n = send(fd, buf + total, (int)(len - (uint32_t)total), 0);
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

/* "a.b.c.d" -> the address in host byte order; 0 for anything else (the
 * same parser as platform_posix.c's). */
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
    SOCKET fd;
    uint32_t ip = INADDR_ANY;

    if (bind_addr && !parse_ipv4(bind_addr, &ip))
        return PLATFORM_SOCKET_INVALID;

    socket_table_ensure_init();

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) return PLATFORM_SOCKET_INVALID;

    /* No SO_REUSEADDR here, deliberately: on Windows the option does not
     * mean "rebind after TIME_WAIT" as it does on POSIX — it lets ANY
     * process bind a port this one is already listening on and silently
     * steal its connections.  The cost of leaving it off is that a restarted
     * server may have to wait out TIME_WAIT on a fixed port; the cost of
     * turning it on is a hijackable listener. */

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(ip);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        closesocket(fd);
        return PLATFORM_SOCKET_INVALID;
    }
    if (listen(fd, 4) < 0) {
        closesocket(fd);
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
            closesocket(fd);
            return PLATFORM_SOCKET_INVALID;
        }
        return sh;
    }
}

PlatformSocket platform_socket_accept(PlatformSocket listener)
{
    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    SOCKET fd;
    SockSlot *ls = sock_slot(listener);

    if (listener == 0 || !ls || ls->fd == INVALID_SOCKET)
        return PLATFORM_SOCKET_INVALID;

    /* accept() blocks — must run outside the table lock, and as a GC safe
     * region so a thread parked here waiting for a client does not stall a
     * concurrent stop-the-world GC.  This is the SLY read-loop deadlock. */
    {
        SOCKET lfd = ls->fd;
        cl_gc_enter_safe_region();
        fd = accept(lfd, (struct sockaddr *)&caddr, &clen);
        cl_gc_leave_safe_region();
    }
    if (fd == INVALID_SOCKET) return PLATFORM_SOCKET_INVALID;

    {
        PlatformSocket sh;
        socket_table_lock();
        sh = socket_claim_locked(fd, 1);
        socket_table_unlock();
        if (sh == PLATFORM_SOCKET_INVALID) {
            closesocket(fd);
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
    SOCKET fd;
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
    if (fd == INVALID_SOCKET) return PLATFORM_SOCKET_INVALID;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    /* connect() on a datagram socket only records the peer — no handshake,
     * no blocking — so no timeout/safe-region dance is needed. */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        closesocket(fd);
        return PLATFORM_SOCKET_INVALID;
    }

    {
        PlatformSocket sh;
        socket_table_lock();
        sh = socket_claim_locked(fd, 0);   /* no IOBuf: message I/O only */
        socket_table_unlock();
        if (sh == PLATFORM_SOCKET_INVALID) {
            closesocket(fd);
            return PLATFORM_SOCKET_INVALID;
        }
        return sh;
    }
}

int platform_udp_send(PlatformSocket sh, const uint8_t *buf, uint32_t len)
{
    SockSlot *s = sock_slot(sh);
    SOCKET fd;
    ssize_t n;
    if (sh == 0 || !s || s->fd == INVALID_SOCKET) return -1;
    fd = s->fd;
    if (s->wtimeout > 0) {
        int r = socket_wait_ready(fd, s->wtimeout, 1);
        if (r == 0) return PLATFORM_SOCKET_TIMEOUT;
        if (r < 0) return -1;
    }
    cl_gc_enter_safe_region();
    n = send(fd, (const char *)buf, (int)len, 0);
    cl_gc_leave_safe_region();
    return (n == (ssize_t)len) ? 0 : -1;
}

int platform_udp_recv(PlatformSocket sh, uint8_t *buf, uint32_t maxlen)
{
    SockSlot *s = sock_slot(sh);
    SOCKET fd;
    ssize_t n;
    if (sh == 0 || !s || s->fd == INVALID_SOCKET) return -1;
    fd = s->fd;
    if (s->rtimeout > 0) {
        int r = socket_wait_ready(fd, s->rtimeout, 0);
        if (r == 0) return PLATFORM_SOCKET_TIMEOUT;
        if (r < 0) return -1;
    }
    /* recv blocks until a datagram arrives — stay GC-cooperative. */
    cl_gc_enter_safe_region();
    n = recv(fd, (char *)buf, (int)maxlen, 0);
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
    if (sh == 0 || !s || s->fd == INVALID_SOCKET) return -1;
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
    if (sh == 0 || !s || s->fd == INVALID_SOCKET) {
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
    t = tls_conn_start((int)s->fd, params, err, errlen);
    cl_gc_leave_safe_region();
    if (!t) return -1;
    s->tls = t;
    return 0;
}

int platform_tls_active(PlatformSocket sh)
{
    SockSlot *s = sock_slot(sh);
    return (sh > 0 && s && s->fd != INVALID_SOCKET && s->tls) ? 1 : 0;
}

int platform_tls_peer_cert_field(PlatformSocket sh, int field,
                                 char *out, uint32_t outlen)
{
    SockSlot *s = sock_slot(sh);
    if (sh == 0 || !s || s->fd == INVALID_SOCKET || !s->tls) return -1;
    return tls_conn_peer_cert_field(s->tls, field, out, outlen);
}
/* ---- Ctrl-C break-in (see platform_break_pending in platform.h) ----
 * The handler only sets a flag; the VM polls it at safepoints and enters
 * the debugger (CL:BREAK) from well-defined interpreter state.  A second
 * Ctrl-C while the first is still unconsumed force-exits — the escape
 * hatch when the runtime is wedged somewhere the poll can't reach (a hung
 * C loop or blocking syscall).
 *
 * Windows delivers console control events on a thread of its own making
 * rather than by interrupting this one, so — unlike the POSIX SIGINT
 * handler — nothing here has to be async-signal-safe; the flag write is
 * still the only thing done, so both platforms behave identically. */
static volatile LONG break_requested = 0;

static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
        return FALSE;           /* close/logoff/shutdown: default handling */
    if (InterlockedExchange(&break_requested, 1) != 0)
        _exit(130);
    return TRUE;                /* handled: do not terminate the process */
}

int platform_break_pending(void)
{
    if (!break_requested)
        return 0;
    InterlockedExchange(&break_requested, 0);
    return 1;
}

void platform_fpu_setup(void)
{
    /* IEEE double semantics are the hardware default on Windows targets;
     * only the hard-float m68k build needs FPU control setup. */
}

/* Console code pages are process-global but outlive the process in the
 * hosting shell, so the originals are restored on the way out. */
static UINT saved_out_cp = 0;
static UINT saved_in_cp = 0;

static void console_restore_at_exit(void)
{
    if (saved_out_cp) SetConsoleOutputCP(saved_out_cp);
    if (saved_in_cp)  SetConsoleCP(saved_in_cp);
}

void platform_init(void)
{
    WSADATA wsa;
    HANDLE hout;
    DWORD mode;

    /* Winsock must be started before any socket call; nothing later in the
     * runtime checks, so a failure here is reported by every socket
     * operation failing rather than by a crash. */
    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Binary OUTPUT streams: no CRLF translation on the way out.  A newline
     * is one LF byte here exactly as it is on POSIX and AmigaOS — which is
     * what every other Common Lisp on Windows does, what the Windows console
     * itself renders correctly, and what keeps a redirected `amilisp --batch`
     * byte-identical across platforms.  File streams are already binary
     * (platform_file_open opens "rb"/"wb"/"ab").
     *
     * stdin deliberately stays in TEXT mode, and the asymmetry is the point.
     * A Windows console delivers CR LF for Enter, as does every CRLF file or
     * pipe; in binary mode that CR reaches the reader.  platform_read_line
     * strips only '\n', so every exact-match command in the debugger and
     * the inspector (":q", ":c", ":bt", "q", "h" — strcmp, not a parse)
     * silently stopped matching, and READ-LINE returned strings ending in #\Return.
     * Text mode is what makes a typed line look the same as it does on
     * POSIX.  The cost is that raw binary piped into stdin gets CRLF-folded;
     * a TUI does not pay it, because raw mode reads the console handle
     * directly and never goes through stdio at all. */
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    /* amilisp's strings are UTF-8 end to end (CL_WIDE_STRINGS), so tell the
     * console to interpret its bytes that way in both directions — without
     * this, non-ASCII output lands in the legacy OEM code page. */
    saved_out_cp = GetConsoleOutputCP();
    saved_in_cp = GetConsoleCP();
    if (saved_out_cp || saved_in_cp)
        atexit(console_restore_at_exit);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    /* ANSI escapes (the REPL's --color output, and any TUI) are only
     * interpreted when the console is put into virtual-terminal mode.
     * Windows Terminal enables it by default; conhost does not. */
    hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hout && hout != INVALID_HANDLE_VALUE && GetConsoleMode(hout, &mode))
        SetConsoleMode(hout, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void platform_shutdown(void)
{
    /* Restore cooked mode if a TUI died without cleaning up (the atexit
     * handler also covers this, but shutdown runs earlier and matters for
     * the AmigaOS ordering — keep both platforms symmetric). */
    if (tty_raw_active)
        platform_tty_raw(0);
    /* No WSACleanup: GC finalizers can still close socket streams after
     * shutdown, and process teardown releases Winsock anyway. */
}

void platform_cache_clear(void *addr, uint32_t len)
{
    (void)addr; (void)len;
    /* Windows hosts running amilisp don't execute JIT-emitted m68k code;
     * the JIT only compiles when -DJIT_M68K is on (cross build). */
}

/* =============================================================
 * Generic FFI: foreign memory (Windows implementation)
 *
 * On 64-bit Windows, pointers don't fit in uint32_t, so we use a
 * side table that maps handles (1-based indices) to real pointers.
 * ============================================================= */

/*
 * On 64-bit Windows a real pointer doesn't fit in the 32-bit CL_ForeignPtr
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
 * Dynamic libraries + foreign function calls (LoadLibrary + libffi)
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
} Win32Closure;

/* libffi entry point: decode raw C args, hand them to the generic handler,
 * then write the handler's result back where libffi expects it. */
static void win32_closure_tramp(ffi_cif *cif, void *ret, void **args, void *ud)
{
    Win32Closure *pc = (Win32Closure *)ud;
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
    Win32Closure *pc;
    int i;
    (void)arg_regs;   /* C ABI only: every argument is a stack/register C arg */
    if (nargs < 0 || nargs > CL_FFI_MAX_ARGS) return NULL;
    pc = (Win32Closure *)calloc(1, sizeof(Win32Closure));
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
    if (ffi_prep_closure_loc(pc->closure, &pc->cif, win32_closure_tramp,
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
    Win32Closure *pc = (Win32Closure *)closure;
    if (!pc) return;
    if (pc->closure) ffi_closure_free(pc->closure);
    free(pc);
}

/* =============================================================
 * Amiga-specific FFI stubs (not available on Windows)
 * ============================================================= */

uint32_t platform_amiga_open_library(const char *name, uint32_t version)
{
    (void)name; (void)version;
    return 0;  /* Not available on Windows */
}

void platform_amiga_close_library(uint32_t lib_base)
{
    (void)lib_base;
}

uint32_t platform_amiga_call(uint32_t lib_base, int16_t offset,
                              uint32_t *regs, uint16_t reg_mask)
{
    (void)lib_base; (void)offset; (void)regs; (void)reg_mask;
    return 0;  /* Not available on Windows */
}

uint32_t platform_amiga_alloc_chip(uint32_t size)
{
    /* On Windows, chip memory is just regular memory */
    return platform_ffi_alloc(size);
}

/* No MUI on Windows: the push is recorded for tests/test_amiga_mui.lisp. */
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

void platform_amiga_free_chip(uint32_t addr, uint32_t size)
{
    platform_ffi_free(addr, size);
}
