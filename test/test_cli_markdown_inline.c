#include "cli_markdown_inline.h"
#include "test.h"

#include <string.h>

static void tokenize(const char *text, struct oi_cli_md_delim_list *out) {
    oi_cli_md_delim_list_init(out);
    CHECK_EQ(oi_cli_markdown_tokenize(text, strlen(text), out), OI_OK);
}

TEST(intraword_underscores_cannot_open_or_close) {
    static const char text[] = "foo_bar_baz";
    struct oi_cli_md_delim_list delims;

    tokenize(text, &delims);
    CHECK_EQ(delims.len, (size_t)2);
    CHECK(!delims.runs[0].can_open);
    CHECK(!delims.runs[0].can_close);
    CHECK(!delims.runs[1].can_open);
    CHECK(!delims.runs[1].can_close);

    oi_cli_md_delim_list_free(&delims);
}

TEST(star_has_no_intraword_restriction) {
    static const char text[] = "foo*bar*baz";
    struct oi_cli_md_delim_list delims;

    tokenize(text, &delims);
    CHECK_EQ(delims.len, (size_t)2);
    CHECK(delims.runs[0].can_open);
    CHECK(delims.runs[0].can_close);
    CHECK(delims.runs[1].can_open);
    CHECK(delims.runs[1].can_close);

    oi_cli_md_delim_list_free(&delims);
}

TEST(plain_emphasis_run_is_open_and_close_eligible) {
    static const char text[] = "*foo bar*";
    struct oi_cli_md_delim_list delims;

    tokenize(text, &delims);
    CHECK_EQ(delims.len, (size_t)2);
    CHECK(delims.runs[0].can_open);
    CHECK(delims.runs[1].can_close);

    oi_cli_md_delim_list_free(&delims);
}

TEST(space_after_opener_cannot_open) {
    static const char text[] = "* foo bar*";
    struct oi_cli_md_delim_list delims;

    tokenize(text, &delims);
    CHECK_EQ(delims.len, (size_t)2);
    CHECK(!delims.runs[0].can_open);

    oi_cli_md_delim_list_free(&delims);
}

TEST(space_before_closer_cannot_close) {
    static const char text[] = "*foo bar *";
    struct oi_cli_md_delim_list delims;

    tokenize(text, &delims);
    CHECK_EQ(delims.len, (size_t)2);
    CHECK(!delims.runs[1].can_close);

    oi_cli_md_delim_list_free(&delims);
}

TEST(runs_longer_than_three_are_always_inert) {
    static const char text[] = "foo ****bar**** baz";
    struct oi_cli_md_delim_list delims;

    tokenize(text, &delims);
    CHECK_EQ(delims.len, (size_t)2);
    CHECK(!delims.runs[0].can_open);
    CHECK(!delims.runs[0].can_close);
    CHECK(!delims.runs[1].can_open);
    CHECK(!delims.runs[1].can_close);

    oi_cli_md_delim_list_free(&delims);
}

TEST(bold_and_italic_run_lengths_are_recorded) {
    static const char text[] = "**bold** *italic* ***both***";
    struct oi_cli_md_delim_list delims;

    tokenize(text, &delims);
    CHECK_EQ(delims.len, (size_t)6);
    CHECK_EQ(delims.runs[0].len, (size_t)2);
    CHECK_EQ(delims.runs[1].len, (size_t)2);
    CHECK_EQ(delims.runs[2].len, (size_t)1);
    CHECK_EQ(delims.runs[3].len, (size_t)1);
    CHECK_EQ(delims.runs[4].len, (size_t)3);
    CHECK_EQ(delims.runs[5].len, (size_t)3);
    CHECK(delims.runs[4].can_open);
    CHECK(delims.runs[5].can_close);

    oi_cli_md_delim_list_free(&delims);
}

TEST(text_with_no_delimiters_produces_an_empty_list) {
    static const char text[] = "plain text, no emphasis here.";
    struct oi_cli_md_delim_list delims;

    tokenize(text, &delims);
    CHECK_EQ(delims.len, (size_t)0);

    oi_cli_md_delim_list_free(&delims);
}

TEST(delim_list_grows_and_is_safe_to_free_twice) {
    struct oi_cli_md_delim_list delims;
    size_t i;

    oi_cli_md_delim_list_init(&delims);
    for (i = 0; i < 20; i++) {
        CHECK_EQ(oi_cli_md_delim_list_append(&delims, i, 1,
                                             OI_CLI_MD_DELIM_STAR, 1, 0),
                 OI_OK);
    }
    CHECK_EQ(delims.len, (size_t)20);
    oi_cli_md_delim_list_free(&delims);
    CHECK_EQ(delims.len, (size_t)0);
    CHECK(delims.runs == NULL);
    oi_cli_md_delim_list_free(&delims);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_md_delim_list delims;

    oi_cli_md_delim_list_init(&delims);
    CHECK_EQ(oi_cli_markdown_tokenize(NULL, 1, &delims), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_tokenize("a", 1, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_tokenize(NULL, 0, &delims), OI_OK);
    oi_cli_md_delim_list_free(&delims);
}

int main(void) {
    RUN(intraword_underscores_cannot_open_or_close);
    RUN(star_has_no_intraword_restriction);
    RUN(plain_emphasis_run_is_open_and_close_eligible);
    RUN(space_after_opener_cannot_open);
    RUN(space_before_closer_cannot_close);
    RUN(runs_longer_than_three_are_always_inert);
    RUN(bold_and_italic_run_lengths_are_recorded);
    RUN(text_with_no_delimiters_produces_an_empty_list);
    RUN(delim_list_grows_and_is_safe_to_free_twice);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
