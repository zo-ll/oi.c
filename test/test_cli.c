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
#include <stdint.h>
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

/* Same as start_interactive_cli, but with neither --allow-tools nor
 * --deny-tools: the default `ask` policy, which now (issue #26) routes
 * through the REPL's own permission selector instead of blocking on
 * /dev/tty -- needed for tests exercising that selector directly. */
static pid_t start_interactive_cli_asking_tools(unsigned short port,
                                                int slave_fd,
                                                const char *session_root) {
    pid_t pid = fork();

    CHECK(pid >= 0);
    if (pid == 0) {
        char port_text[16];
        char *argv[12];

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
        argv[8] = (char *)"--session-dir";
        argv[9] = (char *)session_root;
        argv[10] = NULL;
        execv(OI_BIN, argv);
        _exit(127);
    }
    return pid;
}

/* Same as start_interactive_cli, but against an explicit --session ID
 * rather than a fresh automatic session directory -- needed to actually
 * resume the same durable log across two separate process lifetimes (an
 * automatic session's directory is always brand new, so it can never be
 * resumed this way). */
static pid_t start_interactive_cli_with_session(unsigned short port,
                                                int slave_fd,
                                                const char *session_dir,
                                                const char *session_name) {
    pid_t pid = fork();

    CHECK(pid >= 0);
    if (pid == 0) {
        char port_text[16];
        char *argv[14];

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
        argv[10] = (char *)session_dir;
        argv[11] = (char *)"--session";
        argv[12] = (char *)session_name;
        argv[13] = NULL;
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

TEST(default_ask_policy_denies_safely_without_a_controlling_terminal) {
    /* The one-shot loop is only ever reached when stdin/stdout aren't
     * both a tty (cli.c's own `interactive` gate) -- exactly the case
     * this test exercises via run_cli's plain pipe/fork setup, with no
     * --allow-tools/--deny-tools flag at all, so the default policy is
     * `ask`. It must deny promptly rather than hang forever waiting for
     * a decision nothing can ever supply. */
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_ask\",\"type\":\"function\",\"function\":{"
        "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"true\\\"}\""
        "}}]}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    char *tool_response = build_chunked_response(
        tool_sse, strlen(tool_sse), "HTTP/1.1 200 OK", &tool_len);
    unsigned short port;
    pid_t server = start_mock_server(tool_response, tool_len, &port);
    free(tool_response);

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    char session_name[96];
    snprintf(session_name, sizeof session_name, "oi-cli-tool-ask-noninteractive-%d",
             (int)getpid());
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
                    (char *)"4",
                    (char *)"use a tool",
                    NULL};
    struct run_result result;
    run_cli(argv, &result);
    waitpid(server, NULL, 0);
    unlink(log_path);

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

TEST(busy_submit_is_queued_and_a_second_one_is_refused) {
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
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-queue-slot-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    /* First submission while busy: accepted into the one pending slot. */
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    /* /help and /status are read-only: they dispatch immediately
     * regardless of the occupied slot, and don't themselves get queued.
     * Checked before the refused submission below, since a refusal
     * deliberately leaves its draft uncommitted -- typing more after it
     * would land in the same still-open draft rather than starting fresh. */
    CHECK(write_interactive(master_fd, "/help\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "Commands:", 1));
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Model:", 1));

    /* Second submission while the slot is still occupied: refused, not
     * silently dropped and not overwriting the first. */
    CHECK(write_interactive(master_fd, "another\r", 8));
    CHECK(interactive_wait_for(
        master_fd, &result, "oi: a message is already queued", 1));

    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    /* "another" is still sitting in the draft (the refusal above left it
     * uncommitted, and busy/idle share the same editor state) -- clear it
     * before Ctrl+D, which otherwise deletes forward instead of exiting
     * when the draft isn't empty. The turn has already finished by this
     * point (recovered arrived), so Ctrl+C here only clears the draft,
     * it has no turn left to cancel. */
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
            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);

        /* The accepted message ("world") was durably persisted as a
         * QUEUED_INPUT record; the refused one ("another") never reached
         * persist_queued_input at all. */
        {
            FILE *history = fopen(history_path, "r");
            unsigned char contents[8192];
            size_t len = 0;
            size_t offset;
            int found_queued_input_world = 0;
            int found_another = 0;

            CHECK(history != NULL);
            len = fread(contents, 1, sizeof contents, history);
            fclose(history);
            /* oi_sesslog's own file format is a 12-byte binary header
             * ("OISESLOG" + a little-endian u32 version) followed by a
             * sequence of [4-byte little-endian length][JSON bytes]
             * records -- both the header and each record's own length
             * prefix contain embedded NUL/non-ASCII bytes, so treating the
             * whole file as one C string (strstr from the start) stops
             * almost immediately. Walk the length-prefixed records
             * individually instead, each of which *is* a plain, NUL-free
             * JSON string safe to strstr on its own. */
            CHECK(len > 12);
            offset = 12;
            while (offset + 4 <= len) {
                uint32_t record_len = (uint32_t)(unsigned char)contents[offset] |
                    ((uint32_t)(unsigned char)contents[offset + 1] << 8) |
                    ((uint32_t)(unsigned char)contents[offset + 2] << 16) |
                    ((uint32_t)(unsigned char)contents[offset + 3] << 24);
                char record[1024];

                offset += 4;
                CHECK(offset + record_len <= len);
                CHECK(record_len < sizeof record);
                memcpy(record, contents + offset, record_len);
                record[record_len] = '\0';
                if (strstr(record, "\"type\":\"queued_input\"") != NULL &&
                    strstr(record, "\"content\":\"world\"") != NULL) {
                    found_queued_input_world = 1;
                }
                if (strstr(record, "\"content\":\"another\"") != NULL) {
                    found_another = 1;
                }
                offset += record_len;
            }
            CHECK(found_queued_input_world);
            CHECK(!found_another);
        }

        unlink(metadata_path);
        unlink(history_path);
        rmdir(session_path);
        rmdir(session_root);
    }
}

/*
 * Loads oi_sesslog's length-prefixed records (see the file-format comment
 * in busy_submit_is_queued_and_a_second_one_is_refused above) as an array
 * of NUL-terminated JSON strings, one per record -- for tests that need to
 * check more than one record's presence, or the relative order/turn_ids
 * between several of them.
 */
struct oilog_records {
    char **items;
    size_t count;
};

static void oilog_records_load(const char *path, struct oilog_records *out) {
    FILE *history = fopen(path, "r");
    long size;
    unsigned char *contents;
    size_t len;
    size_t offset;
    size_t cap = 16;

    CHECK(history != NULL);
    CHECK_EQ(fseek(history, 0, SEEK_END), 0);
    size = ftell(history);
    CHECK(size > 12);
    rewind(history);
    contents = malloc((size_t)size);
    CHECK(contents != NULL);
    len = fread(contents, 1, (size_t)size, history);
    fclose(history);
    CHECK_EQ(len, (size_t)size);

    out->items = malloc(cap * sizeof(char *));
    CHECK(out->items != NULL);
    out->count = 0;
    offset = 12;
    while (offset + 4 <= len) {
        uint32_t record_len = (uint32_t)contents[offset] |
            ((uint32_t)contents[offset + 1] << 8) |
            ((uint32_t)contents[offset + 2] << 16) |
            ((uint32_t)contents[offset + 3] << 24);
        char *record;

        offset += 4;
        CHECK(offset + record_len <= len);
        record = malloc(record_len + 1);
        CHECK(record != NULL);
        memcpy(record, contents + offset, record_len);
        record[record_len] = '\0';
        offset += record_len;

        if (out->count == cap) {
            cap *= 2;
            out->items = realloc(out->items, cap * sizeof(char *));
            CHECK(out->items != NULL);
        }
        out->items[out->count++] = record;
    }
    free(contents);
}

static void oilog_records_free(struct oilog_records *records) {
    size_t i;

    for (i = 0; i < records->count; i++) {
        free(records->items[i]);
    }
    free(records->items);
}

/* Finds the first record at or after start_index containing every needle;
 * returns its index, or SIZE_MAX if none match. */
static size_t oilog_find(const struct oilog_records *records,
                         size_t start_index, const char *const *needles,
                         size_t needle_count) {
    size_t i;

    for (i = start_index; i < records->count; i++) {
        size_t j;

        for (j = 0; j < needle_count; j++) {
            if (strstr(records->items[i], needles[j]) == NULL) {
                break;
            }
        }
        if (j == needle_count) {
            return i;
        }
    }
    return SIZE_MAX;
}

/* Every history record's turn_id is written as a JSON string, e.g.
 * "turn_id":"2" -- extracts its numeric value. */
static uint64_t oilog_record_turn_id(const char *record) {
    const char *marker = strstr(record, "\"turn_id\":\"");

    CHECK(marker != NULL);
    return (uint64_t)strtoull(marker + strlen("\"turn_id\":\""), NULL, 10);
}

/* Counts every record containing all of the given needles -- used where
 * oilog_find's "first match" isn't enough, e.g. asserting a crash-recovery
 * pass ran exactly once, not zero or twice. */
static size_t oilog_count(const struct oilog_records *records,
                          const char *const *needles, size_t needle_count) {
    size_t count = 0;
    size_t i;

    for (i = 0; i < records->count; i++) {
        size_t j;

        for (j = 0; j < needle_count; j++) {
            if (strstr(records->items[i], needles[j]) == NULL) {
                break;
            }
        }
        if (j == needle_count) {
            count++;
        }
    }
    return count;
}

TEST(queued_message_resumes_at_the_safe_boundary_with_correct_turn_ids) {
    const char *first_reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recovered\"}}]}\n\n"
        "data: [DONE]\n\n";
    const char *second_reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"secondreply\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t first_len;
    size_t second_len;
    char *first_reply = build_chunked_response(
        first_reply_sse, strlen(first_reply_sse), "HTTP/1.1 200 OK",
        &first_len);
    char *second_reply = build_chunked_response(
        second_reply_sse, strlen(second_reply_sse), "HTTP/1.1 200 OK",
        &second_len);
    struct slow_mock_turn turns[2];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    /* The first turn's 2s delay keeps it reliably busy long enough to
     * submit "world" while it's still in flight; the second turn (no
     * delay) is the queued message's own resumed turn, run automatically
     * once the first reaches its safe boundary. */
    turns[0].response = first_reply;
    turns[0].response_len = first_len;
    turns[0].delay_seconds = 2;
    turns[1].response = second_reply;
    turns[1].response_len = second_len;
    turns[1].delay_seconds = 0;
    server = start_slow_mock_server(turns, 2, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-queue-resume-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    /* The queued message runs immediately once the first turn finishes,
     * with no further input from us. */
    CHECK(interactive_wait_for(master_fd, &result, "secondreply", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
    free(first_reply);
    free(second_reply);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};
        char metadata_path[640] = {0};
        char history_path[640] = {0};
        size_t sessions_found = 0;
        struct oilog_records records;
        size_t hello_index;
        size_t queued_index;
        size_t resolved_index;
        size_t resumed_index;
        uint64_t hello_turn_id;
        uint64_t queued_turn_id;
        const char *hello_needles[] = {"\"type\":\"message\"",
                                       "\"role\":\"user\"",
                                       "\"content\":\"hello\""};
        const char *queued_needles[] = {"\"type\":\"queued_input\"",
                                        "\"content\":\"world\""};
        const char *resolved_needles[] = {"\"type\":\"queue_resolved\"",
                                          "\"resolution\":\"consumed\""};
        const char *resumed_needles[] = {"\"type\":\"message\"",
                                         "\"role\":\"user\"",
                                         "\"content\":\"world\""};

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
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);

        oilog_records_load(history_path, &records);

        hello_index = oilog_find(&records, 0, hello_needles, 3);
        CHECK(hello_index != SIZE_MAX);
        queued_index =
            oilog_find(&records, hello_index + 1, queued_needles, 2);
        CHECK(queued_index != SIZE_MAX);
        resolved_index =
            oilog_find(&records, queued_index + 1, resolved_needles, 2);
        CHECK(resolved_index != SIZE_MAX);
        resumed_index =
            oilog_find(&records, resolved_index + 1, resumed_needles, 3);
        CHECK(resumed_index != SIZE_MAX);

        /* The core correctness property: the queued item, the record that
         * resolves it, and the turn it becomes must all share one turn_id,
         * distinct from (and later than) the first turn's. */
        hello_turn_id = oilog_record_turn_id(records.items[hello_index]);
        queued_turn_id = oilog_record_turn_id(records.items[queued_index]);
        CHECK(queued_turn_id > hello_turn_id);
        CHECK_EQ(oilog_record_turn_id(records.items[resolved_index]),
                 queued_turn_id);
        CHECK_EQ(oilog_record_turn_id(records.items[resumed_index]),
                 queued_turn_id);

        oilog_records_free(&records);

        unlink(metadata_path);
        unlink(history_path);
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(queued_command_while_busy_resolves_discarded_and_dispatches_live) {
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
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-queue-cmd-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    /* /model is not read-only (unlike /help and /status): it queues like
     * any plain message while busy, per the schema-forced rule that a
     * queued command can only ever resolve DISCARDED, then dispatch live. */
    CHECK(write_interactive(master_fd, "/model gpt-test\r", 17));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    /* Once the turn reaches its safe boundary, the queued command
     * dispatches live -- its own confirmation text proves this ran through
     * dispatch_model, not as a new conversation turn. */
    CHECK(interactive_wait_for(master_fd, &result, "Model: gpt-test", 1));

    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Model: gpt-test", 2));

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
        char history_path[640] = {0};
        size_t sessions_found = 0;
        struct oilog_records records;
        size_t queued_index;
        size_t resolved_index;
        const char *queued_needles[] = {"\"type\":\"queued_input\"",
                                        "\"content\":\"/model gpt-test\""};
        const char *resolved_needles[] = {"\"type\":\"queue_resolved\"",
                                          "\"resolution\":\"discarded\""};

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
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);

        oilog_records_load(history_path, &records);

        queued_index = oilog_find(&records, 0, queued_needles, 2);
        CHECK(queued_index != SIZE_MAX);
        resolved_index =
            oilog_find(&records, queued_index + 1, resolved_needles, 2);
        CHECK(resolved_index != SIZE_MAX);
        CHECK_EQ(oilog_record_turn_id(records.items[queued_index]),
                 oilog_record_turn_id(records.items[resolved_index]));

        oilog_records_free(&records);

        unlink(metadata_path);
        unlink(history_path);
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(ctrl_c_with_a_pending_item_discards_it_and_restores_the_draft) {
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

    /* A generous delay: Ctrl+C only needs to land while the turn is
     * genuinely still in flight, not for the full delay to elapse. */
    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].delay_seconds = 3;
    server = start_slow_mock_server(turns, 1, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-queue-ctrlc-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    /* The composer only redraws once per reactor step, after any decoded
     * SUBMIT has already been handled -- since handle_busy_submit commits
     * and clears the draft before that redraw fires, "world" is never
     * actually echoed while being typed here (the whole "world\r" is
     * decoded and submitted within a single step). It only appears once
     * it's restored below. */
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    /* Ctrl+C cancels the turn AND discards the queued item -- restoring it
     * as a live, editable draft rather than either silently running it or
     * dropping it. */
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(interactive_wait_for(
        master_fd, &result, "oi: queued input discarded", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    /* "world" is echoed here for the only time in this whole session, as
     * the restored draft is redrawn -- proving it came back rather than
     * being dropped. */
    CHECK(interactive_wait_for(master_fd, &result, "world", 1));

    /* The restored draft is a normal, still-open line: clear it (Ctrl+C is
     * safe now, no turn left to cancel) before Ctrl+D, which otherwise
     * deletes forward instead of exiting when the draft isn't empty. */
    CHECK(write_interactive(master_fd, "\x03", 1));
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
        char history_path[640] = {0};
        size_t sessions_found = 0;
        struct oilog_records records;
        size_t queued_index;
        size_t resolved_index;
        size_t resumed_index;
        const char *queued_needles[] = {"\"type\":\"queued_input\"",
                                        "\"content\":\"world\""};
        const char *resolved_needles[] = {"\"type\":\"queue_resolved\"",
                                          "\"resolution\":\"discarded\""};
        const char *resumed_needles[] = {"\"type\":\"message\"",
                                         "\"role\":\"user\"",
                                         "\"content\":\"world\""};

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
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
        }
        if (directory != NULL) {
            closedir(directory);
        }
        CHECK_EQ(sessions_found, 1);

        oilog_records_load(history_path, &records);

        queued_index = oilog_find(&records, 0, queued_needles, 2);
        CHECK(queued_index != SIZE_MAX);
        resolved_index =
            oilog_find(&records, queued_index + 1, resolved_needles, 2);
        CHECK(resolved_index != SIZE_MAX);
        CHECK_EQ(oilog_record_turn_id(records.items[queued_index]),
                 oilog_record_turn_id(records.items[resolved_index]));

        /* The discarded item must never actually run as a real turn -- no
         * matching user message record should exist anywhere in the log. */
        resumed_index = oilog_find(&records, 0, resumed_needles, 3);
        CHECK(resumed_index == SIZE_MAX);

        oilog_records_free(&records);

        unlink(metadata_path);
        unlink(history_path);
        rmdir(session_path);
        rmdir(session_root);
    }
}

TEST(crash_with_a_pending_item_restores_it_as_a_startup_draft) {
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
    char session_dir[] = "/tmp";
    char session_name[64];
    char log_path[160];

    snprintf(session_name, sizeof session_name, "oi-cli-crash-queue-%d",
             (int)getpid());
    snprintf(log_path, sizeof log_path, "%s/%s.oilog", session_dir,
             session_name);
    unlink(log_path);

    /* First "life": queue something, then simulate a hard crash (SIGKILL,
     * no graceful shutdown at all) while it's still sitting unresolved. */
    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].delay_seconds = 5;
    server = start_slow_mock_server(turns, 1, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_with_session(port, slave_fd, session_dir,
                                             session_name);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    CHECK_EQ(kill(cli, SIGKILL), 0);
    waitpid(cli, NULL, 0);
    close(master_fd);
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    free(reply);

    /* Second "life": same session, fresh process -- the crash-recovery
     * window must be closed durably and the queued text handed back as a
     * plain, editable startup draft, never auto-run. No mock server is
     * needed: nothing here ever submits a message. */
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_with_session(1, slave_fd, session_dir,
                                             session_name);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(interactive_wait_for(master_fd, &result, "world", 1));

    /* Clear the restored draft (Ctrl+C is safe here -- no turn is active)
     * before Ctrl+D, which otherwise deletes forward rather than exiting
     * when the draft isn't empty. */
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);

    {
        struct oilog_records records;
        size_t queued_index;
        size_t resolved_index;
        const char *queued_needles[] = {"\"type\":\"queued_input\"",
                                        "\"content\":\"world\""};
        const char *resolved_needles[] = {"\"type\":\"queue_resolved\"",
                                          "\"resolution\":\"discarded\""};

        oilog_records_load(log_path, &records);
        queued_index = oilog_find(&records, 0, queued_needles, 2);
        CHECK(queued_index != SIZE_MAX);
        resolved_index =
            oilog_find(&records, queued_index + 1, resolved_needles, 2);
        CHECK(resolved_index != SIZE_MAX);
        CHECK_EQ(oilog_record_turn_id(records.items[queued_index]),
                 oilog_record_turn_id(records.items[resolved_index]));
        oilog_records_free(&records);
    }

    /* Third "life": restart again immediately. The crash window is already
     * closed (has_pending_input is now false), so this must not seed a
     * draft again or append a second resolution record -- no duplication. */
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_with_session(1, slave_fd, session_dir,
                                             session_name);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);

    {
        struct oilog_records records;
        const char *resolved_needles[] = {"\"type\":\"queue_resolved\""};

        oilog_records_load(log_path, &records);
        CHECK_EQ(oilog_count(&records, resolved_needles, 1), (size_t)1);
        oilog_records_free(&records);
    }

    unlink(log_path);
}

static const char permission_tool_sse[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
    "\"index\":0,\"id\":\"call_ask\",\"type\":\"function\",\"function\":{"
    "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"printf "
    "tool-ok\\\"}\"}}]}}]}\n\n"
    "data: [DONE]\n\n";

TEST(permission_ask_allow_once_lets_the_tool_run) {
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"finished\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_response = build_chunked_response(
        permission_tool_sse, strlen(permission_tool_sse), "HTTP/1.1 200 OK",
        &tool_len);
    char *answer_response = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_response, answer_response};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(tool_response);
    free(answer_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-ask-allow-once-%d",
             (int)getpid());
    cli = start_interactive_cli_asking_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "Tool: shell", 1));
    CHECK(interactive_wait_for(master_fd, &result, "Allow once", 1));
    CHECK(interactive_wait_for(master_fd, &result, "Deny", 1));

    /* Confirms at the default selection (index 0, "Allow once"). */
    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "finished", 1));

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

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0) {
                snprintf(session_path, sizeof session_path, "%s/%s",
                         session_root, entry->d_name);
            }
        }
        if (directory != NULL) {
            closedir(directory);
        }
        if (session_path[0] != '\0') {
            char metadata_path[640];
            char history_path[640];

            snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
                     session_path);
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(metadata_path);
            unlink(history_path);
            rmdir(session_path);
        }
    }
    rmdir(session_root);
}

TEST(permission_ask_deny_ends_the_turn) {
    size_t tool_len;
    char *tool_response = build_chunked_response(
        permission_tool_sse, strlen(permission_tool_sse), "HTTP/1.1 200 OK",
        &tool_len);
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server(tool_response, tool_len, &port);
    free(tool_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-ask-deny-%d",
             (int)getpid());
    cli = start_interactive_cli_asking_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "Tool: shell", 1));

    /* Digit shortcut: '3' jumps directly to and confirms the third option
     * (Deny) without needing arrow keys. */
    CHECK(write_interactive(master_fd, "3", 1));
    CHECK(interactive_wait_for(master_fd, &result, "shell: denied", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: turn failed: denied",
                               1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(permission_ask_allow_for_process_elevates_policy) {
    const char *first_answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"first done\"}}]}\n\n"
        "data: [DONE]\n\n";
    const char *second_answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"second done\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t first_answer_len;
    size_t second_answer_len;
    char *tool_response = build_chunked_response(
        permission_tool_sse, strlen(permission_tool_sse), "HTTP/1.1 200 OK",
        &tool_len);
    char *first_answer = build_chunked_response(
        first_answer_sse, strlen(first_answer_sse), "HTTP/1.1 200 OK",
        &first_answer_len);
    char *second_answer = build_chunked_response(
        second_answer_sse, strlen(second_answer_sse), "HTTP/1.1 200 OK",
        &second_answer_len);
    const char *responses[] = {tool_response, first_answer, tool_response,
                              second_answer};
    size_t lengths[] = {tool_len, first_answer_len, tool_len,
                       second_answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 4, &port);
    free(tool_response);
    free(first_answer);
    free(second_answer);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-ask-allow-process-%d", (int)getpid());
    cli = start_interactive_cli_asking_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "Tool: shell", 1));

    /* Digit shortcut '2' -> "Allow for process". */
    CHECK(write_interactive(master_fd, "2", 1));
    CHECK(interactive_wait_for(
        master_fd, &result,
        "oi: tool policy set to allow for the rest of this process", 1));
    CHECK(interactive_wait_for(master_fd, &result, "first done", 1));

    /* A second tool call in a later turn must not prompt again -- the
     * process-wide policy is now allow. */
    CHECK(write_interactive(master_fd, "run it again\r", 13));
    CHECK(interactive_wait_for(master_fd, &result, "second done", 1));
    CHECK_EQ(count_text(result.output, "Tool: shell"), 1);

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(permission_ask_ctrl_c_while_awaiting_denies_without_hanging) {
    size_t tool_len;
    char *tool_response = build_chunked_response(
        permission_tool_sse, strlen(permission_tool_sse), "HTTP/1.1 200 OK",
        &tool_len);
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server(tool_response, tool_len, &port);
    free(tool_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-ask-ctrlc-%d",
             (int)getpid());
    cli = start_interactive_cli_asking_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "Tool: shell", 1));

    /* Dismissing the selector via Ctrl+C must resolve as a deny, not hang
     * waiting for a decision that will never come. */
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: turn failed: denied",
                               1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(permission_ask_resize_redraws_the_selector) {
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"finished\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_response = build_chunked_response(
        permission_tool_sse, strlen(permission_tool_sse), "HTTP/1.1 200 OK",
        &tool_len);
    char *answer_response = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_response, answer_response};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    struct winsize resized = {0};
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(tool_response);
    free(answer_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-ask-resize-%d",
             (int)getpid());
    cli = start_interactive_cli_asking_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "Tool: shell", 1));

    /* A fresh "\x1b[J" appearing with no further keystroke proves the
     * redraw was driven by the resize signal, not by typing -- same
     * technique as the composer-level resize test. */
    result.output_len = 0;
    resized.ws_row = 24;
    resized.ws_col = 100;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &resized), 0);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[J", 1));

    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "finished", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(permission_ask_typing_does_not_leak_into_the_draft) {
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"finished\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_response = build_chunked_response(
        permission_tool_sse, strlen(permission_tool_sse), "HTTP/1.1 200 OK",
        &tool_len);
    char *answer_response = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_response, answer_response};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(tool_response);
    free(answer_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-ask-notype-%d",
             (int)getpid());
    cli = start_interactive_cli_asking_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "Tool: shell", 1));

    /* Plain letters are not digit shortcuts -- they must have no visible
     * effect on the selector and must never reach the editor. */
    CHECK(write_interactive(master_fd, "xyz", 3));
    /* Confirms at the default selection (index 0, "Allow once"): if any
     * of "xyz" had leaked into the editor instead, this "\r" would submit
     * garbage as a new message rather than confirming the selector. */
    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "finished", 1));

    /* Back at an idle prompt: if "xyz" had leaked into the draft, Ctrl+D
     * would delete forward instead of exiting (same technique used
     * elsewhere in this file to prove a draft is empty). */
    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(permissions_allow_requires_confirmation_and_can_be_cancelled) {
    int master_fd = -1;
    int slave_fd = -1;
    pid_t cli;
    struct interactive_result result;
    char session_root[128];

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-permissions-cancel-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_asking_tools(9, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "/permissions allow\r", 20));
    CHECK(interactive_wait_for(master_fd, &result,
                               "Cancel (keep current policy)", 1));
    CHECK(interactive_wait_for(master_fd, &result, "Allow all tools", 1));

    /* Confirms at the default selection (index 0, "Cancel"). */
    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: permissions unchanged",
                               1));

    /* Policy is genuinely unchanged -- still `ask`, not silently
     * elevated. */
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Permissions: ask", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
}

TEST(permissions_allow_confirmed_elevates_policy) {
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"finished\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_response = build_chunked_response(
        permission_tool_sse, strlen(permission_tool_sse), "HTTP/1.1 200 OK",
        &tool_len);
    char *answer_response = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_response, answer_response};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(tool_response);
    free(answer_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-permissions-confirm-%d", (int)getpid());
    cli = start_interactive_cli_asking_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "/permissions allow\r", 20));
    CHECK(interactive_wait_for(master_fd, &result, "Allow all tools", 1));

    /* Digit shortcut '2' -> the second option, "Allow...". */
    CHECK(write_interactive(master_fd, "2", 1));
    CHECK(interactive_wait_for(master_fd, &result, "Permissions: allow", 1));

    /* A subsequent tool call must run immediately -- no selector, no
     * further prompt. */
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "finished", 1));
    CHECK(strstr(result.output, "Tool: shell") == NULL);

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(permissions_allow_when_already_allowed_skips_confirmation) {
    int master_fd = -1;
    int slave_fd = -1;
    pid_t cli;
    struct interactive_result result;
    char session_root[128];

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-permissions-already-allowed-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_allowing_tools(9, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "/permissions allow\r", 20));
    /* Dispatches immediately: no confirm selector at all. */
    CHECK(interactive_wait_for(master_fd, &result, "Permissions: allow", 1));
    CHECK(strstr(result.output, "Cancel (keep current policy)") == NULL);

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
}

TEST(tool_panel_shows_live_output_and_failed_status) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_panel\",\"type\":\"function\",\"function\":{"
        "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"printf "
        "part1; sleep 1; printf part2; exit 7\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"finished\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_response = build_chunked_response(tool_sse, strlen(tool_sse),
                                                 "HTTP/1.1 200 OK", &tool_len);
    char *answer_response = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_response, answer_response};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(tool_response);
    free(answer_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-tool-panel-%d",
             (int)getpid());
    cli = start_interactive_cli_allowing_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "shell: running", 1));
    CHECK(interactive_wait_for(master_fd, &result, "part1", 1));
    CHECK(interactive_wait_for(master_fd, &result, "part2", 1));
    CHECK(interactive_wait_for(master_fd, &result, "shell: failed", 1));
    CHECK(interactive_wait_for(master_fd, &result, "finished", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(tool_panel_survives_malicious_escape_bytes_in_output) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_evil\",\"type\":\"function\",\"function\":{"
        "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"printf "
        "'\\\\\\\\033[31mred\\\\\\\\033[0m'\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"finished\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_response = build_chunked_response(tool_sse, strlen(tool_sse),
                                                 "HTTP/1.1 200 OK", &tool_len);
    char *answer_response = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_response, answer_response};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(tool_response);
    free(answer_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-tool-evil-%d",
             (int)getpid());
    cli = start_interactive_cli_allowing_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    /* The tool prints raw CSI-colored "red" -- sanitized down to plain
     * text by the panel; the process must not hang or crash regardless. */
    CHECK(interactive_wait_for(master_fd, &result, "red", 1));
    CHECK(interactive_wait_for(master_fd, &result, "finished", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(tool_panel_coexists_with_resize_and_queued_input) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_coexist\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":"
        "\\\"sleep 1\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"second done\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_response = build_chunked_response(tool_sse, strlen(tool_sse),
                                                 "HTTP/1.1 200 OK", &tool_len);
    char *answer_response = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_response, answer_response};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    struct winsize resized = {0};
    char session_root[128];

    /* Only 2 mock turns: queuing "queue this" sets steering immediately,
     * which (per issue #25's own design) also blocks the follow-up model
     * round that would otherwise reply to the tool's result -- once the
     * tool finishes, this turn ends right away with no assistant reply of
     * its own, and the queued message resumes as its own fresh turn. */
    server = start_mock_server_turns(responses, lengths, 2, &port);
    free(tool_response);
    free(answer_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-tool-coexist-%d",
             (int)getpid());
    cli = start_interactive_cli_allowing_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "shell: running", 1));

    /* Resize while the tool panel is showing: must redraw cleanly, not
     * crash or corrupt the display. */
    result.output_len = 0;
    resized.ws_row = 24;
    resized.ws_col = 100;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &resized), 0);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[J", 1));

    /* Queue a message while the tool is still running. */
    CHECK(write_interactive(master_fd, "queue this\r", 11));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    /* The tool finishing and steering ending the turn both happen within
     * the same reactor step, so the turn loop's own "don't redraw once
     * present.done is true" rule (needed to avoid corrupting a final
     * streamed reply) means the panel's "completed" frame is never drawn
     * here -- only its earlier "running" state was. The queued message
     * resumes automatically at the safe boundary regardless. */
    CHECK(interactive_wait_for(master_fd, &result, "second done", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
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

/* Every history record's record_id (and the checkpoint-specific
 * source_first_record_id/source_last_record_id) are written as JSON
 * strings, e.g. "record_id":"3" -- extracts one's numeric value. */
static uint64_t oilog_record_u64_field(const char *record, const char *key) {
    char needle[64];
    const char *marker;

    snprintf(needle, sizeof needle, "\"%s\":\"", key);
    marker = strstr(record, needle);
    CHECK(marker != NULL);
    return (uint64_t)strtoull(marker + strlen(needle), NULL, 10);
}

/* Isolates the last captured HTTP request in a multi-turn capture file
 * (start_mock_server_turns_capture appends every turn's raw request bytes
 * back-to-back with no delimiter) by finding the last occurrence of the
 * request line every chat-completion request starts with. `buffer` must
 * outlive the returned pointer, which points into it. */
static const char *last_request_text(const char *capture_path, char *buffer,
                                     size_t buffer_len) {
    static const char needle[] = "POST /v1/chat/completions";
    FILE *capture = fopen(capture_path, "r");
    size_t n;
    const char *last = NULL;
    const char *scan;

    CHECK(capture != NULL);
    n = capture == NULL ? 0 : fread(buffer, 1, buffer_len - 1, capture);
    if (capture != NULL) {
        fclose(capture);
    }
    buffer[n] = '\0';

    scan = buffer;
    for (;;) {
        const char *marker = strstr(scan, needle);
        if (marker == NULL) {
            break;
        }
        last = marker;
        scan = marker + (sizeof needle - 1);
    }
    CHECK(last != NULL);
    return last != NULL ? last : buffer;
}

static const char compact_first_sse[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
    "\"first reply\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},"
    "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";
static const char compact_second_sse[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
    "\"second reply\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},"
    "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";
static const char compact_summary_sse[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
    "\"COMPACT_SUMMARY_MARKER\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},"
    "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";
static const char compact_third_sse[] =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
    "\"third reply\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},"
    "\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";

TEST(compact_replaces_older_turns_with_a_checkpoint) {
    size_t first_len;
    size_t second_len;
    size_t summary_len;
    size_t third_len;
    char *first = build_chunked_response(
        compact_first_sse, strlen(compact_first_sse), "HTTP/1.1 200 OK",
        &first_len);
    char *second = build_chunked_response(
        compact_second_sse, strlen(compact_second_sse), "HTTP/1.1 200 OK",
        &second_len);
    char *summary = build_chunked_response(
        compact_summary_sse, strlen(compact_summary_sse), "HTTP/1.1 200 OK",
        &summary_len);
    char *third = build_chunked_response(
        compact_third_sse, strlen(compact_third_sse), "HTTP/1.1 200 OK",
        &third_len);
    const char *responses[] = {first, second, summary, third};
    size_t lengths[] = {first_len, second_len, summary_len, third_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_dir[] = "/tmp";
    char session_name[64];
    char log_path[160];
    char capture_path[160];

    snprintf(session_name, sizeof session_name, "oi-cli-compact-%d",
             (int)getpid());
    snprintf(log_path, sizeof log_path, "%s/%s.oilog", session_dir,
             session_name);
    unlink(log_path);
    snprintf(capture_path, sizeof capture_path,
             "/tmp/oi-cli-compact-capture-%d", (int)getpid());
    unlink(capture_path);

    server = start_mock_server_turns_capture(responses, lengths, 4, &port,
                                             capture_path);
    free(first);
    free(second);
    free(summary);
    free(third);

    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_with_session(port, slave_fd, session_dir,
                                             session_name);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "first message\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "first reply", 1));
    CHECK(write_interactive(master_fd, "second message\r", 15));
    CHECK(interactive_wait_for(master_fd, &result, "second reply", 1));

    CHECK(write_interactive(master_fd, "/compact 1\r", 11));
    CHECK(interactive_wait_for(
        master_fd, &result, "oi: compacted 1 turn into a checkpoint", 1));

    CHECK(write_interactive(master_fd, "third message\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "third reply", 1));

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
        char buffer[16384];
        const char *last =
            last_request_text(capture_path, buffer, sizeof buffer);

        CHECK(strstr(last, "COMPACT_SUMMARY_MARKER") != NULL);
        CHECK(strstr(last, "second reply") != NULL);
        CHECK(strstr(last, "third message") != NULL);
        CHECK(strstr(last, "first reply") == NULL);
        CHECK(strstr(last, "first message") == NULL);
    }

    {
        struct oilog_records records;
        size_t checkpoint_index;
        size_t user1_index;
        size_t assistant1_index;
        const char *checkpoint_needles[] = {"\"type\":\"checkpoint\"",
                                            "COMPACT_SUMMARY_MARKER"};
        const char *user1_needle[] = {"\"content\":\"first message\""};
        const char *assistant1_needle[] = {"\"content\":\"first reply\""};

        oilog_records_load(log_path, &records);
        checkpoint_index =
            oilog_find(&records, 0, checkpoint_needles, 2);
        CHECK(checkpoint_index != SIZE_MAX);
        user1_index = oilog_find(&records, 0, user1_needle, 1);
        assistant1_index = oilog_find(&records, 0, assistant1_needle, 1);
        CHECK(user1_index != SIZE_MAX);
        CHECK(assistant1_index != SIZE_MAX);
        CHECK_EQ(oilog_record_u64_field(records.items[checkpoint_index],
                                        "source_first_record_id"),
                 oilog_record_u64_field(records.items[user1_index],
                                        "record_id"));
        CHECK_EQ(oilog_record_u64_field(records.items[checkpoint_index],
                                        "source_last_record_id"),
                 oilog_record_u64_field(records.items[assistant1_index],
                                        "record_id"));
        oilog_records_free(&records);
    }

    unlink(log_path);
    unlink(capture_path);
}

TEST(compact_reports_usage_error_and_nothing_to_compact) {
    const char *sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"ok\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},"
        "\"finish_reason\":\"stop\"}]}\n\n"
        "data: [DONE]\n\n";
    size_t total;
    char *response =
        build_chunked_response(sse, strlen(sse), "HTTP/1.1 200 OK", &total);
    unsigned short port;
    pid_t server = start_mock_server(response, total, &port);
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    free(response);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-compact-usage-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));

    /* No conversation exists yet. */
    CHECK(write_interactive(master_fd, "/compact\r", 9));
    CHECK(interactive_wait_for(master_fd, &result,
                               "oi: nothing to compact yet", 1));

    /* Malformed argument, still before any real turn. */
    CHECK(write_interactive(master_fd, "/compact abc\r", 13));
    CHECK(interactive_wait_for(master_fd, &result,
                               "oi: usage: /compact [turns]", 1));

    /* One real turn; the default keep_turns=8 leaves nothing to compact. */
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "ok", 1));
    CHECK(write_interactive(master_fd, "/compact\r", 9));
    CHECK(interactive_wait_for(
        master_fd, &result, "oi: nothing to compact (1 turn, keeping 8)",
        1));

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

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0) {
                snprintf(session_path, sizeof session_path, "%s/%s",
                        session_root, entry->d_name);
            }
        }
        if (directory != NULL) {
            closedir(directory);
        }
        if (session_path[0] != '\0') {
            char metadata_path[640];
            char history_path[640];

            snprintf(metadata_path, sizeof metadata_path,
                     "%s/metadata.json", session_path);
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(metadata_path);
            unlink(history_path);
            rmdir(session_path);
        }
    }
    rmdir(session_root);
}

TEST(compact_typed_while_a_turn_is_active_queues_and_runs_next) {
    size_t first_len;
    size_t summary_len;
    char *first = build_chunked_response(
        compact_first_sse, strlen(compact_first_sse), "HTTP/1.1 200 OK",
        &first_len);
    char *summary = build_chunked_response(
        compact_summary_sse, strlen(compact_summary_sse), "HTTP/1.1 200 OK",
        &summary_len);
    struct slow_mock_turn turns[2];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    /* The only real turn is the slow one, so this races the queue against
     * the very first submission in the whole test -- avoiding any
     * ambiguity from an earlier "\r\n" already sitting in the buffer (see
     * queued_command_while_busy_resolves_discarded_and_dispatches_live's
     * identical "hello\r" / wait-for-"\r\n" / queue-behind-it shape,
     * which relies on the same "first ever submission" property). */
    turns[0].response = first;
    turns[0].response_len = first_len;
    turns[0].delay_seconds = 3;
    turns[1].response = summary;
    turns[1].response_len = summary_len;
    turns[1].delay_seconds = 0;
    server = start_slow_mock_server(turns, 2, &port);

    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-compact-queue-%d",
             (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "first message\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    /* Queues /compact while the only turn so far is still in flight --
     * once it completes there's exactly 1 turn, so /compact 0 (compact
     * everything) is what actually has something to do. */
    CHECK(write_interactive(master_fd, "/compact 0\r", 11));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    CHECK(interactive_wait_for(master_fd, &result, "first reply", 1));
    CHECK(interactive_wait_for(
        master_fd, &result, "oi: compacted 1 turn into a checkpoint", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
    free(first);
    free(summary);

    {
        DIR *directory = opendir(session_root);
        struct dirent *entry;
        char session_path[512] = {0};

        CHECK(directory != NULL);
        while (directory != NULL && (entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0) {
                snprintf(session_path, sizeof session_path, "%s/%s",
                        session_root, entry->d_name);
            }
        }
        if (directory != NULL) {
            closedir(directory);
        }
        if (session_path[0] != '\0') {
            char metadata_path[640];
            char history_path[640];

            snprintf(metadata_path, sizeof metadata_path,
                     "%s/metadata.json", session_path);
            snprintf(history_path, sizeof history_path, "%s/history.oilog",
                     session_path);
            unlink(metadata_path);
            unlink(history_path);
            rmdir(session_path);
        }
    }
    rmdir(session_root);
}

TEST(compact_failure_leaves_durable_and_live_state_untouched) {
    size_t first_len;
    size_t second_len;
    size_t error_len;
    size_t third_len;
    char *first = build_chunked_response(
        compact_first_sse, strlen(compact_first_sse), "HTTP/1.1 200 OK",
        &first_len);
    char *second = build_chunked_response(
        compact_second_sse, strlen(compact_second_sse), "HTTP/1.1 200 OK",
        &second_len);
    char *error = build_chunked_response(
        "internal error", 14, "HTTP/1.1 500 Internal Server Error",
        &error_len);
    char *third = build_chunked_response(
        compact_third_sse, strlen(compact_third_sse), "HTTP/1.1 200 OK",
        &third_len);
    const char *responses[] = {first, second, error, third};
    size_t lengths[] = {first_len, second_len, error_len, third_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_dir[] = "/tmp";
    char session_name[64];
    char log_path[160];
    char capture_path[160];

    snprintf(session_name, sizeof session_name, "oi-cli-compact-fail-%d",
             (int)getpid());
    snprintf(log_path, sizeof log_path, "%s/%s.oilog", session_dir,
             session_name);
    unlink(log_path);
    snprintf(capture_path, sizeof capture_path,
             "/tmp/oi-cli-compact-fail-capture-%d", (int)getpid());
    unlink(capture_path);

    server = start_mock_server_turns_capture(responses, lengths, 4, &port,
                                             capture_path);
    free(first);
    free(second);
    free(error);
    free(third);

    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_with_session(port, slave_fd, session_dir,
                                             session_name);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "first message\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "first reply", 1));
    CHECK(write_interactive(master_fd, "second message\r", 15));
    CHECK(interactive_wait_for(master_fd, &result, "second reply", 1));

    CHECK(write_interactive(master_fd, "/compact 1\r", 11));
    CHECK(interactive_wait_for(master_fd, &result,
                               "oi: compaction failed", 1));

    CHECK(write_interactive(master_fd, "third message\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "third reply", 1));

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
        char buffer[16384];
        const char *last =
            last_request_text(capture_path, buffer, sizeof buffer);

        /* Nothing was compacted: the third turn's own request still
         * carries the original first turn's content in full. */
        CHECK(strstr(last, "first message") != NULL);
        CHECK(strstr(last, "first reply") != NULL);
        CHECK(strstr(last, "second reply") != NULL);
    }

    {
        struct oilog_records records;
        const char *checkpoint_needles[] = {"\"type\":\"checkpoint\""};

        oilog_records_load(log_path, &records);
        CHECK_EQ(oilog_count(&records, checkpoint_needles, 1), (size_t)0);
        oilog_records_free(&records);
    }

    unlink(log_path);
    unlink(capture_path);
}

TEST(compact_survives_restart_and_replay_shows_the_checkpoint) {
    size_t first_len;
    size_t second_len;
    size_t summary_len;
    char *first = build_chunked_response(
        compact_first_sse, strlen(compact_first_sse), "HTTP/1.1 200 OK",
        &first_len);
    char *second = build_chunked_response(
        compact_second_sse, strlen(compact_second_sse), "HTTP/1.1 200 OK",
        &second_len);
    char *summary = build_chunked_response(
        compact_summary_sse, strlen(compact_summary_sse), "HTTP/1.1 200 OK",
        &summary_len);
    const char *first_life_responses[] = {first, second, summary};
    size_t first_life_lengths[] = {first_len, second_len, summary_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_dir[] = "/tmp";
    char session_name[64];
    char log_path[160];

    snprintf(session_name, sizeof session_name, "oi-cli-compact-restart-%d",
             (int)getpid());
    snprintf(log_path, sizeof log_path, "%s/%s.oilog", session_dir,
             session_name);
    unlink(log_path);

    /* First "life": two turns, then compact the first one. */
    server = start_mock_server_turns(first_life_responses,
                                     first_life_lengths, 3, &port);
    free(first);
    free(second);
    free(summary);

    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_with_session(port, slave_fd, session_dir,
                                             session_name);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "first message\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "first reply", 1));
    CHECK(write_interactive(master_fd, "second message\r", 15));
    CHECK(interactive_wait_for(master_fd, &result, "second reply", 1));
    CHECK(write_interactive(master_fd, "/compact 1\r", 11));
    CHECK(interactive_wait_for(
        master_fd, &result, "oi: compacted 1 turn into a checkpoint", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);

    /* Second "life": restart against the same session, then send one more
     * message -- its outgoing request is the proof replay reconstructed
     * the checkpoint correctly, not just that the live process remembered
     * it. */
    {
        size_t restart_len;
        char *restart_reply = build_chunked_response(
            compact_third_sse, strlen(compact_third_sse), "HTTP/1.1 200 OK",
            &restart_len);
        const char *restart_responses[] = {restart_reply};
        size_t restart_lengths[] = {restart_len};
        unsigned short restart_port;
        pid_t restart_server;
        char capture_path[160];
        char buffer[16384];
        const char *last;

        snprintf(capture_path, sizeof capture_path,
                 "/tmp/oi-cli-compact-restart-capture-%d", (int)getpid());
        unlink(capture_path);
        restart_server = start_mock_server_turns_capture(
            restart_responses, restart_lengths, 1, &restart_port,
            capture_path);
        free(restart_reply);

        CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
        memset(&result, 0, sizeof result);
        cli = start_interactive_cli_with_session(
            restart_port, slave_fd, session_dir, session_name);
        close(slave_fd);

        CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
        CHECK(write_interactive(master_fd, "fourth message\r", 15));
        CHECK(interactive_wait_for(master_fd, &result, "third reply", 1));

        CHECK(write_interactive(master_fd, "\x04", 1));
        {
            int status = 0;
            CHECK_EQ(waitpid(cli, &status, 0), cli);
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        CHECK_EQ(result.exit_code, 0);
        close(master_fd);
        waitpid(restart_server, NULL, 0);

        last = last_request_text(capture_path, buffer, sizeof buffer);
        CHECK(strstr(last, "COMPACT_SUMMARY_MARKER") != NULL);
        CHECK(strstr(last, "second reply") != NULL);
        CHECK(strstr(last, "fourth message") != NULL);
        CHECK(strstr(last, "first reply") == NULL);
        CHECK(strstr(last, "first message") == NULL);

        unlink(capture_path);
    }

    unlink(log_path);
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
    RUN(default_ask_policy_denies_safely_without_a_controlling_terminal);
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
    RUN(busy_submit_is_queued_and_a_second_one_is_refused);
    RUN(queued_message_resumes_at_the_safe_boundary_with_correct_turn_ids);
    RUN(queued_command_while_busy_resolves_discarded_and_dispatches_live);
    RUN(ctrl_c_with_a_pending_item_discards_it_and_restores_the_draft);
    RUN(crash_with_a_pending_item_restores_it_as_a_startup_draft);
    RUN(permission_ask_allow_once_lets_the_tool_run);
    RUN(permission_ask_deny_ends_the_turn);
    RUN(permission_ask_allow_for_process_elevates_policy);
    RUN(permission_ask_ctrl_c_while_awaiting_denies_without_hanging);
    RUN(permission_ask_resize_redraws_the_selector);
    RUN(permission_ask_typing_does_not_leak_into_the_draft);
    RUN(permissions_allow_requires_confirmation_and_can_be_cancelled);
    RUN(permissions_allow_confirmed_elevates_policy);
    RUN(permissions_allow_when_already_allowed_skips_confirmation);
    RUN(tool_panel_shows_live_output_and_failed_status);
    RUN(tool_panel_survives_malicious_escape_bytes_in_output);
    RUN(tool_panel_coexists_with_resize_and_queued_input);
    RUN(interactive_cwd_command_changes_the_process_directory);
    RUN(model_override_persists_across_a_restart);
    RUN(resize_redraws_the_live_prompt_at_the_new_width);
    RUN(interactive_exit_before_submission_creates_no_session);
    RUN(interactive_help_and_exit_are_dispatched_without_a_session);
    RUN(compact_replaces_older_turns_with_a_checkpoint);
    RUN(compact_reports_usage_error_and_nothing_to_compact);
    RUN(compact_typed_while_a_turn_is_active_queues_and_runs_next);
    RUN(compact_failure_leaves_durable_and_live_state_untouched);
    RUN(compact_survives_restart_and_replay_shows_the_checkpoint);
    return oi_test_report();
}
