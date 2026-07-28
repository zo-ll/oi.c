#define _GNU_SOURCE

#include "reactor_backend.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>

struct native_watch {
    uintptr_t handle;
    int fd;
    struct native_watch *next;
};

struct oi_reactor_backend {
    int epfd;
    uintptr_t next_handle;
    struct native_watch *watches;
};

oi_reactor_backend *oi_reactor_backend_create(void) {
    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        return NULL;
    }

    oi_reactor_backend *b = malloc(sizeof *b);
    if (b == NULL) {
        close(epfd);
        return NULL;
    }
    b->epfd = epfd;
    b->next_handle = 1;
    b->watches = NULL;
    return b;
}

void oi_reactor_backend_destroy(oi_reactor_backend *b) {
    if (b == NULL) {
        return;
    }
    struct native_watch *watch = b->watches;
    while (watch != NULL) {
        struct native_watch *next = watch->next;
        close(watch->fd);
        free(watch);
        watch = next;
    }
    close(b->epfd);
    free(b);
}

static oi_status native_watch_add(oi_reactor_backend *b, int fd,
                                   uint64_t token, uintptr_t *out_handle) {
    struct epoll_event event;
    memset(&event, 0, sizeof event);
    event.events = EPOLLIN;
    event.data.u64 = token;
    if (epoll_ctl(b->epfd, EPOLL_CTL_ADD, fd, &event) != 0) {
        close(fd);
        return OI_ERR_IO;
    }
    struct native_watch *watch = malloc(sizeof *watch);
    if (watch == NULL) {
        epoll_ctl(b->epfd, EPOLL_CTL_DEL, fd, &event);
        close(fd);
        return OI_ERR_NOMEM;
    }
    uintptr_t handle = b->next_handle++;
    if (handle == 0) {
        handle = b->next_handle++;
    }
    watch->handle = handle;
    watch->fd = fd;
    watch->next = b->watches;
    b->watches = watch;
    *out_handle = handle;
    return OI_OK;
}

static oi_status native_watch_remove(oi_reactor_backend *b,
                                      uintptr_t handle) {
    struct native_watch **link = &b->watches;
    while (*link != NULL && (*link)->handle != handle) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        return OI_ERR_NOTFOUND;
    }
    struct native_watch *watch = *link;
    *link = watch->next;
    struct epoll_event event;
    memset(&event, 0, sizeof event);
    epoll_ctl(b->epfd, EPOLL_CTL_DEL, watch->fd, &event);
    close(watch->fd);
    free(watch);
    return OI_OK;
}

oi_status oi_reactor_backend_timer_add(oi_reactor_backend *b, int timeout_ms,
                                        uint64_t token,
                                        uintptr_t *out_handle) {
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        return OI_ERR_IO;
    }
    struct itimerspec spec;
    memset(&spec, 0, sizeof spec);
    spec.it_value.tv_sec = timeout_ms / 1000;
    spec.it_value.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    if (timerfd_settime(fd, 0, &spec, NULL) != 0) {
        close(fd);
        return OI_ERR_IO;
    }
    return native_watch_add(b, fd, token, out_handle);
}

oi_status oi_reactor_backend_timer_remove(oi_reactor_backend *b,
                                           uintptr_t handle) {
    return native_watch_remove(b, handle);
}

oi_status oi_reactor_backend_process_add(oi_reactor_backend *b, pid_t pid,
                                          uint64_t token,
                                          uintptr_t *out_handle) {
#ifdef SYS_pidfd_open
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) {
        return OI_ERR_IO;
    }
    return native_watch_add(b, fd, token, out_handle);
#else
    (void)b;
    (void)pid;
    (void)token;
    (void)out_handle;
    return OI_ERR_IO;
#endif
}

oi_status oi_reactor_backend_process_remove(oi_reactor_backend *b,
                                             uintptr_t handle) {
    return native_watch_remove(b, handle);
}

static uint32_t interest_to_epoll(int interest) {
    uint32_t events = 0;
    if (interest & OI_BACKEND_EV_READ) {
        events |= EPOLLIN;
    }
    if (interest & OI_BACKEND_EV_WRITE) {
        events |= EPOLLOUT;
    }
    return events;
}

oi_status oi_reactor_backend_add(oi_reactor_backend *b, int fd,
                                  int interest, uint64_t token) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = interest_to_epoll(interest);
    ev.data.u64 = token;

    if (epoll_ctl(b->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        return errno == EEXIST ? OI_ERR_EXISTS : OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_reactor_backend_modify(oi_reactor_backend *b, int fd,
                                     int interest, uint64_t token) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = interest_to_epoll(interest);
    ev.data.u64 = token;

    if (epoll_ctl(b->epfd, EPOLL_CTL_MOD, fd, &ev) != 0) {
        return errno == ENOENT ? OI_ERR_NOTFOUND : OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_reactor_backend_remove(oi_reactor_backend *b, int fd) {
    /* Pre-2.6.9 kernels required a non-NULL event pointer for
     * EPOLL_CTL_DEL even though it's ignored; pass a zeroed dummy. */
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);

    if (epoll_ctl(b->epfd, EPOLL_CTL_DEL, fd, &ev) != 0) {
        return errno == ENOENT ? OI_ERR_NOTFOUND : OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_reactor_backend_wait(oi_reactor_backend *b, int timeout_ms,
                                   uint64_t *out_tokens, int *out_revents,
                                   int max_events, int *out_n) {
    struct epoll_event evs[OI_REACTOR_MAX_EVENTS];
    int cap = max_events < OI_REACTOR_MAX_EVENTS ? max_events
                                                   : OI_REACTOR_MAX_EVENTS;
    int n;

    do {
        n = epoll_wait(b->epfd, evs, cap, timeout_ms);
    } while (n < 0 && errno == EINTR);

    if (n < 0) {
        return OI_ERR_IO;
    }

    for (int i = 0; i < n; i++) {
        int revents = 0;
        if (evs[i].events & EPOLLIN) {
            revents |= OI_BACKEND_EV_READ;
        }
        if (evs[i].events & EPOLLOUT) {
            revents |= OI_BACKEND_EV_WRITE;
        }
        if (evs[i].events & EPOLLERR) {
            revents |= OI_BACKEND_EV_ERROR;
        }
        if (evs[i].events & EPOLLHUP) {
            revents |= OI_BACKEND_EV_HUP;
        }
        out_tokens[i] = evs[i].data.u64;
        out_revents[i] = revents;
    }

    *out_n = n;
    return OI_OK;
}
