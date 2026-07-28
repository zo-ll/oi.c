#include "cli_input_history.h"
#include "test.h"

#include <string.h>

TEST(navigation_walks_newest_first_and_restores_draft) {
    struct oi_cli_input_history history;
    const char *text = NULL;
    size_t len = 0;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_input_history_append(&history, "first", 5), OI_OK);
    CHECK_EQ(oi_cli_input_history_append(&history, "second", 6), OI_OK);

    CHECK_EQ(oi_cli_input_history_previous(&history, "draft", 5, &text, &len),
             OI_OK);
    CHECK_EQ(len, 6);
    CHECK_STREQ(text, "second");
    CHECK_EQ(oi_cli_input_history_previous(&history, "ignored", 7, &text,
                                           &len),
             OI_OK);
    CHECK_STREQ(text, "first");
    CHECK_EQ(oi_cli_input_history_previous(&history, "", 0, &text, &len),
             OI_ERR_NOTFOUND);
    CHECK_EQ(oi_cli_input_history_next(&history, &text, &len), OI_OK);
    CHECK_STREQ(text, "second");
    CHECK_EQ(oi_cli_input_history_next(&history, &text, &len), OI_OK);
    CHECK_EQ(len, 5);
    CHECK_STREQ(text, "draft");
    CHECK_EQ(oi_cli_input_history_next(&history, &text, &len),
             OI_ERR_NOTFOUND);

    oi_cli_input_history_free(&history);
}

TEST(empty_draft_is_restored) {
    struct oi_cli_input_history history;
    const char *text = NULL;
    size_t len = 99;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_input_history_append(&history, "entry", 5), OI_OK);
    CHECK_EQ(oi_cli_input_history_previous(&history, NULL, 0, &text, &len),
             OI_OK);
    CHECK_STREQ(text, "entry");
    CHECK_EQ(oi_cli_input_history_next(&history, &text, &len), OI_OK);
    CHECK_EQ(len, 0);
    CHECK_STREQ(text, "");

    oi_cli_input_history_free(&history);
}

TEST(consecutive_duplicates_are_collapsed) {
    struct oi_cli_input_history history;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_input_history_append(&history, "same", 4), OI_OK);
    CHECK_EQ(oi_cli_input_history_append(&history, "same", 4), OI_OK);
    CHECK_EQ(history.len, 1);
    CHECK_EQ(history.bytes, 4);

    oi_cli_input_history_free(&history);
}

TEST(append_resets_navigation) {
    struct oi_cli_input_history history;
    const char *text = NULL;
    size_t len = 0;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_input_history_append(&history, "one", 3), OI_OK);
    CHECK_EQ(oi_cli_input_history_previous(&history, "draft", 5, &text, &len),
             OI_OK);
    CHECK_EQ(oi_cli_input_history_append(&history, "two", 3), OI_OK);
    CHECK_EQ(oi_cli_input_history_next(&history, &text, &len),
             OI_ERR_NOTFOUND);
    CHECK_EQ(oi_cli_input_history_previous(&history, "new", 3, &text, &len),
             OI_OK);
    CHECK_STREQ(text, "two");

    oi_cli_input_history_free(&history);
}

TEST(old_entries_are_bounded_and_evicted) {
    struct oi_cli_input_history history;
    char text[32];
    size_t i;

    oi_cli_input_history_init(&history);
    for (i = 0; i < OI_CLI_INPUT_HISTORY_MAX_ENTRIES + 10U; i++) {
        int written = snprintf(text, sizeof text, "entry-%zu", i);
        CHECK(written > 0);
        CHECK_EQ(oi_cli_input_history_append(&history, text, (size_t)written),
                 OI_OK);
    }
    CHECK_EQ(history.len, OI_CLI_INPUT_HISTORY_MAX_ENTRIES);
    CHECK_STREQ(history.entries[0].data, "entry-10");
    CHECK_STREQ(history.entries[history.len - 1].data, "entry-265");

    oi_cli_input_history_free(&history);
}

TEST(invalid_text_and_arguments_are_rejected) {
    static const char malformed[] = {(char)0xc0, (char)0x80};
    static const char nul[] = {'a', '\0', 'b'};
    struct oi_cli_input_history history;
    const char *text = NULL;
    size_t len = 0;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_input_history_append(&history, "", 0), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_input_history_append(&history, NULL, 1), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_input_history_append(&history, malformed,
                                         sizeof malformed),
             OI_ERR_PARSE);
    CHECK_EQ(oi_cli_input_history_append(&history, nul, sizeof nul),
             OI_ERR_PARSE);
    CHECK_EQ(oi_cli_input_history_previous(&history, "", 0, &text, &len),
             OI_ERR_NOTFOUND);
    CHECK_EQ(oi_cli_input_history_next(&history, &text, &len),
             OI_ERR_NOTFOUND);
    CHECK_EQ(oi_cli_input_history_next(NULL, &text, &len), OI_ERR_INVAL);

    oi_cli_input_history_free(&history);
    oi_cli_input_history_free(&history);
}

int main(void) {
    RUN(navigation_walks_newest_first_and_restores_draft);
    RUN(empty_draft_is_restored);
    RUN(consecutive_duplicates_are_collapsed);
    RUN(append_resets_navigation);
    RUN(old_entries_are_bounded_and_evicted);
    RUN(invalid_text_and_arguments_are_rejected);
    return oi_test_report();
}
