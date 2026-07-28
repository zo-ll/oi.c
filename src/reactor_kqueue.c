#include "reactor_backend.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct fd_watch {
    int fd;
    int interest;
    uint64_t token;
    struct fd_watch *next;
};

struct oi_reactor_backend {
    int kq;
    uintptr_t next_timer;
    struct fd_watch *fds;
};

static int change(oi_reactor_backend *backend, uintptr_t ident, int16_t filter,
                  uint16_t flags, uint32_t fflags, intptr_t data,
                  uint64_t token) {
    struct kevent event;
    EV_SET(&event, ident, filter, flags, fflags, data,
           (void *)(uintptr_t)token);
    return kevent(backend->kq, &event, 1, NULL, 0, NULL);
}

oi_reactor_backend *oi_reactor_backend_create(void) {
    int kq = kqueue();
    if (kq < 0) {
        return NULL;
    }
    oi_reactor_backend *backend = calloc(1, sizeof *backend);
    if (backend == NULL) {
        close(kq);
        return NULL;
    }
    backend->kq = kq;
    backend->next_timer = 1;
    return backend;
}

void oi_reactor_backend_destroy(oi_reactor_backend *backend) {
    if (backend == NULL) {
        return;
    }
    struct fd_watch *watch = backend->fds;
    while (watch != NULL) {
        struct fd_watch *next = watch->next;
        free(watch);
        watch = next;
    }
    close(backend->kq);
    free(backend);
}

static struct fd_watch *find_fd(oi_reactor_backend *backend, int fd) {
    for (struct fd_watch *watch = backend->fds; watch != NULL;
         watch = watch->next) {
        if (watch->fd == fd) {
            return watch;
        }
    }
    return NULL;
}

static oi_status update_filter(oi_reactor_backend *backend, int fd,
                               int16_t filter, int old_enabled,
                               int new_enabled, uint64_t token) {
    if (old_enabled == new_enabled) {
        if (!new_enabled) {
            return OI_OK;
        }
        return change(backend, (uintptr_t)fd, filter, EV_ADD | EV_ENABLE, 0,
                      0, token) == 0
                   ? OI_OK
                   : OI_ERR_IO;
    }
    uint16_t flags = new_enabled ? (EV_ADD | EV_ENABLE) : EV_DELETE;
    if (change(backend, (uintptr_t)fd, filter, flags, 0, 0, token) != 0) {
        if (!new_enabled && errno == ENOENT) {
            return OI_OK;
        }
        return OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_reactor_backend_add(oi_reactor_backend *backend, int fd,
                                  int interest, uint64_t token) {
    if (find_fd(backend, fd) != NULL) {
        return OI_ERR_EXISTS;
    }
    struct fd_watch *watch = malloc(sizeof *watch);
    if (watch == NULL) {
        return OI_ERR_NOMEM;
    }
    watch->fd = fd;
    watch->interest = 0;
    watch->token = token;
    watch->next = backend->fds;
    backend->fds = watch;
    oi_status st =
        oi_reactor_backend_modify(backend, fd, interest, token);
    if (st != OI_OK) {
        backend->fds = watch->next;
        free(watch);
    }
    return st;
}

oi_status oi_reactor_backend_modify(oi_reactor_backend *backend, int fd,
                                     int interest, uint64_t token) {
    struct fd_watch *watch = find_fd(backend, fd);
    if (watch == NULL) {
        return OI_ERR_NOTFOUND;
    }
    int old = watch->interest;
    oi_status st = update_filter(backend, fd, EVFILT_READ,
                                  (old & OI_BACKEND_EV_READ) != 0,
                                  (interest & OI_BACKEND_EV_READ) != 0,
                                  token);
    if (st == OI_OK) {
        st = update_filter(backend, fd, EVFILT_WRITE,
                           (old & OI_BACKEND_EV_WRITE) != 0,
                           (interest & OI_BACKEND_EV_WRITE) != 0, token);
    }
    if (st != OI_OK) {
        update_filter(backend, fd, EVFILT_READ,
                      (interest & OI_BACKEND_EV_READ) != 0,
                      (old & OI_BACKEND_EV_READ) != 0, watch->token);
        return st;
    }
    watch->interest = interest;
    watch->token = token;
    return OI_OK;
}

oi_status oi_reactor_backend_remove(oi_reactor_backend *backend, int fd) {
    struct fd_watch **link = &backend->fds;
    while (*link != NULL && (*link)->fd != fd) {
        link = &(*link)->next;
    }
    if (*link == NULL) {
        return OI_ERR_NOTFOUND;
    }
    struct fd_watch *watch = *link;
    if (watch->interest & OI_BACKEND_EV_READ) {
        update_filter(backend, fd, EVFILT_READ, 1, 0, watch->token);
    }
    if (watch->interest & OI_BACKEND_EV_WRITE) {
        update_filter(backend, fd, EVFILT_WRITE, 1, 0, watch->token);
    }
    *link = watch->next;
    free(watch);
    return OI_OK;
}

oi_status oi_reactor_backend_timer_add(oi_reactor_backend *backend,
                                        int timeout_ms, uint64_t token,
                                        uintptr_t *out_handle) {
    uintptr_t handle = backend->next_timer++;
    if (handle == 0) {
        handle = backend->next_timer++;
    }
#ifdef NOTE_MSECONDS
    uint32_t units = NOTE_MSECONDS;
#else
    uint32_t units = 0;
#endif
    if (change(backend, handle, EVFILT_TIMER, EV_ADD | EV_ONESHOT, units,
               timeout_ms, token) != 0) {
        return OI_ERR_IO;
    }
    *out_handle = handle;
    return OI_OK;
}

oi_status oi_reactor_backend_timer_remove(oi_reactor_backend *backend,
                                           uintptr_t handle) {
    if (change(backend, handle, EVFILT_TIMER, EV_DELETE, 0, 0, 0) != 0 &&
        errno != ENOENT) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_reactor_backend_process_add(oi_reactor_backend *backend,
                                          pid_t pid, uint64_t token,
                                          uintptr_t *out_handle) {
    uintptr_t handle = (uintptr_t)pid;
    if (change(backend, handle, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT,
               0, token) != 0) {
        return OI_ERR_IO;
    }
    *out_handle = handle;
    return OI_OK;
}

oi_status oi_reactor_backend_process_remove(oi_reactor_backend *backend,
                                             uintptr_t handle) {
    if (change(backend, handle, EVFILT_PROC, EV_DELETE, 0, 0, 0) != 0 &&
        errno != ENOENT) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_reactor_backend_wait(oi_reactor_backend *backend, int timeout_ms,
                                   uint64_t *out_tokens, int *out_revents,
                                   int max_events, int *out_n) {
    struct kevent events[OI_REACTOR_MAX_EVENTS];
    int cap = max_events < OI_REACTOR_MAX_EVENTS ? max_events
                                                  : OI_REACTOR_MAX_EVENTS;
    struct timespec timeout;
    struct timespec *timeout_ptr = NULL;
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        timeout_ptr = &timeout;
    }
    int count;
    do {
        count = kevent(backend->kq, NULL, 0, events, cap, timeout_ptr);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        return OI_ERR_IO;
    }

    int used = 0;
    for (int i = 0; i < count; i++) {
        uint64_t token = (uint64_t)(uintptr_t)events[i].udata;
        int revents = 0;
        if (events[i].filter == EVFILT_READ) {
            revents |= OI_BACKEND_EV_READ;
        } else if (events[i].filter == EVFILT_WRITE) {
            revents |= OI_BACKEND_EV_WRITE;
        } else {
            revents |= OI_BACKEND_EV_READ;
        }
        if (events[i].flags & EV_EOF) {
            revents |= OI_BACKEND_EV_HUP;
        }
        if ((events[i].flags & EV_ERROR) && events[i].data != 0) {
            revents |= OI_BACKEND_EV_ERROR;
        }
        int existing = -1;
        for (int j = 0; j < used; j++) {
            if (out_tokens[j] == token) {
                existing = j;
                break;
            }
        }
        if (existing >= 0) {
            out_revents[existing] |= revents;
        } else {
            out_tokens[used] = token;
            out_revents[used] = revents;
            used++;
        }
    }
    *out_n = used;
    return OI_OK;
}
