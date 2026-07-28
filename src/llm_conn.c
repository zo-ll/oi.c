#include "llm_conn.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "compat.h"

enum conn_state { CS_TCP_CONNECTING, CS_TLS_HANDSHAKE, CS_ESTABLISHED };

struct oi_llm_conn {
    oi_reactor *reactor;
    int fd;
    enum conn_state state;

    int use_tls;
    char *host; /* only needed (and non-NULL) when use_tls */
    char *ca_file; /* NULL = system default trust store */
    SSL_CTX *ssl_ctx;
    SSL *ssl;

    struct oi_llm_conn_callbacks cbs;
    void *user_data;

    char *out_buf;
    size_t out_len;
    size_t out_off;
    size_t out_cap;

    /* Set by the top-level reactor callback before invoking any of
     * *cbs, so a reentrant oi_llm_conn_close() from within a callback
     * can signal back to still-executing internal code that `c` is
     * gone. See the call-site pattern in fail_conn/read_available. */
    int *destroyed_flag;
};

static oi_status set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static void update_interest(oi_llm_conn *c) {
    int interest = OI_EV_READ;
    if (c->out_off < c->out_len) {
        interest |= OI_EV_WRITE;
    }
    oi_reactor_modify(c->reactor, c->fd, interest);
}

void oi_llm_conn_close(oi_llm_conn *c) {
    if (c == NULL) {
        return;
    }
    if (c->fd >= 0) {
        oi_reactor_remove(c->reactor, c->fd);
        close(c->fd);
    }
    if (c->ssl) {
        SSL_free(c->ssl);
    }
    if (c->ssl_ctx) {
        SSL_CTX_free(c->ssl_ctx);
    }
    free(c->out_buf);
    free(c->host);
    free(c->ca_file);
    if (c->destroyed_flag) {
        *c->destroyed_flag = 1;
    }
    free(c);
}

/* Invokes on_error, then closes `c` unless the callback already did so
 * reentrantly. Callers must treat `c` as potentially freed once this
 * returns. */
static void fail_conn(oi_llm_conn *c, oi_status reason) {
    int *destroyed = c->destroyed_flag;
    if (c->cbs.on_error) {
        c->cbs.on_error(c, reason, c->user_data);
    }
    if (*destroyed) {
        return;
    }
    oi_llm_conn_close(c);
}

/* --- raw byte I/O, TLS-transparent --- */

/* Returns >0 bytes transferred, 0 on clean EOF (read only), -1 if it
 * would block (*would_block=1), -2 on a hard error. */
static long conn_raw_read(oi_llm_conn *c, void *buf, size_t len,
                           int *would_block) {
    *would_block = 0;
    if (c->use_tls) {
        int rc = SSL_read(c->ssl, buf, (int)len);
        if (rc > 0) {
            return rc;
        }
        int err = SSL_get_error(c->ssl, rc);
        if (err == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        /* WANT_WRITE during a read (TLS renegotiation) is treated the
         * same as WANT_READ here: a documented simplification, fine for
         * a simple request/response client. */
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            *would_block = 1;
            return -1;
        }
        return -2;
    }

    ssize_t rc = recv(c->fd, buf, len, 0);
    if (rc > 0) {
        return rc;
    }
    if (rc == 0) {
        return 0;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *would_block = 1;
        return -1;
    }
    return -2;
}

static long conn_raw_write(oi_llm_conn *c, const void *buf, size_t len,
                            int *would_block) {
    *would_block = 0;
    if (c->use_tls) {
        int rc = SSL_write(c->ssl, buf, (int)len);
        if (rc > 0) {
            return rc;
        }
        int err = SSL_get_error(c->ssl, rc);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            *would_block = 1;
            return -1;
        }
        return -2;
    }

    ssize_t rc = send(c->fd, buf, len, MSG_NOSIGNAL);
    if (rc >= 0) {
        return rc;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *would_block = 1;
        return -1;
    }
    return -2;
}

static void read_available(oi_llm_conn *c) {
    char buf[16384];
    for (;;) {
        int would_block;
        long n = conn_raw_read(c, buf, sizeof buf, &would_block);
        if (n > 0) {
            int *destroyed = c->destroyed_flag;
            if (c->cbs.on_data) {
                c->cbs.on_data(c, buf, (size_t)n, c->user_data);
            }
            if (*destroyed) {
                return;
            }
            continue;
        }
        if (n == 0) {
            fail_conn(c, OI_ERR_CLOSED);
            return;
        }
        if (would_block) {
            return;
        }
        fail_conn(c, OI_ERR_IO);
        return;
    }
}

/* Returns OI_OK if `c` is still alive afterward (queue drained or still
 * pending), or an error status if `c` was closed (by fail_conn) due to a
 * write error. */
static oi_status flush_write_queue(oi_llm_conn *c) {
    while (c->out_off < c->out_len) {
        int would_block;
        long n = conn_raw_write(c, c->out_buf + c->out_off,
                                 c->out_len - c->out_off, &would_block);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (would_block) {
            update_interest(c);
            return OI_OK;
        }
        fail_conn(c, OI_ERR_IO);
        return OI_ERR_IO;
    }
    c->out_len = 0;
    c->out_off = 0;
    update_interest(c);
    return OI_OK;
}

/* --- TLS handshake --- */

static oi_status start_tls_handshake(oi_llm_conn *c) {
    c->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (c->ssl_ctx == NULL) {
        return OI_ERR_IO;
    }
    SSL_CTX_set_min_proto_version(c->ssl_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(c->ssl_ctx, SSL_VERIFY_PEER, NULL);

    int loaded = c->ca_file
                     ? SSL_CTX_load_verify_locations(c->ssl_ctx, c->ca_file,
                                                      NULL)
                     : SSL_CTX_set_default_verify_paths(c->ssl_ctx);
    if (!loaded) {
        return OI_ERR_IO;
    }

    c->ssl = SSL_new(c->ssl_ctx);
    if (c->ssl == NULL) {
        return OI_ERR_IO;
    }
    if (!SSL_set_fd(c->ssl, c->fd)) {
        return OI_ERR_IO;
    }

    SSL_set_connect_state(c->ssl);
    SSL_set_tlsext_host_name(c->ssl, c->host); /* SNI */
    X509_VERIFY_PARAM *param = SSL_get0_param(c->ssl);
    X509_VERIFY_PARAM_set1_host(param, c->host, 0);

    c->state = CS_TLS_HANDSHAKE;
    return OI_OK;
}

static void drive_tls_handshake(oi_llm_conn *c) {
    int rc = SSL_do_handshake(c->ssl);
    if (rc == 1) {
        c->state = CS_ESTABLISHED;
        oi_reactor_modify(c->reactor, c->fd, OI_EV_READ);
        if (c->cbs.on_connected) {
            c->cbs.on_connected(c, c->user_data); /* nothing touches c after this */
        }
        return;
    }

    int err = SSL_get_error(c->ssl, rc);
    if (err == SSL_ERROR_WANT_READ) {
        oi_reactor_modify(c->reactor, c->fd, OI_EV_READ);
    } else if (err == SSL_ERROR_WANT_WRITE) {
        oi_reactor_modify(c->reactor, c->fd, OI_EV_WRITE);
    } else {
        fail_conn(c, OI_ERR_IO);
    }
}

/* --- reactor dispatch --- */

static void handle_tcp_connecting(oi_llm_conn *c) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
        err != 0) {
        fail_conn(c, OI_ERR_IO);
        return;
    }

    if (c->use_tls) {
        if (start_tls_handshake(c) != OI_OK) {
            fail_conn(c, OI_ERR_IO);
            return;
        }
        drive_tls_handshake(c);
        return;
    }

    c->state = CS_ESTABLISHED;
    oi_reactor_modify(c->reactor, c->fd, OI_EV_READ);
    if (c->cbs.on_connected) {
        c->cbs.on_connected(c, c->user_data);
    }
}

static void handle_established(oi_llm_conn *c, int revents) {
    if (revents & OI_EV_WRITE) {
        if (flush_write_queue(c) != OI_OK) {
            return; /* c was closed by flush_write_queue's fail_conn */
        }
    }
    if (revents & OI_EV_READ) {
        read_available(c);
    }
}

static void on_fd_event(oi_reactor *r, int fd, int revents, void *user_data) {
    (void)r;
    (void)fd;
    oi_llm_conn *c = user_data;
    int destroyed = 0;
    /* GCC's escape analysis for -Wdangling-pointer can't see that this
     * is safe: c->destroyed_flag is always cleared before this frame
     * returns (see below) unless `destroyed` was set, in which case `c`
     * itself was freed and nothing ever dereferences the stale pointer
     * again. */
OI_DIAG_PUSH_IGNORE_DANGLING
    c->destroyed_flag = &destroyed;
OI_DIAG_POP

    switch (c->state) {
    case CS_TCP_CONNECTING:
        handle_tcp_connecting(c);
        break;
    case CS_TLS_HANDSHAKE:
        drive_tls_handshake(c);
        break;
    case CS_ESTABLISHED:
        handle_established(c, revents);
        break;
    }

    if (!destroyed) {
        c->destroyed_flag = NULL;
    }
}

/* --- public API --- */

oi_status oi_llm_conn_connect(oi_reactor *r, const char *host,
                               unsigned short port, int use_tls,
                               const char *ca_file,
                               const struct oi_llm_conn_callbacks *cbs,
                               void *user_data, oi_llm_conn **out_conn) {
    if (r == NULL || host == NULL || cbs == NULL || out_conn == NULL) {
        return OI_ERR_INVAL;
    }

    signal(SIGPIPE, SIG_IGN); /* a peer closing mid-write must not kill us */

    char port_str[6];
    snprintf(port_str, sizeof port_str, "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        return OI_ERR_IO;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC,
                    ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (set_nonblocking(fd) != OI_OK) {
            close(fd);
            fd = -1;
            continue;
        }
        int cr = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (cr == 0 || errno == EINPROGRESS) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        return OI_ERR_IO;
    }

    oi_llm_conn *c = malloc(sizeof *c);
    if (c == NULL) {
        close(fd);
        return OI_ERR_NOMEM;
    }
    c->reactor = r;
    c->fd = fd;
    c->state = CS_TCP_CONNECTING;
    c->use_tls = use_tls;
    c->host = NULL;
    c->ca_file = NULL;
    c->ssl_ctx = NULL;
    c->ssl = NULL;
    c->cbs = *cbs;
    c->user_data = user_data;
    c->out_buf = NULL;
    c->out_len = 0;
    c->out_off = 0;
    c->out_cap = 0;
    c->destroyed_flag = NULL;

    if (use_tls) {
        c->host = strdup(host);
        if (c->host == NULL) {
            close(fd);
            free(c);
            return OI_ERR_NOMEM;
        }
    }
    if (ca_file) {
        c->ca_file = strdup(ca_file);
        if (c->ca_file == NULL) {
            free(c->host);
            close(fd);
            free(c);
            return OI_ERR_NOMEM;
        }
    }

    oi_status st = oi_reactor_add(r, fd, OI_EV_WRITE, on_fd_event, c);
    if (st != OI_OK) {
        free(c->host);
        free(c->ca_file);
        close(fd);
        free(c);
        return st;
    }

    *out_conn = c;
    return OI_OK;
}

oi_status oi_llm_conn_write(oi_llm_conn *c, const void *data, size_t len) {
    if (c == NULL || data == NULL || c->state != CS_ESTABLISHED) {
        return OI_ERR_INVAL;
    }
    if (len == 0) {
        return OI_OK;
    }

    if (len > (size_t)-1 - c->out_len) {
        return OI_ERR_NOMEM;
    }
    size_t needed = c->out_len + len;
    if (needed > c->out_cap) {
        size_t new_cap = c->out_cap == 0 ? 4096 : c->out_cap;
        while (new_cap < needed) {
            if (new_cap > (size_t)-1 / 2) {
                return OI_ERR_NOMEM;
            }
            new_cap *= 2;
        }
        char *nb = realloc(c->out_buf, new_cap);
        if (nb == NULL) {
            return OI_ERR_NOMEM;
        }
        c->out_buf = nb;
        c->out_cap = new_cap;
    }

    memcpy(c->out_buf + c->out_len, data, len);
    c->out_len += len;
    update_interest(c);
    return OI_OK;
}
