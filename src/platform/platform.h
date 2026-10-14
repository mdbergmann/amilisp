#ifndef CL_PLATFORM_H
#define CL_PLATFORM_H

/*
 * Platform abstraction layer for AmiLisp.
 * Every OS-specific call goes through these functions.
 * Implementations: platform_posix.c (Linux/macOS), platform_amiga.c (AmigaOS 3+)
 */

#include <stddef.h>
#include <stdint.h>

/* GCC lacks Clang's __has_feature builtin; define a no-op fallback so that
 * `__has_feature(address_sanitizer)` and similar probes parse on every
 * compiler. Must be visible wherever those probes appear. */
#ifndef __has_feature
#define __has_feature(x) 0
#endif

/* Suppress unused-parameter warnings portably.
 * vbcc warns on (void)x ("statement has no effect", warning 153),
 * gcc/clang warn on unused parameters without (void)x. */
#ifdef PLATFORM_AMIGA
#define CL_UNUSED(x)
#else
#define CL_UNUSED(x)  (void)(x)
#endif

/* Keep a function out of its caller's stack frame.  Used to hoist large
 * local buffers off recursive paths (reader/compiler recurse once per
 * nesting level; Amiga shell stacks are small and fixed), which only
 * works if the compiler doesn't inline the buffer right back. */
#if defined(__GNUC__) || defined(__clang__)
#define CL_NOINLINE __attribute__((noinline))
#else
#define CL_NOINLINE
#endif

/* The opposite: a helper that MUST dissolve into its caller.  Used for the
 * cursor cuts of cl_alloc's fast path, which are written once and
 * instantiated in the fast path and in the refills — the m68k build is -Os
 * without LTO, where a plain `static inline` with several callers is not
 * reliably inlined, and a call there would be a call on every cons. */
#if defined(__GNUC__) || defined(__clang__)
#define CL_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define CL_ALWAYS_INLINE static
#endif

/* Memory */
void *platform_alloc(unsigned long size);
void  platform_free(void *ptr);

/* Off-heap allocation leak tracer — see src/core/mem_track.c.
 *
 * Built only with -DDEBUG_MEM_TRACK, where it redirects every platform_alloc
 * / platform_free to a wrapper that tags each block with its call site, and
 * AMILISP_MEM_DIAG=1 then prints what is still outstanding at exit, grouped
 * by file:line.  This is the tool for AmigaOS memory loss: nothing there
 * reclaims a process's memory, so a block never freed is Fast RAM the machine
 * loses until reboot, and no arena statistic can see it.
 *
 * mem_track.c defines CL_MEM_TRACK_IMPL so it alone still reaches the real
 * allocator underneath. */
#ifdef DEBUG_MEM_TRACK
void *cl_mem_track_alloc(unsigned long size, const char *file, int line);
void  cl_mem_track_free(void *ptr);
void  cl_mem_track_report(void);
#  ifndef CL_MEM_TRACK_IMPL
#    define platform_alloc(size) cl_mem_track_alloc((size), __FILE__, __LINE__)
#    define platform_free(ptr)   cl_mem_track_free(ptr)
#  endif
#else
#  define cl_mem_track_report() ((void)0)
#endif

/* Console I/O */
void  platform_write_string(const char *str);
/* Push anything still pending on the standard-output handle out to the OS.
 * This is what FINISH-OUTPUT / FORCE-OUTPUT on a console stream reaches, and
 * what the fatal-exit paths call before terminating so a crash-time diagnostic
 * cannot be stranded behind an unflushed buffer.  Both current back-ends
 * already write through (POSIX fflushes per write, AmigaOS uses raw Write()),
 * so today this is cheap insurance rather than a fix — but it is the only
 * thing standing between a redirected log and a lost death point should any
 * layer ever start buffering. */
void  platform_flush_output(void);
int   platform_read_line(char *buf, int bufsize);
int   platform_getchar(void);
void  platform_ungetchar(int ch);
void  platform_drain_input(void);  /* Drain residual data from stdin (AmigaOS CLI leak) */
/* Non-zero iff stdin is an interactive terminal (a real console/tty).
 * Used to gate the interactive C debugger: it may only block in
 * platform_read_line when there is a human at a terminal to answer.  Returns
 * 0 when stdin is a pipe, file or /dev/null (e.g. a `tail -f /dev/null | amilisp`
 * launcher, or a SLY worker), so the debugger falls back to non-interactive
 * reporting instead of deadlocking on a read no one will ever satisfy. */
int   platform_stdin_is_interactive(void);
/* Clear a sticky end-of-file condition on stdin so reading can resume after
 * a terminal-generated EOF (Ctrl-D / Ctrl-Z).  stdio's EOF flag latches: once
 * fgets has seen EOF every later fgets returns NULL without touching the fd,
 * even though a tty happily delivers more input after Ctrl-D.  The debugger
 * calls this when Ctrl-D at the Debug> prompt aborts to top level, so the
 * top-level REPL's next read blocks for fresh input instead of inheriting
 * the EOF and exiting the session (issue #13).  Only meaningful when stdin
 * is interactive; on a closed pipe the next read just returns EOF again. */
void  platform_clear_stdin_eof(void);

/* --- TTY control (raw mode / size / input availability) ---------------
 * Backing for EXT:TTY-RAW-MODE and EXT:TTY-SIZE, and for LISTEN /
 * READ-CHAR-NO-HANG on console streams.  All operate on the process
 * console (POSIX: stdin/stdout; Amiga: Input()/Output()). */

/* Enter (enable != 0) or leave (enable == 0) raw mode: no echo, no line
 * buffering, characters delivered as typed (POSIX: termios; Amiga:
 * SetMode raw).  Returns 0 on success, -1 on failure (console is not an
 * interactive terminal, or the OS refused).  Idempotent in both
 * directions.  The platform layer restores cooked mode on process exit
 * as crash insurance, but callers should pair enable/disable. */
int   platform_tty_raw(int enable);

/* Non-zero iff raw mode is currently active. */
int   platform_tty_raw_active(void);

/* Console size in character cells.  On success fills *cols and *rows and
 * returns 0; returns -1 when the size cannot be determined (not a
 * terminal, or the query failed) — callers pick their own fallback. */
int   platform_tty_size(int *cols, int *rows);

/* Returns 1 if platform_getchar() would return without blocking, else 0.
 * Exact while raw mode is active (raw-mode reads bypass the C library's
 * input buffering).  In cooked mode it also counts what the line reader
 * has already buffered ahead (stdio's buffer on glibc and the BSD libcs
 * including macOS; the DOS FileHandle buffer on AmigaOS) on top of the
 * OS-level probe, so the tail of a pasted multi-line form is reported as
 * pending after platform_read_line returned its first line — that is what
 * the REPL asks before printing a continuation prompt.  Where the libc's
 * buffer is opaque (Windows) a cooked-mode 0 is conservative for input
 * that arrived through a pipe, matching LISTEN's historical behavior; a
 * console/tty is exact everywhere, because canonical reads deliver one line
 * at a time and the rest stays visible to the OS probe. */
int   platform_tty_char_avail(void);

/* File I/O (bulk read) */
char *platform_file_read(const char *path, unsigned long *size_out);

/* Handle-based file I/O (for CL streams) */
typedef uint32_t PlatformFile;
#define PLATFORM_FILE_INVALID 0
#define PLATFORM_FILE_READ    0
#define PLATFORM_FILE_WRITE   1
#define PLATFORM_FILE_APPEND  2

PlatformFile platform_file_open(const char *path, int mode);
void         platform_file_close(PlatformFile fh);
int          platform_file_getchar(PlatformFile fh);
/* Bulk read up to len bytes into buf (which MUST be C memory, not arena —
 * the read may block inside a GC safe region).  Returns the byte count
 * (0 = EOF), or -1 on error/invalid handle. */
int          platform_file_read_buf(PlatformFile fh, char *buf, uint32_t len);
int          platform_file_write_string(PlatformFile fh, const char *str);
int          platform_file_write_char(PlatformFile fh, int ch);
int          platform_file_write_buf(PlatformFile fh, const char *buf, uint32_t len);
int          platform_file_flush(PlatformFile fh);
int          platform_file_eof(PlatformFile fh);
long         platform_file_position(PlatformFile fh);
int          platform_file_set_position(PlatformFile fh, long pos);
long         platform_file_length(PlatformFile fh);

/* TCP Socket I/O */
typedef uint32_t PlatformSocket;
#define PLATFORM_SOCKET_INVALID 0
/* Returned by read/write/flush when a per-socket timeout elapses before the
 * operation could make progress.  Distinct from -1 (EOF/error) so the stream
 * layer can raise EXT:SOCKET-TIMEOUT rather than treating it as a clean EOF. */
#define PLATFORM_SOCKET_TIMEOUT (-2)

/* Connect to host:port and return a socket handle, or PLATFORM_SOCKET_INVALID
 * on error/timeout.  connect_ms > 0 bounds the TCP connect handshake to that
 * many milliseconds (an unreachable host fails fast instead of stalling on the
 * OS connect timeout); connect_ms == 0 blocks until the OS gives up — the
 * historical behaviour. */
PlatformSocket platform_socket_connect(const char *host, int port, int connect_ms);
void           platform_socket_close(PlatformSocket sh);
/* GC-sweep variant: close without flushing buffered output and without
 * entering a GC safe region — only for the stop-the-world sweep finalizer
 * (the normal close's flush brackets a safe region, which must not run on
 * the thread orchestrating the collection). */
void           platform_socket_close_gc(PlatformSocket sh);
int            platform_socket_read(PlatformSocket sh);       /* Read one byte, -1 EOF/err, -2 timeout */
int            platform_socket_write(PlatformSocket sh, int byte); /* Write one byte, 0=ok, -1=err, -2=timeout */
int            platform_socket_write_buf(PlatformSocket sh, const char *buf, uint32_t len); /* 0=ok,-1=err,-2=timeout */
int            platform_socket_flush(PlatformSocket sh);      /* Flush output, 0=ok, -1=err, -2=timeout */
/* Set per-socket read/write timeouts in milliseconds; 0 = block indefinitely
 * (the default).  When a timeout is set, a read/write that cannot make progress
 * within the window returns PLATFORM_SOCKET_TIMEOUT instead of blocking. */
void           platform_socket_set_timeout(PlatformSocket sh, int read_ms, int write_ms);
/* Non-blocking readiness probe: 1 if a read/accept would not block (buffered
 * data, fd readable, or EOF; for a listener: a connection is pending), 0 if it
 * would block, -1 on an invalid handle.  Backs CL:LISTEN for socket streams. */
int            platform_socket_data_available(PlatformSocket sh);

/* Server-side: bind to `port` and start listening.
 * loopback != 0 binds 127.0.0.1 only; otherwise INADDR_ANY (all interfaces).
 * If port == 0 the OS assigns an ephemeral port; the chosen port is written
 * to *actual_port when actual_port != NULL.
 * Returns a listener handle or PLATFORM_SOCKET_INVALID. */
PlatformSocket platform_socket_listen(int port, int loopback, int *actual_port);
/* As platform_socket_listen, bound to ONE address: `addr` is a dotted-quad
 * IPv4 address ("0.0.0.0" is every interface, "127.0.0.1" loopback), or
 * NULL for every interface.  An address that is not dotted-quad, or that
 * no interface of this machine has, fails with PLATFORM_SOCKET_INVALID. */
PlatformSocket platform_socket_listen_addr(int port, const char *addr, int *actual_port);
/* Block until a client connects to `listener`, returning a fresh connection
 * handle (a normal read/write socket) or PLATFORM_SOCKET_INVALID on error. */
PlatformSocket platform_socket_accept(PlatformSocket listener);

/* UDP (datagram) sockets.  A connected UDP socket shares the handle table
 * with TCP sockets — platform_socket_close / _set_timeout /
 * _data_available work on it — but I/O is message-oriented through the two
 * functions below; the byte-stream read/write/flush entry points are never
 * used on a UDP handle. */
PlatformSocket platform_udp_connect(const char *host, int port);
/* Send LEN bytes as one datagram.  0=ok, -1=error, -2=timeout (when a write
 * timeout is set via platform_socket_set_timeout). */
int platform_udp_send(PlatformSocket sh, const uint8_t *buf, uint32_t len);
/* Receive one datagram into buf (blocks; honors the read timeout set via
 * platform_socket_set_timeout).  Returns the received length (truncated to
 * maxlen by the OS), -1 on error/closed socket, -2 on timeout. */
int platform_udp_recv(PlatformSocket sh, uint8_t *buf, uint32_t maxlen);
/* Local endpoint of a connected socket (TCP or UDP): writes the dotted-quad
 * address into ip_out (must hold >= 16 bytes) and the port into *port_out.
 * 0=ok, -1=error. */
int platform_socket_local_endpoint(PlatformSocket sh, char *ip_out, int *port_out);

/* --- TLS (optional, provider loaded at runtime) ---
 *
 * TLS upgrades an existing connected TCP socket in place: after a successful
 * platform_tls_start the ordinary platform_socket_read/_write_buf/_flush/
 * _data_available/_close entry points transparently move ciphertext — the
 * stream layer above needs no TLS awareness at all.
 *
 * The provider is loaded lazily on first use and is optional:
 *   POSIX host    — OpenSSL 1.1.1/3.x via dlopen (libssl/libcrypto)
 *   AmigaOS       — AmiSSL v5 (amisslmaster.library), owned by the reactor
 *   MorphOS       — openssl3.library
 * When no provider is present, platform_tls_available() returns 0 and
 * platform_tls_start fails with a clear message; plain sockets are
 * unaffected. */
typedef struct {
    int         server;        /* 0 = client (connect), 1 = server (accept) */
    int         verify;        /* client: 0 = accept any cert, 1 = verify the
                                * peer chain (+ hostname when set) */
    const char *hostname;      /* client: SNI + hostname verification; NULL to omit */
    const char *ca_file;       /* PEM CA bundle for verification, or NULL */
    const char *ca_path;       /* hashed CA cert directory, or NULL;
                                * neither set => provider default store */
    const char *cert_file;     /* own PEM cert (chain); required for server */
    const char *key_file;      /* PEM private key; NULL = cert_file */
    const char *key_password;  /* passphrase for an encrypted key, or NULL */
    int         timeout_ms;    /* handshake deadline; 0 = block indefinitely */
} PlatformTLSParams;

/* Field selectors for platform_tls_peer_cert_field. */
#define PLATFORM_TLS_CERT_SUBJECT    0
#define PLATFORM_TLS_CERT_ISSUER     1
#define PLATFORM_TLS_CERT_NOT_BEFORE 2
#define PLATFORM_TLS_CERT_NOT_AFTER  3

/* 1 when a TLS provider is present (loading it on first call), else 0. */
int platform_tls_available(void);
/* Human-readable provider/version string ("OpenSSL 3.2.1", "AmiSSL 5.21"),
 * or NULL when unavailable.  The string is static — do not free. */
const char *platform_tls_version(void);
/* Upgrade connected socket `sh` to TLS.  All strings in `params` must point
 * outside the Lisp arena (the handshake parks in a GC safe region).  Returns
 * 0 on success; -1 on failure with a diagnostic in err (always NUL-terminated
 * when errlen > 0).  On failure the socket is left in an undefined half-
 * handshaken state and should be closed. */
int platform_tls_start(PlatformSocket sh, const PlatformTLSParams *params,
                       char *err, uint32_t errlen);
/* 1 if `sh` is TLS-upgraded, 0 if plain/invalid. */
int platform_tls_active(PlatformSocket sh);
/* Copy a peer-certificate field (PLATFORM_TLS_CERT_*) into out as a
 * NUL-terminated string.  0=ok, -1 = no TLS/no peer cert/bad field. */
int platform_tls_peer_cert_field(PlatformSocket sh, int field,
                                 char *out, uint32_t outlen);

/* --- Page write-watch (generational GC dirty tracking; POSIX only) ---
 * The generational collector (CL_GENGC, see core/mem.h) tracks old→young
 * stores by hardware page protection instead of a source-level write
 * barrier: old-space pages are made read-only after each GC; the first
 * store to a clean page faults, the handler records the page in a dirty
 * bitmap and re-enables writes, and the store retries.  AmigaOS (no MMU
 * on the 68020 target) does not implement these — they are only
 * referenced from CL_GENGC code, which is compiled out there. */

/* Page-aligned region allocation (mmap).  Returns NULL on failure. */
void *platform_alloc_pages(uint32_t size);
void  platform_free_pages(void *ptr, uint32_t size);
uint32_t platform_page_size(void);

/* Install the process write-fault handler (SIGSEGV/SIGBUS) covering
 * [base, base+len).  dirty_bitmap holds 1 bit per page of the region
 * (len/page_size/8 bytes, caller-owned); a faulting store to a protected
 * page atomically sets its bit, re-enables the page and returns.  Faults
 * outside the region chain to the previous handler/default disposition.
 * Returns 0 on success. */
int  platform_write_watch_install(uint8_t *base, uint32_t len,
                                  volatile uint8_t *dirty_bitmap);
void platform_write_watch_remove(void);

/* Protect/unprotect [addr, addr+len) (page-aligned) against writes.
 * Returns 0 on success. */
int  platform_page_protect(uint8_t *addr, uint32_t len, int readonly);

/* Timing */
uint32_t platform_time_ms(void);   /* Monotonic milliseconds (for elapsed time) */
/* Monotonic microseconds (for elapsed time).  64-bit so accumulated phase
 * timers (GC diagnostics) never wrap in practice; NOT for heap objects.
 * AmigaOS resolution is the 1/50s DateStamp tick scaled to microseconds. */
uint64_t platform_time_us(void);
/* Process CPU time (user+system) in milliseconds.  On platforms without
 * per-task CPU accounting (AmigaOS) this falls back to wall-clock time. */
uint32_t platform_run_time_ms(void);
void     platform_sleep_ms(uint32_t milliseconds);
uint32_t platform_universal_time(void); /* Seconds since 1900-01-01 00:00:00 UTC */

/* File system operations */
int      platform_file_exists(const char *path);
int      platform_file_is_directory(const char *path);
int      platform_file_delete(const char *path);
int      platform_file_rename(const char *oldpath, const char *newpath);
/* Mark PATH as a program the user can start (EXT:SAVE-IMAGE :EXECUTABLE T
 * wrote it).  POSIX sets the x bits the umask allows; AmigaOS and Windows
 * have nothing to do (a new file is executable there).  0 on success. */
int      platform_file_make_executable(const char *path);
uint32_t platform_file_mtime(const char *path); /* Universal time of last mod, 0 on error */
int      platform_mkdir(const char *path);       /* Create single directory, 0=success */

/* Directory listing — returns NULL-terminated array of names (caller frees each + array) */
char **platform_directory(const char *pattern, int *count_out);

/* Resolve path to absolute — returns static buffer or NULL on error */
const char *platform_realpath(const char *path, char *buf, int bufsize);

/* Expand leading ~ to home directory ($HOME on POSIX, PROGDIR: on Amiga).
 * Returns buf if expansion occurred, or the original path if no ~ prefix. */
const char *platform_expand_home(const char *path, char *buf, int bufsize);

/* Environment.  platform_getenv_raw is the platform's own lookup (getenv,
 * GetVar); platform_getenv (string_utils.c) is what the runtime calls: it
 * adds the fallback from an AMILISP_* name to its CLAMIGA_* spelling, the
 * name every variable had before the rename to AmiLisp (0.12), so a user's
 * environment keeps working.  buf/bufsize hold the value where the platform
 * has to copy it (AmigaOS); POSIX and Win32 return the process environment's
 * own string. */
const char *platform_getenv_raw(const char *name, char *buf, int bufsize);
const char *platform_getenv(const char *name, char *buf, int bufsize);
const char *platform_getenv_s(const char *name);   /* static buffer, one-shot reads */

/* Free system memory in bytes, or 0 where the platform has no meaningful
 * answer (POSIX/Windows, where a process's memory is reclaimed at exit and
 * "free RAM" is a kernel bookkeeping figure, not something amilisp can leak).
 *
 * On AmigaOS this is AvailMem(MEMF_ANY) — the real, shared system pool.
 * There is no per-process reclaim there: whatever amilisp does not hand back
 * before it exits is gone until the machine is rebooted.  Sampling this at
 * startup and again at the end of shutdown is the ground truth for "did this
 * run leak?", and is what AMILISP_MEM_DIAG=1 reports. */
unsigned long platform_mem_available(void);

/* Break-request poll (Ctrl-C).  Returns nonzero — and consumes the request —
 * when the user asked to interrupt the running program: SIGINT on POSIX
 * (a second SIGINT while one is still pending force-exits the process),
 * SIGBREAKF_CTRL_C on the calling task on AmigaOS/MorphOS.  Polled by the
 * VM at loop back-edges and calls (counter-gated), so it may be called
 * often: it must stay cheap and must have no side effect beyond consuming
 * the pending request. */
int platform_break_pending(void);

/* Directory of the running executable as a prefix ready for direct
 * concatenation with a relative path: "<dir>/" (trailing slash included) on
 * POSIX, "PROGDIR:" on AmigaOS.  Symlinks to the executable are resolved.
 * Used to locate the bundled lib/ regardless of the process cwd.  Returns
 * buf on success, NULL when the location cannot be determined. */
const char *platform_executable_prefix(char *buf, int bufsize);

/* The running executable itself, as a path another process can start it
 * by: "/usr/local/bin/amilisp" (symlinks resolved) on POSIX, "C:/.../
 * amilisp.exe" on Windows, "PROGDIR:amilisp" on AmigaOS.  Backs
 * EXT:EXECUTABLE-PATH, which is how the host Clamacs starts a second
 * amilisp on the binary it runs on.  Returns buf, or NULL when the path
 * cannot be determined. */
const char *platform_executable_path(char *buf, int bufsize);

/* Remaining C stack in bytes at the point of call, or -1 when the platform
 * cannot tell (POSIX — big default stacks plus OS guard pages make the
 * generic budget in cl_check_c_stack sufficient there).  On AmigaOS this
 * measures against the task's real stack bounds (tc_SPLower): the shell
 * `stack` is small and fixed, so deep recursion (compiler, VM) must be
 * turned into a clean Lisp error before it silently corrupts memory. */
long platform_stack_headroom(void);

/* CPU store self-test: replay ROUNDS times, into a static buffer (NOT the
 * stack: cpu_store_probe_m68k.s says why) at every alignment mod 16, the
 * five-store sequence gcc emits for a function prologue that initialises
 * locals through a post-incremented pointer (`move.l (a4),(a0)+ ; move.l
 * 4(a4),(a0)+ ; move.l <abs>,(a0)+ ; clr.l (a0)+ ; clr.l (a0)`), and count
 * the replays after which the five words do not read back as written.  A
 * sound CPU answers 0.  The Apollo 68080
 * (Vampire V4, core 10760, 2026-09-19) drops or delays the last store of
 * that chain whenever the absolute source's address mod 64 is one of a
 * fixed set of alignments (verify/realamiga/storeprobe.c, PROBES.md); the
 * data hunk lands somewhere else on every launch, so on that core the
 * test flags about every second launch (a sweep over several source
 * addresses would flag every launch but froze the machine -- see
 * cpu_store_probe_m68k.s).  In amilisp that was a C local reading back
 * as whatever dead data its slot held, i.e. random "not a function" /
 * "too few arguments" errors.  Only the m68k build has the test; every
 * other platform answers 0.  main.c runs it once at startup (a few
 * thousand replays, milliseconds) and warns; (ext:%cpu-store-selftest)
 * repeats it. */
uint32_t platform_cpu_store_selftest(uint32_t rounds);

#if defined(PLATFORM_AMIGA) && !defined(PLATFORM_MORPHOS)
/* The m68k main-stack swap, in assembly (stack_swap_m68k.s): run FN on
 * the stack SS describes and come back, or leave the process from it.
 * See platform_run_main / platform_process_exit for why not C. */
struct StackSwapStruct;
int  platform_stack_call(struct StackSwapStruct *ss, int (*fn)(int, char **),
                         int argc, char **argv);
void platform_stack_exit(struct StackSwapStruct *ss, void *mem, int code);
#endif

/* Directory LEVELS above the executable's directory, as a prefix ready for
 * direct concatenation with a relative path (trailing separator included).
 * On AmigaOS this resolves through dos.library ParentDir — the "PROGDIR:"
 * prefix cannot express a parent climb ("PROGDIR://" is NOT parent-of-parent;
 * climb slashes only apply to cwd-relative paths).  On POSIX it appends
 * "../" per level to the executable prefix.  Returns 1 on success. */
int platform_executable_ancestor_prefix(int levels, char *buf, int bufsize);

/* Subprocess execution */
int platform_system(const char *command);

/* Current working directory (returns length, 0 on error) */
int platform_getcwd(char *buf, int bufsize);

/* The program's arguments when the OS gave it none on a command line.
 * AmigaOS: a Workbench start hands main() argc == 0 and the WBStartup
 * message as argv; this rewrites both — argv[0], the ARGS tool type of the
 * tool's icon and then of every project icon (split as a command line),
 * `--`, one full path per project — opens the console a WINDOW tool type
 * names, and returns 1.  Returns 0, leaving argc/argv alone, on a CLI
 * start and on every other platform. */
int platform_startup_args(int *argc, char ***argv);

/* The C stack the runtime needs on the main thread: the reader and the
 * compiler recurse per nesting level, and a 64K stack fails the GUI load
 * path (cleanly, at the recursion guard) — README "Amiga Stack". */
#define PLATFORM_MAIN_STACK_MIN (128u * 1024u)

/* Run the program's real main on a C stack of at least
 * PLATFORM_MAIN_STACK_MIN bytes.  m68k AmigaOS: a Shell gives a process
 * whatever `stack` says (4K by default) and Workbench the icon's, so this
 * swaps to a stack of its own when the one it was given is smaller (exec
 * StackSwap) and gives it back when FN returns or platform_process_exit
 * runs.  Everywhere else the stack is ample and this just calls FN. */
int platform_run_main(int (*fn)(int, char **), int argc, char **argv);

/* End the process NOW, without the C runtime's post-main teardown (the
 * 68k exit path and the workers-still-alive path need that).  Flushes C
 * stdio first; the Amiga implementation also closes the consoles a
 * Workbench start opened, unlocks its current directory and hands the
 * swapped stack back — libnix does that from its exit list, which _exit
 * skips.  Never returns. */
void platform_process_exit(int code);

/* Lifecycle */
void  platform_init(void);
void  platform_shutdown(void);

/* Final platform teardown, called only once NO worker thread is left alive
 * (main.c runs it after cl_thread_shutdown, past the point where a surviving
 * worker forces an early _exit).  Kept separate from platform_shutdown for
 * exactly that reason: it takes away shared state — open DOS file handles and
 * their buffers, the public ARexx port, the platform's own mutexes — and
 * doing that under a live thread is the "yank shared state out from under
 * running code" hazard the shutdown path is otherwise careful to avoid.
 *
 * Real work only on AmigaOS, where nothing is reclaimed when a process ends.
 * A no-op on POSIX and Windows, where the kernel closes the descriptors and
 * frees the address space. */
void  platform_release_resources(void);

/* Put the FPU control state into the mode the runtime expects.  Real work
 * only on the hard-float m68k build (FPU=1: sets the 68881/68882 FPCR to
 * double-precision rounding so C doubles behave as strict IEEE doubles);
 * a no-op everywhere else.  FPU control registers are per-task context on
 * AmigaOS, so this must run once in EVERY OS thread that executes Lisp —
 * platform_init() covers the main task, the thread entry point the rest. */
void  platform_fpu_setup(void);

/* =============================================================
 * Generic FFI: foreign memory access
 * ============================================================= */

/* Foreign memory allocation/deallocation.
 * Returns a handle (POSIX: side-table index, Amiga: raw address).
 * Returns 0 on failure. */
uint32_t platform_ffi_alloc(uint32_t size);
void     platform_ffi_free(uint32_t handle, uint32_t size);

/* Resolve handle to a dereferenceable address.
 * On Amiga: identity (handle IS the address).
 * On POSIX: looks up side table.
 * Returns NULL on invalid handle. */
void    *platform_ffi_resolve(uint32_t handle);

/* Register an externally-owned real pointer (dlsym result, value returned
 * from a foreign call, computed via pointer arithmetic, etc.) so it can be
 * referenced by a 32-bit handle.  The memory is NOT owned by us and must
 * not be freed when the handle is released.
 *   POSIX: inserts PTR into the side table, returns a fresh handle.
 *          PTR == NULL returns handle 0 (the canonical null pointer).
 *   Amiga: identity — returns (uint32_t)PTR (no table needed).
 * Returns 0 on table-full / NULL ptr. */
uint32_t platform_ffi_register(void *ptr);

/* Release a handle obtained from platform_ffi_register WITHOUT freeing the
 * underlying memory (it is owned elsewhere).  Used by the GC finalizer to
 * reclaim side-table slots for transient foreign pointers.
 *   POSIX: frees the side-table slot.
 *   Amiga: no-op (handles are raw addresses; nothing to reclaim). */
void     platform_ffi_release(uint32_t handle);

/* Peek/poke at handle + byte offset.
 * The handle must come from platform_ffi_alloc or (on Amiga) a raw address. */
uint32_t platform_ffi_peek32(uint32_t handle, uint32_t offset);
uint16_t platform_ffi_peek16(uint32_t handle, uint32_t offset);
uint8_t  platform_ffi_peek8(uint32_t handle, uint32_t offset);
void     platform_ffi_poke32(uint32_t handle, uint32_t offset, uint32_t val);
void     platform_ffi_poke16(uint32_t handle, uint32_t offset, uint16_t val);
void     platform_ffi_poke8(uint32_t handle, uint32_t offset, uint8_t val);

/* =============================================================
 * Generic FFI: dynamic libraries + foreign function calls (host)
 *
 * Implemented on POSIX via dlopen/dlsym + libffi.  On Amiga these are
 * stubs that signal "unsupported" (the Amiga path uses the library-vector
 * model — platform_amiga_call — instead).
 * ============================================================= */

/* dlopen NAME (NULL = the global/default symbol namespace).  Returns a
 * handle usable with platform_ffi_dlsym / platform_ffi_dlclose, or 0 on
 * failure.  The handle is a side-table entry (POSIX) like other pointers. */
uint32_t platform_ffi_dlopen(const char *name);

/* dlsym: look up symbol NAME.  LIB_HANDLE 0 searches the default namespace.
 * Returns a side-table handle to the symbol's address, or 0 if not found. */
uint32_t platform_ffi_dlsym(uint32_t lib_handle, const char *name);

/* dlclose a handle from platform_ffi_dlopen. */
void     platform_ffi_dlclose(uint32_t lib_handle);

/* Primitive C types the generic call/marshaling layer understands. */
typedef enum {
    CL_FFI_VOID = 0,
    CL_FFI_I8,  CL_FFI_U8,
    CL_FFI_I16, CL_FFI_U16,
    CL_FFI_I32, CL_FFI_U32,
    CL_FFI_I64, CL_FFI_U64,
    CL_FFI_FLOAT, CL_FFI_DOUBLE,
    CL_FFI_POINTER
} CLFFIType;

/* A single argument/return slot, interpreted per its CLFFIType. */
typedef union {
    int8_t   i8;  uint8_t  u8;
    int16_t  i16; uint16_t u16;
    int32_t  i32; uint32_t u32;
    int64_t  i64; uint64_t u64;
    float    f;   double   d;
    void    *p;
} CLFFIValue;

/* Upper bound on argument count for a single generic foreign call. */
#define CL_FFI_MAX_ARGS 32

/* Call the C function at FN with NARGS arguments (arg_types[i]/arg_vals[i]).
 * NFIXED is the count of fixed args for a variadic call (NFIXED == NARGS for
 * a non-variadic call).  The result is written to *ret_val, interpreted per
 * RET_TYPE.  Returns 0 on success, nonzero if FFI calls are unsupported on
 * this platform or the call could not be prepared. */
int platform_ffi_call(void *fn, CLFFIType ret_type, CLFFIValue *ret_val,
                      int nargs, int nfixed,
                      const CLFFIType *arg_types, const CLFFIValue *arg_vals);

/* Callback (Lisp-as-C-function) support.
 *
 * The handler is invoked when foreign code calls the trampoline: ARGS holds
 * the decoded C arguments (one per arg type), and the handler writes the
 * result into *RET (interpreted per the closure's return type).  USER_DATA
 * is passed through verbatim. */
typedef void (*platform_ffi_cb_handler)(void *user_data,
                                        const CLFFIValue *args,
                                        CLFFIValue *ret);

/* Where a callback argument arrives (ARG_REGS below): the C stack, or a
 * 68k register — 0..7 = d0..d7, 8..14 = a0..a6 — for the AmigaOS
 * register conventions (a struct Hook entry gets hook/object/message in
 * a0/a2/a1, a BOOPSI dispatcher class/object/message the same way). */
#define CL_FFI_REG_STACK (-1)

/* Build an executable trampoline callable from C with the given signature.
 * ARG_REGS is NULL (every argument on the C stack) or an array of NARGS
 * entries per the encoding above; platforms without register-passing
 * conventions (POSIX, Windows) ignore it, so a hook entry built there is
 * a plain C function of (hook, object, message).  Returns the callable
 * code address (wrap it as a foreign pointer), or NULL on failure /
 * unsupported.  *OUT_CLOSURE receives an opaque handle to pass to
 * platform_ffi_free_closure.  The trampoline zeroes the result slot
 * before invoking HANDLER, so a handler that declines leaves 0 / NULL. */
void *platform_ffi_make_closure(CLFFIType ret_type, int nargs,
                                const CLFFIType *arg_types,
                                const int8_t *arg_regs,
                                platform_ffi_cb_handler handler,
                                void *user_data, void **out_closure);

/* Free a closure created by platform_ffi_make_closure. */
void  platform_ffi_free_closure(void *closure);

/* =============================================================
 * Amiga-specific FFI: shared library calls
 * ============================================================= */

/* Open/close AmigaOS shared library.
 * Returns library base as uint32_t (0 on failure).
 * On POSIX: stubs that return 0 / do nothing. */
uint32_t platform_amiga_open_library(const char *name, uint32_t version);
void     platform_amiga_close_library(uint32_t lib_base);

/* Call an AmigaOS library function via register dispatch.
 * regs[0..7] = d0..d7, regs[8..13] = a0..a5.
 * reg_mask: bitmask of which registers to load (bit 0=d0, ..., bit 13=a5).
 * Returns d0 result.
 * On POSIX: returns 0 (not supported). */
uint32_t platform_amiga_call(uint32_t lib_base, int16_t offset,
                              uint32_t *regs, uint16_t reg_mask);

/* Amiga chip memory allocation (MEMF_CHIP|MEMF_CLEAR).
 * On POSIX: same as platform_ffi_alloc. */
uint32_t platform_amiga_alloc_chip(uint32_t size);
void     platform_amiga_free_chip(uint32_t addr, uint32_t size);

/* DoMethod(APP, PUSH_METHOD, OBJECT, 2, METHOD, VALUE) -- MUI's
 * MUIM_Application_PushMethod, the one call MUI allows from a task that is
 * not the application's own: the method is queued and run by the
 * application's input loop.  Called from C that runs on such a task (the
 * native string-key hook, invoked by Intuition from input.device), so it
 * must not touch Lisp.  On AmigaOS the app's class dispatcher is invoked
 * through utility.library's CallHookPkt; on POSIX / Windows the call is
 * recorded for the tests (platform_amiga_last_pushed_method). */
void     platform_amiga_push_method(uint32_t app, uint32_t push_method,
                                    uint32_t object, uint32_t method,
                                    uint32_t value);

/* Called on the Lisp task before a hook that will push is handed out:
 * opens what the push needs (utility.library on AmigaOS).  Returns 1 when
 * pushes can be made, 0 when not (the hook is then refused).  Always 1 on
 * the host builds. */
int      platform_amiga_push_method_prepare(void);

/* DoSuperMethodA(CL, OBJECT, MESSAGE): hand a BOOPSI message to the
 * dispatcher of CL's superclass and return its result.  Pure C -- what a
 * method-filtered dispatcher (builtins_ffi.c) does with the methods its
 * Lisp function did not ask for.  On AmigaOS the superclass's dispatcher
 * Hook is called through utility.library's CallHookPkt, which
 * platform_amiga_push_method_prepare must have opened.  On POSIX /
 * Windows there is no BOOPSI: CL is read as a block holding a native
 * pointer at offset 24 (cl_Super) to a block holding a native pointer at
 * offset 8 (h_Entry), a C function of (hook, object, message) returning
 * a uint32 -- the layout tests/test_amiga_boopsi.lisp builds by hand. */
uint32_t platform_amiga_do_super_method(void *cl, void *object, void *message);

/* Host builds only: the arguments of the most recent
 * platform_amiga_push_method into OUT[0..4] (app, push_method, object,
 * method, value).  Returns 0 when nothing was pushed since the last call
 * (the record is cleared on read), else 1.  On AmigaOS always 0. */
int      platform_amiga_last_pushed_method(uint32_t out[5]);

/* =============================================================
 * Amiga-specific: ARexx host port  (platform_amiga_rexx.c)
 * =============================================================
 *
 * Transport for the development ARexx port — an editor macro sends a
 * command string, amilisp replies with a return code and (when the code is
 * 0) a result string.  Amiga/MorphOS builds only: platform_amiga_rexx.c is
 * not in the host source list, so these have no definition on POSIX and
 * every caller sits behind #ifdef PLATFORM_AMIGA.
 */

/* Status codes.  0 = success, negative = failure; run through
 * platform_arexx_strerror() for a message fit to show a user. */
#define PLATFORM_AREXX_OK          0
#define PLATFORM_AREXX_ALREADY   (-1)   /* a port is already open here */
#define PLATFORM_AREXX_NOLIB     (-2)   /* rexxsyslib.library missing */
#define PLATFORM_AREXX_NOMEM     (-3)
#define PLATFORM_AREXX_NONAME    (-4)   /* every candidate port name taken */
#define PLATFORM_AREXX_NOTOWNER  (-5)   /* wait() called from the wrong task */
#define PLATFORM_AREXX_NOTOPEN   (-6)
#define PLATFORM_AREXX_NOPORT    (-7)   /* send(): no such public port */

/* ARexx severity ladder used for rm_Result1 (see rexx/storage.h and the
 * protocol note in platform_amiga_rexx.c).  Kept below/above ARexx's default
 * FAILAT of 10 deliberately: warnings must not abort a macro, errors must. */
#define PLATFORM_AREXX_RC_OK       0
#define PLATFORM_AREXX_RC_WARN     5
#define PLATFORM_AREXX_RC_ERROR   10
#define PLATFORM_AREXX_RC_FATAL   20

const char *platform_arexx_strerror(int code);

/* Create the public port, claiming BASENAME (upcased) or the first free
 * BASENAME.<n>.  MUST be called from the task that will wait on it — exec
 * binds the port's signal to its creator.  The chosen name is copied to
 * name_out.  Returns PLATFORM_AREXX_OK or a negative code. */
int  platform_arexx_open(const char *basename, char *name_out, int name_size);

/* Remove the port, replying to anything still queued, and release the
 * message port, wake signal and library.  Call from the owning task. */
void platform_arexx_close(void);

int  platform_arexx_is_open(void);
int  platform_arexx_port_name(char *buf, int bufsize);

/* Ask the owning task to leave platform_arexx_wait().  Safe from any task —
 * this is how another thread shuts the handler down. */
void platform_arexx_request_stop(void);
int  platform_arexx_stop_requested(void);

/* Block until a command arrives.  Returns 1 with *cmd_out pointing at the
 * command string (owned by the sender, valid until the matching reply), 0
 * when woken by platform_arexx_request_stop(), or a negative status code.
 * The Wait is GC-safe-region bracketed. */
int  platform_arexx_wait(const char **cmd_out);

/* Answer the message returned by the last wait().  A result string is only
 * transmitted when rc is 0 and the sender asked for one (ARexx protocol). */
void platform_arexx_reply(int32_t rc, const char *result, uint32_t result_len);

/* Post a command to a public ARexx host port and wait for its reply — the
 * sending half of the protocol, used by the test suite to exercise the port
 * in-process and available to Lisp for driving other Amiga applications. */
int  platform_arexx_send(const char *portname, const char *cmd,
                         int32_t *rc_out, char *result, int result_size,
                         int32_t *rc2_out);

/* References currently held on rexxsyslib (the open port + every send or
 * reply in progress, from any thread); 0 means it is closed.  Test hook. */
uint32_t platform_arexx_lib_users(void);

/* exec Wait(MASK) bracketed in a GC safe region; returns the signals
 * received.  A Lisp thread parked in a raw Wait() (AMIGA.RAW.EXEC:WAIT is a
 * plain library call) is invisible to a stop-the-world collection started
 * by another thread: the initiator waits for a safepoint the sleeper cannot
 * reach until some signal wakes it, so a GUI event loop with a worker
 * thread beside it would stall every collection.  This is the Wait an
 * event loop must use (AMIGA:WAIT-SIGNALS, the MUI and ReAction loops). */
uint32_t platform_wait_signals(uint32_t mask);

/* Flush I/D caches for a freshly written code buffer.
 * Required on AmigaOS 68040/060 after emitting JIT code — calls
 * CacheClearU() so the CPU doesn't execute stale instruction-cache
 * lines.  No-op on 68020/030 and on POSIX. */
void     platform_cache_clear(void *addr, uint32_t len);

#ifdef JIT_A64
/* Executable memory for the AArch64 JIT (specs/native-backend-a64.md),
 * used only through src/jit/codeheap.c.  platform_jit_map returns the
 * executable view of BYTES bytes and stores in *WRITABLE the address code
 * is written through:
 *   - macOS: one MAP_JIT mapping, so both are the same address; a thread
 *     writes it between platform_jit_write_begin and _end (a per-thread
 *     switch, so other threads keep executing it meanwhile).
 *   - Linux: a memfd mapped twice, read+write and read+execute, so no page
 *     is ever writable and executable at once; write_begin/_end are no-ops.
 * platform_jit_flush (on the executable view) makes the written
 * instructions visible to fetch -- on AArch64 the instruction and data
 * caches are not coherent. */
void    *platform_jit_map(uint32_t bytes, void **writable); /* NULL on failure */
void     platform_jit_unmap(void *exec, void *writable, uint32_t bytes);
void     platform_jit_write_begin(void);
void     platform_jit_write_end(void);
void     platform_jit_flush(void *addr, uint32_t len);
#endif

#endif /* CL_PLATFORM_H */
