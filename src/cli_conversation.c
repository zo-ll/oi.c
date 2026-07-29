#include "cli_conversation.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_history.h"
#include "oi/json.h"

struct buffer {
    char *data;
    size_t len;
    size_t cap;
};

struct streamed_tool_call {
    size_t index;
    struct buffer id;
    struct buffer name;
    struct buffer arguments;
};

struct oi_cli_conversation {
    oi_llm_client *client;
    oi_reactor *reactor;
    oi_arena *arena;
    oi_tool_registry *tools;
    struct oi_cli_conversation_config config;
    struct oi_cli_string model;
    struct oi_cli_message_list messages;

    oi_llm_request *request;
    oi_tool_call *tool;
    int busy;
    int cancelled;
    oi_status last_status;
    int http_status;
    int model_steps;

    struct buffer assistant;
    struct streamed_tool_call *streamed_calls;
    size_t streamed_calls_len;

    size_t assistant_message_index;
    size_t tool_index;
    struct buffer tool_output;
};

static oi_status buffer_append(struct buffer *buffer, const void *data,
                               size_t len, size_t limit) {
    if ((data == NULL && len > 0) || buffer->len > limit ||
        len > limit - buffer->len || len > SIZE_MAX - buffer->len - 1) {
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

static void streamed_call_free(struct streamed_tool_call *call) {
    buffer_free(&call->id);
    buffer_free(&call->name);
    buffer_free(&call->arguments);
}

static void clear_streamed_response(oi_cli_conversation *conversation) {
    buffer_free(&conversation->assistant);
    for (size_t i = 0; i < conversation->streamed_calls_len; i++) {
        streamed_call_free(&conversation->streamed_calls[i]);
    }
    free(conversation->streamed_calls);
    conversation->streamed_calls = NULL;
    conversation->streamed_calls_len = 0;
}

static oi_status emit(oi_cli_conversation *conversation,
                      const struct oi_cli_conversation_event *event) {
    if (conversation->config.on_event == NULL) {
        return OI_OK;
    }
    return conversation->config.on_event(event,
                                         conversation->config.event_user_data);
}

static void finish_turn(oi_cli_conversation *conversation, oi_status status) {
    if (!conversation->busy) {
        return;
    }
    conversation->busy = 0;
    conversation->last_status = status;
    struct oi_cli_conversation_event event = {
        .type = OI_CLI_CONVERSATION_EVENT_TURN_DONE,
        .as.turn_done = {status, conversation->http_status},
    };
    (void)emit(conversation, &event);
}

static oi_status emit_message(
    oi_cli_conversation *conversation, const struct oi_cli_message *message,
    enum oi_cli_conversation_tool_outcome outcome,
    const unsigned char *raw_output, size_t raw_output_len, int has_raw,
    int is_repair) {
    /* A repair placeholder isn't attributable to any real model reply --
     * history_message_is_valid requires a REPAIR-sourced assistant message
     * to carry no model, matching cli_history_repair.c's own
     * append_interruption_marker. */
    int attribute_model =
        message->role == OI_CLI_MESSAGE_ASSISTANT && !is_repair;
    struct oi_cli_conversation_event event = {
        .type = OI_CLI_CONVERSATION_EVENT_MESSAGE,
        .as.message = {
            .value = message,
            .model = attribute_model ? conversation->model.data : NULL,
            .model_len = attribute_model ? conversation->model.len : 0,
            .tool_outcome = outcome,
            .raw_tool_output = raw_output,
            .raw_tool_output_len = raw_output_len,
            .has_raw_tool_output = has_raw,
            .is_repair = is_repair,
        },
    };
    return emit(conversation, &event);
}

static oi_status commit_message(
    oi_cli_conversation *conversation, struct oi_cli_message *message,
    enum oi_cli_conversation_tool_outcome outcome,
    const unsigned char *raw_output, size_t raw_output_len, int has_raw,
    int is_repair) {
    oi_status st = emit_message(conversation, message, outcome, raw_output,
                                raw_output_len, has_raw, is_repair);
    if (st == OI_OK) {
        st = oi_cli_message_list_append_take(&conversation->messages, message);
    }
    return st;
}

static oi_status write_tool_definition(oi_json_writer *writer) {
    oi_status st;
#define WRITE(expr)                                                           \
    do {                                                                      \
        st = (expr);                                                          \
        if (st != OI_OK) {                                                    \
            return st;                                                        \
        }                                                                     \
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

static oi_status build_request_body(const oi_cli_conversation *conversation,
                                    oi_json_writer **out_writer) {
    oi_json_writer *writer = oi_json_writer_create();
    if (writer == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_status st;
#define WRITE(expr)                                                           \
    do {                                                                      \
        st = (expr);                                                          \
        if (st != OI_OK) {                                                    \
            goto fail;                                                        \
        }                                                                     \
    } while (0)
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "model", 5));
    WRITE(oi_json_write_string(writer, conversation->model.data,
                               conversation->model.len));
    WRITE(oi_json_write_object_key(writer, "stream", 6));
    WRITE(oi_json_write_bool(writer, 1));
    WRITE(oi_json_write_object_key(writer, "messages", 8));
    WRITE(oi_json_write_array_begin(writer));
    for (size_t i = 0; i < conversation->messages.len; i++) {
        const struct oi_cli_message *message =
            &conversation->messages.items[i];
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

static struct streamed_tool_call *find_or_add_streamed_call(
    oi_cli_conversation *conversation, size_t index) {
    for (size_t i = 0; i < conversation->streamed_calls_len; i++) {
        if (conversation->streamed_calls[i].index == index) {
            return &conversation->streamed_calls[i];
        }
    }
    if (conversation->streamed_calls_len ==
        SIZE_MAX / sizeof *conversation->streamed_calls) {
        return NULL;
    }
    size_t count = conversation->streamed_calls_len + 1;
    struct streamed_tool_call *calls =
        realloc(conversation->streamed_calls, count * sizeof *calls);
    if (calls == NULL) {
        return NULL;
    }
    conversation->streamed_calls = calls;
    struct streamed_tool_call *call =
        &calls[conversation->streamed_calls_len];
    memset(call, 0, sizeof *call);
    call->index = index;
    conversation->streamed_calls_len = count;
    return call;
}

static int compare_streamed_calls(const void *left, const void *right) {
    const struct streamed_tool_call *a = left;
    const struct streamed_tool_call *b = right;
    return a->index < b->index ? -1 : (a->index > b->index ? 1 : 0);
}

static oi_status build_assistant_message(
    oi_cli_conversation *conversation, struct oi_cli_message *message) {
    oi_status st = oi_cli_message_set_assistant(
        message,
        conversation->assistant.data != NULL ? conversation->assistant.data
                                             : "",
        conversation->assistant.len);
    if (st != OI_OK || conversation->streamed_calls_len == 0) {
        return st;
    }
    qsort(conversation->streamed_calls, conversation->streamed_calls_len,
          sizeof *conversation->streamed_calls, compare_streamed_calls);
    for (size_t i = 0; st == OI_OK &&
                       i < conversation->streamed_calls_len;
         i++) {
        const struct streamed_tool_call *call =
            &conversation->streamed_calls[i];
        if (call->index != i || call->id.len == 0 || call->name.len == 0 ||
            call->arguments.len == 0) {
            return OI_ERR_PARSE;
        }
        st = oi_cli_message_add_tool_call(
            message, call->id.data, call->id.len, call->name.data,
            call->name.len, call->arguments.data, call->arguments.len);
    }
    return st;
}

static oi_status parse_arguments(oi_cli_conversation *conversation,
                                 const struct oi_cli_string *arguments,
                                 oi_json_value **out) {
    oi_json_parser *parser = oi_json_parser_create(conversation->arena);
    if (parser == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_status st =
        oi_json_parser_feed(parser, arguments->data, arguments->len);
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

static size_t valid_utf8_sequence(const unsigned char *data, size_t len) {
    if (len == 0 || data[0] < 0x80) {
        return len == 0 ? 0 : 1;
    }
    if (data[0] >= 0xc2 && data[0] <= 0xdf && len >= 2 &&
        (data[1] & 0xc0) == 0x80) {
        return 2;
    }
    if (data[0] >= 0xe0 && data[0] <= 0xef && len >= 3 &&
        (data[1] & 0xc0) == 0x80 && (data[2] & 0xc0) == 0x80 &&
        !(data[0] == 0xe0 && data[1] < 0xa0) &&
        !(data[0] == 0xed && data[1] >= 0xa0)) {
        return 3;
    }
    if (data[0] >= 0xf0 && data[0] <= 0xf4 && len >= 4 &&
        (data[1] & 0xc0) == 0x80 && (data[2] & 0xc0) == 0x80 &&
        (data[3] & 0xc0) == 0x80 &&
        !(data[0] == 0xf0 && data[1] < 0x90) &&
        !(data[0] == 0xf4 && data[1] >= 0x90)) {
        return 4;
    }
    return 0;
}

static oi_status normalize_utf8(const unsigned char *data, size_t len,
                                struct buffer *output) {
    static const unsigned char replacement[] = {0xef, 0xbf, 0xbd};
    size_t offset = 0;
    while (offset < len) {
        size_t sequence = valid_utf8_sequence(data + offset, len - offset);
        oi_status st;
        if (sequence == 0) {
            st = buffer_append(output, replacement, sizeof replacement,
                               OI_CLI_HISTORY_MAX_CONTENT);
            offset++;
        } else {
            st = buffer_append(output, data + offset, sequence,
                               OI_CLI_HISTORY_MAX_CONTENT);
            offset += sequence;
        }
        if (st != OI_OK) {
            return st;
        }
    }
    return buffer_append(output, "", 0, OI_CLI_HISTORY_MAX_CONTENT);
}

static oi_status start_model_request(oi_cli_conversation *conversation);
static void start_next_tool(oi_cli_conversation *conversation);
static oi_status repair_interrupted_turn(oi_cli_conversation *conversation);

static oi_status commit_tool_text(
    oi_cli_conversation *conversation,
    const struct oi_cli_tool_call_value *call, const char *text,
    size_t text_len, enum oi_cli_conversation_tool_outcome outcome,
    const unsigned char *raw, size_t raw_len, int has_raw, int is_repair) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    oi_status st = oi_cli_message_set_tool(
        &message, call->id.data, call->id.len, text, text_len);
    if (st == OI_OK) {
        st = commit_message(conversation, &message, outcome, raw, raw_len,
                            has_raw, is_repair);
    }
    oi_cli_message_free(&message);
    return st;
}

static const char cancelled_tool_unknown_text[] =
    "[tool outcome unknown: cancelled while it may have already started]";
static const char cancelled_tool_not_executed_text[] =
    "[tool not executed: cancelled before it started]";

/*
 * Restores protocol validity after cancelling a running (or about-to-run)
 * tool: the assistant message committed by on_llm_done already carries the
 * full tool_calls array, but only calls before tool_index have a matching
 * tool-result message. Commits a placeholder for the call at tool_index --
 * OUTCOME_UNKNOWN (with whatever raw output it had already produced) if
 * `first_call_may_have_started`, else NOT_EXECUTED, matching whether a
 * process was actually spawned for it -- and a NOT_EXECUTED placeholder for
 * each later call that never started. Makes conversation->messages
 * immediately safe to reuse for a next turn without touching the
 * durable-replay repair machinery at all. Captures tool_calls/tool_calls_len
 * once, up front: each commit_tool_text call below can append to and
 * reallocate conversation->messages.items, which would invalidate a pointer
 * held directly into that array, but not the assistant message's own
 * (separately-allocated, ownership-copied) tool_calls array.
 */
static oi_status repair_dangling_tool_calls(oi_cli_conversation *conversation,
                                            int first_call_may_have_started) {
    const struct oi_cli_message *assistant =
        &conversation->messages.items[conversation->assistant_message_index];
    const struct oi_cli_tool_call_value *tool_calls = assistant->tool_calls;
    size_t tool_calls_len = assistant->tool_calls_len;
    size_t cancelled_index = conversation->tool_index;
    oi_status first_status = OI_OK;
    size_t i;

    for (i = cancelled_index; i < tool_calls_len; i++) {
        const struct oi_cli_tool_call_value *call = &tool_calls[i];
        oi_status st;

        if (i == cancelled_index && first_call_may_have_started) {
            /* is_repair=0: history_message_is_valid requires a
             * REPAIR-sourced tool message to never carry raw output, but
             * this placeholder's whole value over the durable crash-repair
             * equivalent (cli_history_repair.c's append_repair_tool, which
             * always passes NULL/0) is carrying whatever was already
             * buffered -- NORMAL is the source that permits that for an
             * OUTCOME_UNKNOWN tool result. */
            st = commit_tool_text(
                conversation, call, cancelled_tool_unknown_text,
                sizeof cancelled_tool_unknown_text - 1,
                OI_CLI_CONVERSATION_TOOL_OUTCOME_UNKNOWN,
                (const unsigned char *)conversation->tool_output.data,
                conversation->tool_output.len, 1, /*is_repair=*/0);
        } else {
            st = commit_tool_text(
                conversation, call, cancelled_tool_not_executed_text,
                sizeof cancelled_tool_not_executed_text - 1,
                OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED, NULL, 0, 0,
                /*is_repair=*/0);
        }
        if (st != OI_OK && first_status == OI_OK) {
            first_status = st;
        }
    }
    buffer_free(&conversation->tool_output);
    return first_status;
}

static oi_tool_decision always_stage(const char *tool_name,
                                     const oi_json_value *args,
                                     void *user_data) {
    (void)tool_name;
    (void)args;
    (void)user_data;
    return OI_TOOL_ASK;
}

static void on_tool_output(const void *data, size_t len, void *user_data) {
    oi_cli_conversation *conversation = user_data;
    oi_status st = buffer_append(&conversation->tool_output, data, len,
                                 OI_CLI_HISTORY_MAX_CONTENT);
    if (st == OI_OK) {
        struct oi_cli_conversation_event event = {
            .type = OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT,
            .as.bytes = {data, len},
        };
        st = emit(conversation, &event);
    }
    if (st != OI_OK) {
        oi_tool_call *tool = conversation->tool;
        conversation->tool = NULL;
        oi_tool_call_cancel(tool);
        finish_turn(conversation, st);
    }
}

static void on_tool_done(oi_tool_exit_kind kind, int code, void *user_data) {
    oi_cli_conversation *conversation = user_data;
    conversation->tool = NULL;
    struct buffer text = {0};
    oi_status st = normalize_utf8(
        (const unsigned char *)conversation->tool_output.data,
        conversation->tool_output.len, &text);
    if (st == OI_OK && (kind != OI_TOOL_EXIT_NORMAL || code != 0)) {
        char status[96];
        int len = snprintf(status, sizeof status,
                           "\n[tool exit kind=%d code=%d]\n", (int)kind,
                           code);
        if (len < 0 || (size_t)len >= sizeof status) {
            st = OI_ERR_NOMEM;
        } else {
            st = buffer_append(&text, status, (size_t)len,
                               OI_CLI_HISTORY_MAX_CONTENT);
        }
    }
    const struct oi_cli_message *assistant =
        &conversation->messages.items[conversation->assistant_message_index];
    const struct oi_cli_tool_call_value *call =
        &assistant->tool_calls[conversation->tool_index];
    if (st == OI_OK) {
        st = commit_tool_text(
            conversation, call, text.data, text.len,
            OI_CLI_CONVERSATION_TOOL_COMPLETED,
            (const unsigned char *)conversation->tool_output.data,
            conversation->tool_output.len, 1, /*is_repair=*/0);
    }
    buffer_free(&text);
    buffer_free(&conversation->tool_output);
    if (st != OI_OK) {
        finish_turn(conversation, st);
        return;
    }
    conversation->tool_index++;
    start_next_tool(conversation);
}

static void start_next_tool(oi_cli_conversation *conversation) {
    while (conversation->busy) {
        const struct oi_cli_message *assistant =
            &conversation->messages
                 .items[conversation->assistant_message_index];
        if (conversation->tool_index >= assistant->tool_calls_len) {
            oi_status st = start_model_request(conversation);
            if (st != OI_OK) {
                finish_turn(conversation, st);
            }
            return;
        }
        const struct oi_cli_tool_call_value *call =
            &assistant->tool_calls[conversation->tool_index];
        oi_json_value *arguments = NULL;
        oi_status st =
            parse_arguments(conversation, &call->arguments, &arguments);
        if (st != OI_OK) {
            finish_turn(conversation, st);
            return;
        }

        oi_tool_call *staged = NULL;
        st = oi_tool_call_start(
            conversation->tools, conversation->reactor, conversation->arena,
            call->name.data, arguments, always_stage, NULL, on_tool_output,
            on_tool_done, conversation, &staged);
        if (st != OI_OK) {
            finish_turn(conversation, st);
            return;
        }
        oi_tool_decision decision =
            conversation->config.permission == NULL
                ? OI_TOOL_ALLOW
                : conversation->config.permission(
                      call->name.data, arguments,
                      conversation->config.permission_user_data);
        if (decision != OI_TOOL_ALLOW) {
            (void)oi_tool_call_resolve(staged, 0);
            finish_turn(conversation, OI_ERR_DENIED);
            return;
        }

        struct oi_cli_conversation_event event = {
            .type = OI_CLI_CONVERSATION_EVENT_TOOL_STARTING,
            .as.tool_starting = {&call->id, &call->name, &call->arguments},
        };
        st = emit(conversation, &event);
        if (st != OI_OK) {
            oi_tool_call_cancel(staged);
            finish_turn(conversation, st);
            return;
        }
        if (!conversation->busy) {
            /* The embedder's on_event callback cancelled the conversation
             * reentrantly from within the TOOL_STARTING emit above (a
             * supported pattern one layer down, mirroring
             * oi_tool_call_cancel's own documented reentrancy from within
             * its own on_output). conversation->tool is still NULL at this
             * point, so oi_cli_conversation_cancel's own repair pass (gated
             * on tool != NULL) never saw this call as running and skipped
             * it -- repair it here instead (nothing was ever spawned for
             * it, so NOT_EXECUTED, not OUTCOME_UNKNOWN), best-effort: the
             * turn already finished and reported whatever status it had,
             * so there is nothing left to escalate a repair failure to. */
            if (repair_dangling_tool_calls(
                    conversation, /*first_call_may_have_started=*/0) ==
                OI_OK) {
                (void)repair_interrupted_turn(conversation);
            }
            oi_tool_call_cancel(staged);
            return;
        }
        conversation->tool = staged;
        st = oi_tool_call_resolve(staged, 1);
        if (st != OI_OK) {
            conversation->tool = NULL;
            finish_turn(conversation, st);
            return;
        }
        if (conversation->config.tool_timeout_ms > 0) {
            st = oi_tool_call_set_timeout(
                conversation->tool,
                conversation->config.tool_timeout_ms);
        }
        if (st == OI_OK) {
            st = oi_tool_call_close_stdin(conversation->tool);
        }
        if (st != OI_OK) {
            oi_tool_call *tool = conversation->tool;
            conversation->tool = NULL;
            oi_tool_call_cancel(tool);
            finish_turn(conversation, st);
        }
        return;
    }
}

static void on_llm_event(const oi_llm_event *event, void *user_data) {
    oi_cli_conversation *conversation = user_data;
    oi_status st;
    if (event->type == OI_LLM_EVENT_TEXT) {
        st = buffer_append(&conversation->assistant, event->as.text.data,
                           event->as.text.len,
                           OI_CLI_HISTORY_MAX_CONTENT);
        if (st == OI_OK) {
            struct oi_cli_conversation_event output = {
                .type = OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA,
                .as.bytes = {event->as.text.data, event->as.text.len},
            };
            st = emit(conversation, &output);
        }
    } else {
        struct streamed_tool_call *call =
            find_or_add_streamed_call(conversation,
                                      event->as.tool_call.index);
        st = call == NULL
                 ? OI_ERR_NOMEM
                 : buffer_append(&call->id, event->as.tool_call.id,
                                 event->as.tool_call.id_len,
                                 OI_CLI_HISTORY_MAX_CONTENT);
        if (st == OI_OK) {
            st = buffer_append(&call->name, event->as.tool_call.name,
                               event->as.tool_call.name_len,
                               OI_CLI_HISTORY_MAX_CONTENT);
        }
        if (st == OI_OK) {
            st = buffer_append(
                &call->arguments, event->as.tool_call.arguments,
                event->as.tool_call.arguments_len,
                OI_CLI_HISTORY_MAX_CONTENT);
        }
    }
    if (st != OI_OK) {
        oi_llm_request *request = conversation->request;
        conversation->request = NULL;
        oi_llm_request_cancel(request);
        finish_turn(conversation, st);
    }
}

static void on_llm_done(oi_status status, int http_status,
                        const char *error_body, size_t error_body_len,
                        void *user_data) {
    oi_cli_conversation *conversation = user_data;
    conversation->request = NULL;
    conversation->http_status = http_status;
    if (status != OI_OK) {
        if (error_body != NULL && error_body_len > 0) {
            struct oi_cli_conversation_event error = {
                .type = OI_CLI_CONVERSATION_EVENT_MODEL_ERROR,
                .as.model_error = {http_status, error_body, error_body_len},
            };
            oi_status event_status = emit(conversation, &error);
            if (event_status != OI_OK) {
                status = event_status;
            }
        }
        if (conversation->assistant.len > 0) {
            struct oi_cli_conversation_event partial = {
                .type = OI_CLI_CONVERSATION_EVENT_PARTIAL_ASSISTANT,
                .as.bytes = {conversation->assistant.data,
                             conversation->assistant.len},
            };
            oi_status event_status = emit(conversation, &partial);
            if (event_status != OI_OK) {
                status = event_status;
            }
        }
        finish_turn(conversation, status);
        return;
    }

    struct oi_cli_message assistant;
    oi_cli_message_init(&assistant);
    oi_status st = build_assistant_message(conversation, &assistant);
    if (st == OI_OK) {
        st = commit_message(conversation, &assistant,
                            OI_CLI_CONVERSATION_TOOL_NONE, NULL, 0, 0,
                            /*is_repair=*/0);
    }
    oi_cli_message_free(&assistant);
    if (st != OI_OK) {
        finish_turn(conversation, st);
        return;
    }
    conversation->assistant_message_index = conversation->messages.len - 1;
    size_t tool_calls_len =
        conversation->messages
            .items[conversation->assistant_message_index]
            .tool_calls_len;
    clear_streamed_response(conversation);

    struct oi_cli_conversation_event response_done = {
        .type = OI_CLI_CONVERSATION_EVENT_RESPONSE_DONE,
    };
    st = emit(conversation, &response_done);
    if (st != OI_OK) {
        finish_turn(conversation, st);
    } else if (tool_calls_len == 0) {
        finish_turn(conversation, OI_OK);
    } else {
        conversation->tool_index = 0;
        start_next_tool(conversation);
    }
}

static oi_status start_model_request(oi_cli_conversation *conversation) {
    if (conversation->model_steps >=
        conversation->config.max_model_steps) {
        return OI_ERR_INVAL;
    }
    oi_json_writer *writer = NULL;
    oi_status st = build_request_body(conversation, &writer);
    if (st != OI_OK) {
        return st;
    }
    size_t body_len;
    const char *body = oi_json_writer_data(writer, &body_len);
    conversation->model_steps++;
    st = oi_llm_request_start_events(
        conversation->client, conversation->reactor, conversation->arena,
        body, body_len, on_llm_event, on_llm_done, conversation,
        &conversation->request);
    oi_json_writer_destroy(writer);
    return st;
}

oi_status oi_cli_conversation_create(
    oi_llm_client *client, oi_reactor *reactor, oi_arena *arena,
    oi_tool_registry *tools,
    const struct oi_cli_conversation_config *config,
    const struct oi_cli_message_list *initial_context,
    oi_cli_conversation **out_conversation) {
    if (client == NULL || reactor == NULL || arena == NULL || tools == NULL ||
        config == NULL || config->model == NULL ||
        config->model[0] == '\0' || config->max_model_steps <= 0 ||
        config->tool_timeout_ms < 0 || out_conversation == NULL) {
        return OI_ERR_INVAL;
    }
    oi_cli_conversation *conversation =
        calloc(1, sizeof *conversation);
    if (conversation == NULL) {
        return OI_ERR_NOMEM;
    }
    conversation->client = client;
    conversation->reactor = reactor;
    conversation->arena = arena;
    conversation->tools = tools;
    conversation->config = *config;
    oi_cli_message_list_init(&conversation->messages);
    oi_status st = oi_cli_string_set(&conversation->model, config->model,
                                     strlen(config->model));
    if (st == OI_OK && initial_context != NULL) {
        for (size_t i = 0; st == OI_OK && i < initial_context->len; i++) {
            st = oi_cli_message_list_append_clone(
                &conversation->messages, &initial_context->items[i]);
        }
    }
    if (st != OI_OK) {
        oi_cli_conversation_destroy(conversation);
        return st;
    }
    *out_conversation = conversation;
    return OI_OK;
}

oi_status oi_cli_conversation_set_model(oi_cli_conversation *conversation,
                                        const char *model,
                                        size_t model_len) {
    if (conversation == NULL || model == NULL || model[0] == '\0') {
        return OI_ERR_INVAL;
    }
    return oi_cli_string_set(&conversation->model, model, model_len);
}

void oi_cli_conversation_destroy(oi_cli_conversation *conversation) {
    if (conversation == NULL) {
        return;
    }
    if (conversation->request != NULL) {
        oi_llm_request_cancel(conversation->request);
    }
    if (conversation->tool != NULL) {
        oi_tool_call_cancel(conversation->tool);
    }
    clear_streamed_response(conversation);
    buffer_free(&conversation->tool_output);
    oi_cli_message_list_free(&conversation->messages);
    oi_cli_string_free(&conversation->model);
    free(conversation);
}

oi_status oi_cli_conversation_start(oi_cli_conversation *conversation,
                                    const char *content,
                                    size_t content_len) {
    if (conversation == NULL || (content == NULL && content_len > 0) ||
        content_len > OI_CLI_HISTORY_MAX_CONTENT) {
        return OI_ERR_INVAL;
    }
    if (conversation->busy) {
        return OI_ERR_EXISTS;
    }
    oi_arena_reset(conversation->arena);
    clear_streamed_response(conversation);
    buffer_free(&conversation->tool_output);
    conversation->last_status = OI_OK;
    conversation->http_status = 0;
    conversation->model_steps = 0;
    conversation->cancelled = 0;
    conversation->busy = 1;

    struct oi_cli_message user;
    oi_cli_message_init(&user);
    oi_status st = oi_cli_message_set_user(&user, content, content_len);
    if (st == OI_OK) {
        st = commit_message(conversation, &user,
                            OI_CLI_CONVERSATION_TOOL_NONE, NULL, 0, 0,
                            /*is_repair=*/0);
    }
    oi_cli_message_free(&user);
    if (st == OI_OK) {
        st = start_model_request(conversation);
    }
    if (st != OI_OK) {
        finish_turn(conversation, st);
    }
    return st;
}

static const char interrupted_turn_text[] =
    "[assistant turn interrupted before completion]";

/*
 * Closes out the turn's own assistant slot after a cancel, whether or not
 * any content had streamed in yet: oi_cli_conversation_start always commits
 * the user message before start_model_request ever runs, so durable replay
 * is already expecting a matching assistant message for this turn the
 * instant a turn becomes busy -- cancelling without ever supplying one
 * leaves replay stuck expecting an assistant message forever (the next
 * turn's user message is then itself invalid). Mirrors
 * cli_history_repair.c's append_interruption_marker, the equivalent repair
 * applied at process-restart replay time for a turn a crash left dangling;
 * marked as a repair (not a real model reply) since replay requires that
 * tag on the assistant message that closes out a turn for which a
 * PARTIAL_ASSISTANT record was already written.
 */
static oi_status repair_interrupted_turn(oi_cli_conversation *conversation) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    oi_status st = oi_cli_message_set_assistant(
        &message, interrupted_turn_text, sizeof interrupted_turn_text - 1);
    if (st == OI_OK) {
        st = commit_message(conversation, &message,
                            OI_CLI_CONVERSATION_TOOL_NONE, NULL, 0, 0,
                            /*is_repair=*/1);
    }
    oi_cli_message_free(&message);
    return st;
}

void oi_cli_conversation_cancel(oi_cli_conversation *conversation) {
    oi_status status = OI_OK;
    int need_interrupted_marker = 0;

    if (conversation == NULL || !conversation->busy) {
        return;
    }
    if (conversation->request != NULL) {
        oi_llm_request *request = conversation->request;
        conversation->request = NULL;
        if (conversation->assistant.len > 0) {
            struct oi_cli_conversation_event partial = {
                .type = OI_CLI_CONVERSATION_EVENT_PARTIAL_ASSISTANT,
                .as.bytes = {conversation->assistant.data,
                             conversation->assistant.len},
            };
            (void)emit(conversation, &partial);
        }
        oi_llm_request_cancel(request);
        need_interrupted_marker = 1;
    }
    if (conversation->tool != NULL) {
        oi_tool_call *tool = conversation->tool;
        conversation->tool = NULL;
        oi_tool_call_cancel(tool);
        /* Repairs conversation->messages so it stays protocol-valid: the
         * assistant message already committed by on_llm_done carries this
         * call (and any later ones in the same message) with no matching
         * tool-result yet. A repair-commit failure (e.g. a durable-storage
         * append failure) is a genuine structural problem, not an ordinary
         * cancel -- surface it as-is instead of masking it below. */
        status = repair_dangling_tool_calls(conversation,
                                            /*first_call_may_have_started=*/1);
        need_interrupted_marker = status == OI_OK;
    }
    if (need_interrupted_marker && status == OI_OK) {
        status = repair_interrupted_turn(conversation);
    }
    if (status == OI_OK) {
        conversation->cancelled = 1;
        status = OI_ERR_CLOSED;
    }
    finish_turn(conversation, status);
}

int oi_cli_conversation_is_busy(
    const oi_cli_conversation *conversation) {
    return conversation != NULL && conversation->busy;
}

int oi_cli_conversation_was_cancelled(
    const oi_cli_conversation *conversation) {
    return conversation != NULL && conversation->cancelled;
}

oi_status oi_cli_conversation_last_status(
    const oi_cli_conversation *conversation) {
    return conversation == NULL ? OI_ERR_INVAL : conversation->last_status;
}

const struct oi_cli_message_list *oi_cli_conversation_messages(
    const oi_cli_conversation *conversation) {
    return conversation == NULL ? NULL : &conversation->messages;
}
