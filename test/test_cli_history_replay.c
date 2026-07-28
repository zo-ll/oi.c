#include "cli_history_replay.h"
#include "test.h"

#include <string.h>

static void append_record(struct oi_cli_history *history,
                          struct oi_cli_history_record *record) {
    CHECK_EQ(oi_cli_history_append_take(history, record), OI_OK);
}

static void add_transition(struct oi_cli_history *history,
                           struct oi_cli_history_record *record,
                           uint64_t legacy_count) {
    CHECK_EQ(oi_cli_history_record_set_transition(
                 record, legacy_count + 1, legacy_count),
             OI_OK);
    append_record(history, record);
}

static void add_message(struct oi_cli_history *history,
                        struct oi_cli_history_record *record,
                        struct oi_cli_message *message, uint64_t record_id,
                        uint64_t turn_id,
                        enum oi_cli_history_message_source source,
                        const char *model,
                        enum oi_cli_history_tool_outcome outcome,
                        int has_raw) {
    static const unsigned char empty_raw[] = {0};
    CHECK_EQ(oi_cli_history_record_set_message(
                 record, record_id, turn_id, message, source, model,
                 model == NULL ? 0 : strlen(model), outcome,
                 has_raw ? empty_raw : NULL, 0, has_raw),
             OI_OK);
    append_record(history, record);
}

TEST(empty_and_legacy_histories_replay) {
    struct oi_cli_history history;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    struct oi_cli_message_list legacy;
    oi_cli_history_init(&history);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);
    oi_cli_message_list_init(&legacy);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_OK);
    CHECK(state.needs_transition);
    CHECK_EQ(state.next_record_id, 1);
    CHECK_EQ(state.next_turn_id, 1);

    CHECK_EQ(oi_cli_message_set_user(&message, "old question", 12), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&legacy, &message), OI_OK);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "old answer", 10), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&legacy, &message), OI_OK);
    CHECK_EQ(oi_cli_history_replay(&legacy, &history, &state), OI_OK);
    CHECK(state.needs_transition);
    CHECK(!state.needs_repair);
    CHECK_EQ(state.context_len, 2);
    CHECK_EQ(state.next_record_id, 3);
    CHECK_EQ(state.next_turn_id, 2);

    oi_cli_message_free(&message);
    oi_cli_message_list_free(&legacy);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_free(&history);
}

TEST(typed_history_continues_after_legacy_transition) {
    struct oi_cli_history history;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    struct oi_cli_message_list legacy;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);
    oi_cli_message_list_init(&legacy);

    CHECK_EQ(oi_cli_message_set_user(&message, "old q", 5), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&legacy, &message), OI_OK);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "old a", 5), OI_OK);
    CHECK_EQ(oi_cli_message_list_append_take(&legacy, &message), OI_OK);
    add_transition(&history, &record, 2);
    CHECK_EQ(oi_cli_message_set_user(&message, "new q", 5), OI_OK);
    add_message(&history, &record, &message, 4, 2,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "new a", 5), OI_OK);
    add_message(&history, &record, &message, 5, 2,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);

    CHECK_EQ(oi_cli_history_replay(&legacy, &history, &state), OI_OK);
    CHECK(!state.needs_transition);
    CHECK_EQ(state.context_len, 4);
    CHECK_EQ(state.next_record_id, 6);
    CHECK_EQ(state.next_turn_id, 3);

    oi_cli_message_free(&message);
    oi_cli_message_list_free(&legacy);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&history);
}

TEST(complete_tool_turn_reconstructs_context) {
    struct oi_cli_history history;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);

    add_transition(&history, &record, 0);
    CHECK_EQ(oi_cli_message_set_user(&message, "run", 3), OI_OK);
    add_message(&history, &record, &message, 2, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "", 0), OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "call-1", 6, "shell", 5,
                                          "{\"command\":\"true\"}", 18),
             OI_OK);
    add_message(&history, &record, &message, 3, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_history_record_set_tool_started(&record, 4, 1, "call-1",
                                                    6),
             OI_OK);
    append_record(&history, &record);
    CHECK_EQ(oi_cli_message_set_tool(&message, "call-1", 6, "", 0), OI_OK);
    add_message(&history, &record, &message, 5, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_COMPLETED, 1);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "done", 4), OI_OK);
    add_message(&history, &record, &message, 6, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_OK);
    CHECK(!state.needs_transition);
    CHECK(!state.needs_repair);
    CHECK_EQ(state.context_len, 4);
    CHECK_EQ(state.next_record_id, 7);
    CHECK_EQ(state.next_turn_id, 2);

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&history);
}

TEST(interrupted_tools_report_precise_repair_outcomes) {
    struct oi_cli_history history;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);

    add_transition(&history, &record, 0);
    CHECK_EQ(oi_cli_message_set_user(&message, "run", 3), OI_OK);
    add_message(&history, &record, &message, 2, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "", 0), OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "started", 7, "shell", 5,
                                          "{}", 2),
             OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "waiting", 7, "shell", 5,
                                          "{}", 2),
             OI_OK);
    add_message(&history, &record, &message, 3, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_history_record_set_tool_started(&record, 4, 1, "started",
                                                    7),
             OI_OK);
    append_record(&history, &record);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_OK);
    CHECK(state.needs_repair);
    CHECK_EQ(state.repair_turn_id, 1);
    CHECK_EQ(state.unresolved_tools_len, 2);
    CHECK(state.unresolved_tools[0].may_have_started);
    CHECK(!state.unresolved_tools[1].may_have_started);

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&history);
}

TEST(completed_tool_result_requires_started_record) {
    struct oi_cli_history history;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);

    add_transition(&history, &record, 0);
    CHECK_EQ(oi_cli_message_set_user(&message, "run", 3), OI_OK);
    add_message(&history, &record, &message, 2, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "", 0), OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "call", 4, "shell", 5,
                                          "{}", 2),
             OI_OK);
    add_message(&history, &record, &message, 3, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_message_set_tool(&message, "call", 4, "", 0), OI_OK);
    add_message(&history, &record, &message, 4, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_COMPLETED, 1);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_ERR_PARSE);

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&history);
}

TEST(partial_response_and_pending_queue_survive_replay) {
    struct oi_cli_history history;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);

    add_transition(&history, &record, 0);
    CHECK_EQ(oi_cli_message_set_user(&message, "first", 5), OI_OK);
    add_message(&history, &record, &message, 2, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_history_record_set_queued_input(&record, 3, 2, "queued",
                                                    6),
             OI_OK);
    append_record(&history, &record);
    CHECK_EQ(oi_cli_history_record_set_partial_assistant(
                 &record, 4, 1, "half", 4, "model", 5),
             OI_OK);
    append_record(&history, &record);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_OK);
    CHECK(state.needs_repair);
    CHECK(state.has_partial_assistant);
    CHECK(state.has_pending_input);
    CHECK_STREQ(state.pending_input.data, "queued");
    CHECK_EQ(state.context_len, 1);

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&history);
}

TEST(queue_consumption_requires_matching_next_user_message) {
    struct oi_cli_history history;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);

    add_transition(&history, &record, 0);
    CHECK_EQ(oi_cli_message_set_user(&message, "first", 5), OI_OK);
    add_message(&history, &record, &message, 2, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_history_record_set_queued_input(&record, 3, 2, "queued",
                                                    6),
             OI_OK);
    append_record(&history, &record);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "done", 4), OI_OK);
    add_message(&history, &record, &message, 4, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_history_record_set_queue_resolved(
                 &record, 5, 2, 3, OI_CLI_HISTORY_QUEUE_CONSUMED),
             OI_OK);
    append_record(&history, &record);
    CHECK_EQ(oi_cli_message_set_user(&message, "different", 9), OI_OK);
    add_message(&history, &record, &message, 6, 2,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_ERR_PARSE);

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&history);
}

TEST(interrupted_queue_consumption_restores_editable_input) {
    struct oi_cli_history history;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);

    add_transition(&history, &record, 0);
    CHECK_EQ(oi_cli_message_set_user(&message, "first", 5), OI_OK);
    add_message(&history, &record, &message, 2, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_history_record_set_queued_input(&record, 3, 2, "draft", 5),
             OI_OK);
    append_record(&history, &record);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "done", 4), OI_OK);
    add_message(&history, &record, &message, 4, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_history_record_set_queue_resolved(
                 &record, 5, 2, 3, OI_CLI_HISTORY_QUEUE_CONSUMED),
             OI_OK);
    append_record(&history, &record);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_OK);
    CHECK(state.has_pending_input);
    CHECK_STREQ(state.pending_input.data, "draft");
    CHECK(!state.needs_repair);

    CHECK_EQ(oi_cli_history_record_set_queue_resolved(
                 &record, 6, 2, 3, OI_CLI_HISTORY_QUEUE_DISCARDED),
             OI_OK);
    append_record(&history, &record);
    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_OK);
    CHECK(!state.has_pending_input);
    CHECK_EQ(state.next_turn_id, 2);

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&history);
}

TEST(checkpoint_replaces_only_the_context_prefix) {
    struct oi_cli_history history;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);

    add_transition(&history, &record, 0);
    CHECK_EQ(oi_cli_message_set_user(&message, "q1", 2), OI_OK);
    add_message(&history, &record, &message, 2, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "a1", 2), OI_OK);
    add_message(&history, &record, &message, 3, 1,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_message_set_user(&message, "q2", 2), OI_OK);
    add_message(&history, &record, &message, 4, 2,
                OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "a2", 2), OI_OK);
    add_message(&history, &record, &message, 5, 2,
                OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                OI_CLI_HISTORY_TOOL_OUTCOME_NONE, 0);
    CHECK_EQ(oi_cli_history_record_set_checkpoint(
                 &record, 6, "summary", 7, "model", 5, 2, 3),
             OI_OK);
    append_record(&history, &record);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_OK);
    CHECK_EQ(state.context_len, 3);
    CHECK_EQ(state.context[0].record_id, 6);
    CHECK_STREQ(state.context[0].message.content.data, "summary");
    CHECK_STREQ(state.context[1].message.content.data, "q2");

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&history);
}

int main(void) {
    RUN(empty_and_legacy_histories_replay);
    RUN(typed_history_continues_after_legacy_transition);
    RUN(complete_tool_turn_reconstructs_context);
    RUN(interrupted_tools_report_precise_repair_outcomes);
    RUN(completed_tool_result_requires_started_record);
    RUN(partial_response_and_pending_queue_survive_replay);
    RUN(queue_consumption_requires_matching_next_user_message);
    RUN(interrupted_queue_consumption_restores_editable_input);
    RUN(checkpoint_replaces_only_the_context_prefix);
    return oi_test_report();
}
