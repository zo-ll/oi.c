#ifndef OI_REACTOR_BACKEND_H
#define OI_REACTOR_BACKEND_H

#include <stdint.h>
#include <sys/types.h>

#include "oi/status.h"

typedef struct oi_reactor_backend oi_reactor_backend;

#define OI_BACKEND_EV_READ (1 << 0)
#define OI_BACKEND_EV_WRITE (1 << 1)
#define OI_BACKEND_EV_ERROR (1 << 2)
#define OI_BACKEND_EV_HUP (1 << 3)

#define OI_REACTOR_MAX_EVENTS 64

oi_reactor_backend *oi_reactor_backend_create(void);
void oi_reactor_backend_destroy(oi_reactor_backend *b);

oi_status oi_reactor_backend_add(oi_reactor_backend *b, int fd, int interest,
                                  uint64_t token);
oi_status oi_reactor_backend_modify(oi_reactor_backend *b, int fd,
                                     int interest, uint64_t token);
oi_status oi_reactor_backend_remove(oi_reactor_backend *b, int fd);

oi_status oi_reactor_backend_process_add(oi_reactor_backend *b, pid_t pid,
                                          uint64_t token,
                                          uintptr_t *out_handle);
oi_status oi_reactor_backend_process_remove(oi_reactor_backend *b,
                                             uintptr_t handle);

int oi_reactor_backend_fd(const oi_reactor_backend *b);

oi_status oi_reactor_backend_wait(oi_reactor_backend *b, int timeout_ms,
                                   uint64_t *out_tokens, int *out_revents,
                                   int max_events, int *out_n);

#endif
