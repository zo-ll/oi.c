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
#include <sys/wait.h>
#include <unistd.h>

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

/*
 * SIGCHLD delivery and reaping are inherently process-global (signal
 * handlers aren't per-object), so this is deliberately the one piece of
 * shared mutable state in the module: a self-pipe waking the reactor,
 * plus a pid -> oi_tool_call table for dispatching each reaped exit to
 * the right call.
 */
static int g_sigchld_pipe[2] = {-1, -1};

struct pid_entry {
    pid_t pid;
    oi_tool_call *call;
};
static struct pid_entry *g_pid_table = NULL;
static size_t g_pid_count = 0;
static size_t g_pid_cap = 0;

static oi_status pid_table_insert(pid_t pid, oi_tool_call *call) {
    if (g_pid_count == g_pid_cap) {
        size_t new_cap = g_pid_cap == 0 ? 8 : g_pid_cap * 2;
        struct pid_entry *ne = realloc(g_pid_table, new_cap * sizeof *ne);
        if (ne == NULL) {
            return OI_ERR_NOMEM;
        }
        g_pid_table = ne;
        g_pid_cap = new_cap;
    }
    g_pid_table[g_pid_count].pid = pid;
    g_pid_table[g_pid_count].call = call;
    g_pid_count++;
    return OI_OK;
}

static oi_tool_call *pid_table_remove(pid_t pid) {
    for (size_t i = 0; i < g_pid_count; i++) {
        if (g_pid_table[i].pid == pid) {
            oi_tool_call *call = g_pid_table[i].call;
            g_pid_table[i] = g_pid_table[g_pid_count - 1];
            g_pid_count--;
            return call;
        }
    }
    return NULL;
}

static oi_status set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static void sigchld_handler(int sig) {
    (void)sig;
    int saved_errno = errno;
    char b = 0;
    ssize_t unused = write(g_sigchld_pipe[1], &b, 1); /* async-signal-safe */
    (void)unused;                                      /* best-effort */
    errno = saved_errno;
}

static void on_sigchld_readable(oi_reactor *r, int fd, int revents,
                                 void *ud);

/*
 * PLAN.md's design has exactly one reactor per process for the whole
 * run, but nothing stops an embedder (or a test suite) from tearing one
 * down and creating another later -- in that case the self-pipe's old
 * registration is simply gone (oi_reactor_destroy doesn't need to be
 * told about each fd individually), so a later call needs to
 * re-register it with the new reactor.
 *
 * This deliberately does NOT try to remember "the" reactor and compare
 * pointers to decide whether re-registration is needed: a freed
 * oi_reactor can have its memory immediately reused by the next
 * oi_reactor_create, so a stale cached pointer can alias a live one
 * (ABA problem) and wrongly look unchanged. Instead this just always
 * attempts oi_reactor_add and treats OI_ERR_EXISTS -- meaning the fd is
 * already registered on *this* reactor's live epoll instance, the only
 * case that actually matters -- as success.
 */
static oi_status ensure_sigchld_setup(oi_reactor *r) {
    if (g_sigchld_pipe[0] < 0) {
        signal(SIGPIPE, SIG_IGN); /* writing to a child's closed stdin must not kill us */

        if (pipe(g_sigchld_pipe) != 0) {
            return OI_ERR_IO;
        }
        if (set_nonblocking(g_sigchld_pipe[0]) != OI_OK ||
            set_nonblocking(g_sigchld_pipe[1]) != OI_OK) {
            close(g_sigchld_pipe[0]);
            close(g_sigchld_pipe[1]);
            g_sigchld_pipe[0] = g_sigchld_pipe[1] = -1;
            return OI_ERR_IO;
        }

        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        if (sigaction(SIGCHLD, &sa, NULL) != 0) {
            close(g_sigchld_pipe[0]);
            close(g_sigchld_pipe[1]);
            g_sigchld_pipe[0] = g_sigchld_pipe[1] = -1;
            return OI_ERR_IO;
        }
    }

    oi_status st = oi_reactor_add(r, g_sigchld_pipe[0], OI_EV_READ,
                                   on_sigchld_readable, NULL);
    if (st != OI_OK && st != OI_ERR_EXISTS) {
        return st;
    }
    return OI_OK;
}

static void free_call(oi_tool_call *call) {
    if (call->stdin_fd >= 0) {
        oi_reactor_remove(call->reactor, call->stdin_fd);
        close(call->stdin_fd);
    }
    if (call->stdout_fd >= 0) {
        oi_reactor_remove(call->reactor, call->stdout_fd);
        close(call->stdout_fd);
    }
    if (call->err_fd >= 0) {
        oi_reactor_remove(call->reactor, call->err_fd);
        close(call->err_fd);
    }
    free(call->out_buf);
    if (call->destroyed_flag) {
        *call->destroyed_flag = 1;
    }
    free(call);
}

static void maybe_finish(oi_tool_call *call) {
    if (!call->child_exited || !call->stdout_eof) {
        return;
    }

    oi_tool_exit_kind kind;
    int code;
    if (call->exec_failed) {
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

static void on_sigchld_readable(oi_reactor *r, int fd, int revents,
                                 void *ud) {
    (void)r;
    (void)ud;
    (void)revents;
    char buf[64];
    while (read(fd, buf, sizeof buf) > 0) {
        /* drain */
    }

    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            break;
        }
        oi_tool_call *call = pid_table_remove(pid);
        if (call == NULL) {
            continue; /* not one of ours, or already abandoned via cancel */
        }
        call->child_exited = 1;
        call->exit_status = status;
        maybe_finish(call);
    }
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
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer"
    call->destroyed_flag = &destroyed;
#pragma GCC diagnostic pop

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
        ssize_t n = write(call->stdin_fd, call->out_buf + call->out_off,
                           call->out_len - call->out_off);
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

/* Kills a just-forked child we're abandoning before it's registered
 * anywhere durable (pid table / reactor). Non-blocking: reaping is left
 * to the normal SIGCHLD path, which will find no matching call and
 * silently discard it. */
static void abandon_child(pid_t pid) { kill(pid, SIGKILL); }

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

    st = ensure_sigchld_setup(call->reactor);
    if (st != OI_OK) {
        return st;
    }

    int stdin_pipe[2], stdout_pipe[2], err_pipe[2];
    if (pipe(stdin_pipe) != 0) {
        return OI_ERR_IO;
    }
    if (pipe(stdout_pipe) != 0) {
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

        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO); /* merge stderr into stdout */

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        execvp(argv[0], argv);

        int e = errno;
        ssize_t unused = write(err_pipe[1], &e, sizeof e);
        (void)unused;
        _exit(127);
    }

    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    close(err_pipe[1]);

    if (set_nonblocking(stdin_pipe[1]) != OI_OK ||
        set_nonblocking(stdout_pipe[0]) != OI_OK ||
        set_nonblocking(err_pipe[0]) != OI_OK) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(err_pipe[0]);
        abandon_child(pid);
        return OI_ERR_IO;
    }

    call->pid = pid;
    call->stdin_fd = stdin_pipe[1];
    call->stdout_fd = stdout_pipe[0];
    call->err_fd = err_pipe[0];

    st = oi_reactor_add(call->reactor, call->stdout_fd, OI_EV_READ,
                         on_stdout_event, call);
    if (st != OI_OK) {
        close(call->stdin_fd);
        close(call->stdout_fd);
        close(call->err_fd);
        abandon_child(pid);
        return st;
    }
    st = oi_reactor_add(call->reactor, call->err_fd, OI_EV_READ, on_err_event,
                         call);
    if (st != OI_OK) {
        oi_reactor_remove(call->reactor, call->stdout_fd);
        close(call->stdin_fd);
        close(call->stdout_fd);
        close(call->err_fd);
        abandon_child(pid);
        return st;
    }

    st = pid_table_insert(pid, call);
    if (st != OI_OK) {
        oi_reactor_remove(call->reactor, call->stdout_fd);
        oi_reactor_remove(call->reactor, call->err_fd);
        close(call->stdin_fd);
        close(call->stdout_fd);
        close(call->err_fd);
        abandon_child(pid);
        return st;
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

    size_t needed = call->out_len + len;
    if (needed > call->out_cap) {
        size_t new_cap = call->out_cap == 0 ? 4096 : call->out_cap;
        while (new_cap < needed) {
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

void oi_tool_call_cancel(oi_tool_call *call) {
    if (call == NULL) {
        return;
    }
    if (call->state == CALL_PENDING_PERMISSION) {
        free(call); /* nothing was ever spawned or registered */
        return;
    }
    if (call->pid > 0) {
        kill(call->pid, SIGKILL);
        pid_table_remove(call->pid); /* let SIGCHLD reap it silently */
    }
    free_call(call);
}
