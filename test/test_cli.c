/*
 * Exercises the actual compiled `oi` binary (build/oi) as a subprocess
 * rather than any library code directly -- this is the one test in the
 * suite whose job is to prove the CLI wrapper itself works, not just
 * the pieces it's built from. Assumes CWD is the repo root, matching
 * how `make test` invokes every test binary.
 */
#include "test.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define OI_BIN "build/oi"

/* --- mock SSE server, same shape as test_llm.c's --- */

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

/* --- run the built oi binary, capturing stdout+stderr --- */

struct run_result {
    char output[4096];
    size_t output_len;
    int exit_code;
};

static void run_cli(char *const argv[], struct run_result *out) {
    memset(out, 0, sizeof *out);

    int pipefd[2];
    CHECK_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(OI_BIN, argv);
        _exit(127);
    }
    close(pipefd[1]);

    ssize_t n;
    while (out->output_len < sizeof out->output - 1 &&
           (n = read(pipefd[0], out->output + out->output_len,
                      sizeof out->output - 1 - out->output_len)) > 0) {
        out->output_len += (size_t)n;
    }
    close(pipefd[0]);

    int status;
    waitpid(pid, &status, 0);
    out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* --- tests requiring no network --- */

TEST(help_exits_zero) {
    char *argv[] = {(char *)OI_BIN, (char *)"--help", NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "usage:") != NULL);
}

TEST(missing_api_key_fails) {
    char *argv[] = {(char *)OI_BIN, (char *)"hello", NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 1);
    CHECK(strstr(r.output, "API key") != NULL);
}

TEST(unrecognized_flag_fails) {
    char *argv[] = {(char *)OI_BIN, (char *)"--not-a-real-flag", NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 1);
    CHECK(strstr(r.output, "unrecognized") != NULL);
}

TEST(flag_missing_value_fails) {
    char *argv[] = {(char *)OI_BIN, (char *)"--host", NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 1);
}

TEST(dry_run_reports_resolved_config) {
    char *argv[] = {(char *)OI_BIN,        (char *)"--dry-run",
                     (char *)"--host",      (char *)"example.test",
                     (char *)"--port",      (char *)"1234",
                     (char *)"--no-tls",    (char *)"--model",
                     (char *)"test-model",  (char *)"a prompt",
                     NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "host: example.test") != NULL);
    CHECK(strstr(r.output, "port: 1234") != NULL);
    CHECK(strstr(r.output, "use_tls: false") != NULL);
    CHECK(strstr(r.output, "test-model") != NULL);
    CHECK(strstr(r.output, "\"content\":\"a prompt\"") != NULL);
}

TEST(dry_run_does_not_require_api_key) {
    char *argv[] = {(char *)OI_BIN, (char *)"--dry-run", (char *)"x", NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 0);
}

TEST(config_file_is_applied) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/oi_cli_test_conf_%d.conf",
             (int)getpid());
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("model = from-config-file\n", f);
    fclose(f);

    char *argv[] = {(char *)OI_BIN, (char *)"--dry-run", (char *)"--config",
                     path,           (char *)"hi",         NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "model: from-config-file") != NULL);

    unlink(path);
}

TEST(cli_flag_overrides_config_file) {
    char path[256];
    snprintf(path, sizeof path, "/tmp/oi_cli_test_conf2_%d.conf",
             (int)getpid());
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs("model = file-model\n", f);
    fclose(f);

    char *argv[] = {(char *)OI_BIN,   (char *)"--dry-run",
                     (char *)"--config", path,
                     (char *)"--model", (char *)"cli-model",
                     (char *)"hi",       NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "model: cli-model") != NULL);
    CHECK(strstr(r.output, "file-model") == NULL);

    unlink(path);
}

TEST(missing_config_file_fails) {
    char *argv[] = {(char *)OI_BIN, (char *)"--dry-run", (char *)"--config",
                     (char *)"/nonexistent/oi_test.conf", (char *)"hi",
                     NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 1);
}

/* --- end-to-end streaming through a mock server --- */

TEST(end_to_end_streaming_reply) {
    const char *sse_body =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"Hello\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\", CLI\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t total;
    char *response = build_chunked_response(sse_body, strlen(sse_body),
                                             "HTTP/1.1 200 OK", &total);
    unsigned short port;
    pid_t child = start_mock_server(response, total, &port);
    free(response);

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    char session_dir[] = "/tmp";
    char session_name[64];
    snprintf(session_name, sizeof session_name, "oi-cli-e2e-%d",
             (int)getpid());
    char log_path[128];
    snprintf(log_path, sizeof log_path, "%s/%s.oilog", session_dir,
             session_name);
    unlink(log_path);

    char *argv[] = {(char *)OI_BIN,
                     (char *)"--host",
                     (char *)"127.0.0.1",
                     (char *)"--port",
                     port_str,
                     (char *)"--no-tls",
                     (char *)"--api-key",
                     (char *)"test-key",
                     (char *)"--session-dir",
                     session_dir,
                     (char *)"--session",
                     session_name,
                     (char *)"say hi",
                     NULL};
    struct run_result r;
    run_cli(argv, &r);

    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "Hello, CLI") != NULL);

    waitpid(child, NULL, 0);
    unlink(log_path);
}

TEST(resume_replays_prior_exchange) {
    char session_dir[] = "/tmp";
    char session_name[64];
    snprintf(session_name, sizeof session_name, "oi-cli-resume-%d",
             (int)getpid());
    char log_path[128];
    snprintf(log_path, sizeof log_path, "%s/%s.oilog", session_dir,
             session_name);
    unlink(log_path);

    /* first exchange */
    const char *sse1 = "data: {\"choices\":[{\"index\":0,\"delta\":{"
                        "\"content\":\"first-reply\"}}]}\n\ndata: "
                        "[DONE]\n\n";
    size_t total1;
    char *response1 =
        build_chunked_response(sse1, strlen(sse1), "HTTP/1.1 200 OK", &total1);
    unsigned short port1;
    pid_t child1 = start_mock_server(response1, total1, &port1);
    free(response1);

    char port1_str[16];
    snprintf(port1_str, sizeof port1_str, "%u", (unsigned)port1);
    char *argv1[] = {(char *)OI_BIN,
                      (char *)"--host",
                      (char *)"127.0.0.1",
                      (char *)"--port",
                      port1_str,
                      (char *)"--no-tls",
                      (char *)"--api-key",
                      (char *)"test-key",
                      (char *)"--session-dir",
                      session_dir,
                      (char *)"--session",
                      session_name,
                      (char *)"first prompt",
                      NULL};
    struct run_result r1;
    run_cli(argv1, &r1);
    CHECK_EQ(r1.exit_code, 0);
    waitpid(child1, NULL, 0);

    /* second run against the same session: must replay the first */
    const char *sse2 = "data: {\"choices\":[{\"index\":0,\"delta\":{"
                        "\"content\":\"second-reply\"}}]}\n\ndata: "
                        "[DONE]\n\n";
    size_t total2;
    char *response2 =
        build_chunked_response(sse2, strlen(sse2), "HTTP/1.1 200 OK", &total2);
    unsigned short port2;
    pid_t child2 = start_mock_server(response2, total2, &port2);
    free(response2);

    char port2_str[16];
    snprintf(port2_str, sizeof port2_str, "%u", (unsigned)port2);
    char *argv2[] = {(char *)OI_BIN,
                      (char *)"--host",
                      (char *)"127.0.0.1",
                      (char *)"--port",
                      port2_str,
                      (char *)"--no-tls",
                      (char *)"--api-key",
                      (char *)"test-key",
                      (char *)"--session-dir",
                      session_dir,
                      (char *)"--session",
                      session_name,
                      (char *)"second prompt",
                      NULL};
    struct run_result r2;
    run_cli(argv2, &r2);
    CHECK_EQ(r2.exit_code, 0);
    CHECK(strstr(r2.output, "[resumed user] first prompt") != NULL);
    CHECK(strstr(r2.output, "[resumed assistant] first-reply") != NULL);
    CHECK(strstr(r2.output, "second-reply") != NULL);

    waitpid(child2, NULL, 0);
    unlink(log_path);
}

int main(void) {
    signal(SIGCHLD, SIG_DFL);
    RUN(help_exits_zero);
    RUN(missing_api_key_fails);
    RUN(unrecognized_flag_fails);
    RUN(flag_missing_value_fails);
    RUN(dry_run_reports_resolved_config);
    RUN(dry_run_does_not_require_api_key);
    RUN(config_file_is_applied);
    RUN(cli_flag_overrides_config_file);
    RUN(missing_config_file_fails);
    RUN(end_to_end_streaming_reply);
    RUN(resume_replays_prior_exchange);
    return oi_test_report();
}
