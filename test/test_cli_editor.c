#include "cli_editor.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

TEST(empty_editor_is_safe_and_idempotent) {
    struct oi_cli_editor editor;

    oi_cli_editor_init(&editor);
    CHECK_STREQ(oi_cli_editor_data(&editor), "");
    CHECK_EQ(oi_cli_editor_length(&editor), 0);
    CHECK_EQ(oi_cli_editor_cursor(&editor), 0);
    CHECK_EQ(oi_cli_editor_move_left(&editor), OI_OK);
    CHECK_EQ(oi_cli_editor_move_right(&editor), OI_OK);
    CHECK_EQ(oi_cli_editor_backspace(&editor), OI_OK);
    CHECK_EQ(oi_cli_editor_delete(&editor), OI_OK);

    oi_cli_editor_clear(&editor);
    oi_cli_editor_free(&editor);
    oi_cli_editor_free(&editor);
}

TEST(insert_works_at_code_point_boundaries) {
    static const char cent[] = {(char)0xc2, (char)0xa2};
    struct oi_cli_editor editor;

    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_insert(&editor, "AB", 2), OI_OK);
    CHECK_EQ(oi_cli_editor_move_left(&editor), OI_OK);
    CHECK_EQ(oi_cli_editor_insert(&editor, cent, sizeof cent), OI_OK);
    CHECK_EQ(oi_cli_editor_length(&editor), 4);
    CHECK_EQ(oi_cli_editor_cursor(&editor), 3);
    CHECK(memcmp(oi_cli_editor_data(&editor), "A\xc2\xa2"
                                               "B",
                 4) == 0);

    CHECK_EQ(oi_cli_editor_move_left(&editor), OI_OK);
    CHECK_EQ(oi_cli_editor_cursor(&editor), 1);
    CHECK_EQ(oi_cli_editor_move_right(&editor), OI_OK);
    CHECK_EQ(oi_cli_editor_cursor(&editor), 3);
    oi_cli_editor_free(&editor);
}

TEST(backspace_and_delete_remove_whole_code_points) {
    static const char text[] = {'A', (char)0xe2, (char)0x82,
                                (char)0xac, 'B'};
    struct oi_cli_editor editor;

    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, text, sizeof text), OI_OK);
    CHECK_EQ(oi_cli_editor_move_left(&editor), OI_OK);
    CHECK_EQ(oi_cli_editor_backspace(&editor), OI_OK);
    CHECK_STREQ(oi_cli_editor_data(&editor), "AB");
    CHECK_EQ(oi_cli_editor_cursor(&editor), 1);
    CHECK_EQ(oi_cli_editor_delete(&editor), OI_OK);
    CHECK_STREQ(oi_cli_editor_data(&editor), "A");
    CHECK_EQ(oi_cli_editor_length(&editor), 1);

    oi_cli_editor_free(&editor);
}

TEST(line_navigation_stays_within_current_line) {
    struct oi_cli_editor editor;

    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "one\ntwo\nthree", 13), OI_OK);
    oi_cli_editor_move_line_start(&editor);
    CHECK_EQ(oi_cli_editor_cursor(&editor), 8);
    CHECK_EQ(oi_cli_editor_move_left(&editor), OI_OK);
    CHECK_EQ(oi_cli_editor_move_left(&editor), OI_OK);
    oi_cli_editor_move_line_start(&editor);
    CHECK_EQ(oi_cli_editor_cursor(&editor), 4);
    oi_cli_editor_move_line_end(&editor);
    CHECK_EQ(oi_cli_editor_cursor(&editor), 7);

    oi_cli_editor_free(&editor);
}

TEST(clear_retains_a_reusable_buffer) {
    struct oi_cli_editor editor;
    char *allocated;

    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "draft", 5), OI_OK);
    allocated = editor.data;
    oi_cli_editor_clear(&editor);
    CHECK_EQ(editor.data, allocated);
    CHECK_STREQ(oi_cli_editor_data(&editor), "");
    CHECK_EQ(oi_cli_editor_insert(&editor, "next", 4), OI_OK);
    CHECK_EQ(editor.data, allocated);
    CHECK_STREQ(oi_cli_editor_data(&editor), "next");

    oi_cli_editor_free(&editor);
}

TEST(malformed_text_and_nul_are_rejected_atomically) {
    static const char malformed[] = {(char)0xc0, (char)0x80};
    static const char nul[] = {'x', '\0', 'y'};
    struct oi_cli_editor editor;

    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, "kept", 4), OI_OK);
    CHECK_EQ(oi_cli_editor_set(&editor, malformed, sizeof malformed),
             OI_ERR_PARSE);
    CHECK_STREQ(oi_cli_editor_data(&editor), "kept");
    CHECK_EQ(oi_cli_editor_insert(&editor, nul, sizeof nul), OI_ERR_PARSE);
    CHECK_STREQ(oi_cli_editor_data(&editor), "kept");
    CHECK_EQ(oi_cli_editor_set(&editor, NULL, 1), OI_ERR_INVAL);
    CHECK_STREQ(oi_cli_editor_data(&editor), "kept");

    oi_cli_editor_free(&editor);
}

TEST(input_limit_is_enforced) {
    struct oi_cli_editor editor;
    char *text = malloc(OI_CLI_EDITOR_MAX_BYTES + 1U);

    CHECK(text != NULL);
    if (text == NULL) {
        return;
    }
    memset(text, 'x', OI_CLI_EDITOR_MAX_BYTES + 1U);
    oi_cli_editor_init(&editor);
    CHECK_EQ(oi_cli_editor_set(&editor, text, OI_CLI_EDITOR_MAX_BYTES), OI_OK);
    CHECK_EQ(oi_cli_editor_insert(&editor, "x", 1), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_editor_length(&editor), OI_CLI_EDITOR_MAX_BYTES);
    CHECK_EQ(oi_cli_editor_set(&editor, text, OI_CLI_EDITOR_MAX_BYTES + 1U),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_editor_length(&editor), OI_CLI_EDITOR_MAX_BYTES);

    free(text);
    oi_cli_editor_free(&editor);
}

int main(void) {
    RUN(empty_editor_is_safe_and_idempotent);
    RUN(insert_works_at_code_point_boundaries);
    RUN(backspace_and_delete_remove_whole_code_points);
    RUN(line_navigation_stays_within_current_line);
    RUN(clear_retains_a_reusable_buffer);
    RUN(malformed_text_and_nul_are_rejected_atomically);
    RUN(input_limit_is_enforced);
    return oi_test_report();
}
