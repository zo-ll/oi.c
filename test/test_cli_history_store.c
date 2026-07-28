#include "cli_history_store.h"
#include "test.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char test_path[256];

static const char *fresh_path(void) {
    static int counter;
    snprintf(test_path, sizeof test_path, "/tmp/oi_history_store_%d_%d.log",
             (int)getpid(), counter++);
    unlink(test_path);
    return test_path;
}

static void set_transition(struct oi_cli_history_record *record,
                           uint64_t legacy_count) {
    CHECK_EQ(oi_cli_history_record_set_transition(
                 record, legacy_count + 1, legacy_count),
             OI_OK);
}

static void set_user_record(struct oi_cli_history_record *record,
                            uint64_t record_id, uint64_t turn_id,
                            const char *content) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    CHECK_EQ(oi_cli_message_set_user(&message, content, strlen(content)),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 record, record_id, turn_id, &message,
                 OI_CLI_HISTORY_MESSAGE_NORMAL, NULL, 0,
                 OI_CLI_HISTORY_TOOL_OUTCOME_NONE, NULL, 0, 0),
             OI_OK);
    oi_cli_message_free(&message);
}

TEST(fresh_log_appends_and_reloads_typed_history) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_history_record record;
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    oi_cli_history_record_init(&record);

    CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);
    CHECK(state.needs_transition);
    set_transition(&record, 0);
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
    set_user_record(&record, 2, 1, "hello");
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
    CHECK(state.needs_repair);
    CHECK_EQ(state.context_len, 1);

    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);

    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);
    CHECK(!state.needs_transition);
    CHECK(state.needs_repair);
    CHECK_EQ(store.typed_history.len, 2);
    CHECK_STREQ(state.context[0].message.content.data, "hello");

    oi_cli_history_record_free(&record);
    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);
    unlink(path);
}

TEST(legacy_records_gain_an_explicit_transition) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "old question", 12), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "old answer", 10), OI_OK);

    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_history_record record;
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    oi_cli_history_record_init(&record);
    CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);
    CHECK_EQ(store.legacy_messages.len, 2);
    CHECK(state.needs_transition);
    CHECK_EQ(state.next_record_id, 3);

    set_transition(&record, 2);
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
    CHECK(!state.needs_transition);
    set_user_record(&record, 4, 2, "new question");
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);

    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);
    CHECK_EQ(store.legacy_messages.len, 2);
    CHECK_EQ(store.typed_history.len, 2);
    CHECK_EQ(state.context_len, 3);
    CHECK_STREQ(state.context[2].message.content.data, "new question");

    oi_cli_history_record_free(&record);
    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);
    unlink(path);
}

TEST(typed_corruption_after_transition_is_not_legacy) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_history_record record;
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    oi_cli_history_record_init(&record);
    CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);
    set_transition(&record, 0);
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "{\"version\":99}", 14), OI_OK);

    struct oi_cli_history_store failed_store;
    struct oi_cli_history_replay_state failed_state;
    oi_cli_history_store_init(&failed_store);
    oi_cli_history_replay_state_init(&failed_state);
    CHECK_EQ(oi_cli_history_store_load(log, &failed_store, &failed_state),
             OI_ERR_PARSE);
    CHECK_EQ(failed_store.typed_history.len, 0);

    oi_cli_history_store_free(&failed_store);
    oi_cli_history_replay_state_free(&failed_state);
    oi_cli_history_record_free(&record);
    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);
    unlink(path);
}

TEST(append_rejects_invalid_protocol_without_writing) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_history_record record;
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    oi_cli_history_record_init(&record);
    CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);
    set_transition(&record, 0);
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);

    set_user_record(&record, 2, 2, "skipped turn");
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state),
             OI_ERR_PARSE);
    CHECK_EQ(store.typed_history.len, 1);

    oi_cli_history_record_free(&record);
    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);
    unlink(path);
}

int main(void) {
    RUN(fresh_log_appends_and_reloads_typed_history);
    RUN(legacy_records_gain_an_explicit_transition);
    RUN(typed_corruption_after_transition_is_not_legacy);
    RUN(append_rejects_invalid_protocol_without_writing);
    return oi_test_report();
}
