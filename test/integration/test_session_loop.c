/*
 * End-to-end integration tests: reactor + LLM client + tool execution +
 * session lifecycle, driven against the local mock API server.
 *
 * Where the unit tests exercise one module against hand-fed bytes,
 * these drive the combination the way an embedder would -- a real
 * socket, real HTTP/chunked/SSE framing, a real forked subprocess for
 * the tool, and a real on-disk session log -- with only the model
 * itself faked.
 *
 * One structural note, because it shapes these tests: the library
 * deliberately ships no turn-loop driver (see oi/session.h -- the
 * session module owns identity, arena, and log, not conversation
 * shape), so the loop below *is* the embedder. It builds each request
 * body, consumes the stream, runs the tool, and feeds the result into
 * the next turn.
 */

#include "../test.h"
#include "mock_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "oi/arena.h"
#include "oi/json.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/session.h"
#include "oi/sesslog.h"
#include "oi/tool.h"

#define TOOL_NAME "echo_tool"
#define TOOL_SCHEMA                                                          \
    "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},"  \
    "\"required\":[\"text\"]}"

/* What the mock's first turn asks the tool to emit. */
#define TOOL_TEXT "hello from tool"

/* --- shared helpers --- */

static void run_until(oi_reactor *r, const int *flag, int max_steps) {
    for (int i = 0; i < max_steps && !*flag; i++) {
        oi_status st;
        oi_reactor_step(r, 200, &st);
    }
}

static void unique_path(char *buf, size_t cap, const char *tag) {
    static unsigned seq = 0;
    snprintf(buf, cap, "/tmp/oi_itest_%ld_%s_%u", (long)getpid(), tag, seq++);
}

struct stream_sink {
    char text[1024];
    size_t len;
    int delta_count;
    int done;
    oi_status status;
    int http_status;
};

static void on_delta(const char *text, size_t len, void *ud) {
    struct stream_sink *s = ud;
    CHECK(s->len + len < sizeof s->text);
    memcpy(s->text + s->len, text, len);
    s->len += len;
    s->text[s->len] = '\0';
    s->delta_count++;
}

static void on_done(oi_status status, int http_status, const char *error_body,
                     size_t error_body_len, void *ud) {
    (void)error_body;
    (void)error_body_len;
    struct stream_sink *s = ud;
    s->status = status;
    s->http_status = http_status;
    s->done = 1;
}

/* --- request building ---
 *
 * The request body is assembled with oi/json.h's writer, which is the
 * path that matters: the tool's output is untrusted bytes going back
 * out over the wire, so its escaping has to be exercised for real
 * rather than sprintf'd into place by the test.
 */

struct msg {
    const char *role;
    const char *content;
};

/* Returns a malloc'd NUL-terminated body the caller owns. */
static char *build_body(const struct msg *msgs, size_t nmsgs,
                         size_t *out_len) {
    oi_json_writer *w = oi_json_writer_create();
    if (w == NULL) {
        return NULL;
    }

    oi_status st = oi_json_write_object_begin(w);
    st |= oi_json_write_object_key(w, "model", 5);
    st |= oi_json_write_string(w, "mock-model", 10);
    st |= oi_json_write_object_key(w, "stream", 6);
    st |= oi_json_write_bool(w, 1);
    st |= oi_json_write_object_key(w, "messages", 8);
    st |= oi_json_write_array_begin(w);
    for (size_t i = 0; i < nmsgs; i++) {
        st |= oi_json_write_object_begin(w);
        st |= oi_json_write_object_key(w, "role", 4);
        st |= oi_json_write_string(w, msgs[i].role, strlen(msgs[i].role));
        st |= oi_json_write_object_key(w, "content", 7);
        st |= oi_json_write_string(w, msgs[i].content,
                                    strlen(msgs[i].content));
        st |= oi_json_write_object_end(w);
    }
    st |= oi_json_write_array_end(w);
    st |= oi_json_write_object_end(w);
    CHECK_EQ(st, OI_OK);

    size_t len = 0;
    const char *data = oi_json_writer_data(w, &len);
    char *copy = NULL;
    if (data != NULL) {
        copy = malloc(len + 1);
        if (copy != NULL) {
            memcpy(copy, data, len + 1);
            if (out_len) {
                *out_len = len;
            }
        }
    }
    oi_json_writer_destroy(w);
    return copy;
}

/* --- the tool --- */

static char *arena_dup(oi_arena *arena, const char *s, size_t len) {
    char *out = oi_arena_alloc(arena, len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* Turns {"text": "..."} into `/bin/echo <text>`. */
static oi_status echo_build_argv(const oi_json_value *args, oi_arena *arena,
                                  void *user_data, char ***out_argv) {
    (void)user_data;

    oi_json_value *text = oi_json_object_get(args, "text");
    const char *s = NULL;
    size_t len = 0;
    if (text == NULL || oi_json_get_string(text, &s, &len) != OI_OK) {
        return OI_ERR_INVAL;
    }

    char **argv = oi_arena_alloc(arena, 3 * sizeof *argv);
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = arena_dup(arena, "/bin/echo", 9);
    argv[1] = arena_dup(arena, s, len);
    argv[2] = NULL;
    if (argv[0] == NULL || argv[1] == NULL) {
        return OI_ERR_NOMEM;
    }

    *out_argv = argv;
    return OI_OK;
}

static oi_tool_decision permission_from_flag(const char *tool_name,
                                              const oi_json_value *args,
                                              void *user_data) {
    (void)tool_name;
    (void)args;
    return *(const oi_tool_decision *)user_data;
}

struct tool_sink {
    char out[1024];
    size_t len;
    int done;
    oi_tool_exit_kind kind;
    int code;
};

static void on_tool_output(const void *data, size_t len, void *ud) {
    struct tool_sink *t = ud;
    CHECK(t->len + len < sizeof t->out);
    memcpy(t->out + t->len, data, len);
    t->len += len;
    t->out[t->len] = '\0';
}

static void on_tool_done(oi_tool_exit_kind kind, int code, void *ud) {
    struct tool_sink *t = ud;
    t->kind = kind;
    t->code = code;
    t->done = 1;
}

/* Parses `json` into `arena` and returns the root value. */
static oi_json_value *parse_into(oi_arena *arena, const char *json) {
    oi_json_parser *p = oi_json_parser_create(arena);
    if (p == NULL) {
        return NULL;
    }
    oi_json_value *root = NULL;
    if (oi_json_parser_feed(p, json, strlen(json)) == OI_OK &&
        oi_json_parser_finish(p) == OI_OK) {
        root = oi_json_parser_root(p);
    }
    oi_json_parser_destroy(p);
    return root;
}

/* Collects replayed log records so a test can assert on the whole
 * history a restart would see. */
#define REPLAY_MAX 8

struct replay_sink {
    size_t count;
    char records[REPLAY_MAX][512];
    size_t lens[REPLAY_MAX];
    int overflowed;
};

static void on_record(const void *data, size_t len, void *ud) {
    struct replay_sink *rs = ud;
    if (rs->count >= REPLAY_MAX || len >= sizeof rs->records[0]) {
        rs->overflowed = 1;
        rs->count++;
        return;
    }
    if (len > 0) {
        memcpy(rs->records[rs->count], data, len);
    }
    rs->records[rs->count][len] = '\0';
    rs->lens[rs->count] = len;
    rs->count++;
}

/* ================= the full tool-use loop ================= */

/*
 * Turn 1's stream carries content deltas *and* a tool_calls delta. The
 * client documents that it forwards assistant text only and treats a
 * tool-call-only chunk as "nothing to do" -- so this asserts the text
 * arrives intact and the tool-call chunk neither errors nor produces a
 * spurious delta.
 *
 * That documented behavior is also why the tool arguments below are
 * supplied by the test rather than read out of the stream: the client
 * exposes no way for an embedder to see a tool_calls delta, so the
 * model-to-tool hop cannot be closed through the public API today.
 * Everything after that hop -- argument parsing, permission check,
 * subprocess execution, logging, and feeding the result into turn 2 --
 * is the real code path.
 */
TEST(full_tool_use_loop) {
    const char *turn1 =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
        "\"content\":\"Let me \"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"check.\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{"
        "\"index\":0,\"id\":\"call_1\",\"type\":\"function\",\"function\":{"
        "\"name\":\"" TOOL_NAME "\",\"arguments\":\"{\\\"text\\\":\\\"" TOOL_TEXT
        "\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
        "\"tool_calls\"}]}\n\n"
        "data: [DONE]\n\n";
    const char *turn2 =
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":"
        "\"The tool said: \"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" TOOL_TEXT
        "\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
        "\"stop\"}]}\n\n"
        "data: [DONE]\n\n";

    /* A deliberately small chunk size so SSE events straddle HTTP chunk
     * boundaries -- the framing layers have to reassemble them. */
    struct mock_turn turns[2] = {{NULL, turn1, 13}, {NULL, turn2, 0}};
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 2));

    char log_path[128];
    unique_path(log_path, sizeof log_path, "loop");

    oi_reactor *r = oi_reactor_create();
    oi_session_registry *reg = oi_session_registry_create();
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(r != NULL && reg != NULL && tools != NULL);

    CHECK_EQ(oi_tool_registry_add(tools, TOOL_NAME, TOOL_SCHEMA,
                                   echo_build_argv, NULL),
              OI_OK);
    /* The registry hands the schema back verbatim for the caller to put
     * in the request's "tools" field. */
    CHECK(oi_tool_registry_schema(tools, TOOL_NAME) != NULL);
    CHECK_STREQ(oi_tool_registry_schema(tools, TOOL_NAME), TOOL_SCHEMA);

    oi_session *session = NULL;
    CHECK_EQ(oi_session_create(reg, "s1", log_path, 0, &session), OI_OK);
    CHECK(session != NULL);
    oi_arena *arena = oi_session_arena(session);
    oi_sesslog *log = oi_session_log(session);
    CHECK(arena != NULL && log != NULL);

    struct oi_llm_config cfg = {"127.0.0.1", api.port, 0, NULL, NULL,
                                 "/v1/chat/completions", 0};
    oi_llm_client *client = oi_llm_client_create(&cfg);
    CHECK(client != NULL);

    /* --- turn 1: user prompt -> assistant text + a tool request --- */

    struct msg turn1_msgs[] = {{"user", "Run the echo tool."}};
    size_t body_len = 0;
    char *body = build_body(turn1_msgs, 1, &body_len);
    CHECK(body != NULL);
    CHECK_EQ(oi_sesslog_append(log, body, body_len), OI_OK);

    struct stream_sink s1 = {0};
    oi_llm_request *req = NULL;
    CHECK_EQ(oi_llm_request_start(client, r, arena, body, body_len, on_delta,
                                   on_done, &s1, &req),
              OI_OK);
    run_until(r, &s1.done, 200);
    free(body);

    CHECK(s1.done);
    CHECK_EQ(s1.status, OI_OK);
    CHECK_EQ(s1.http_status, 200);
    CHECK_STREQ(s1.text, "Let me check.");
    CHECK_EQ(s1.delta_count, 2); /* the tool_calls chunk contributes none */
    CHECK_EQ(oi_sesslog_append(log, s1.text, s1.len), OI_OK);

    /* --- run the requested tool --- */

    oi_json_value *args = parse_into(arena, "{\"text\":\"" TOOL_TEXT "\"}");
    CHECK(args != NULL);

    oi_tool_decision decision = OI_TOOL_ALLOW;
    struct tool_sink t = {0};
    oi_tool_call *call = NULL;
    CHECK_EQ(oi_tool_call_start(tools, r, arena, TOOL_NAME, args,
                                 permission_from_flag, &decision,
                                 on_tool_output, on_tool_done, &t, &call),
              OI_OK);
    run_until(r, &t.done, 200);

    CHECK(t.done);
    CHECK_EQ(t.kind, OI_TOOL_EXIT_NORMAL);
    CHECK_EQ(t.code, 0);
    CHECK_STREQ(t.out, TOOL_TEXT "\n");
    CHECK_EQ(oi_sesslog_append(log, t.out, t.len), OI_OK);

    /* --- turn 2: the tool result goes back to the model --- */

    struct msg turn2_msgs[] = {{"user", "Run the echo tool."},
                                {"assistant", "Let me check."},
                                {"tool", TOOL_TEXT "\n"}};
    body = build_body(turn2_msgs, 3, &body_len);
    CHECK(body != NULL);

    struct stream_sink s2 = {0};
    CHECK_EQ(oi_llm_request_start(client, r, arena, body, body_len, on_delta,
                                   on_done, &s2, &req),
              OI_OK);
    run_until(r, &s2.done, 200);
    free(body);

    CHECK(s2.done);
    CHECK_EQ(s2.status, OI_OK);
    CHECK_STREQ(s2.text, "The tool said: " TOOL_TEXT);

    /* The mock captured what actually went over the wire: the second
     * request must carry the tool's output, JSON-escaped. */
    char *captured = mock_api_request(&api, 1, NULL);
    CHECK(captured != NULL);
    if (captured != NULL) {
        CHECK(strstr(captured, TOOL_TEXT) != NULL);
        CHECK(strstr(captured, "\\n") != NULL); /* the newline was escaped */
        CHECK(strstr(captured, "\"role\":\"tool\"") != NULL);
        free(captured);
    }

    /* --- the log holds the whole turn, in order --- */

    oi_session_destroy(reg, session); /* releases the log's flock */

    oi_sesslog *reopened = NULL;
    CHECK_EQ(oi_sesslog_open(log_path, &reopened), OI_OK);
    struct replay_sink rs = {0};
    CHECK_EQ(oi_sesslog_replay(reopened, on_record, &rs), OI_OK);
    CHECK(!rs.overflowed);
    CHECK_EQ(rs.count, 3u); /* request, assistant text, tool output */
    if (rs.count == 3 && !rs.overflowed) {
        CHECK(strstr(rs.records[0], "\"role\":\"user\"") != NULL);
        CHECK_STREQ(rs.records[1], "Let me check.");
        CHECK_STREQ(rs.records[2], TOOL_TEXT "\n");
    }
    oi_sesslog_close(reopened);

    oi_llm_client_destroy(client);
    oi_tool_registry_destroy(tools);
    oi_session_registry_destroy(reg);
    oi_reactor_destroy(r);
    mock_api_stop(&api);
    unlink(log_path);
}

/* ================= permission policy ================= */

/* A denied tool must never reach fork/exec, and must report
 * synchronously rather than through the completion callback. */
TEST(denied_tool_never_executes) {
    oi_reactor *r = oi_reactor_create();
    oi_arena *arena = oi_arena_create(0);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK(r != NULL && arena != NULL && tools != NULL);
    CHECK_EQ(oi_tool_registry_add(tools, TOOL_NAME, TOOL_SCHEMA,
                                   echo_build_argv, NULL),
              OI_OK);

    oi_json_value *args = parse_into(arena, "{\"text\":\"" TOOL_TEXT "\"}");
    CHECK(args != NULL);

    oi_tool_decision decision = OI_TOOL_DENY;
    struct tool_sink t = {0};
    oi_tool_call *call = NULL;
    CHECK_EQ(oi_tool_call_start(tools, r, arena, TOOL_NAME, args,
                                 permission_from_flag, &decision,
                                 on_tool_output, on_tool_done, &t, &call),
              OI_ERR_DENIED);

    /* Give the reactor a chance to dispatch anything that was wrongly
     * queued; nothing should have been. */
    int never = 0;
    run_until(r, &never, 5);
    CHECK(!t.done);
    CHECK_EQ(t.len, 0u);

    /* An unregistered name is a distinct, equally synchronous failure. */
    CHECK_EQ(oi_tool_call_start(tools, r, arena, "no_such_tool", args,
                                 permission_from_flag, &decision,
                                 on_tool_output, on_tool_done, &t, &call),
              OI_ERR_NOTFOUND);

    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(r);
}

/* A deferred decision runs nothing until it is resolved. */
TEST(deferred_permission_resolves) {
    oi_reactor *r = oi_reactor_create();
    oi_arena *arena = oi_arena_create(0);
    oi_tool_registry *tools = oi_tool_registry_create();
    CHECK_EQ(oi_tool_registry_add(tools, TOOL_NAME, TOOL_SCHEMA,
                                   echo_build_argv, NULL),
              OI_OK);

    oi_json_value *args = parse_into(arena, "{\"text\":\"" TOOL_TEXT "\"}");
    CHECK(args != NULL);

    oi_tool_decision decision = OI_TOOL_ASK;
    struct tool_sink t = {0};
    oi_tool_call *call = NULL;
    CHECK_EQ(oi_tool_call_start(tools, r, arena, TOOL_NAME, args,
                                 permission_from_flag, &decision,
                                 on_tool_output, on_tool_done, &t, &call),
              OI_OK);
    CHECK(call != NULL);

    int never = 0;
    run_until(r, &never, 5);
    CHECK(!t.done); /* still awaiting the decision */

    CHECK_EQ(oi_tool_call_resolve(call, 1), OI_OK);
    run_until(r, &t.done, 200);
    CHECK(t.done);
    CHECK_EQ(t.kind, OI_TOOL_EXIT_NORMAL);
    CHECK_STREQ(t.out, TOOL_TEXT "\n");

    oi_tool_registry_destroy(tools);
    oi_arena_destroy(arena);
    oi_reactor_destroy(r);
}

/* ================= fault containment ================= */

/*
 * PLAN.md's claim is that a session-level fault tears down that
 * session's resources and leaves the reactor and every other session
 * running. This drives a real API error through the client, fails the
 * session on it, and checks the neighbouring session is untouched.
 */
TEST(api_error_fails_only_its_own_session) {
    const char *body_500 = "internal error";
    struct mock_turn turns[1] = {
        {"HTTP/1.1 500 Internal Server Error", body_500, 0}};
    struct mock_api api;
    CHECK(mock_api_start(&api, turns, 1));

    char path_a[128];
    char path_b[128];
    unique_path(path_a, sizeof path_a, "fault_a");
    unique_path(path_b, sizeof path_b, "fault_b");

    oi_reactor *r = oi_reactor_create();
    oi_session_registry *reg = oi_session_registry_create();
    oi_session *a = NULL;
    oi_session *b = NULL;
    CHECK_EQ(oi_session_create(reg, "a", path_a, 0, &a), OI_OK);
    CHECK_EQ(oi_session_create(reg, "b", path_b, 0, &b), OI_OK);

    struct oi_llm_config cfg = {"127.0.0.1", api.port, 0, NULL, NULL,
                                 "/v1/chat/completions", 0};
    oi_llm_client *client = oi_llm_client_create(&cfg);
    CHECK(client != NULL);

    struct msg msgs[] = {{"user", "hi"}};
    size_t body_len = 0;
    char *body = build_body(msgs, 1, &body_len);
    CHECK(body != NULL);

    struct stream_sink s = {0};
    oi_llm_request *req = NULL;
    CHECK_EQ(oi_llm_request_start(client, r, oi_session_arena(a), body,
                                   body_len, on_delta, on_done, &s, &req),
              OI_OK);
    run_until(r, &s.done, 200);
    free(body);

    CHECK(s.done);
    CHECK(s.status != OI_OK);
    CHECK_EQ(s.http_status, 500);
    CHECK_EQ(s.delta_count, 0);

    /* The embedder decides this session is unrecoverable. */
    oi_session_fail(a);
    CHECK_EQ(oi_session_state_of(a), OI_SESSION_FAILED);
    CHECK(oi_session_arena(a) == NULL);
    CHECK(oi_session_log(a) == NULL);
    /* Still registered, for diagnostics. */
    CHECK(oi_session_lookup(reg, "a") == a);

    /* The neighbour is untouched and still usable. */
    CHECK_EQ(oi_session_state_of(b), OI_SESSION_ACTIVE);
    CHECK(oi_session_arena(b) != NULL);
    CHECK_EQ(oi_sesslog_append(oi_session_log(b), "still here", 10), OI_OK);

    oi_llm_client_destroy(client);
    oi_session_registry_destroy(reg);
    oi_reactor_destroy(r);
    mock_api_stop(&api);
    unlink(path_a);
    unlink(path_b);
}

/* ================= restart / resume ================= */

/*
 * The point of the durable log: a process that dies mid-conversation
 * can reopen the same log by session ID and replay what it had.
 */
TEST(session_resumes_from_its_log) {
    char log_path[128];
    unique_path(log_path, sizeof log_path, "resume");

    oi_session_registry *reg = oi_session_registry_create();
    oi_session *session = NULL;
    CHECK_EQ(oi_session_create(reg, "s", log_path, 0, &session), OI_OK);
    CHECK_EQ(oi_sesslog_append(oi_session_log(session), "first", 5), OI_OK);
    CHECK_EQ(oi_sesslog_append(oi_session_log(session), "second", 6), OI_OK);

    /* A duplicate ID is refused rather than silently aliasing. */
    oi_session *dup = NULL;
    CHECK_EQ(oi_session_create(reg, "s", log_path, 0, &dup), OI_ERR_EXISTS);

    /* Simulate the restart. */
    oi_session_registry_destroy(reg);

    oi_session_registry *reg2 = oi_session_registry_create();
    oi_session *resumed = NULL;
    CHECK_EQ(oi_session_create(reg2, "s", log_path, 0, &resumed), OI_OK);
    CHECK(oi_session_lookup(reg2, "s") == resumed);

    struct replay_sink rs = {0};
    CHECK_EQ(oi_sesslog_replay(oi_session_log(resumed), on_record, &rs),
              OI_OK);
    CHECK(!rs.overflowed);
    CHECK_EQ(rs.count, 2u);
    if (rs.count == 2 && !rs.overflowed) {
        CHECK_STREQ(rs.records[0], "first");
        CHECK_STREQ(rs.records[1], "second");
    }

    /* Resuming appends to the existing history rather than truncating. */
    CHECK_EQ(oi_sesslog_append(oi_session_log(resumed), "third", 5), OI_OK);
    struct replay_sink rs2 = {0};
    CHECK_EQ(oi_sesslog_replay(oi_session_log(resumed), on_record, &rs2),
              OI_OK);
    CHECK_EQ(rs2.count, 3u);

    oi_session_registry_destroy(reg2);
    unlink(log_path);
}

int main(void) {
    RUN(full_tool_use_loop);
    RUN(denied_tool_never_executes);
    RUN(deferred_permission_resolves);
    RUN(api_error_fails_only_its_own_session);
    RUN(session_resumes_from_its_log);
    return oi_test_report();
}
