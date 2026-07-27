#include "reactor_backend.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

struct oi_reactor_backend {
    int epfd;
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
    return b;
}

void oi_reactor_backend_destroy(oi_reactor_backend *b) {
    if (b == NULL) {
        return;
    }
    close(b->epfd);
    free(b);
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
                                  int interest) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = interest_to_epoll(interest);
    ev.data.fd = fd;

    if (epoll_ctl(b->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        return errno == EEXIST ? OI_ERR_EXISTS : OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_reactor_backend_modify(oi_reactor_backend *b, int fd,
                                     int interest) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = interest_to_epoll(interest);
    ev.data.fd = fd;

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
                                   int *out_fds, int *out_revents,
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
        out_fds[i] = evs[i].data.fd;
        out_revents[i] = revents;
    }

    *out_n = n;
    return OI_OK;
}
