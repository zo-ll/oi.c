#include "oi/reactor.h"

#include "reactor_backend.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "reactor_process.h"

_Static_assert(OI_EV_READ == OI_BACKEND_EV_READ, "read bit mismatch");
_Static_assert(OI_EV_WRITE == OI_BACKEND_EV_WRITE, "write bit mismatch");
_Static_assert(OI_EV_ERROR == OI_BACKEND_EV_ERROR, "error bit mismatch");
_Static_assert(OI_EV_HUP == OI_BACKEND_EV_HUP, "hup bit mismatch");

#define OI_REACTOR_MAX_FD (1 << 20)

struct fd_entry {
    oi_reactor_cb cb;
    void *user_data;
    int interest;
    int in_use;
    uint32_t generation;
};

struct oi_reactor {
    oi_reactor_backend *backend;
    struct fd_entry *entries;
    size_t capacity;
    size_t registered_count;
    int stop_requested;
    uint64_t next_native_id;
    struct oi_reactor_timer **timer_heap;
    size_t timer_count;
    size_t timer_cap;
    struct oi_reactor_process *processes;
};

struct oi_reactor_timer {
    oi_reactor *reactor;
    uint64_t deadline_ns;
    size_t heap_index;
    oi_reactor_timer_cb cb;
    void *user_data;
    int active;
};

struct oi_reactor_process {
    oi_reactor *reactor;
    uint64_t id;
    uintptr_t backend_handle;
    pid_t pid;
    oi_reactor_process_cb cb;
    void *user_data;
    int active;
    struct oi_reactor_process *next;
};

#define TOKEN_KIND_SHIFT 62
#define TOKEN_PAYLOAD_MASK ((UINT64_C(1) << TOKEN_KIND_SHIFT) - 1)
#define TOKEN_PROCESS (UINT64_C(2) << TOKEN_KIND_SHIFT)

static uint64_t monotonic_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void heap_place(oi_reactor *r, size_t index, oi_reactor_timer *t) {
    r->timer_heap[index] = t;
    t->heap_index = index;
}

static void heap_sift_up(oi_reactor *r, size_t index) {
    oi_reactor_timer *t = r->timer_heap[index];
    while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (r->timer_heap[parent]->deadline_ns <= t->deadline_ns) {
            break;
        }
        heap_place(r, index, r->timer_heap[parent]);
        index = parent;
    }
    heap_place(r, index, t);
}

static void heap_sift_down(oi_reactor *r, size_t index) {
    oi_reactor_timer *t = r->timer_heap[index];
    for (;;) {
        size_t child = 2 * index + 1;
        if (child >= r->timer_count) {
            break;
        }
        if (child + 1 < r->timer_count &&
            r->timer_heap[child + 1]->deadline_ns <
                r->timer_heap[child]->deadline_ns) {
            child++;
        }
        if (t->deadline_ns <= r->timer_heap[child]->deadline_ns) {
            break;
        }
        heap_place(r, index, r->timer_heap[child]);
        index = child;
    }
    heap_place(r, index, t);
}

static void heap_remove(oi_reactor *r, oi_reactor_timer *t) {
    size_t index = t->heap_index;
    oi_reactor_timer *last = r->timer_heap[--r->timer_count];
    if (last == t) {
        return;
    }
    heap_place(r, index, last);
    heap_sift_up(r, index);
    heap_sift_down(r, last->heap_index);
}

static int wait_timeout(const oi_reactor *r, int timeout_ms) {
    if (r->timer_count == 0) {
        return timeout_ms;
    }
    uint64_t now = monotonic_ns();
    uint64_t deadline = r->timer_heap[0]->deadline_ns;
    uint64_t left_ms = deadline <= now ? 0 : (deadline - now + 999999) / 1000000;
    if (left_ms > INT_MAX) {
        left_ms = INT_MAX;
    }
    if (timeout_ms < 0 || (uint64_t)timeout_ms > left_ms) {
        return (int)left_ms;
    }
    return timeout_ms;
}

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
    r->next_native_id = 1;
    r->timer_heap = NULL;
    r->timer_count = 0;
    r->timer_cap = 0;
    r->processes = NULL;
    return r;
}

void oi_reactor_destroy(oi_reactor *r) {
    if (r == NULL) {
        return;
    }
    oi_reactor_backend_destroy(r->backend);
    for (size_t i = 0; i < r->timer_count; i++) {
        free(r->timer_heap[i]);
    }
    free(r->timer_heap);
    while (r->processes != NULL) {
        oi_reactor_process *next = r->processes->next;
        free(r->processes);
        r->processes = next;
    }
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

static uint64_t registration_token(int fd, uint32_t generation) {
    return (((uint64_t)generation << 32) | (uint32_t)fd) &
           TOKEN_PAYLOAD_MASK;
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

    uint32_t generation = r->entries[fd].generation + 1;
    generation &= UINT32_C(0x3fffffff);
    if (generation == 0) {
        generation = 1;
    }
    st = oi_reactor_backend_add(r->backend, fd, interest,
                                 registration_token(fd, generation));
    if (st != OI_OK) {
        return st;
    }

    r->entries[fd].cb = cb;
    r->entries[fd].user_data = user_data;
    r->entries[fd].interest = interest;
    r->entries[fd].in_use = 1;
    r->entries[fd].generation = generation;
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
    if ((size_t)fd < r->capacity && r->entries[fd].in_use &&
        r->entries[fd].interest == interest) {
        return OI_OK;
    }

    uint32_t generation =
        (size_t)fd < r->capacity ? r->entries[fd].generation : 0;
    st = oi_reactor_backend_modify(
        r->backend, fd, interest, registration_token(fd, generation));
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

    uint64_t tokens[OI_REACTOR_MAX_EVENTS];
    int revents[OI_REACTOR_MAX_EVENTS];
    int n = 0;

    oi_status st = oi_reactor_backend_wait(
        r->backend, wait_timeout(r, timeout_ms), tokens, revents,
        OI_REACTOR_MAX_EVENTS, &n);
    if (out_status) {
        *out_status = st;
    }
    if (st != OI_OK) {
        return -1;
    }

    int dispatched = 0;
    for (int i = 0; i < n; i++) {
        uint64_t kind = tokens[i] >> TOKEN_KIND_SHIFT;
        if (kind == 2) {
            uint64_t id = tokens[i] & TOKEN_PAYLOAD_MASK;
            oi_reactor_process **link = &r->processes;
            while (*link != NULL && (*link)->id != id) {
                link = &(*link)->next;
            }
            if (*link != NULL) {
                oi_reactor_process *process = *link;
                *link = process->next;
                oi_reactor_backend_process_remove(r->backend,
                                                   process->backend_handle);
                process->active = 0;
                process->next = NULL;
                r->registered_count--;
                process->cb(r, process->pid, process->user_data);
                dispatched++;
            }
            continue;
        }

        int fd = (int)(uint32_t)tokens[i];
        uint32_t generation = (uint32_t)(tokens[i] >> 32);
        if ((size_t)fd >= r->capacity || !r->entries[fd].in_use ||
            r->entries[fd].generation != generation) {
            continue;
        }
        r->entries[fd].cb(r, fd, revents[i], r->entries[fd].user_data);
        dispatched++;
    }

    uint64_t now = monotonic_ns();
    while (r->timer_count > 0 && r->timer_heap[0]->deadline_ns <= now) {
        oi_reactor_timer *timer = r->timer_heap[0];
        heap_remove(r, timer);
        timer->active = 0;
        r->registered_count--;
        timer->cb(r, timer->user_data);
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

oi_status oi_reactor_timer_start(oi_reactor *r, int timeout_ms,
                                  oi_reactor_timer_cb cb, void *user_data,
                                  oi_reactor_timer **out_timer) {
    if (r == NULL || timeout_ms <= 0 || cb == NULL || out_timer == NULL) {
        return OI_ERR_INVAL;
    }
    if (r->timer_count == r->timer_cap) {
        size_t new_cap = r->timer_cap == 0 ? 8 : r->timer_cap * 2;
        oi_reactor_timer **heap =
            realloc(r->timer_heap, new_cap * sizeof *heap);
        if (heap == NULL) {
            return OI_ERR_NOMEM;
        }
        r->timer_heap = heap;
        r->timer_cap = new_cap;
    }

    oi_reactor_timer *timer = malloc(sizeof *timer);
    if (timer == NULL) {
        return OI_ERR_NOMEM;
    }
    timer->reactor = r;
    timer->deadline_ns =
        monotonic_ns() + (uint64_t)timeout_ms * UINT64_C(1000000);
    timer->cb = cb;
    timer->user_data = user_data;
    timer->active = 1;
    heap_place(r, r->timer_count++, timer);
    heap_sift_up(r, timer->heap_index);
    r->registered_count++;
    *out_timer = timer;
    return OI_OK;
}

void oi_reactor_timer_cancel(oi_reactor_timer *timer) {
    if (timer == NULL) {
        return;
    }
    if (timer->active) {
        heap_remove(timer->reactor, timer);
        timer->reactor->registered_count--;
    }
    free(timer);
}

oi_status oi_reactor_process_start(oi_reactor *r, pid_t pid,
                                    oi_reactor_process_cb cb, void *user_data,
                                    oi_reactor_process **out_process) {
    if (r == NULL || pid <= 0 || cb == NULL || out_process == NULL) {
        return OI_ERR_INVAL;
    }
    oi_reactor_process *process = calloc(1, sizeof *process);
    if (process == NULL) {
        return OI_ERR_NOMEM;
    }
    process->reactor = r;
    process->id = r->next_native_id++;
    if (process->id == 0 || process->id > TOKEN_PAYLOAD_MASK) {
        process->id = 1;
        r->next_native_id = 2;
    }
    process->pid = pid;
    process->cb = cb;
    process->user_data = user_data;
    process->active = 1;
    process->next = r->processes;
    oi_status st = oi_reactor_backend_process_add(
        r->backend, pid, TOKEN_PROCESS | process->id,
        &process->backend_handle);
    if (st != OI_OK) {
        free(process);
        return st;
    }
    r->processes = process;
    r->registered_count++;
    *out_process = process;
    return OI_OK;
}

void oi_reactor_process_cancel(oi_reactor_process *process) {
    if (process == NULL) {
        return;
    }
    if (process->active) {
        oi_reactor_process **link = &process->reactor->processes;
        while (*link != NULL && *link != process) {
            link = &(*link)->next;
        }
        if (*link == process) {
            *link = process->next;
        }
        oi_reactor_backend_process_remove(process->reactor->backend,
                                           process->backend_handle);
        process->reactor->registered_count--;
    }
    free(process);
}

int oi_reactor_fd(const oi_reactor *r) {
    return oi_reactor_backend_fd(r->backend);
}

int oi_reactor_timeout_ms(const oi_reactor *r) {
    return wait_timeout(r, -1);
}
