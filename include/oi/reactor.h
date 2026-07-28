#ifndef OI_REACTOR_H
#define OI_REACTOR_H

#include "oi/status.h"

/*
 * Single-threaded event loop multiplexing non-blocking fds (LLM API
 * sockets, tool subprocess pipes). No locking: every oi_reactor and the fds
 * registered on it must be used from one thread only.
 */

typedef struct oi_reactor oi_reactor;
typedef struct oi_reactor_timer oi_reactor_timer;

/* Interest bits, ORed together when registering an fd. */
enum {
    OI_EV_READ = 1 << 0,
    OI_EV_WRITE = 1 << 1
};

/*
 * Delivered when an interest fires. `revents` is the subset of the fd's
 * registered interest that is ready, plus OI_EV_ERROR/OI_EV_HUP which are
 * always reported regardless of registered interest.
 */
enum {
    OI_EV_ERROR = 1 << 2,
    OI_EV_HUP = 1 << 3
};

/*
 * `revents` is the OI_EV_* bitmask describing what fired. `user_data` is
 * the pointer passed to oi_reactor_add. The callback does not own `fd`;
 * closing it is the caller's responsibility (typically after calling
 * oi_reactor_remove).
 */
typedef void (*oi_reactor_cb)(oi_reactor *r, int fd, int revents,
                               void *user_data);

/* Returns NULL on allocation or backend-init failure. */
oi_reactor *oi_reactor_create(void);

/* NULL-safe. Does not close any fds still registered. */
void oi_reactor_destroy(oi_reactor *r);

/*
 * Registers `fd` for the given OI_EV_READ/OI_EV_WRITE interest. `fd` must
 * not already be registered on `r` (OI_ERR_EXISTS). The fd itself is not
 * duplicated or owned by the reactor; the caller must keep it open and
 * valid until oi_reactor_remove.
 */
oi_status oi_reactor_add(oi_reactor *r, int fd, int interest,
                          oi_reactor_cb cb, void *user_data);

/* Changes the registered interest for an fd already added. */
oi_status oi_reactor_modify(oi_reactor *r, int fd, int interest);

/* Deregisters `fd`. Safe to call from within that fd's own callback. */
oi_status oi_reactor_remove(oi_reactor *r, int fd);

/*
 * Blocks waiting for I/O (or `timeout_ms`, -1 for indefinite) and
 * dispatches ready callbacks once. Returns the number of callbacks
 * dispatched (>= 0), or a negative-mapped error via *out_status. Safe to
 * call oi_reactor_add/remove/modify from within a callback invoked during
 * this step.
 */
int oi_reactor_step(oi_reactor *r, int timeout_ms, oi_status *out_status);

/*
 * Steps until oi_reactor_stop is called or no fds remain registered.
 * Returns OI_OK on a clean stop, or the first step error encountered.
 */
oi_status oi_reactor_run(oi_reactor *r);

/* Requests oi_reactor_run return after the current step. */
void oi_reactor_stop(oi_reactor *r);

typedef void (*oi_reactor_timer_cb)(oi_reactor *r, void *user_data);

/*
 * Registers a one-shot monotonic timer. After it fires, the callback runs
 * once and the timer stops consuming reactor interest, but its handle
 * remains caller-owned until oi_reactor_timer_cancel. The callback may
 * cancel its own timer. `timeout_ms` must be positive.
 */
oi_status oi_reactor_timer_start(oi_reactor *r, int timeout_ms,
                                  oi_reactor_timer_cb cb, void *user_data,
                                  oi_reactor_timer **out_timer);

/* Cancels and frees a timer, whether still pending or already fired.
 * NULL-safe. Must be called before destroying its reactor. */
void oi_reactor_timer_cancel(oi_reactor_timer *timer);

#endif /* OI_REACTOR_H */
