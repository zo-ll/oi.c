#include "oi/arena.h"
#include "oi/json.h"
#include "oi/reactor.h"
#include "oi/tool.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static oi_json_value *parse_json(oi_arena *a, const char *text) {
    oi_json_parser *p = oi_json_parser_create(a);
    oi_status st = oi_json_parser_feed(p, text, strlen(text));
    CHECK_EQ(st, OI_OK);
    CHECK(oi_json_parser_done(p));
    oi_json_value *v = oi_json_parser_root(p);
    oi_json_parser_destroy(p);
    return v;
}

/* --- test tool argv builders --- */

static oi_status echo_build_argv(const oi_json_value *args, oi_arena *arena,
                                  void *ud, char ***out_argv) {
    (void)ud;
    const char *text_ptr = "default";
    size_t text_len = strlen(text_ptr);
    oi_json_value *tv = args ? oi_json_object_get(args, "text") : NULL;
    if (tv) {
        oi_json_get_string(tv, &text_ptr, &text_len);
    }
    (void)text_len;

    char **argv = oi_arena_alloc(arena, 3 * sizeof(char *));
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)"/bin/echo";
    argv[1] = (char *)text_ptr;
    argv[2] = NULL;
    *out_argv = argv;
    return OI_OK;
}

static oi_status cat_build_argv(const oi_json_value *args, oi_arena *arena,
                                 void *ud, char ***out_argv) {
    (void)args;
    (void)ud;
    char **argv = oi_arena_alloc(arena, 2 * sizeof(char *));
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)"/bin/cat";
    argv[1] = NULL;
    *out_argv = argv;
    return OI_OK;
}

static oi_status exit_build_argv(const oi_json_value *args, oi_arena *arena,
                                  void *ud, char ***out_argv) {
    (void)ud;
    double code = 0;
    oi_json_value *cv = args ? oi_json_object_get(args, "code") : NULL;
    if (cv) {
        oi_json_get_number(cv, &code);
    }
    char *cmd = oi_arena_alloc(arena, 64);
    if (cmd == NULL) {
        return OI_ERR_NOMEM;
    }
    snprintf(cmd, 64, "exit %d", (int)code);

    char **argv = oi_arena_alloc(arena, 4 * sizeof(char *));
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = cmd;
    argv[3] = NULL;
    *out_argv = argv;
    return OI_OK;
}

static oi_status sleep_build_argv(const oi_json_value *args, oi_arena *arena,
                                   void *ud, char ***out_argv) {
    (void)args;
    (void)ud;
    char **argv = oi_arena_alloc(arena, 3 * sizeof(char *));
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)"/bin/sleep";
    argv[1] = (char *)"5";
    argv[2] = NULL;
    *out_argv = argv;
    return OI_OK;
}

static oi_status fdcheck_build_argv(const oi_json_value *args,
                                     oi_arena *arena, void *ud,
                                     char ***out_argv) {
    (void)args;
    (void)ud;
    char **argv = oi_arena_alloc(arena, 4 * sizeof(char *));
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"for f in /proc/self/fd/[3-9]*; do "
                            "[ -e \"$f\" ] && exit 9; done; exit 0";
    argv[3] = NULL;
    *out_argv = argv;
    return OI_OK;
}

static oi_status tree_build_argv(const oi_json_value *args, oi_arena *arena,
                                  void *ud, char ***out_argv) {
    (void)args;
    (void)ud;
    char **argv = oi_arena_alloc(arena, 4 * sizeof(char *));
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)"sleep 30 & echo $!; wait";
    argv[3] = NULL;
    *out_argv = argv;
    return OI_OK;
}

static oi_status missing_build_argv(const oi_json_value *args,
                                     oi_arena *arena, void *ud,
                                     char ***out_argv) {
    (void)args;
    (void)ud;
    char **argv = oi_arena_alloc(arena, 2 * sizeof(char *));
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)"/nonexistent/oi_test_binary_xyz";
    argv[1] = NULL;
    *out_argv = argv;
    return OI_OK;
}

static oi_status build_argv_fails(const oi_json_value *args, oi_arena *arena,
                                   void *ud, char ***out_argv) {
    (void)args;
    (void)arena;
    (void)ud;
    (void)out_argv;
    return OI_ERR_INVAL;
}

static oi_tool_registry *make_registry(void) {
    oi_tool_registry *reg = oi_tool_registry_create();
    oi_tool_registry_add(reg, "echo", "{}", echo_build_argv, NULL);
    oi_tool_registry_add(reg, "cat", "{}", cat_build_argv, NULL);
    oi_tool_registry_add(reg, "exit", "{}", exit_build_argv, NULL);
    oi_tool_registry_add(reg, "sleep", "{}", sleep_build_argv, NULL);
    oi_tool_registry_add(reg, "fdcheck", "{}", fdcheck_build_argv, NULL);
    oi_tool_registry_add(reg, "tree", "{}", tree_build_argv, NULL);
    oi_tool_registry_add(reg, "missing", "{}", missing_build_argv, NULL);
    oi_tool_registry_add(reg, "badargv", "{}", build_argv_fails, NULL);
    return reg;
}

static void run_until(oi_reactor *r, const int *done_flag, int max_steps) {
    for (int i = 0; i < max_steps && !*done_flag; i++) {
        oi_status st;
        oi_reactor_step(r, 200, &st);
    }
}

static oi_tool_decision allow_cb(const char *name, const oi_json_value *args,
                                  void *ud) {
    (void)name;
    (void)args;
    (void)ud;
    return OI_TOOL_ALLOW;
}

static oi_tool_decision deny_cb(const char *name, const oi_json_value *args,
                                 void *ud) {
    (void)name;
    (void)args;
    (void)ud;
    return OI_TOOL_DENY;
}

static oi_tool_decision ask_cb(const char *name, const oi_json_value *args,
                                void *ud) {
    (void)name;
    (void)args;
    (void)ud;
    return OI_TOOL_ASK;
}

/* --- basic run: echo --- */

struct out_ctx {
    char buf[256];
    size_t len;
    int done;
    oi_tool_exit_kind kind;
    int code;
};

static void capture_output(const void *data, size_t len, void *ud) {
    struct out_ctx *ctx = ud;
    CHECK(ctx->len + len <= sizeof ctx->buf);
    memcpy(ctx->buf + ctx->len, data, len);
    ctx->len += len;
}

static void capture_done(oi_tool_exit_kind kind, int code, void *ud) {
    struct out_ctx *ctx = ud;
    ctx->kind = kind;
    ctx->code = code;
    ctx->done = 1;
}

TEST(run_echo_captures_output_and_normal_exit) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    oi_json_value *args = parse_json(a, "{\"text\":\"hello-tool\"}");

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "echo", args, allow_cb, NULL,
                                       capture_output, capture_done, &ctx,
                                       &call);
    CHECK_EQ(st, OI_OK);
    CHECK(call != NULL);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK_EQ(ctx.kind, OI_TOOL_EXIT_NORMAL);
    CHECK_EQ(ctx.code, 0);
    CHECK_EQ(ctx.len, strlen("hello-tool\n"));
    CHECK(memcmp(ctx.buf, "hello-tool\n", ctx.len) == 0);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

/* --- nonzero exit code --- */

TEST(nonzero_exit_code_reported) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    oi_json_value *args = parse_json(a, "{\"code\":7}");

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "exit", args, allow_cb, NULL,
                                       capture_output, capture_done, &ctx,
                                       &call);
    CHECK_EQ(st, OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK_EQ(ctx.kind, OI_TOOL_EXIT_NORMAL);
    CHECK_EQ(ctx.code, 7);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

/* --- exec failure (missing binary) --- */

TEST(missing_binary_reports_exec_failed) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "missing", NULL, allow_cb,
                                       NULL, capture_output, capture_done,
                                       &ctx, &call);
    CHECK_EQ(st, OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK_EQ(ctx.kind, OI_TOOL_EXIT_FAILED);
    CHECK(ctx.code != 0); /* an errno, e.g. ENOENT */
    CHECK_EQ(ctx.len, 0u);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

/* --- unknown tool / permission denied / build_argv failure: synchronous --- */

TEST(unknown_tool_rejected_synchronously) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "nope", NULL, allow_cb, NULL,
                                       NULL, NULL, NULL, &call);
    CHECK_EQ(st, OI_ERR_NOTFOUND);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(permission_denied_rejected_synchronously) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "echo", NULL, deny_cb, NULL,
                                       NULL, NULL, NULL, &call);
    CHECK_EQ(st, OI_ERR_DENIED);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(build_argv_failure_rejected_synchronously) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "badargv", NULL, allow_cb,
                                       NULL, NULL, NULL, NULL, &call);
    CHECK_EQ(st, OI_ERR_INVAL);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

/* --- ASK then resolve(allow) --- */

TEST(ask_then_resolve_allow_runs) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    oi_json_value *args = parse_json(a, "{\"text\":\"asked\"}");

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "echo", args, ask_cb, NULL,
                                       capture_output, capture_done, &ctx,
                                       &call);
    CHECK_EQ(st, OI_OK);
    CHECK(call != NULL);
    CHECK(!ctx.done); /* nothing runs until resolved */

    CHECK_EQ(oi_tool_call_resolve(call, 1), OI_OK);
    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK_EQ(ctx.kind, OI_TOOL_EXIT_NORMAL);
    CHECK(memcmp(ctx.buf, "asked\n", 6) == 0);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

/* --- ASK then resolve(deny) --- */

TEST(ask_then_resolve_deny) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "echo", NULL, ask_cb, NULL,
                                       capture_output, capture_done, &ctx,
                                       &call);
    CHECK_EQ(st, OI_OK);

    CHECK_EQ(oi_tool_call_resolve(call, 0), OI_ERR_DENIED);
    /* `call` is now invalid; resolving again would be a use-after-free
     * by the caller, not something to test. */

    int never = 0;
    run_until(r, &never, 5);
    CHECK(!ctx.done); /* on_done never fires for a denial */

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(resolve_on_non_pending_call_rejected) {
    CHECK_EQ(oi_tool_call_resolve(NULL, 1), OI_ERR_INVAL);
}

/* --- stdin round trip via cat --- */

TEST(cat_echoes_stdin) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "cat", NULL, allow_cb, NULL,
                                       capture_output, capture_done, &ctx,
                                       &call);
    CHECK_EQ(st, OI_OK);

    CHECK_EQ(oi_tool_call_write_stdin(call, "roundtrip", 9), OI_OK);
    CHECK_EQ(oi_tool_call_close_stdin(call), OI_OK);

    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK_EQ(ctx.kind, OI_TOOL_EXIT_NORMAL);
    CHECK_EQ(ctx.code, 0);
    CHECK_EQ(ctx.len, 9u);
    CHECK(memcmp(ctx.buf, "roundtrip", 9) == 0);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(stdin_queue_rejects_overflow_without_corrupting_call) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    CHECK_EQ(oi_tool_call_start(reg, r, a, "cat", NULL, allow_cb, NULL,
                                 capture_output, capture_done, &ctx, &call),
              OI_OK);
    CHECK_EQ(oi_tool_call_write_stdin(call, "x", (size_t)-1), OI_ERR_NOMEM);
    CHECK_EQ(oi_tool_call_write_stdin(call, "ok", 2), OI_OK);
    CHECK_EQ(oi_tool_call_close_stdin(call), OI_OK);

    run_until(r, &ctx.done, 100);
    CHECK(ctx.done);
    CHECK_EQ(ctx.kind, OI_TOOL_EXIT_NORMAL);
    CHECK_EQ(ctx.len, 2u);
    CHECK(memcmp(ctx.buf, "ok", 2) == 0);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(write_stdin_before_running_rejected) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    oi_tool_call_start(reg, r, a, "cat", NULL, ask_cb, NULL, capture_output,
                        capture_done, &ctx, &call);
    CHECK_EQ(oi_tool_call_write_stdin(call, "x", 1), OI_ERR_INVAL);
    oi_tool_call_cancel(call);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

/* --- cancellation, including reentrant-from-on_output --- */

struct cancel_ctx {
    oi_tool_call *call;
    int output_count;
    int done_fired;
};

static void cancel_on_output(const void *data, size_t len, void *ud) {
    (void)data;
    (void)len;
    struct cancel_ctx *ctx = ud;
    ctx->output_count++;
    oi_tool_call_cancel(ctx->call);
}

static void cancel_on_done(oi_tool_exit_kind kind, int code, void *ud) {
    (void)kind;
    (void)code;
    struct cancel_ctx *ctx = ud;
    ctx->done_fired = 1; /* must never happen */
}

TEST(cancel_from_within_on_output) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    oi_json_value *args = parse_json(a, "{\"text\":\"data-then-cancel\"}");

    struct cancel_ctx ctx = {0};
    oi_status st = oi_tool_call_start(reg, r, a, "echo", args, allow_cb, NULL,
                                       cancel_on_output, cancel_on_done, &ctx,
                                       &ctx.call);
    CHECK_EQ(st, OI_OK);

    int never = 0;
    run_until(r, &never, 20);

    CHECK(ctx.output_count >= 1);
    CHECK(!ctx.done_fired);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(cancel_kills_long_running_process) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    oi_status st = oi_tool_call_start(reg, r, a, "sleep", NULL, allow_cb,
                                       NULL, capture_output, capture_done,
                                       &ctx, &call);
    CHECK_EQ(st, OI_OK);

    /* Give it a moment to actually be running, then cancel well before
     * its 5s sleep would finish on its own. */
    oi_status s2;
    oi_reactor_step(r, 100, &s2);
    oi_tool_call_cancel(call);

    int never = 0;
    run_until(r, &never, 5);
    CHECK(!ctx.done); /* on_done must not fire for a cancelled call */

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(tool_deadline_reports_timeout) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    CHECK_EQ(oi_tool_call_start(reg, r, a, "sleep", NULL, allow_cb, NULL,
                                 capture_output, capture_done, &ctx, &call),
              OI_OK);
    CHECK_EQ(oi_tool_call_set_timeout(call, 10), OI_OK);
    CHECK_EQ(oi_tool_call_set_timeout(call, 20), OI_ERR_INVAL);
    run_until(r, &ctx.done, 100);

    CHECK(ctx.done);
    CHECK_EQ(ctx.kind, OI_TOOL_EXIT_TIMEOUT);
    CHECK_EQ(ctx.code, 0);
    CHECK_EQ(oi_tool_call_set_timeout(NULL, 10), OI_ERR_INVAL);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(pending_tool_deadline_starts_after_permission) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    CHECK_EQ(oi_tool_call_start(reg, r, a, "sleep", NULL, ask_cb, NULL,
                                 capture_output, capture_done, &ctx, &call),
              OI_OK);
    CHECK_EQ(oi_tool_call_set_timeout(call, 10), OI_OK);

    oi_status step_status;
    CHECK_EQ(oi_reactor_step(r, 20, &step_status), 0);
    CHECK(!ctx.done);
    CHECK_EQ(oi_tool_call_resolve(call, 1), OI_OK);
    run_until(r, &ctx.done, 100);
    CHECK(ctx.done);
    CHECK_EQ(ctx.kind, OI_TOOL_EXIT_TIMEOUT);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

struct tree_cancel_ctx {
    oi_tool_call *call;
    char output[64];
    size_t output_len;
    pid_t descendant;
};

static void cancel_tree_on_output(const void *data, size_t len, void *ud) {
    struct tree_cancel_ctx *ctx = ud;
    size_t available = sizeof ctx->output - 1 - ctx->output_len;
    size_t copy_len = len < available ? len : available;
    memcpy(ctx->output + ctx->output_len, data, copy_len);
    ctx->output_len += copy_len;
    ctx->output[ctx->output_len] = '\0';
    if (ctx->descendant == 0 && strchr(ctx->output, '\n') != NULL) {
        ctx->descendant = (pid_t)strtol(ctx->output, NULL, 10);
        oi_tool_call_cancel(ctx->call);
    }
}

static int process_is_running(pid_t pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    char line[512];
    int running = 0;
    if (fgets(line, sizeof line, f) != NULL) {
        char *comm_end = strrchr(line, ')');
        running = comm_end != NULL && comm_end[1] == ' ' &&
                  comm_end[2] != 'Z';
    }
    fclose(f);
    return running;
}

TEST(cancel_kills_descendant_processes) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    struct tree_cancel_ctx ctx = {0};
    CHECK_EQ(oi_tool_call_start(reg, r, a, "tree", NULL, allow_cb, NULL,
                                 cancel_tree_on_output, NULL, &ctx,
                                 &ctx.call),
              OI_OK);

    for (int i = 0; i < 100; i++) {
        oi_status st;
        oi_reactor_step(r, 20, &st);
        if (ctx.descendant > 0 && !process_is_running(ctx.descendant)) {
            break;
        }
    }
    CHECK(ctx.descendant > 0);
    CHECK(!process_is_running(ctx.descendant));

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(cancel_null_safe) { oi_tool_call_cancel(NULL); }

/* --- concurrent calls don't cross-wire --- */

struct multi_ctx {
    int done_count;
    char last_text[3][64];
};

struct multi_slot_ctx {
    struct multi_ctx *shared;
    int index;
};

static void multi_on_output(const void *data, size_t len, void *ud) {
    struct multi_slot_ctx *slot = ud;
    size_t n = len < sizeof slot->shared->last_text[0] - 1
                   ? len
                   : sizeof slot->shared->last_text[0] - 1;
    memcpy(slot->shared->last_text[slot->index], data, n);
    slot->shared->last_text[slot->index][n] = '\0';
}

static void multi_on_done(oi_tool_exit_kind kind, int code, void *ud) {
    (void)kind;
    (void)code;
    struct multi_slot_ctx *slot = ud;
    slot->shared->done_count++;
}

TEST(concurrent_calls_do_not_cross_wire) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    struct multi_ctx shared = {0};
    struct multi_slot_ctx slots[3] = {
        {&shared, 0}, {&shared, 1}, {&shared, 2}};
    const char *texts[3] = {"{\"text\":\"one\"}", "{\"text\":\"two\"}",
                             "{\"text\":\"three\"}"};
    oi_tool_call *calls[3];

    for (int i = 0; i < 3; i++) {
        oi_json_value *args = parse_json(a, texts[i]);
        oi_status st =
            oi_tool_call_start(reg, r, a, "echo", args, allow_cb, NULL,
                                multi_on_output, multi_on_done, &slots[i],
                                &calls[i]);
        CHECK_EQ(st, OI_OK);
    }

    int all_done = 0;
    for (int i = 0; i < 100 && shared.done_count < 3; i++) {
        oi_status st;
        oi_reactor_step(r, 200, &st);
    }
    all_done = shared.done_count == 3;
    CHECK(all_done);
    CHECK_STREQ(shared.last_text[0], "one\n");
    CHECK_STREQ(shared.last_text[1], "two\n");
    CHECK_STREQ(shared.last_text[2], "three\n");

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(child_does_not_inherit_other_tool_descriptors) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    struct out_ctx sleep_ctx = {0};
    oi_tool_call *sleep_call = NULL;
    CHECK_EQ(oi_tool_call_start(reg, r, a, "sleep", NULL, allow_cb, NULL,
                                 capture_output, capture_done, &sleep_ctx,
                                 &sleep_call),
              OI_OK);

    struct out_ctx check_ctx = {0};
    oi_tool_call *check_call = NULL;
    CHECK_EQ(oi_tool_call_start(reg, r, a, "fdcheck", NULL, allow_cb, NULL,
                                 capture_output, capture_done, &check_ctx,
                                 &check_call),
              OI_OK);
    run_until(r, &check_ctx.done, 100);

    CHECK(check_ctx.done);
    CHECK_EQ(check_ctx.kind, OI_TOOL_EXIT_NORMAL);
    CHECK_EQ(check_ctx.code, 0);
    oi_tool_call_cancel(sleep_call);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

TEST(tool_execution_does_not_reap_unrelated_children) {
    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);

    pid_t unrelated = fork();
    CHECK(unrelated >= 0);
    if (unrelated == 0) {
        _exit(0);
    }

    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    CHECK_EQ(oi_tool_call_start(reg, r, a, "echo", NULL, allow_cb, NULL,
                                 capture_output, capture_done, &ctx, &call),
              OI_OK);
    run_until(r, &ctx.done, 100);
    CHECK(ctx.done);

    int status = 0;
    CHECK_EQ(waitpid(unrelated, &status, 0), unrelated);
    CHECK(WIFEXITED(status));
    CHECK_EQ(WEXITSTATUS(status), 0);

    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

static void test_signal_handler(int sig) { (void)sig; }

TEST(tool_execution_preserves_signal_dispositions) {
    struct sigaction custom;
    memset(&custom, 0, sizeof custom);
    custom.sa_handler = test_signal_handler;
    sigemptyset(&custom.sa_mask);

    struct sigaction old_chld;
    struct sigaction old_pipe;
    CHECK_EQ(sigaction(SIGCHLD, &custom, &old_chld), 0);
    CHECK_EQ(sigaction(SIGPIPE, &custom, &old_pipe), 0);

    oi_tool_registry *reg = make_registry();
    oi_reactor *r = oi_reactor_create();
    oi_arena *a = oi_arena_create(0);
    struct out_ctx ctx = {0};
    oi_tool_call *call = NULL;
    CHECK_EQ(oi_tool_call_start(reg, r, a, "echo", NULL, allow_cb, NULL,
                                 capture_output, capture_done, &ctx, &call),
              OI_OK);
    run_until(r, &ctx.done, 100);
    CHECK(ctx.done);

    struct sigaction current;
    CHECK_EQ(sigaction(SIGCHLD, NULL, &current), 0);
    CHECK(current.sa_handler == test_signal_handler);
    CHECK_EQ(sigaction(SIGPIPE, NULL, &current), 0);
    CHECK(current.sa_handler == test_signal_handler);

    CHECK_EQ(sigaction(SIGCHLD, &old_chld, NULL), 0);
    CHECK_EQ(sigaction(SIGPIPE, &old_pipe, NULL), 0);
    oi_arena_destroy(a);
    oi_reactor_destroy(r);
    oi_tool_registry_destroy(reg);
}

int main(void) {
    RUN(run_echo_captures_output_and_normal_exit);
    RUN(nonzero_exit_code_reported);
    RUN(missing_binary_reports_exec_failed);
    RUN(unknown_tool_rejected_synchronously);
    RUN(permission_denied_rejected_synchronously);
    RUN(build_argv_failure_rejected_synchronously);
    RUN(ask_then_resolve_allow_runs);
    RUN(ask_then_resolve_deny);
    RUN(resolve_on_non_pending_call_rejected);
    RUN(cat_echoes_stdin);
    RUN(stdin_queue_rejects_overflow_without_corrupting_call);
    RUN(write_stdin_before_running_rejected);
    RUN(cancel_from_within_on_output);
    RUN(cancel_kills_long_running_process);
    RUN(tool_deadline_reports_timeout);
    RUN(pending_tool_deadline_starts_after_permission);
    RUN(cancel_kills_descendant_processes);
    RUN(cancel_null_safe);
    RUN(concurrent_calls_do_not_cross_wire);
    RUN(child_does_not_inherit_other_tool_descriptors);
    RUN(tool_execution_does_not_reap_unrelated_children);
    RUN(tool_execution_preserves_signal_dispositions);
    return oi_test_report();
}
