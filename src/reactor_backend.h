#ifndef OI_REACTOR_BACKEND_H
#define OI_REACTOR_BACKEND_H

#include "oi/status.h"

/*
 * Internal seam between the generic reactor (fd table, callback dispatch,
 * run/stop bookkeeping) and an OS-specific polling mechanism. A backend
 * knows nothing about oi_reactor_cb/user_data — it only tracks fd ->
 * interest and reports which fds are ready. Exactly one backend is
 * compiled in per platform (reactor_epoll.c on Linux; reactor_kqueue.c on
 * macOS is a best-effort, not-tested-here counterpart).
 */

typedef struct oi_reactor_backend oi_reactor_backend;

/* Mirrors OI_EV_READ/OI_EV_WRITE from reactor.h; backend.c does not
 * include reactor.h to keep the seam one-directional, so the bit values
 * are re-asserted equal at compile time in reactor.c. */
#define OI_BACKEND_EV_READ (1 << 0)
#define OI_BACKEND_EV_WRITE (1 << 1)
#define OI_BACKEND_EV_ERROR (1 << 2)
#define OI_BACKEND_EV_HUP (1 << 3)

/* Hard cap on events returned by one oi_reactor_backend_wait call. */
#define OI_REACTOR_MAX_EVENTS 64

oi_reactor_backend *oi_reactor_backend_create(void);
void oi_reactor_backend_destroy(oi_reactor_backend *b);

oi_status oi_reactor_backend_add(oi_reactor_backend *b, int fd, int interest);
oi_status oi_reactor_backend_modify(oi_reactor_backend *b, int fd,
                                     int interest);
oi_status oi_reactor_backend_remove(oi_reactor_backend *b, int fd);

/*
 * Waits up to timeout_ms (-1 = indefinite) for ready fds. On success,
 * fills out_fds/out_revents (parallel arrays, caller-owned, capacity
 * max_events) and returns the count via *out_n (0 on timeout). Returns
 * OI_ERR_IO on a hard backend failure.
 */
oi_status oi_reactor_backend_wait(oi_reactor_backend *b, int timeout_ms,
                                   int *out_fds, int *out_revents,
                                   int max_events, int *out_n);

#endif /* OI_REACTOR_BACKEND_H */
