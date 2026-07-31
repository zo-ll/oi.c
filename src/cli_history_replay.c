#include "cli_history_replay.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "oi/arena.h"
#include "oi/json.h"

enum replay_phase {
    REPLAY_EXPECT_USER,
    REPLAY_EXPECT_ASSISTANT,
    REPLAY_EXPECT_TOOL_RESULTS
};

struct replay_tool {
    const struct oi_cli_tool_call_value *call;
    int started;
    int resolved;
};

struct replay_builder {
    struct oi_cli_history_replay_state state;
    enum replay_phase phase;
    uint64_t current_turn_id;
    uint64_t last_started_turn_id;
    struct replay_tool *tools;
    size_t tools_len;
    int partial_seen;
    int queue_consumed;
};

static int string_contains_nul(const struct oi_cli_string *string) {
    return string->len > 0 &&
           memchr(string->data, '\0', string->len) != NULL;
}

static oi_status validate_arguments(
    const struct oi_cli_tool_call_value *call) {
    if (string_contains_nul(&call->id) || string_contains_nul(&call->name)) {
        return OI_ERR_PARSE;
    }
    oi_arena *arena = oi_arena_create(OI_CLI_HISTORY_MAX_CONTENT + 1);
    if (arena == NULL) {
        return OI_ERR_NOMEM;
    }
    oi_json_parser *parser = oi_json_parser_create(arena);
    if (parser == NULL) {
        oi_arena_destroy(arena);
        return OI_ERR_NOMEM;
    }
    oi_status st =
        oi_json_parser_feed(parser, call->arguments.data, call->arguments.len);
    if (st == OI_OK) {
        st = oi_json_parser_finish(parser);
    }
    oi_json_value *root = oi_json_parser_root(parser);
    if (st == OI_OK &&
        (root == NULL || oi_json_type_of(root) != OI_JSON_OBJECT)) {
        st = OI_ERR_PARSE;
    }
    oi_json_parser_destroy(parser);
    oi_arena_destroy(arena);
    return st;
}

static oi_status context_reserve(struct oi_cli_history_replay_state *state,
                                 size_t needed) {
    if (needed <= state->context_cap) {
        return OI_OK;
    }
    size_t cap = state->context_cap == 0 ? 16 : state->context_cap;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2) {
            return OI_ERR_NOMEM;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof *state->context) {
        return OI_ERR_NOMEM;
    }
    struct oi_cli_history_context_item *context =
        realloc(state->context, cap * sizeof *context);
    if (context == NULL) {
        return OI_ERR_NOMEM;
    }
    state->context = context;
    state->context_cap = cap;
    return OI_OK;
}

static oi_status context_append(struct oi_cli_history_replay_state *state,
                                uint64_t record_id,
                                const struct oi_cli_message *message) {
    if (state->context_len == SIZE_MAX) {
        return OI_ERR_NOMEM;
    }
    oi_status st = context_reserve(state, state->context_len + 1);
    if (st != OI_OK) {
        return st;
    }
    struct oi_cli_history_context_item *item =
        &state->context[state->context_len];
    item->record_id = record_id;
    oi_cli_message_init(&item->message);
    st = oi_cli_message_clone(message, &item->message);
    if (st == OI_OK) {
        state->context_len++;
    }
    return st;
}

static void replay_tools_free(struct replay_builder *builder) {
    free(builder->tools);
    builder->tools = NULL;
    builder->tools_len = 0;
}

static struct replay_tool *find_tool(struct replay_builder *builder,
                                     const struct oi_cli_string *id) {
    for (size_t i = 0; i < builder->tools_len; i++) {
        const struct oi_cli_string *candidate = &builder->tools[i].call->id;
        if (candidate->len == id->len &&
            memcmp(candidate->data, id->data, id->len) == 0) {
            return &builder->tools[i];
        }
    }
    return NULL;
}

static int all_tools_resolved(const struct replay_builder *builder) {
    for (size_t i = 0; i < builder->tools_len; i++) {
        if (!builder->tools[i].resolved) {
            return 0;
        }
    }
    return 1;
}

static oi_status begin_tool_results(
    struct replay_builder *builder,
    const struct oi_cli_message *assistant_message) {
    if (assistant_message->tool_calls_len >
        SIZE_MAX / sizeof *builder->tools) {
        return OI_ERR_NOMEM;
    }
    struct replay_tool *tools =
        calloc(assistant_message->tool_calls_len, sizeof *tools);
    if (tools == NULL) {
        return OI_ERR_NOMEM;
    }
    for (size_t i = 0; i < assistant_message->tool_calls_len; i++) {
        oi_status st = validate_arguments(&assistant_message->tool_calls[i]);
        if (st != OI_OK) {
            free(tools);
            return st;
        }
        tools[i].call = &assistant_message->tool_calls[i];
        for (size_t j = 0; j < i; j++) {
            if (tools[j].call->id.len == tools[i].call->id.len &&
                memcmp(tools[j].call->id.data, tools[i].call->id.data,
                       tools[i].call->id.len) == 0) {
                free(tools);
                return OI_ERR_PARSE;
            }
        }
    }
    replay_tools_free(builder);
    builder->tools = tools;
    builder->tools_len = assistant_message->tool_calls_len;
    builder->phase = REPLAY_EXPECT_TOOL_RESULTS;
    return OI_OK;
}

static oi_status apply_user_message(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    const struct oi_cli_message *message = &record->as.message.value;
    if (builder->phase != REPLAY_EXPECT_USER ||
        record->turn_id != builder->last_started_turn_id + 1 ||
        record->turn_id == 0) {
        return OI_ERR_PARSE;
    }
    if (builder->queue_consumed) {
        if (!builder->state.has_pending_input ||
            record->turn_id != builder->state.pending_input_turn_id ||
            message->content.len != builder->state.pending_input.len ||
            memcmp(message->content.data, builder->state.pending_input.data,
                   message->content.len) != 0) {
            return OI_ERR_PARSE;
        }
        oi_cli_string_free(&builder->state.pending_input);
        builder->state.has_pending_input = 0;
        builder->state.pending_input_record_id = 0;
        builder->state.pending_input_turn_id = 0;
        builder->queue_consumed = 0;
    }
    oi_status st = context_append(&builder->state, record->record_id, message);
    if (st != OI_OK) {
        return st;
    }
    builder->last_started_turn_id = record->turn_id;
    builder->current_turn_id = record->turn_id;
    builder->phase = REPLAY_EXPECT_ASSISTANT;
    builder->partial_seen = 0;
    return OI_OK;
}

static oi_status apply_assistant_message(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    const struct oi_cli_history_message *history_message =
        &record->as.message;
    const struct oi_cli_message *message = &history_message->value;
    if (builder->phase != REPLAY_EXPECT_ASSISTANT ||
        record->turn_id != builder->current_turn_id) {
        return OI_ERR_PARSE;
    }
    if (history_message->model.data != NULL &&
        string_contains_nul(&history_message->model)) {
        return OI_ERR_PARSE;
    }
    if (builder->partial_seen &&
        history_message->source != OI_CLI_HISTORY_MESSAGE_REPAIR) {
        return OI_ERR_PARSE;
    }
    oi_status st = context_append(&builder->state, record->record_id, message);
    if (st != OI_OK) {
        return st;
    }
    builder->partial_seen = 0;
    if (message->tool_calls_len > 0) {
        return begin_tool_results(builder, message);
    }
    replay_tools_free(builder);
    builder->phase = REPLAY_EXPECT_USER;
    builder->current_turn_id = 0;
    return OI_OK;
}

static oi_status apply_tool_message(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    const struct oi_cli_history_message *history_message =
        &record->as.message;
    const struct oi_cli_message *message = &history_message->value;
    if (builder->phase != REPLAY_EXPECT_TOOL_RESULTS ||
        record->turn_id != builder->current_turn_id ||
        string_contains_nul(&message->tool_call_id)) {
        return OI_ERR_PARSE;
    }
    struct replay_tool *tool =
        find_tool(builder, &message->tool_call_id);
    if (tool == NULL || tool->resolved) {
        return OI_ERR_PARSE;
    }
    if ((history_message->tool_outcome ==
             OI_CLI_HISTORY_TOOL_COMPLETED &&
         !tool->started) ||
        (history_message->tool_outcome ==
             OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN &&
         !tool->started) ||
        (history_message->tool_outcome ==
             OI_CLI_HISTORY_TOOL_NOT_EXECUTED &&
         tool->started)) {
        return OI_ERR_PARSE;
    }
    oi_status st = context_append(&builder->state, record->record_id, message);
    if (st != OI_OK) {
        return st;
    }
    tool->resolved = 1;
    if (all_tools_resolved(builder)) {
        replay_tools_free(builder);
        builder->phase = REPLAY_EXPECT_ASSISTANT;
    }
    return OI_OK;
}

static oi_status apply_message(struct replay_builder *builder,
                               const struct oi_cli_history_record *record) {
    switch (record->as.message.value.role) {
    case OI_CLI_MESSAGE_USER:
        return apply_user_message(builder, record);
    case OI_CLI_MESSAGE_ASSISTANT:
        return apply_assistant_message(builder, record);
    case OI_CLI_MESSAGE_TOOL:
        return apply_tool_message(builder, record);
    case OI_CLI_MESSAGE_NONE:
        return OI_ERR_PARSE;
    }
    return OI_ERR_PARSE;
}

static oi_status apply_tool_started(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    if (builder->phase != REPLAY_EXPECT_TOOL_RESULTS ||
        record->turn_id != builder->current_turn_id ||
        string_contains_nul(&record->as.tool_started.tool_call_id)) {
        return OI_ERR_PARSE;
    }
    struct replay_tool *tool =
        find_tool(builder, &record->as.tool_started.tool_call_id);
    if (tool == NULL || tool->started || tool->resolved) {
        return OI_ERR_PARSE;
    }
    tool->started = 1;
    return OI_OK;
}

static oi_status apply_partial(struct replay_builder *builder,
                               const struct oi_cli_history_record *record) {
    if (builder->phase != REPLAY_EXPECT_ASSISTANT ||
        record->turn_id != builder->current_turn_id ||
        builder->partial_seen ||
        string_contains_nul(&record->as.partial_assistant.model)) {
        return OI_ERR_PARSE;
    }
    builder->partial_seen = 1;
    return OI_OK;
}

static oi_status apply_queued_input(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    if (builder->phase == REPLAY_EXPECT_USER ||
        builder->state.has_pending_input || builder->queue_consumed ||
        record->turn_id != builder->last_started_turn_id + 1) {
        return OI_ERR_PARSE;
    }
    oi_status st = oi_cli_string_set(&builder->state.pending_input,
                                     record->as.queued_input.content.data,
                                     record->as.queued_input.content.len);
    if (st != OI_OK) {
        return st;
    }
    builder->state.pending_input_record_id = record->record_id;
    builder->state.pending_input_turn_id = record->turn_id;
    builder->state.has_pending_input = 1;
    return OI_OK;
}

static oi_status apply_queue_resolved(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    if (builder->phase != REPLAY_EXPECT_USER ||
        !builder->state.has_pending_input ||
        record->turn_id != builder->state.pending_input_turn_id ||
        record->as.queue_resolved.queued_record_id !=
            builder->state.pending_input_record_id) {
        return OI_ERR_PARSE;
    }
    if (builder->queue_consumed) {
        if (record->as.queue_resolved.resolution !=
            OI_CLI_HISTORY_QUEUE_DISCARDED) {
            return OI_ERR_PARSE;
        }
        oi_cli_string_free(&builder->state.pending_input);
        builder->state.pending_input_record_id = 0;
        builder->state.pending_input_turn_id = 0;
        builder->state.has_pending_input = 0;
        builder->queue_consumed = 0;
        return OI_OK;
    }
    if (record->as.queue_resolved.resolution ==
        OI_CLI_HISTORY_QUEUE_CONSUMED) {
        builder->queue_consumed = 1;
    } else {
        oi_cli_string_free(&builder->state.pending_input);
        builder->state.pending_input_record_id = 0;
        builder->state.pending_input_turn_id = 0;
        builder->state.has_pending_input = 0;
    }
    return OI_OK;
}

static oi_status apply_checkpoint(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    if (builder->phase != REPLAY_EXPECT_USER ||
        builder->state.has_pending_input || builder->queue_consumed ||
        builder->state.context_len == 0 ||
        string_contains_nul(&record->as.checkpoint.model)) {
        return OI_ERR_PARSE;
    }
    uint64_t first = record->as.checkpoint.source_first_record_id;
    uint64_t last = record->as.checkpoint.source_last_record_id;
    if (first > builder->state.context[0].record_id ||
        last < builder->state.context[0].record_id) {
        return OI_ERR_PARSE;
    }
    size_t remove_count = 0;
    while (remove_count < builder->state.context_len &&
           builder->state.context[remove_count].record_id <= last) {
        if (builder->state.context[remove_count].record_id < first) {
            return OI_ERR_PARSE;
        }
        remove_count++;
    }
    if (remove_count == 0) {
        return OI_ERR_PARSE;
    }

    struct oi_cli_message checkpoint;
    oi_cli_message_init(&checkpoint);
    oi_status st = oi_cli_message_set_assistant(
        &checkpoint, record->as.checkpoint.summary.data,
        record->as.checkpoint.summary.len);
    if (st != OI_OK) {
        return st;
    }
    for (size_t i = 0; i < remove_count; i++) {
        oi_cli_message_free(&builder->state.context[i].message);
    }
    size_t remaining = builder->state.context_len - remove_count;
    memmove(&builder->state.context[1],
            &builder->state.context[remove_count],
            remaining * sizeof *builder->state.context);
    builder->state.context[0].record_id = record->record_id;
    builder->state.context[0].message = checkpoint;
    builder->state.context_len = remaining + 1;
    /* Recorded only now that the splice has committed, so a rejected or
     * failed checkpoint never leaves the state claiming a compacted context
     * it does not have. */
    builder->state.checkpoint_source_first_record_id = first;
    builder->state.checkpoint_source_last_record_id = last;
    builder->state.has_checkpoint = 1;
    return OI_OK;
}

static oi_status apply_session_setting(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    if (builder->phase != REPLAY_EXPECT_USER ||
        builder->state.has_pending_input || builder->queue_consumed ||
        string_contains_nul(&record->as.session_setting.value)) {
        return OI_ERR_PARSE;
    }
    struct oi_cli_string *target =
        record->as.session_setting.field == OI_CLI_HISTORY_SESSION_SETTING_MODEL
            ? &builder->state.last_model
            : &builder->state.last_cwd;
    return oi_cli_string_set(target, record->as.session_setting.value.data,
                             record->as.session_setting.value.len);
}

static oi_status apply_typed_record(
    struct replay_builder *builder,
    const struct oi_cli_history_record *record) {
    if (builder->queue_consumed &&
        !(record->kind == OI_CLI_HISTORY_RECORD_MESSAGE &&
          record->as.message.value.role == OI_CLI_MESSAGE_USER) &&
        !(record->kind == OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED &&
          record->as.queue_resolved.resolution ==
              OI_CLI_HISTORY_QUEUE_DISCARDED)) {
        return OI_ERR_PARSE;
    }
    switch (record->kind) {
    case OI_CLI_HISTORY_RECORD_MESSAGE:
        return apply_message(builder, record);
    case OI_CLI_HISTORY_RECORD_TOOL_STARTED:
        return apply_tool_started(builder, record);
    case OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT:
        return apply_partial(builder, record);
    case OI_CLI_HISTORY_RECORD_QUEUED_INPUT:
        return apply_queued_input(builder, record);
    case OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED:
        return apply_queue_resolved(builder, record);
    case OI_CLI_HISTORY_RECORD_CHECKPOINT:
        return apply_checkpoint(builder, record);
    case OI_CLI_HISTORY_RECORD_SESSION_SETTING:
        return apply_session_setting(builder, record);
    case OI_CLI_HISTORY_RECORD_NONE:
    case OI_CLI_HISTORY_RECORD_TRANSITION:
        return OI_ERR_PARSE;
    }
    return OI_ERR_PARSE;
}

static oi_status apply_legacy(struct replay_builder *builder,
                              const struct oi_cli_message_list *legacy) {
    if (legacy == NULL) {
        return OI_OK;
    }
    for (size_t i = 0; i < legacy->len; i++) {
        const struct oi_cli_message *message = &legacy->items[i];
        enum oi_cli_message_role expected =
            i % 2 == 0 ? OI_CLI_MESSAGE_USER : OI_CLI_MESSAGE_ASSISTANT;
        if (!oi_cli_message_is_valid(message) || message->role != expected ||
            message->tool_calls_len != 0) {
            return OI_ERR_PARSE;
        }
        oi_status st = context_append(&builder->state, (uint64_t)i + 1,
                                      message);
        if (st != OI_OK) {
            return st;
        }
    }
    if (legacy->len > 0) {
        builder->last_started_turn_id =
            ((uint64_t)legacy->len + 1) / 2;
        if (legacy->len % 2 != 0) {
            builder->phase = REPLAY_EXPECT_ASSISTANT;
            builder->current_turn_id = builder->last_started_turn_id;
        }
    }
    return OI_OK;
}

static oi_status publish_unresolved(struct replay_builder *builder) {
    size_t count = 0;
    for (size_t i = 0; i < builder->tools_len; i++) {
        if (!builder->tools[i].resolved) {
            count++;
        }
    }
    if (count == 0) {
        return OI_OK;
    }
    if (count > SIZE_MAX / sizeof *builder->state.unresolved_tools) {
        return OI_ERR_NOMEM;
    }
    struct oi_cli_history_unresolved_tool *unresolved =
        calloc(count, sizeof *unresolved);
    if (unresolved == NULL) {
        return OI_ERR_NOMEM;
    }
    size_t output = 0;
    for (size_t i = 0; i < builder->tools_len; i++) {
        if (builder->tools[i].resolved) {
            continue;
        }
        oi_status st = oi_cli_string_set(
            &unresolved[output].tool_call_id,
            builder->tools[i].call->id.data, builder->tools[i].call->id.len);
        if (st != OI_OK) {
            for (size_t j = 0; j < output; j++) {
                oi_cli_string_free(&unresolved[j].tool_call_id);
            }
            free(unresolved);
            return st;
        }
        unresolved[output].may_have_started = builder->tools[i].started;
        output++;
    }
    builder->state.unresolved_tools = unresolved;
    builder->state.unresolved_tools_len = count;
    return OI_OK;
}

void oi_cli_history_replay_state_init(
    struct oi_cli_history_replay_state *state) {
    if (state != NULL) {
        memset(state, 0, sizeof *state);
    }
}

void oi_cli_history_replay_state_free(
    struct oi_cli_history_replay_state *state) {
    if (state == NULL) {
        return;
    }
    for (size_t i = 0; i < state->context_len; i++) {
        oi_cli_message_free(&state->context[i].message);
    }
    free(state->context);
    oi_cli_string_free(&state->pending_input);
    oi_cli_string_free(&state->last_model);
    oi_cli_string_free(&state->last_cwd);
    for (size_t i = 0; i < state->unresolved_tools_len; i++) {
        oi_cli_string_free(&state->unresolved_tools[i].tool_call_id);
    }
    free(state->unresolved_tools);
    memset(state, 0, sizeof *state);
}

oi_status oi_cli_history_replay(
    const struct oi_cli_message_list *legacy_messages,
    const struct oi_cli_history *typed_history,
    struct oi_cli_history_replay_state *out_state) {
    if (typed_history == NULL || out_state == NULL ||
        (legacy_messages != NULL && legacy_messages->len > UINT64_MAX)) {
        return OI_ERR_INVAL;
    }
    struct replay_builder builder;
    memset(&builder, 0, sizeof builder);
    builder.phase = REPLAY_EXPECT_USER;
    builder.state.needs_transition = 1;
    oi_status st = apply_legacy(&builder, legacy_messages);

    uint64_t legacy_count =
        legacy_messages == NULL ? 0 : (uint64_t)legacy_messages->len;
    if (st == OI_OK && typed_history->len > 0) {
        const struct oi_cli_history_record *transition =
            &typed_history->records[0];
        if (!oi_cli_history_record_is_valid(transition) ||
            transition->kind != OI_CLI_HISTORY_RECORD_TRANSITION ||
            transition->as.transition.legacy_record_count != legacy_count) {
            st = OI_ERR_PARSE;
        } else {
            builder.state.needs_transition = 0;
        }
    }
    for (size_t i = 1; st == OI_OK && i < typed_history->len; i++) {
        const struct oi_cli_history_record *record =
            &typed_history->records[i];
        if (!oi_cli_history_record_is_valid(record) ||
            record->record_id !=
                typed_history->records[i - 1].record_id + 1) {
            st = OI_ERR_PARSE;
            break;
        }
        st = apply_typed_record(&builder, record);
    }

    if (st == OI_OK && typed_history->len == 0 && legacy_count == UINT64_MAX) {
        st = OI_ERR_PARSE;
    }
    if (st == OI_OK) {
        uint64_t last_record_id =
            typed_history->len > 0
                ? typed_history->records[typed_history->len - 1].record_id
                : legacy_count;
        if (last_record_id == UINT64_MAX ||
            builder.last_started_turn_id == UINT64_MAX) {
            st = OI_ERR_PARSE;
        } else {
            builder.state.next_record_id = last_record_id + 1;
            builder.state.next_turn_id =
                builder.last_started_turn_id + 1;
            builder.state.needs_repair =
                builder.phase != REPLAY_EXPECT_USER;
            builder.state.repair_turn_id =
                builder.state.needs_repair ? builder.current_turn_id : 0;
            builder.state.has_partial_assistant = builder.partial_seen;
            st = publish_unresolved(&builder);
        }
    }
    replay_tools_free(&builder);
    if (st == OI_OK) {
        oi_cli_history_replay_state_free(out_state);
        *out_state = builder.state;
        oi_cli_history_replay_state_init(&builder.state);
    }
    oi_cli_history_replay_state_free(&builder.state);
    return st;
}
