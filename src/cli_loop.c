#include "cli_loop.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli_conversation.h"

struct output_buffer {
    char *data;
    size_t len;
    size_t cap;
};

struct wrapper_state {
    FILE *out;
    FILE *err;
    struct output_buffer assistant;
    int done;
    oi_status status;
    oi_cli_conversation_event_cb external_event;
    void *external_user_data;
};

static oi_status output_buffer_append(struct output_buffer *buffer,
                                      const char *data, size_t len) {
    if (len > SIZE_MAX - buffer->len - 1) {
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
        char *data_copy = realloc(buffer->data, cap);
        if (data_copy == NULL) {
            return OI_ERR_NOMEM;
        }
        buffer->data = data_copy;
        buffer->cap = cap;
    }
    if (len > 0) {
        memcpy(buffer->data + buffer->len, data, len);
    }
    buffer->len += len;
    buffer->data[buffer->len] = '\0';
    return OI_OK;
}

static oi_status write_tool_start(FILE *stream,
                                  const struct oi_cli_string *name) {
    static const char prefix[] = "oi: running tool ";
    if (fwrite(prefix, 1, sizeof prefix - 1, stream) != sizeof prefix - 1 ||
        (name->len > 0 &&
         fwrite(name->data, 1, name->len, stream) != name->len) ||
        putc('\n', stream) == EOF || fflush(stream) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static oi_status write_model_error(
    FILE *stream, const struct oi_cli_conversation_event *event) {
    char header[96];
    int header_len = snprintf(header, sizeof header,
                              "oi: model error body (http=%d): ",
                              event->as.model_error.http_status);
    if (header_len < 0 || (size_t)header_len >= sizeof header ||
        fwrite(header, 1, (size_t)header_len, stream) !=
            (size_t)header_len ||
        (event->as.model_error.body_len > 0 &&
         fwrite(event->as.model_error.body, 1,
                event->as.model_error.body_len, stream) !=
             event->as.model_error.body_len) ||
        putc('\n', stream) == EOF) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static oi_status on_conversation_event(
    const struct oi_cli_conversation_event *event, void *user_data) {
    struct wrapper_state *state = user_data;
    if (state->external_event != NULL) {
        oi_status st =
            state->external_event(event, state->external_user_data);
        if (st != OI_OK) {
            return st;
        }
    }
    switch (event->type) {
    case OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA:
        if ((event->as.bytes.len > 0 &&
             fwrite(event->as.bytes.data, 1, event->as.bytes.len,
                    state->out) != event->as.bytes.len) ||
            fflush(state->out) != 0) {
            return OI_ERR_IO;
        }
        return output_buffer_append(&state->assistant, event->as.bytes.data,
                                    event->as.bytes.len);
    case OI_CLI_CONVERSATION_EVENT_TOOL_STARTING:
        return write_tool_start(state->err, event->as.tool_starting.name);
    case OI_CLI_CONVERSATION_EVENT_RESPONSE_DONE:
        return putc('\n', state->out) == EOF ? OI_ERR_IO : OI_OK;
    case OI_CLI_CONVERSATION_EVENT_MODEL_ERROR:
        return write_model_error(state->err, event);
    case OI_CLI_CONVERSATION_EVENT_TURN_DONE:
        state->status = event->as.turn_done.status;
        state->done = 1;
        return OI_OK;
    case OI_CLI_CONVERSATION_EVENT_MESSAGE:
    case OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT:
    case OI_CLI_CONVERSATION_EVENT_PARTIAL_ASSISTANT:
        return OI_OK;
    }
    return OI_ERR_INVAL;
}

oi_status oi_cli_loop_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_loop_config *config,
                          const char *prompt,
                          struct oi_cli_loop_result *out_result) {
    if (client == NULL || reactor == NULL || arena == NULL || tools == NULL ||
        config == NULL || config->model == NULL || config->max_turns <= 0 ||
        config->tool_timeout_ms < 0 || config->permission == NULL ||
        config->out == NULL || config->err == NULL || prompt == NULL ||
        out_result == NULL) {
        return OI_ERR_INVAL;
    }
    memset(out_result, 0, sizeof *out_result);
    struct wrapper_state state = {
        .out = config->out,
        .err = config->err,
        .external_event = config->on_event,
        .external_user_data = config->event_user_data,
    };
    struct oi_cli_conversation_config conversation_config = {
        .model = config->model,
        .max_model_steps = config->max_turns,
        .tool_timeout_ms = config->tool_timeout_ms,
        .permission = oi_cli_tool_permission,
        .permission_user_data = config->permission,
        .on_event = on_conversation_event,
        .event_user_data = &state,
    };
    oi_cli_conversation *conversation = NULL;
    oi_status st = oi_cli_conversation_create(
        client, reactor, arena, tools, &conversation_config,
        config->initial_context,
        &conversation);
    if (st == OI_OK) {
        st = oi_cli_conversation_start(conversation, prompt, strlen(prompt));
    }
    while (st == OI_OK && !state.done) {
        oi_status step_status;
        if (oi_reactor_step(reactor, -1, &step_status) < 0) {
            st = step_status;
        }
    }
    if (st != OI_OK && conversation != NULL &&
        oi_cli_conversation_is_busy(conversation)) {
        oi_cli_conversation_cancel(conversation);
    }
    if (st == OI_OK) {
        st = state.status;
    }
    if (st != OI_OK) {
        if (st == OI_ERR_DENIED) {
            fprintf(config->err, "oi: tool permission denied\n");
        }
        if (st == OI_ERR_INVAL) {
            fprintf(config->err, "oi: model tool loop exceeded %d turns\n",
                    config->max_turns);
        }
        fprintf(config->err, "oi: model/tool loop failed (status=%d)\n",
                (int)st);
    }
    if (st == OI_OK && state.assistant.data == NULL) {
        st = output_buffer_append(&state.assistant, "", 0);
    }
    if (st == OI_OK) {
        out_result->assistant_text = state.assistant.data;
        out_result->assistant_text_len = state.assistant.len;
        memset(&state.assistant, 0, sizeof state.assistant);
    }
    free(state.assistant.data);
    oi_cli_conversation_destroy(conversation);
    return st;
}
