#include "llm_conn.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "compat.h"

/* A connect() that has no answer after this time moves to the next
 * address, so a dead route (often IPv6) does not use the full request
 * deadline. The last address has no such timer: it gets the remainder
 * of the request deadline. */
#define OI_LLM_CONN_ADDRESS_TIMEOUT_MS 3000

enum conn_state {
    CS_RESOLVING,
    CS_TCP_CONNECTING,
    CS_TLS_HANDSHAKE,
    CS_ESTABLISHED
};

struct resolver_job {
    pthread_mutex_t mutex;
    pthread_t thread;
    struct oi_llm_conn *conn; /* protected by mutex; NULL once abandoned */
    char *host;
    char port[6];
    int notify_write;
    struct addrinfo *result;
    int gai_error;
    int finished;
};

struct oi_llm_conn {
    oi_reactor *reactor;
    int fd;
    enum conn_state state;
    struct resolver_job *resolver;
    struct addrinfo *addresses;
    struct addrinfo *next_address;
    oi_reactor_timer *address_timer;

    int use_tls;
    char *host;
    unsigned short port;
    int connect_errno; /* why the last address failed; 0 = none tried */
    char cause[256];   /* see oi_llm_conn_error_cause */
    char *ca_file; /* NULL = system default trust store */
    SSL_CTX *ssl_ctx;
    SSL *ssl;

    struct oi_llm_conn_callbacks cbs;
    void *user_data;

    char *out_buf;
    size_t out_len;
    size_t out_off;
    size_t out_cap;
    int read_needs_write;
    int write_needs_read;

    /* Set by the top-level reactor callback before invoking any of
     * *cbs, so a reentrant oi_llm_conn_close() from within a callback
     * can signal back to still-executing internal code that `c` is
     * gone. See the call-site pattern in fail_conn/read_available. */
    int *destroyed_flag;
};

static void on_fd_event(oi_reactor *r, int fd, int revents, void *user_data);

static oi_status set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static oi_status set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static oi_status set_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                      sizeof enabled) == 0
               ? OI_OK
               : OI_ERR_IO;
#else
    (void)fd;
    return OI_OK;
#endif
}

static void update_interest(oi_llm_conn *c) {
    int interest = OI_EV_READ;
    if (c->read_needs_write ||
        (c->out_off < c->out_len && !c->write_needs_read)) {
        interest |= OI_EV_WRITE;
    }
    oi_reactor_modify(c->reactor, c->fd, interest);
}

void oi_llm_conn_close(oi_llm_conn *c) {
    if (c == NULL) {
        return;
    }
    oi_reactor_timer_cancel(c->address_timer);
    if (c->fd >= 0) {
        oi_reactor_remove(c->reactor, c->fd);
        close(c->fd);
    }
    if (c->resolver != NULL) {
        struct resolver_job *job = c->resolver;
        pthread_t resolver_thread;
        int finished;
        pthread_mutex_lock(&job->mutex);
        resolver_thread = job->thread;
        finished = job->finished;
        if (!finished) {
            job->conn = NULL;
        }
        pthread_mutex_unlock(&job->mutex);
        if (finished) {
            pthread_join(resolver_thread, NULL);
            close(job->notify_write);
            freeaddrinfo(job->result);
            pthread_mutex_destroy(&job->mutex);
            free(job->host);
            free(job);
        } else {
            pthread_detach(resolver_thread);
        }
    }
    if (c->ssl) {
        SSL_free(c->ssl);
    }
    if (c->ssl_ctx) {
        SSL_CTX_free(c->ssl_ctx);
    }
    /* The request head holds the Authorization header. */
    oi_wipe(c->out_buf, c->out_cap);
    free(c->out_buf);
    freeaddrinfo(c->addresses);
    free(c->host);
    free(c->ca_file);
    if (c->destroyed_flag) {
        *c->destroyed_flag = 1;
    }
    free(c);
}

static void set_cause(oi_llm_conn *c, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(c->cause, sizeof c->cause, format, args);
    va_end(args);
}

/* Names the TLS failure: an untrusted certificate, an OpenSSL error, or
 * the socket errno under SSL_ERROR_SYSCALL. */
static void set_tls_cause(oi_llm_conn *c, const char *what) {
    long verify = c->ssl != NULL ? SSL_get_verify_result(c->ssl) : X509_V_OK;
    unsigned long error = ERR_peek_last_error();
    if (verify != X509_V_OK) {
        set_cause(c, "TLS certificate of %s is not trusted: %s", c->host,
                  X509_verify_cert_error_string(verify));
    } else if (error != 0) {
        char text[160];
        ERR_error_string_n(error, text, sizeof text);
        set_cause(c, "TLS %s with %s failed: %s", what, c->host, text);
    } else if (errno != 0) {
        set_cause(c, "TLS %s with %s failed: %s", what, c->host,
                  strerror(errno));
    } else {
        set_cause(c, "TLS %s with %s failed", what, c->host);
    }
    ERR_clear_error();
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

/* --- socket BIO for TLS --- */

/*
 * OpenSSL's socket BIO writes with write(), and a write to a closed peer
 * raises SIGPIPE in the whole process. This BIO sends with MSG_NOSIGNAL,
 * so SSL_write gets EPIPE and the caller's SIGPIPE disposition is never
 * touched. The BIO data is the socket fd; the connection owns the fd.
 */
static BIO_METHOD *nosigpipe_method;
static pthread_once_t nosigpipe_once = PTHREAD_ONCE_INIT;

static int nosigpipe_write(BIO *bio, const char *data, int len) {
    int fd = (int)(intptr_t)BIO_get_data(bio);
    ssize_t n;
    BIO_clear_retry_flags(bio);
    do {
        n = send(fd, data, (size_t)len, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        BIO_set_retry_write(bio);
    }
    return (int)n;
}

static int nosigpipe_read(BIO *bio, char *buf, int len) {
    int fd = (int)(intptr_t)BIO_get_data(bio);
    ssize_t n;
    BIO_clear_retry_flags(bio);
    do {
        n = recv(fd, buf, (size_t)len, 0);
    } while (n < 0 && errno == EINTR);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        BIO_set_retry_read(bio);
    } else if (n == 0) {
        BIO_set_flags(bio, BIO_FLAGS_IN_EOF);
    }
    return (int)n;
}

static long nosigpipe_ctrl(BIO *bio, int cmd, long num, void *ptr) {
    (void)num;
    (void)ptr;
    switch (cmd) {
    case BIO_CTRL_FLUSH:
        return 1;
    case BIO_CTRL_EOF:
        return BIO_test_flags(bio, BIO_FLAGS_IN_EOF) != 0;
    default:
        return 0;
    }
}

static void make_nosigpipe_method(void) {
    BIO_METHOD *method = BIO_meth_new(
        BIO_get_new_index() | BIO_TYPE_SOURCE_SINK, "oi socket");
    if (method == NULL || !BIO_meth_set_write(method, nosigpipe_write) ||
        !BIO_meth_set_read(method, nosigpipe_read) ||
        !BIO_meth_set_ctrl(method, nosigpipe_ctrl)) {
        BIO_meth_free(method);
        return;
    }
    nosigpipe_method = method;
}

/* Attaches `fd` to `ssl` through the MSG_NOSIGNAL BIO. */
static oi_status set_nosigpipe_bio(SSL *ssl, int fd) {
    pthread_once(&nosigpipe_once, make_nosigpipe_method);
    if (nosigpipe_method == NULL) {
        return OI_ERR_NOMEM;
    }
    BIO *bio = BIO_new(nosigpipe_method);
    if (bio == NULL) {
        return OI_ERR_NOMEM;
    }
    BIO_set_data(bio, (void *)(intptr_t)fd);
    BIO_set_init(bio, 1);
    SSL_set_bio(ssl, bio, bio); /* one reference, for both directions */
    return OI_OK;
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
            c->read_needs_write = 0;
            return rc;
        }
        int err = SSL_get_error(c->ssl, rc);
        if (err == SSL_ERROR_ZERO_RETURN) {
            c->read_needs_write = 0;
            return 0;
        }
        if (err == SSL_ERROR_WANT_READ) {
            c->read_needs_write = 0;
            *would_block = 1;
            return -1;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            c->read_needs_write = 1;
            *would_block = 1;
            return -1;
        }
        set_tls_cause(c, "read");
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
    set_cause(c, "read from %s failed: %s", c->host, strerror(errno));
    return -2;
}

static long conn_raw_write(oi_llm_conn *c, const void *buf, size_t len,
                            int *would_block) {
    *would_block = 0;
    if (c->use_tls) {
        int rc = SSL_write(c->ssl, buf, (int)len);
        int err = rc > 0 ? SSL_ERROR_NONE : SSL_get_error(c->ssl, rc);
        if (rc > 0) {
            c->write_needs_read = 0;
            return rc;
        }
        if (err == SSL_ERROR_WANT_READ) {
            c->write_needs_read = 1;
            *would_block = 1;
            return -1;
        }
        if (err == SSL_ERROR_WANT_WRITE) {
            c->write_needs_read = 0;
            *would_block = 1;
            return -1;
        }
        set_tls_cause(c, "write");
        return -2;
    }

#ifdef MSG_NOSIGNAL
    ssize_t rc = send(c->fd, buf, len, MSG_NOSIGNAL);
#else
    ssize_t rc = send(c->fd, buf, len, 0);
#endif
    if (rc >= 0) {
        return rc;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *would_block = 1;
        return -1;
    }
    set_cause(c, "write to %s failed: %s", c->host, strerror(errno));
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
            set_cause(c, "%s closed the connection", c->host);
            fail_conn(c, OI_ERR_CLOSED);
            return;
        }
        if (would_block) {
            update_interest(c);
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
    if (!SSL_CTX_set_min_proto_version(c->ssl_ctx, TLS1_2_VERSION)) {
        return OI_ERR_IO;
    }
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
    if (set_nosigpipe_bio(c->ssl, c->fd) != OI_OK) {
        return OI_ERR_IO;
    }

    SSL_set_connect_state(c->ssl);
    X509_VERIFY_PARAM *param = SSL_get0_param(c->ssl);
    if (param == NULL) {
        return OI_ERR_IO;
    }

    unsigned char address[sizeof(struct in6_addr)];
    int address_family = 0;
    size_t address_len = 0;
    if (inet_pton(AF_INET, c->host, address) == 1) {
        address_family = AF_INET;
        address_len = sizeof(struct in_addr);
    } else if (inet_pton(AF_INET6, c->host, address) == 1) {
        address_family = AF_INET6;
        address_len = sizeof(struct in6_addr);
    }

    if (address_family != 0) {
        if (!X509_VERIFY_PARAM_set1_ip(param, address, address_len)) {
            return OI_ERR_IO;
        }
    } else {
        if (!SSL_set_tlsext_host_name(c->ssl, c->host) ||
            !X509_VERIFY_PARAM_set1_host(param, c->host, 0)) {
            return OI_ERR_IO;
        }
    }

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
        set_tls_cause(c, "handshake");
        fail_conn(c, OI_ERR_IO);
    }
}

/* --- reactor dispatch --- */

static void on_address_timeout(oi_reactor *r, void *user_data);

/* Starts a non-blocking connect to the next address that accepts one and
 * registers it for OI_EV_WRITE. */
static oi_status connect_next_address(oi_llm_conn *c) {
    int fd = -1;
    while (c->next_address != NULL) {
        struct addrinfo *ai = c->next_address;
        c->next_address = ai->ai_next;
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            c->connect_errno = errno;
            continue;
        }
        if (set_cloexec(fd) != OI_OK || set_nonblocking(fd) != OI_OK ||
            set_no_sigpipe(fd) != OI_OK) {
            c->connect_errno = errno;
            close(fd);
            fd = -1;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0 || errno == EINPROGRESS) {
            break;
        }
        c->connect_errno = errno;
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        set_cause(c, "could not connect to %s port %u: %s", c->host,
                  (unsigned)c->port,
                  c->connect_errno != 0 ? strerror(c->connect_errno)
                                        : "no address");
        return OI_ERR_IO;
    }
    c->fd = fd;
    c->state = CS_TCP_CONNECTING;
    oi_status st = oi_reactor_add(c->reactor, fd, OI_EV_WRITE, on_fd_event, c);
    if (st != OI_OK) {
        close(fd);
        c->fd = -1;
        return st;
    }
    if (c->next_address != NULL) {
        st = oi_reactor_timer_start(c->reactor, OI_LLM_CONN_ADDRESS_TIMEOUT_MS,
                                    on_address_timeout, c, &c->address_timer);
    }
    return st;
}

static void abandon_address(oi_llm_conn *c) {
    oi_reactor_timer_cancel(c->address_timer);
    c->address_timer = NULL;
    oi_reactor_remove(c->reactor, c->fd);
    close(c->fd);
    c->fd = -1;
}

static void on_address_timeout(oi_reactor *r, void *user_data) {
    (void)r;
    oi_llm_conn *c = user_data;
    int destroyed = 0;
OI_DIAG_PUSH_IGNORE_DANGLING
    c->destroyed_flag = &destroyed;
OI_DIAG_POP
    c->connect_errno = ETIMEDOUT;
    abandon_address(c);
    oi_status st = connect_next_address(c);
    if (st != OI_OK) {
        fail_conn(c, st);
    }
    if (!destroyed) {
        c->destroyed_flag = NULL;
    }
}

static void resolver_job_free(struct resolver_job *job) {
    close(job->notify_write);
    freeaddrinfo(job->result);
    pthread_mutex_destroy(&job->mutex);
    free(job->host);
    free(job);
}

static void *resolver_main(void *ud) {
    struct resolver_job *job = ud;
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *result = NULL;
    int gai_error = getaddrinfo(job->host, job->port, &hints, &result);

    pthread_mutex_lock(&job->mutex);
    if (job->conn == NULL) {
        pthread_mutex_unlock(&job->mutex);
        freeaddrinfo(result);
        resolver_job_free(job);
        return NULL;
    }
    job->result = result;
    job->gai_error = gai_error;
    job->finished = 1;
    int notify_write = job->notify_write;
    pthread_mutex_unlock(&job->mutex);

    char byte = 1;
    ssize_t n;
    do {
        n = write(notify_write, &byte, 1);
    } while (n < 0 && errno == EINTR);
    return NULL;
}

static void handle_resolver_ready(oi_llm_conn *c) {
    char byte;
    ssize_t n;
    do {
        n = read(c->fd, &byte, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
        set_cause(c, "address lookup for %s failed", c->host);
        fail_conn(c, OI_ERR_IO);
        return;
    }

    oi_reactor_remove(c->reactor, c->fd);
    close(c->fd);
    c->fd = -1;

    struct resolver_job *job = c->resolver;
    pthread_join(job->thread, NULL);
    c->resolver = NULL;
    struct addrinfo *result = job->result;
    int gai_error = job->gai_error;
    job->result = NULL;
    resolver_job_free(job);

    c->addresses = result;
    c->next_address = result;
    oi_status st;
    if (gai_error != 0) {
        set_cause(c, "could not resolve %s: %s", c->host,
                  gai_strerror(gai_error));
        st = OI_ERR_IO;
    } else {
        st = connect_next_address(c);
    }
    if (st != OI_OK) {
        fail_conn(c, st);
    }
}

static void handle_tcp_connecting(oi_llm_conn *c) {
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
        err != 0) {
        c->connect_errno = err != 0 ? err : errno;
        abandon_address(c);
        oi_status st = connect_next_address(c);
        if (st != OI_OK) {
            fail_conn(c, st);
        }
        return;
    }
    oi_reactor_timer_cancel(c->address_timer);
    c->address_timer = NULL;
    freeaddrinfo(c->addresses);
    c->addresses = NULL;
    c->next_address = NULL;

    if (c->use_tls) {
        if (start_tls_handshake(c) != OI_OK) {
            set_tls_cause(c, "setup");
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
    /* An error or hangup with no other bit leaves the descriptor ready.
     * A read reports the error or the EOF and stops the connection. */
    if (revents & (OI_EV_ERROR | OI_EV_HUP)) {
        revents |= OI_EV_READ;
    }
    if ((revents & OI_EV_READ) && c->write_needs_read &&
        c->out_off < c->out_len) {
        if (flush_write_queue(c) != OI_OK) {
            return;
        }
    }
    if ((revents & OI_EV_WRITE) && c->read_needs_write) {
        read_available(c);
        return;
    }
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
    case CS_RESOLVING:
        handle_resolver_ready(c);
        break;
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

    oi_llm_conn *c = calloc(1, sizeof *c);
    if (c == NULL) {
        return OI_ERR_NOMEM;
    }
    c->reactor = r;
    c->fd = -1;
    c->state = CS_RESOLVING;
    c->use_tls = use_tls;
    c->port = port;
    c->host = strdup(host);
    c->ca_file = NULL;
    c->ssl_ctx = NULL;
    c->ssl = NULL;
    c->cbs = *cbs;
    c->user_data = user_data;
    c->out_buf = NULL;
    c->out_len = 0;
    c->out_off = 0;
    c->out_cap = 0;
    c->read_needs_write = 0;
    c->write_needs_read = 0;
    c->destroyed_flag = NULL;

    if (c->host == NULL) {
        free(c);
        return OI_ERR_NOMEM;
    }
    if (ca_file) {
        c->ca_file = strdup(ca_file);
        if (c->ca_file == NULL) {
            free(c->host);
            free(c);
            return OI_ERR_NOMEM;
        }
    }

    int notify[2] = {-1, -1};
    if (pipe(notify) != 0 || set_nonblocking(notify[0]) != OI_OK ||
        set_cloexec(notify[0]) != OI_OK || set_cloexec(notify[1]) != OI_OK) {
        if (notify[0] >= 0) {
            close(notify[0]);
        }
        if (notify[1] >= 0) {
            close(notify[1]);
        }
        free(c->host);
        free(c->ca_file);
        free(c);
        return OI_ERR_IO;
    }

    struct resolver_job *job = calloc(1, sizeof *job);
    if (job == NULL) {
        close(notify[0]);
        close(notify[1]);
        free(c->host);
        free(c->ca_file);
        free(c);
        return OI_ERR_NOMEM;
    }
    job->host = strdup(host);
    if (job->host == NULL || pthread_mutex_init(&job->mutex, NULL) != 0) {
        free(job->host);
        free(job);
        close(notify[0]);
        close(notify[1]);
        free(c->host);
        free(c->ca_file);
        free(c);
        return OI_ERR_NOMEM;
    }
    snprintf(job->port, sizeof job->port, "%u", (unsigned)port);
    job->conn = c;
    job->notify_write = notify[1];
    c->resolver = job;
    c->fd = notify[0];

    oi_status st =
        oi_reactor_add(r, c->fd, OI_EV_READ, on_fd_event, c);
    if (st != OI_OK) {
        pthread_mutex_destroy(&job->mutex);
        free(job->host);
        free(job);
        close(notify[0]);
        close(notify[1]);
        free(c->host);
        free(c->ca_file);
        free(c);
        return st;
    }
    if (pthread_create(&job->thread, NULL, resolver_main, job) != 0) {
        oi_reactor_remove(r, c->fd);
        close(notify[0]);
        close(notify[1]);
        pthread_mutex_destroy(&job->mutex);
        free(job->host);
        free(job);
        free(c->host);
        free(c->ca_file);
        free(c);
        return OI_ERR_IO;
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
        /* Not realloc: a moved block would keep a copy of the
         * Authorization header in freed memory. */
        char *nb = malloc(new_cap);
        if (nb == NULL) {
            return OI_ERR_NOMEM;
        }
        if (c->out_buf != NULL) {
            memcpy(nb, c->out_buf, c->out_len);
            oi_wipe(c->out_buf, c->out_cap);
            free(c->out_buf);
        }
        c->out_buf = nb;
        c->out_cap = new_cap;
    }

    memcpy(c->out_buf + c->out_len, data, len);
    c->out_len += len;
    update_interest(c);
    return OI_OK;
}

const char *oi_llm_conn_error_cause(const oi_llm_conn *c) {
    return c != NULL && c->cause[0] != '\0' ? c->cause : NULL;
}
