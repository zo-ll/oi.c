#include "cli_session_metadata.h"
#include "test.h"

#include <string.h>

TEST(valid_metadata_round_trips_fields) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/home/az/project", 16, 100,
                                         200),
             OI_OK);
    CHECK(oi_cli_session_metadata_is_valid(&metadata));
    CHECK_EQ(metadata.version, (unsigned int)OI_CLI_SESSION_METADATA_SCHEMA_VERSION);
    CHECK_STREQ(metadata.session_id.data, "sess-1");
    CHECK_STREQ(metadata.model.data, "gpt-test");
    CHECK_STREQ(metadata.cwd.data, "/home/az/project");
    CHECK_EQ(metadata.created_at, 100);
    CHECK_EQ(metadata.updated_at, 200);

    oi_cli_session_metadata_free(&metadata);
}

TEST(empty_fields_are_rejected) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "", 0, "gpt-test", 8,
                                         "/tmp", 4, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "", 0, "/tmp",
                                         4, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "", 0, 1, 1),
             OI_ERR_INVAL);

    oi_cli_session_metadata_free(&metadata);
}

TEST(oversized_fields_are_rejected) {
    struct oi_cli_session_metadata metadata;
    char oversized_id[OI_CLI_SESSION_METADATA_MAX_SESSION_ID + 1];
    char oversized_value[OI_CLI_HISTORY_MAX_SETTING_VALUE + 1];
    oi_cli_session_metadata_init(&metadata);
    memset(oversized_id, 'a', sizeof oversized_id);
    memset(oversized_value, 'a', sizeof oversized_value);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, oversized_id,
                                         sizeof oversized_id, "gpt-test", 8,
                                         "/tmp", 4, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6,
                                         oversized_value,
                                         sizeof oversized_value, "/tmp", 4, 1,
                                         1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, oversized_value,
                                         sizeof oversized_value, 1, 1),
             OI_ERR_INVAL);

    oi_cli_session_metadata_free(&metadata);
}

TEST(negative_or_backwards_timestamps_are_rejected) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, -1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, 1, -1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, 200, 100),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, 100, 100),
             OI_OK);

    oi_cli_session_metadata_free(&metadata);
}

TEST(set_failure_does_not_disturb_the_prior_valid_value) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, 1, 1),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "", 0, "/tmp",
                                         4, 1, 1),
             OI_ERR_INVAL);
    CHECK_STREQ(metadata.model.data, "gpt-test");

    oi_cli_session_metadata_free(&metadata);
}

TEST(free_resets_to_empty_and_is_safe_to_call_twice) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, 1, 1),
             OI_OK);
    oi_cli_session_metadata_free(&metadata);
    CHECK(metadata.session_id.data == NULL);
    CHECK(!oi_cli_session_metadata_is_valid(&metadata));
    oi_cli_session_metadata_free(&metadata);
}

TEST(bad_arguments_are_rejected) {
    CHECK_EQ(oi_cli_session_metadata_set(NULL, "sess-1", 6, "gpt-test", 8,
                                         "/tmp", 4, 1, 1),
             OI_ERR_INVAL);
    CHECK(!oi_cli_session_metadata_is_valid(NULL));
}

int main(void) {
    RUN(valid_metadata_round_trips_fields);
    RUN(empty_fields_are_rejected);
    RUN(oversized_fields_are_rejected);
    RUN(negative_or_backwards_timestamps_are_rejected);
    RUN(set_failure_does_not_disturb_the_prior_valid_value);
    RUN(free_resets_to_empty_and_is_safe_to_call_twice);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
