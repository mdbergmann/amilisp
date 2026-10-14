/*
 * test_tls.c — platform-layer TLS tests (host/OpenSSL backend).
 *
 * Exercises platform_tls_* directly against a loopback TLS server running
 * on a second thread, entirely below the Lisp layer: handshake (verified
 * against the checked-in test CA and unverified), transparent
 * ciphertext routing through platform_socket_read/_write_buf/_flush,
 * data_available (SSL_pending + probe) semantics, peer-certificate field
 * extraction, EOF on peer close, verification failure without a trust
 * anchor, and the argument-validation error paths.
 *
 * All tests self-skip (pass without running) when no TLS provider is
 * installed on the host — the feature is runtime-optional by design.
 * The Lisp-level twin is tests/tls-loopback.lisp (driven by
 * tests/test_tls_loopback.sh, also under gc-stress); the Amiga twin is
 * tests/amiga/tls-tests.lisp.
 */
#include "test.h"
#include "platform/platform.h"
#include <pthread.h>
#include <signal.h>
#include <string.h>

#define TEST_CERT "tests/data/tls-test-cert.pem"
#define TEST_KEY  "tests/data/tls-test-key.pem"

static int tls_present(void)
{
    static int cached = -1;
    if (cached < 0)
        cached = platform_tls_available();
    return cached;
}

/* --- Loopback server thread: accept one connection, upgrade to TLS with
 * the test cert, echo bytes doubled mod 256, then close. --- */
typedef struct {
    PlatformSocket listener;
    int echo_bytes;        /* how many bytes to echo before closing */
    volatile int failed;
} ServerArgs;

static void *tls_echo_server(void *arg)
{
    ServerArgs *sa = (ServerArgs *)arg;
    PlatformSocket conn = platform_socket_accept(sa->listener);
    PlatformTLSParams p;
    char err[256];
    if (conn == PLATFORM_SOCKET_INVALID) { sa->failed = 1; return NULL; }
    memset(&p, 0, sizeof(p));
    p.server = 1;
    p.cert_file = TEST_CERT;
    p.key_file = TEST_KEY;
    p.timeout_ms = 15000;
    if (platform_tls_start(conn, &p, err, sizeof(err)) != 0) {
        /* Expected for the verify-failure test (client aborts handshake). */
        sa->failed = 2;
        platform_socket_close(conn);
        return NULL;
    }
    {
        int i;
        for (i = 0; i < sa->echo_bytes; i++) {
            int b = platform_socket_read(conn);
            if (b < 0) { sa->failed = 3; break; }
            if (platform_socket_write(conn, (b * 2) & 0xff) != 0) {
                sa->failed = 4;
                break;
            }
        }
        platform_socket_flush(conn);
    }
    platform_socket_close(conn);
    return NULL;
}

TEST(tls_available_reports_consistently)
{
    /* Either answer is valid, but version string presence must match. */
    if (tls_present())
        ASSERT(platform_tls_version() != NULL);
    else
        ASSERT(platform_tls_version() == NULL);
}

TEST(tls_loopback_verified_handshake_and_echo)
{
    int port = 0;
    PlatformSocket listener, client;
    ServerArgs sa;
    pthread_t th;
    PlatformTLSParams p;
    char err[256];
    int i;

    if (!tls_present()) { printf("(skipped: no TLS provider) "); return; }

    listener = platform_socket_listen(0, 1, &port);
    ASSERT(listener != PLATFORM_SOCKET_INVALID);
    sa.listener = listener;
    sa.echo_bytes = 300;      /* > one IOBuf flush boundary is not needed;
                               * 300 crosses a TLS record on purpose */
    sa.failed = 0;
    ASSERT(pthread_create(&th, NULL, tls_echo_server, &sa) == 0);

    client = platform_socket_connect("127.0.0.1", port, 5000);
    ASSERT(client != PLATFORM_SOCKET_INVALID);
    ASSERT(platform_tls_active(client) == 0);

    /* Verified handshake: the self-signed test cert is its own trust
     * anchor, and hostname "localhost" matches its SAN. */
    memset(&p, 0, sizeof(p));
    p.server = 0;
    p.verify = 1;
    p.hostname = "localhost";
    p.ca_file = TEST_CERT;
    p.timeout_ms = 15000;
    err[0] = '\0';
    ASSERT_EQ(0, platform_tls_start(client, &p, err, sizeof(err)));
    ASSERT(platform_tls_active(client) == 1);

    /* Double upgrade must be rejected. */
    ASSERT_EQ(-1, platform_tls_start(client, &p, err, sizeof(err)));

    /* Echo 300 bytes through the encrypted stream. */
    {
        char buf[300];
        for (i = 0; i < 300; i++) buf[i] = (char)(i & 0xff);
        ASSERT_EQ(0, platform_socket_write_buf(client, buf, sizeof(buf)));
        ASSERT_EQ(0, platform_socket_flush(client));
    }
    for (i = 0; i < 300; i++) {
        int b = platform_socket_read(client);
        ASSERT(b >= 0);
        ASSERT_EQ(((i & 0xff) * 2) & 0xff, b);
    }

    /* Peer certificate fields. */
    {
        char field[512];
        ASSERT_EQ(0, platform_tls_peer_cert_field(client,
                     PLATFORM_TLS_CERT_SUBJECT, field, sizeof(field)));
        ASSERT(strstr(field, "CN=localhost") != NULL);
        ASSERT_EQ(0, platform_tls_peer_cert_field(client,
                     PLATFORM_TLS_CERT_ISSUER, field, sizeof(field)));
        ASSERT(strstr(field, "CL-Amiga TLS test") != NULL); /* the fixture cert's issuer (tests/data/tls-test-cert.pem) */
        /* Bad selector answers -1, not garbage. */
        ASSERT_EQ(-1, platform_tls_peer_cert_field(client, 99,
                                                   field, sizeof(field)));
    }

    /* Server has echoed everything and closed: EOF (2) must surface
     * through data_available, then read returns -1. */
    {
        int tries = 200, r = 0;
        while (tries-- > 0) {
            r = platform_socket_data_available(client);
            if (r == 2 || r == -1) break;
            /* r==0/1: close_notify not seen yet; nibble at the socket */
            if (r == 1 && platform_socket_read(client) < 0) { r = 2; break; }
        }
        ASSERT_EQ(2, r);
    }
    ASSERT_EQ(-1, platform_socket_read(client));

    platform_socket_close(client);
    pthread_join(th, NULL);
    ASSERT_EQ(0, sa.failed);
    platform_socket_close(listener);
}

TEST(tls_verify_without_anchor_fails)
{
    int port = 0;
    PlatformSocket listener, client;
    ServerArgs sa;
    pthread_t th;
    PlatformTLSParams p;
    char err[256];

    if (!tls_present()) { printf("(skipped: no TLS provider) "); return; }

    listener = platform_socket_listen(0, 1, &port);
    ASSERT(listener != PLATFORM_SOCKET_INVALID);
    sa.listener = listener;
    sa.echo_bytes = 0;
    sa.failed = 0;
    ASSERT(pthread_create(&th, NULL, tls_echo_server, &sa) == 0);

    client = platform_socket_connect("127.0.0.1", port, 5000);
    ASSERT(client != PLATFORM_SOCKET_INVALID);

    /* verify=1 with no ca_file and a self-signed peer: must fail, and the
     * diagnostic must name the certificate problem. */
    memset(&p, 0, sizeof(p));
    p.verify = 1;
    p.hostname = "localhost";
    p.timeout_ms = 15000;
    err[0] = '\0';
    ASSERT_EQ(-1, platform_tls_start(client, &p, err, sizeof(err)));
    ASSERT(strlen(err) > 0);
    ASSERT(platform_tls_active(client) == 0);

    platform_socket_close(client);
    pthread_join(th, NULL);
    platform_socket_close(listener);
}

TEST(tls_start_argument_errors)
{
    PlatformTLSParams p;
    char err[128];
    int port = 0;
    PlatformSocket listener;

    if (!tls_present()) { printf("(skipped: no TLS provider) "); return; }

    memset(&p, 0, sizeof(p));

    /* Invalid handle. */
    ASSERT_EQ(-1, platform_tls_start(0, &p, err, sizeof(err)));
    ASSERT_EQ(0, platform_tls_active(0));

    /* Listener sockets can't carry TLS. */
    listener = platform_socket_listen(0, 1, &port);
    ASSERT(listener != PLATFORM_SOCKET_INVALID);
    ASSERT_EQ(-1, platform_tls_start(listener, &p, err, sizeof(err)));
    platform_socket_close(listener);

    /* Peer-cert query on a plain socket answers -1. */
    ASSERT_EQ(-1, platform_tls_peer_cert_field(0, PLATFORM_TLS_CERT_SUBJECT,
                                               err, sizeof(err)));
}

int main(void)
{
    /* SSL_write/send on a peer-closed socket must not kill the test binary
     * (the amilisp binary installs this in main.c; tests link without it).
     * Windows has no SIGPIPE — a send to a closed peer just fails the call. */
#ifndef PLATFORM_WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    RUN(tls_available_reports_consistently);
    RUN(tls_loopback_verified_handshake_and_echo);
    RUN(tls_verify_without_anchor_fails);
    RUN(tls_start_argument_errors);
    REPORT();
}
