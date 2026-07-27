#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "test.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Reads (and discards) whatever the client sends until the socket is
 * quiet for 150ms -- test-only stand-in for actually parsing
 * Content-Length, since the mock server doesn't need to validate the
 * request, only let the client finish writing it before responding. */
static void drain_request(int cfd) {
    char buf[8192];
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(cfd, &rfds);
        struct timeval tv = {0, 150000};
        int rc = select(cfd + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) {
            break;
        }
        ssize_t n = read(cfd, buf, sizeof buf);
        if (n <= 0) {
            break;
        }
    }
}

static pid_t start_mock_server(const char *response, size_t response_len,
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
            drain_request(cfd);
            size_t off = 0;
            while (off < response_len) {
                ssize_t w = write(cfd, response + off, response_len - off);
                if (w <= 0) {
                    break;
                }
                off += (size_t)w;
            }
            close(cfd);
        }
        close(listen_fd);
        _exit(0);
    }
    close(listen_fd);
    return pid;
}

static char *build_chunked_response(const char *body, size_t body_len,
                                     const char *status_line,
                                     size_t *out_total_len) {
    char preamble[256];
    int pn = snprintf(preamble, sizeof preamble,
                       "%s\r\nTransfer-Encoding: chunked\r\n\r\n",
                       status_line);
    char chunk_hdr[32];
    int cn = snprintf(chunk_hdr, sizeof chunk_hdr, "%zx\r\n", body_len);

    size_t total = (size_t)pn + (size_t)cn + body_len + 2 + 5;
    char *buf = malloc(total);
    size_t off = 0;
    memcpy(buf + off, preamble, (size_t)pn);
    off += (size_t)pn;
    memcpy(buf + off, chunk_hdr, (size_t)cn);
    off += (size_t)cn;
    memcpy(buf + off, body, body_len);
    off += body_len;
    memcpy(buf + off, "\r\n", 2);
    off += 2;
    memcpy(buf + off, "0\r\n\r\n", 5);
    off += 5;

    *out_total_len = off;
    return buf;
}

static char *build_content_length_response(const char *body, size_t body_len,
                                            const char *status_line,
                                            size_t *out_total_len) {
    char header[256];
    int hn = snprintf(header, sizeof header,
                       "%s\r\nContent-Length: %zu\r\n\r\n", status_line,
                       body_len);
    size_t total = (size_t)hn + body_len;
    char *buf = malloc(total);
    memcpy(buf, header, (size_t)hn);
    memcpy(buf + hn, body, body_len);
    *out_total_len = total;
    return buf;
}

static void run_until(oi_reactor *r, const int *done_flag, int max_steps) {
    for (int i = 0; i < max_steps && !*done_flag; i++) {
        oi_status st;
        oi_reactor_step(r, 200, &st);
    }
}

/* --- successful streaming completion --- */

struct stream_ctx {
    char text[256];
    size_t text_len;
    int delta_count;
    int done;
    oi_status done_status;
    int http_status;
};

static void on_delta(const char *text, size_t len, void *ud) {
    struct stream_ctx *ctx = ud;
    CHECK(ctx->text_len + len <= sizeof ctx->text);
    memcpy(ctx->text + ctx->text_len, text, len);
    ctx->text_len += len;
    ctx->delta_count++;
}

static void on_done(oi_status status, int http_status, const char *error_body,
                     size_t error_body_len, void *ud) {
    (void)error_body;
    (void)error_body_len;
    struct stream_ctx *ctx = ud;
    ctx->done_status = status;
    ctx->http_status = http_status;
    ctx->done = 1;
}

TEST(streaming_success_delivers_deltas) {
    const char *sse_body =
        "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"Hello\"}}]}\n\n"
        "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\", world\"}}]}\n\n"
        "data: {\"id\":\"1\",\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    size_t total;
    char *response = build_chunked_response(sse_body, strlen(sse_body),
                                             "HTTP/1.1 200 OK", &total);

    unsigned short port;
    pid_t child = start_mock_server(response, total, &port);
    free(response);

    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct oi_llm_config cfg = {"127.0.0.1", port, 0, NULL, NULL,
                                 "/v1/chat/completions"};
    oi_llm_client *client = oi_llm_client_create(&cfg);
    CHECK(client != NULL);

    struct stream_ctx ctx = {0};
    oi_llm_request *req = NULL;
    const char *body = "{\"model\":\"x\",\"stream\":true}";
    oi_status st = oi_llm_request_start(client, r, a, body, strlen(body),
                                         on_delta, on_done, &ctx, &req);
    CHECK_EQ(st, OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK_EQ(ctx.done_status, OI_OK);
    CHECK_EQ(ctx.http_status, 200);
    CHECK_EQ(ctx.delta_count, 2); /* the empty-delta finish chunk adds none */
    CHECK_EQ(ctx.text_len, strlen("Hello, world"));
    CHECK(memcmp(ctx.text, "Hello, world", ctx.text_len) == 0);

    oi_llm_client_destroy(client);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

/* --- non-2xx status surfaces the error body --- */

struct error_ctx {
    int done;
    oi_status done_status;
    int http_status;
    char error_body[256];
    size_t error_body_len;
    int delta_count;
};

static void err_on_delta(const char *text, size_t len, void *ud) {
    (void)text;
    (void)len;
    struct error_ctx *ctx = ud;
    ctx->delta_count++;
}

static void err_on_done(oi_status status, int http_status,
                         const char *error_body, size_t error_body_len,
                         void *ud) {
    struct error_ctx *ctx = ud;
    ctx->done_status = status;
    ctx->http_status = http_status;
    if (error_body) {
        CHECK(error_body_len <= sizeof ctx->error_body);
        memcpy(ctx->error_body, error_body, error_body_len);
    }
    ctx->error_body_len = error_body_len;
    ctx->done = 1;
}

TEST(non_2xx_status_reports_error_body) {
    const char *err_body = "{\"error\":{\"message\":\"bad key\"}}";
    size_t total;
    char *response = build_content_length_response(
        err_body, strlen(err_body), "HTTP/1.1 401 Unauthorized", &total);

    unsigned short port;
    pid_t child = start_mock_server(response, total, &port);
    free(response);

    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct oi_llm_config cfg = {"127.0.0.1", port, 0, NULL, "sk-bad",
                                 "/v1/chat/completions"};
    oi_llm_client *client = oi_llm_client_create(&cfg);

    struct error_ctx ctx = {0};
    oi_llm_request *req = NULL;
    const char *body = "{\"model\":\"x\",\"stream\":true}";
    oi_status st = oi_llm_request_start(client, r, a, body, strlen(body),
                                         err_on_delta, err_on_done, &ctx,
                                         &req);
    CHECK_EQ(st, OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK(ctx.done_status != OI_OK);
    CHECK_EQ(ctx.http_status, 401);
    CHECK_EQ(ctx.delta_count, 0);
    CHECK_EQ(ctx.error_body_len, strlen(err_body));
    CHECK(memcmp(ctx.error_body, err_body, ctx.error_body_len) == 0);

    oi_llm_client_destroy(client);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

/* --- malformed SSE payload surfaces a parse error, not silent loss --- */

struct malformed_ctx {
    int done;
    oi_status done_status;
    int delta_count;
};

static void mal_on_delta(const char *text, size_t len, void *ud) {
    (void)text;
    (void)len;
    struct malformed_ctx *ctx = ud;
    ctx->delta_count++;
}

static void mal_on_done(oi_status status, int http_status,
                         const char *error_body, size_t error_body_len,
                         void *ud) {
    (void)http_status;
    (void)error_body;
    (void)error_body_len;
    struct malformed_ctx *ctx = ud;
    ctx->done_status = status;
    ctx->done = 1;
}

TEST(malformed_sse_json_is_reported_not_dropped) {
    const char *sse_body = "data: {this is not json}\n\n";
    size_t total;
    char *response = build_chunked_response(sse_body, strlen(sse_body),
                                             "HTTP/1.1 200 OK", &total);

    unsigned short port;
    pid_t child = start_mock_server(response, total, &port);
    free(response);

    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct oi_llm_config cfg = {"127.0.0.1", port, 0, NULL, NULL,
                                 "/v1/chat/completions"};
    oi_llm_client *client = oi_llm_client_create(&cfg);

    struct malformed_ctx ctx = {0};
    oi_llm_request *req = NULL;
    const char *body = "{}";
    oi_status st = oi_llm_request_start(client, r, a, body, strlen(body),
                                         mal_on_delta, mal_on_done, &ctx,
                                         &req);
    CHECK_EQ(st, OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK_EQ(ctx.done_status, OI_ERR_PARSE);
    CHECK_EQ(ctx.delta_count, 0);

    oi_llm_client_destroy(client);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

/* --- cancellation from within on_delta --- */

struct cancel_ctx {
    oi_llm_request *req;
    int delta_count;
    int done_fired;
};

static void cancel_on_delta(const char *text, size_t len, void *ud) {
    (void)text;
    (void)len;
    struct cancel_ctx *ctx = ud;
    ctx->delta_count++;
    oi_llm_request_cancel(ctx->req);
}

static void cancel_on_done(oi_status status, int http_status,
                            const char *error_body, size_t error_body_len,
                            void *ud) {
    (void)status;
    (void)http_status;
    (void)error_body;
    (void)error_body_len;
    struct cancel_ctx *ctx = ud;
    ctx->done_fired = 1; /* must never happen */
}

TEST(cancel_from_within_on_delta) {
    const char *sse_body =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"a\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"b\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t total;
    char *response = build_chunked_response(sse_body, strlen(sse_body),
                                             "HTTP/1.1 200 OK", &total);

    unsigned short port;
    pid_t child = start_mock_server(response, total, &port);
    free(response);

    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct oi_llm_config cfg = {"127.0.0.1", port, 0, NULL, NULL,
                                 "/v1/chat/completions"};
    oi_llm_client *client = oi_llm_client_create(&cfg);

    struct cancel_ctx ctx = {0};
    const char *body = "{}";
    oi_status st = oi_llm_request_start(client, r, a, body, strlen(body),
                                         cancel_on_delta, cancel_on_done,
                                         &ctx, &ctx.req);
    CHECK_EQ(st, OI_OK);

    int never_done = 0;
    run_until(r, &never_done, 20); /* bounded: cancellation means no "done" signal to wait for */

    CHECK_EQ(ctx.delta_count, 1); /* canceled after the first delta */
    CHECK(!ctx.done_fired);

    oi_llm_client_destroy(client);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    waitpid(child, NULL, 0);
}

/* --- argument validation --- */

TEST(start_rejects_bad_args) {
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct oi_llm_config cfg = {"127.0.0.1", 1, 0, NULL, NULL, "/x"};
    oi_llm_client *client = oi_llm_client_create(&cfg);
    oi_llm_request *req;

    CHECK_EQ(oi_llm_request_start(NULL, r, a, "{}", 2, NULL, NULL, NULL,
                                   &req),
              OI_ERR_INVAL);
    CHECK_EQ(oi_llm_request_start(client, NULL, a, "{}", 2, NULL, NULL, NULL,
                                   &req),
              OI_ERR_INVAL);
    CHECK_EQ(oi_llm_request_start(client, r, NULL, "{}", 2, NULL, NULL, NULL,
                                   &req),
              OI_ERR_INVAL);
    CHECK_EQ(oi_llm_request_start(client, r, a, NULL, 2, NULL, NULL, NULL,
                                   &req),
              OI_ERR_INVAL);
    CHECK_EQ(oi_llm_request_start(client, r, a, "{}", 2, NULL, NULL, NULL,
                                   NULL),
              OI_ERR_INVAL);

    CHECK(oi_llm_client_create(NULL) == NULL);

    oi_llm_client_destroy(client);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
}

TEST(cancel_null_safe) { oi_llm_request_cancel(NULL); }

int main(void) {
    signal(SIGCHLD, SIG_DFL);
    RUN(streaming_success_delivers_deltas);
    RUN(non_2xx_status_reports_error_body);
    RUN(malformed_sse_json_is_reported_not_dropped);
    RUN(cancel_from_within_on_delta);
    RUN(start_rejects_bad_args);
    RUN(cancel_null_safe);
    return oi_test_report();
}
