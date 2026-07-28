#include "cli_history_codec.h"
#include "test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

TEST(user_message_encoding_is_stable) {
    struct oi_cli_message message;
    struct oi_cli_history_record record;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&record);
    char *json = NULL;
    size_t json_len = 0;

    CHECK_EQ(oi_cli_message_set_user(&message, "hello", 5), OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 &record, UINT64_C(18446744073709551615), 42, &message,
                 OI_CLI_HISTORY_MESSAGE_NORMAL, NULL, 0,
                 OI_CLI_HISTORY_TOOL_OUTCOME_NONE, NULL, 0, 0),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_encode(&record, &json, &json_len), OI_OK);
    CHECK_STREQ(
        json,
        "{\"version\":1,\"record_id\":\"18446744073709551615\","
        "\"turn_id\":\"42\",\"type\":\"message\",\"role\":\"user\","
        "\"content\":\"hello\",\"source\":\"normal\"}");
    CHECK_EQ(json_len, strlen(json));

    free(json);
    oi_cli_message_free(&message);
    oi_cli_history_record_free(&record);
}

TEST(assistant_tool_calls_encode_as_owned_values) {
    struct oi_cli_message message;
    struct oi_cli_history_record record;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&record);
    char *json = NULL;
    size_t json_len = 0;

    CHECK_EQ(oi_cli_message_set_assistant(&message, "", 0), OI_OK);
    CHECK_EQ(oi_cli_message_add_tool_call(&message, "call-1", 6, "shell", 5,
                                          "{\"command\":\"true\"}", 18),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 &record, 2, 1, &message, OI_CLI_HISTORY_MESSAGE_NORMAL,
                 "gpt-test", 8, OI_CLI_HISTORY_TOOL_OUTCOME_NONE, NULL, 0, 0),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_encode(&record, &json, &json_len), OI_OK);
    CHECK_STREQ(
        json,
        "{\"version\":1,\"record_id\":\"2\",\"turn_id\":\"1\","
        "\"type\":\"message\",\"role\":\"assistant\",\"content\":\"\","
        "\"source\":\"normal\",\"model\":\"gpt-test\",\"tool_calls\":[{"
        "\"id\":\"call-1\",\"name\":\"shell\","
        "\"arguments\":\"{\\\"command\\\":\\\"true\\\"}\"}]}");

    free(json);
    oi_cli_message_free(&message);
    oi_cli_history_record_free(&record);
}

TEST(raw_tool_bytes_are_base64_encoded) {
    static const unsigned char raw[] = {0x00, 0xff, 0x10, 0x20};
    struct oi_cli_message message;
    struct oi_cli_history_record record;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&record);
    char *json = NULL;
    size_t json_len = 0;

    CHECK_EQ(oi_cli_message_set_tool(&message, "call-1", 6, "text", 4),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 &record, 4, 1, &message, OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                 0, OI_CLI_HISTORY_TOOL_COMPLETED, raw, sizeof raw, 1),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_encode(&record, &json, &json_len), OI_OK);
    CHECK(strstr(json, "\"raw_output_base64\":\"AP8QIA==\"") != NULL);
    CHECK(strstr(json, "\"tool_outcome\":\"completed\"") != NULL);

    free(json);
    oi_cli_message_free(&message);
    oi_cli_history_record_free(&record);
}

TEST(audit_and_checkpoint_records_encode_stable_ids) {
    struct oi_cli_history_record record;
    oi_cli_history_record_init(&record);
    char *json = NULL;
    size_t json_len = 0;

    CHECK_EQ(oi_cli_history_record_set_queue_resolved(
                 &record, 9, 3, 8, OI_CLI_HISTORY_QUEUE_DISCARDED),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_encode(&record, &json, &json_len), OI_OK);
    CHECK_STREQ(
        json,
        "{\"version\":1,\"record_id\":\"9\",\"turn_id\":\"3\","
        "\"type\":\"queue_resolved\",\"queued_record_id\":\"8\","
        "\"resolution\":\"discarded\"}");
    free(json);
    json = NULL;

    CHECK_EQ(oi_cli_history_record_set_checkpoint(
                 &record, 12, "summary", 7, "gpt-test", 8, 2, 9),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_encode(&record, &json, &json_len), OI_OK);
    CHECK(strstr(json, "\"source_first_record_id\":\"2\"") != NULL);
    CHECK(strstr(json, "\"source_last_record_id\":\"9\"") != NULL);

    free(json);
    oi_cli_history_record_free(&record);
}

TEST(invalid_arguments_do_not_publish_output) {
    struct oi_cli_history_record record;
    oi_cli_history_record_init(&record);
    char *json = (char *)(uintptr_t)1;
    size_t json_len = 99;

    CHECK_EQ(oi_cli_history_record_encode(&record, &json, &json_len),
             OI_ERR_INVAL);
    CHECK_EQ(json, (char *)(uintptr_t)1);
    CHECK_EQ(json_len, 99);

    oi_cli_history_record_free(&record);
}

static void check_decode_fails(const char *json) {
    struct oi_cli_history_record record;
    oi_cli_history_record_init(&record);
    CHECK_EQ(oi_cli_history_record_set_transition(&record, 1, 0), OI_OK);
    CHECK_EQ(oi_cli_history_record_decode(json, strlen(json), &record),
             OI_ERR_PARSE);
    CHECK_EQ(record.kind, OI_CLI_HISTORY_RECORD_TRANSITION);
    oi_cli_history_record_free(&record);
}

TEST(records_round_trip) {
    static const unsigned char raw[] = {0x00, 0xff, 0x10, 0x20};
    struct oi_cli_message message;
    struct oi_cli_history_record source;
    struct oi_cli_history_record decoded;
    oi_cli_message_init(&message);
    oi_cli_history_record_init(&source);
    oi_cli_history_record_init(&decoded);
    char *json = NULL;
    size_t json_len = 0;

    CHECK_EQ(oi_cli_message_set_tool(&message, "call-1", 6, "text", 4),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 &source, 4, 1, &message, OI_CLI_HISTORY_MESSAGE_NORMAL, NULL,
                 0, OI_CLI_HISTORY_TOOL_COMPLETED, raw, sizeof raw, 1),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_encode(&source, &json, &json_len), OI_OK);
    CHECK_EQ(oi_cli_history_record_decode(json, json_len, &decoded), OI_OK);
    CHECK_EQ(decoded.record_id, 4);
    CHECK_EQ(decoded.turn_id, 1);
    CHECK_EQ(decoded.as.message.tool_outcome,
             OI_CLI_HISTORY_TOOL_COMPLETED);
    CHECK_EQ(decoded.as.message.raw_tool_output.len, sizeof raw);
    CHECK(memcmp(decoded.as.message.raw_tool_output.data, raw, sizeof raw) ==
          0);

    free(json);
    oi_cli_message_free(&message);
    oi_cli_history_record_free(&source);
    oi_cli_history_record_free(&decoded);
}

TEST(all_non_message_record_types_round_trip) {
    struct oi_cli_history_record source;
    struct oi_cli_history_record decoded;
    oi_cli_history_record_init(&source);
    oi_cli_history_record_init(&decoded);
    char *json = NULL;
    size_t json_len = 0;

#define ROUND_TRIP()                                                          \
    do {                                                                      \
        CHECK_EQ(oi_cli_history_record_encode(&source, &json, &json_len),     \
                 OI_OK);                                                      \
        CHECK_EQ(oi_cli_history_record_decode(json, json_len, &decoded),      \
                 OI_OK);                                                      \
        CHECK_EQ(decoded.kind, source.kind);                                  \
        free(json);                                                           \
        json = NULL;                                                          \
        oi_cli_history_record_free(&decoded);                                 \
    } while (0)

    CHECK_EQ(oi_cli_history_record_set_transition(&source, 5, 4), OI_OK);
    ROUND_TRIP();
    CHECK_EQ(oi_cli_history_record_set_tool_started(&source, 6, 2, "call", 4),
             OI_OK);
    ROUND_TRIP();
    CHECK_EQ(oi_cli_history_record_set_partial_assistant(
                 &source, 7, 2, "part", 4, "model", 5),
             OI_OK);
    ROUND_TRIP();
    CHECK_EQ(oi_cli_history_record_set_queued_input(&source, 8, 3, "next", 4),
             OI_OK);
    ROUND_TRIP();
    CHECK_EQ(oi_cli_history_record_set_queue_resolved(
                 &source, 9, 3, 8, OI_CLI_HISTORY_QUEUE_CONSUMED),
             OI_OK);
    ROUND_TRIP();
    CHECK_EQ(oi_cli_history_record_set_checkpoint(
                 &source, 10, "summary", 7, "model", 5, 2, 8),
             OI_OK);
    ROUND_TRIP();

#undef ROUND_TRIP
    oi_cli_history_record_free(&source);
    oi_cli_history_record_free(&decoded);
}

TEST(strict_decoder_rejects_schema_and_id_errors) {
    check_decode_fails(
        "{\"version\":2,\"record_id\":\"1\",\"turn_id\":\"0\","
        "\"type\":\"transition\",\"legacy_record_count\":\"0\"}");
    check_decode_fails(
        "{\"version\":1,\"record_id\":\"01\",\"turn_id\":\"0\","
        "\"type\":\"transition\",\"legacy_record_count\":\"0\"}");
    check_decode_fails(
        "{\"version\":1,\"record_id\":\"18446744073709551616\","
        "\"turn_id\":\"0\",\"type\":\"transition\","
        "\"legacy_record_count\":\"0\"}");
    check_decode_fails(
        "{\"version\":1,\"record_id\":\"1\",\"turn_id\":\"0\","
        "\"type\":\"future\",\"value\":\"x\"}");
}

TEST(strict_decoder_rejects_field_and_base64_errors) {
    check_decode_fails(
        "{\"version\":1,\"record_id\":\"1\",\"turn_id\":\"0\","
        "\"type\":\"transition\",\"legacy_record_count\":\"0\","
        "\"unknown\":true}");
    check_decode_fails(
        "{\"version\":1,\"version\":1,\"record_id\":\"1\","
        "\"turn_id\":\"0\",\"type\":\"transition\","
        "\"legacy_record_count\":\"0\"}");
    check_decode_fails(
        "{\"version\":1,\"record_id\":\"3\",\"turn_id\":\"1\","
        "\"type\":\"message\",\"role\":\"tool\",\"content\":\"x\","
        "\"source\":\"normal\",\"tool_call_id\":\"call\","
        "\"tool_outcome\":\"completed\",\"raw_output_base64\":\"AB==\"}");
    check_decode_fails(
        "{\"version\":1,\"record_id\":\"3\",\"turn_id\":\"1\","
        "\"type\":\"message\",\"role\":\"tool\",\"content\":\"x\","
        "\"source\":\"normal\",\"tool_call_id\":\"call\","
        "\"tool_outcome\":\"completed\",\"raw_output_base64\":\"A===\"}");
}

int main(void) {
    RUN(user_message_encoding_is_stable);
    RUN(assistant_tool_calls_encode_as_owned_values);
    RUN(raw_tool_bytes_are_base64_encoded);
    RUN(audit_and_checkpoint_records_encode_stable_ids);
    RUN(invalid_arguments_do_not_publish_output);
    RUN(records_round_trip);
    RUN(all_non_message_record_types_round_trip);
    RUN(strict_decoder_rejects_schema_and_id_errors);
    RUN(strict_decoder_rejects_field_and_base64_errors);
    return oi_test_report();
}
