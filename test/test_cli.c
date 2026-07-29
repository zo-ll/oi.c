/*
 * Exercises the actual compiled `oi` binary (build/oi) as a subprocess
 * rather than any library code directly -- this is the one test in the
 * suite whose job is to prove the CLI wrapper itself works, not just
 * the pieces it's built from. Assumes CWD is the repo root, matching
 * how `make test` invokes every test binary.
 */
#include "test.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The Makefile defines OI_CLI_BIN to match whatever $(BUILD) directory
 * this test binary itself was compiled into (build/, build-asan/, ...)
 * -- hardcoding "build/oi" here caused a real hang: under `make asan`,
 * the plain build/oi didn't exist (a stale/absent path after `make
 * clean`), execv() failed, and the orphaned mock-server helper below
 * was left blocked in accept() forever with nothing to ever connect to
 * it, which then hung this test's waitpid() indefinitely. Falls back to
 * "build/oi" only for a manual/non-Makefile compile. */
#ifndef OI_CLI_BIN
#define OI_CLI_BIN "build/oi"
#endif
#define OI_BIN OI_CLI_BIN

/* --- mock SSE server, same shape as test_llm.c's --- */

static void drain_request(int cfd, int capture_fd) {
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
        if (capture_fd >= 0) {
            ssize_t unused = write(capture_fd, buf, (size_t)n);
            (void)unused;
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

static pid_t start_mock_server_turns_capture(
    const char *const *responses, const size_t *response_lens,
    size_t response_count, unsigned short *out_port,
    const char *capture_path) {
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
    CHECK_EQ(listen(listen_fd, (int)response_count), 0);

    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &alen), 0);
    *out_port = ntohs(addr.sin_port);
    int capture_fd = -1;
    if (capture_path != NULL) {
        capture_fd =
            open(capture_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        CHECK(capture_fd >= 0);
    }

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        /* Bounded, not accept() outright: if the CLI process we expect
         * to connect never does (e.g. it failed to even start), this
         * must not hang forever -- see the OI_CLI_BIN comment above for
         * exactly the incident that motivated this. */
        for (size_t turn = 0; turn < response_count; turn++) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listen_fd, &rfds);
            struct timeval tv = {20, 0};
            int rc = select(listen_fd + 1, &rfds, NULL, NULL, &tv);
            int cfd = rc > 0 ? accept(listen_fd, NULL, NULL) : -1;
            if (cfd < 0) {
                fprintf(stderr,
                        "[mock_server] gave up waiting for a connection on "
                        "turn %zu (select rc=%d)\n",
                        turn, rc);
                fflush(stderr);
                break;
            }
            drain_request(cfd, capture_fd);
            size_t off = 0;
            while (off < response_lens[turn]) {
                ssize_t w =
                    write(cfd, responses[turn] + off,
                          response_lens[turn] - off);
                if (w <= 0) {
                    break;
                }
                off += (size_t)w;
            }
            close(cfd);
        }
        if (capture_fd >= 0) {
            close(capture_fd);
        }
        close(listen_fd);
        _exit(0);
    }
    if (capture_fd >= 0) {
        close(capture_fd);
    }
    close(listen_fd);
    return pid;
}

static pid_t start_mock_server_turns(const char *const *responses,
                                     const size_t *response_lens,
                                     size_t response_count,
                                     unsigned short *out_port) {
    return start_mock_server_turns_capture(
        responses, response_lens, response_count, out_port, NULL);
}

static pid_t start_mock_server(const char *response, size_t response_len,
                                unsigned short *out_port) {
    const char *responses[] = {response};
    size_t lengths[] = {response_len};
    return start_mock_server_turns(responses, lengths, 1, out_port);
}

struct slow_mock_turn {
    const char *response;
    size_t response_len;
    int delay_seconds;
};

/*
 * A standalone mock server (not the shared start_mock_server_turns_capture
 * used everywhere else) that sleeps for each turn's delay_seconds after
 * accepting its connection and reading its request, before writing that
 * turn's response -- needed to make a Ctrl+C-during-a-turn test reliably
 * reach the CLI while a request is genuinely still in flight, rather than
 * racing a same-host loopback round trip that would otherwise complete
 * before the test could ever send the signal. Later turns typically use
 * delay_seconds=0 to verify the REPL is still usable after a cancel.
 */
static pid_t start_slow_mock_server(const struct slow_mock_turn *turns,
                                    size_t turn_count,
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
    CHECK_EQ(listen(listen_fd, (int)turn_count), 0);

    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &alen), 0);
    *out_port = ntohs(addr.sin_port);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        signal(SIGPIPE, SIG_IGN);
        for (size_t i = 0; i < turn_count; i++) {
            fd_set rfds;
            struct timeval tv = {20, 0};
            int cfd;

            FD_ZERO(&rfds);
            FD_SET(listen_fd, &rfds);
            cfd = select(listen_fd + 1, &rfds, NULL, NULL, &tv) > 0
                      ? accept(listen_fd, NULL, NULL)
                      : -1;
            if (cfd < 0) {
                break;
            }
            drain_request(cfd, -1);
            if (turns[i].delay_seconds > 0) {
                sleep((unsigned)turns[i].delay_seconds);
            }
            size_t off = 0;
            while (off < turns[i].response_len) {
                ssize_t w = write(cfd, turns[i].response + off,
                                  turns[i].response_len - off);
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

/*
 * A single-turn mock server that writes `response` in two pieces (the
 * first `split_at` bytes, a pause, then the rest) over the same
 * connection -- unlike start_slow_mock_server's one-sleep-then-write-it-
 * all shape, this puts a real mid-stream gap *inside* the SSE body itself,
 * so a client is genuinely still assembling a partial assistant delta (not
 * just waiting for the first byte) when the pause happens. Used to
 * exercise typing concurrently with an in-flight, partially-delivered
 * response.
 */
static pid_t start_split_response_mock_server(const char *response,
                                              size_t response_len,
                                              size_t split_at,
                                              int pause_ms,
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
        fd_set rfds;
        struct timeval tv = {20, 0};
        int cfd;

        signal(SIGPIPE, SIG_IGN);
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        cfd = select(listen_fd + 1, &rfds, NULL, NULL, &tv) > 0
                  ? accept(listen_fd, NULL, NULL)
                  : -1;
        if (cfd >= 0) {
            drain_request(cfd, -1);
            {
                size_t off = 0;
                size_t first_len = split_at < response_len ? split_at
                                                            : response_len;
                while (off < first_len) {
                    ssize_t w = write(cfd, response + off, first_len - off);
                    if (w <= 0) {
                        break;
                    }
                    off += (size_t)w;
                }
            }
            {
                struct timespec delay = {pause_ms / 1000,
                                         (pause_ms % 1000) * 1000000L};
                nanosleep(&delay, NULL);
            }
            {
                size_t off = split_at < response_len ? split_at
                                                     : response_len;
                while (off < response_len) {
                    ssize_t w =
                        write(cfd, response + off, response_len - off);
                    if (w <= 0) {
                        break;
                    }
                    off += (size_t)w;
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

/* --- run the built oi binary, capturing stdout+stderr --- */

struct run_result {
    char output[4096];
    size_t output_len;
    int exit_code;
};

struct interactive_result {
    char output[16384];
    size_t output_len;
    int exit_code;
};

static size_t count_text(const char *haystack, const char *needle) {
    size_t count = 0;
    size_t needle_len = strlen(needle);
    const char *cursor = haystack;

    while ((cursor = strstr(cursor, needle)) != NULL) {
        count++;
        cursor += needle_len;
    }
    return count;
}

static int interactive_wait_for(int master_fd,
                                struct interactive_result *result,
                                const char *text, size_t minimum_count) {
    while (result->output_len < sizeof result->output - 1) {
        fd_set reads;
        /* Bounded, not a real deadline: a slower CI runner under
         * asan/tsan can legitimately take longer than a quiet local
         * machine for one round trip. Each read-to-read gap gets its own
         * fresh window, so this only matters when data genuinely stalls. */
        struct timeval timeout = {20, 0};
        ssize_t len;

        if (count_text(result->output, text) >= minimum_count) {
            return 1;
        }
        FD_ZERO(&reads);
        FD_SET(master_fd, &reads);
        if (select(master_fd + 1, &reads, NULL, NULL, &timeout) <= 0) {
            fprintf(stderr,
                    "[interactive_wait_for] timed out waiting for %zu x "
                    "%s%s%s; %zu bytes received so far:\n%.*s\n--- end ---\n",
                    minimum_count, "\"", text, "\"", result->output_len,
                    (int)result->output_len, result->output);
            fflush(stderr);
            return 0;
        }
        len = read(master_fd, result->output + result->output_len,
                   sizeof result->output - 1 - result->output_len);
        if (len <= 0) {
            fprintf(stderr,
                    "[interactive_wait_for] read() returned %zd (errno=%d) "
                    "waiting for %zu x \"%s\"; %zu bytes received so far:\n"
                    "%.*s\n--- end ---\n",
                    len, errno, minimum_count, text, result->output_len,
                    (int)result->output_len, result->output);
            fflush(stderr);
            return 0;
        }
        result->output_len += (size_t)len;
        result->output[result->output_len] = '\0';
    }
    return 0;
}

static int write_interactive(int fd, const char *data, size_t len) {
    size_t written = 0;

    while (written < len) {
        ssize_t result = write(fd, data + written, len - written);
        if (result <= 0) {
            return 0;
        }
        written += (size_t)result;
    }
    return 1;
}

static pid_t start_interactive_cli(unsigned short port, int slave_fd,
                                   const char *session_root) {
    pid_t pid = fork();

    CHECK(pid >= 0);
    if (pid == 0) {
        char port_text[16];
        char *argv[13];

        /* A plain fork() inherits the test harness's own session/process
         * group, not one attached to this pty -- a resize test's
         * TIOCSWINSZ-triggered SIGWINCH has nowhere to go without a real
         * controlling terminal established here first. */
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            _exit(126);
        }
        snprintf(port_text, sizeof port_text, "%u", (unsigned)port);
        if (dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }
        argv[0] = (char *)OI_BIN;
        argv[1] = (char *)"--host";
        argv[2] = (char *)"127.0.0.1";
        argv[3] = (char *)"--port";
        argv[4] = port_text;
        argv[5] = (char *)"--no-tls";
        argv[6] = (char *)"--api-key";
        argv[7] = (char *)"test-key";
        argv[8] = (char *)"--deny-tools";
        argv[9] = (char *)"--session-dir";
        argv[10] = (char *)session_root;
        argv[11] = NULL;
        execv(OI_BIN, argv);
        _exit(127);
    }
    return pid;
}

/* Same as start_interactive_cli, but allows tools to run without prompting
 * -- needed for tests that cancel a genuinely-running tool subprocess,
 * where --deny-tools would never let one start at all. */
static pid_t start_interactive_cli_allowing_tools(unsigned short port,
                                                  int slave_fd,
                                                  const char *session_root) {
    pid_t pid = fork();

    CHECK(pid >= 0);
    if (pid == 0) {
        char port_text[16];
        char *argv[13];

        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            _exit(126);
        }
        snprintf(port_text, sizeof port_text, "%u", (unsigned)port);
        if (dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        if (slave_fd > STDERR_FILENO) {
            close(slave_fd);
        }
        argv[0] = (char *)OI_BIN;
        argv[1] = (char *)"--host";
        argv[2] = (char *)"127.0.0.1";
        argv[3] = (char *)"--port";
        argv[4] = port_text;
        argv[5] = (char *)"--no-tls";
        argv[6] = (char *)"--api-key";
        argv[7] = (char *)"test-key";
        argv[8] = (char *)"--allow-tools";
        argv[9] = (char *)"--session-dir";
        argv[10] = (char *)session_root;
        argv[11] = NULL;
        execv(OI_BIN, argv);
        _exit(127);
    }
    return pid;
}

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

TEST(overlong_session_path_fails_cleanly) {
    char *long_dir = malloc(5000);
    CHECK(long_dir != NULL);
    memset(long_dir, 'x', 4999);
    long_dir[4999] = '\0';
    char *argv[] = {(char *)OI_BIN,
                     (char *)"--api-key",
                     (char *)"test-key",
                     (char *)"--session-dir",
                     long_dir,
                     (char *)"--session",
                     (char *)"test",
                     (char *)"hello",
                     NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 1);
    CHECK(strstr(r.output, "path is too long") != NULL);
    free(long_dir);
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
    char session_dir[] = "/tmp/oi-cli-ephemeral-XXXXXX";
    CHECK(mkdtemp(session_dir) != NULL);
    char log_path[128];
    snprintf(log_path, sizeof log_path, "%s/default.oilog", session_dir);
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
                     (char *)"say hi",
                     NULL};
    struct run_result r;
    run_cli(argv, &r);

    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "Hello, CLI") != NULL);
    CHECK(access(log_path, F_OK) != 0);

    waitpid(child, NULL, 0);
    rmdir(session_dir);
}

static void run_tool_cli(const char *first_sse, const char *second_sse,
                         const char *policy_flag, const char *max_turns,
                         const char *session_suffix,
                         struct run_result *out) {
    size_t first_len;
    char *first = build_chunked_response(first_sse, strlen(first_sse),
                                          "HTTP/1.1 200 OK", &first_len);
    size_t second_len = 0;
    char *second = NULL;
    if (second_sse != NULL) {
        second = build_chunked_response(second_sse, strlen(second_sse),
                                         "HTTP/1.1 200 OK", &second_len);
    }
    const char *responses[] = {first, second};
    size_t lengths[] = {first_len, second_len};
    unsigned short port;
    pid_t child = start_mock_server_turns(
        responses, lengths, second_sse != NULL ? 2 : 1, &port);
    free(first);
    free(second);

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    char session_name[96];
    snprintf(session_name, sizeof session_name, "oi-cli-tool-%s-%d",
             session_suffix, (int)getpid());
    char log_path[160];
    snprintf(log_path, sizeof log_path, "/tmp/%s.oilog", session_name);
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
                     (char *)"/tmp",
                     (char *)"--session",
                     session_name,
                     (char *)"--max-turns",
                     (char *)max_turns,
                     (char *)policy_flag,
                     (char *)"use a tool",
                     NULL};
    run_cli(argv, out);
    waitpid(child, NULL, 0);
    unlink(log_path);
}

TEST(tool_loop_executes_and_returns_result) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"printf "
        "tool-ok\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"tool completed\"}}]}\n\n"
        "data: [DONE]\n\n";
    struct run_result result;
    run_tool_cli(tool_sse, answer_sse, "--allow-tools", "4", "success",
                 &result);
    if (result.exit_code != 0) {
        fprintf(stderr, "tool success CLI output: %s\n", result.output);
    }
    CHECK_EQ(result.exit_code, 0);
    CHECK(strstr(result.output, "running tool shell") != NULL);
    CHECK(strstr(result.output, "tool completed") != NULL);
}

TEST(tool_denial_stops_loop) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_2\",\"type\":\"function\",\"function\":{"
        "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"true\\\"}\""
        "}}]}}]}\n\n"
        "data: [DONE]\n\n";
    struct run_result result;
    run_tool_cli(tool_sse, NULL, "--deny-tools", "4", "denied", &result);
    CHECK_EQ(result.exit_code, 1);
    CHECK(strstr(result.output, "denied") != NULL);
}

TEST(tool_failure_is_returned_to_model) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_3\",\"type\":\"function\",\"function\":{"
        "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"exit 7\\\"}"
        "\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"observed failure\"}}]}\n\n"
        "data: [DONE]\n\n";
    struct run_result result;
    run_tool_cli(tool_sse, answer_sse, "--allow-tools", "4", "failure",
                 &result);
    if (result.exit_code != 0) {
        fprintf(stderr, "tool failure CLI output: %s\n", result.output);
    }
    CHECK_EQ(result.exit_code, 0);
    CHECK(strstr(result.output, "observed failure") != NULL);
}

TEST(tool_turn_limit_is_enforced) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_4\",\"type\":\"function\",\"function\":{"
        "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"true\\\"}\""
        "}}]}}]}\n\n"
        "data: [DONE]\n\n";
    struct run_result result;
    run_tool_cli(tool_sse, NULL, "--allow-tools", "1", "limit", &result);
    CHECK_EQ(result.exit_code, 1);
    CHECK(strstr(result.output, "exceeded 1 turns") != NULL);
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
    char capture_path[160];
    snprintf(capture_path, sizeof capture_path,
             "/tmp/oi-cli-resume-request-%d", (int)getpid());
    unlink(capture_path);
    const char *responses2[] = {response2};
    size_t response_lengths2[] = {total2};
    pid_t child2 = start_mock_server_turns_capture(
        responses2, response_lengths2, 1, &port2, capture_path);
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
    CHECK(strstr(r2.output, "[resumed") == NULL);
    CHECK(strstr(r2.output, "second-reply") != NULL);

    waitpid(child2, NULL, 0);
    FILE *capture = fopen(capture_path, "r");
    CHECK(capture != NULL);
    char request[8192];
    size_t request_len =
        capture == NULL ? 0 : fread(request, 1, sizeof request - 1, capture);
    if (capture != NULL) {
        fclose(capture);
    }
    request[request_len] = '\0';
    CHECK(strstr(request, "first prompt") != NULL);
    CHECK(strstr(request, "first-reply") != NULL);
    CHECK(strstr(request, "second prompt") != NULL);
    unlink(capture_path);
    unlink(log_path);
}

TEST(interactive_repl_preserves_context_across_prompts) {
    const char *first_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"first-reply\"}}]}\n\n"
        "data: [DONE]\n\n";
    const char *second_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"second-reply\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t first_len;
    size_t second_len;
    char *first = build_chunked_response(first_sse, strlen(first_sse),
                                         "HTTP/1.1 200 OK", &first_len);
    char *second = build_chunked_response(second_sse, strlen(second_sse),
                                          "HTTP/1.1 200 OK", &second_len);
    const char *responses[] = {first, second};
    size_t lengths[] = {first_len, second_len};
    char capture_path[160];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    snprintf(capture_path, sizeof capture_path,
             "/tmp/oi-cli-repl-request-%d", (int)getpid());
    unlink(capture_path);
    server = start_mock_server_turns_capture(responses, lengths, 2, &port,
                                             capture_path);
    free(first);
    free(second);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-repl-sessions-%d", (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "first prompt\r", 13));
    CHECK(interactive_wait_for(master_fd, &result, "first-reply", 1));
    CHECK(write_interactive(master_fd, "second prompt\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "second-reply", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code =
            WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);

    {
        FILE *capture = fopen(capture_path, "r");
        char requests[16384];
        size_t request_len =
            capture == NULL
                ? 0
                : fread(requests, 1, sizeof requests - 1, capture);
        CHECK(capture != NULL);
        if (capture != NULL) {
            fclose(capture);
        }
        requests[request_len] = '\0';
        CHECK(count_text(requests, "first prompt") >= 2);
        CHECK(strstr(requests, "first-reply") != NULL);
        CHECK(strstr(requests, "second prompt") != NULL);
    }
    unlink(capture_path);
    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char history_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s",
                     session_root, entry->d_name);
            snprintf(history_path, sizeof history_path,
                     "%s/history.oilog", session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        CHECK(access(history_path, F_OK) == 0);
        unlink(history_path);
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(interactive_model_command_changes_the_live_model) {
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"changed-reply\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                         "HTTP/1.1 200 OK", &reply_len);
    const char *responses[] = {reply};
    size_t lengths[] = {reply_len};
    char capture_path[160];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    snprintf(capture_path, sizeof capture_path,
             "/tmp/oi-cli-model-request-%d", (int)getpid());
    unlink(capture_path);
    server = start_mock_server_turns_capture(responses, lengths, 1, &port,
                                             capture_path);
    free(reply);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-model-sessions-%d", (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "/model changed-model\r", 21));
    CHECK(interactive_wait_for(master_fd, &result, "Model: changed-model", 1));
    /* Raw mode now spans the whole session (oi_cli_terminal_enable's
     * tcsetattr(TCSAFLUSH) only ever runs once, at startup -- see
     * cli_composer.c), so there is no more per-command discard race here:
     * the next line can be written as soon as the confirmation is seen. */
    CHECK(write_interactive(master_fd, "a prompt\r", 9));
    CHECK(interactive_wait_for(master_fd, &result, "changed-reply", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);

    {
        FILE *capture = fopen(capture_path, "r");
        char requests[8192];
        size_t request_len =
            capture == NULL ? 0
                            : fread(requests, 1, sizeof requests - 1, capture);
        CHECK(capture != NULL);
        if (capture != NULL) {
            fclose(capture);
        }
        requests[request_len] = '\0';
        CHECK(strstr(requests, "\"model\":\"changed-model\"") != NULL);
        CHECK(strstr(requests, "gpt-4o-mini") == NULL);
    }
    unlink(capture_path);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s", session_root,
                     entry->d_name);
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        {
            FILE *metadata_file = fopen(metadata_path, "r");
            char metadata[4096];
            size_t metadata_len =
                metadata_file == NULL
                    ? 0
                    : fread(metadata, 1, sizeof metadata - 1, metadata_file);
            CHECK(metadata_file != NULL);
            if (metadata_file != NULL) {
                fclose(metadata_file);
            }
            metadata[metadata_len] = '\0';
            CHECK(strstr(metadata, "\"model\":\"changed-model\"") != NULL);
            CHECK(strstr(metadata, "test-key") == NULL);
            CHECK(strstr(metadata, "api_key") == NULL);
        }
        unlink(metadata_path);
        {
            char history_path[640];
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(history_path);
        }
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(sigint_cancels_an_in_flight_request_and_returns_to_the_prompt) {
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recovered\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                         "HTTP/1.1 200 OK", &reply_len);
    struct slow_mock_turn turns[2];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].delay_seconds = 3;
    turns[1].response = reply;
    turns[1].response_len = reply_len;
    turns[1].delay_seconds = 0;
    server = start_slow_mock_server(turns, 2, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-sigint-req-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    /* Wait for the "\r\n" oi_cli_composer_wait_submit's cleanup
     * unconditionally emits (render_finish) right before it returns:
     * without this, a SIGINT sent immediately after write_interactive can
     * race the idle-prompt's own poll() loop, which still has signal_fd
     * registered until the read actually returns -- if both input_fd and
     * signal_fd become readable in the same poll() call, the signal
     * branch is checked first and the submission is never even read.
     * Raw mode now spans the whole session (no more per-cycle
     * tcsetattr(TCSAFLUSH)), so once this is seen there is no further
     * race and no further discard risk -- the mock server's 3s delay only
     * needs to outlast the turn's own setup, not this wait. */
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    CHECK_EQ(kill(cli, SIGINT), 0);
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    free(reply);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s",
                     session_root, entry->d_name);
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        unlink(metadata_path);
        {
            char history_path[640];
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(history_path);
        }
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(sigint_cancels_a_running_tool_and_returns_to_the_prompt) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_sigint\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":"
        "\\\"printf started; sleep 3\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recovered\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_resp =
        build_chunked_response(tool_sse, strlen(tool_sse), "HTTP/1.1 200 OK",
                              &tool_len);
    char *answer_resp = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_resp, answer_resp};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(tool_resp);
    free(answer_resp);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-sigint-tool-%d",
             (int)getpid());
    cli = start_interactive_cli_allowing_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    /* The interactive present layer only ever prints a "running tool"
     * status line, never the tool's own raw stdout -- there is no
     * terminal-visible marker for "the shell command has actually
     * produced output" to wait for. Wait for the status line, then give
     * the forked child a short, generous fixed window to actually start
     * running (fork+exec completing in a few milliseconds, in practice)
     * well before its own 3-second sleep would complete on its own. */
    CHECK(interactive_wait_for(master_fd, &result, "running tool shell", 1));
    {
        struct timespec delay = {0, 300000000L};
        nanosleep(&delay, NULL);
    }
    CHECK_EQ(kill(cli, SIGINT), 0);
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s",
                     session_root, entry->d_name);
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        unlink(metadata_path);
        {
            char history_path[640];
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(history_path);
        }
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(recoverable_turn_error_returns_to_the_prompt) {
    /* --deny-tools rejects this tool call, producing OI_ERR_DENIED -- a
     * genuine turn failure, but not a durable-storage failure, so the REPL
     * must print a message and return to a working prompt rather than
     * exit. */
    const char *denied_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_denied\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":"
        "\\\"true\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recovered\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t denied_len;
    size_t answer_len;
    char *denied_resp = build_chunked_response(
        denied_sse, strlen(denied_sse), "HTTP/1.1 200 OK", &denied_len);
    char *answer_resp = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {denied_resp, answer_resp};
    size_t lengths[] = {denied_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(denied_resp);
    free(answer_resp);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-recoverable-error-%d", (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "oi: turn failed: denied",
                              1));
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s",
                     session_root, entry->d_name);
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        unlink(metadata_path);
        {
            char history_path[640];
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(history_path);
        }
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(ctrl_d_during_a_turn_has_no_effect) {
    /* Raw mode now spans the whole session, and input_fd is read live
     * during a turn (see handle_turn_input in cli_repl.c), so Ctrl+D is
     * actually decoded here, not just sitting unread -- but with the
     * editor empty, it classifies as an EXIT action, and EXIT (like
     * SUBMIT) is a deliberate no-op while busy: there is no queue or
     * "exit at the next safe boundary" logic wired up yet to act on it.
     * The turn completes normally and the REPL is left exactly as if
     * Ctrl+D had never been sent. */
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recovered\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                         "HTTP/1.1 200 OK", &reply_len);
    struct slow_mock_turn turns[1];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].delay_seconds = 2;
    server = start_slow_mock_server(turns, 1, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-ctrl-d-turn-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    /* "\r\n" (render_finish, emitted right before oi_cli_composer_wait_submit
     * returns) proves the turn has actually started before sending Ctrl+D
     * mid-turn, matching the same reasoning used for SIGINT above. */
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));
    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    free(reply);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s",
                     session_root, entry->d_name);
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        unlink(metadata_path);
        {
            char history_path[640];
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(history_path);
        }
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(typing_during_a_turn_does_not_corrupt_the_display) {
    /* The response is written in two pieces with a real pause between
     * them (not just a delay before the first byte), so the client is
     * still assembling a partial assistant delta -- with its own composer
     * frame erased/redrawn around every reactor step in between (see
     * cli_repl.c) -- when the second piece and further keystrokes both
     * land in the same window. */
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recov\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"ered\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                         "HTTP/1.1 200 OK", &reply_len);
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    /* Split partway through the SSE body (well past the HTTP header and
     * first delta), so the first write already reached the client as a
     * real in-flight, partially-received response. */
    server = start_split_response_mock_server(reply, reply_len, reply_len / 2,
                                              300, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-typing-turn-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    /* Type while the turn is genuinely in flight (mid-response, per the
     * server-side pause above). Enter is deliberately not sent -- SUBMIT
     * while busy is still a documented no-op as of this commit (the
     * one-slot queue lands in a later commit), so this only exercises
     * that the keystrokes are decoded and redrawn without corrupting the
     * turn's own streamed output. */
    CHECK(write_interactive(master_fd, "world", 5));
    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    /* The typed text is echoed as an uninterrupted run: if the composer's
     * erase/redraw around each reactor step failed to coordinate with the
     * turn's own streamed writes, the two would interleave and this exact
     * substring would not appear intact. */
    CHECK(strstr(result.output, "world") != NULL);
    /* Nothing submitted it (no Enter), so it carries over as the next
     * prompt's draft exactly as typed -- clear it with Ctrl+C before
     * exiting cleanly. */
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
    free(reply);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s",
                     session_root, entry->d_name);
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        unlink(metadata_path);
        {
            char history_path[640];
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(history_path);
        }
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(typed_ctrl_c_during_a_turn_cancels_it) {
    /* Unlike the existing sigint_cancels_* tests (an external kill(SIGINT)
     * on the process), this sends the raw Ctrl+C byte (0x03) through the
     * pty as a real keystroke, exercising the composer's own decoded-
     * CTRL_C path in handle_turn_input rather than the signalfd path. */
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recovered\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                         "HTTP/1.1 200 OK", &reply_len);
    struct slow_mock_turn turns[2];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].delay_seconds = 3;
    turns[1].response = reply;
    turns[1].response_len = reply_len;
    turns[1].delay_seconds = 0;
    server = start_slow_mock_server(turns, 2, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-typed-ctrl-c-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    free(reply);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s",
                     session_root, entry->d_name);
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        unlink(metadata_path);
        {
            char history_path[640];
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(history_path);
        }
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(sigterm_during_a_turn_terminates_cleanly) {
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recovered\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                         "HTTP/1.1 200 OK", &reply_len);
    struct slow_mock_turn turns[1];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    /* A generous delay: the turn only needs to still be in flight by the
     * time SIGTERM is sent below, not for the full delay to elapse -- the
     * mock server is killed once the test is done with it regardless. */
    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].delay_seconds = 3;
    server = start_slow_mock_server(turns, 1, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-sigterm-turn-%d", (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    /* Wait for "\r\n" (render_finish) before signalling, matching the
     * SIGINT tests above: it proves oi_cli_composer_wait_submit has
     * already returned and cli_repl.c's own turn setup (including
     * registering signal_fd on the reactor) has already run, so the
     * signal is guaranteed to be handled by the turn-time reactor
     * callback rather than raced against the idle-prompt's own signal
     * draining. */
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    CHECK_EQ(kill(cli, SIGTERM), 0);

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    free(reply);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        size_t sessions_found = 0;

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            sessions_found++;
            snprintf(session_path, sizeof session_path, "%s/%s",
                     session_root, entry->d_name);
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);
        unlink(metadata_path);
        {
            char history_path[640];
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(history_path);
        }
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(interactive_cwd_command_changes_the_process_directory) {
    char target_dir[160];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];
    char cwd_command[256];

    snprintf(target_dir, sizeof target_dir, "/tmp/oi-cli-cwd-target-%d",
             (int)getpid());
    CHECK_EQ(mkdir(target_dir, 0700), 0);
    /* /cwd and /status never submit a model message, so this server is
     * never actually queried -- a harmless placeholder response. */
    server = start_mock_server("data: [DONE]\n\n", 14, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-cwd-sessions-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    snprintf(cwd_command, sizeof cwd_command, "/cwd %s\r", target_dir);
    CHECK(write_interactive(master_fd, cwd_command, strlen(cwd_command)));
    CHECK(interactive_wait_for(master_fd, &result, "CWD:", 1));
    /* Raw mode now spans the whole session (only one tcsetattr(TCSAFLUSH)
     * call ever, at startup), so there is no more per-command discard
     * race here: the next line can be written as soon as the confirmation
     * is seen. */
    CHECK(write_interactive(master_fd, "/status\r", 8));
    {
        char expected[192];
        snprintf(expected, sizeof expected, "CWD: %s", target_dir);
        /* The /cwd confirmation above already printed this exact text once;
         * /status's own CWD line is the second occurrence. Waiting for
         * count 1 here would be satisfied instantly without draining the
         * PTY, leaving /status's real response unread and deadlocking the
         * child once its output fills the PTY buffer. */
        CHECK(interactive_wait_for(master_fd, &result, expected, 2));
    }
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);

    /* No session was ever created: /cwd alone doesn't submit a model
     * message, matching interactive_exit_before_submission_creates_no_session. */
    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        size_t sessions_found = 0;

        if (directory != NULL) {
            while ((entry = readdir(directory)) != NULL) {
                if (strcmp(entry->d_name, ".") != 0 &&
                    strcmp(entry->d_name, "..") != 0) {
                    sessions_found++;
                }
            }
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 0);
    }
    rmdir(session_root);
    rmdir(target_dir);
}

TEST(model_override_persists_across_a_restart) {
    char session_dir[] = "/tmp";
    char session_name[64];
    char log_path[128];
    const char *sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"a-reply\"}}]}\n\ndata: [DONE]\n\n";

    snprintf(session_name, sizeof session_name, "oi-cli-model-restart-%d",
             (int)getpid());
    snprintf(log_path, sizeof log_path, "%s/%s.oilog", session_dir,
             session_name);
    unlink(log_path);

    /* Run 1: create the session with the default model. */
    {
        size_t total;
        char *response =
            build_chunked_response(sse, strlen(sse), "HTTP/1.1 200 OK",
                                   &total);
        unsigned short port;
        pid_t child = start_mock_server(response, total, &port);
        char port_str[16];
        struct run_result r;
        free(response);
        snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
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
                        (char *)"first prompt",
                        NULL};
        run_cli(argv, &r);
        CHECK_EQ(r.exit_code, 0);
        waitpid(child, NULL, 0);
    }

    /* Run 2: pass --model explicitly; the override must win and persist. */
    {
        size_t total;
        char *response =
            build_chunked_response(sse, strlen(sse), "HTTP/1.1 200 OK",
                                   &total);
        unsigned short port;
        pid_t child = start_mock_server(response, total, &port);
        char port_str[16];
        struct run_result r;
        free(response);
        snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
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
                        (char *)"--model",
                        (char *)"overridden-model",
                        (char *)"second prompt",
                        NULL};
        run_cli(argv, &r);
        CHECK_EQ(r.exit_code, 0);
        waitpid(child, NULL, 0);
    }

    /* Run 3: no --model given; the overridden value must still apply. */
    {
        size_t total;
        char *response =
            build_chunked_response(sse, strlen(sse), "HTTP/1.1 200 OK",
                                   &total);
        unsigned short port;
        char capture_path[160];
        const char *responses[] = {response};
        size_t lengths[] = {total};
        pid_t child;
        char port_str[16];
        struct run_result r;

        snprintf(capture_path, sizeof capture_path,
                 "/tmp/oi-cli-model-restart-request-%d", (int)getpid());
        unlink(capture_path);
        child = start_mock_server_turns_capture(responses, lengths, 1, &port,
                                                capture_path);
        free(response);
        snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
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
                        (char *)"third prompt",
                        NULL};
        run_cli(argv, &r);
        CHECK_EQ(r.exit_code, 0);
        waitpid(child, NULL, 0);

        {
            FILE *capture = fopen(capture_path, "r");
            char request[8192];
            size_t request_len =
                capture == NULL
                    ? 0
                    : fread(request, 1, sizeof request - 1, capture);
            CHECK(capture != NULL);
            if (capture != NULL) {
                fclose(capture);
            }
            request[request_len] = '\0';
            CHECK(strstr(request, "\"model\":\"overridden-model\"") != NULL);
        }
        unlink(capture_path);
    }

    unlink(log_path);
    {
        char metadata_path[160];
        snprintf(metadata_path, sizeof metadata_path, "%s/%s.metadata.json",
                session_dir, session_name);
        unlink(metadata_path);
    }
}

TEST(resize_redraws_the_live_prompt_at_the_new_width) {
    int master_fd = -1;
    int slave_fd = -1;
    pid_t cli;
    struct interactive_result result;
    char session_root[128];
    struct winsize ws;
    size_t clears_before_resize;

    memset(&ws, 0, sizeof ws);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-resize-%d",
             (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    ws.ws_row = 24;
    ws.ws_col = 30;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &ws), 0);
    memset(&result, 0, sizeof result);
    /* Never submits a prompt, so the mock-server port is never actually
     * connected to -- matching interactive_exit_before_submission_creates_
     * no_session's use of an arbitrary unused port below. */
    cli = start_interactive_cli(9, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello", 5));
    CHECK(interactive_wait_for(master_fd, &result, "hello", 1));

    /* Resize mid-edit, without any further keystroke: a fresh "\x1b[J"
     * clear sequence appearing proves the redraw was driven by the
     * SIGWINCH/signalfd path added in cli_repl.c, not by continued
     * typing. */
    clears_before_resize = count_text(result.output, "\x1b[J");
    ws.ws_col = 10;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &ws), 0);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[J",
                               clears_before_resize + 1));

    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    CHECK(access(session_root, F_OK) != 0);
    close(master_fd);
}

TEST(interactive_exit_before_submission_creates_no_session) {
    int master_fd = -1;
    int slave_fd = -1;
    pid_t cli;
    struct interactive_result result;
    char session_root[128];

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-lazy-session-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(9, slave_fd, session_root);
    close(slave_fd);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 0);
    }
    CHECK(access(session_root, F_OK) != 0);
    close(master_fd);
}

TEST(interactive_help_and_exit_are_dispatched_without_a_session) {
    int master_fd = -1;
    int slave_fd = -1;
    pid_t cli;
    struct interactive_result result;
    char session_root[128];

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-command-session-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(9, slave_fd, session_root);
    close(slave_fd);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "/help\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "Commands:", 1));
    CHECK(write_interactive(master_fd, "/exit\r", 6));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 0);
    }
    CHECK(strstr(result.output, "/permissions") != NULL);
    CHECK(access(session_root, F_OK) != 0);
    close(master_fd);
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
    RUN(overlong_session_path_fails_cleanly);
    RUN(end_to_end_streaming_reply);
    RUN(tool_loop_executes_and_returns_result);
    RUN(tool_denial_stops_loop);
    RUN(tool_failure_is_returned_to_model);
    RUN(tool_turn_limit_is_enforced);
    RUN(resume_replays_prior_exchange);
    RUN(interactive_repl_preserves_context_across_prompts);
    RUN(interactive_model_command_changes_the_live_model);
    RUN(sigint_cancels_an_in_flight_request_and_returns_to_the_prompt);
    RUN(sigint_cancels_a_running_tool_and_returns_to_the_prompt);
    RUN(recoverable_turn_error_returns_to_the_prompt);
    RUN(sigterm_during_a_turn_terminates_cleanly);
    RUN(ctrl_d_during_a_turn_has_no_effect);
    RUN(typing_during_a_turn_does_not_corrupt_the_display);
    RUN(typed_ctrl_c_during_a_turn_cancels_it);
    RUN(interactive_cwd_command_changes_the_process_directory);
    RUN(model_override_persists_across_a_restart);
    RUN(resize_redraws_the_live_prompt_at_the_new_width);
    RUN(interactive_exit_before_submission_creates_no_session);
    RUN(interactive_help_and_exit_are_dispatched_without_a_session);
    return oi_test_report();
}
