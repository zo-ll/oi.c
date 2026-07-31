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
    /*
     * False from the moment start_model_request begins awaiting a new
     * assistant reply (the very first model round of a turn, or a later
     * round following resolved tool results) until on_llm_done commits one
     * -- lets close_out_incomplete_turn tell whether assistant_message_index
     * already refers to this round's reply (and so whether its tool_calls,
     * if any, might need repairing) or is still a stale index from a
     * previous round.
     */
    int assistant_committed;
    /*
     * Set by oi_cli_conversation_steer, consulted only at the top of
     * start_next_tool's loop body -- unlike cancel, steering never touches
     * request/tool directly; it just changes what happens the next time
     * that loop runs (whether entered fresh from on_llm_done or re-entered
     * from on_tool_done), letting whatever is already running finish
     * naturally.
     */
    int steering;
    /*
     * Set only between request_tool_call staging a call whose policy
     * decision is OI_TOOL_ASK and oi_cli_conversation_resolve_permission
     * supplying the eventual decision -- mutually exclusive with `tool`
     * (only ever set once a process has actually spawned) and `request`
     * (only ever set while awaiting a model reply): exactly one of the
     * three, or none, is non-NULL at any moment while busy.
     */
    oi_tool_call *pending_permission_tool;
    /*
     * Set for the duration of oi_cli_conversation_cancel's own unwind --
     * from before the partial-response event it emits until finish_turn
     * clears it. Purely for reporting: the events emitted during that
     * window (PARTIAL_ASSISTANT, then each repair MESSAGE) run callbacks
     * that can ask what the conversation is doing, and "streaming" or
     * "running a tool" would both be wrong answers by then, since cancel
     * has already detached whatever was in flight.
     */
    int cancelling;
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
    conversation->cancelling = 0;
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
static oi_status close_out_incomplete_turn(oi_cli_conversation *conversation,
                                           int first_call_may_have_started);
static void finish_turn_with_repair(oi_cli_conversation *conversation,
                                    oi_status status,
                                    int first_call_may_have_started);

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

static const char steered_tool_not_executed_text[] =
    "[tool not executed: skipped because a new message was queued]";
static const char denied_tool_not_executed_text[] =
    "[tool not executed: denied by user]";

/*
 * Steering's analog of repair_dangling_tool_calls, deliberately not a
 * reuse of it: that function's wording ("cancelled while it may have
 * already started") is simply false here. By construction, steering is
 * only ever consulted at the top of start_next_tool's loop -- the call
 * actively running (if any) always finishes normally through the ordinary
 * on_tool_done path first, so every call from tool_index onward here was
 * never started. Unlike a cancelled turn, the assistant reply that
 * produced these tool_calls already completed normally (RESPONSE_DONE
 * already fired), so there is no interrupted-turn bookend to add either --
 * the caller finishes the turn with OI_OK once this returns OI_OK.
 */
static oi_status skip_steered_tool_calls(oi_cli_conversation *conversation) {
    const struct oi_cli_message *assistant =
        &conversation->messages.items[conversation->assistant_message_index];
    const struct oi_cli_tool_call_value *tool_calls = assistant->tool_calls;
    size_t tool_calls_len = assistant->tool_calls_len;
    oi_status first_status = OI_OK;
    size_t i;

    for (i = conversation->tool_index; i < tool_calls_len; i++) {
        oi_status st = commit_tool_text(
            conversation, &tool_calls[i], steered_tool_not_executed_text,
            sizeof steered_tool_not_executed_text - 1,
            OI_CLI_CONVERSATION_TOOL_NOT_EXECUTED, NULL, 0, 0,
            /*is_repair=*/0);
        if (st != OI_OK && first_status == OI_OK) {
            first_status = st;
        }
    }
    return first_status;
}

static const char steered_turn_ended_text[] =
    "[assistant turn ended: a new message was queued]";

/*
 * Steering's own turn-closing bookend -- needed for exactly the reason
 * repair_interrupted_turn's own doc comment gives (durable replay requires
 * an assistant message with no tool_calls to return its phase machine to
 * "expect user" before anything else can validly follow), but with
 * accurate wording: unlike a cancelled/aborted turn, this assistant reply
 * already completed normally (RESPONSE_DONE already fired) -- it's
 * steering's own decision not to make the follow-up model round that
 * would ordinarily supply that bookend, so nothing else ever will.
 * Skipping this (as the original steering design assumed it could,
 * reasoning "the reply already completed, so there's no interrupted-turn
 * bookend to add") leaves the durable log with an unresolved phase the
 * moment all of a round's tool_calls happen to have already resolved
 * normally when steering is consulted -- the very next durable write
 * (e.g. resolving a queued item into its own turn) then fails replay
 * validation.
 */
static oi_status close_out_steered_turn(oi_cli_conversation *conversation) {
    struct oi_cli_message message;
    oi_status st;

    oi_cli_message_init(&message);
    st = oi_cli_message_set_assistant(&message, steered_turn_ended_text,
                                      sizeof steered_turn_ended_text - 1);
    if (st == OI_OK) {
        st = commit_message(conversation, &message,
                            OI_CLI_CONVERSATION_TOOL_NONE, NULL, 0, 0,
                            /*is_repair=*/1);
    }
    oi_cli_message_free(&message);
    return st;
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
        finish_turn_with_repair(conversation, st,
                               /*first_call_may_have_started=*/1);
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
        enum oi_cli_conversation_tool_outcome outcome =
            kind == OI_TOOL_EXIT_NORMAL && code == 0
                ? OI_CLI_CONVERSATION_TOOL_COMPLETED
                : OI_CLI_CONVERSATION_TOOL_FAILED;

        st = commit_tool_text(
            conversation, call, text.data, text.len, outcome,
            (const unsigned char *)conversation->tool_output.data,
            conversation->tool_output.len, 1, /*is_repair=*/0);
    }
    buffer_free(&text);
    buffer_free(&conversation->tool_output);
    if (st != OI_OK) {
        finish_turn_with_repair(conversation, st,
                               /*first_call_may_have_started=*/1);
        return;
    }
    conversation->tool_index++;
    start_next_tool(conversation);
}

/*
 * Today's synchronous ALLOW tail (TOOL_STARTING emit, the reentrant-cancel-
 * during-emit guard, conversation->tool = staged, oi_tool_call_resolve,
 * set_timeout/close_stdin) and today's synchronous DENY handling, unified
 * behind one `allow` bool -- called either immediately (a synchronous
 * OI_TOOL_ALLOW/OI_TOOL_DENY decision) or arbitrarily later, from
 * oi_cli_conversation_resolve_permission, once an OI_TOOL_ASK decision's
 * answer finally arrives. `call` must be freshly derived from
 * conversation->tool_index/assistant_message_index by the caller, never a
 * pointer held across the (potentially indefinite) async gap: committing
 * other messages in between can reallocate conversation->messages.items.
 */
static void resolve_permission_body(oi_cli_conversation *conversation,
                                    const struct oi_cli_tool_call_value *call,
                                    oi_tool_call *staged, int allow) {
    oi_status st;

    if (!allow) {
        (void)oi_tool_call_resolve(staged, 0);
        /* Committed directly (not via repair_dangling_tool_calls) so this
         * specific call gets its own distinct wording -- a denial is not
         * the same thing as a genuine cancel-before-start, which is what
         * repair_dangling_tool_calls' shared text otherwise means. Any
         * later tool_calls in the same round that never got this far are
         * still exactly a "cancelled before it started" case, so bumping
         * tool_index past just this one and letting the ordinary repair
         * path handle the rest (via finish_turn_with_repair below) is
         * correct for them. */
        st = commit_tool_text(conversation, call, denied_tool_not_executed_text,
                              sizeof denied_tool_not_executed_text - 1,
                              OI_CLI_CONVERSATION_TOOL_DENIED, NULL, 0,
                              0, /*is_repair=*/0);
        conversation->tool_index++;
        finish_turn_with_repair(conversation,
                                st != OI_OK ? st : OI_ERR_DENIED,
                                /*first_call_may_have_started=*/0);
        return;
    }

    struct oi_cli_conversation_event event = {
        .type = OI_CLI_CONVERSATION_EVENT_TOOL_STARTING,
        .as.tool_starting = {&call->id, &call->name, &call->arguments},
    };
    st = emit(conversation, &event);
    if (st != OI_OK) {
        oi_tool_call_cancel(staged);
        finish_turn_with_repair(conversation, st,
                                /*first_call_may_have_started=*/0);
        return;
    }
    if (!conversation->busy) {
        /* The embedder's on_event callback cancelled the conversation
         * reentrantly from within the TOOL_STARTING emit above (a
         * supported pattern one layer down, mirroring
         * oi_tool_call_cancel's own documented reentrancy from within
         * its own on_output). oi_cli_conversation_cancel's repair now
         * runs unconditionally (no longer gated on conversation->tool
         * != NULL), so that reentrant call already closed the turn out
         * fully -- nothing left to do here but tear down the
         * staged-but-never-started call. */
        oi_tool_call_cancel(staged);
        return;
    }
    conversation->tool = staged;
    st = oi_tool_call_resolve(staged, 1);
    if (st != OI_OK) {
        conversation->tool = NULL;
        finish_turn_with_repair(conversation, st,
                                /*first_call_may_have_started=*/1);
        return;
    }
    if (conversation->config.tool_timeout_ms > 0) {
        st = oi_tool_call_set_timeout(conversation->tool,
                                      conversation->config.tool_timeout_ms);
    }
    if (st == OI_OK) {
        st = oi_tool_call_close_stdin(conversation->tool);
    }
    if (st != OI_OK) {
        oi_tool_call *tool = conversation->tool;
        conversation->tool = NULL;
        oi_tool_call_cancel(tool);
        finish_turn_with_repair(conversation, st,
                                /*first_call_may_have_started=*/1);
    }
}

/*
 * Stages a tool call (already done by the caller) and asks the embedder's
 * policy what to do with it. OI_TOOL_ASK defers the actual decision:
 * conversation->pending_permission_tool holds the staged call, an
 * AWAITING_PERMISSION event is emitted, and this returns without resolving
 * anything -- the turn stays busy until oi_cli_conversation_resolve_permission
 * is called, arbitrarily later, from anywhere. ALLOW/DENY resolve
 * immediately via resolve_permission_body, exactly as before this split.
 */
static void request_tool_call(oi_cli_conversation *conversation,
                              const struct oi_cli_tool_call_value *call,
                              const oi_json_value *arguments,
                              oi_tool_call *staged) {
    oi_tool_decision decision =
        conversation->config.permission == NULL
            ? OI_TOOL_ALLOW
            : conversation->config.permission(
                  call->name.data, arguments,
                  conversation->config.permission_user_data);

    if (decision == OI_TOOL_ASK) {
        struct oi_cli_conversation_event event = {
            .type = OI_CLI_CONVERSATION_EVENT_AWAITING_PERMISSION,
            .as.awaiting_permission = {&call->id, &call->name,
                                      &call->arguments},
        };
        oi_status st;

        conversation->pending_permission_tool = staged;
        st = emit(conversation, &event);
        if (st != OI_OK) {
            conversation->pending_permission_tool = NULL;
            oi_tool_call_cancel(staged);
            finish_turn_with_repair(conversation, st,
                                    /*first_call_may_have_started=*/0);
            return;
        }
        if (!conversation->busy) {
            /* Reentrant cancel from within the AWAITING_PERMISSION emit
             * above, mirroring the same TOOL_STARTING reentrancy pattern
             * documented in resolve_permission_body --
             * oi_cli_conversation_cancel's own pending_permission_tool
             * branch already cleared it and cancelled staged. */
            return;
        }
        return;
    }
    resolve_permission_body(conversation, call, staged,
                            decision == OI_TOOL_ALLOW);
}

static void start_next_tool(oi_cli_conversation *conversation) {
    while (conversation->busy) {
        const struct oi_cli_message *assistant =
            &conversation->messages
                 .items[conversation->assistant_message_index];
        if (conversation->steering) {
            /* Consulted here and only here -- entered fresh from
             * on_llm_done (tool_index == 0) or re-entered from
             * on_tool_done (tool_index already past whatever just
             * finished normally) -- so it doesn't matter whether steering
             * was requested mid-stream, mid-tool, or between rounds: any
             * remaining tool_calls this round are skipped, and no new
             * model round starts either (one more round would only
             * produce more tool_calls that get skipped right back here on
             * the next re-entry, at the cost of a wasted request). Either
             * way, close_out_steered_turn's bookend is still required --
             * skipping some calls doesn't produce it, and when there was
             * nothing left to skip (the round's only tool_call already
             * resolved normally), nothing else would supply it at all. */
            oi_status st = skip_steered_tool_calls(conversation);
            if (st == OI_OK) {
                st = close_out_steered_turn(conversation);
            }
            finish_turn(conversation, st);
            return;
        }
        if (conversation->tool_index >= assistant->tool_calls_len) {
            oi_status st = start_model_request(conversation);
            if (st != OI_OK) {
                finish_turn_with_repair(conversation, st,
                                        /*first_call_may_have_started=*/0);
            }
            return;
        }
        const struct oi_cli_tool_call_value *call =
            &assistant->tool_calls[conversation->tool_index];
        oi_json_value *arguments = NULL;
        oi_status st =
            parse_arguments(conversation, &call->arguments, &arguments);
        if (st != OI_OK) {
            finish_turn_with_repair(conversation, st,
                                    /*first_call_may_have_started=*/0);
            return;
        }

        oi_tool_call *staged = NULL;
        st = oi_tool_call_start(
            conversation->tools, conversation->reactor, conversation->arena,
            call->name.data, arguments, always_stage, NULL, on_tool_output,
            on_tool_done, conversation, &staged);
        if (st != OI_OK) {
            finish_turn_with_repair(conversation, st,
                                    /*first_call_may_have_started=*/0);
            return;
        }
        request_tool_call(conversation, call, arguments, staged);
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
        finish_turn_with_repair(conversation, st,
                               /*first_call_may_have_started=*/0);
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
        finish_turn_with_repair(conversation, status,
                               /*first_call_may_have_started=*/0);
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
        finish_turn_with_repair(conversation, st,
                               /*first_call_may_have_started=*/0);
        return;
    }
    conversation->assistant_message_index = conversation->messages.len - 1;
    conversation->assistant_committed = 1;
    conversation->tool_index = 0;
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
        finish_turn_with_repair(conversation, st,
                               /*first_call_may_have_started=*/0);
    } else if (tool_calls_len == 0) {
        finish_turn(conversation, OI_OK);
    } else {
        start_next_tool(conversation);
    }
}

static oi_status start_model_request(oi_cli_conversation *conversation) {
    /* About to await a new assistant reply (the turn's first model round,
     * or a later one following resolved tool results): nothing commits it
     * until on_llm_done runs, so close_out_incomplete_turn must not treat
     * assistant_message_index as belonging to this round until then. */
    conversation->assistant_committed = 0;
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
    if (conversation->pending_permission_tool != NULL) {
        oi_tool_call_cancel(conversation->pending_permission_tool);
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
    conversation->cancelling = 0;
    conversation->steering = 0;
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

/*
 * The single place that decides what (if anything) needs repairing before a
 * turn ends any way other than a normal, fully-resolved completion --
 * cancellation, tool denial, a mid-loop failure, or anything else. Whether
 * this round's assistant message has been committed yet
 * (assistant_committed) and, if so, whether all of its tool_calls already
 * have a matching result (tool_index vs. tool_calls_len) together say
 * exactly how much of the turn's protocol shape is still open; closing it
 * is always repair_dangling_tool_calls (if anything there is still open)
 * followed by repair_interrupted_turn (unconditionally, since neither an
 * uncommitted assistant reply nor a pending tool loop leaves replay back at
 * REPLAY_EXPECT_USER on its own). Returns the first repair-commit failure,
 * if any -- a genuine structural problem that should replace the turn's
 * original status rather than being masked by it, matching how
 * oi_cli_conversation_cancel already treated its own repair failures.
 */
static oi_status close_out_incomplete_turn(oi_cli_conversation *conversation,
                                           int first_call_may_have_started) {
    oi_status status = OI_OK;
    if (conversation->assistant_committed) {
        const struct oi_cli_message *assistant =
            &conversation->messages
                 .items[conversation->assistant_message_index];
        if (conversation->tool_index < assistant->tool_calls_len) {
            status = repair_dangling_tool_calls(conversation,
                                                first_call_may_have_started);
        }
    }
    if (status == OI_OK) {
        status = repair_interrupted_turn(conversation);
    }
    return status;
}

/* Convenience wrapper for the common case: repair, then finish with
 * whichever of the repair failure or the turn's own status is worse
 * (repair failure wins, since it's the more structural problem). */
static void finish_turn_with_repair(oi_cli_conversation *conversation,
                                    oi_status status,
                                    int first_call_may_have_started) {
    oi_status repair_status =
        close_out_incomplete_turn(conversation, first_call_may_have_started);
    finish_turn(conversation,
               repair_status != OI_OK ? repair_status : status);
}

void oi_cli_conversation_cancel(oi_cli_conversation *conversation) {
    oi_status status;
    int first_call_may_have_started = 0;

    if (conversation == NULL || !conversation->busy) {
        return;
    }
    /* Before the first event this unwind emits, so no callback can see a
     * conversation that still claims to be streaming or running a tool. */
    conversation->cancelling = 1;
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
    }
    if (conversation->pending_permission_tool != NULL) {
        oi_tool_call *staged = conversation->pending_permission_tool;
        conversation->pending_permission_tool = NULL;
        oi_tool_call_cancel(staged);
        /* first_call_may_have_started stays 0: nothing was ever spawned
         * for a call still awaiting a permission decision. */
    }
    if (conversation->tool != NULL) {
        oi_tool_call *tool = conversation->tool;
        conversation->tool = NULL;
        oi_tool_call_cancel(tool);
        first_call_may_have_started = 1;
    }
    status = close_out_incomplete_turn(conversation,
                                       first_call_may_have_started);
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

enum oi_cli_conversation_activity oi_cli_conversation_activity(
    const oi_cli_conversation *conversation) {
    if (conversation == NULL) {
        return OI_CLI_CONVERSATION_ACTIVITY_IDLE;
    }
    if (!conversation->busy) {
        /* A completed cancel is idle, not failed, even though it leaves a
         * non-OK last_status behind (OI_ERR_CLOSED): the same reasoning
         * cli_repl.c applies when it treats a cancelled turn as always
         * recoverable. Reporting the status the cancel happened to carry
         * would describe a working prompt as broken. */
        return conversation->last_status == OI_OK || conversation->cancelled
                   ? OI_CLI_CONVERSATION_ACTIVITY_IDLE
                   : OI_CLI_CONVERSATION_ACTIVITY_FAILED;
    }
    /* Checked before the three in-flight kinds: cancel detaches whichever
     * one was set before emitting anything, so by the time a callback can
     * observe this they are all NULL anyway -- ordering it first states the
     * intent rather than relying on that. */
    if (conversation->cancelling) {
        return OI_CLI_CONVERSATION_ACTIVITY_CANCELLING;
    }
    if (conversation->request != NULL) {
        return OI_CLI_CONVERSATION_ACTIVITY_STREAMING;
    }
    if (conversation->pending_permission_tool != NULL) {
        return OI_CLI_CONVERSATION_ACTIVITY_AWAITING_PERMISSION;
    }
    if (conversation->tool != NULL) {
        return OI_CLI_CONVERSATION_ACTIVITY_TOOL_RUNNING;
    }
    return OI_CLI_CONVERSATION_ACTIVITY_WORKING;
}

int oi_cli_conversation_was_cancelled(
    const oi_cli_conversation *conversation) {
    return conversation != NULL && conversation->cancelled;
}

void oi_cli_conversation_steer(oi_cli_conversation *conversation) {
    if (conversation == NULL || !conversation->busy) {
        return;
    }
    conversation->steering = 1;
}

int oi_cli_conversation_is_steering(
    const oi_cli_conversation *conversation) {
    return conversation != NULL && conversation->steering;
}

oi_status oi_cli_conversation_resolve_permission(
    oi_cli_conversation *conversation, int allow) {
    oi_tool_call *staged;
    const struct oi_cli_message *assistant;
    const struct oi_cli_tool_call_value *call;

    if (conversation == NULL || conversation->pending_permission_tool == NULL) {
        return OI_ERR_INVAL;
    }
    staged = conversation->pending_permission_tool;
    conversation->pending_permission_tool = NULL;
    /* Freshly derived, not a pointer carried across the async gap since
     * this was staged: committing other messages in between (e.g. a
     * steered sibling call's placeholder) can reallocate
     * conversation->messages.items, but tool_index/assistant_message_index
     * themselves stay valid the whole time nothing has resolved yet. */
    assistant =
        &conversation->messages.items[conversation->assistant_message_index];
    call = &assistant->tool_calls[conversation->tool_index];
    resolve_permission_body(conversation, call, staged, allow != 0);
    return OI_OK;
}

oi_status oi_cli_conversation_last_status(
    const oi_cli_conversation *conversation) {
    return conversation == NULL ? OI_ERR_INVAL : conversation->last_status;
}

const struct oi_cli_message_list *oi_cli_conversation_messages(
    const oi_cli_conversation *conversation) {
    return conversation == NULL ? NULL : &conversation->messages;
}

oi_arena *oi_cli_conversation_arena(const oi_cli_conversation *conversation) {
    return conversation == NULL ? NULL : conversation->arena;
}

oi_status oi_cli_conversation_apply_checkpoint(
    oi_cli_conversation *conversation, size_t prefix_count,
    const char *summary, size_t summary_len) {
    struct oi_cli_message checkpoint;
    oi_status st;

    if (conversation == NULL || conversation->busy || prefix_count == 0 ||
        prefix_count > conversation->messages.len) {
        return OI_ERR_INVAL;
    }

    oi_cli_message_init(&checkpoint);
    st = oi_cli_message_set_assistant(&checkpoint, summary, summary_len);
    if (st != OI_OK) {
        return st;
    }
    st = oi_cli_conversation_apply_checkpoint_take_summary(
        conversation, prefix_count, &checkpoint.content.data,
        checkpoint.content.len);
    if (st == OI_OK) {
        checkpoint.content.len = 0;
    }
    oi_cli_message_free(&checkpoint);
    return st;
}

oi_status oi_cli_conversation_apply_checkpoint_take_summary(
    oi_cli_conversation *conversation, size_t prefix_count, char **summary,
    size_t summary_len) {
    struct oi_cli_message checkpoint;
    struct oi_cli_message *items;
    size_t remaining;

    if (conversation == NULL || conversation->busy || prefix_count == 0 ||
        prefix_count > conversation->messages.len || summary == NULL ||
        *summary == NULL) {
        return OI_ERR_INVAL;
    }

    oi_cli_message_init(&checkpoint);
    checkpoint.role = OI_CLI_MESSAGE_ASSISTANT;
    checkpoint.content.data = *summary;
    checkpoint.content.len = summary_len;
    items = conversation->messages.items;
    for (size_t i = 0; i < prefix_count; i++) {
        oi_cli_message_free(&items[i]);
    }
    remaining = conversation->messages.len - prefix_count;
    memmove(&items[1], &items[prefix_count], remaining * sizeof *items);
    items[0] = checkpoint;
    conversation->messages.len = remaining + 1;
    *summary = NULL;
    return OI_OK;
}
