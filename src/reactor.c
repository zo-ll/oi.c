#include "oi/reactor.h"

#include "reactor_backend.h"

#include <stdlib.h>
#include <string.h>

/* The generic layer passes OI_EV_* bits straight through to the backend
 * without translation; keep the two bit-layouts in lockstep. */
_Static_assert(OI_EV_READ == OI_BACKEND_EV_READ, "read bit mismatch");
_Static_assert(OI_EV_WRITE == OI_BACKEND_EV_WRITE, "write bit mismatch");
_Static_assert(OI_EV_ERROR == OI_BACKEND_EV_ERROR, "error bit mismatch");
_Static_assert(OI_EV_HUP == OI_BACKEND_EV_HUP, "hup bit mismatch");

/* Bound on fd value accepted by oi_reactor_add, to keep the fd-indexed
 * table from growing unboundedly on a bogus/huge fd. Comfortably above
 * any realistic RLIMIT_NOFILE. */
#define OI_REACTOR_MAX_FD (1 << 20)

struct fd_entry {
    oi_reactor_cb cb;
    void *user_data;
    int interest;
    int in_use;
};

struct oi_reactor {
    oi_reactor_backend *backend;
    struct fd_entry *entries;
    size_t capacity;
    size_t registered_count;
    int stop_requested;
};

oi_reactor *oi_reactor_create(void) {
    oi_reactor *r = malloc(sizeof *r);
    if (r == NULL) {
        return NULL;
    }

    r->backend = oi_reactor_backend_create();
    if (r->backend == NULL) {
        free(r);
        return NULL;
    }

    r->entries = NULL;
    r->capacity = 0;
    r->registered_count = 0;
    r->stop_requested = 0;
    return r;
}

void oi_reactor_destroy(oi_reactor *r) {
    if (r == NULL) {
        return;
    }
    oi_reactor_backend_destroy(r->backend);
    free(r->entries);
    free(r);
}

static oi_status ensure_capacity(oi_reactor *r, int fd) {
    if ((size_t)fd < r->capacity) {
        return OI_OK;
    }

    size_t new_capacity = r->capacity == 0 ? 16 : r->capacity;
    while (new_capacity <= (size_t)fd) {
        new_capacity *= 2;
    }

    struct fd_entry *new_entries =
        realloc(r->entries, new_capacity * sizeof *new_entries);
    if (new_entries == NULL) {
        return OI_ERR_NOMEM;
    }

    memset(new_entries + r->capacity, 0,
           (new_capacity - r->capacity) * sizeof *new_entries);
    r->entries = new_entries;
    r->capacity = new_capacity;
    return OI_OK;
}

static oi_status validate_fd_interest(int fd, int interest) {
    if (fd < 0 || fd >= OI_REACTOR_MAX_FD) {
        return OI_ERR_INVAL;
    }
    if (interest == 0 || (interest & ~(OI_EV_READ | OI_EV_WRITE)) != 0) {
        return OI_ERR_INVAL;
    }
    return OI_OK;
}

oi_status oi_reactor_add(oi_reactor *r, int fd, int interest,
                          oi_reactor_cb cb, void *user_data) {
    if (r == NULL || cb == NULL) {
        return OI_ERR_INVAL;
    }
    oi_status st = validate_fd_interest(fd, interest);
    if (st != OI_OK) {
        return st;
    }

    st = ensure_capacity(r, fd);
    if (st != OI_OK) {
        return st;
    }

    st = oi_reactor_backend_add(r->backend, fd, interest);
    if (st != OI_OK) {
        return st;
    }

    r->entries[fd].cb = cb;
    r->entries[fd].user_data = user_data;
    r->entries[fd].interest = interest;
    r->entries[fd].in_use = 1;
    r->registered_count++;
    return OI_OK;
}

oi_status oi_reactor_modify(oi_reactor *r, int fd, int interest) {
    if (r == NULL) {
        return OI_ERR_INVAL;
    }
    oi_status st = validate_fd_interest(fd, interest);
    if (st != OI_OK) {
        return st;
    }

    st = oi_reactor_backend_modify(r->backend, fd, interest);
    if (st != OI_OK) {
        return st;
    }

    r->entries[fd].interest = interest;
    return OI_OK;
}

oi_status oi_reactor_remove(oi_reactor *r, int fd) {
    if (r == NULL || fd < 0 || fd >= OI_REACTOR_MAX_FD) {
        return OI_ERR_INVAL;
    }

    oi_status st = oi_reactor_backend_remove(r->backend, fd);
    if (st != OI_OK) {
        return st;
    }

    if ((size_t)fd < r->capacity) {
        r->entries[fd].cb = NULL;
        r->entries[fd].user_data = NULL;
        r->entries[fd].interest = 0;
        r->entries[fd].in_use = 0;
    }
    r->registered_count--;
    return OI_OK;
}

int oi_reactor_step(oi_reactor *r, int timeout_ms, oi_status *out_status) {
    if (r == NULL) {
        if (out_status) {
            *out_status = OI_ERR_INVAL;
        }
        return -1;
    }

    int fds[OI_REACTOR_MAX_EVENTS];
    int revents[OI_REACTOR_MAX_EVENTS];
    int n = 0;

    oi_status st = oi_reactor_backend_wait(r->backend, timeout_ms, fds,
                                            revents, OI_REACTOR_MAX_EVENTS,
                                            &n);
    if (out_status) {
        *out_status = st;
    }
    if (st != OI_OK) {
        return -1;
    }

    int dispatched = 0;
    for (int i = 0; i < n; i++) {
        int fd = fds[i];
        if ((size_t)fd >= r->capacity || !r->entries[fd].in_use) {
            /* Removed by an earlier callback in this same batch. */
            continue;
        }
        r->entries[fd].cb(r, fd, revents[i], r->entries[fd].user_data);
        dispatched++;
    }
    return dispatched;
}

oi_status oi_reactor_run(oi_reactor *r) {
    if (r == NULL) {
        return OI_ERR_INVAL;
    }

    r->stop_requested = 0;
    while (!r->stop_requested && r->registered_count > 0) {
        oi_status st;
        int n = oi_reactor_step(r, -1, &st);
        if (n < 0) {
            return st;
        }
    }
    r->stop_requested = 0;
    return OI_OK;
}

void oi_reactor_stop(oi_reactor *r) {
    if (r == NULL) {
        return;
    }
    r->stop_requested = 1;
}
