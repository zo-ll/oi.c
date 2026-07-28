#ifndef OI_REACTOR_BACKEND_H
#define OI_REACTOR_BACKEND_H

#include <stdint.h>
#include <sys/types.h>

#include "oi/status.h"

/*
 * Internal seam between the generic reactor (fd table, callback dispatch,
 * run/stop bookkeeping) and an OS-specific polling mechanism. A backend
 * knows nothing about oi_reactor_cb/user_data — it only tracks fd,
 * interest, and an opaque registration token reported with readiness.
 * reactor_epoll.c is the only backend; the project targets Linux.
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

oi_status oi_reactor_backend_add(oi_reactor_backend *b, int fd, int interest,
                                  uint64_t token);
oi_status oi_reactor_backend_modify(oi_reactor_backend *b, int fd,
                                     int interest, uint64_t token);
oi_status oi_reactor_backend_remove(oi_reactor_backend *b, int fd);

/* Native one-shot timer and child-exit watches. `out_handle` is an opaque
 * backend-owned identifier used only to cancel the watch. */
oi_status oi_reactor_backend_timer_add(oi_reactor_backend *b, int timeout_ms,
                                        uint64_t token,
                                        uintptr_t *out_handle);
oi_status oi_reactor_backend_timer_remove(oi_reactor_backend *b,
                                           uintptr_t handle);
oi_status oi_reactor_backend_process_add(oi_reactor_backend *b, pid_t pid,
                                          uint64_t token,
                                          uintptr_t *out_handle);
oi_status oi_reactor_backend_process_remove(oi_reactor_backend *b,
                                             uintptr_t handle);

/*
 * Waits up to timeout_ms (-1 = indefinite) for ready fds. On success,
 * fills out_tokens/out_revents (parallel arrays, caller-owned, capacity
 * max_events) and returns the count via *out_n (0 on timeout). Returns
 * OI_ERR_IO on a hard backend failure.
 */
oi_status oi_reactor_backend_wait(oi_reactor_backend *b, int timeout_ms,
                                   uint64_t *out_tokens, int *out_revents,
                                   int max_events, int *out_n);

#endif /* OI_REACTOR_BACKEND_H */
