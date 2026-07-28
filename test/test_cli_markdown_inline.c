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

static void parse(const char *text, struct oi_cli_bytebuf *out_text,
                  struct oi_cli_md_span_list *out_spans) {
    oi_cli_bytebuf_init(out_text);
    oi_cli_md_span_list_init(out_spans);
    CHECK_EQ(oi_cli_markdown_inline_parse(text, strlen(text), out_text,
                                          out_spans),
             OI_OK);
    CHECK(oi_cli_md_span_list_covers(out_spans, out_text->len));
}

static void check_text(const struct oi_cli_bytebuf *out, const char *expected) {
    CHECK_EQ(out->len, strlen(expected));
    if (out->len == strlen(expected)) {
        CHECK(memcmp(out->data, expected, out->len) == 0);
    }
}

static void check_span(const struct oi_cli_md_span_list *spans, size_t index,
                       size_t start, size_t len, unsigned style_bits) {
    CHECK(index < spans->len);
    if (index >= spans->len) {
        return;
    }
    CHECK_EQ(spans->runs[index].start, start);
    CHECK_EQ(spans->runs[index].len, len);
    CHECK_EQ(spans->runs[index].style_bits, style_bits);
}

TEST(plain_text_is_a_single_unstyled_span) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    parse("plain text", &out_text, &spans);
    check_text(&out_text, "plain text");
    CHECK_EQ(spans.len, (size_t)1);
    check_span(&spans, 0, 0, 10, 0);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(bold_delimiters_are_stripped_and_styled) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    parse("**bold**", &out_text, &spans);
    check_text(&out_text, "bold");
    CHECK_EQ(spans.len, (size_t)1);
    check_span(&spans, 0, 0, 4, OI_CLI_MD_STYLE_BOLD);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(italic_delimiters_are_stripped_and_styled) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    parse("*italic*", &out_text, &spans);
    check_text(&out_text, "italic");
    CHECK_EQ(spans.len, (size_t)1);
    check_span(&spans, 0, 0, 6, OI_CLI_MD_STYLE_ITALIC);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(code_span_is_stripped_and_styled_without_emphasis_interpretation) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    parse("`**not bold**`", &out_text, &spans);
    check_text(&out_text, "**not bold**");
    CHECK_EQ(spans.len, (size_t)1);
    check_span(&spans, 0, 0, 12, OI_CLI_MD_STYLE_CODE);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(mismatched_backtick_run_length_keeps_scanning_for_a_match) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    /* "``a`b``": a length-2 run, then a stray length-1 backtick (not a
     * match), then the matching length-2 close run. */
    parse("``a`b``", &out_text, &spans);
    check_text(&out_text, "a`b");
    CHECK_EQ(spans.len, (size_t)1);
    check_span(&spans, 0, 0, 3, OI_CLI_MD_STYLE_CODE);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(three_delimiters_combine_bold_and_italic) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    parse("***both***", &out_text, &spans);
    check_text(&out_text, "both");
    CHECK_EQ(spans.len, (size_t)1);
    check_span(&spans, 0, 0, 4,
              OI_CLI_MD_STYLE_BOLD | OI_CLI_MD_STYLE_ITALIC);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(nested_italic_inside_bold_produces_three_flat_spans) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    parse("**a *b* c**", &out_text, &spans);
    check_text(&out_text, "a b c");
    CHECK_EQ(spans.len, (size_t)3);
    check_span(&spans, 0, 0, 2, OI_CLI_MD_STYLE_BOLD); /* "a " */
    check_span(&spans, 1, 2, 1,
              OI_CLI_MD_STYLE_BOLD | OI_CLI_MD_STYLE_ITALIC); /* "b" */
    check_span(&spans, 2, 3, 2, OI_CLI_MD_STYLE_BOLD); /* " c" */

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(intraword_underscores_stay_literal_and_unstyled) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    parse("foo_bar_baz", &out_text, &spans);
    check_text(&out_text, "foo_bar_baz");
    CHECK_EQ(spans.len, (size_t)1);
    check_span(&spans, 0, 0, 11, 0);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(unmatched_opener_stays_literal) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    parse("**bold", &out_text, &spans);
    check_text(&out_text, "**bold");
    CHECK_EQ(spans.len, (size_t)1);
    check_span(&spans, 0, 0, 6, 0);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(multiple_of_three_rule_blocks_incompatible_pairing) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    /* "foo*bar**baz*qux**e": the single '*' after "foo" and the '**' after
     * "bar" are both "can open and close" (intraword-adjacent stars have
     * no intraword restriction) with lengths 1 and 2 -- their sum (3) is a
     * multiple of 3 but the lengths individually are not, so CommonMark's
     * rule forbids that pairing. The single '*' instead matches the later
     * single '*' after "baz", leaving the first '**' permanently enclosed
     * (unmatched, literal) and the final '**' unmatched too. */
    parse("foo*bar**baz*qux**e", &out_text, &spans);
    check_text(&out_text, "foobar**bazqux**e");
    CHECK_EQ(spans.len, (size_t)3);
    check_span(&spans, 0, 0, 3, 0); /* "foo" */
    check_span(&spans, 1, 3, 8, OI_CLI_MD_STYLE_ITALIC); /* "bar**baz" */
    check_span(&spans, 2, 11, 6, 0); /* "qux**e" */

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(empty_input_produces_empty_output) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    oi_cli_bytebuf_init(&out_text);
    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_markdown_inline_parse("", 0, &out_text, &spans), OI_OK);
    CHECK_EQ(out_text.len, (size_t)0);
    CHECK_EQ(spans.len, (size_t)0);
    CHECK(oi_cli_md_span_list_covers(&spans, 0));

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
}

TEST(inline_parse_rejects_bad_arguments) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;

    oi_cli_bytebuf_init(&out_text);
    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_markdown_inline_parse(NULL, 1, &out_text, &spans),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_inline_parse("a", 1, NULL, &spans),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_inline_parse("a", 1, &out_text, NULL),
             OI_ERR_INVAL);

    oi_cli_bytebuf_free(&out_text);
    oi_cli_md_span_list_free(&spans);
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
    RUN(plain_text_is_a_single_unstyled_span);
    RUN(bold_delimiters_are_stripped_and_styled);
    RUN(italic_delimiters_are_stripped_and_styled);
    RUN(code_span_is_stripped_and_styled_without_emphasis_interpretation);
    RUN(mismatched_backtick_run_length_keeps_scanning_for_a_match);
    RUN(three_delimiters_combine_bold_and_italic);
    RUN(nested_italic_inside_bold_produces_three_flat_spans);
    RUN(intraword_underscores_stay_literal_and_unstyled);
    RUN(unmatched_opener_stays_literal);
    RUN(multiple_of_three_rule_blocks_incompatible_pairing);
    RUN(empty_input_produces_empty_output);
    RUN(inline_parse_rejects_bad_arguments);
    return oi_test_report();
}
