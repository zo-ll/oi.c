#include "cli_history_repair.h"
#include "test.h"

#include <string.h>

static void append_record(struct oi_cli_history *history,
                          struct oi_cli_history_record *record) {
    CHECK_EQ(oi_cli_history_append_take(history, record), OI_OK);
}

static void append_transition(struct oi_cli_history *history,
                              struct oi_cli_history_record *record) {
    CHECK_EQ(oi_cli_history_record_set_transition(record, 1, 0), OI_OK);
    append_record(history, record);
}

static void append_message(struct oi_cli_history *history,
                           struct oi_cli_history_record *record,
                           struct oi_cli_message *message, uint64_t record_id,
                           enum oi_cli_history_message_source source,
                           const char *model,
                           enum oi_cli_history_tool_outcome outcome) {
    CHECK_EQ(oi_cli_history_record_set_message(
                 record, record_id, 1, message, source, model,
                 model == NULL ? 0 : strlen(model), outcome, NULL, 0, 0),
             OI_OK);
    append_record(history, record);
}

static void append_repairs(struct oi_cli_history *history,
                           const struct oi_cli_history *repairs) {
    for (size_t i = 0; i < repairs->len; i++) {
        CHECK_EQ(oi_cli_history_append_clone(history, &repairs->records[i]),
                 OI_OK);
    }
}

TEST(repairs_distinguish_started_and_unstarted_tools) {
    struct oi_cli_history history;
    struct oi_cli_history repairs;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state before;
    struct oi_cli_history_replay_state after;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_init(&repairs);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&before);
    oi_cli_history_replay_state_init(&after);
    oi_cli_message_init(&message);

    append_transition(&history, &record);
    CHECK_EQ(oi_cli_message_set_user(&message, "run", 3), OI_OK);
    append_message(&history, &record, &message, 2,
                   OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                   OI_CLI_HISTORY_TOOL_OUTCOME_NONE);
    CHECK_EQ(oi_cli_message_set_assistant(&message, "", 0), OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "started", 7, "shell", 5,
                                          "{}", 2),
             OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "waiting", 7, "shell", 5,
                                          "{}", 2),
             OI_OK);
    append_message(&history, &record, &message, 3,
                   OI_CLI_HISTORY_MESSAGE_NORMAL, "model",
                   OI_CLI_HISTORY_TOOL_OUTCOME_NONE);
    CHECK_EQ(oi_cli_history_record_set_tool_started(&record, 4, 1, "started",
                                                    7),
             OI_OK);
    append_record(&history, &record);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &before), OI_OK);
    CHECK_EQ(oi_cli_history_build_repairs(&before, &repairs), OI_OK);
    CHECK_EQ(repairs.len, 3);
    CHECK_EQ(repairs.records[0].record_id, 5);
    CHECK_EQ(repairs.records[0].as.message.tool_outcome,
             OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN);
    CHECK_EQ(repairs.records[1].as.message.tool_outcome,
             OI_CLI_HISTORY_TOOL_NOT_EXECUTED);
    CHECK_EQ(repairs.records[2].as.message.value.role,
             OI_CLI_MESSAGE_ASSISTANT);
    CHECK_EQ(repairs.records[2].as.message.source,
             OI_CLI_HISTORY_MESSAGE_REPAIR);

    append_repairs(&history, &repairs);
    CHECK_EQ(oi_cli_history_replay(NULL, &history, &after), OI_OK);
    CHECK(!after.needs_repair);
    CHECK_EQ(after.context_len, 5);
    CHECK_EQ(after.next_record_id, 8);
    CHECK_EQ(after.next_turn_id, 2);

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&before);
    oi_cli_history_replay_state_free(&after);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&repairs);
    oi_cli_history_free(&history);
}

TEST(interrupted_assistant_gets_one_marker) {
    struct oi_cli_history history;
    struct oi_cli_history repairs;
    struct oi_cli_history_record record;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message message;
    oi_cli_history_init(&history);
    oi_cli_history_init(&repairs);
    oi_cli_history_record_init(&record);
    oi_cli_history_replay_state_init(&state);
    oi_cli_message_init(&message);

    append_transition(&history, &record);
    CHECK_EQ(oi_cli_message_set_user(&message, "question", 8), OI_OK);
    append_message(&history, &record, &message, 2,
                   OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                   OI_CLI_HISTORY_TOOL_OUTCOME_NONE);
    CHECK_EQ(oi_cli_history_record_set_partial_assistant(
                 &record, 3, 1, "visible partial", 15, "model", 5),
             OI_OK);
    append_record(&history, &record);

    CHECK_EQ(oi_cli_history_replay(NULL, &history, &state), OI_OK);
    CHECK(state.has_partial_assistant);
    CHECK_EQ(oi_cli_history_build_repairs(&state, &repairs), OI_OK);
    CHECK_EQ(repairs.len, 1);
    CHECK_EQ(repairs.records[0].record_id, 4);
    CHECK_EQ(repairs.records[0].as.message.value.role,
             OI_CLI_MESSAGE_ASSISTANT);
    CHECK(strstr(repairs.records[0].as.message.value.content.data,
                 "interrupted") != NULL);

    oi_cli_message_free(&message);
    oi_cli_history_replay_state_free(&state);
    oi_cli_history_record_free(&record);
    oi_cli_history_free(&repairs);
    oi_cli_history_free(&history);
}

TEST(completed_history_does_not_request_repairs) {
    struct oi_cli_history_replay_state state;
    struct oi_cli_history repairs;
    oi_cli_history_replay_state_init(&state);
    oi_cli_history_init(&repairs);
    state.next_record_id = 1;

    CHECK_EQ(oi_cli_history_build_repairs(&state, &repairs), OI_ERR_INVAL);
    CHECK_EQ(repairs.len, 0);

    oi_cli_history_free(&repairs);
    oi_cli_history_replay_state_free(&state);
}

int main(void) {
    RUN(repairs_distinguish_started_and_unstarted_tools);
    RUN(interrupted_assistant_gets_one_marker);
    RUN(completed_history_does_not_request_repairs);
    return oi_test_report();
}
