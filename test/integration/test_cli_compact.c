/* struct signalfd_siginfo / signalfd(2) are GNU/Linux extensions. */
#define _GNU_SOURCE

#include "../test.h"
#include "mock_api.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cli_compact.h"
#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"

/* A listening server that accepts one connection and then goes silent --
 * used to hold a request in flight indefinitely so a test can control
 * exactly when (if ever) cancellation fires, with no reliance on real
 * response timing. */
struct silent_server {
    pid_t pid;
    int port;
};

static int start_silent_server(struct silent_server *server) {
    memset(server, 0, sizeof *server);
    server->pid = -1;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return 0;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(listen_fd, 1) != 0) {
        close(listen_fd);
        return 0;
    }
    socklen_t alen = sizeof addr;
    if (getsockname(listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
        close(listen_fd);
        return 0;
    }
    server->port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid < 0) {
        close(listen_fd);
        return 0;
    }
    if (pid == 0) {
        signal(SIGPIPE, SIG_IGN);
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd >= 0) {
            struct timespec delay = {5, 0};
            nanosleep(&delay, NULL);
            close(cfd);
        }
        close(listen_fd);
        _exit(0);
    }
    close(listen_fd);
    server->pid = pid;
    return 1;
}

static void stop_silent_server(struct silent_server *server) {
    if (server->pid > 0) {
        kill(server->pid, SIGKILL);
        while (waitpid(server->pid, NULL, 0) < 0 && errno == EINTR) {
            /* retry */
        }
    }
}

static void raise_sigint(oi_reactor *r, void *user_data) {
    (void)r;
    (void)user_data;
    raise(SIGINT);
}

static void raise_sigterm(oi_reactor *r, void *user_data) {
    (void)r;
    (void)user_data;
    raise(SIGTERM);
}

static int make_test_signal_fd(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0) {
        return -1;
    }
    return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

static void unblock_test_signals(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGHUP);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
}

TEST(compact_run_produces_a_summary_on_success) {
    const char *response =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"This is the summary.\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    struct mock_turn turns[1] = {{NULL, response, 0}};
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    CHECK(reactor != NULL);
    CHECK(arena != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    const char *body = "{\"model\":\"m\",\"stream\":true,\"messages\":[]}";
    struct oi_cli_compact_result result;
    CHECK_EQ(oi_cli_compact_run(client, reactor, arena, -1, body,
                                strlen(body), &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_COMPACT_OK);
    CHECK_EQ(result.http_status, 200);
    CHECK_STREQ(result.summary, "This is the summary.");
    CHECK_EQ(result.summary_len, strlen("This is the summary."));
    {
        size_t request_len = 0;
        char *request = mock_api_request(&api, 0, &request_len);
        CHECK(request != NULL);
        CHECK(strstr(request, "\"model\":\"m\"") != NULL);
        free(request);
    }

    oi_cli_compact_result_free(&result);
    CHECK(result.summary == NULL);
    oi_llm_client_destroy(client);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(compact_run_reports_failed_on_http_error) {
    const char *body_500 = "internal error";
    struct mock_turn turns[1] = {
        {"HTTP/1.1 500 Internal Server Error", body_500, 0}};
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    CHECK(reactor != NULL);
    CHECK(arena != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    const char *body = "{\"model\":\"m\",\"stream\":true,\"messages\":[]}";
    struct oi_cli_compact_result result;
    CHECK_EQ(oi_cli_compact_run(client, reactor, arena, -1, body,
                                strlen(body), &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_COMPACT_FAILED);
    CHECK(result.status != OI_OK);
    CHECK_EQ(result.http_status, 500);
    CHECK(result.summary == NULL);

    oi_cli_compact_result_free(&result);
    oi_llm_client_destroy(client);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(compact_run_reports_failed_on_malformed_body) {
    const char *response = "data: {this is not json}\n\n";
    struct mock_turn turns[1] = {{NULL, response, 0}};
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    CHECK(reactor != NULL);
    CHECK(arena != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = api.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    const char *body = "{\"model\":\"m\",\"stream\":true,\"messages\":[]}";
    struct oi_cli_compact_result result;
    CHECK_EQ(oi_cli_compact_run(client, reactor, arena, -1, body,
                                strlen(body), &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_COMPACT_FAILED);
    CHECK(result.status != OI_OK);
    CHECK(result.summary == NULL);

    oi_cli_compact_result_free(&result);
    oi_llm_client_destroy(client);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    mock_api_stop(&api);
}

TEST(compact_run_cancels_on_sigint_and_never_calls_on_done) {
    struct silent_server server;
    CHECK(start_silent_server(&server));

    int signal_fd = make_test_signal_fd();
    CHECK(signal_fd >= 0);

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    CHECK(reactor != NULL);
    CHECK(arena != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = server.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_reactor_timer *timer = NULL;
    CHECK_EQ(oi_reactor_timer_start(reactor, 30, raise_sigint, NULL, &timer),
             OI_OK);

    const char *body = "{\"model\":\"m\",\"stream\":true,\"messages\":[]}";
    struct oi_cli_compact_result result;
    CHECK_EQ(oi_cli_compact_run(client, reactor, arena, signal_fd, body,
                                strlen(body), &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_COMPACT_CANCELLED);
    CHECK_EQ(result.terminate_signal, 0);
    CHECK(result.summary == NULL);

    oi_reactor_timer_cancel(timer);
    oi_cli_compact_result_free(&result);
    oi_llm_client_destroy(client);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    close(signal_fd);
    unblock_test_signals();
    stop_silent_server(&server);
}

TEST(compact_run_cancels_on_sigterm_and_reports_the_signal) {
    struct silent_server server;
    CHECK(start_silent_server(&server));

    int signal_fd = make_test_signal_fd();
    CHECK(signal_fd >= 0);

    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(64 * 1024);
    CHECK(reactor != NULL);
    CHECK(arena != NULL);

    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = server.port,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 5000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);

    oi_reactor_timer *timer = NULL;
    CHECK_EQ(
        oi_reactor_timer_start(reactor, 30, raise_sigterm, NULL, &timer),
        OI_OK);

    const char *body = "{\"model\":\"m\",\"stream\":true,\"messages\":[]}";
    struct oi_cli_compact_result result;
    CHECK_EQ(oi_cli_compact_run(client, reactor, arena, signal_fd, body,
                                strlen(body), &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_COMPACT_CANCELLED);
    CHECK_EQ(result.terminate_signal, SIGTERM);
    CHECK(result.summary == NULL);

    oi_reactor_timer_cancel(timer);
    oi_cli_compact_result_free(&result);
    oi_llm_client_destroy(client);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
    close(signal_fd);
    unblock_test_signals();
    stop_silent_server(&server);
}

TEST(compact_run_rejects_invalid_arguments) {
    oi_reactor *reactor = oi_reactor_create();
    oi_arena *arena = oi_arena_create(4096);
    CHECK(reactor != NULL);
    CHECK(arena != NULL);
    struct oi_llm_config llm_config = {
        .host = "127.0.0.1",
        .port = 9,
        .use_tls = 0,
        .api_key = "test",
        .path = "/v1/chat/completions",
        .timeout_ms = 1000,
    };
    oi_llm_client *client = oi_llm_client_create(&llm_config);
    CHECK(client != NULL);
    const char *body = "{}";
    struct oi_cli_compact_result result;

    CHECK_EQ(oi_cli_compact_run(NULL, reactor, arena, -1, body, 2, &result),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_compact_run(client, NULL, arena, -1, body, 2, &result),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_compact_run(client, reactor, NULL, -1, body, 2, &result),
             OI_ERR_INVAL);
    CHECK_EQ(
        oi_cli_compact_run(client, reactor, arena, -1, NULL, 2, &result),
        OI_ERR_INVAL);
    CHECK_EQ(oi_cli_compact_run(client, reactor, arena, -1, body, 0, &result),
             OI_ERR_INVAL);
    CHECK_EQ(
        oi_cli_compact_run(client, reactor, arena, -1, body, 2, NULL),
        OI_ERR_INVAL);

    oi_llm_client_destroy(client);
    oi_arena_destroy(arena);
    oi_reactor_destroy(reactor);
}

int main(void) {
    RUN(compact_run_produces_a_summary_on_success);
    RUN(compact_run_reports_failed_on_http_error);
    RUN(compact_run_reports_failed_on_malformed_body);
    RUN(compact_run_cancels_on_sigint_and_never_calls_on_done);
    RUN(compact_run_cancels_on_sigterm_and_reports_the_signal);
    RUN(compact_run_rejects_invalid_arguments);
    return oi_test_report();
}
