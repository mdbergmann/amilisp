#ifndef CL_WIN32_COMPAT_H
#define CL_WIN32_COMPAT_H

/*
 * Thin POSIX shims for the Windows (mingw-w64) build.
 *
 * The Windows platform layer (platform_win32.c) and the OpenSSL TLS backend
 * (tls_openssl.c) are otherwise line-for-line the POSIX implementations.  Two
 * POSIX facilities have no mingw header at all, and both have exact Win32
 * counterparts, so they are shimmed here rather than #ifdef'd through the
 * call sites:
 *
 *   dlopen/dlsym/dlclose/dlerror -> LoadLibraryA/GetProcAddress/FreeLibrary
 *   poll()                       -> WSAPoll()  (sockets only, which is all
 *                                   amilisp ever polls)
 *
 * Winsock's own errno lives in WSAGetLastError(), NOT in the C errno the
 * mingw CRT keeps for file I/O, so socket call sites use sock_errno() and the
 * SOCK_E* aliases below instead of errno/EAGAIN/EINTR.
 *
 * Include this INSTEAD of <dlfcn.h>/<poll.h>; it pulls in winsock2.h first,
 * which must precede <windows.h> (windows.h would otherwise drag in the
 * incompatible winsock 1.1 declarations).
 */

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00      /* Windows 10: GetCurrentThreadStackLimits */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <stddef.h>
#include <direct.h>     /* _mkdir, before the mkdir() macro below shadows it */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* ---- dlopen family ---------------------------------------------------
 * The flags are accepted and ignored: LoadLibraryA already resolves eagerly
 * (RTLD_NOW) and Windows has no two-namespace split (RTLD_GLOBAL). */
#define RTLD_NOW      0x0002
#define RTLD_LAZY     0x0001
#define RTLD_GLOBAL   0x0100
#define RTLD_DEFAULT  ((void *)0)

void       *cl_win_dlopen(const char *name, int flags);
void       *cl_win_dlsym(void *lib, const char *name);
int         cl_win_dlclose(void *lib);
const char *cl_win_dlerror(void);

#define dlopen(name, flags) cl_win_dlopen((name), (flags))
#define dlsym(lib, name)    cl_win_dlsym((lib), (name))
#define dlclose(lib)        cl_win_dlclose(lib)
#define dlerror()           cl_win_dlerror()

/* ---- poll ------------------------------------------------------------
 * winsock2.h already declares `struct pollfd` and the POLL* bits; only the
 * function name differs.  WSAPoll is Vista+.
 *
 * Caveat carried by every caller: WSAPoll does NOT report a failed
 * non-blocking connect() as POLLOUT|POLLERR (a long-standing Windows
 * defect), so the bounded-connect path in platform_win32.c uses select()
 * with an exceptfds set instead.  For ordinary readable/writable waits on
 * an established socket WSAPoll is correct. */
#define poll(fds, nfds, timeout) WSAPoll((fds), (nfds), (timeout))

/* ---- environment ------------------------------------------------------
 * The Windows CRT has putenv but not POSIX setenv/unsetenv.  Both views of
 * the environment are updated: _putenv_s for what getenv() sees, and
 * SetEnvironmentVariableA for what a child process inherits. */
int cl_win_setenv(const char *name, const char *value, int overwrite);
int cl_win_unsetenv(const char *name);

#define setenv(n, v, o) cl_win_setenv((n), (v), (o))
#define unsetenv(n)     cl_win_unsetenv(n)

/* ---- UTF-8 <-> UTF-16 path conversion --------------------------------
 * amilisp carries paths as UTF-8 on every platform.  The CRT's narrow file
 * calls (fopen, stat, _mkdir) honour that here because main() puts the C
 * locale in UTF-8 mode, but the Win32 "A" entry points do NOT: they use the
 * process ANSI code page whatever the CRT locale says, so a path they cannot
 * represent comes back with '?' substituted — a directory listing of a
 * Japanese filename produced "???".  Anything calling Win32 directly goes
 * through these and the "W" entry point instead.
 *
 * Both return 0 on failure (including "would not fit"), so a caller can
 * treat it as "this path is not usable" without a second error path. */
int cl_win_utf8_to_wide(const char *utf8, wchar_t *out, int out_chars);
int cl_win_wide_to_utf8(const wchar_t *wide, char *out, int out_bytes);

/* ---- mkdir -----------------------------------------------------------
 * POSIX mkdir takes a permission mode; the CRT's takes only the path
 * (Windows uses ACLs, not permission bits), so the mode is dropped.
 * <direct.h> is included above so its own declaration is already parsed
 * before this function-like macro could rewrite it. */
#define mkdir(path, mode) _mkdir(path)

/* ---- pipe ------------------------------------------------------------
 * The CRT spells POSIX pipe(2) as _pipe(fds, size, mode); this wrapper picks
 * a 64 KB binary pipe, which is what the callers assume of a POSIX one. */
int cl_win_pipe(int fds[2]);

#define pipe(fds) cl_win_pipe(fds)

/* ---- memmem ----------------------------------------------------------
 * A GNU extension the Windows CRT does not have. */
void *cl_win_memmem(const void *hay, size_t haylen,
                    const void *needle, size_t needlelen);

#define memmem(h, hl, n, nl) cl_win_memmem((h), (hl), (n), (nl))

/* ---- socket errno ---------------------------------------------------- */
#define sock_errno()      WSAGetLastError()
#define SOCK_EINTR        WSAEINTR
#define SOCK_EAGAIN       WSAEWOULDBLOCK
#define SOCK_EWOULDBLOCK  WSAEWOULDBLOCK
#define SOCK_EINPROGRESS  WSAEWOULDBLOCK   /* non-blocking connect in progress */

#endif /* CL_WIN32_COMPAT_H */
