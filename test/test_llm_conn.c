#include "../src/llm_conn.h"
#include "oi/reactor.h"
#include "test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
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

    oi_status st = oi_llm_conn_connect(r, "127.0.0.1", port, 0, NULL, &cbs,
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
    (void)c;
    struct error_ctx *ctx = ud;
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

int main(void) {
    signal(SIGCHLD, SIG_DFL);
    RUN(connect_write_echo_and_reentrant_close);
    RUN(connection_refused_reports_error);
    RUN(peer_close_reports_closed);
    RUN(connect_rejects_bad_args);
    RUN(write_before_connected_rejected);
    RUN(close_null_safe);
    return oi_test_report();
}
