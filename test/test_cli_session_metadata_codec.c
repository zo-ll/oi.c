#include "cli_session_metadata_codec.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

TEST(encoding_is_stable) {
    struct oi_cli_session_metadata metadata;
    char *json = NULL;
    size_t json_len = 0;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/home/az/project", 16, 100,
                                         200),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_encode(&metadata, &json, &json_len),
             OI_OK);
    CHECK_STREQ(json,
               "{\"version\":1,\"session_id\":\"sess-1\","
               "\"model\":\"gpt-test\",\"cwd\":\"/home/az/project\","
               "\"created_at\":\"100\",\"updated_at\":\"200\"}");

    free(json);
    oi_cli_session_metadata_free(&metadata);
}

TEST(encode_rejects_invalid_metadata) {
    struct oi_cli_session_metadata metadata;
    char *json = (char *)(uintptr_t)1;
    size_t json_len = 99;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_encode(&metadata, &json, &json_len),
             OI_ERR_INVAL);
    CHECK_EQ(json, (char *)(uintptr_t)1);
    CHECK_EQ(json_len, (size_t)99);
}

TEST(records_round_trip) {
    struct oi_cli_session_metadata source;
    struct oi_cli_session_metadata decoded;
    char *json = NULL;
    size_t json_len = 0;
    oi_cli_session_metadata_init(&source);
    oi_cli_session_metadata_init(&decoded);

    CHECK_EQ(oi_cli_session_metadata_set(&source, "sess-1", 6, "gpt-test", 8,
                                         "/home/az/project", 16, 100, 200),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_encode(&source, &json, &json_len),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_decode(json, json_len, &decoded), OI_OK);
    CHECK_STREQ(decoded.session_id.data, "sess-1");
    CHECK_STREQ(decoded.model.data, "gpt-test");
    CHECK_STREQ(decoded.cwd.data, "/home/az/project");
    CHECK_EQ(decoded.created_at, 100);
    CHECK_EQ(decoded.updated_at, 200);

    free(json);
    oi_cli_session_metadata_free(&source);
    oi_cli_session_metadata_free(&decoded);
}

static void check_decode_fails(const char *json) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sentinel", 8, "model", 5,
                                         "/tmp", 4, 1, 1),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_decode(json, strlen(json), &metadata),
             OI_ERR_PARSE);
    CHECK_STREQ(metadata.session_id.data, "sentinel");
    oi_cli_session_metadata_free(&metadata);
}

TEST(strict_decoder_rejects_malformed_metadata) {
    check_decode_fails("not json");
    check_decode_fails("[]");
    check_decode_fails(
        "{\"version\":2,\"session_id\":\"s\",\"model\":\"m\",\"cwd\":\"c\","
        "\"created_at\":\"1\",\"updated_at\":\"1\"}");
    check_decode_fails(
        "{\"version\":1,\"session_id\":\"\",\"model\":\"m\",\"cwd\":\"c\","
        "\"created_at\":\"1\",\"updated_at\":\"1\"}");
    check_decode_fails(
        "{\"version\":1,\"session_id\":\"s\",\"model\":\"m\",\"cwd\":\"c\","
        "\"created_at\":\"-1\",\"updated_at\":\"1\"}");
    check_decode_fails(
        "{\"version\":1,\"session_id\":\"s\",\"model\":\"m\",\"cwd\":\"c\","
        "\"created_at\":\"01\",\"updated_at\":\"1\"}");
    check_decode_fails(
        "{\"version\":1,\"session_id\":\"s\",\"model\":\"m\",\"cwd\":\"c\","
        "\"created_at\":\"1\",\"updated_at\":\"1\",\"extra\":true}");
    check_decode_fails(
        "{\"version\":1,\"session_id\":\"s\",\"model\":\"m\","
        "\"created_at\":\"1\",\"updated_at\":\"1\"}");
}

TEST(oversized_input_is_rejected_without_reading) {
    char oversized[OI_CLI_SESSION_METADATA_MAX_FILE + 1];
    struct oi_cli_session_metadata metadata;
    memset(oversized, ' ', sizeof oversized);
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_decode(oversized, sizeof oversized,
                                            &metadata),
             OI_ERR_INVAL);

    oi_cli_session_metadata_free(&metadata);
}

int main(void) {
    RUN(encoding_is_stable);
    RUN(encode_rejects_invalid_metadata);
    RUN(records_round_trip);
    RUN(strict_decoder_rejects_malformed_metadata);
    RUN(oversized_input_is_rejected_without_reading);
    return oi_test_report();
}
