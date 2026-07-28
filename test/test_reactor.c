#include "oi/reactor.h"
#include "test.h"

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void make_socketpair(int fds[2]) {
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
    CHECK_EQ(rc, 0);
    set_nonblocking(fds[0]);
    set_nonblocking(fds[1]);
}

/* --- create/destroy --- */

TEST(create_destroy) {
    oi_reactor *r = oi_reactor_create();
    CHECK(r != NULL);
    oi_reactor_destroy(r);
    oi_reactor_destroy(NULL); /* NULL-safe */
}

/* --- add validates args --- */

static void noop_cb(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    (void)fd;
    (void)revents;
    (void)ud;
}

TEST(add_rejects_bad_args) {
    oi_reactor *r = oi_reactor_create();
    int fds[2];
    make_socketpair(fds);

    CHECK_EQ(oi_reactor_add(NULL, fds[0], OI_EV_READ, noop_cb, NULL),
              OI_ERR_INVAL);
    CHECK_EQ(oi_reactor_add(r, -1, OI_EV_READ, noop_cb, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_reactor_add(r, fds[0], 0, noop_cb, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_reactor_add(r, fds[0], OI_EV_READ, NULL, NULL), OI_ERR_INVAL);

    close(fds[0]);
    close(fds[1]);
    oi_reactor_destroy(r);
}

/* --- duplicate add rejected --- */

TEST(add_duplicate_fails) {
    oi_reactor *r = oi_reactor_create();
    int fds[2];
    make_socketpair(fds);

    CHECK_EQ(oi_reactor_add(r, fds[0], OI_EV_READ, noop_cb, NULL), OI_OK);
    CHECK_EQ(oi_reactor_add(r, fds[0], OI_EV_READ, noop_cb, NULL),
              OI_ERR_EXISTS);

    CHECK_EQ(oi_reactor_remove(r, fds[0]), OI_OK);
    CHECK_EQ(oi_reactor_remove(r, fds[0]), OI_ERR_NOTFOUND);

    /* A valid, open fd that was simply never registered: ENOENT from the
     * backend, not EBADF, so it must map to OI_ERR_NOTFOUND too. */
    int unreg[2];
    make_socketpair(unreg);
    CHECK_EQ(oi_reactor_remove(r, unreg[0]), OI_ERR_NOTFOUND);
    close(unreg[0]);
    close(unreg[1]);

    /* A completely invalid fd (never opened) is a different backend
     * failure (EBADF), reported as an I/O error rather than NOTFOUND. */
    CHECK_EQ(oi_reactor_remove(r, 99999), OI_ERR_IO);

    close(fds[0]);
    close(fds[1]);
    oi_reactor_destroy(r);
}

/* --- readiness dispatch --- */

struct capture {
    int fired;
    int fd;
    int revents;
};

static void capture_cb(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    struct capture *c = ud;
    c->fired++;
    c->fd = fd;
    c->revents = revents;
}

TEST(dispatches_on_readable) {
    oi_reactor *r = oi_reactor_create();
    int fds[2];
    make_socketpair(fds);
    struct capture cap = {0};

    CHECK_EQ(oi_reactor_add(r, fds[0], OI_EV_READ, capture_cb, &cap), OI_OK);

    /* fds[0] not readable yet: a bounded step should time out with 0. */
    oi_status st;
    int n = oi_reactor_step(r, 10, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(n, 0);
    CHECK_EQ(cap.fired, 0);

    CHECK_EQ(write(fds[1], "hi", 2), 2);

    n = oi_reactor_step(r, 1000, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(n, 1);
    CHECK_EQ(cap.fired, 1);
    CHECK_EQ(cap.fd, fds[0]);
    CHECK((cap.revents & OI_EV_READ) != 0);

    close(fds[0]);
    close(fds[1]);
    oi_reactor_destroy(r);
}

TEST(dispatches_on_writable) {
    oi_reactor *r = oi_reactor_create();
    int fds[2];
    make_socketpair(fds);
    struct capture cap = {0};

    /* A fresh socket's send buffer is empty, so it should be immediately
     * writable. */
    CHECK_EQ(oi_reactor_add(r, fds[0], OI_EV_WRITE, capture_cb, &cap), OI_OK);

    oi_status st;
    int n = oi_reactor_step(r, 1000, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(n, 1);
    CHECK((cap.revents & OI_EV_WRITE) != 0);

    close(fds[0]);
    close(fds[1]);
    oi_reactor_destroy(r);
}

/* --- modify changes interest --- */

TEST(modify_changes_interest) {
    oi_reactor *r = oi_reactor_create();
    int fds[2];
    make_socketpair(fds);
    struct capture cap = {0};

    CHECK_EQ(oi_reactor_add(r, fds[0], OI_EV_WRITE, capture_cb, &cap), OI_OK);
    CHECK_EQ(oi_reactor_modify(r, fds[0], OI_EV_READ), OI_OK);

    /* Now only interested in READ; fds[0] has nothing to read. */
    oi_status st;
    int n = oi_reactor_step(r, 10, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(n, 0);

    int unreg[2];
    make_socketpair(unreg);
    CHECK_EQ(oi_reactor_modify(r, unreg[0], OI_EV_READ), OI_ERR_NOTFOUND);
    close(unreg[0]);
    close(unreg[1]);

    CHECK_EQ(oi_reactor_modify(r, 99999, OI_EV_READ), OI_ERR_IO);
    CHECK_EQ(oi_reactor_modify(r, fds[0], 0), OI_ERR_INVAL);

    close(fds[0]);
    close(fds[1]);
    oi_reactor_destroy(r);
}

/* --- remove during dispatch (safe reentrancy) --- */

struct remove_ctx {
    oi_reactor *r;
    int other_fd;
    int self_fired;
};

static void remove_other_cb(oi_reactor *r, int fd, int revents, void *ud) {
    (void)fd;
    (void)revents;
    struct remove_ctx *ctx = ud;
    oi_reactor_remove(r, ctx->other_fd);
}

static void mark_fired_cb(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    (void)fd;
    (void)revents;
    struct remove_ctx *ctx = ud;
    ctx->self_fired = 1;
}

TEST(remove_during_dispatch_is_safe) {
    oi_reactor *r = oi_reactor_create();
    int a[2], b[2];
    make_socketpair(a);
    make_socketpair(b);

    struct remove_ctx ctx_a = {r, b[0], 0};
    struct remove_ctx ctx_b = {r, a[0], 0};

    /* Both a[0] and b[0] become readable; a's callback removes b before
     * b's callback would run. Must not crash or double-dispatch. */
    CHECK_EQ(write(a[1], "x", 1), 1);
    CHECK_EQ(write(b[1], "x", 1), 1);

    CHECK_EQ(oi_reactor_add(r, a[0], OI_EV_READ, remove_other_cb, &ctx_a),
              OI_OK);
    CHECK_EQ(oi_reactor_add(r, b[0], OI_EV_READ, mark_fired_cb, &ctx_b),
              OI_OK);

    oi_status st;
    int n = oi_reactor_step(r, 1000, &st);
    CHECK_EQ(st, OI_OK);
    /* Either 1 (b already removed before its turn) or 2 (b's turn came
     * first) is acceptable; what must not happen is a crash. */
    CHECK(n == 1 || n == 2);

    close(a[0]);
    close(a[1]);
    close(b[0]);
    close(b[1]);
    oi_reactor_destroy(r);
}

struct reuse_ctx {
    int victim_fd;
    int replacement_source;
    int old_fired;
    int replacement_fired;
};

static void replacement_cb(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    (void)fd;
    (void)revents;
    struct reuse_ctx *ctx = ud;
    ctx->replacement_fired++;
}

static void old_victim_cb(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    (void)fd;
    (void)revents;
    struct reuse_ctx *ctx = ud;
    ctx->old_fired++;
}

static void replace_other_fd_cb(oi_reactor *r, int fd, int revents,
                                 void *ud) {
    (void)revents;
    struct reuse_ctx *ctx = ud;
    CHECK_EQ(oi_reactor_remove(r, fd), OI_OK);
    CHECK_EQ(oi_reactor_remove(r, ctx->victim_fd), OI_OK);
    close(ctx->victim_fd);
    CHECK_EQ(dup2(ctx->replacement_source, ctx->victim_fd), ctx->victim_fd);
    close(ctx->replacement_source);
    ctx->replacement_source = -1;
    set_nonblocking(ctx->victim_fd);
    CHECK_EQ(oi_reactor_add(r, ctx->victim_fd, OI_EV_READ, replacement_cb,
                             ctx),
              OI_OK);
}

TEST(stale_event_does_not_target_reused_fd) {
    oi_reactor *r = oi_reactor_create();
    int controller[2], victim[2], replacement[2];
    make_socketpair(controller);
    make_socketpair(victim);
    make_socketpair(replacement);
    struct reuse_ctx ctx = {victim[0], replacement[0], 0, 0};

    CHECK_EQ(oi_reactor_add(r, controller[0], OI_EV_READ,
                             replace_other_fd_cb, &ctx),
              OI_OK);
    CHECK_EQ(oi_reactor_add(r, victim[0], OI_EV_READ, old_victim_cb, &ctx),
              OI_OK);
    /* Readiness transitions are queued in this order on epoll, making the
     * controller replace the victim while its old event is in the batch. */
    CHECK_EQ(write(controller[1], "c", 1), 1);
    CHECK_EQ(write(victim[1], "v", 1), 1);

    oi_status st;
    int n = oi_reactor_step(r, 1000, &st);
    CHECK_EQ(st, OI_OK);
    CHECK(n == 1 || n == 2);
    CHECK_EQ(ctx.replacement_fired, 0);

    CHECK_EQ(write(replacement[1], "r", 1), 1);
    CHECK_EQ(oi_reactor_step(r, 1000, &st), 1);
    CHECK_EQ(ctx.replacement_fired, 1);

    close(controller[0]);
    close(controller[1]);
    close(ctx.victim_fd);
    close(victim[1]);
    close(replacement[1]);
    oi_reactor_destroy(r);
}

/* --- HUP detection --- */

TEST(reports_hup_on_peer_close) {
    oi_reactor *r = oi_reactor_create();
    int fds[2];
    make_socketpair(fds);
    struct capture cap = {0};

    CHECK_EQ(oi_reactor_add(r, fds[0], OI_EV_READ, capture_cb, &cap), OI_OK);
    close(fds[1]);

    oi_status st;
    int n = oi_reactor_step(r, 1000, &st);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(n, 1);
    CHECK((cap.revents & (OI_EV_HUP | OI_EV_READ)) != 0);

    close(fds[0]);
    oi_reactor_destroy(r);
}

/* --- run/stop --- */

struct stop_ctx {
    oi_reactor *r;
    int count;
};

static void stop_after_first_cb(oi_reactor *r, int fd, int revents,
                                 void *ud) {
    (void)fd;
    (void)revents;
    struct stop_ctx *ctx = ud;
    ctx->count++;
    oi_reactor_stop(r);
}

TEST(run_stops_on_request) {
    oi_reactor *r = oi_reactor_create();
    int fds[2];
    make_socketpair(fds);
    struct stop_ctx ctx = {r, 0};

    CHECK_EQ(write(fds[1], "x", 1), 1);
    CHECK_EQ(oi_reactor_add(r, fds[0], OI_EV_READ, stop_after_first_cb, &ctx),
              OI_OK);

    oi_status st = oi_reactor_run(r);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(ctx.count, 1);

    close(fds[0]);
    close(fds[1]);
    oi_reactor_destroy(r);
}

TEST(run_returns_immediately_with_no_fds) {
    oi_reactor *r = oi_reactor_create();
    oi_status st = oi_reactor_run(r);
    CHECK_EQ(st, OI_OK);
    oi_reactor_destroy(r);
}

static void timer_fired(oi_reactor *r, void *ud) {
    (void)r;
    int *count = ud;
    (*count)++;
}

TEST(one_shot_timer_fires_once) {
    oi_reactor *r = oi_reactor_create();
    oi_reactor_timer *timer = NULL;
    int count = 0;
    CHECK_EQ(oi_reactor_timer_start(r, 10, timer_fired, &count, &timer),
              OI_OK);
    oi_status st;
    CHECK_EQ(oi_reactor_step(r, 1000, &st), 1);
    CHECK_EQ(st, OI_OK);
    CHECK_EQ(count, 1);
    CHECK_EQ(oi_reactor_step(r, 10, &st), 0);
    CHECK_EQ(count, 1);
    oi_reactor_timer_cancel(timer);
    oi_reactor_destroy(r);
}

TEST(cancelled_timer_does_not_fire) {
    oi_reactor *r = oi_reactor_create();
    oi_reactor_timer *timer = NULL;
    int count = 0;
    CHECK_EQ(oi_reactor_timer_start(r, 10, timer_fired, &count, &timer),
              OI_OK);
    oi_reactor_timer_cancel(timer);
    oi_status st;
    CHECK_EQ(oi_reactor_step(r, 20, &st), 0);
    CHECK_EQ(count, 0);
    oi_reactor_timer_cancel(NULL);
    CHECK_EQ(oi_reactor_timer_start(r, 0, timer_fired, &count, &timer),
              OI_ERR_INVAL);
    oi_reactor_destroy(r);
}

/* --- many fds (exercises fd-table growth) --- */

TEST(many_fds_grow_table) {
    oi_reactor *r = oi_reactor_create();
    enum { N = 40 };
    int pipes[N][2];
    struct capture caps[N];
    memset(caps, 0, sizeof caps);

    for (int i = 0; i < N; i++) {
        CHECK_EQ(pipe(pipes[i]), 0);
        set_nonblocking(pipes[i][0]);
        set_nonblocking(pipes[i][1]);
        CHECK_EQ(oi_reactor_add(r, pipes[i][0], OI_EV_READ, capture_cb,
                                  &caps[i]),
                  OI_OK);
    }

    for (int i = 0; i < N; i++) {
        CHECK_EQ(write(pipes[i][1], "z", 1), 1);
    }

    int total = 0;
    while (total < N) {
        oi_status st;
        int n = oi_reactor_step(r, 1000, &st);
        CHECK_EQ(st, OI_OK);
        CHECK(n >= 0);
        total += n;
    }
    for (int i = 0; i < N; i++) {
        CHECK_EQ(caps[i].fired, 1);
    }

    for (int i = 0; i < N; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }
    oi_reactor_destroy(r);
}

int main(void) {
    RUN(create_destroy);
    RUN(add_rejects_bad_args);
    RUN(add_duplicate_fails);
    RUN(dispatches_on_readable);
    RUN(dispatches_on_writable);
    RUN(modify_changes_interest);
    RUN(remove_during_dispatch_is_safe);
    RUN(stale_event_does_not_target_reused_fd);
    RUN(reports_hup_on_peer_close);
    RUN(run_stops_on_request);
    RUN(run_returns_immediately_with_no_fds);
    RUN(one_shot_timer_fires_once);
    RUN(cancelled_timer_does_not_fire);
    RUN(many_fds_grow_table);
    return oi_test_report();
}
