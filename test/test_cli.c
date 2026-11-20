#include "test.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
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
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "oi/sesslog.h"

#ifndef OI_CLI_BIN
#define OI_CLI_BIN "build/oi"
#endif
#define OI_BIN OI_CLI_BIN

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
    int hold;
};

struct slow_mock_control {
    int accepted;
    int release;
    size_t accepted_unreleased;
};

static void slow_mock_wait_accepted(struct slow_mock_control *control,
                                    size_t count) {
    size_t received = 0;

    while (received < count) {
        struct pollfd descriptor;
        char byte;
        ssize_t n;

        descriptor.fd = control->accepted;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 20000) <= 0) {
            fprintf(stderr,
                    "[slow_mock] timed out waiting for accepted request "
                    "%zu of %zu\n",
                    received + 1, count);
            fflush(stderr);
            CHECK(0);
            return;
        }
        n = read(control->accepted, &byte, 1);
        if (n != 1) {
            fprintf(stderr,
                    "[slow_mock] accepted channel %s after %zu of %zu\n",
                    n == 0 ? "hit EOF (server gone)" : "failed", received,
                    count);
            fflush(stderr);
            CHECK(0);
            return;
        }
        received++;
        control->accepted_unreleased++;
    }
}

static void slow_mock_release(struct slow_mock_control *control) {
    ssize_t n;

    if (control->accepted_unreleased == 0) {
        CHECK(control->accepted_unreleased > 0);
        return;
    }
    n = write(control->release, "r", 1);
    CHECK(n == 1);
    if (n == 1) {
        control->accepted_unreleased--;
    }
}

static void slow_mock_control_free(struct slow_mock_control *control) {
    if (control->accepted >= 0) {
        close(control->accepted);
        control->accepted = -1;
    }
    if (control->release >= 0) {
        close(control->release);
        control->release = -1;
    }
}

#define SLOW_MOCK_EXIT_OK 0
#define SLOW_MOCK_EXIT_NO_CONNECTION 2
#define SLOW_MOCK_EXIT_CONTROL 3

static int slow_mock_await_release(int release_fd, size_t turn) {
    fd_set release_set;
    struct timeval timeout = {20, 0};
    char token;
    ssize_t got;
    int ready;

    FD_ZERO(&release_set);
    FD_SET(release_fd, &release_set);
    ready = select(release_fd + 1, &release_set, NULL, NULL, &timeout);
    if (ready <= 0) {
        fprintf(stderr,
                "[mock_server] turn %zu was never released (select rc=%d)\n",
                turn, ready);
        fflush(stderr);
        return 0;
    }
    got = read(release_fd, &token, 1);
    if (got != 1) {
        fprintf(stderr,
                "[mock_server] turn %zu release channel %s (read rc=%zd)\n",
                turn, got == 0 ? "hit EOF" : "failed", got);
        fflush(stderr);
        return 0;
    }
    return 1;
}

static pid_t start_slow_mock_server(const struct slow_mock_turn *turns,
                                    size_t turn_count,
                                    unsigned short *out_port,
                                    struct slow_mock_control *out_control) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int accepted_pipe[2];
    int release_pipe[2];
    pid_t pid;

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

    CHECK_EQ(pipe(accepted_pipe), 0);
    CHECK_EQ(pipe(release_pipe), 0);

    pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        int exit_code = SLOW_MOCK_EXIT_OK;

        close(accepted_pipe[0]);
        close(release_pipe[1]);

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
                fprintf(stderr,
                        "[mock_server] gave up waiting for a connection on "
                        "turn %zu\n",
                        i);
                fflush(stderr);
                exit_code = SLOW_MOCK_EXIT_NO_CONNECTION;
                break;
            }
            drain_request(cfd, -1);
            if (turns[i].hold) {
                char notification = 'A';
                ssize_t sent = write(accepted_pipe[1], &notification, 1);

                if (sent != 1) {
                    exit_code = SLOW_MOCK_EXIT_CONTROL;
                    close(cfd);
                    break;
                }
                if (!slow_mock_await_release(release_pipe[0], i)) {
                    exit_code = SLOW_MOCK_EXIT_CONTROL;
                    close(cfd);
                    break;
                }
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
        close(accepted_pipe[1]);
        close(release_pipe[0]);
        close(listen_fd);
        _exit(exit_code);
    }

    close(accepted_pipe[1]);
    close(release_pipe[0]);
    close(listen_fd);
    out_control->accepted = accepted_pipe[0];
    out_control->release = release_pipe[1];
    return pid;
}

static void slow_mock_server_expect_clean_exit(pid_t server) {
    int status = 0;

    CHECK_EQ(waitpid(server, &status, 0), server);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), SLOW_MOCK_EXIT_OK);
}

static void slow_mock_server_terminate(pid_t server) {
    int status = 0;

    CHECK_EQ(kill(server, SIGTERM), 0);
    CHECK_EQ(waitpid(server, &status, 0), server);
    if (WIFEXITED(status)) {
        CHECK_EQ(WEXITSTATUS(status), SLOW_MOCK_EXIT_OK);
    } else {
        CHECK(WIFSIGNALED(status));
        CHECK_EQ(WTERMSIG(status), SIGTERM);
    }
}

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

static pid_t start_interactive_cli_with_api_key(unsigned short port,
                                                int slave_fd,
                                                const char *session_root,
                                                const char *api_key) {
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
        argv[7] = (char *)api_key;
        argv[8] = (char *)"--deny-tools";
        argv[9] = (char *)"--session-dir";
        argv[10] = (char *)session_root;
        argv[11] = NULL;
        execv(OI_BIN, argv);
        _exit(127);
    }
    return pid;
}

static pid_t start_interactive_cli(unsigned short port, int slave_fd,
                                   const char *session_root) {
    return start_interactive_cli_with_api_key(port, slave_fd, session_root,
                                              "test-key");
}

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

static void run_cli_with_stdin(char *const argv[], const char *input,
                               struct run_result *out) {
    size_t input_len = strlen(input);
    int in_pipe[2];
    int out_pipe[2];

    memset(out, 0, sizeof *out);
    CHECK_EQ(pipe(in_pipe), 0);
    CHECK_EQ(pipe(out_pipe), 0);

    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]);
        close(out_pipe[1]);
        execv(OI_BIN, argv);
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);

    {
        void (*previous)(int) = signal(SIGPIPE, SIG_IGN);
        size_t written = 0;

        while (written < input_len) {
            ssize_t n = write(in_pipe[1], input + written,
                              input_len - written);

            if (n <= 0) {
                break;
            }
            written += (size_t)n;
        }
        signal(SIGPIPE, previous);
    }
    close(in_pipe[1]);

    ssize_t n;
    while (out->output_len < sizeof out->output - 1 &&
           (n = read(out_pipe[0], out->output + out->output_len,
                     sizeof out->output - 1 - out->output_len)) > 0) {
        out->output_len += (size_t)n;
    }
    close(out_pipe[0]);

    int status;
    waitpid(pid, &status, 0);
    out->exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

TEST(help_exits_zero) {
    char *argv[] = {(char *)OI_BIN, (char *)"--help", NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "usage:") != NULL);
}

TEST(help_flags_are_documented_in_the_guide) {
    static const char guide_path[] = "docs/CLI.md";
    char *argv[] = {(char *)OI_BIN, (char *)"--help", NULL};
    struct run_result r;
    char *guide = NULL;
    size_t guide_len = 0;
    size_t guide_cap = 0;
    FILE *file = fopen(guide_path, "r");
    const char *cursor;
    int flags_seen = 0;

    CHECK(file != NULL);
    if (file == NULL) {
        return;
    }
    for (;;) {
        size_t n;

        if (guide_len + 1 >= guide_cap) {
            size_t next_cap = guide_cap == 0 ? 65536 : guide_cap * 2;
            char *grown = realloc(guide, next_cap);

            CHECK(grown != NULL);
            if (grown == NULL) {
                free(guide);
                fclose(file);
                return;
            }
            guide = grown;
            guide_cap = next_cap;
        }
        n = fread(guide + guide_len, 1, guide_cap - 1 - guide_len, file);
        guide_len += n;
        if (n == 0) {
            break;
        }
    }
    guide[guide_len] = '\0';
    CHECK(ferror(file) == 0);
    CHECK(feof(file) != 0);
    CHECK(guide_len > 0);
    fclose(file);

    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 0);
    for (cursor = strstr(r.output, "--"); cursor != NULL;
         cursor = strstr(cursor + 2, "--")) {
        char flag[64];
        size_t len = 0;

        while (len + 2 < sizeof flag &&
               (isalnum((unsigned char)cursor[2 + len]) ||
                cursor[2 + len] == '-')) {
            len++;
        }
        if (len == 0) {
            continue;
        }
        memcpy(flag, cursor, len + 2);
        flag[len + 2] = '\0';
        flags_seen++;
        if (strstr(guide, flag) == NULL) {
            fprintf(stderr, "FAIL %s: %s does not document \"%s\"\n",
                    oi_test_current, guide_path, flag);
            CHECK(0);
        }
    }
    CHECK(flags_seen >= 10);
    free(guide);
}

TEST(piped_prompt_carries_stdin_into_the_request) {
    static const char piped_input[] =
        "Review this diff:\n"
        "+oi-stdin-sentinel-line\n";
    char *piped_argv[] = {(char *)OI_BIN, (char *)"--dry-run", NULL};
    char *argument_argv[] = {(char *)OI_BIN, (char *)"--dry-run",
                             (char *)"Review this diff", NULL};
    struct run_result r;

    run_cli_with_stdin(piped_argv, piped_input, &r);
    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "Review this diff:") != NULL);
    CHECK(strstr(r.output, "oi-stdin-sentinel-line") != NULL);

    run_cli_with_stdin(argument_argv, piped_input, &r);
    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "Review this diff") != NULL);
    CHECK(strstr(r.output, "oi-stdin-sentinel-line") == NULL);
}

TEST(missing_api_key_fails) {
    char *argv[] = {(char *)OI_BIN, (char *)"hello", NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 1);
    CHECK(strstr(r.output, "API key") != NULL);
}

TEST(api_key_flag_is_removed_from_the_command_line) {
    static const char key[] = "sk-cmdline-secret";
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
    char port[8];
    snprintf(port, sizeof port, "%u", (unsigned)ntohs(addr.sin_port));

    pid_t cli = fork();
    CHECK(cli >= 0);
    if (cli == 0) {
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        char *argv[] = {(char *)OI_BIN,     (char *)"--no-tls",
                        (char *)"--host",   (char *)"127.0.0.1",
                        (char *)"--port",   port,
                        (char *)"--api-key", (char *)key,
                        (char *)"hello",    NULL};
        execv(OI_BIN, argv);
        _exit(127);
    }

    struct pollfd pending = {listen_fd, POLLIN, 0};
    CHECK_EQ(poll(&pending, 1, 20000), 1);
    int cfd = accept(listen_fd, NULL, NULL);
    CHECK(cfd >= 0);
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", (int)cli);
    char cmdline[4096];
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = read(fd, cmdline, sizeof cmdline - 1);
    close(fd);
    CHECK(n > 0);
    for (ssize_t i = 0; i < n; i++) {
        if (cmdline[i] == '\0') {
            cmdline[i] = ' ';
        }
    }
    cmdline[n > 0 ? n : 0] = '\0';
    CHECK(strstr(cmdline, key) == NULL);
    CHECK(strstr(cmdline, "--api-key") != NULL);

    close(cfd);
    close(listen_fd);
    waitpid(cli, NULL, 0);
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

TEST(unsafe_session_id_fails_without_creating_a_file) {
    char root[] = "/tmp/oi-cli-unsafe-session-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char session_dir[96];
    snprintf(session_dir, sizeof session_dir, "%s/sessions", root);
    CHECK_EQ(mkdir(session_dir, 0700), 0);
    char escaped_path[128];
    snprintf(escaped_path, sizeof escaped_path, "%s/x.oilog", root);
    char *argv[] = {(char *)OI_BIN,      (char *)"--api-key",
                    (char *)"test-key",  (char *)"--port",
                    (char *)"1",         (char *)"--no-tls",
                    (char *)"--session-dir", session_dir,
                    (char *)"--session", (char *)"../x",
                    (char *)"hi",        NULL};
    struct run_result r;
    run_cli(argv, &r);
    CHECK_EQ(r.exit_code, 1);
    CHECK(strstr(r.output, "invalid session ID") != NULL);
    CHECK(access(escaped_path, F_OK) != 0);
    unlink(escaped_path);
    rmdir(session_dir);
    rmdir(root);
}

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

TEST(reply_whose_body_ends_at_the_close_is_printed) {
    const char *response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"no length header\"}}]}\n\n"
        "data: [DONE]\n\n";
    unsigned short port;
    pid_t child = start_mock_server(response, strlen(response), &port);

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    char *argv[] = {(char *)OI_BIN,      (char *)"--host",
                    (char *)"127.0.0.1", (char *)"--port",
                    port_str,            (char *)"--no-tls",
                    (char *)"--api-key", (char *)"test-key",
                    (char *)"go",        NULL};
    struct run_result r;
    run_cli(argv, &r);

    CHECK_EQ(r.exit_code, 0);
    CHECK(strstr(r.output, "no length header") != NULL);
    waitpid(child, NULL, 0);
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

TEST(tool_subprocess_does_not_inherit_the_api_key) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_env\",\"type\":\"function\",\"function\":{"
        "\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":\\\"printf "
        "key=$OI_API_KEY.\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"env checked\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t first_len;
    size_t second_len;
    char *first = build_chunked_response(tool_sse, strlen(tool_sse),
                                         "HTTP/1.1 200 OK", &first_len);
    char *second = build_chunked_response(answer_sse, strlen(answer_sse),
                                          "HTTP/1.1 200 OK", &second_len);
    const char *responses[] = {first, second};
    size_t lengths[] = {first_len, second_len};
    char capture_path[160];
    unsigned short port;
    snprintf(capture_path, sizeof capture_path,
             "/tmp/oi-cli-env-key-request-%d", (int)getpid());
    unlink(capture_path);
    pid_t server = start_mock_server_turns_capture(responses, lengths, 2,
                                                   &port, capture_path);
    free(first);
    free(second);

    char port_str[16];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);
    char *argv[] = {(char *)OI_BIN,     (char *)"--host",
                    (char *)"127.0.0.1", (char *)"--port",
                    port_str,           (char *)"--no-tls",
                    (char *)"--allow-tools", (char *)"go",
                    NULL};
    struct run_result result;
    CHECK_EQ(setenv("OI_API_KEY", "sk-env-secret", 1), 0);
    run_cli(argv, &result);
    unsetenv("OI_API_KEY");
    waitpid(server, NULL, 0);

    if (result.exit_code != 0) {
        fprintf(stderr, "env key CLI output: %s\n", result.output);
    }
    CHECK_EQ(result.exit_code, 0);
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
        CHECK(strstr(requests, "Bearer sk-env-secret") != NULL);
        CHECK(strstr(requests, "key=.") != NULL);
        CHECK(strstr(requests, "key=sk-env-secret") == NULL);
    }
    unlink(capture_path);
}

TEST(tool_argument_larger_than_an_arena_block_runs) {
    enum { LARGE = 70000 };
    char *filler = malloc(LARGE + 1);
    char *tool_sse = malloc(LARGE + 512);
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"large argument ran\"}}]}\n\n"
        "data: [DONE]\n\n";
    struct run_result result;

    CHECK(filler != NULL && tool_sse != NULL);
    memset(filler, 'x', LARGE);
    filler[LARGE] = '\0';
    snprintf(tool_sse, LARGE + 512,
             "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
             "\"index\":0,\"id\":\"call_big\",\"type\":\"function\","
             "\"function\":{\"name\":\"shell\",\"arguments\":"
             "\"{\\\"command\\\":\\\"true # %s\\\"}\"}}]}}]}\n\n"
             "data: [DONE]\n\n",
             filler);
    run_tool_cli(tool_sse, answer_sse, "--allow-tools", "4", "large",
                 &result);
    free(tool_sse);
    free(filler);
    if (result.exit_code != 0) {
        fprintf(stderr, "large argument CLI output: %s\n", result.output);
    }
    CHECK_EQ(result.exit_code, 0);
    CHECK(strstr(result.output, "large argument ran") != NULL);
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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    turns[1].response = reply;
    turns[1].response_len = reply_len;
    turns[1].hold = 0;
    server = start_slow_mock_server(turns, 2, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-sigint-req-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    slow_mock_wait_accepted(&control, 1);
    CHECK_EQ(kill(cli, SIGINT), 0);
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    slow_mock_release(&control);
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
    slow_mock_control_free(&control);
    slow_mock_server_terminate(server);
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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = first_reply;
    turns[0].response_len = first_len;
    turns[0].hold = 1;
    turns[1].response = second_reply;
    turns[1].response_len = second_len;
    turns[1].hold = 0;
    server = start_slow_mock_server(turns, 2, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-ctrl-d-turn-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    slow_mock_wait_accepted(&control, 1);
    {
        size_t erases_before_ctrl_d = count_text(result.output, "\x1b[J");

        CHECK(write_interactive(master_fd, "\x04", 1));
        CHECK(interactive_wait_for(master_fd, &result, "\x1b[J",
                                   erases_before_ctrl_d + 1));
    }
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));
    slow_mock_release(&control);
    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(interactive_wait_for(master_fd, &result, "secondreply", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    slow_mock_control_free(&control);
    slow_mock_server_expect_clean_exit(server);
    free(first_reply);
    free(second_reply);

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
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"recov\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"ered\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *built = build_chunked_response(reply_sse, strlen(reply_sse),
                                         "HTTP/1.1 200 OK", &reply_len);
    char reply[512];
    CHECK(reply_len <= sizeof reply);
    memcpy(reply, built, reply_len);
    free(built);
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

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
    CHECK(write_interactive(master_fd, "world", 5));
    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(strstr(result.output, "world") != NULL);
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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    turns[1].response = reply;
    turns[1].response_len = reply_len;
    turns[1].hold = 0;
    server = start_slow_mock_server(turns, 2, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-typed-ctrl-c-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    slow_mock_wait_accepted(&control, 1);
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    slow_mock_release(&control);
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
    slow_mock_control_free(&control);
    slow_mock_server_terminate(server);
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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    server = start_slow_mock_server(turns, 1, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-queue-slot-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    slow_mock_wait_accepted(&control, 1);
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    CHECK(write_interactive(master_fd, "/help\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "Commands:", 1));
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Model:", 1));

    CHECK(write_interactive(master_fd, "another\r", 8));
    CHECK(interactive_wait_for(
        master_fd, &result, "oi: a message is already queued", 1));
        slow_mock_release(&control);

    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    slow_mock_control_free(&control);
    slow_mock_server_expect_clean_exit(server);
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
            CHECK(len > 12);
            CHECK_EQ(contents[8], 2);
            offset = 12;
            while (offset + 8 <= len) {
                uint32_t record_len = (uint32_t)(unsigned char)contents[offset] |
                    ((uint32_t)(unsigned char)contents[offset + 1] << 8) |
                    ((uint32_t)(unsigned char)contents[offset + 2] << 16) |
                    ((uint32_t)(unsigned char)contents[offset + 3] << 24);
                char record[1024];

                offset += 8;
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
    CHECK_EQ(contents[8], 2);
    offset = 12;
    while (offset + 8 <= len) {
        uint32_t record_len = (uint32_t)contents[offset] |
            ((uint32_t)contents[offset + 1] << 8) |
            ((uint32_t)contents[offset + 2] << 16) |
            ((uint32_t)contents[offset + 3] << 24);
        char *record;

        offset += 8;
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

static uint64_t oilog_record_turn_id(const char *record) {
    const char *marker = strstr(record, "\"turn_id\":\"");

    CHECK(marker != NULL);
    return (uint64_t)strtoull(marker + strlen("\"turn_id\":\""), NULL, 10);
}

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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = first_reply;
    turns[0].response_len = first_len;
    turns[0].hold = 1;
    turns[1].response = second_reply;
    turns[1].response_len = second_len;
    turns[1].hold = 0;
    server = start_slow_mock_server(turns, 2, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-queue-resume-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    slow_mock_wait_accepted(&control, 1);
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));
    slow_mock_release(&control);

    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
    CHECK(interactive_wait_for(master_fd, &result, "secondreply", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    slow_mock_control_free(&control);
    slow_mock_server_expect_clean_exit(server);
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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    server = start_slow_mock_server(turns, 1, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-queue-cmd-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    slow_mock_wait_accepted(&control, 1);
    CHECK(write_interactive(master_fd, "/model gpt-test\r", 17));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));
    slow_mock_release(&control);

    CHECK(interactive_wait_for(master_fd, &result, "recovered", 1));
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
    slow_mock_control_free(&control);
    slow_mock_server_expect_clean_exit(server);
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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    server = start_slow_mock_server(turns, 1, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-queue-ctrlc-%d",
             (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    slow_mock_wait_accepted(&control, 1);
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(interactive_wait_for(
        master_fd, &result, "oi: queued input discarded", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    slow_mock_release(&control);
    CHECK(interactive_wait_for(master_fd, &result, "world", 1));

    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    slow_mock_control_free(&control);
    slow_mock_server_terminate(server);
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
    struct slow_mock_control control = {-1, -1, 0};
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

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    server = start_slow_mock_server(turns, 1, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_with_session(port, slave_fd, session_dir,
                                             session_name);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    slow_mock_wait_accepted(&control, 1);
    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

    CHECK_EQ(kill(cli, SIGKILL), 0);
    slow_mock_release(&control);
    waitpid(cli, NULL, 0);
    close(master_fd);
    slow_mock_control_free(&control);
    slow_mock_server_terminate(server);
    free(reply);

    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_with_session(1, slave_fd, session_dir,
                                             session_name);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(interactive_wait_for(master_fd, &result, "world", 1));

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

static void remove_sessions_under(const char *root);

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

    CHECK(write_interactive(master_fd, "1", 1));
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

TEST(permission_ask_shows_the_full_decoded_command_and_enter_denies) {
    char padding[1001];
    char tool_sse[2048];
    char expected[1100];
    size_t tool_len;
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    memset(padding, 'x', sizeof padding - 1);
    padding[sizeof padding - 1] = '\0';
    snprintf(tool_sse, sizeof tool_sse,
             "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
             "\"index\":0,\"id\":\"call_long\",\"type\":\"function\","
             "\"function\":{\"name\":\"shell\",\"arguments\":"
             "\"{\\\"command\\\":\\\"printf \\\\u0041BC; %s tail-marker\\\"}\""
             "}}]}}]}\n\n"
             "data: [DONE]\n\n",
             padding);
    snprintf(expected, sizeof expected, "printf ABC; %s tail-marker", padding);
    {
        char *tool_response = build_chunked_response(
            tool_sse, strlen(tool_sse), "HTTP/1.1 200 OK", &tool_len);
        server = start_mock_server(tool_response, tool_len, &port);
        free(tool_response);
    }
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-ask-long-%d",
             (int)getpid());
    cli = start_interactive_cli_asking_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "Tool: shell", 1));
    CHECK(interactive_wait_for(master_fd, &result, "Deny", 1));
    CHECK(strstr(result.output, expected) != NULL);
    CHECK(strstr(result.output, "\\u0041") == NULL);

    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "shell: denied", 1));
    CHECK_EQ(count_text(result.output, "ABC"), 1);

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
    remove_sessions_under(session_root);
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

    CHECK(write_interactive(master_fd, "2", 1));
    CHECK(interactive_wait_for(
        master_fd, &result,
        "oi: tool policy set to allow for the rest of this process", 1));
    CHECK(interactive_wait_for(master_fd, &result, "first done", 1));

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

    result.output_len = 0;
    resized.ws_row = 24;
    resized.ws_col = 100;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &resized), 0);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[J", 1));

    CHECK(write_interactive(master_fd, "1", 1));
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

    CHECK(write_interactive(master_fd, "xyz", 3));
    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "shell: denied", 1));

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

    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: permissions unchanged",
                               1));

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

static int send_oauth_callback(const char *query, char *reply,
                               size_t reply_size) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    char request[512];
    size_t used = 0;
    ssize_t got;

    if (fd < 0) {
        return 0;
    }
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(1455);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return 0;
    }
    snprintf(request, sizeof request,
             "GET /auth/callback?%s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n",
             query);
    if (!write_interactive(fd, request, strlen(request))) {
        close(fd);
        return 0;
    }
    while (used < reply_size - 1 &&
           (got = read(fd, reply + used, reply_size - 1 - used)) > 0) {
        used += (size_t)got;
    }
    reply[used] = '\0';
    close(fd);
    return used > 0;
}

static pid_t start_renewal_mock(unsigned short *out_port,
                                const char *log_path, int token_delay_ms) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    int one = 1;
    CHECK(listen_fd >= 0);
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof addr), 0);
    CHECK_EQ(listen(listen_fd, 8), 0);
    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &alen), 0);
    *out_port = ntohs(addr.sin_port);

    pid_t server = fork();
    CHECK(server >= 0);
    if (server != 0) {
        close(listen_fd);
        return server;
    }
    signal(SIGCHLD, SIG_IGN);
    for (;;) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            _exit(0);
        }
        if (fork() != 0) {
            close(cfd);
            continue;
        }
        char request[16384];
        size_t len = 0;
        char *body = NULL;
        size_t content_length = 0;
        while (len < sizeof request - 1) {
            ssize_t n = read(cfd, request + len, sizeof request - 1 - len);
            if (n <= 0) {
                break;
            }
            len += (size_t)n;
            request[len] = '\0';
            body = strstr(request, "\r\n\r\n");
            if (body != NULL) {
                const char *cl = strstr(request, "Content-Length: ");
                content_length = cl != NULL ? strtoul(cl + 16, NULL, 10) : 0;
                if ((size_t)(request + len - (body + 4)) >= content_length) {
                    break;
                }
            }
        }
        char path[128] = "";
        char auth[128] = "-";
        sscanf(request, "%*s %127s", path);
        const char *bearer = strstr(request, "Authorization: Bearer ");
        if (bearer != NULL) {
            sscanf(bearer + 22, "%127[^\r]", auth);
        }
        char line[300];
        int line_len = snprintf(line, sizeof line, "%s %s\n", path, auth);
        int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (log_fd >= 0) {
            (void)!write(log_fd, line, (size_t)line_len);
            close(log_fd);
        }
        const char *reply_body;
        if (strcmp(path, "/api/accounts/oauth/token") == 0) {
            struct timespec delay = {token_delay_ms / 1000,
                                     (long)(token_delay_ms % 1000) * 1000000L};
            nanosleep(&delay, NULL);
            reply_body = "{\"access_token\":\"fresh\",\"refresh_token\":"
                         "\"rotated\",\"expires_in\":3600}";
        } else {
            struct timespec pause = {0, 300 * 1000 * 1000};
            nanosleep(&pause, NULL);
            reply_body =
                "data: {\"type\":\"response.output_text.delta\","
                "\"delta\":\"ok\"}\n\n"
                "data: {\"type\":\"response.completed\",\"response\":{}}\n\n";
        }
        char head[256];
        int head_len = snprintf(head, sizeof head,
                                "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                                "Connection: close\r\n\r\n",
                                strlen(reply_body));
        (void)!write(cfd, head, (size_t)head_len);
        (void)!write(cfd, reply_body, strlen(reply_body));
        close(cfd);
        _exit(0);
    }
}

TEST(token_renewal_runs_beside_the_turn_and_the_next_run_uses_it) {
    char config_root[] = "/tmp/oi-cli-renewed-XXXXXX";
    char path[256];
    char log_path[256];
    char base[64];
    char credentials[512];
    unsigned short port;

    CHECK(mkdtemp(config_root) != NULL);
    snprintf(log_path, sizeof log_path, "%s/requests.log", config_root);
    pid_t server = start_renewal_mock(&port, log_path, 0);
    snprintf(base, sizeof base, "http://127.0.0.1:%u", (unsigned)port);
    int credentials_len = snprintf(
        credentials, sizeof credentials,
        "{\"version\":1,\"active\":\"openai\",\"openai\":{"
        "\"auth\":\"oauth\",\"client_id\":\"app_test\",\"host_id\":\"h\","
        "\"access_token\":\"old\",\"refresh_token\":\"refresh\","
        "\"expires_at\":%.0f,\"welcome_shown\":true}}",
        (double)time(NULL) + 100);
    snprintf(path, sizeof path, "%s/oi", config_root);
    CHECK_EQ(mkdir(path, 0700), 0);
    snprintf(path, sizeof path, "%s/oi/credentials.json", config_root);
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        CHECK_EQ(write(fd, credentials, (size_t)credentials_len),
                 (ssize_t)credentials_len);
        close(fd);
    }

    setenv("OI_CHATGPT_TEST_BASE", base, 1);
    setenv("XDG_CONFIG_HOME", config_root, 1);
    char *argv[] = {(char *)OI_BIN, (char *)"--deny-tools", (char *)"one",
                    NULL};
    struct run_result first;
    run_cli(argv, &first);
    argv[2] = (char *)"two";
    struct run_result second;
    run_cli(argv, &second);
    unsetenv("OI_CHATGPT_TEST_BASE");
    unsetenv("XDG_CONFIG_HOME");
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);

    CHECK_EQ(first.exit_code, 0);
    CHECK(strstr(first.output, "ok") != NULL);
    CHECK_EQ(second.exit_code, 0);
    char log[1024] = "";
    {
        int fd = open(log_path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t n = read(fd, log, sizeof log - 1);
        close(fd);
        CHECK(n > 0);
        log[n > 0 ? n : 0] = '\0';
    }
    CHECK_EQ(count_text(log, "/api/accounts/oauth/token"), (size_t)1);
    CHECK_EQ(count_text(log, "/v1/responses old\n"), (size_t)1);
    CHECK_EQ(count_text(log, "/v1/responses fresh\n"), (size_t)1);
    {
        char saved[2048] = "";
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t n = read(fd, saved, sizeof saved - 1);
        close(fd);
        saved[n > 0 ? n : 0] = '\0';
        CHECK(strstr(saved, "\"access_token\":\"fresh\"") != NULL);
        CHECK(strstr(saved, "\"refresh_token\":\"rotated\"") != NULL);
    }
    unlink(log_path);
    unlink(path);
    snprintf(path, sizeof path, "%s/oi/credentials.json.lock", config_root);
    unlink(path);
    snprintf(path, sizeof path, "%s/oi", config_root);
    rmdir(path);
    rmdir(config_root);
}

TEST(sent_renewal_survives_the_turn_end_and_finishes_while_idle) {
    char config_root[] = "/tmp/oi-cli-renewal-lock-XXXXXX";
    char session_root[128];
    char path[256];
    char lock_path[sizeof path + 8];
    char log_path[256];
    char base[64];
    char credentials[512];
    unsigned short port;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;

    CHECK(mkdtemp(config_root) != NULL);
    snprintf(log_path, sizeof log_path, "%s/requests.log", config_root);
    pid_t server = start_renewal_mock(&port, log_path, 1500);
    snprintf(base, sizeof base, "http://127.0.0.1:%u", (unsigned)port);
    int credentials_len = snprintf(
        credentials, sizeof credentials,
        "{\"version\":1,\"active\":\"openai\",\"openai\":{"
        "\"auth\":\"oauth\",\"client_id\":\"app_test\",\"host_id\":\"h\","
        "\"access_token\":\"old\",\"refresh_token\":\"refresh\","
        "\"expires_at\":%.0f,\"welcome_shown\":true}}",
        (double)time(NULL) + 100);
    snprintf(path, sizeof path, "%s/oi", config_root);
    CHECK_EQ(mkdir(path, 0700), 0);
    snprintf(path, sizeof path, "%s/oi/credentials.json", config_root);
    snprintf(lock_path, sizeof lock_path, "%s.lock", path);
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        CHECK_EQ(write(fd, credentials, (size_t)credentials_len),
                 (ssize_t)credentials_len);
        close(fd);
    }
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-renewal-lock-sessions-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);

    pid_t cli = fork();
    CHECK(cli >= 0);
    if (cli == 0) {
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0 ||
            dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        setenv("OI_CHATGPT_TEST_BASE", base, 1);
        setenv("XDG_CONFIG_HOME", config_root, 1);
        unsetenv("OI_API_KEY");
        char *argv[] = {(char *)OI_BIN, (char *)"--deny-tools",
                        (char *)"--session-dir", session_root, NULL};
        execv(OI_BIN, argv);
        _exit(127);
    }
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "ok\n\r\x1b[J> ", 1));
    {
        int saved_rotated = 0;
        int lock_free = 0;
        for (int i = 0; i < 45 && !(saved_rotated && lock_free); i++) {
            struct timespec pause = {0, 100 * 1000 * 1000};
            char saved[2048] = "";
            nanosleep(&pause, NULL);
            int fd = open(path, O_RDONLY);
            CHECK(fd >= 0);
            ssize_t n = read(fd, saved, sizeof saved - 1);
            close(fd);
            saved[n > 0 ? n : 0] = '\0';
            saved_rotated =
                strstr(saved, "\"refresh_token\":\"rotated\"") != NULL;
            fd = open(lock_path, O_RDWR);
            if (fd >= 0) {
                struct flock lock;
                memset(&lock, 0, sizeof lock);
                lock.l_type = F_WRLCK;
                lock.l_whence = SEEK_SET;
                lock_free = fcntl(fd, F_SETLK, &lock) == 0;
                close(fd);
            }
        }
        CHECK(saved_rotated);
        CHECK(lock_free);
    }
    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    {
        char saved[2048] = "";
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t n = read(fd, saved, sizeof saved - 1);
        close(fd);
        saved[n > 0 ? n : 0] = '\0';
        CHECK(strstr(saved, "\"refresh_token\":\"rotated\"") != NULL);
        CHECK(strstr(saved, "\"access_token\":\"fresh\"") != NULL);
    }
    {
        char log[1024] = "";
        int fd = open(log_path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t n = read(fd, log, sizeof log - 1);
        close(fd);
        log[n > 0 ? n : 0] = '\0';
        CHECK_EQ(count_text(log, "/api/accounts/oauth/token"), (size_t)1);
    }
    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    close(master_fd);
    remove_sessions_under(session_root);
    rmdir(session_root);
    unlink(log_path);
    unlink(path);
    unlink(lock_path);
    snprintf(path, sizeof path, "%s/oi", config_root);
    rmdir(path);
    rmdir(config_root);
}

static void ctrl_c_ends_the_renewal_wait(int at_login) {
    char config_root[] = "/tmp/oi-cli-renewal-exit-XXXXXX";
    char session_root[128];
    char path[256];
    char lock_path[sizeof path + 8];
    char log_path[256];
    char base[64];
    char credentials[512];
    unsigned short port;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;

    CHECK(mkdtemp(config_root) != NULL);
    snprintf(log_path, sizeof log_path, "%s/requests.log", config_root);
    pid_t server = start_renewal_mock(&port, log_path, 10000);
    snprintf(base, sizeof base, "http://127.0.0.1:%u", (unsigned)port);
    int credentials_len = snprintf(
        credentials, sizeof credentials,
        "{\"version\":1,\"active\":\"openai\",\"openai\":{"
        "\"auth\":\"oauth\",\"client_id\":\"app_test\",\"host_id\":\"h\","
        "\"access_token\":\"old\",\"refresh_token\":\"refresh\","
        "\"expires_at\":%.0f,\"welcome_shown\":true}}",
        (double)time(NULL) + 100);
    snprintf(path, sizeof path, "%s/oi", config_root);
    CHECK_EQ(mkdir(path, 0700), 0);
    snprintf(path, sizeof path, "%s/oi/credentials.json", config_root);
    snprintf(lock_path, sizeof lock_path, "%s.lock", path);
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        CHECK_EQ(write(fd, credentials, (size_t)credentials_len),
                 (ssize_t)credentials_len);
        close(fd);
    }
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-renewal-exit-sessions-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);

    pid_t cli = fork();
    CHECK(cli >= 0);
    if (cli == 0) {
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0 ||
            dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        setenv("OI_CHATGPT_TEST_BASE", base, 1);
        setenv("XDG_CONFIG_HOME", config_root, 1);
        unsetenv("OI_API_KEY");
        char *argv[] = {(char *)OI_BIN, (char *)"--deny-tools",
                        (char *)"--session-dir", session_root, NULL};
        execv(OI_BIN, argv);
        _exit(127);
    }
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "ok\n\r\x1b[J> ", 1));
    if (at_login) {
        CHECK(write_interactive(master_fd, "/login\r", 7));
    } else {
        CHECK(write_interactive(master_fd, "\x04", 1));
    }
    CHECK(interactive_wait_for(master_fd, &result,
                               "finishing the ChatGPT token renewal", 1));

    struct timespec before;
    struct timespec after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    CHECK(write_interactive(master_fd, "\x03", 1));
    if (at_login) {
        CHECK(interactive_wait_for(master_fd, &result, "choose a provider",
                                   1));
    } else {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    clock_gettime(CLOCK_MONOTONIC, &after);
    double waited = (double)(after.tv_sec - before.tv_sec) +
                    (double)(after.tv_nsec - before.tv_nsec) / 1e9;
    CHECK(waited < 1.0);
    if (at_login) {
        CHECK(write_interactive(master_fd, "\x1b", 1));
        CHECK(interactive_wait_for(master_fd, &result, "oi: login cancelled",
                                   1));
        CHECK(write_interactive(master_fd, "\x04", 1));
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    {
        char saved[2048] = "";
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t n = read(fd, saved, sizeof saved - 1);
        close(fd);
        CHECK_EQ(n, (ssize_t)credentials_len);
        CHECK(memcmp(saved, credentials, (size_t)credentials_len) == 0);
    }

    kill(server, SIGTERM);
    waitpid(server, NULL, 0);
    close(master_fd);
    remove_sessions_under(session_root);
    rmdir(session_root);
    unlink(log_path);
    unlink(path);
    unlink(lock_path);
    snprintf(path, sizeof path, "%s/oi", config_root);
    rmdir(path);
    rmdir(config_root);
}

TEST(exit_wait_for_a_token_renewal_is_reported_and_ctrl_c_ends_it) {
    ctrl_c_ends_the_renewal_wait(0);
}

TEST(login_wait_for_a_token_renewal_is_reported_and_ctrl_c_ends_it) {
    ctrl_c_ends_the_renewal_wait(1);
}

TEST(ctrl_c_cancels_a_turn_that_waits_for_a_token_renewal) {
    char config_root[] = "/tmp/oi-cli-renewal-XXXXXX";
    char session_root[128];
    char path[256];
    char base[64];
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    static const char credentials[] =
        "{\"version\":1,\"active\":\"openai\",\"openai\":{"
        "\"auth\":\"oauth\",\"client_id\":\"app_test\",\"host_id\":\"h\","
        "\"access_token\":\"expired\",\"refresh_token\":\"refresh\","
        "\"expires_at\":1,\"welcome_shown\":true}}";

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQ(bind(listen_fd, (struct sockaddr *)&addr, sizeof addr), 0);
    CHECK_EQ(listen(listen_fd, 8), 0);
    socklen_t alen = sizeof addr;
    CHECK_EQ(getsockname(listen_fd, (struct sockaddr *)&addr, &alen), 0);
    snprintf(base, sizeof base, "http://127.0.0.1:%u",
             (unsigned)ntohs(addr.sin_port));

    CHECK(mkdtemp(config_root) != NULL);
    snprintf(path, sizeof path, "%s/oi", config_root);
    CHECK_EQ(mkdir(path, 0700), 0);
    snprintf(path, sizeof path, "%s/oi/credentials.json", config_root);
    {
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        CHECK_EQ(write(fd, credentials, sizeof credentials - 1),
                 (ssize_t)(sizeof credentials - 1));
        close(fd);
    }
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-renewal-sessions-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);

    pid_t cli = fork();
    CHECK(cli >= 0);
    if (cli == 0) {
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0 ||
            dup2(slave_fd, STDIN_FILENO) < 0 ||
            dup2(slave_fd, STDOUT_FILENO) < 0 ||
            dup2(slave_fd, STDERR_FILENO) < 0) {
            _exit(126);
        }
        setenv("OI_CHATGPT_TEST_BASE", base, 1);
        setenv("XDG_CONFIG_HOME", config_root, 1);
        unsetenv("OI_API_KEY");
        char *argv[] = {(char *)OI_BIN, (char *)"--deny-tools",
                        (char *)"--session-dir", session_root, NULL};
        execv(OI_BIN, argv);
        _exit(127);
    }
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    struct timespec pause = {0, 300 * 1000 * 1000};
    nanosleep(&pause, NULL);

    struct timespec before;
    struct timespec after;
    clock_gettime(CLOCK_MONOTONIC, &before);
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    clock_gettime(CLOCK_MONOTONIC, &after);
    double waited = (double)(after.tv_sec - before.tv_sec) +
                    (double)(after.tv_nsec - before.tv_nsec) / 1e9;
    CHECK(waited < 5.0);

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);

    {
        char saved[1024];
        int fd = open(path, O_RDONLY);
        CHECK(fd >= 0);
        ssize_t n = read(fd, saved, sizeof saved - 1);
        close(fd);
        CHECK_EQ(n, (ssize_t)(sizeof credentials - 1));
        CHECK(memcmp(saved, credentials, sizeof credentials - 1) == 0);
    }
    close(master_fd);
    close(listen_fd);
    remove_sessions_under(session_root);
    rmdir(session_root);
    unlink(path);
    snprintf(path, sizeof path, "%s/oi/credentials.json.lock", config_root);
    unlink(path);
    snprintf(path, sizeof path, "%s/oi", config_root);
    rmdir(path);
    rmdir(config_root);
}

TEST(oauth_callback_with_a_wrong_state_does_not_end_the_login) {
    int master_fd = -1;
    int slave_fd = -1;
    pid_t cli;
    struct interactive_result result;
    char session_root[128];
    char config_root[] = "/tmp/oi-cli-oauth-state-XXXXXX";
    char state[128] = {0};
    char query[256];
    char reply[1024];

    CHECK(mkdtemp(config_root) != NULL);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-oauth-state-sessions-%d", (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    setenv("OI_CHATGPT_TEST_BASE", "http://127.0.0.1:9", 1);
    setenv("XDG_CONFIG_HOME", config_root, 1);
    cli = start_interactive_cli(9, slave_fd, session_root);
    unsetenv("OI_CHATGPT_TEST_BASE");
    unsetenv("XDG_CONFIG_HOME");
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "/login\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "choose a provider", 1));
    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "Choose a login method",
                               1));
    CHECK(write_interactive(master_fd, "\x1b[B\r", 4));
    CHECK(interactive_wait_for(master_fd, &result,
                               "Waiting for the browser sign-in", 1));
    {
        const char *start = strstr(result.output, "state=");
        size_t len = 0;
        CHECK(start != NULL);
        if (start != NULL) {
            start += strlen("state=");
            while (start[len] != '\0' && start[len] != '&' &&
                   !isspace((unsigned char)start[len]) &&
                   len < sizeof state - 1) {
                len++;
            }
            memcpy(state, start, len);
        }
        CHECK(len > 0);
    }

    CHECK(send_oauth_callback("code=x&state=wrong", reply, sizeof reply));
    CHECK(strstr(reply, "400 Bad Request") != NULL);
    CHECK(send_oauth_callback("code=x", reply, sizeof reply));
    CHECK(strstr(reply, "400 Bad Request") != NULL);

    {
        struct sockaddr_in addr;
        int silent = socket(AF_INET, SOCK_STREAM, 0);
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(1455);
        CHECK(silent >= 0);
        CHECK_EQ(connect(silent, (struct sockaddr *)&addr, sizeof addr), 0);
        CHECK(write_interactive(silent, "GET /auth", 9));
        CHECK(write_interactive(master_fd, "\x1b", 1));
        CHECK(interactive_wait_for(master_fd, &result, "oi: login cancelled",
                                   1));
        close(silent);
    }
    CHECK(strstr(result.output, "did not match") == NULL);

    {
        size_t waits =
            count_text(result.output, "Waiting for the browser sign-in");
        const char *start;
        size_t len = 0;
        CHECK(write_interactive(master_fd, "/login\r", 7));
        CHECK(interactive_wait_for(master_fd, &result, "choose a provider",
                                   2));
        CHECK(write_interactive(master_fd, "\r", 1));
        CHECK(interactive_wait_for(master_fd, &result,
                                   "Choose a login method", 2));
        CHECK(write_interactive(master_fd, "\x1b[B\r", 4));
        CHECK(interactive_wait_for(master_fd, &result,
                                   "Waiting for the browser sign-in",
                                   waits + 1));
        snprintf(query, sizeof query, "code=x&state=%s", state);
        CHECK(send_oauth_callback(query, reply, sizeof reply));
        CHECK(strstr(reply, "400 Bad Request") != NULL);
        start = strstr(result.output, "state=");
        start = start != NULL ? strstr(start + 1, "state=") : NULL;
        CHECK(start != NULL);
        if (start != NULL) {
            start += strlen("state=");
            while (start[len] != '\0' && start[len] != '&' &&
                   !isspace((unsigned char)start[len]) &&
                   len < sizeof state - 1) {
                len++;
            }
            memcpy(state, start, len);
            state[len] = '\0';
        }
        CHECK(len > 0);
    }

    snprintf(query, sizeof query, "error=access_denied&state=%s", state);
    CHECK(send_oauth_callback(query, reply, sizeof reply));
    CHECK(strstr(reply, "200 OK") != NULL);
    CHECK(interactive_wait_for(master_fd, &result,
                               "access was denied in the browser", 1));
    CHECK(strstr(result.output, "did not match") == NULL);

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    remove_sessions_under(session_root);
    rmdir(session_root);
    rmdir(config_root);
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

    CHECK(write_interactive(master_fd, "2", 1));
    CHECK(interactive_wait_for(master_fd, &result, "Permissions: allow", 1));

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

    result.output_len = 0;
    resized.ws_row = 24;
    resized.ws_col = 100;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &resized), 0);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[J", 1));

    CHECK(write_interactive(master_fd, "queue this\r", 11));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));

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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    server = start_slow_mock_server(turns, 1, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-sigterm-turn-%d", (int)getpid());
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));
    slow_mock_wait_accepted(&control, 1);
    CHECK_EQ(kill(cli, SIGTERM), 0);

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        slow_mock_release(&control);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    slow_mock_control_free(&control);
    slow_mock_server_terminate(server);
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
    CHECK(write_interactive(master_fd, "/status\r", 8));
    {
        char expected[192];
        snprintf(expected, sizeof expected, "CWD: %s", target_dir);
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

static const char status_sentinel_api_key[] =
    "sk-oi-status-sentinel-MUST-NOT-BE-PRINTED";

static void remove_sessions_under(const char *root) {
    DIR *directory = opendir(root);
    struct dirent *entry;

    while (directory != NULL && (entry = readdir(directory)) != NULL) {
        char path[640];

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(path, sizeof path, "%s/%s/history.oilog", root,
                 entry->d_name);
        unlink(path);
        snprintf(path, sizeof path, "%s/%s/metadata.json", root,
                 entry->d_name);
        unlink(path);
        snprintf(path, sizeof path, "%s/%s/metadata.json.lock", root,
                 entry->d_name);
        unlink(path);
        snprintf(path, sizeof path, "%s/%s", root, entry->d_name);
        rmdir(path);
    }
    if (directory != NULL) {
        closedir(directory);
    }
    rmdir(root);
}

TEST(status_reports_every_field_before_a_session_exists) {
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];
    char expected_endpoint[64];

    server = start_mock_server("data: [DONE]\n\n", 14, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-status-fresh-%d", (int)getpid());
    cli = start_interactive_cli_with_api_key(port, slave_fd, session_root,
                                             status_sentinel_api_key);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Context: not compacted",
                               1));
    CHECK(write_interactive(master_fd, "/permissions ask\r", 17));
    CHECK(interactive_wait_for(master_fd, &result, "Permissions: ask", 1));
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Context: not compacted",
                               2));
    CHECK(count_text(result.output, "Permissions: ask") >= 2);
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

    CHECK(strstr(result.output, "Session: (not created)") != NULL);
    CHECK(strstr(result.output, "Model: ") != NULL);
    snprintf(expected_endpoint, sizeof expected_endpoint,
             "Endpoint: 127.0.0.1:%u/v1/chat/completions (TLS off)",
             (unsigned)port);
    CHECK(strstr(result.output, expected_endpoint) != NULL);
    CHECK(strstr(result.output, "Permissions: deny") != NULL);
    CHECK(strstr(result.output, "Request timeout: ") != NULL);
    CHECK(strstr(result.output, "Tool timeout: ") != NULL);
    CHECK(strstr(result.output, "CWD: /") != NULL);
    CHECK(strstr(result.output, "Conversation: idle") != NULL);
    CHECK(strstr(result.output, "Queue: empty") != NULL);
    CHECK(strstr(result.output, "Checkpoint: none") != NULL);
    CHECK(strstr(result.output, "Context: not compacted") != NULL);

    CHECK(strstr(result.output, status_sentinel_api_key) == NULL);
    CHECK(strstr(result.output, "sk-oi-status-sentinel") == NULL);
    CHECK(strstr(result.output, "Authorization") == NULL);
    CHECK(strstr(result.output, "Bearer") == NULL);

    remove_sessions_under(session_root);
}

TEST(status_reports_streaming_and_queue_state_during_a_turn) {
    const char *first_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"first reply\"}}]}\n\n"
        "data: [DONE]\n\n";
    const char *second_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"second reply\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t first_len;
    size_t second_len;
    char *first = build_chunked_response(first_sse, strlen(first_sse),
                                        "HTTP/1.1 200 OK", &first_len);
    char *second = build_chunked_response(second_sse, strlen(second_sse),
                                          "HTTP/1.1 200 OK", &second_len);
    struct slow_mock_turn turns[2];
    unsigned short port;
    pid_t server;
    pid_t cli;
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = first;
    turns[0].response_len = first_len;
    turns[0].hold = 1;
    turns[1].response = second;
    turns[1].response_len = second_len;
    turns[1].hold = 0;
    server = start_slow_mock_server(turns, 2, &port, &control);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-status-stream-%d", (int)getpid());
    cli = start_interactive_cli_with_api_key(port, slave_fd, session_root,
                                             status_sentinel_api_key);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    slow_mock_wait_accepted(&control, 1);

    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Context: not compacted",
                               1));
    CHECK(strstr(result.output, "Conversation: model streaming") != NULL);
    CHECK(strstr(result.output, "Queue: empty") != NULL);
    CHECK(strstr(result.output, "Session: (not created)") == NULL);

    CHECK(write_interactive(master_fd, "world\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result,
                               "Queue: 1 message queued (5 bytes) (steering "
                               "to a safe boundary)",
                               1));

    CHECK(write_interactive(master_fd, "another\r", 8));
    CHECK(interactive_wait_for(master_fd, &result,
                               "oi: a message is already queued", 1));
    slow_mock_release(&control);

    CHECK(interactive_wait_for(master_fd, &result, "first reply", 1));
    CHECK(interactive_wait_for(master_fd, &result, "second reply", 1));
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    slow_mock_control_free(&control);
    slow_mock_server_expect_clean_exit(server);
    free(first);
    free(second);

    CHECK(strstr(result.output, "queued (5 bytes)") != NULL);
    CHECK(strstr(result.output, "queued: world") == NULL);
    CHECK(strstr(result.output, status_sentinel_api_key) == NULL);

    remove_sessions_under(session_root);
}

TEST(status_reports_a_running_tool_then_returns_to_idle_after_a_cancel) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_status\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":"
        "\\\"printf started; sleep 3\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"done here\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_resp = build_chunked_response(tool_sse, strlen(tool_sse),
                                             "HTTP/1.1 200 OK", &tool_len);
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
    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-status-tool-%d",
             (int)getpid());
    cli = start_interactive_cli_allowing_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "running tool shell", 1));
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Conversation: tool "
                                                  "running",
                               1));

    CHECK_EQ(kill(cli, SIGINT), 0);
    CHECK(interactive_wait_for(master_fd, &result, "oi: cancelled", 1));
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Conversation: idle", 1));
    CHECK(strstr(result.output, "Conversation: failed") == NULL);

    CHECK(write_interactive(master_fd, "and again\r", 10));
    CHECK(interactive_wait_for(master_fd, &result, "done here", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
    remove_sessions_under(session_root);
}

TEST(status_reports_a_failed_turn_and_a_model_change) {
    const char *denied_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_denied\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":"
        "\\\"echo hi\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    size_t denied_len;
    char *denied = build_chunked_response(denied_sse, strlen(denied_sse),
                                          "HTTP/1.1 200 OK", &denied_len);
    const char *responses[] = {denied};
    size_t lengths[] = {denied_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    server = start_mock_server_turns(responses, lengths, 1, &port);
    free(denied);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-status-failed-%d", (int)getpid());
    cli = start_interactive_cli_with_api_key(port, slave_fd, session_root,
                                             status_sentinel_api_key);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "run it\r", 7));
    CHECK(interactive_wait_for(master_fd, &result, "oi: turn failed", 1));

    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Conversation: failed", 1));

    CHECK(write_interactive(master_fd, "/model status-model\r", 20));
    CHECK(interactive_wait_for(master_fd, &result, "Model: status-model", 1));
    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(
        master_fd, &result, "Model: status-model (changed with /model)", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));

    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);
    CHECK(strstr(result.output, status_sentinel_api_key) == NULL);
    remove_sessions_under(session_root);
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

static uint64_t oilog_record_u64_field(const char *record, const char *key) {
    char needle[64];
    const char *marker;

    snprintf(needle, sizeof needle, "\"%s\":\"", key);
    marker = strstr(record, needle);
    CHECK(marker != NULL);
    return (uint64_t)strtoull(marker + strlen(needle), NULL, 10);
}

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

    CHECK(write_interactive(master_fd, "/status\r", 8));
    CHECK(interactive_wait_for(master_fd, &result, "Context: compacted", 1));
    CHECK(strstr(result.output, "Checkpoint: records ") != NULL);

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
        {
            char expected[64];
            snprintf(expected, sizeof expected,
                     "Checkpoint: records %llu-%llu",
                     (unsigned long long)oilog_record_u64_field(
                         records.items[user1_index], "record_id"),
                     (unsigned long long)oilog_record_u64_field(
                         records.items[assistant1_index], "record_id"));
            CHECK(strstr(result.output, expected) != NULL);
        }
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

    CHECK(write_interactive(master_fd, "/compact\r", 9));
    CHECK(interactive_wait_for(master_fd, &result,
                               "oi: nothing to compact yet", 1));

    CHECK(write_interactive(master_fd, "/compact abc\r", 13));
    CHECK(interactive_wait_for(master_fd, &result,
                               "oi: usage: /compact [turns]", 1));

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
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    turns[0].response = first;
    turns[0].response_len = first_len;
    turns[0].hold = 1;
    turns[1].response = summary;
    turns[1].response_len = summary_len;
    turns[1].hold = 0;
    server = start_slow_mock_server(turns, 2, &port, &control);

    snprintf(session_root, sizeof session_root, "/tmp/oi-cli-compact-queue-%d",
             (int)getpid());
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "first message\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "\r\n", 1));

    slow_mock_wait_accepted(&control, 1);
    CHECK(write_interactive(master_fd, "/compact 0\r", 11));
    CHECK(interactive_wait_for(master_fd, &result, "oi: queued", 1));
    slow_mock_release(&control);

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
    slow_mock_control_free(&control);
    slow_mock_server_expect_clean_exit(server);
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

TEST(typed_ctrl_c_cancels_compaction_and_returns_to_the_prompt) {
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
    struct slow_mock_turn turns[4] = {
        {first, first_len, 0},
        {second, second_len, 0},
        {summary, summary_len, 1},
        {third, third_len, 0},
    };
    unsigned short port;
    pid_t server;
    pid_t cli;
    struct slow_mock_control control = {-1, -1, 0};
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_dir[] = "/tmp";
    char session_name[64];
    char log_path[160];

    snprintf(session_name, sizeof session_name, "oi-cli-compact-ctrl-c-%d",
             (int)getpid());
    snprintf(log_path, sizeof log_path, "%s/%s.oilog", session_dir,
             session_name);
    unlink(log_path);

    server = start_slow_mock_server(turns, 4, &port, &control);
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
    slow_mock_wait_accepted(&control, 1);
    CHECK(write_interactive(master_fd, "\x03", 1));
    CHECK(interactive_wait_for(master_fd, &result,
                               "oi: compaction cancelled", 1));

    slow_mock_release(&control);
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
    slow_mock_control_free(&control);
    slow_mock_server_terminate(server);

    {
        struct oilog_records records;
        const char *checkpoint_needles[] = {"\"type\":\"checkpoint\""};

        oilog_records_load(log_path, &records);
        CHECK_EQ(oilog_count(&records, checkpoint_needles, 1), (size_t)0);
        oilog_records_free(&records);
    }

    unlink(log_path);
    free(first);
    free(second);
    free(summary);
    free(third);
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
    cli = start_interactive_cli(9, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "hello", 5));
    CHECK(interactive_wait_for(master_fd, &result, "hello", 1));

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

static int termios_is_cooked(int fd) {
    struct termios attributes;

    if (tcgetattr(fd, &attributes) != 0) {
        return 0;
    }
    return (attributes.c_lflag & ICANON) != 0 &&
           (attributes.c_lflag & ECHO) != 0 &&
           (attributes.c_lflag & ISIG) != 0;
}

static int exit_path_restores_terminal(int master_fd, int slave_fd,
                                       const char *session_root,
                                       void (*trigger)(int, pid_t)) {
    struct interactive_result result;
    int status = 0;
    pid_t cli;

    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(9, slave_fd, session_root);
    if (!interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1)) {
        return 0;
    }
    trigger(master_fd, cli);
    if (waitpid(cli, &status, 0) != cli || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[restore] exit path did not terminate cleanly\n");
        fflush(stderr);
        return 0;
    }
    if (!termios_is_cooked(slave_fd)) {
        fprintf(stderr, "[restore] terminal left in raw mode after exit\n");
        fflush(stderr);
        return 0;
    }
    {
        int flags = fcntl(master_fd, F_GETFL, 0);
        char buffer[256];
        ssize_t len;

        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);
        while ((len = read(master_fd, buffer, sizeof buffer)) > 0) {
            size_t room = sizeof result.output - 1 - result.output_len;
            if (result.output_len < sizeof result.output - 1) {
                if ((size_t)len > room) {
                    len = (ssize_t)room;
                }
                memcpy(result.output + result.output_len, buffer, (size_t)len);
                result.output_len += (size_t)len;
                result.output[result.output_len] = '\0';
            }
        }
        fcntl(master_fd, F_SETFL, flags);
    }
    if (count_text(result.output, "\x1b[?2004l") < 1) {
        fprintf(stderr,
                "[restore] bracketed paste was not disabled on exit\n");
        fflush(stderr);
        return 0;
    }
    if (access(session_root, F_OK) == 0) {
        fprintf(stderr, "[restore] exit path created a session directory\n");
        fflush(stderr);
        return 0;
    }
    return 1;
}

static void trigger_exit_command(int master_fd, pid_t cli) {
    (void)cli;
    CHECK(write_interactive(master_fd, "/exit\r", 6));
}

static void trigger_ctrl_d(int master_fd, pid_t cli) {
    (void)cli;
    CHECK(write_interactive(master_fd, "\x04", 1));
}

static void trigger_sigterm(int master_fd, pid_t cli) {
    (void)master_fd;
    CHECK_EQ(kill(cli, SIGTERM), 0);
}

TEST(repl_restores_terminal_state_on_every_exit_path) {
    static const struct {
        const char *name;
        void (*trigger)(int, pid_t);
    } paths[] = {
        {"the /exit command", trigger_exit_command},
        {"Ctrl+D", trigger_ctrl_d},
        {"SIGTERM", trigger_sigterm},
    };
    size_t i;

    for (i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        int master_fd = -1;
        int slave_fd = -1;
        char session_root[128];

        snprintf(session_root, sizeof session_root,
                 "/tmp/oi-cli-restore-%d-%zu", (int)getpid(), i);
        CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
        CHECK(exit_path_restores_terminal(master_fd, slave_fd, session_root,
                                          paths[i].trigger));
        close(master_fd);
        close(slave_fd);
    }
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

struct session_ids {
    char items[8][64];
    size_t count;
};

static void collect_session_ids(const char *root, struct session_ids *out) {
    DIR *directory = opendir(root);
    struct dirent *entry;

    out->count = 0;
    CHECK(directory != NULL);
    while (directory != NULL && (entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.' || out->count >= 8) {
            continue;
        }
        if (strlen(entry->d_name) >= sizeof out->items[0]) {
            continue;
        }
        memcpy(out->items[out->count], entry->d_name,
               strlen(entry->d_name) + 1);
        out->count++;
    }
    if (directory != NULL) {
        closedir(directory);
    }
    for (size_t i = 0; i + 1 < out->count; i++) {
        for (size_t j = i + 1; j < out->count; j++) {
            if (strcmp(out->items[j], out->items[i]) < 0) {
                char swap[64];
                memcpy(swap, out->items[i], sizeof swap);
                memcpy(out->items[i], out->items[j], sizeof swap);
                memcpy(out->items[j], swap, sizeof swap);
            }
        }
    }
}

static void remove_session_tree(const char *root, const char *id) {
    char path[640];
    snprintf(path, sizeof path, "%s/%s/history.oilog", root, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s/metadata.json", root, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s/metadata.json.lock", root, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s", root, id);
    rmdir(path);
}

static void run_one_interactive_turn(const char *session_root,
                                     const char *message,
                                     const char *reply_text) {
    char reply_sse[256];
    size_t reply_len;
    char *reply;
    const char *responses[1];
    size_t lengths[1];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char line[256];

    snprintf(reply_sse, sizeof reply_sse,
             "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
             "\"%s\"}}]}\n\n"
             "data: [DONE]\n\n",
             reply_text);
    reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                   "HTTP/1.1 200 OK", &reply_len);
    responses[0] = reply;
    lengths[0] = reply_len;
    server = start_mock_server_turns(responses, lengths, 1, &port);
    free(reply);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    snprintf(line, sizeof line, "%s\r", message);
    CHECK(write_interactive(master_fd, line, strlen(line)));
    CHECK(interactive_wait_for(master_fd, &result, reply_text, 1));
    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 0);
    }
    close(master_fd);
    waitpid(server, NULL, 0);
}

static void run_one_interactive_tool_turn(const char *session_root) {
    const char *tool_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_replay\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell\",\"arguments\":\"{\\\"command\\\":"
        "\\\"printf replay-tool-result\\\"}\"}}]}}]}\n\n"
        "data: [DONE]\n\n";
    const char *answer_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"remembered-answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t tool_len;
    size_t answer_len;
    char *tool_response = build_chunked_response(
        tool_sse, strlen(tool_sse), "HTTP/1.1 200 OK", &tool_len);
    char *answer_response = build_chunked_response(
        answer_sse, strlen(answer_sse), "HTTP/1.1 200 OK", &answer_len);
    const char *responses[] = {tool_response, answer_response};
    size_t lengths[] = {tool_len, answer_len};
    unsigned short port;
    pid_t server =
        start_mock_server_turns(responses, lengths, 2, &port);
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;

    free(tool_response);
    free(answer_response);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli_allowing_tools(port, slave_fd, session_root);
    close(slave_fd);

    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));
    CHECK(write_interactive(master_fd, "remembered question\r", 20));
    CHECK(interactive_wait_for(master_fd, &result, "remembered-answer", 1));
    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 0);
    }
    close(master_fd);
    waitpid(server, NULL, 0);
}

TEST(interactive_session_list_and_current_describe_the_active_session) {
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"list-reply\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply;
    const char *responses[1];
    size_t lengths[1];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];
    struct session_ids ids;

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-session-list-%d", (int)getpid());
    run_one_interactive_turn(session_root, "first message", "first-reply");

    reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                   "HTTP/1.1 200 OK", &reply_len);
    responses[0] = reply;
    lengths[0] = reply_len;
    server = start_mock_server_turns(responses, lengths, 1, &port);
    free(reply);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));

    CHECK(write_interactive(master_fd, "/session current\r", 17));
    CHECK(interactive_wait_for(master_fd, &result, "Session: (not created)",
                               1));
    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "list-reply", 1));

    collect_session_ids(session_root, &ids);
    CHECK_EQ(ids.count, 2);
    CHECK(write_interactive(master_fd, "/session list\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "Sessions (2):", 1));
    {
        char current_row[128];
        snprintf(current_row, sizeof current_row, "* %s", ids.items[1]);
        CHECK(interactive_wait_for(master_fd, &result, current_row, 1));
    }

    CHECK(write_interactive(master_fd, "/session current\r", 17));
    CHECK(interactive_wait_for(master_fd, &result, "Status: healthy", 1));
    {
        char expected_path[640];
        snprintf(expected_path, sizeof expected_path, "Path: %s/%s",
                 session_root, ids.items[1]);
        CHECK(interactive_wait_for(master_fd, &result, expected_path, 1));
    }

    {
        char line[160];
        snprintf(line, sizeof line, "/session rename %s my old work\r",
                 ids.items[0]);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    CHECK(interactive_wait_for(master_fd, &result, "Renamed session", 1));
    CHECK(write_interactive(master_fd, "/session list\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "\"my old work\"", 1));

    CHECK(write_interactive(master_fd, "/session trash ", 15));
    {
        char tail[128];
        snprintf(tail, sizeof tail, "%s\r", ids.items[1]);
        CHECK(write_interactive(master_fd, tail, strlen(tail)));
    }
    CHECK(interactive_wait_for(master_fd, &result, "active session", 1));

    CHECK(write_interactive(master_fd, "\x04", 1));
    {
        int status = 0;
        CHECK_EQ(waitpid(cli, &status, 0), cli);
        result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    CHECK_EQ(result.exit_code, 0);
    close(master_fd);
    waitpid(server, NULL, 0);

    for (size_t i = 0; i < ids.count; i++) {
        remove_session_tree(session_root, ids.items[i]);
    }
    rmdir(session_root);
}

TEST(interactive_session_switch_restores_context_and_offers_replay) {
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"second-reply\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply;
    const char *responses[1];
    size_t lengths[1];
    char capture_path[160];
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];
    struct session_ids ids;

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-session-switch-%d", (int)getpid());
    snprintf(capture_path, sizeof capture_path,
             "/tmp/oi-cli-session-switch-req-%d", (int)getpid());
    unlink(capture_path);

    run_one_interactive_tool_turn(session_root);
    collect_session_ids(session_root, &ids);
    CHECK_EQ(ids.count, 1);

    reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                   "HTTP/1.1 200 OK", &reply_len);
    responses[0] = reply;
    lengths[0] = reply_len;
    server = start_mock_server_turns_capture(responses, lengths, 1, &port,
                                             capture_path);
    free(reply);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));

    {
        char line[128];
        snprintf(line, sizeof line, "/session switch %s\r", ids.items[0]);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    CHECK(interactive_wait_for(master_fd, &result, "switched to session", 1));

    CHECK(interactive_wait_for(master_fd, &result,
                               "Show the restored conversation", 1));
    CHECK(write_interactive(master_fd, "2", 1));
    CHECK(interactive_wait_for(master_fd, &result, "you: remembered question",
                               1));
    CHECK(interactive_wait_for(master_fd, &result, "oi: remembered-answer",
                               1));
    CHECK(interactive_wait_for(master_fd, &result, "id: call_replay", 1));
    CHECK(interactive_wait_for(master_fd, &result, "name: shell", 1));
    CHECK(interactive_wait_for(
        master_fd, &result,
        "arguments: {\"command\":\"printf replay-tool-result\"}", 1));
    CHECK(interactive_wait_for(master_fd, &result,
                               "tool [call_replay]: replay-tool-result", 1));

    CHECK(write_interactive(master_fd, "/session current\r", 17));
    {
        char expected[128];
        snprintf(expected, sizeof expected, "Session: %s", ids.items[0]);
        CHECK(interactive_wait_for(master_fd, &result, expected, 1));
    }
    {
        char expected[640];
        snprintf(expected, sizeof expected, "Path: %s/%s", session_root,
                 ids.items[0]);
        CHECK(interactive_wait_for(master_fd, &result, expected, 1));
    }

    CHECK(write_interactive(master_fd, "follow up\r", 10));
    CHECK(interactive_wait_for(master_fd, &result, "second-reply", 1));
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
        char requests[16384];
        size_t request_len =
            capture == NULL ? 0
                            : fread(requests, 1, sizeof requests - 1, capture);
        CHECK(capture != NULL);
        if (capture != NULL) {
            fclose(capture);
        }
        requests[request_len] = '\0';
        CHECK(strstr(requests, "remembered question") != NULL);
        CHECK(strstr(requests, "follow up") != NULL);
    }
    unlink(capture_path);

    {
        char history_path[640];
        struct oilog_records records;
        const char *needles[1];

        snprintf(history_path, sizeof history_path, "%s/%s/history.oilog",
                 session_root, ids.items[0]);
        oilog_records_load(history_path, &records);
        needles[0] = "follow up";
        CHECK_EQ(oilog_count(&records, needles, 1), (size_t)1);
        oilog_records_free(&records);
    }

    {
        struct session_ids after;
        collect_session_ids(session_root, &after);
        for (size_t i = 0; i < after.count; i++) {
            remove_session_tree(session_root, after.items[i]);
        }
    }
    rmdir(session_root);
}

TEST(interactive_session_switch_reports_a_missing_target_and_keeps_working) {
    const char *reply_sse =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"still-here\"}}]}\n\n"
        "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(reply_sse, strlen(reply_sse),
                                         "HTTP/1.1 200 OK", &reply_len);
    const char *responses[] = {reply};
    size_t lengths[] = {reply_len};
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-session-noswitch-%d", (int)getpid());
    server = start_mock_server_turns(responses, lengths, 1, &port);
    free(reply);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));

    CHECK(write_interactive(master_fd, "hello\r", 6));
    CHECK(interactive_wait_for(master_fd, &result, "still-here", 1));

    CHECK(write_interactive(master_fd, "/session switch nosuchsession\r", 30));
    CHECK(interactive_wait_for(master_fd, &result, "no such session", 1));
    CHECK(write_interactive(master_fd, "/session switch\r", 16));
    CHECK(interactive_wait_for(master_fd, &result,
                               "usage: /session switch ID", 1));
    CHECK(write_interactive(master_fd, "/session switch ../escape\r", 26));
    CHECK(interactive_wait_for(master_fd, &result, "invalid session id", 1));

    CHECK(write_interactive(master_fd, "/session current\r", 17));
    CHECK(interactive_wait_for(master_fd, &result, "Status: healthy", 1));
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
        struct session_ids ids;
        collect_session_ids(session_root, &ids);
        for (size_t i = 0; i < ids.count; i++) {
            remove_session_tree(session_root, ids.items[i]);
        }
    }
    rmdir(session_root);
}

TEST(interactive_session_trash_restore_and_confirmed_delete) {
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];
    char trashed_dir[640];
    char live_dir[640];
    struct session_ids ids;

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-session-trash-%d", (int)getpid());
    run_one_interactive_turn(session_root, "old work", "old-reply");
    collect_session_ids(session_root, &ids);
    CHECK_EQ(ids.count, 1);
    snprintf(live_dir, sizeof live_dir, "%s/%s", session_root,
             ids.items[0]);
    snprintf(trashed_dir, sizeof trashed_dir, "%s/.trash/%s", session_root,
             ids.items[0]);

    server = start_mock_server_turns(NULL, NULL, 0, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));

    {
        char line[128];
        snprintf(line, sizeof line, "/session trash %s\r", ids.items[0]);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    CHECK(interactive_wait_for(master_fd, &result, "Trashed session", 1));
    CHECK(access(trashed_dir, F_OK) == 0);
    CHECK(access(live_dir, F_OK) != 0);

    CHECK(write_interactive(master_fd, "/session trash-list\r", 20));
    CHECK(interactive_wait_for(master_fd, &result, "Trashed sessions (1):",
                               1));

    {
        char line[128];
        snprintf(line, sizeof line, "/session restore %s\r", ids.items[0]);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    CHECK(interactive_wait_for(master_fd, &result, "Restored session", 1));
    CHECK(access(live_dir, F_OK) == 0);

    {
        char line[128];
        snprintf(line, sizeof line, "/session delete %s\r", ids.items[0]);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    {
        char confirmation[256];
        snprintf(confirmation, sizeof confirmation,
                 "Permanently delete session %s (cannot be undone)",
                 ids.items[0]);
        CHECK(interactive_wait_for(master_fd, &result, confirmation, 1));
    }
    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "deletion cancelled", 1));
    CHECK(access(live_dir, F_OK) == 0);

    {
        char line[128];
        snprintf(line, sizeof line, "/session trash %s\r", ids.items[0]);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    CHECK(interactive_wait_for(master_fd, &result, "Trashed session", 2));
    {
        char line[128];
        snprintf(line, sizeof line, "/session delete %s\r", ids.items[0]);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    {
        char confirmation[256];
        snprintf(confirmation, sizeof confirmation,
                 "Permanently delete session %s (cannot be undone)",
                 ids.items[0]);
        CHECK(interactive_wait_for(master_fd, &result, confirmation, 2));
    }
    CHECK(write_interactive(master_fd, "2", 1));
    CHECK(interactive_wait_for(master_fd, &result, "deleted session", 1));
    CHECK(access(trashed_dir, F_OK) != 0);
    CHECK(access(live_dir, F_OK) != 0);

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
        struct session_ids after;
        char trash_root[256];
        collect_session_ids(session_root, &after);
        for (size_t i = 0; i < after.count; i++) {
            remove_session_tree(session_root, after.items[i]);
        }
        snprintf(trash_root, sizeof trash_root, "%s/.trash", session_root);
        rmdir(trash_root);
    }
    rmdir(session_root);
}

TEST(interactive_session_import_requires_confirmation_and_keeps_the_source) {
    unsigned short port;
    pid_t server;
    pid_t cli;
    int master_fd = -1;
    int slave_fd = -1;
    struct interactive_result result;
    char session_root[128];
    char source_path[160];
    char *before = NULL;
    long before_len = 0;

    snprintf(session_root, sizeof session_root,
             "/tmp/oi-cli-session-import-%d", (int)getpid());
    snprintf(source_path, sizeof source_path, "/tmp/oi-cli-legacy-%d.oilog",
             (int)getpid());
    unlink(source_path);

    {
        oi_sesslog *log = NULL;
        CHECK_EQ(oi_sesslog_open(source_path, &log), OI_OK);
        CHECK_EQ(oi_sesslog_append(log, "legacy question", 15), OI_OK);
        CHECK_EQ(oi_sesslog_append(log, "legacy answer", 13), OI_OK);
        oi_sesslog_close(log);
    }
    {
        FILE *source = fopen(source_path, "rb");
        CHECK(source != NULL);
        CHECK_EQ(fseek(source, 0, SEEK_END), 0);
        before_len = ftell(source);
        CHECK(before_len > 0);
        rewind(source);
        before = malloc((size_t)before_len);
        CHECK(before != NULL);
        CHECK_EQ(fread(before, 1, (size_t)before_len, source),
                 (size_t)before_len);
        fclose(source);
    }

    server = start_mock_server_turns(NULL, NULL, 0, &port);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    memset(&result, 0, sizeof result);
    cli = start_interactive_cli(port, slave_fd, session_root);
    close(slave_fd);
    CHECK(interactive_wait_for(master_fd, &result, "\x1b[?2004h", 1));

    {
        char line[256];
        snprintf(line, sizeof line, "/session import %s\r", source_path);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    {
        char confirmation[384];
        snprintf(confirmation, sizeof confirmation,
                 "Copy '%s' into a new session", source_path);
        CHECK(interactive_wait_for(master_fd, &result, confirmation, 1));
    }
    CHECK(write_interactive(master_fd, "\r", 1));
    CHECK(interactive_wait_for(master_fd, &result, "import cancelled", 1));
    CHECK(write_interactive(master_fd, "/session list\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "Sessions: none", 1));

    {
        char line[256];
        snprintf(line, sizeof line, "/session import %s\r", source_path);
        CHECK(write_interactive(master_fd, line, strlen(line)));
    }
    {
        char confirmation[384];
        snprintf(confirmation, sizeof confirmation,
                 "Copy '%s' into a new session", source_path);
        CHECK(interactive_wait_for(master_fd, &result, confirmation, 2));
    }
    CHECK(write_interactive(master_fd, "2", 1));
    CHECK(interactive_wait_for(master_fd, &result, "imported as session", 1));
    CHECK(write_interactive(master_fd, "/session list\r", 14));
    CHECK(interactive_wait_for(master_fd, &result, "Sessions (1):", 1));

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
        FILE *source = fopen(source_path, "rb");
        char *after;
        long after_len;
        CHECK(source != NULL);
        CHECK_EQ(fseek(source, 0, SEEK_END), 0);
        after_len = ftell(source);
        CHECK_EQ(after_len, before_len);
        rewind(source);
        after = malloc((size_t)after_len);
        CHECK(after != NULL);
        CHECK_EQ(fread(after, 1, (size_t)after_len, source),
                 (size_t)after_len);
        fclose(source);
        CHECK_EQ(memcmp(before, after, (size_t)before_len), 0);
        free(after);
    }
    free(before);
    unlink(source_path);

    {
        struct session_ids ids;
        collect_session_ids(session_root, &ids);
        for (size_t i = 0; i < ids.count; i++) {
            remove_session_tree(session_root, ids.items[i]);
        }
    }
    rmdir(session_root);
}

static int slow_mock_client_request(unsigned short port) {
    struct sockaddr_in addr;
    static const char request[] = "POST /v1/chat/completions HTTP/1.1\r\n\r\n";
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    CHECK(fd >= 0);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    CHECK_EQ(connect(fd, (struct sockaddr *)&addr, sizeof addr), 0);
    CHECK_EQ(write(fd, request, sizeof request - 1),
             (ssize_t)(sizeof request - 1));
    return fd;
}

static int slow_mock_readable_within(int fd, int timeout_ms) {
    struct pollfd descriptor;

    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    return poll(&descriptor, 1, timeout_ms) > 0;
}

TEST(mock_control_holds_a_response_until_it_is_released) {
    const char *body = "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(body, strlen(body),
                                         "HTTP/1.1 200 OK", &reply_len);
    struct slow_mock_turn turns[1];
    struct slow_mock_control control = {-1, -1, 0};
    unsigned short port;
    pid_t server;
    int client;

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    server = start_slow_mock_server(turns, 1, &port, &control);
    client = slow_mock_client_request(port);

    slow_mock_wait_accepted(&control, 1);
    CHECK(!slow_mock_readable_within(client, 300));

    slow_mock_release(&control);
    CHECK(slow_mock_readable_within(client, 20000));

    close(client);
    slow_mock_control_free(&control);
    slow_mock_server_expect_clean_exit(server);
    free(reply);
}

TEST(mock_control_release_before_acceptance_is_rejected) {
    const char *body = "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(body, strlen(body),
                                         "HTTP/1.1 200 OK", &reply_len);
    struct slow_mock_turn turns[2];
    struct slow_mock_control control = {-1, -1, 0};
    unsigned short port;
    pid_t server;
    pid_t checker;
    int client;

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 0;
    turns[1].response = reply;
    turns[1].response_len = reply_len;
    turns[1].hold = 1;
    server = start_slow_mock_server(turns, 2, &port, &control);

    client = slow_mock_client_request(port);
    CHECK(slow_mock_readable_within(client, 20000));
    close(client);

    checker = fork();
    CHECK(checker >= 0);
    if (checker < 0) {
        slow_mock_control_free(&control);
        slow_mock_server_terminate(server);
        free(reply);
        return;
    }
    if (checker == 0) {
        int failures_before_release = oi_test_failures;
        slow_mock_release(&control);
        _exit(oi_test_failures != failures_before_release ? 42 : 0);
    }
    {
        int status = 0;
        CHECK_EQ(waitpid(checker, &status, 0), checker);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 42);
    }

    client = slow_mock_client_request(port);
    slow_mock_wait_accepted(&control, 1);
    CHECK(!slow_mock_readable_within(client, 300));
    slow_mock_release(&control);
    CHECK(slow_mock_readable_within(client, 20000));

    close(client);
    slow_mock_control_free(&control);
    slow_mock_server_expect_clean_exit(server);
    free(reply);
}

TEST(mock_control_closing_the_release_end_fails_the_server_promptly) {
    const char *body = "data: [DONE]\n\n";
    size_t reply_len;
    char *reply = build_chunked_response(body, strlen(body),
                                         "HTTP/1.1 200 OK", &reply_len);
    struct slow_mock_turn turns[1];
    struct slow_mock_control control = {-1, -1, 0};
    unsigned short port;
    pid_t server;
    int client;
    int status = 0;
    time_t started;

    turns[0].response = reply;
    turns[0].response_len = reply_len;
    turns[0].hold = 1;
    server = start_slow_mock_server(turns, 1, &port, &control);
    client = slow_mock_client_request(port);
    slow_mock_wait_accepted(&control, 1);

    started = time(NULL);
    slow_mock_control_free(&control);
    CHECK_EQ(waitpid(server, &status, 0), server);
    CHECK(time(NULL) - started < 10);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), SLOW_MOCK_EXIT_CONTROL);
    {
        char leftover[64];
        ssize_t got;
        CHECK(slow_mock_readable_within(client, 2000));
        got = read(client, leftover, sizeof leftover);
        CHECK_EQ(got, (ssize_t)0);
    }

    close(client);
    free(reply);
}

int main(void) {
    oi_test_set_deadline(900);
    signal(SIGCHLD, SIG_DFL);
    RUN(help_exits_zero);
    RUN(help_flags_are_documented_in_the_guide);
    RUN(piped_prompt_carries_stdin_into_the_request);
    RUN(missing_api_key_fails);
    RUN(api_key_flag_is_removed_from_the_command_line);
    RUN(unrecognized_flag_fails);
    RUN(flag_missing_value_fails);
    RUN(dry_run_reports_resolved_config);
    RUN(dry_run_does_not_require_api_key);
    RUN(config_file_is_applied);
    RUN(cli_flag_overrides_config_file);
    RUN(missing_config_file_fails);
    RUN(overlong_session_path_fails_cleanly);
    RUN(unsafe_session_id_fails_without_creating_a_file);
    RUN(end_to_end_streaming_reply);
    RUN(reply_whose_body_ends_at_the_close_is_printed);
    RUN(tool_loop_executes_and_returns_result);
    RUN(tool_denial_stops_loop);
    RUN(default_ask_policy_denies_safely_without_a_controlling_terminal);
    RUN(tool_failure_is_returned_to_model);
    RUN(tool_argument_larger_than_an_arena_block_runs);
    RUN(tool_subprocess_does_not_inherit_the_api_key);
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
    RUN(permission_ask_shows_the_full_decoded_command_and_enter_denies);
    RUN(permission_ask_allow_for_process_elevates_policy);
    RUN(permission_ask_ctrl_c_while_awaiting_denies_without_hanging);
    RUN(permission_ask_resize_redraws_the_selector);
    RUN(permission_ask_typing_does_not_leak_into_the_draft);
    RUN(permissions_allow_requires_confirmation_and_can_be_cancelled);
    RUN(permissions_allow_confirmed_elevates_policy);
    RUN(permissions_allow_when_already_allowed_skips_confirmation);
    RUN(oauth_callback_with_a_wrong_state_does_not_end_the_login);
    RUN(ctrl_c_cancels_a_turn_that_waits_for_a_token_renewal);
    RUN(token_renewal_runs_beside_the_turn_and_the_next_run_uses_it);
    RUN(sent_renewal_survives_the_turn_end_and_finishes_while_idle);
    RUN(exit_wait_for_a_token_renewal_is_reported_and_ctrl_c_ends_it);
    RUN(login_wait_for_a_token_renewal_is_reported_and_ctrl_c_ends_it);
    RUN(tool_panel_shows_live_output_and_failed_status);
    RUN(tool_panel_survives_malicious_escape_bytes_in_output);
    RUN(tool_panel_coexists_with_resize_and_queued_input);
    RUN(interactive_cwd_command_changes_the_process_directory);
    RUN(status_reports_every_field_before_a_session_exists);
    RUN(status_reports_streaming_and_queue_state_during_a_turn);
    RUN(status_reports_a_running_tool_then_returns_to_idle_after_a_cancel);
    RUN(status_reports_a_failed_turn_and_a_model_change);
    RUN(model_override_persists_across_a_restart);
    RUN(resize_redraws_the_live_prompt_at_the_new_width);
    RUN(interactive_exit_before_submission_creates_no_session);
    RUN(repl_restores_terminal_state_on_every_exit_path);
    RUN(interactive_help_and_exit_are_dispatched_without_a_session);
    RUN(compact_replaces_older_turns_with_a_checkpoint);
    RUN(compact_reports_usage_error_and_nothing_to_compact);
    RUN(compact_typed_while_a_turn_is_active_queues_and_runs_next);
    RUN(compact_failure_leaves_durable_and_live_state_untouched);
    RUN(typed_ctrl_c_cancels_compaction_and_returns_to_the_prompt);
    RUN(compact_survives_restart_and_replay_shows_the_checkpoint);
    RUN(interactive_session_list_and_current_describe_the_active_session);
    RUN(interactive_session_switch_restores_context_and_offers_replay);
    RUN(interactive_session_switch_reports_a_missing_target_and_keeps_working);
    RUN(interactive_session_trash_restore_and_confirmed_delete);
    RUN(interactive_session_import_requires_confirmation_and_keeps_the_source);
    RUN(mock_control_holds_a_response_until_it_is_released);
    RUN(mock_control_release_before_acceptance_is_rejected);
    RUN(mock_control_closing_the_release_end_fails_the_server_promptly);
    return oi_test_report();
}
