#include "cli_command_dispatch.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static char *read_stream(FILE *stream) {
    long len;
    char *text;

    CHECK_EQ(fflush(stream), 0);
    CHECK_EQ(fseek(stream, 0, SEEK_END), 0);
    len = ftell(stream);
    CHECK(len >= 0);
    CHECK_EQ(fseek(stream, 0, SEEK_SET), 0);
    text = malloc((size_t)len + 1);
    CHECK(text != NULL);
    if (text == NULL) {
        return NULL;
    }
    CHECK_EQ(fread(text, 1, (size_t)len, stream), (size_t)len);
    text[len] = '\0';
    return text;
}

static oi_status dispatch(
    const char *text, struct oi_cli_command_context *context,
    enum oi_cli_command_result *result) {
    struct oi_cli_command_parse parsed;
    oi_status status =
        oi_cli_command_parse_text(text, strlen(text), &parsed);
    return status == OI_OK
               ? oi_cli_command_dispatch(&parsed, context, result)
               : status;
}

TEST(help_lists_commands_and_exit_requests_exit) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "test-model",
        .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK(out != NULL);
    CHECK(err != NULL);
    CHECK_EQ(dispatch("/help", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    output = read_stream(out);
    CHECK(strstr(output, "/permissions") != NULL);
    CHECK(strstr(output, "Ctrl+J") != NULL);
    free(output);
    CHECK_EQ(dispatch("/exit", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_EXIT_REPL);
    fclose(out);
    fclose(err);
}

TEST(help_with_an_argument_prints_usage) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "test-model",
        .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *text;

    CHECK(out != NULL);
    CHECK(err != NULL);
    CHECK_EQ(dispatch("/help model", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    text = read_stream(err);
    CHECK_STREQ(text, "oi: usage: /help\n");
    free(text);
    text = read_stream(out);
    CHECK_STREQ(text, "");
    free(text);
    fclose(out);
    fclose(err);
}

TEST(permissions_are_process_scoped_and_validated) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "test-model",
        .permission = &permission, .session_id = "session-1",
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/permissions allow", &context, &result), OI_OK);
    CHECK_EQ(permission.policy, OI_CLI_TOOLS_ALLOW);
    CHECK_EQ(dispatch("/permissions invalid", &context, &result), OI_OK);
    CHECK_EQ(permission.policy, OI_CLI_TOOLS_ALLOW);
    errors = read_stream(err);
    CHECK(strstr(errors, "usage") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

struct status_cb_call {
    int call_count;
    oi_status result;
};

static oi_status fake_status(void *user_data,
                             struct oi_cli_status_snapshot *out_snapshot) {
    struct status_cb_call *call = user_data;

    call->call_count++;
    if (call->result != OI_OK) {
        return call->result;
    }
    out_snapshot->session_state = OI_CLI_STATUS_SESSION_ACTIVE;
    out_snapshot->session_id = "snapshot-session";
    out_snapshot->model = "snapshot-model";
    out_snapshot->model_origin = OI_CLI_SESSION_MODEL_DEFAULT;
    out_snapshot->endpoint.host = "endpoint.invalid";
    out_snapshot->endpoint.path = "/v1/chat/completions";
    out_snapshot->endpoint.port = 8443;
    out_snapshot->endpoint.use_tls = 1;
    out_snapshot->permission = OI_CLI_STATUS_PERMISSION_DENY;
    out_snapshot->request_timeout_ms = 1500;
    out_snapshot->tool_timeout_ms = 2500;
    out_snapshot->cwd = "/snapshot/cwd";
    out_snapshot->conversation = OI_CLI_CONVERSATION_ACTIVITY_TOOL_RUNNING;
    out_snapshot->queue = OI_CLI_STATUS_QUEUE_MESSAGE;
    out_snapshot->queue_bytes = 11;
    out_snapshot->checkpoint.known = 1;
    return OI_OK;
}

static oi_status silent_status(void *user_data,
                              struct oi_cli_status_snapshot *out_snapshot) {
    struct status_cb_call *call = user_data;

    (void)out_snapshot;
    call->call_count++;
    return OI_OK;
}

TEST(status_reports_the_assembled_snapshot) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct status_cb_call call = {0, OI_OK};
    struct oi_cli_command_context context = {
        .out = out,
        .err = err,
        .model = "context-model",
        .permission = &permission,
        .session_id = "context-session",
        .status = fake_status,
        .status_user_data = &call,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/status", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);
    output = read_stream(out);
    CHECK(strstr(output, "Session: snapshot-session") != NULL);
    CHECK(strstr(output, "Model: snapshot-model (startup default)") != NULL);
    CHECK(strstr(output, "Endpoint: endpoint.invalid:8443"
                         "/v1/chat/completions (TLS on)") != NULL);
    CHECK(strstr(output, "Permissions: deny") != NULL);
    CHECK(strstr(output, "Request timeout: 1500 ms") != NULL);
    CHECK(strstr(output, "Tool timeout: 2500 ms") != NULL);
    CHECK(strstr(output, "CWD: /snapshot/cwd") != NULL);
    CHECK(strstr(output, "Conversation: tool running") != NULL);
    CHECK(strstr(output, "Queue: 1 message queued (11 bytes)") != NULL);
    CHECK(strstr(output, "Checkpoint: none") != NULL);
    CHECK(strstr(output, "Context: not compacted") != NULL);
    CHECK(strstr(output, "context-model") == NULL);
    CHECK(strstr(output, "context-session") == NULL);
    free(output);
    fclose(out);
    fclose(err);
}

TEST(status_reports_unknown_for_everything_a_callback_leaves_alone) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ALLOW};
    struct status_cb_call call = {0, OI_OK};
    struct oi_cli_command_context context = {
        .out = out,
        .err = err,
        .model = "context-model",
        .permission = &permission,
        .session_id = "context-session",
        .status = silent_status,
        .status_user_data = &call,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/status", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);
    output = read_stream(out);
    CHECK_STREQ(output,
                "Session: (unknown)\n"
                "Model: (unknown)\n"
                "Endpoint: (unknown)\n"
                "Permissions: (unknown)\n"
                "Request timeout: (unknown)\n"
                "Tool timeout: (unknown)\n"
                "CWD: (unknown)\n"
                "Conversation: (unknown)\n"
                "Queue: (unknown)\n"
                "Checkpoint: (unknown)\n"
                "Context: (unknown)\n"
                "Tokens: (unknown)\n");
    free(output);
    fclose(out);
    fclose(err);
}

TEST(status_rejects_arguments_and_survives_an_unavailable_snapshot) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct status_cb_call call = {0, OI_ERR_IO};
    struct oi_cli_command_context context = {
        .out = out,
        .err = err,
        .model = "context-model",
        .permission = &permission,
        .status = fake_status,
        .status_user_data = &call,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/status", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    CHECK_EQ(call.call_count, 1);

    context.status = NULL;
    CHECK_EQ(dispatch("/status", &context, &result), OI_OK);

    CHECK_EQ(dispatch("/status extra", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);

    errors = read_stream(err);
    CHECK(strstr(errors, "could not read the current status") != NULL);
    CHECK(strstr(errors, "/status is not available in this context") != NULL);
    CHECK(strstr(errors, "usage: /status") != NULL);
    free(errors);
    {
        char *output = read_stream(out);
        CHECK_STREQ(output, "");
        free(output);
    }
    fclose(out);
    fclose(err);
}

struct model_cb_call {
    char name[64];
    size_t name_len;
    int call_count;
    oi_status result;
};

static oi_status record_set_model(void *user_data, const char *name,
                                  size_t name_len) {
    struct model_cb_call *call = user_data;
    call->call_count++;
    CHECK(name_len < sizeof call->name);
    memcpy(call->name, name, name_len);
    call->name[name_len] = '\0';
    call->name_len = name_len;
    return call->result;
}

TEST(model_with_no_argument_prints_the_active_model) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/model", &context, &result), OI_OK);
    output = read_stream(out);
    CHECK_STREQ(output, "Model: current-model\n");
    free(output);
    fclose(out);
    fclose(err);
}

TEST(model_with_no_callback_reports_unavailable) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/model new-model", &context, &result), OI_OK);
    errors = read_stream(err);
    CHECK(strstr(errors, "not available") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

TEST(model_with_argument_invokes_the_callback_with_exact_bytes) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct model_cb_call call = {.result = OI_OK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission, .set_model = record_set_model,
        .set_model_user_data = &call,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/model gpt-new", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);
    CHECK_STREQ(call.name, "gpt-new");
    output = read_stream(out);
    CHECK_STREQ(output, "Model: gpt-new\n");
    free(output);
    fclose(out);
    fclose(err);
}

TEST(model_callback_failure_is_reported_and_stays_in_the_repl) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct model_cb_call call = {.result = OI_ERR_INVAL};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission, .set_model = record_set_model,
        .set_model_user_data = &call,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/model bad-model", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);
    errors = read_stream(err);
    CHECK(strstr(errors, "could not change the model") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

TEST(model_callback_structural_failure_propagates) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct model_cb_call call = {.result = OI_ERR_IO};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission, .set_model = record_set_model,
        .set_model_user_data = &call,
    };
    enum oi_cli_command_result result;

    CHECK_EQ(dispatch("/model gpt-new", &context, &result), OI_ERR_IO);
    fclose(out);
    fclose(err);
}

TEST(model_rejects_an_oversized_name_without_calling_back) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct model_cb_call call = {.result = OI_OK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission, .set_model = record_set_model,
        .set_model_user_data = &call,
    };
    enum oi_cli_command_result result;
    char oversized[300] = "/model ";
    char *errors;
    size_t i;

    for (i = strlen(oversized); i < sizeof oversized - 1; i++) {
        oversized[i] = 'a';
    }
    oversized[sizeof oversized - 1] = '\0';

    CHECK_EQ(dispatch(oversized, &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 0);
    errors = read_stream(err);
    CHECK(strstr(errors, "too long") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

struct cwd_cb_call {
    char path[512];
    size_t path_len;
    int call_count;
    oi_status result;
};

static oi_status record_set_cwd(void *user_data, const char *path,
                                size_t path_len) {
    struct cwd_cb_call *call = user_data;
    call->call_count++;
    CHECK(path_len < sizeof call->path);
    memcpy(call->path, path, path_len);
    call->path[path_len] = '\0';
    call->path_len = path_len;
    return call->result;
}

TEST(cwd_with_no_argument_prints_the_process_cwd) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/cwd", &context, &result), OI_OK);
    output = read_stream(out);
    CHECK(strstr(output, "CWD:") != NULL);
    free(output);
    fclose(out);
    fclose(err);
}

TEST(cwd_with_no_callback_reports_unavailable) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/cwd /tmp", &context, &result), OI_OK);
    errors = read_stream(err);
    CHECK(strstr(errors, "not available") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

TEST(cwd_with_argument_invokes_the_callback_with_exact_bytes) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct cwd_cb_call call = {.result = OI_OK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission, .set_cwd = record_set_cwd,
        .set_cwd_user_data = &call,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/cwd /tmp/project", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);
    CHECK_STREQ(call.path, "/tmp/project");
    output = read_stream(out);
    CHECK(strstr(output, "CWD:") != NULL);
    free(output);
    fclose(out);
    fclose(err);
}

TEST(cwd_callback_failure_is_reported_and_stays_in_the_repl) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct cwd_cb_call call = {.result = OI_ERR_INVAL};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "current-model",
        .permission = &permission, .set_cwd = record_set_cwd,
        .set_cwd_user_data = &call,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/cwd /nonexistent", &context, &result), OI_OK);
    errors = read_stream(err);
    CHECK(strstr(errors, "could not change the working directory") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

struct session_cb_state {
    char id[128];
    size_t id_len;
    char name[128];
    size_t name_len;
    const char *last_operation;
    int call_count;
    oi_status result;
    const char *detail;
    int entry_count;
    int list_result_error;
};

static oi_status fake_list(void *user_data,
                          struct oi_cli_session_list *out_list) {
    struct session_cb_state *state = user_data;
    int index;

    state->call_count++;
    state->last_operation = "list";
    if (state->list_result_error) {
        return OI_ERR_IO;
    }
    for (index = 0; index < state->entry_count; index++) {
        struct oi_cli_session_list_entry entry;
        char id[32];

        memset(&entry, 0, sizeof entry);
        snprintf(id, sizeof id, "sess-%d", index);
        entry.id = strdup(id);
        CHECK(entry.id != NULL);
        CHECK_EQ(oi_cli_string_set(&entry.model, "gpt-test", 8), OI_OK);
        entry.updated_at = 1785262200 + index;
        if (index == 1) {
            CHECK_EQ(oi_cli_string_set(&entry.display_name, "named one", 9),
                     OI_OK);
            entry.lock_state = OI_CLI_SESSION_LOCK_BUSY;
        }
        if (index == 2) {
            entry.degraded = 1;
            oi_cli_string_free(&entry.model);
        }
        {
            struct oi_cli_session_list_entry *grown =
                realloc(out_list->entries,
                        (out_list->len + 1) * sizeof *grown);
            CHECK(grown != NULL);
            out_list->entries = grown;
            out_list->cap = out_list->len + 1;
            out_list->entries[out_list->len++] = entry;
        }
    }
    return OI_OK;
}

static oi_status fake_current(
    void *user_data, struct oi_cli_command_session_current *out_current) {
    struct session_cb_state *state = user_data;
    state->call_count++;
    state->last_operation = "current";
    if (state->result != OI_OK) {
        return state->result;
    }
    out_current->id = "sess-current";
    out_current->path = "/tmp/sessions/sess-current";
    out_current->healthy = state->entry_count > 0;
    return OI_OK;
}

static oi_status fake_rename(void *user_data, const char *id, size_t id_len,
                            const char *name, size_t name_len,
                            char **out_error_detail) {
    struct session_cb_state *state = user_data;
    state->call_count++;
    state->last_operation = "rename";
    CHECK(id_len < sizeof state->id);
    CHECK(name_len < sizeof state->name);
    memcpy(state->id, id, id_len);
    state->id[id_len] = '\0';
    state->id_len = id_len;
    memcpy(state->name, name, name_len);
    state->name[name_len] = '\0';
    state->name_len = name_len;
    if (state->result != OI_OK && state->detail != NULL) {
        *out_error_detail = strdup(state->detail);
    }
    return state->result;
}

static oi_status fake_admin(void *user_data, const char *id, size_t id_len,
                           char **out_error_detail) {
    struct session_cb_state *state = user_data;
    state->call_count++;
    state->last_operation = "admin";
    CHECK(id_len < sizeof state->id);
    memcpy(state->id, id, id_len);
    state->id[id_len] = '\0';
    state->id_len = id_len;
    if (state->result != OI_OK && state->detail != NULL) {
        *out_error_detail = strdup(state->detail);
    }
    return state->result;
}

static struct oi_cli_command_session_ops wired_ops(
    struct session_cb_state *state) {
    struct oi_cli_command_session_ops ops;
    memset(&ops, 0, sizeof ops);
    ops.user_data = state;
    ops.list = fake_list;
    ops.list_trashed = fake_list;
    ops.current = fake_current;
    ops.rename = fake_rename;
    ops.trash = fake_admin;
    ops.restore = fake_admin;
    return ops;
}

TEST(session_list_renders_every_entry_state) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct session_cb_state state;
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "m", .permission = &permission,
        .session_id = "sess-0",
    };
    enum oi_cli_command_result result;
    char *output;

    memset(&state, 0, sizeof state);
    state.entry_count = 3;
    context.session = wired_ops(&state);

    CHECK_EQ(dispatch("/session", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    CHECK_STREQ(state.last_operation, "list");
    output = read_stream(out);
    CHECK(strstr(output, "Sessions (3):") != NULL);
    CHECK(strstr(output, "* sess-0") != NULL);
    CHECK(strstr(output, "  sess-1") != NULL);
    CHECK(strstr(output, "\"named one\"") != NULL);
    CHECK(strstr(output, "[open elsewhere]") != NULL);
    CHECK(strstr(output, "[metadata rebuilt]") != NULL);
    CHECK(strstr(output, "model gpt-test") != NULL);
    CHECK(strstr(output, "model unknown") != NULL);
    free(output);

    CHECK_EQ(dispatch("/session list", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session trash-list", &context, &result), OI_OK);
    output = read_stream(out);
    CHECK(strstr(output, "Trashed sessions (3):") != NULL);
    free(output);

    fclose(out);
    fclose(err);
}

TEST(session_list_reports_an_empty_directory_and_a_read_failure) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct session_cb_state state;
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "m", .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *text;

    memset(&state, 0, sizeof state);
    context.session = wired_ops(&state);

    CHECK_EQ(dispatch("/session list", &context, &result), OI_OK);
    text = read_stream(out);
    CHECK(strstr(text, "Sessions: none") != NULL);
    free(text);

    state.list_result_error = 1;
    CHECK_EQ(dispatch("/session list", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    text = read_stream(err);
    CHECK(strstr(text, "could not read the sessions directory") != NULL);
    free(text);

    fclose(out);
    fclose(err);
}

TEST(session_current_reports_health_live) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct session_cb_state state;
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "m", .permission = &permission,
        .session_id = "sess-current",
    };
    enum oi_cli_command_result result;
    char *text;

    memset(&state, 0, sizeof state);
    state.entry_count = 1;
    context.session = wired_ops(&state);

    CHECK_EQ(dispatch("/session current", &context, &result), OI_OK);
    text = read_stream(out);
    CHECK(strstr(text, "Session: sess-current") != NULL);
    CHECK(strstr(text, "Path: /tmp/sessions/sess-current") != NULL);
    CHECK(strstr(text, "Status: healthy") != NULL);
    free(text);

    state.entry_count = 0;
    CHECK_EQ(dispatch("/session current", &context, &result), OI_OK);
    text = read_stream(out);
    CHECK(strstr(text, "Status: degraded") != NULL);
    CHECK(strstr(text, "rebuilt from history") != NULL);
    free(text);

    fclose(out);
    fclose(err);
}

TEST(session_rename_passes_exact_bytes_and_reports_details) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct session_cb_state state;
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "m", .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *text;

    memset(&state, 0, sizeof state);
    context.session = wired_ops(&state);

    CHECK_EQ(dispatch("/session rename sess-7 my long name", &context,
                      &result),
             OI_OK);
    CHECK_STREQ(state.id, "sess-7");
    CHECK_STREQ(state.name, "my long name");
    text = read_stream(out);
    CHECK(strstr(text, "Renamed session sess-7") != NULL);
    free(text);

    state.result = OI_ERR_INVAL;
    state.detail = "a session name cannot contain control characters";
    CHECK_EQ(dispatch("/session rename sess-7 bad", &context, &result),
             OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    text = read_stream(err);
    CHECK(strstr(text, "control characters") != NULL);
    free(text);

    state.call_count = 0;
    CHECK_EQ(dispatch("/session rename", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session rename only-an-id", &context, &result),
             OI_OK);
    CHECK_EQ(state.call_count, 0);
    text = read_stream(err);
    CHECK(strstr(text, "usage: /session rename ID NAME") != NULL);
    free(text);

    fclose(out);
    fclose(err);
}

TEST(session_trash_and_restore_take_exactly_one_id) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct session_cb_state state;
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "m", .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *text;

    memset(&state, 0, sizeof state);
    context.session = wired_ops(&state);

    CHECK_EQ(dispatch("/session trash sess-9", &context, &result), OI_OK);
    CHECK_STREQ(state.id, "sess-9");
    text = read_stream(out);
    CHECK(strstr(text, "Trashed session sess-9") != NULL);
    free(text);

    CHECK_EQ(dispatch("/session restore sess-9", &context, &result), OI_OK);
    text = read_stream(out);
    CHECK(strstr(text, "Restored session sess-9") != NULL);
    free(text);

    state.result = OI_ERR_INVAL;
    state.detail = "cannot trash the active session -- switch away first";
    CHECK_EQ(dispatch("/session trash sess-9", &context, &result), OI_OK);
    text = read_stream(err);
    CHECK(strstr(text, "switch away first") != NULL);
    free(text);

    state.call_count = 0;
    CHECK_EQ(dispatch("/session trash", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session trash sess-9 extra", &context, &result),
             OI_OK);
    CHECK_EQ(state.call_count, 0);
    text = read_stream(err);
    CHECK(strstr(text, "usage: /session trash ID") != NULL);
    free(text);

    fclose(out);
    fclose(err);
}

TEST(session_reports_unavailable_operations_and_bad_subcommands) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "m", .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *text;

    CHECK_EQ(dispatch("/session list", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session current", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session rename a b", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session trash a", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session restore a", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    text = read_stream(err);
    CHECK(strstr(text, "/session list is not available") != NULL);
    CHECK(strstr(text, "/session current is not available") != NULL);
    CHECK(strstr(text, "/session rename is not available") != NULL);
    CHECK(strstr(text, "/session trash is not available") != NULL);
    CHECK(strstr(text, "/session restore is not available") != NULL);
    free(text);

    CHECK_EQ(dispatch("/session frobnicate", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session current extra", &context, &result), OI_OK);
    CHECK_EQ(dispatch("/session list extra", &context, &result), OI_OK);
    text = read_stream(err);
    CHECK(strstr(text, "usage: /session") != NULL);
    free(text);

    fclose(out);
    fclose(err);
}

TEST(repl_owned_commands_are_a_caller_error) {
    static const char *const commands[] = {
        "/compact", "/compact 4", "/login", "/session switch sess-1",
        "/session delete sess-1", "/session import /tmp/a dir/b.oilog",
    };
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        .out = out, .err = err, .model = "m", .permission = &permission,
    };
    enum oi_cli_command_result result;
    char *text;

    CHECK(out != NULL);
    CHECK(err != NULL);
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++) {
        result = OI_CLI_COMMAND_EXIT_REPL;
        CHECK_EQ(dispatch(commands[i], &context, &result), OI_ERR_INVAL);
        CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    }
    text = read_stream(err);
    CHECK_STREQ(text, "");
    free(text);
    text = read_stream(out);
    CHECK_STREQ(text, "");
    free(text);
    fclose(out);
    fclose(err);
}

int main(void) {
    RUN(help_lists_commands_and_exit_requests_exit);
    RUN(help_with_an_argument_prints_usage);
    RUN(repl_owned_commands_are_a_caller_error);
    RUN(permissions_are_process_scoped_and_validated);
    RUN(status_reports_the_assembled_snapshot);
    RUN(status_reports_unknown_for_everything_a_callback_leaves_alone);
    RUN(status_rejects_arguments_and_survives_an_unavailable_snapshot);
    RUN(model_with_no_argument_prints_the_active_model);
    RUN(model_with_no_callback_reports_unavailable);
    RUN(model_with_argument_invokes_the_callback_with_exact_bytes);
    RUN(model_callback_failure_is_reported_and_stays_in_the_repl);
    RUN(model_callback_structural_failure_propagates);
    RUN(model_rejects_an_oversized_name_without_calling_back);
    RUN(cwd_with_no_argument_prints_the_process_cwd);
    RUN(cwd_with_no_callback_reports_unavailable);
    RUN(cwd_with_argument_invokes_the_callback_with_exact_bytes);
    RUN(cwd_callback_failure_is_reported_and_stays_in_the_repl);
    RUN(session_list_renders_every_entry_state);
    RUN(session_list_reports_an_empty_directory_and_a_read_failure);
    RUN(session_current_reports_health_live);
    RUN(session_rename_passes_exact_bytes_and_reports_details);
    RUN(session_trash_and_restore_take_exactly_one_id);
    RUN(session_reports_unavailable_operations_and_bad_subcommands);
    return oi_test_report();
}
