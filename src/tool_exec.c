/* pipe2() is a Linux extension, not POSIX -- _GNU_SOURCE exposes it.
 * Scoped to this file rather than loosening the whole project's
 * feature-test macros. */
#define _GNU_SOURCE

#include "tool_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat.h"

enum call_state { CALL_PENDING_PERMISSION, CALL_RUNNING };

struct oi_tool_call {
    oi_reactor *reactor;
    oi_arena *arena;

    oi_tool_output_cb on_output;
    oi_tool_done_cb on_done;
    void *user_data;

    enum call_state state;

    /* staged for a call awaiting an ASK decision; consumed by spawn() */
    oi_tool_build_argv build_argv;
    void *tool_user_data;
    const oi_json_value *args;

    pid_t pid;
    int pid_fd;     /* pidfd watched by this call's reactor */
    oi_reactor_timer *timer;
    int timeout_ms;
    int stdin_fd;  /* -1 once closed */
    int stdin_close_pending; /* close_stdin() called while a write was
                               * still queued; actually closed once
                               * on_stdin_event finishes flushing it */
    int stdout_fd; /* -1 once closed/EOF */
    int err_fd;    /* -1 once closed/EOF */

    char *out_buf; /* queued stdin bytes */
    size_t out_len;
    size_t out_off;
    size_t out_cap;

    int child_exited;
    int cancelled;
    int timed_out;
    int stdout_eof;
    int exec_failed;
    int exec_errno;
    int exit_status; /* raw waitpid status, valid once child_exited */

    /* See the identical mechanism in oi_llm_conn/oi_llm_http_parser/
     * oi_llm_sse_parser: set before invoking on_output, so a reentrant
     * oi_tool_call_cancel() from within it can signal back that `call`
     * was freed out from under the still-running dispatch loop. */
    int *destroyed_flag;
};

static oi_status set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static int open_pidfd(pid_t pid) {
#ifdef SYS_pidfd_open
    return (int)syscall(SYS_pidfd_open, pid, 0);
#else
    (void)pid;
    errno = ENOSYS;
    return -1;
#endif
}

static void close_call_io(oi_tool_call *call) {
    if (call->stdin_fd >= 0) {
        oi_reactor_remove(call->reactor, call->stdin_fd);
        close(call->stdin_fd);
        call->stdin_fd = -1;
    }
    if (call->stdout_fd >= 0) {
        oi_reactor_remove(call->reactor, call->stdout_fd);
        close(call->stdout_fd);
        call->stdout_fd = -1;
    }
    if (call->err_fd >= 0) {
        oi_reactor_remove(call->reactor, call->err_fd);
        close(call->err_fd);
        call->err_fd = -1;
    }
    free(call->out_buf);
    call->out_buf = NULL;
    call->out_len = call->out_off = call->out_cap = 0;
}

static void free_call(oi_tool_call *call) {
    oi_reactor_timer_cancel(call->timer);
    if (call->pid_fd >= 0) {
        oi_reactor_remove(call->reactor, call->pid_fd);
        close(call->pid_fd);
    }
    close_call_io(call);
    if (call->destroyed_flag) {
        *call->destroyed_flag = 1;
    }
    free(call);
}

static void maybe_finish(oi_tool_call *call) {
    if (!call->child_exited ||
        (!call->cancelled && !call->timed_out && !call->stdout_eof)) {
        return;
    }

    if (call->cancelled) {
        free_call(call);
        return;
    }

    oi_tool_exit_kind kind;
    int code;
    if (call->timed_out) {
        kind = OI_TOOL_EXIT_TIMEOUT;
        code = 0;
    } else if (call->exec_failed) {
        kind = OI_TOOL_EXIT_FAILED;
        code = call->exec_errno;
    } else if (WIFSIGNALED(call->exit_status)) {
        kind = OI_TOOL_EXIT_SIGNALED;
        code = WTERMSIG(call->exit_status);
    } else {
        kind = OI_TOOL_EXIT_NORMAL;
        code = WEXITSTATUS(call->exit_status);
    }

    if (call->on_done) {
        call->on_done(kind, code, call->user_data);
    }
    free_call(call);
}

static void on_pidfd_event(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    (void)fd;
    (void)revents;
    oi_tool_call *call = ud;

    int status;
    pid_t rc;
    do {
        rc = waitpid(call->pid, &status, WNOHANG);
    } while (rc < 0 && errno == EINTR);
    if (rc < 0 && errno == ECHILD) {
        /* The embedder may use SIG_IGN/SA_NOCLDWAIT for SIGCHLD, causing
         * the kernel to reap children automatically. The pidfd still
         * becomes readable, but no exit status remains to collect. */
        status = 0;
        call->exec_failed = 1;
        call->exec_errno = ECHILD;
        rc = call->pid;
    }
    if (rc != call->pid) {
        return;
    }

    oi_reactor_remove(call->reactor, call->pid_fd);
    close(call->pid_fd);
    call->pid_fd = -1;
    call->child_exited = 1;
    call->exit_status = status;
    maybe_finish(call);
}

static void kill_and_reap(pid_t pid) {
    if (kill(-pid, SIGKILL) != 0) {
        kill(pid, SIGKILL);
    }
    for (;;) {
        if (waitpid(pid, NULL, 0) >= 0) {
            return;
        }
        if (errno != EINTR) {
            return;
        }
    }
}

static void terminate_process_group(pid_t pid) {
    if (kill(-pid, SIGKILL) != 0) {
        kill(pid, SIGKILL);
    }
}

static void on_call_timeout(oi_reactor *r, void *ud) {
    (void)r;
    oi_tool_call *call = ud;
    call->timed_out = 1;
    call->on_output = NULL;
    terminate_process_group(call->pid);
    close_call_io(call);
    maybe_finish(call);
}

static void on_err_event(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    (void)fd;
    (void)revents;
    oi_tool_call *call = ud;

    char buf[16];
    ssize_t n = read(call->err_fd, buf, sizeof buf);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return; /* wait for the next readiness event */
        }
        /* hard error reading the pipe: no exec-failure info available,
         * fall through and close it like any other terminal case */
    } else if (n > 0) {
        int e = 0;
        if ((size_t)n >= sizeof e) {
            memcpy(&e, buf, sizeof e); /* single atomic pipe write on the child side */
        }
        call->exec_failed = 1;
        call->exec_errno = e;
    }
    /* n == 0 (EOF: exec succeeded, the CLOEXEC write end closed itself),
     * or we just consumed the one failure notice, or a hard error:
     * nothing more will ever arrive on this fd. */
    oi_reactor_remove(call->reactor, call->err_fd);
    close(call->err_fd);
    call->err_fd = -1;
    maybe_finish(call);
}

static void on_stdout_event(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    (void)fd;
    (void)revents;
    oi_tool_call *call = ud;

    int destroyed = 0;
OI_DIAG_PUSH_IGNORE_DANGLING
    call->destroyed_flag = &destroyed;
OI_DIAG_POP

    char buf[16384];
    for (;;) {
        ssize_t n = read(call->stdout_fd, buf, sizeof buf);
        if (n > 0) {
            if (call->on_output) {
                call->on_output(buf, (size_t)n, call->user_data);
            }
            if (destroyed) {
                return; /* `call` was freed by a reentrant cancel */
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            call->destroyed_flag = NULL;
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        /* n == 0 (EOF) or a hard read error: treat both as "no more
         * output is coming." */
        oi_reactor_remove(call->reactor, call->stdout_fd);
        close(call->stdout_fd);
        call->stdout_fd = -1;
        call->stdout_eof = 1;
        maybe_finish(call);
        if (!destroyed) {
            call->destroyed_flag = NULL;
        }
        return;
    }
}

static void on_stdin_event(oi_reactor *r, int fd, int revents, void *ud) {
    (void)r;
    (void)fd;
    (void)revents;
    oi_tool_call *call = ud;

    while (call->out_off < call->out_len) {
        ssize_t n = send(call->stdin_fd, call->out_buf + call->out_off,
                          call->out_len - call->out_off, MSG_NOSIGNAL);
        if (n > 0) {
            call->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; /* wait for the next writable event */
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        /* hard write error (e.g. EPIPE, the child closed stdin/exited):
         * give up on further stdin writes and drop whatever's queued. */
        call->out_len = call->out_off = 0;
        oi_reactor_remove(call->reactor, call->stdin_fd);
        close(call->stdin_fd);
        call->stdin_fd = -1;
        call->stdin_close_pending = 0;
        return;
    }
    call->out_len = call->out_off = 0;
    oi_reactor_remove(call->reactor, call->stdin_fd);
    if (call->stdin_close_pending) {
        close(call->stdin_fd);
        call->stdin_fd = -1;
        call->stdin_close_pending = 0;
    }
}

static oi_status spawn(oi_tool_call *call) {
    char **argv;
    oi_status st =
        call->build_argv(call->args, call->arena, call->tool_user_data, &argv);
    if (st != OI_OK) {
        return st;
    }
    if (argv == NULL || argv[0] == NULL) {
        return OI_ERR_INVAL;
    }

    int stdin_pipe[2], stdout_pipe[2], err_pipe[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, stdin_pipe) != 0) {
        return OI_ERR_IO;
    }
    if (pipe2(stdout_pipe, O_CLOEXEC) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        return OI_ERR_IO;
    }
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return OI_ERR_IO;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return OI_ERR_IO;
    }

    if (pid == 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(err_pipe[0]);

        if (setpgid(0, 0) != 0 ||
            dup2(stdin_pipe[0], STDIN_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDOUT_FILENO) < 0 ||
            dup2(stdout_pipe[1], STDERR_FILENO) < 0) {
            goto child_failed;
        }

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        execvp(argv[0], argv);

child_failed:;
        int e = errno;
        ssize_t unused = write(err_pipe[1], &e, sizeof e);
        (void)unused;
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(err_pipe[1]);

    int pid_fd = open_pidfd(pid);
    if (pid_fd < 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(err_pipe[0]);
        kill_and_reap(pid);
        return OI_ERR_IO;
    }

    if (set_nonblocking(stdin_pipe[1]) != OI_OK ||
        set_nonblocking(stdout_pipe[0]) != OI_OK ||
        set_nonblocking(err_pipe[0]) != OI_OK) {
        close(pid_fd);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(err_pipe[0]);
        kill_and_reap(pid);
        return OI_ERR_IO;
    }

    call->pid = pid;
    call->pid_fd = pid_fd;
    call->stdin_fd = stdin_pipe[1];
    call->stdout_fd = stdout_pipe[0];
    call->err_fd = err_pipe[0];

    st = oi_reactor_add(call->reactor, call->stdout_fd, OI_EV_READ,
                         on_stdout_event, call);
    if (st != OI_OK) {
        close(call->stdin_fd);
        close(call->stdout_fd);
        close(call->err_fd);
        close(call->pid_fd);
        call->pid_fd = -1;
        kill_and_reap(pid);
        return st;
    }
    st = oi_reactor_add(call->reactor, call->err_fd, OI_EV_READ, on_err_event,
                         call);
    if (st != OI_OK) {
        oi_reactor_remove(call->reactor, call->stdout_fd);
        close(call->stdin_fd);
        close(call->stdout_fd);
        close(call->err_fd);
        close(call->pid_fd);
        call->pid_fd = -1;
        kill_and_reap(pid);
        return st;
    }

    st = oi_reactor_add(call->reactor, call->pid_fd, OI_EV_READ,
                         on_pidfd_event, call);
    if (st != OI_OK) {
        oi_reactor_remove(call->reactor, call->stdout_fd);
        oi_reactor_remove(call->reactor, call->err_fd);
        close(call->stdin_fd);
        close(call->stdout_fd);
        close(call->err_fd);
        close(call->pid_fd);
        call->pid_fd = -1;
        kill_and_reap(pid);
        return st;
    }

    if (call->timeout_ms > 0) {
        st = oi_reactor_timer_start(call->reactor, call->timeout_ms,
                                     on_call_timeout, call, &call->timer);
        if (st != OI_OK) {
            oi_reactor_remove(call->reactor, call->stdout_fd);
            oi_reactor_remove(call->reactor, call->err_fd);
            oi_reactor_remove(call->reactor, call->pid_fd);
            close(call->stdin_fd);
            close(call->stdout_fd);
            close(call->err_fd);
            close(call->pid_fd);
            call->stdin_fd = call->stdout_fd = call->err_fd = -1;
            call->pid_fd = -1;
            kill_and_reap(pid);
            return st;
        }
    }

    return OI_OK;
}

oi_status oi_tool_call_start(oi_tool_registry *reg, oi_reactor *r,
                              oi_arena *arena, const char *tool_name,
                              const oi_json_value *args,
                              oi_tool_permission_cb permission_cb,
                              void *permission_ud, oi_tool_output_cb on_output,
                              oi_tool_done_cb on_done, void *user_data,
                              oi_tool_call **out_call) {
    if (reg == NULL || r == NULL || arena == NULL || tool_name == NULL ||
        out_call == NULL) {
        return OI_ERR_INVAL;
    }

    oi_tool_build_argv build_argv;
    void *tool_ud;
    oi_status st = oi_tool_registry_lookup(reg, tool_name, &build_argv,
                                            &tool_ud);
    if (st != OI_OK) {
        return st; /* OI_ERR_NOTFOUND */
    }

    oi_tool_decision decision =
        permission_cb ? permission_cb(tool_name, args, permission_ud)
                      : OI_TOOL_ALLOW;
    if (decision == OI_TOOL_DENY) {
        return OI_ERR_DENIED;
    }

    oi_tool_call *call = calloc(1, sizeof *call);
    if (call == NULL) {
        return OI_ERR_NOMEM;
    }
    call->reactor = r;
    call->arena = arena;
    call->on_output = on_output;
    call->on_done = on_done;
    call->user_data = user_data;
    call->build_argv = build_argv;
    call->tool_user_data = tool_ud;
    call->args = args;
    call->stdin_fd = -1;
    call->stdout_fd = -1;
    call->err_fd = -1;
    call->pid_fd = -1;

    if (decision == OI_TOOL_ASK) {
        call->state = CALL_PENDING_PERMISSION;
        *out_call = call;
        return OI_OK;
    }

    st = spawn(call);
    if (st != OI_OK) {
        free(call);
        return st;
    }
    call->state = CALL_RUNNING;
    *out_call = call;
    return OI_OK;
}

oi_status oi_tool_call_resolve(oi_tool_call *call, int allow) {
    if (call == NULL || call->state != CALL_PENDING_PERMISSION) {
        return OI_ERR_INVAL;
    }
    if (!allow) {
        free(call);
        return OI_ERR_DENIED;
    }

    oi_status st = spawn(call);
    if (st != OI_OK) {
        free(call);
        return st;
    }
    call->state = CALL_RUNNING;
    return OI_OK;
}

oi_status oi_tool_call_write_stdin(oi_tool_call *call, const void *data,
                                    size_t len) {
    if (call == NULL || data == NULL || call->state != CALL_RUNNING ||
        call->stdin_fd < 0) {
        return OI_ERR_INVAL;
    }
    if (len == 0) {
        return OI_OK;
    }

    int was_empty = call->out_off == call->out_len;

    if (len > (size_t)-1 - call->out_len) {
        return OI_ERR_NOMEM;
    }
    size_t needed = call->out_len + len;
    if (needed > call->out_cap) {
        size_t new_cap = call->out_cap == 0 ? 4096 : call->out_cap;
        while (new_cap < needed) {
            if (new_cap > (size_t)-1 / 2) {
                return OI_ERR_NOMEM;
            }
            new_cap *= 2;
        }
        char *nb = realloc(call->out_buf, new_cap);
        if (nb == NULL) {
            return OI_ERR_NOMEM;
        }
        call->out_buf = nb;
        call->out_cap = new_cap;
    }
    memcpy(call->out_buf + call->out_len, data, len);
    call->out_len += len;

    if (was_empty) {
        oi_status st = oi_reactor_add(call->reactor, call->stdin_fd,
                                       OI_EV_WRITE, on_stdin_event, call);
        if (st != OI_OK) {
            call->out_len -= len;
            return st;
        }
    }
    return OI_OK;
}

oi_status oi_tool_call_close_stdin(oi_tool_call *call) {
    if (call == NULL || call->state != CALL_RUNNING || call->stdin_fd < 0) {
        return OI_ERR_INVAL;
    }
    if (call->out_off < call->out_len) {
        /* still draining a queued write: defer the actual close until
         * on_stdin_event finishes flushing it, so the child sees
         * everything written before it sees EOF. */
        call->stdin_close_pending = 1;
        return OI_OK;
    }
    close(call->stdin_fd);
    call->stdin_fd = -1;
    return OI_OK;
}

oi_status oi_tool_call_set_timeout(oi_tool_call *call, int timeout_ms) {
    if (call == NULL || timeout_ms <= 0 || call->timeout_ms > 0 ||
        call->timer != NULL) {
        return OI_ERR_INVAL;
    }
    call->timeout_ms = timeout_ms;
    if (call->state == CALL_PENDING_PERMISSION) {
        return OI_OK;
    }
    oi_status st = oi_reactor_timer_start(call->reactor, timeout_ms,
                                           on_call_timeout, call,
                                           &call->timer);
    if (st != OI_OK) {
        call->timeout_ms = 0;
    }
    return st;
}

void oi_tool_call_cancel(oi_tool_call *call) {
    if (call == NULL) {
        return;
    }
    if (call->state == CALL_PENDING_PERMISSION) {
        free(call); /* nothing was ever spawned or registered */
        return;
    }
    if (call->pid > 0) {
        /* The child creates a fresh process group before exec, so a tool
         * cannot leave grandchildren running after its call is cancelled. */
        terminate_process_group(call->pid);
    }
    call->cancelled = 1;
    call->on_output = NULL;
    call->on_done = NULL;
    if (call->destroyed_flag) {
        *call->destroyed_flag = 1;
        call->destroyed_flag = NULL;
    }
    close_call_io(call);
    maybe_finish(call);
}
