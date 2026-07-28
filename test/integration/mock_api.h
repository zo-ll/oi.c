#ifndef OI_MOCK_API_H
#define OI_MOCK_API_H

/*
 * A scripted, local, OpenAI-compatible mock API server for integration
 * tests -- header-only, in the same spirit as test.h.
 *
 * It runs in a forked child so the test process can drive its own
 * reactor loop against a real socket rather than a stubbed transport:
 * everything from connect() through HTTP framing, chunked
 * transfer-decoding, and SSE splitting is exercised for real, and only
 * the model behind it is fake.
 *
 * Each request the client makes is served the next scripted turn, and
 * the raw bytes of every request are captured to a file so the parent
 * can assert on what the harness actually sent -- which is how a test
 * verifies that a tool result made it back into the next turn.
 *
 * The client opens one connection per request ("Connection: close"), so
 * one scripted turn is consumed per connection.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Refuses to buffer a request larger than this; no test sends one. */
#define MOCK_MAX_REQUEST (1u << 20)

struct mock_turn {
    /* NULL selects "HTTP/1.1 200 OK". */
    const char *status_line;
    /* Response body, sent with Transfer-Encoding: chunked. */
    const char *body;
    /* Bytes per HTTP chunk, so a test can force event boundaries to
     * straddle chunks. 0 sends the whole body as one chunk. */
    size_t chunk_size;
};

struct mock_api {
    pid_t pid;
    unsigned short port;
    char capture_path[64];
};

/* --- child side --- */

static long mock_parse_content_length(const char *buf, size_t header_len) {
    const char *name = "content-length:";
    size_t name_len = strlen(name);
    size_t line_start = 0;

    for (size_t i = 0; i + 1 < header_len; i++) {
        if (buf[i] != '\r' || buf[i + 1] != '\n') {
            continue;
        }
        size_t line_len = i - line_start;
        if (line_len > name_len &&
            strncasecmp(buf + line_start, name, name_len) == 0) {
            long v = 0;
            for (size_t k = line_start + name_len; k < i; k++) {
                if (buf[k] == ' ') {
                    continue;
                }
                if (buf[k] < '0' || buf[k] > '9') {
                    return -1;
                }
                v = v * 10 + (buf[k] - '0');
            }
            return v;
        }
        line_start = i + 2;
    }
    return -1;
}

/* Reads one complete HTTP request (headers plus any Content-Length
 * body). Returns its length and, via *out, a malloc'd buffer the caller
 * owns; -1 on failure with *out untouched. */
static ssize_t mock_read_request(int fd, char **out) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return -1;
    }

    size_t header_end = 0;
    long content_length = -1;

    for (;;) {
        if (len == cap) {
            if (cap >= MOCK_MAX_REQUEST) {
                free(buf);
                return -1;
            }
            char *grown = realloc(buf, cap * 2);
            if (grown == NULL) {
                free(buf);
                return -1;
            }
            buf = grown;
            cap *= 2;
        }

        ssize_t n = read(fd, buf + len, cap - len);
        if (n <= 0) {
            break;
        }
        len += (size_t)n;

        if (header_end == 0 && len >= 4) {
            for (size_t i = 3; i < len; i++) {
                if (buf[i - 3] == '\r' && buf[i - 2] == '\n' &&
                    buf[i - 1] == '\r' && buf[i] == '\n') {
                    header_end = i + 1;
                    break;
                }
            }
            if (header_end > 0) {
                content_length = mock_parse_content_length(buf, header_end);
            }
        }
        if (header_end > 0) {
            if (content_length < 0) {
                break; /* no body announced */
            }
            if (len >= header_end + (size_t)content_length) {
                break;
            }
        }
    }

    *out = buf;
    return (ssize_t)len;
}

static int mock_write_all(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            return 0;
        }
        off += (size_t)n;
    }
    return 1;
}

static void mock_serve_turn(int cfd, const struct mock_turn *turn) {
    const char *status =
        turn->status_line ? turn->status_line : "HTTP/1.1 200 OK";
    const char *body = turn->body ? turn->body : "";
    size_t body_len = strlen(body);

    char head[256];
    int hn = snprintf(head, sizeof head,
                      "%s\r\n"
                      "Content-Type: text/event-stream\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status);
    if (hn < 0 || !mock_write_all(cfd, head, (size_t)hn)) {
        return;
    }

    size_t step = turn->chunk_size ? turn->chunk_size : body_len;
    size_t off = 0;
    while (off < body_len) {
        size_t n = body_len - off < step ? body_len - off : step;
        char chunk_hdr[32];
        int cn = snprintf(chunk_hdr, sizeof chunk_hdr, "%zx\r\n", n);
        if (cn < 0 || !mock_write_all(cfd, chunk_hdr, (size_t)cn) ||
            !mock_write_all(cfd, body + off, n) ||
            !mock_write_all(cfd, "\r\n", 2)) {
            return;
        }
        off += n;
    }
    mock_write_all(cfd, "0\r\n\r\n", 5);
}

/* Capture format: each request stored as a decimal length, a newline,
 * then that many raw bytes. */
static void mock_capture(int fd, const char *req, size_t len) {
    char hdr[32];
    int hn = snprintf(hdr, sizeof hdr, "%zu\n", len);
    if (hn > 0 && mock_write_all(fd, hdr, (size_t)hn)) {
        mock_write_all(fd, req, len);
    }
}

/* --- parent side --- */

/* Returns 1 on success. On success the caller must call
 * mock_api_stop to reap the child and remove the capture file. */
static int mock_api_start(struct mock_api *api, const struct mock_turn *turns,
                           size_t nturns) {
    memset(api, 0, sizeof *api);
    api->pid = -1;

    static unsigned seq = 0;
    snprintf(api->capture_path, sizeof api->capture_path,
             "/tmp/oi_mock_capture_%ld_%u", (long)getpid(), seq++);

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
        listen(listen_fd, 8) != 0) {
        close(listen_fd);
        return 0;
    }

    socklen_t alen = sizeof addr;
    if (getsockname(listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
        close(listen_fd);
        return 0;
    }
    api->port = ntohs(addr.sin_port);

    int capture_fd =
        open(api->capture_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (capture_fd < 0) {
        close(listen_fd);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(capture_fd);
        close(listen_fd);
        unlink(api->capture_path);
        return 0;
    }

    if (pid == 0) {
        /* A client that gives up mid-response must not kill the mock. */
        signal(SIGPIPE, SIG_IGN);
        for (size_t i = 0; i < nturns; i++) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd < 0) {
                break;
            }
            char *req = NULL;
            ssize_t n = mock_read_request(cfd, &req);
            if (n >= 0) {
                mock_capture(capture_fd, req, (size_t)n);
                free(req);
            }
            mock_serve_turn(cfd, &turns[i]);
            close(cfd);
        }
        close(capture_fd);
        close(listen_fd);
        _exit(0);
    }

    close(capture_fd);
    close(listen_fd);
    api->pid = pid;
    return 1;
}

static void mock_api_stop(struct mock_api *api) {
    if (api->pid > 0) {
        pid_t rc = 0;
        for (int i = 0; i < 20 && rc == 0; i++) {
            rc = waitpid(api->pid, NULL, WNOHANG);
            if (rc == 0) {
                struct timespec delay = {0, 10000000L};
                nanosleep(&delay, NULL);
            }
        }
        if (rc == 0) {
            kill(api->pid, SIGKILL);
            while (waitpid(api->pid, NULL, 0) < 0 && errno == EINTR) {
                /* retry */
            }
        }
        api->pid = -1;
    }
    if (api->capture_path[0] != '\0') {
        unlink(api->capture_path);
        api->capture_path[0] = '\0';
    }
}

/*
 * Returns request number `index` (0-based) as a malloc'd, NUL-terminated
 * buffer the caller owns, or NULL if the mock never received that many.
 * Only meaningful after the child has finished serving them, i.e. after
 * the request that triggered it has completed.
 */
static char *mock_api_request(const struct mock_api *api, size_t index,
                               size_t *out_len) {
    FILE *f = fopen(api->capture_path, "rb");
    if (f == NULL) {
        return NULL;
    }

    char *result = NULL;
    for (size_t i = 0;; i++) {
        unsigned long len = 0;
        if (fscanf(f, "%lu\n", &len) != 1) {
            break;
        }
        if (len > MOCK_MAX_REQUEST) {
            break;
        }
        char *buf = malloc(len + 1);
        if (buf == NULL) {
            break;
        }
        if (fread(buf, 1, len, f) != len) {
            free(buf);
            break;
        }
        buf[len] = '\0';
        if (i == index) {
            result = buf;
            if (out_len) {
                *out_len = len;
            }
            break;
        }
        free(buf);
    }

    fclose(f);
    return result;
}

#endif /* OI_MOCK_API_H */
