#ifndef OI_REACTOR_H
#define OI_REACTOR_H

#include "oi/status.h"

typedef struct oi_reactor oi_reactor;
typedef struct oi_reactor_timer oi_reactor_timer;

enum {
    OI_EV_READ = 1 << 0,
    OI_EV_WRITE = 1 << 1
};

enum {
    OI_EV_ERROR = 1 << 2,
    OI_EV_HUP = 1 << 3
};

typedef void (*oi_reactor_cb)(oi_reactor *r, int fd, int revents,
                               void *user_data);

oi_reactor *oi_reactor_create(void);

void oi_reactor_destroy(oi_reactor *r);

oi_status oi_reactor_add(oi_reactor *r, int fd, int interest,
                          oi_reactor_cb cb, void *user_data);

oi_status oi_reactor_modify(oi_reactor *r, int fd, int interest);

oi_status oi_reactor_remove(oi_reactor *r, int fd);

int oi_reactor_step(oi_reactor *r, int timeout_ms, oi_status *out_status);

oi_status oi_reactor_run(oi_reactor *r);

void oi_reactor_stop(oi_reactor *r);

int oi_reactor_fd(const oi_reactor *r);
int oi_reactor_timeout_ms(const oi_reactor *r);

typedef void (*oi_reactor_timer_cb)(oi_reactor *r, void *user_data);

oi_status oi_reactor_timer_start(oi_reactor *r, int timeout_ms,
                                  oi_reactor_timer_cb cb, void *user_data,
                                  oi_reactor_timer **out_timer);

void oi_reactor_timer_cancel(oi_reactor_timer *timer);

#endif
