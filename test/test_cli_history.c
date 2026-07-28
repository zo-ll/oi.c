#include "cli_history.h"
#include "test.h"

#include <string.h>

TEST(normal_tool_result_preserves_raw_bytes) {
    static const unsigned char raw[] = {'a', '\0', 0xff, 'b'};
    static const char model_text[] = {'a', (char)0xef, (char)0xbf,
                                      (char)0xbd, 'b'};
    struct oi_cli_message message;
    struct oi_cli_history_record record;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&record);

    CHECK_EQ(oi_cli_message_set_tool(&message, "call-1", 6, model_text,
                                     sizeof model_text),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 &record, 3, 1, &message, OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                 0, OI_CLI_HISTORY_TOOL_COMPLETED, raw, sizeof raw, 1),
             OI_OK);
    CHECK(oi_cli_history_record_is_valid(&record));
    CHECK_EQ(record.as.message.raw_tool_output.len, sizeof raw);
    CHECK(memcmp(record.as.message.raw_tool_output.data, raw, sizeof raw) == 0);
    CHECK_EQ(record.as.message.value.content.len, 5);

    oi_cli_message_free(&message);
    oi_cli_history_record_free(&record);
}

TEST(repair_tool_results_have_no_raw_output) {
    struct oi_cli_message message;
    struct oi_cli_history_record record;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&record);

    CHECK_EQ(oi_cli_message_set_tool(&message, "call-1", 6,
                                     "[outcome unknown]", 17),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 &record, 4, 1, &message, OI_CLI_HISTORY_MESSAGE_REPAIR, NULL,
                 0, OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN, NULL, 0, 0),
             OI_OK);
    CHECK(oi_cli_history_record_is_valid(&record));
    CHECK(!record.as.message.has_raw_tool_output);

    CHECK_EQ(oi_cli_history_record_set_message(
                 &record, 4, 1, &message, OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                 0, OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN, NULL, 0, 0),
             OI_ERR_INVAL);

    oi_cli_message_free(&message);
    oi_cli_history_record_free(&record);
}

TEST(partial_assistant_is_audit_only_record) {
    struct oi_cli_history_record record;
    oi_cli_history_record_init(&record);

    CHECK_EQ(oi_cli_history_record_set_partial_assistant(
                 &record, 8, 2, "partial", 7, "gpt-test", 8),
             OI_OK);
    CHECK_EQ(record.kind, OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT);
    CHECK_STREQ(record.as.partial_assistant.content.data, "partial");
    CHECK(oi_cli_history_record_is_valid(&record));
    CHECK_EQ(oi_cli_history_record_set_partial_assistant(
                 &record, 8, 2, "partial", 7, NULL, 0),
             OI_ERR_INVAL);
    CHECK_STREQ(record.as.partial_assistant.content.data, "partial");

    oi_cli_history_record_free(&record);
}

TEST(queue_lifecycle_uses_stable_record_reference) {
    struct oi_cli_history_record queued;
    struct oi_cli_history_record resolved;
    oi_cli_history_record_init(&queued);
    oi_cli_history_record_init(&resolved);

    CHECK_EQ(oi_cli_history_record_set_queued_input(&queued, 9, 3, "next", 4),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_queue_resolved(
                 &resolved, 10, 3, 9, OI_CLI_HISTORY_QUEUE_CONSUMED),
             OI_OK);
    CHECK_EQ(resolved.as.queue_resolved.queued_record_id, 9);
    CHECK(oi_cli_history_record_is_valid(&queued));
    CHECK(oi_cli_history_record_is_valid(&resolved));

    oi_cli_history_record_free(&queued);
    oi_cli_history_record_free(&resolved);
}

TEST(checkpoint_requires_prior_nonempty_range) {
    struct oi_cli_history_record checkpoint;
    oi_cli_history_record_init(&checkpoint);

    CHECK_EQ(oi_cli_history_record_set_checkpoint(
                 &checkpoint, 20, "summary", 7, "gpt-test", 8, 2, 17),
             OI_OK);
    CHECK(oi_cli_history_record_is_valid(&checkpoint));
    CHECK_EQ(checkpoint.as.checkpoint.source_first_record_id, 2);
    CHECK_EQ(checkpoint.as.checkpoint.source_last_record_id, 17);

    CHECK_EQ(oi_cli_history_record_set_checkpoint(
                 &checkpoint, 20, "summary", 7, NULL, 0, 17, 2),
             OI_ERR_INVAL);
    CHECK_EQ(checkpoint.as.checkpoint.source_first_record_id, 2);
    oi_cli_history_record_free(&checkpoint);
}

TEST(history_enforces_contiguous_ids_and_owns_records) {
    struct oi_cli_history history;
    struct oi_cli_history_record first;
    struct oi_cli_history_record second;
    oi_cli_history_init(&history);
    oi_cli_history_record_init(&first);
    oi_cli_history_record_init(&second);

    CHECK_EQ(oi_cli_history_record_set_transition(&first, 5, 4), OI_OK);
    CHECK_EQ(oi_cli_history_append_take(&history, &first), OI_OK);
    CHECK_EQ(first.kind, OI_CLI_HISTORY_RECORD_NONE);

    CHECK_EQ(oi_cli_history_record_set_queued_input(&second, 7, 1, "later", 5),
             OI_OK);
    CHECK_EQ(oi_cli_history_append_take(&history, &second), OI_ERR_INVAL);
    CHECK_EQ(second.kind, OI_CLI_HISTORY_RECORD_QUEUED_INPUT);

    CHECK_EQ(oi_cli_history_record_set_queued_input(&second, 6, 1, "next", 4),
             OI_OK);
    CHECK_EQ(oi_cli_history_append_take(&history, &second), OI_OK);
    CHECK_EQ(history.len, 2);
    CHECK_STREQ(history.records[1].as.queued_input.content.data, "next");

    oi_cli_history_record_free(&first);
    oi_cli_history_record_free(&second);
    oi_cli_history_free(&history);
    oi_cli_history_free(&history);
}

TEST(record_clone_is_deep) {
    struct oi_cli_history_record source;
    struct oi_cli_history_record clone;
    oi_cli_history_record_init(&source);
    oi_cli_history_record_init(&clone);

    CHECK_EQ(oi_cli_history_record_set_partial_assistant(
                 &source, 2, 1, "partial", 7, "model", 5),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_clone(&source, &clone), OI_OK);
    CHECK(source.as.partial_assistant.content.data !=
          clone.as.partial_assistant.content.data);
    source.as.partial_assistant.content.data[0] = 'X';
    CHECK_STREQ(clone.as.partial_assistant.content.data, "partial");

    oi_cli_history_record_free(&source);
    oi_cli_history_record_free(&clone);
}

TEST(assistant_attribution_is_required) {
    struct oi_cli_message message;
    struct oi_cli_history_record record;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&record);

    CHECK_EQ(oi_cli_message_set_assistant(&message, "answer", 6), OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 &record, 2, 1, &message, OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                 0, OI_CLI_HISTORY_TOOL_OUTCOME_NONE, NULL, 0, 0),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_history_record_set_message(
                 &record, 2, 1, &message, OI_CLI_HISTORY_MESSAGE_NORMAL,
                 "gpt-test", 8, OI_CLI_HISTORY_TOOL_OUTCOME_NONE, NULL, 0, 0),
             OI_OK);

    oi_cli_message_free(&message);
    oi_cli_history_record_free(&record);
}

int main(void) {
    RUN(normal_tool_result_preserves_raw_bytes);
    RUN(repair_tool_results_have_no_raw_output);
    RUN(partial_assistant_is_audit_only_record);
    RUN(queue_lifecycle_uses_stable_record_reference);
    RUN(checkpoint_requires_prior_nonempty_range);
    RUN(history_enforces_contiguous_ids_and_owns_records);
    RUN(record_clone_is_deep);
    RUN(assistant_attribution_is_required);
    return oi_test_report();
}
