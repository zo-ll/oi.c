#include "cli_loop.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli_message.h"
#include "oi/json.h"

#define OI_CLI_MAX_TOOL_OUTPUT (1024u * 1024u)

struct buffer {
    char *data;
    size_t len;
    size_t cap;
};

struct tool_call {
    size_t index;
    struct buffer id;
    struct buffer name;
    struct buffer arguments;
};

struct loop_state {
    oi_llm_client *client;
    oi_reactor *reactor;
    oi_arena *arena;
    oi_tool_registry *tools;
    const struct oi_cli_loop_config *config;

    struct oi_cli_message_list messages;
    struct buffer all_assistant;
    struct buffer assistant;
    struct tool_call *calls;
    size_t calls_len;

    oi_llm_request *request;
    int llm_done;
    oi_status llm_status;
    int http_status;
};

static oi_status buffer_append(struct buffer *buffer, const char *data,
                               size_t len, size_t limit) {
    if (buffer->len >= limit || len > limit - buffer->len ||
        len > SIZE_MAX - buffer->len - 1) {
        return OI_ERR_NOMEM;
    }
    size_t needed = buffer->len + len + 1;
    if (needed > buffer->cap) {
        size_t cap = buffer->cap == 0 ? 256 : buffer->cap;
        while (cap < needed) {
            if (cap > SIZE_MAX / 2) {
                return OI_ERR_NOMEM;
            }
            cap *= 2;
        }
        char *next = realloc(buffer->data, cap);
        if (next == NULL) {
            return OI_ERR_NOMEM;
        }
        buffer->data = next;
        buffer->cap = cap;
    }
    if (len > 0) {
        memcpy(buffer->data + buffer->len, data, len);
    }
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return OI_OK;
}

static void buffer_free(struct buffer *buffer) {
    free(buffer->data);
    memset(buffer, 0, sizeof *buffer);
}

static void tool_call_free(struct tool_call *call) {
    buffer_free(&call->id);
    buffer_free(&call->name);
    buffer_free(&call->arguments);
}

static void state_free(struct loop_state *state) {
    oi_cli_message_list_free(&state->messages);
    buffer_free(&state->all_assistant);
    buffer_free(&state->assistant);
    for (size_t i = 0; i < state->calls_len; i++) {
        tool_call_free(&state->calls[i]);
    }
    free(state->calls);
}

static oi_status add_user_message(struct loop_state *state,
                                  const char *content, size_t content_len) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    oi_status st =
        oi_cli_message_set_user(&message, content, content_len);
    if (st == OI_OK) {
        st = oi_cli_message_list_append_take(&state->messages, &message);
    }
    oi_cli_message_free(&message);
    return st;
}

static oi_status add_tool_message(struct loop_state *state,
                                  const char *tool_call_id,
                                  size_t tool_call_id_len,
                                  const char *content, size_t content_len) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    oi_status st = oi_cli_message_set_tool(
        &message, tool_call_id, tool_call_id_len, content, content_len);
    if (st == OI_OK) {
        st = oi_cli_message_list_append_take(&state->messages, &message);
    }
    oi_cli_message_free(&message);
    return st;
}

static oi_status add_assistant_message(struct loop_state *state) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    oi_status st = oi_cli_message_set_assistant(
        &message,
        state->assistant.data != NULL ? state->assistant.data : "",
        state->assistant.len);
    for (size_t i = 0; st == OI_OK && i < state->calls_len; i++) {
        const struct tool_call *call = &state->calls[i];
        st = oi_cli_message_add_tool_call(
            &message, call->id.data, call->id.len, call->name.data,
            call->name.len, call->arguments.data, call->arguments.len);
    }
    if (st == OI_OK) {
        st = oi_cli_message_list_append_take(&state->messages, &message);
    }
    oi_cli_message_free(&message);
    if (st == OI_OK) {
        buffer_free(&state->assistant);
        for (size_t i = 0; i < state->calls_len; i++) {
            tool_call_free(&state->calls[i]);
        }
        free(state->calls);
        state->calls = NULL;
        state->calls_len = 0;
    }
    return st;
}

static struct tool_call *find_or_add_call(struct loop_state *state,
                                           size_t index) {
    for (size_t i = 0; i < state->calls_len; i++) {
        if (state->calls[i].index == index) {
            return &state->calls[i];
        }
    }
    if (state->calls_len == SIZE_MAX / sizeof *state->calls) {
        return NULL;
    }
    size_t count = state->calls_len + 1;
    struct tool_call *calls = realloc(state->calls, count * sizeof *calls);
    if (calls == NULL) {
        return NULL;
    }
    state->calls = calls;
    struct tool_call *call = &calls[state->calls_len];
    memset(call, 0, sizeof *call);
    call->index = index;
    state->calls_len = count;
    return call;
}

static void on_llm_event(const oi_llm_event *event, void *ud) {
    struct loop_state *state = ud;
    oi_status st;
    if (event->type == OI_LLM_EVENT_TEXT) {
        if ((event->as.text.len > 0 &&
             fwrite(event->as.text.data, 1, event->as.text.len,
                    state->config->out) != event->as.text.len) ||
            fflush(state->config->out) != 0) {
            st = OI_ERR_IO;
        } else {
            st = buffer_append(&state->assistant, event->as.text.data,
                               event->as.text.len, SIZE_MAX);
            if (st == OI_OK) {
                st = buffer_append(&state->all_assistant,
                                   event->as.text.data, event->as.text.len,
                                   SIZE_MAX);
            }
        }
    } else {
        struct tool_call *call =
            find_or_add_call(state, event->as.tool_call.index);
        st = call == NULL
                 ? OI_ERR_NOMEM
                 : buffer_append(&call->id, event->as.tool_call.id,
                                 event->as.tool_call.id_len, SIZE_MAX);
        if (st == OI_OK) {
            st = buffer_append(&call->name, event->as.tool_call.name,
                               event->as.tool_call.name_len, SIZE_MAX);
        }
        if (st == OI_OK) {
            st = buffer_append(&call->arguments,
                               event->as.tool_call.arguments,
                               event->as.tool_call.arguments_len, SIZE_MAX);
        }
    }
    if (st != OI_OK) {
        state->llm_status = st;
        state->llm_done = 1;
        oi_llm_request_cancel(state->request);
    }
}

static void on_llm_done(oi_status status, int http_status,
                        const char *error_body, size_t error_body_len,
                        void *ud) {
    struct loop_state *state = ud;
    state->llm_status = status;
    state->http_status = http_status;
    state->llm_done = 1;
    if (status != OI_OK && error_body != NULL && error_body_len > 0) {
        fprintf(state->config->err, "oi: model error body: %.*s\n",
                (int)error_body_len, error_body);
    }
}

static oi_status write_tool_definition(oi_json_writer *writer) {
    oi_status st;
#define WRITE(expr)                                                            \
    do {                                                                       \
        st = (expr);                                                           \
        if (st != OI_OK) {                                                     \
            return st;                                                         \
        }                                                                      \
    } while (0)
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "type", 4));
    WRITE(oi_json_write_string(writer, "function", 8));
    WRITE(oi_json_write_object_key(writer, "function", 8));
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "name", 4));
    WRITE(oi_json_write_string(writer, "shell", 5));
    WRITE(oi_json_write_object_key(writer, "description", 11));
    WRITE(oi_json_write_string(writer, "Execute a shell command", 23));
    WRITE(oi_json_write_object_key(writer, "parameters", 10));
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "type", 4));
    WRITE(oi_json_write_string(writer, "object", 6));
    WRITE(oi_json_write_object_key(writer, "properties", 10));
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "command", 7));
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "type", 4));
    WRITE(oi_json_write_string(writer, "string", 6));
    WRITE(oi_json_write_object_end(writer));
    WRITE(oi_json_write_object_end(writer));
    WRITE(oi_json_write_object_key(writer, "required", 8));
    WRITE(oi_json_write_array_begin(writer));
    WRITE(oi_json_write_string(writer, "command", 7));
    WRITE(oi_json_write_array_end(writer));
    WRITE(oi_json_write_object_key(writer, "additionalProperties", 20));
    WRITE(oi_json_write_bool(writer, 0));
    WRITE(oi_json_write_object_end(writer));
    WRITE(oi_json_write_object_end(writer));
    WRITE(oi_json_write_object_end(writer));
#undef WRITE
    return OI_OK;
}

static oi_status build_body(const struct loop_state *state,
                            oi_json_writer **out_writer) {
    oi_json_writer *writer = oi_json_writer_create();
    if (writer == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_status st;
#define WRITE(expr)                                                            \
    do {                                                                       \
        st = (expr);                                                           \
        if (st != OI_OK) {                                                     \
            goto fail;                                                         \
        }                                                                      \
    } while (0)
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "model", 5));
    WRITE(oi_json_write_string(writer, state->config->model,
                               strlen(state->config->model)));
    WRITE(oi_json_write_object_key(writer, "stream", 6));
    WRITE(oi_json_write_bool(writer, 1));
    WRITE(oi_json_write_object_key(writer, "messages", 8));
    WRITE(oi_json_write_array_begin(writer));
    for (size_t i = 0; i < state->messages.len; i++) {
        const struct oi_cli_message *message = &state->messages.items[i];
        const char *role = message->role == OI_CLI_MESSAGE_USER
                               ? "user"
                               : (message->role ==
                                          OI_CLI_MESSAGE_ASSISTANT
                                      ? "assistant"
                                      : "tool");
        WRITE(oi_json_write_object_begin(writer));
        WRITE(oi_json_write_object_key(writer, "role", 4));
        WRITE(oi_json_write_string(writer, role, strlen(role)));
        WRITE(oi_json_write_object_key(writer, "content", 7));
        WRITE(oi_json_write_string(writer, message->content.data,
                                   message->content.len));
        if (message->role == OI_CLI_MESSAGE_TOOL) {
            WRITE(oi_json_write_object_key(writer, "tool_call_id", 12));
            WRITE(oi_json_write_string(writer, message->tool_call_id.data,
                                       message->tool_call_id.len));
        }
        if (message->tool_calls_len > 0) {
            WRITE(oi_json_write_object_key(writer, "tool_calls", 10));
            WRITE(oi_json_write_array_begin(writer));
            for (size_t j = 0; j < message->tool_calls_len; j++) {
                const struct oi_cli_tool_call_value *call =
                    &message->tool_calls[j];
                WRITE(oi_json_write_object_begin(writer));
                WRITE(oi_json_write_object_key(writer, "id", 2));
                WRITE(oi_json_write_string(writer, call->id.data,
                                           call->id.len));
                WRITE(oi_json_write_object_key(writer, "type", 4));
                WRITE(oi_json_write_string(writer, "function", 8));
                WRITE(oi_json_write_object_key(writer, "function", 8));
                WRITE(oi_json_write_object_begin(writer));
                WRITE(oi_json_write_object_key(writer, "name", 4));
                WRITE(oi_json_write_string(writer, call->name.data,
                                           call->name.len));
                WRITE(oi_json_write_object_key(writer, "arguments", 9));
                WRITE(oi_json_write_string(writer, call->arguments.data,
                                           call->arguments.len));
                WRITE(oi_json_write_object_end(writer));
                WRITE(oi_json_write_object_end(writer));
            }
            WRITE(oi_json_write_array_end(writer));
        }
        WRITE(oi_json_write_object_end(writer));
    }
    WRITE(oi_json_write_array_end(writer));
    WRITE(oi_json_write_object_key(writer, "tools", 5));
    WRITE(oi_json_write_array_begin(writer));
    WRITE(write_tool_definition(writer));
    WRITE(oi_json_write_array_end(writer));
    WRITE(oi_json_write_object_end(writer));
#undef WRITE
    *out_writer = writer;
    return OI_OK;

fail:
#undef WRITE
    oi_json_writer_destroy(writer);
    return st;
}

static oi_status step_until(oi_reactor *reactor, const int *done) {
    while (!*done) {
        oi_status st;
        if (oi_reactor_step(reactor, -1, &st) < 0) {
            return st;
        }
    }
    return OI_OK;
}

struct tool_run {
    struct buffer output;
    oi_tool_call *call;
    int done;
    int output_failed;
    oi_tool_exit_kind kind;
    int code;
};

static void on_tool_output(const void *data, size_t len, void *ud) {
    struct tool_run *run = ud;
    if (buffer_append(&run->output, data, len, OI_CLI_MAX_TOOL_OUTPUT) !=
        OI_OK) {
        run->output_failed = 1;
        run->done = 1;
        oi_tool_call_cancel(run->call);
    }
}

static void on_tool_done(oi_tool_exit_kind kind, int code, void *ud) {
    struct tool_run *run = ud;
    run->kind = kind;
    run->code = code;
    run->done = 1;
}

static oi_status parse_arguments(oi_arena *arena, const struct buffer *input,
                                 oi_json_value **out) {
    oi_json_parser *parser = oi_json_parser_create(arena);
    if (parser == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_status st = oi_json_parser_feed(parser, input->data, input->len);
    if (st == OI_OK) {
        st = oi_json_parser_finish(parser);
    }
    oi_json_value *root = oi_json_parser_root(parser);
    if (st == OI_OK &&
        (root == NULL || oi_json_type_of(root) != OI_JSON_OBJECT)) {
        st = OI_ERR_PARSE;
    }
    if (st == OI_OK) {
        *out = root;
    }
    oi_json_parser_destroy(parser);
    return st;
}

static oi_status run_tool(struct loop_state *state,
                          const struct tool_call *call) {
    oi_json_value *arguments;
    oi_status st = parse_arguments(state->arena, &call->arguments, &arguments);
    if (st != OI_OK) {
        return st;
    }

    fprintf(state->config->err, "oi: running tool %s\n", call->name.data);
    struct tool_run run = {0};
    st = oi_tool_call_start(
        state->tools, state->reactor, state->arena, call->name.data, arguments,
        oi_cli_tool_permission, state->config->permission, on_tool_output,
        on_tool_done, &run, &run.call);
    if (st != OI_OK) {
        buffer_free(&run.output);
        return st;
    }
    if (state->config->tool_timeout_ms > 0) {
        st = oi_tool_call_set_timeout(run.call,
                                      state->config->tool_timeout_ms);
    }
    if (st == OI_OK) {
        st = oi_tool_call_close_stdin(run.call);
    }
    if (st != OI_OK) {
        oi_tool_call_cancel(run.call);
        buffer_free(&run.output);
        return st;
    }
    st = step_until(state->reactor, &run.done);
    if (st != OI_OK || run.output_failed) {
        buffer_free(&run.output);
        return st != OI_OK ? st : OI_ERR_NOMEM;
    }

    char status[96];
    int status_len = 0;
    if (run.kind != OI_TOOL_EXIT_NORMAL || run.code != 0) {
        status_len = snprintf(status, sizeof status,
                              "\n[tool exit kind=%d code=%d]\n",
                              (int)run.kind, run.code);
        if (status_len < 0 || (size_t)status_len >= sizeof status ||
            buffer_append(&run.output, status, (size_t)status_len,
                          OI_CLI_MAX_TOOL_OUTPUT) != OI_OK) {
            buffer_free(&run.output);
            return OI_ERR_NOMEM;
        }
    }
    if (run.output.data == NULL &&
        buffer_append(&run.output, "", 0, OI_CLI_MAX_TOOL_OUTPUT) != OI_OK) {
        return OI_ERR_NOMEM;
    }
    st = add_tool_message(state, call->id.data, call->id.len,
                          run.output.data, run.output.len);
    buffer_free(&run.output);
    return st;
}

oi_status oi_cli_loop_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_loop_config *config,
                          const char *prompt,
                          struct oi_cli_loop_result *out_result) {
    if (client == NULL || reactor == NULL || arena == NULL || tools == NULL ||
        config == NULL || config->model == NULL || config->max_turns <= 0 ||
        config->permission == NULL || config->out == NULL ||
        config->err == NULL || prompt == NULL || out_result == NULL) {
        return OI_ERR_INVAL;
    }
    memset(out_result, 0, sizeof *out_result);
    struct loop_state state = {
        .client = client,
        .reactor = reactor,
        .arena = arena,
        .tools = tools,
        .config = config,
    };
    oi_status st = add_user_message(&state, prompt, strlen(prompt));

    for (int turn = 0; st == OI_OK && turn < config->max_turns; turn++) {
        oi_json_writer *writer = NULL;
        st = build_body(&state, &writer);
        if (st != OI_OK) {
            break;
        }
        size_t body_len;
        const char *body = oi_json_writer_data(writer, &body_len);
        state.llm_done = 0;
        state.llm_status = OI_OK;
        state.http_status = 0;
        st = oi_llm_request_start_events(
            client, reactor, arena, body, body_len, on_llm_event, on_llm_done,
            &state, &state.request);
        oi_json_writer_destroy(writer);
        if (st != OI_OK) {
            break;
        }
        st = step_until(reactor, &state.llm_done);
        if (putc('\n', config->out) == EOF && st == OI_OK) {
            st = OI_ERR_IO;
        }
        if (st == OI_OK) {
            st = state.llm_status;
        }
        if (st != OI_OK) {
            fprintf(config->err, "oi: model request failed (status=%d, http=%d)\n",
                    (int)st, state.http_status);
            break;
        }

        size_t call_count = state.calls_len;
        st = add_assistant_message(&state);
        if (st != OI_OK || call_count == 0) {
            break;
        }
        struct oi_cli_message *assistant =
            &state.messages.items[state.messages.len - 1];
        /*
         * Tool messages may realloc the message list, so retain only the
         * separately allocated tool-call array across that operation.
         */
        struct oi_cli_tool_call_value *assistant_calls =
            assistant->tool_calls;
        size_t assistant_calls_len = assistant->tool_calls_len;
        for (size_t i = 0; st == OI_OK && i < assistant_calls_len; i++) {
            const struct oi_cli_tool_call_value *call = &assistant_calls[i];
            struct tool_call loop_call = {
                .id = {call->id.data, call->id.len, call->id.len + 1},
                .name = {call->name.data, call->name.len,
                         call->name.len + 1},
                .arguments = {call->arguments.data, call->arguments.len,
                              call->arguments.len + 1},
            };
            st = run_tool(&state, &loop_call);
        }
        if (st == OI_OK && turn + 1 == config->max_turns) {
            fprintf(config->err, "oi: model tool loop exceeded %d turns\n",
                    config->max_turns);
            st = OI_ERR_INVAL;
        }
    }

    if (st == OI_OK) {
        if (state.all_assistant.data == NULL) {
            st = buffer_append(&state.all_assistant, "", 0, SIZE_MAX);
        }
    }
    if (st == OI_OK) {
        out_result->assistant_text = state.all_assistant.data;
        out_result->assistant_text_len = state.all_assistant.len;
        memset(&state.all_assistant, 0, sizeof state.all_assistant);
    }
    state_free(&state);
    return st;
}
