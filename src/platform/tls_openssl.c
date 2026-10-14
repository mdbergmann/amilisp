/*
 * tls_openssl.c — TLS backend for the POSIX host: OpenSSL 1.1.1/3.x loaded
 * at runtime via dlopen, so the amilisp binary keeps zero build- and
 * link-time OpenSSL dependency (TLS is a runtime-optional capability on
 * every platform: AmiSSL may be absent on an Amiga, libssl on a host).
 *
 * Design:
 *  - No OpenSSL headers: the ~35 entry points used here are resolved by
 *    name with dlsym into function pointers, with opaque void* object
 *    types.  The few ABI constants required (SSL_ERROR_*, SSL_VERIFY_*,
 *    ctrl codes) have been stable across every 1.1/3.x release.
 *  - The fd is switched to O_NONBLOCK permanently at handshake time; every
 *    SSL_* I/O call is retried on WANT_READ/WANT_WRITE around a poll()
 *    against one absolute deadline — the same discipline as the timed
 *    plain-socket paths in platform_posix.c.
 *  - An SSL* is not thread-safe (unlike a plain fd, where the kernel
 *    serialises send/recv).  Each connection carries a mutex held only
 *    across the SSL_* library call itself, never across a poll() park, so
 *    a reader blocked waiting for bytes cannot starve a writer.
 *  - OpenSSL >= 1.1.1 is required: auto-init, builtin thread locking, and
 *    host-name verification via SSL_set1_host.  The loader refuses older
 *    libraries rather than misbehave.
 */
#if defined(PLATFORM_POSIX) || defined(PLATFORM_WIN32)

#include "platform.h"
#include "tls_openssl.h"

#ifdef PLATFORM_WIN32
/* Supplies dlopen/poll over LoadLibrary/WSAPoll — see win32_compat.h. */
#include "win32_compat.h"
#else
#include <dlfcn.h>
#include <poll.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Stable OpenSSL ABI constants (1.1.x and 3.x) ---- */
#define TLSO_ERROR_NONE          0
#define TLSO_ERROR_SSL           1
#define TLSO_ERROR_WANT_READ     2
#define TLSO_ERROR_WANT_WRITE    3
#define TLSO_ERROR_SYSCALL       5
#define TLSO_ERROR_ZERO_RETURN   6
#define TLSO_FILETYPE_PEM        1
#define TLSO_VERIFY_NONE         0
#define TLSO_VERIFY_PEER         1
#define TLSO_CTRL_SET_TLSEXT_HOSTNAME 55
#define TLSO_NAMETYPE_host_name  0
#define TLSO_X509_V_OK           0

/* ---- dlopen'd entry points ---- */
typedef int (*tlso_passwd_cb_fn)(char *, int, int, void *);

static const void *(*p_TLS_client_method)(void);
static const void *(*p_TLS_server_method)(void);
static void  *(*p_SSL_CTX_new)(const void *method);
static void   (*p_SSL_CTX_free)(void *ctx);
static void   (*p_SSL_CTX_set_verify)(void *ctx, int mode, void *cb);
static int    (*p_SSL_CTX_set_default_verify_paths)(void *ctx);
static int    (*p_SSL_CTX_load_verify_locations)(void *ctx, const char *file,
                                                 const char *path);
static int    (*p_SSL_CTX_use_certificate_chain_file)(void *ctx, const char *file);
static int    (*p_SSL_CTX_use_PrivateKey_file)(void *ctx, const char *file, int type);
static int    (*p_SSL_CTX_check_private_key)(const void *ctx);
static void   (*p_SSL_CTX_set_default_passwd_cb)(void *ctx, tlso_passwd_cb_fn cb);
static void   (*p_SSL_CTX_set_default_passwd_cb_userdata)(void *ctx, void *u);
static void  *(*p_SSL_new)(void *ctx);
static void   (*p_SSL_free)(void *ssl);
static int    (*p_SSL_set_fd)(void *ssl, int fd);
static long   (*p_SSL_ctrl)(void *ssl, int cmd, long larg, void *parg);
static int    (*p_SSL_set1_host)(void *ssl, const char *hostname);
static int    (*p_SSL_connect)(void *ssl);
static int    (*p_SSL_accept)(void *ssl);
static int    (*p_SSL_read)(void *ssl, void *buf, int num);
static int    (*p_SSL_write)(void *ssl, const void *buf, int num);
static int    (*p_SSL_peek)(void *ssl, void *buf, int num);
static int    (*p_SSL_pending)(const void *ssl);
static int    (*p_SSL_shutdown)(void *ssl);
static int    (*p_SSL_get_error)(const void *ssl, int ret);
static long   (*p_SSL_get_verify_result)(const void *ssl);
static void  *(*p_SSL_get1_peer_certificate)(const void *ssl);
static void   (*p_X509_free)(void *x);
static void  *(*p_X509_get_subject_name)(const void *x);
static void  *(*p_X509_get_issuer_name)(const void *x);
static char  *(*p_X509_NAME_oneline)(const void *name, char *buf, int size);
static const char *(*p_X509_verify_cert_error_string)(long n);
static unsigned long (*p_ERR_get_error)(void);
static void   (*p_ERR_error_string_n)(unsigned long e, char *buf, size_t len);
static void   (*p_ERR_clear_error)(void);
static unsigned long (*p_OpenSSL_version_num)(void);
/* Optional (graceful degradation when absent): */
static const char *(*p_OpenSSL_version)(int t);            /* version banner */
static const void *(*p_X509_get0_notBefore)(const void *x); /* validity dates */
static const void *(*p_X509_get0_notAfter)(const void *x);
static int    (*p_ASN1_TIME_to_tm)(const void *t, struct tm *tm);

/* ---- Loader ---- */
static pthread_once_t tls_load_once = PTHREAD_ONCE_INIT;
static int   tls_loaded = 0;
static char  tls_load_err[256] = "TLS provider not loaded";
static char  tls_version_str[160];
static void *lib_ssl;
static void *lib_crypto;

static void *tls_dlopen_first(const char *env_override, const char *const *names)
{
    int i;
    if (env_override) {
        const char *p = getenv(env_override);
        if (p && *p) {
            void *h = dlopen(p, RTLD_NOW | RTLD_GLOBAL);
            if (h) return h;
        }
    }
    for (i = 0; names[i]; i++) {
        void *h = dlopen(names[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) return h;
    }
    return NULL;
}

static void *tls_sym(const char *name)
{
    void *p = lib_ssl ? dlsym(lib_ssl, name) : NULL;
    if (!p && lib_crypto) p = dlsym(lib_crypto, name);
    return p;
}

static void tls_load(void)
{
    /* Unversioned "libssl.dylib" is deliberately absent on macOS: Apple
     * ships an /usr/lib stub of that name that aborts the process on first
     * call.  Only versioned Homebrew/MacPorts paths and versioned SONAMEs
     * are probed. */
    static const char *const crypto_names[] = {
#if defined(PLATFORM_WIN32)
        /* MSYS2/mingw and the official OpenSSL installers use these names;
         * the plain "libcrypto.dll" in System32 is Windows' own private copy
         * and is deliberately not probed. */
        "libcrypto-3-arm64.dll",
        "libcrypto-3-x64.dll",
        "libcrypto-3.dll",
        "libcrypto-1_1-x64.dll",
        "libcrypto-1_1.dll",
#elif defined(__APPLE__)
        "libcrypto.3.dylib",
        "/opt/homebrew/opt/openssl@3/lib/libcrypto.3.dylib",
        "/usr/local/opt/openssl@3/lib/libcrypto.3.dylib",
        "/opt/local/lib/libcrypto.3.dylib",
        "libcrypto.1.1.dylib",
        "/opt/homebrew/opt/openssl@1.1/lib/libcrypto.1.1.dylib",
        "/usr/local/opt/openssl@1.1/lib/libcrypto.1.1.dylib",
#else
        "libcrypto.so.3",
        "libcrypto.so.1.1",
        "libcrypto.so",
#endif
        NULL
    };
    static const char *const ssl_names[] = {
#if defined(PLATFORM_WIN32)
        "libssl-3-arm64.dll",
        "libssl-3-x64.dll",
        "libssl-3.dll",
        "libssl-1_1-x64.dll",
        "libssl-1_1.dll",
#elif defined(__APPLE__)
        "libssl.3.dylib",
        "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib",
        "/usr/local/opt/openssl@3/lib/libssl.3.dylib",
        "/opt/local/lib/libssl.3.dylib",
        "libssl.1.1.dylib",
        "/opt/homebrew/opt/openssl@1.1/lib/libssl.1.1.dylib",
        "/usr/local/opt/openssl@1.1/lib/libssl.1.1.dylib",
#else
        "libssl.so.3",
        "libssl.so.1.1",
        "libssl.so",
#endif
        NULL
    };
    const char *missing = NULL;

    lib_crypto = tls_dlopen_first("AMILISP_LIBCRYPTO", crypto_names);
    lib_ssl    = tls_dlopen_first("AMILISP_LIBSSL", ssl_names);
    if (!lib_ssl || !lib_crypto) {
        snprintf(tls_load_err, sizeof(tls_load_err),
                 "no OpenSSL 1.1.1/3.x found (install openssl, or point "
                 "AMILISP_LIBSSL/AMILISP_LIBCRYPTO at libssl/libcrypto)");
        return;
    }

#define TLSO_REQUIRE(var, name) \
    do { *(void **)&(var) = tls_sym(name); \
         if (!(var) && !missing) missing = name; } while (0)

    TLSO_REQUIRE(p_TLS_client_method, "TLS_client_method");
    TLSO_REQUIRE(p_TLS_server_method, "TLS_server_method");
    TLSO_REQUIRE(p_SSL_CTX_new, "SSL_CTX_new");
    TLSO_REQUIRE(p_SSL_CTX_free, "SSL_CTX_free");
    TLSO_REQUIRE(p_SSL_CTX_set_verify, "SSL_CTX_set_verify");
    TLSO_REQUIRE(p_SSL_CTX_set_default_verify_paths, "SSL_CTX_set_default_verify_paths");
    TLSO_REQUIRE(p_SSL_CTX_load_verify_locations, "SSL_CTX_load_verify_locations");
    TLSO_REQUIRE(p_SSL_CTX_use_certificate_chain_file, "SSL_CTX_use_certificate_chain_file");
    TLSO_REQUIRE(p_SSL_CTX_use_PrivateKey_file, "SSL_CTX_use_PrivateKey_file");
    TLSO_REQUIRE(p_SSL_CTX_check_private_key, "SSL_CTX_check_private_key");
    TLSO_REQUIRE(p_SSL_CTX_set_default_passwd_cb, "SSL_CTX_set_default_passwd_cb");
    TLSO_REQUIRE(p_SSL_CTX_set_default_passwd_cb_userdata, "SSL_CTX_set_default_passwd_cb_userdata");
    TLSO_REQUIRE(p_SSL_new, "SSL_new");
    TLSO_REQUIRE(p_SSL_free, "SSL_free");
    TLSO_REQUIRE(p_SSL_set_fd, "SSL_set_fd");
    TLSO_REQUIRE(p_SSL_ctrl, "SSL_ctrl");
    TLSO_REQUIRE(p_SSL_set1_host, "SSL_set1_host");
    TLSO_REQUIRE(p_SSL_connect, "SSL_connect");
    TLSO_REQUIRE(p_SSL_accept, "SSL_accept");
    TLSO_REQUIRE(p_SSL_read, "SSL_read");
    TLSO_REQUIRE(p_SSL_write, "SSL_write");
    TLSO_REQUIRE(p_SSL_peek, "SSL_peek");
    TLSO_REQUIRE(p_SSL_pending, "SSL_pending");
    TLSO_REQUIRE(p_SSL_shutdown, "SSL_shutdown");
    TLSO_REQUIRE(p_SSL_get_error, "SSL_get_error");
    TLSO_REQUIRE(p_SSL_get_verify_result, "SSL_get_verify_result");
    TLSO_REQUIRE(p_X509_free, "X509_free");
    TLSO_REQUIRE(p_X509_get_subject_name, "X509_get_subject_name");
    TLSO_REQUIRE(p_X509_get_issuer_name, "X509_get_issuer_name");
    TLSO_REQUIRE(p_X509_NAME_oneline, "X509_NAME_oneline");
    TLSO_REQUIRE(p_X509_verify_cert_error_string, "X509_verify_cert_error_string");
    TLSO_REQUIRE(p_ERR_get_error, "ERR_get_error");
    TLSO_REQUIRE(p_ERR_error_string_n, "ERR_error_string_n");
    TLSO_REQUIRE(p_ERR_clear_error, "ERR_clear_error");
    TLSO_REQUIRE(p_OpenSSL_version_num, "OpenSSL_version_num");
#undef TLSO_REQUIRE

    /* 3.0 renamed SSL_get_peer_certificate; both return a new reference. */
    *(void **)&p_SSL_get1_peer_certificate = tls_sym("SSL_get1_peer_certificate");
    if (!p_SSL_get1_peer_certificate)
        *(void **)&p_SSL_get1_peer_certificate = tls_sym("SSL_get_peer_certificate");
    if (!p_SSL_get1_peer_certificate && !missing)
        missing = "SSL_get1_peer_certificate";

    /* Optional niceties. */
    *(void **)&p_OpenSSL_version    = tls_sym("OpenSSL_version");
    *(void **)&p_X509_get0_notBefore = tls_sym("X509_get0_notBefore");
    *(void **)&p_X509_get0_notAfter  = tls_sym("X509_get0_notAfter");
    *(void **)&p_ASN1_TIME_to_tm     = tls_sym("ASN1_TIME_to_tm");

    if (missing) {
        snprintf(tls_load_err, sizeof(tls_load_err),
                 "libssl/libcrypto too old: missing %s (OpenSSL >= 1.1.1 required)",
                 missing);
        return;
    }
    if (p_OpenSSL_version_num() < 0x10101000UL) {
        snprintf(tls_load_err, sizeof(tls_load_err),
                 "OpenSSL %lx is too old (>= 1.1.1 required)",
                 p_OpenSSL_version_num());
        return;
    }
    snprintf(tls_version_str, sizeof(tls_version_str), "%s",
             p_OpenSSL_version ? p_OpenSSL_version(0) : "OpenSSL (>= 1.1.1)");
    tls_loaded = 1;
}

int tls_openssl_available(char *err, uint32_t errlen)
{
    pthread_once(&tls_load_once, tls_load);
    if (tls_loaded) return 0;
    if (err && errlen > 0) {
        snprintf(err, errlen, "%s", tls_load_err);
    }
    return -1;
}

const char *tls_openssl_version(void)
{
    pthread_once(&tls_load_once, tls_load);
    return tls_loaded ? tls_version_str : NULL;
}

/* ---- Connection ---- */
struct TLSConn {
    void *ctx;              /* SSL_CTX* — one per connection, freed with it */
    void *ssl;              /* SSL* */
    int   fd;
    pthread_mutex_t lock;   /* serialises SSL_* calls; never held across poll */
};

/* Passphrase callback for encrypted PEM keys: userdata is the passphrase. */
static int tlso_passwd_cb(char *buf, int size, int rwflag, void *userdata)
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

/* 1 = ready, 0 = timed out, -1 = poll error.  timeout_ms < 0 waits forever.
 * Same EINTR-preserves-deadline discipline as socket_wait_ready. */
static int tlso_poll_fd(int fd, int want_write, int timeout_ms)
{
    struct pollfd pfd;
    uint32_t deadline = 0;
    short want = want_write ? POLLOUT : POLLIN;
    if (timeout_ms >= 0)
        deadline = platform_time_ms() + (uint32_t)timeout_ms;
    for (;;) {
        int r, rem = -1;
        if (timeout_ms >= 0) {
            int32_t left = (int32_t)(deadline - platform_time_ms());
            if (left <= 0) return 0;
            rem = left;
        }
        pfd.fd = fd;
        pfd.events = want;
        pfd.revents = 0;
        r = poll(&pfd, 1, rem);
        if (r < 0) {
#ifdef PLATFORM_WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        if (r == 0) return 0;
        return (pfd.revents & (want | POLLERR | POLLHUP | POLLNVAL)) ? 1 : 0;
    }
}

/* Format a failure diagnostic: thread-local OpenSSL error queue first, then
 * errno (SYSCALL), then the certificate-verify result when it explains a
 * handshake rejection. */
static void tlso_fmt_err(TLSConn *c, char *err, uint32_t errlen,
                         const char *what, int sslerr, int syserr)
{
    char detail[160];
    unsigned long e;
    long vr = TLSO_X509_V_OK;
    if (!err || errlen == 0) return;
    detail[0] = '\0';
    e = p_ERR_get_error();
    if (e)
        p_ERR_error_string_n(e, detail, sizeof(detail));
    else if (sslerr == TLSO_ERROR_SYSCALL && syserr)
#ifdef PLATFORM_WIN32
        /* syserr is a Winsock code (see below), which strerror does not
         * know; FormatMessage does.  Failing that, the bare number still
         * beats "unexpected EOF from peer" for a connection that was reset. */
        {
            char msg[128];
            DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                     NULL, (DWORD)syserr,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     msg, (DWORD)sizeof(msg), NULL);
            while (n > 0 && (msg[n - 1] == '\r' ||
                             msg[n - 1] == '\n'))
                msg[--n] = '\0';
            if (n > 0)
                snprintf(detail, sizeof(detail), "%s", msg);
            else
                snprintf(detail, sizeof(detail), "Winsock error %d", syserr);
        }
#else
        snprintf(detail, sizeof(detail), "%s", strerror(syserr));
#endif
    else if (sslerr == TLSO_ERROR_SYSCALL)
        snprintf(detail, sizeof(detail), "unexpected EOF from peer");
    else
        snprintf(detail, sizeof(detail), "TLS protocol error %d", sslerr);
    if (c) {
        pthread_mutex_lock(&c->lock);
        vr = p_SSL_get_verify_result(c->ssl);
        pthread_mutex_unlock(&c->lock);
    }
    if (vr != TLSO_X509_V_OK)
        snprintf(err, errlen, "%s: %s (certificate verification: %s)",
                 what, detail, p_X509_verify_cert_error_string(vr));
    else
        snprintf(err, errlen, "%s: %s", what, detail);
}

enum { TLSO_OP_CONNECT, TLSO_OP_ACCEPT, TLSO_OP_READ, TLSO_OP_WRITE };

static const char *tlso_op_name(int op)
{
    switch (op) {
    case TLSO_OP_CONNECT: return "TLS handshake (connect)";
    case TLSO_OP_ACCEPT:  return "TLS handshake (accept)";
    case TLSO_OP_READ:    return "TLS read";
    default:              return "TLS write";
    }
}

/* Drive one SSL operation to completion against an absolute deadline.
 * Returns the operation result (> 0), 0 on clean EOF (close_notify),
 * -1 on error (diagnostic in err when non-NULL), -2 on timeout. */
static int tlso_run(TLSConn *c, int op, char *buf, int len, int timeout_ms,
                    char *err, uint32_t errlen)
{
    uint32_t deadline = 0;
    if (timeout_ms > 0)
        deadline = platform_time_ms() + (uint32_t)timeout_ms;
    for (;;) {
        int n, sslerr, syserr;
        pthread_mutex_lock(&c->lock);
        p_ERR_clear_error();
        errno = 0;
        switch (op) {
        case TLSO_OP_CONNECT: n = p_SSL_connect(c->ssl); break;
        case TLSO_OP_ACCEPT:  n = p_SSL_accept(c->ssl); break;
        case TLSO_OP_READ:    n = p_SSL_read(c->ssl, buf, len); break;
        default:              n = p_SSL_write(c->ssl, buf, len); break;
        }
        if (n > 0) {
            pthread_mutex_unlock(&c->lock);
            return n;
        }
        sslerr = p_SSL_get_error(c->ssl, n);
#ifdef PLATFORM_WIN32
        /* OpenSSL on Windows reports socket-level failures through Winsock,
         * not the CRT errno — which stays 0, so every SSL_ERROR_SYSCALL was
         * being described as "unexpected EOF from peer" no matter what
         * actually happened (a reset mid-handshake, a refused connection). */
        syserr = WSAGetLastError();
#else
        syserr = errno;
#endif
        pthread_mutex_unlock(&c->lock);
        if (sslerr == TLSO_ERROR_WANT_READ || sslerr == TLSO_ERROR_WANT_WRITE) {
            int rr, rem_ms = -1;
            if (deadline) {
                int32_t left = (int32_t)(deadline - platform_time_ms());
                if (left <= 0) return -2;
                rem_ms = left;
            }
            rr = tlso_poll_fd(c->fd, sslerr == TLSO_ERROR_WANT_WRITE, rem_ms);
            if (rr == 0) return -2;
            if (rr < 0) {
                tlso_fmt_err(c, err, errlen, tlso_op_name(op), sslerr, syserr);
                return -1;
            }
            continue;
        }
        if (sslerr == TLSO_ERROR_ZERO_RETURN)
            return 0;
        tlso_fmt_err(c, err, errlen, tlso_op_name(op), sslerr, syserr);
        return -1;
    }
}

TLSConn *tls_conn_start(int fd, const PlatformTLSParams *p,
                        char *err, uint32_t errlen)
{
    TLSConn *c = NULL;
    void *ctx = NULL;
    void *ssl = NULL;
    int r;

    if (tls_openssl_available(err, errlen) != 0)
        return NULL;

    ctx = p_SSL_CTX_new(p->server ? p_TLS_server_method() : p_TLS_client_method());
    if (!ctx) {
        tlso_fmt_err(NULL, err, errlen, "TLS context creation", 0, 0);
        return NULL;
    }

    /* Trust anchors: explicit locations win; else the provider's default
     * store (needed only when verifying). */
    if (p->ca_file || p->ca_path) {
        if (p_SSL_CTX_load_verify_locations(ctx, p->ca_file, p->ca_path) != 1) {
            tlso_fmt_err(NULL, err, errlen, "loading CA locations", 0, 0);
            goto fail;
        }
    } else if (!p->server && p->verify) {
        p_SSL_CTX_set_default_verify_paths(ctx);   /* best-effort */
    }

    /* Own certificate/key (server: required; client: optional client cert). */
    if (p->cert_file) {
        const char *key = p->key_file ? p->key_file : p->cert_file;
        if (p->key_password) {
            p_SSL_CTX_set_default_passwd_cb(ctx, tlso_passwd_cb);
            p_SSL_CTX_set_default_passwd_cb_userdata(ctx, (void *)p->key_password);
        }
        if (p_SSL_CTX_use_certificate_chain_file(ctx, p->cert_file) != 1) {
            tlso_fmt_err(NULL, err, errlen, "loading certificate file", 0, 0);
            goto fail;
        }
        if (p_SSL_CTX_use_PrivateKey_file(ctx, key, TLSO_FILETYPE_PEM) != 1) {
            tlso_fmt_err(NULL, err, errlen, "loading private key file", 0, 0);
            goto fail;
        }
        if (p_SSL_CTX_check_private_key(ctx) != 1) {
            tlso_fmt_err(NULL, err, errlen, "private key / certificate mismatch", 0, 0);
            goto fail;
        }
        /* The passphrase points at a caller stack buffer — drop the
         * reference now that the key is loaded. */
        p_SSL_CTX_set_default_passwd_cb(ctx, NULL);
        p_SSL_CTX_set_default_passwd_cb_userdata(ctx, NULL);
    } else if (p->server) {
        if (err && errlen > 0)
            snprintf(err, errlen,
                     "server-side TLS requires a certificate file "
                     "(:certificate ... in EXT:SOCKET-START-TLS)");
        goto fail;
    }

    p_SSL_CTX_set_verify(ctx, (!p->server && p->verify)
                              ? TLSO_VERIFY_PEER : TLSO_VERIFY_NONE, NULL);

    ssl = p_SSL_new(ctx);
    if (!ssl) {
        tlso_fmt_err(NULL, err, errlen, "TLS connection creation", 0, 0);
        goto fail;
    }

    /* Permanently non-blocking: from here every fd interaction is an SSL_*
     * call retried around poll(). */
#ifdef PLATFORM_WIN32
    {
        u_long nb = 1;
        ioctlsocket((SOCKET)fd, FIONBIO, &nb);
    }
#else
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    if (p_SSL_set_fd(ssl, fd) != 1) {
        tlso_fmt_err(NULL, err, errlen, "binding TLS to socket", 0, 0);
        goto fail;
    }
    if (!p->server && p->hostname && p->hostname[0]) {
        /* SNI always (both copy the string); hostname verification only
         * when verifying. */
        p_SSL_ctrl(ssl, TLSO_CTRL_SET_TLSEXT_HOSTNAME,
                   TLSO_NAMETYPE_host_name, (void *)p->hostname);
        if (p->verify && p_SSL_set1_host(ssl, p->hostname) != 1) {
            tlso_fmt_err(NULL, err, errlen, "setting verification hostname", 0, 0);
            goto fail;
        }
    }

    c = (TLSConn *)calloc(1, sizeof(TLSConn));
    if (!c) {
        if (err && errlen > 0) snprintf(err, errlen, "out of memory");
        goto fail;
    }
    c->ctx = ctx;
    c->ssl = ssl;
    c->fd = fd;
    pthread_mutex_init(&c->lock, NULL);

    r = tlso_run(c, p->server ? TLSO_OP_ACCEPT : TLSO_OP_CONNECT,
                 NULL, 0, p->timeout_ms, err, errlen);
    if (r <= 0) {
        if (err && errlen > 0) {
            if (r == -2)
                snprintf(err, errlen, "TLS handshake timed out after %d ms",
                         p->timeout_ms);
            else if (r == 0)
                snprintf(err, errlen, "peer closed the connection during "
                         "the TLS handshake");
        }
        tls_conn_close(c, 0);
        return NULL;
    }
    return c;

fail:
    if (ssl) p_SSL_free(ssl);
    if (ctx) p_SSL_CTX_free(ctx);
    free(c);
    return NULL;
}

int tls_conn_read(TLSConn *c, char *buf, uint32_t len, int timeout_ms)
{
    return tlso_run(c, TLSO_OP_READ, buf, (int)len, timeout_ms, NULL, 0);
}

int tls_conn_write(TLSConn *c, const char *buf, uint32_t len, int timeout_ms)
{
    uint32_t done = 0;
    uint32_t deadline = 0;
    if (timeout_ms > 0)
        deadline = platform_time_ms() + (uint32_t)timeout_ms;
    while (done < len) {
        int n, rem_ms = 0;
        if (deadline) {
            int32_t left = (int32_t)(deadline - platform_time_ms());
            if (left <= 0) return -2;
            rem_ms = left;
        }
        n = tlso_run(c, TLSO_OP_WRITE, (char *)(buf + done),
                     (int)(len - done), rem_ms, NULL, 0);
        if (n == 0) return -1;          /* peer closed mid-write */
        if (n < 0) return n;            /* -1 error / -2 timeout */
        done += (uint32_t)n;
    }
    return 0;
}

int tls_conn_pending(TLSConn *c)
{
    int n;
    pthread_mutex_lock(&c->lock);
    n = p_SSL_pending(c->ssl);
    pthread_mutex_unlock(&c->lock);
    return n;
}

int tls_conn_probe(TLSConn *c)
{
    char tmp;
    int n, sslerr;
    pthread_mutex_lock(&c->lock);
    p_ERR_clear_error();
    n = p_SSL_peek(c->ssl, &tmp, 1);
    if (n > 0) {
        pthread_mutex_unlock(&c->lock);
        return 1;
    }
    sslerr = p_SSL_get_error(c->ssl, n);
    pthread_mutex_unlock(&c->lock);
    if (sslerr == TLSO_ERROR_WANT_READ || sslerr == TLSO_ERROR_WANT_WRITE)
        return 0;                       /* nothing decodable yet */
    return 2;                           /* close_notify / hard EOF / error */
}

int tls_conn_peer_cert_field(TLSConn *c, int field, char *out, uint32_t outlen)
{
    void *x;
    int ok = -1;
    if (!out || outlen == 0) return -1;
    out[0] = '\0';
    pthread_mutex_lock(&c->lock);
    x = p_SSL_get1_peer_certificate(c->ssl);
    pthread_mutex_unlock(&c->lock);
    if (!x) return -1;
    switch (field) {
    case PLATFORM_TLS_CERT_SUBJECT:
    case PLATFORM_TLS_CERT_ISSUER: {
        void *name = (field == PLATFORM_TLS_CERT_SUBJECT)
                     ? p_X509_get_subject_name(x)
                     : p_X509_get_issuer_name(x);
        if (name && p_X509_NAME_oneline(name, out, (int)outlen))
            ok = 0;
        break;
    }
    case PLATFORM_TLS_CERT_NOT_BEFORE:
    case PLATFORM_TLS_CERT_NOT_AFTER: {
        const void *t;
        struct tm tm;
        if (!p_ASN1_TIME_to_tm || !p_X509_get0_notBefore || !p_X509_get0_notAfter)
            break;
        t = (field == PLATFORM_TLS_CERT_NOT_BEFORE)
            ? p_X509_get0_notBefore(x) : p_X509_get0_notAfter(x);
        memset(&tm, 0, sizeof(tm));
        if (t && p_ASN1_TIME_to_tm(t, &tm) == 1) {
            snprintf(out, outlen, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
            ok = 0;
        }
        break;
    }
    default:
        break;
    }
    p_X509_free(x);
    return ok;
}

void tls_conn_close(TLSConn *c, int send_close_notify)
{
    if (!c) return;
    if (send_close_notify) {
        /* One non-blocking close_notify attempt; the fd is O_NONBLOCK so
         * this can never park (WANT_WRITE is simply dropped). */
        pthread_mutex_lock(&c->lock);
        p_SSL_shutdown(c->ssl);
        pthread_mutex_unlock(&c->lock);
    }
    if (c->ssl) p_SSL_free(c->ssl);
    if (c->ctx) p_SSL_CTX_free(c->ctx);
    pthread_mutex_destroy(&c->lock);
    free(c);
}

#endif /* PLATFORM_POSIX || PLATFORM_WIN32 */
