#include "../src/llm_conn.h"
#include "oi/reactor.h"
#include "test.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Starts a blocking echo server in a forked child, listening on
 * loopback. Returns the child pid (caller must waitpid) and the port it
 * bound to. */
static pid_t start_echo_server(unsigned short *out_port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof addr), 0);
    CHECK_EQ(listen(listen_fd, 1), 0);

    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &alen), 0);
    *out_port = ntohs(addr.sin_port);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd >= 0) {
            char buf[4096];
            ssize_t n;
            while ((n = read(cfd, buf, sizeof buf)) > 0) {
                ssize_t off = 0;
                while (off < n) {
                    ssize_t w = write(cfd, buf + off, (size_t)(n - off));
                    if (w <= 0) {
                        _exit(0);
                    }
                    off += w;
                }
            }
            close(cfd);
        }
        close(listen_fd);
        _exit(0);
    }
    close(listen_fd);
    return pid;
}

/* A listening socket that never accepts, so connect() to it fills the
 * kernel backlog behavior predictably is NOT what we want for a
 * "connection refused" test -- instead we bind a socket, learn its
 * port, and close it immediately so nothing is listening there. */
static unsigned short unused_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(fd, (struct sockaddr *)&addr, sizeof addr);
    socklen_t alen = sizeof addr;
    getsockname(fd, (struct sockaddr *)&addr, &alen);
    unsigned short port = ntohs(addr.sin_port);
    close(fd); /* nothing listens here now */
    return port;
}

static void run_until(oi_reactor *r, const int *done_flag, int max_steps) {
    for (int i = 0; i < max_steps && !*done_flag; i++) {
        oi_status st;
        oi_reactor_step(r, 200, &st);
    }
}

/* --- echo round trip, with a reentrant close from on_data --- */

struct echo_ctx {
    oi_llm_conn *conn;
    int connected;
    char received[64];
    size_t received_len;
    int done;
    int error_fired;
};

static void echo_on_connected(oi_llm_conn *c, void *ud) {
    struct echo_ctx *ctx = ud;
    ctx->connected = 1;
    /* An impossible length must fail before reading caller memory and
     * leave the connection usable for the real write that follows. */
    CHECK_EQ(oi_llm_conn_write(c, "x", (size_t)-1), OI_ERR_NOMEM);
    oi_status st = oi_llm_conn_write(c, "hello", 5);
    CHECK_EQ(st, OI_OK);
}

static void echo_on_data(oi_llm_conn *c, const void *data, size_t len,
                          void *ud) {
    struct echo_ctx *ctx = ud;
    CHECK(ctx->received_len + len <= sizeof ctx->received);
    memcpy(ctx->received + ctx->received_len, data, len);
    ctx->received_len += len;
    if (ctx->received_len >= 5) {
        /* Reentrant close from within the connection's own callback --
         * exercises the destroyed-flag safety mechanism. */
        oi_llm_conn_close(c);
        ctx->done = 1;
    }
}

static void echo_on_error(oi_llm_conn *c, oi_status reason, void *ud) {
    (void)c;
    (void)reason;
    struct echo_ctx *ctx = ud;
    ctx->error_fired = 1;
    ctx->done = 1;
}

TEST(connect_write_echo_and_reentrant_close) {
    unsigned short port;
    pid_t child = start_echo_server(&port);

    oi_reactor *r = oi_reactor_create();
    struct echo_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {echo_on_connected, echo_on_data,
                                         echo_on_error};

    /* A hostname exercises asynchronous resolution and address fallback
     * (localhost commonly returns IPv6 before this IPv4 listener). */
    oi_status st = oi_llm_conn_connect(r, "localhost", port, 0, NULL, &cbs,
                                        &ctx, &ctx.conn);
    CHECK_EQ(st, OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.connected);
    CHECK(!ctx.error_fired);
    CHECK_EQ(ctx.received_len, 5u);
    CHECK(memcmp(ctx.received, "hello", 5) == 0);

    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

/* --- connection refused --- */

struct error_ctx {
    int error_fired;
    oi_status reason;
    int done;
    char cause[256];
};

static void err_on_connected(oi_llm_conn *c, void *ud) {
    (void)c;
    (void)ud;
    CHECK(0); /* must not connect */
}

static void err_on_data(oi_llm_conn *c, const void *data, size_t len,
                         void *ud) {
    (void)c;
    (void)data;
    (void)len;
    (void)ud;
    CHECK(0);
}

static void err_on_error(oi_llm_conn *c, oi_status reason, void *ud) {
    struct error_ctx *ctx = ud;
    const char *cause = oi_llm_conn_error_cause(c);
    snprintf(ctx->cause, sizeof ctx->cause, "%s", cause ? cause : "");
    ctx->error_fired = 1;
    ctx->reason = reason;
    ctx->done = 1;
}

TEST(connection_refused_reports_error) {
    unsigned short port = unused_port();

    oi_reactor *r = oi_reactor_create();
    struct error_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {err_on_connected, err_on_data,
                                         err_on_error};
    oi_llm_conn *conn = NULL;

    oi_status st = oi_llm_conn_connect(r, "127.0.0.1", port, 0, NULL, &cbs,
                                        &ctx, &conn);
    CHECK_EQ(st, OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.error_fired);
    CHECK_EQ(ctx.reason, OI_ERR_IO);
    char expected[128];
    snprintf(expected, sizeof expected,
             "could not connect to 127.0.0.1 port %u: Connection refused",
             (unsigned)port);
    CHECK_STREQ(ctx.cause, expected);

    oi_reactor_destroy(r);
}

TEST(resolver_failure_is_reported_asynchronously) {
    oi_reactor *r = oi_reactor_create();
    struct error_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {err_on_connected, err_on_data,
                                         err_on_error};
    oi_llm_conn *conn = NULL;
    CHECK_EQ(oi_llm_conn_connect(r, "does-not-exist.invalid", 443, 0, NULL,
                                  &cbs, &ctx, &conn),
              OI_OK);
    CHECK(!ctx.done); /* connect never blocks to resolve */
    run_until(r, &ctx.done, 100);
    CHECK(ctx.error_fired);
    CHECK_EQ(ctx.reason, OI_ERR_IO);
    CHECK(strncmp(ctx.cause, "could not resolve does-not-exist.invalid: ",
                  strlen("could not resolve does-not-exist.invalid: ")) ==
          0);
    (void)conn; /* terminal callback closes it */
    oi_reactor_destroy(r);
}

TEST(cancel_during_resolution_is_silent) {
    oi_reactor *r = oi_reactor_create();
    struct error_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {err_on_connected, err_on_data,
                                         err_on_error};
    oi_llm_conn *conn = NULL;
    CHECK_EQ(oi_llm_conn_connect(r, "localhost", 443, 0, NULL, &cbs, &ctx,
                                  &conn),
              OI_OK);
    oi_llm_conn_close(conn);
    oi_status st;
    CHECK_EQ(oi_reactor_step(r, 10, &st), 0);
    CHECK_EQ(st, OI_OK);
    CHECK(!ctx.done);
    oi_reactor_destroy(r);
}

/* --- peer closes after sending some data --- */

struct half_ctx {
    char received[64];
    size_t received_len;
    int error_fired;
    oi_status reason;
    int done;
};

static void half_on_connected(oi_llm_conn *c, void *ud) {
    (void)c;
    (void)ud;
}

static void half_on_data(oi_llm_conn *c, const void *data, size_t len,
                          void *ud) {
    (void)c;
    struct half_ctx *ctx = ud;
    CHECK(ctx->received_len + len <= sizeof ctx->received);
    memcpy(ctx->received + ctx->received_len, data, len);
    ctx->received_len += len;
}

static void half_on_error(oi_llm_conn *c, oi_status reason, void *ud) {
    (void)c;
    struct half_ctx *ctx = ud;
    ctx->error_fired = 1;
    ctx->reason = reason;
    ctx->done = 1;
}

/* Listens on an ephemeral loopback port, accepts one connection, writes
 * `data` to it, then closes immediately -- for testing the "peer sent
 * something and then hung up" path. */
static pid_t start_write_then_close_server(const char *data, size_t len,
                                            unsigned short *out_port) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    CHECK_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof addr), 0);
    CHECK_EQ(listen(listen_fd, 1), 0);

    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &alen), 0);
    *out_port = ntohs(addr.sin_port);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd >= 0) {
            ssize_t off = 0;
            while ((size_t)off < len) {
                ssize_t w = write(cfd, data + off, len - (size_t)off);
                if (w <= 0) {
                    break;
                }
                off += w;
            }
            close(cfd);
        }
        close(listen_fd);
        _exit(0);
    }
    close(listen_fd);
    return pid;
}

TEST(peer_close_reports_closed) {
    unsigned short port;
    pid_t child = start_write_then_close_server("hi", 2, &port);

    oi_reactor *r = oi_reactor_create();
    struct half_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {half_on_connected, half_on_data,
                                         half_on_error};
    oi_llm_conn *conn = NULL;
    oi_status st = oi_llm_conn_connect(r, "127.0.0.1", port, 0, NULL, &cbs,
                                        &ctx, &conn);
    CHECK_EQ(st, OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK_EQ(ctx.received_len, 2u);
    CHECK(memcmp(ctx.received, "hi", 2) == 0);
    CHECK(ctx.error_fired);
    CHECK_EQ(ctx.reason, OI_ERR_CLOSED);
    /* fail_conn already closed `conn` via its fallback path, since
     * half_on_error doesn't close it itself -- closing it again here
     * would be a double free. */
    (void)conn;

    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

/* --- argument validation --- */

TEST(connect_rejects_bad_args) {
    oi_reactor *r = oi_reactor_create();
    struct oi_llm_conn_callbacks cbs = {NULL, NULL, NULL};
    oi_llm_conn *conn;
    CHECK_EQ(oi_llm_conn_connect(NULL, "x", 1, 0, NULL, &cbs, NULL, &conn),
              OI_ERR_INVAL);
    CHECK_EQ(oi_llm_conn_connect(r, NULL, 1, 0, NULL, &cbs, NULL, &conn),
              OI_ERR_INVAL);
    CHECK_EQ(oi_llm_conn_connect(r, "x", 1, 0, NULL, NULL, NULL, &conn),
              OI_ERR_INVAL);
    CHECK_EQ(oi_llm_conn_connect(r, "x", 1, 0, NULL, &cbs, NULL, NULL),
              OI_ERR_INVAL);
    oi_reactor_destroy(r);
}

TEST(write_before_connected_rejected) {
    unsigned short port;
    pid_t child = start_echo_server(&port);
    oi_reactor *r = oi_reactor_create();
    struct echo_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {echo_on_connected, echo_on_data,
                                         echo_on_error};
    /* Use a no-op connected callback for this test by overriding: skip
     * writing in on_connected and instead try to write immediately,
     * before the reactor has had a chance to establish the connection. */
    oi_status st = oi_llm_conn_connect(r, "127.0.0.1", port, 0, NULL, &cbs,
                                        &ctx, &ctx.conn);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(oi_llm_conn_write(ctx.conn, "x", 1), OI_ERR_INVAL);

    run_until(r, &ctx.done, 100);
    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

TEST(close_null_safe) { oi_llm_conn_close(NULL); }

static void custom_sigpipe_handler(int sig) { (void)sig; }

TEST(connection_preserves_sigpipe_disposition) {
    struct sigaction custom;
    memset(&custom, 0, sizeof custom);
    custom.sa_handler = custom_sigpipe_handler;
    sigemptyset(&custom.sa_mask);
    struct sigaction old;
    CHECK_EQ(sigaction(SIGPIPE, &custom, &old), 0);

    unsigned short port;
    pid_t child = start_echo_server(&port);
    oi_reactor *r = oi_reactor_create();
    struct echo_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {echo_on_connected, echo_on_data,
                                         echo_on_error};
    CHECK_EQ(oi_llm_conn_connect(r, "127.0.0.1", port, 0, NULL, &cbs, &ctx,
                                  &ctx.conn),
              OI_OK);
    run_until(r, &ctx.done, 100);

    struct sigaction current;
    CHECK_EQ(sigaction(SIGPIPE, NULL, &current), 0);
    CHECK(current.sa_handler == custom_sigpipe_handler);
    CHECK_EQ(sigaction(SIGPIPE, &old, NULL), 0);

    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

/* --- a dead address moves the connect to the next address --- */

/* The test links with -Wl,--wrap=getaddrinfo. A lookup of DEAD_THEN_LIVE
 * returns two loopback addresses: first `dead_port`, then `live_port`. */
#define DEAD_THEN_LIVE "dead-then-live.test"
static unsigned short dead_port;
static unsigned short live_port;

int __real_getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints, struct addrinfo **res);

int __wrap_getaddrinfo(const char *node, const char *service,
                       const struct addrinfo *hints, struct addrinfo **res) {
    if (node == NULL || strcmp(node, DEAD_THEN_LIVE) != 0) {
        return __real_getaddrinfo(node, service, hints, res);
    }
    struct addrinfo numeric = *hints;
    numeric.ai_family = AF_INET;
    numeric.ai_flags |= AI_NUMERICHOST | AI_NUMERICSERV;
    char port[8];
    struct addrinfo *dead = NULL;
    struct addrinfo *live = NULL;
    snprintf(port, sizeof port, "%u", (unsigned)dead_port);
    int rc = __real_getaddrinfo("127.0.0.1", port, &numeric, &dead);
    if (rc != 0) {
        return rc;
    }
    snprintf(port, sizeof port, "%u", (unsigned)live_port);
    rc = __real_getaddrinfo("127.0.0.1", port, &numeric, &live);
    if (rc != 0) {
        freeaddrinfo(dead);
        return rc;
    }
    struct addrinfo *tail = dead;
    while (tail->ai_next != NULL) {
        tail = tail->ai_next;
    }
    tail->ai_next = live;
    *res = dead;
    return 0;
}

/* A listener that never accepts, with its accept queue full: the kernel
 * drops each new SYN, so a connect() to it gets no answer. */
static int start_dead_listener(unsigned short *out_port, int fillers[4]) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(bind(fd, (struct sockaddr *)&addr, sizeof addr), 0);
    CHECK_EQ(listen(fd, 0), 0);
    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(fd, (struct sockaddr *)&addr, &alen), 0);
    *out_port = ntohs(addr.sin_port);
    for (int i = 0; i < 4; i++) {
        fillers[i] = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(fillers[i] >= 0);
        fcntl(fillers[i], F_SETFL, O_NONBLOCK);
        connect(fillers[i], (struct sockaddr *)&addr, sizeof addr);
    }
    return fd;
}

static void close_on_connected(oi_llm_conn *c, void *ud) {
    struct echo_ctx *ctx = ud;
    ctx->connected = 1;
    oi_llm_conn_close(c);
    ctx->done = 1;
}

TEST(dead_address_moves_to_the_next_address) {
    int fillers[4];
    int dead_fd = start_dead_listener(&dead_port, fillers);
    pid_t child = start_echo_server(&live_port);

    oi_reactor *r = oi_reactor_create();
    struct echo_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {close_on_connected, echo_on_data,
                                         echo_on_error};
    oi_llm_conn *conn = NULL;
    CHECK_EQ(oi_llm_conn_connect(r, DEAD_THEN_LIVE, 1, 0, NULL, &cbs, &ctx,
                                  &conn),
              OI_OK);
    /* 6 s: more than the 3 s address timeout, much less than the
     * kernel's SYN retries on the dead address. */
    run_until(r, &ctx.done, 30);

    CHECK(ctx.connected);
    CHECK(!ctx.error_fired);
    oi_reactor_destroy(r);
    for (int i = 0; i < 4; i++) {
        close(fillers[i]);
    }
    close(dead_fd);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
}

/* --- an untrusted certificate names the verify result --- */

/* Serves one TLS connection with a fresh self-signed certificate, which
 * the system trust store does not contain. With `ca_path`, it also writes
 * the certificate there as PEM, so a client can trust it, and echoes the
 * first read back before it closes the socket with no TLS close_notify. */
static pid_t start_self_signed_tls_server(unsigned short *out_port,
                                          const char *ca_path) {
    EVP_PKEY *key = EVP_EC_gen("P-256");
    X509 *cert = X509_new();
    CHECK(key != NULL && cert != NULL);
    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 3600);
    X509_set_pubkey(cert, key);
    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *)"127.0.0.1", -1, -1, 0);
    X509_set_issuer_name(cert, name);
    /* A client checks an IP host against the subjectAltName, not the CN. */
    X509_EXTENSION *san = X509V3_EXT_conf_nid(NULL, NULL,
                                              NID_subject_alt_name,
                                              "IP:127.0.0.1");
    CHECK(san != NULL);
    CHECK_EQ(X509_add_ext(cert, san, -1), 1);
    X509_EXTENSION_free(san);
    CHECK(X509_sign(cert, key, EVP_sha256()) > 0);
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    CHECK(ctx != NULL);
    CHECK_EQ(SSL_CTX_use_certificate(ctx, cert), 1);
    CHECK_EQ(SSL_CTX_use_PrivateKey(ctx, key), 1);
    if (ca_path != NULL) {
        FILE *pem = fopen(ca_path, "w");
        CHECK(pem != NULL);
        CHECK_EQ(PEM_write_X509(pem, cert), 1);
        fclose(pem);
    }

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listen_fd >= 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof addr), 0);
    CHECK_EQ(listen(listen_fd, 1), 0);
    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &alen), 0);
    *out_port = ntohs(addr.sin_port);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd >= 0) {
            SSL *ssl = SSL_new(ctx);
            SSL_set_fd(ssl, cfd);
            if (SSL_accept(ssl) == 1 && ca_path != NULL) {
                char buf[64];
                int n = SSL_read(ssl, buf, sizeof buf);
                if (n > 0) {
                    SSL_write(ssl, buf, n);
                }
            }
            SSL_free(ssl);
            close(cfd);
        }
        _exit(0);
    }
    close(listen_fd);
    SSL_CTX_free(ctx);
    X509_free(cert);
    EVP_PKEY_free(key);
    return pid;
}

TEST(untrusted_certificate_names_the_verify_result) {
    unsigned short port;
    pid_t child = start_self_signed_tls_server(&port, NULL);

    oi_reactor *r = oi_reactor_create();
    struct error_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {err_on_connected, err_on_data,
                                         err_on_error};
    oi_llm_conn *conn = NULL;
    CHECK_EQ(oi_llm_conn_connect(r, "127.0.0.1", port, 1, NULL, &cbs, &ctx,
                                  &conn),
              OI_OK);
    run_until(r, &ctx.done, 100);

    CHECK(ctx.error_fired);
    CHECK_EQ(ctx.reason, OI_ERR_IO);
    CHECK_STREQ(ctx.cause, "TLS certificate of 127.0.0.1 is not trusted: "
                           "self-signed certificate");
    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

/* --- a TLS round trip through the connection socket BIO --- */

/* The test links with -Wl,--wrap=send. liboi's own sends come here;
 * libssl's built-in socket BIO uses write() and never does. */
static int sends_total;
static int sends_without_nosignal;

ssize_t __real_send(int fd, const void *buf, size_t len, int flags);

ssize_t __wrap_send(int fd, const void *buf, size_t len, int flags) {
    sends_total++;
    if ((flags & MSG_NOSIGNAL) == 0) {
        sends_without_nosignal++;
    }
    return __real_send(fd, buf, len, flags);
}

struct tls_echo_ctx {
    char received[16];
    size_t received_len;
    int done;
};

static void tls_on_connected(oi_llm_conn *c, void *ud) {
    (void)ud;
    CHECK_EQ(oi_llm_conn_write(c, "hello", 5), OI_OK);
}

static void tls_on_data(oi_llm_conn *c, const void *data, size_t len,
                        void *ud) {
    struct tls_echo_ctx *ctx = ud;
    CHECK(ctx->received_len + len <= sizeof ctx->received);
    memcpy(ctx->received + ctx->received_len, data, len);
    ctx->received_len += len;
    if (ctx->received_len == 5) {
        oi_llm_conn_close(c);
        ctx->done = 1;
    }
}

static void tls_on_error(oi_llm_conn *c, oi_status reason, void *ud) {
    (void)c;
    (void)reason;
    struct tls_echo_ctx *ctx = ud;
    ctx->done = 1;
}

TEST(tls_round_trip_sends_with_msg_nosignal) {
    char ca_path[64];
    snprintf(ca_path, sizeof ca_path, "/tmp/oi-test-ca-%d.pem", (int)getpid());
    unsigned short port;
    pid_t child = start_self_signed_tls_server(&port, ca_path);
    oi_reactor *r = oi_reactor_create();
    struct tls_echo_ctx ctx = {0};
    struct oi_llm_conn_callbacks cbs = {tls_on_connected, tls_on_data,
                                         tls_on_error};
    oi_llm_conn *conn = NULL;
    sends_total = 0;
    sends_without_nosignal = 0;
    CHECK_EQ(oi_llm_conn_connect(r, "127.0.0.1", port, 1, ca_path, &cbs, &ctx,
                                  &conn),
              OI_OK);
    run_until(r, &ctx.done, 100);

    CHECK_EQ(ctx.received_len, (size_t)5);
    CHECK(memcmp(ctx.received, "hello", 5) == 0);
    /* The handshake and the request: every TLS byte left through send()
     * with MSG_NOSIGNAL, so no write can raise SIGPIPE. */
    CHECK(sends_total >= 2);
    CHECK_EQ(sends_without_nosignal, 0);
    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
    unlink(ca_path);
}

int main(void) {
    /* A regression that hangs a child must fail this binary, not CI. */
    oi_test_set_deadline(900);
    signal(SIGCHLD, SIG_DFL);
    RUN(connect_write_echo_and_reentrant_close);
    RUN(connection_refused_reports_error);
    RUN(resolver_failure_is_reported_asynchronously);
    RUN(cancel_during_resolution_is_silent);
    RUN(peer_close_reports_closed);
    RUN(connect_rejects_bad_args);
    RUN(write_before_connected_rejected);
    RUN(close_null_safe);
    RUN(connection_preserves_sigpipe_disposition);
    RUN(dead_address_moves_to_the_next_address);
    RUN(untrusted_certificate_names_the_verify_result);
    RUN(tls_round_trip_sends_with_msg_nosignal);
    return oi_test_report();
}
