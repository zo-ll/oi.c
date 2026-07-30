#include "cli_session_metadata.h"
#include "test.h"

#include <string.h>

TEST(valid_metadata_round_trips_fields) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/home/az/project", 16,
                                         NULL, 0, 100, 200),
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
                                         "/tmp", 4, NULL, 0, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "", 0,
                                         "/tmp", 4, NULL, 0, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "", 0, NULL, 0, 1, 1),
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
                                         "/tmp", 4, NULL, 0, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6,
                                         oversized_value,
                                         sizeof oversized_value, "/tmp", 4,
                                         NULL, 0, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, oversized_value,
                                         sizeof oversized_value, NULL, 0,
                                         1, 1),
             OI_ERR_INVAL);

    oi_cli_session_metadata_free(&metadata);
}

TEST(negative_or_backwards_timestamps_are_rejected) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, -1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, 1, -1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, 200, 100),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, 100, 100),
             OI_OK);

    oi_cli_session_metadata_free(&metadata);
}

TEST(set_failure_does_not_disturb_the_prior_valid_value) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, 1, 1),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "", 0,
                                         "/tmp", 4, NULL, 0, 1, 1),
             OI_ERR_INVAL);
    CHECK_STREQ(metadata.model.data, "gpt-test");

    oi_cli_session_metadata_free(&metadata);
}

TEST(free_resets_to_empty_and_is_safe_to_call_twice) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, 1, 1),
             OI_OK);
    oi_cli_session_metadata_free(&metadata);
    CHECK(metadata.session_id.data == NULL);
    CHECK(!oi_cli_session_metadata_is_valid(&metadata));
    oi_cli_session_metadata_free(&metadata);
}

TEST(display_names_are_optional_bounded_and_control_free) {
    struct oi_cli_session_metadata metadata;
    char oversized_name[OI_CLI_SESSION_METADATA_MAX_DISPLAY_NAME + 1];
    char at_limit[OI_CLI_SESSION_METADATA_MAX_DISPLAY_NAME];
    oi_cli_session_metadata_init(&metadata);
    memset(oversized_name, 'a', sizeof oversized_name);
    memset(at_limit, 'a', sizeof at_limit);

    /* Unset is the normal state, expressed either way. */
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, 1, 1),
             OI_OK);
    CHECK_EQ(metadata.display_name.len, (size_t)0);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, "", 0, 1, 1),
             OI_OK);
    CHECK_EQ(metadata.display_name.len, (size_t)0);

    /* A set name round-trips, spaces and punctuation included. */
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, "my refactor #2", 14,
                                         1, 1),
             OI_OK);
    CHECK_STREQ(metadata.display_name.data, "my refactor #2");
    /* Multi-byte UTF-8 is fine: only C0 controls and DEL are refused. */
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, "caf\xc3\xa9", 5, 1,
                                         1),
             OI_OK);
    CHECK_STREQ(metadata.display_name.data, "caf\xc3\xa9");

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, at_limit,
                                         sizeof at_limit, 1, 1),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, oversized_name,
                                         sizeof oversized_name, 1, 1),
             OI_ERR_INVAL);

    /* A name reaches the terminal unescaped, so control bytes are refused
     * here rather than left for every consumer to sanitize. */
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, "two\nlines", 9, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, "esc\x1b[2J", 7, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, "bell\x07", 5, 1, 1),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, "del\x7f", 4, 1, 1),
             OI_ERR_INVAL);

    oi_cli_session_metadata_free(&metadata);
}

TEST(bad_arguments_are_rejected) {
    CHECK_EQ(oi_cli_session_metadata_set(NULL, "sess-1", 6, "gpt-test", 8,
                                         "/tmp", 4, NULL, 0, 1, 1),
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
    RUN(display_names_are_optional_bounded_and_control_free);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
