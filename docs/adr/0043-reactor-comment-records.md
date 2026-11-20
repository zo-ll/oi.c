# reactor: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/reactor.c`, `src/reactor_backend.h`.

## `_Static_assert` (src/reactor.c)

The generic layer passes OI_EV_* bits straight through to the backend without translation; keep the two bit-layouts in lockstep.

## `OI_REACTOR_MAX_FD` (src/reactor.c)

Bound on fd value accepted by oi_reactor_add, to keep the fd-indexed table from growing unboundedly on a bogus/huge fd. Comfortably above any realistic RLIMIT_NOFILE.

## `oi_reactor.timer_heap` (src/reactor.c)

Active timers as a binary min-heap on deadline_ns. The heap, not a kernel timer, decides the epoll_wait timeout, so starting and cancelling a timer make no system call.

## `wait_timeout` (src/reactor.c)

The epoll_wait timeout: the caller's limit, cut to the time left to the next timer deadline, rounded up so a wake is never early.

## `oi_reactor_modify` (src/reactor.c)

The backend is level-triggered, so the same interest again changes nothing; callers re-assert their interest after each read.

## `oi_reactor_step.now` (src/reactor.c)

`now` is read once, so a timer that a callback starts here waits for a later step, even with a deadline that has already passed.

## `oi_reactor_backend` (src/reactor_backend.h)

Internal seam between the generic reactor (fd table, callback dispatch, run/stop bookkeeping) and an OS-specific polling mechanism. A backend knows nothing about oi_reactor_cb/user_data — it only tracks fd, interest, and an opaque registration token reported with readiness. reactor_epoll.c is the only backend; the project targets Linux.

## `OI_BACKEND_EV_READ` (src/reactor_backend.h)

Mirrors OI_EV_READ/OI_EV_WRITE from reactor.h; backend.c does not include reactor.h to keep the seam one-directional, so the bit values are re-asserted equal at compile time in reactor.c.

## `oi_reactor_backend_process_add` (src/reactor_backend.h)

Native child-exit watches. `out_handle` is an opaque backend-owned identifier used only to cancel the watch. Timers live in the generic layer, which passes the time to the next deadline to wait.

## `oi_reactor_backend_wait` (src/reactor_backend.h)

Waits up to timeout_ms (-1 = indefinite) for ready fds. On success, fills out_tokens/out_revents (parallel arrays, caller-owned, capacity max_events) and returns the count via *out_n (0 on timeout). Returns

OI_ERR_IO on a hard backend failure.
