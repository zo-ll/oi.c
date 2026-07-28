#include "cli_markdown_block.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static char *read_stream(FILE *stream) {
    long len;
    char *text;

    CHECK_EQ(fflush(stream), 0);
    CHECK_EQ(fseek(stream, 0, SEEK_END), 0);
    len = ftell(stream);
    CHECK(len >= 0);
    CHECK_EQ(fseek(stream, 0, SEEK_SET), 0);
    text = malloc((size_t)len + 1);
    CHECK(text != NULL);
    if (text == NULL) {
        return NULL;
    }
    CHECK_EQ(fread(text, 1, (size_t)len, stream), (size_t)len);
    text[len] = '\0';
    return text;
}

/* Feeds the whole input in one call, then finish(); returns the rendered
 * output as a malloc'd NUL-terminated string. */
static char *render_whole(const char *input, size_t len) {
    struct oi_cli_markdown_block block;
    FILE *out = tmpfile();
    int style_active = 0;
    char *result;

    oi_cli_markdown_block_init(&block);
    CHECK_EQ(oi_cli_markdown_block_feed(&block, input, len, out,
                                        &style_active),
             OI_OK);
    CHECK_EQ(oi_cli_markdown_block_finish(&block, out, &style_active), OI_OK);
    result = read_stream(out);
    oi_cli_markdown_block_free(&block);
    fclose(out);
    return result;
}

/* Verifies every possible two-call chunk split of `input` produces the
 * exact same rendered output as feeding it whole. */
static void check_split_invariant(const char *input, size_t len) {
    size_t split;

    for (split = 1; split < len; split++) {
        struct oi_cli_markdown_block block;
        FILE *out = tmpfile();
        int style_active = 0;
        char *whole;
        char *result;

        oi_cli_markdown_block_init(&block);
        CHECK_EQ(oi_cli_markdown_block_feed(&block, input, split, out,
                                            &style_active),
                 OI_OK);
        CHECK_EQ(oi_cli_markdown_block_feed(&block, input + split,
                                            len - split, out, &style_active),
                 OI_OK);
        CHECK_EQ(oi_cli_markdown_block_finish(&block, out, &style_active),
                 OI_OK);
        result = read_stream(out);
        whole = render_whole(input, len);
        CHECK_STREQ(result, whole);

        free(result);
        free(whole);
        oi_cli_markdown_block_free(&block);
        fclose(out);
    }
}

TEST(plain_line_renders_unstyled) {
    static const char in[] = "hello\n";
    char *out = render_whole(in, sizeof in - 1);

    CHECK_STREQ(out, "hello\n");
    free(out);
}

TEST(heading_levels_require_trailing_space) {
    char *h1 = render_whole("# Title\n", 8);
    char *deep = render_whole("###### Deep\n", 12);
    char *no_space = render_whole("#NoSpace\n", 9);

    CHECK_STREQ(h1, "\x1b[0m\x1b[1;4mTitle\n");
    CHECK_STREQ(deep, "\x1b[0m\x1b[1mDeep\n");
    CHECK_STREQ(no_space, "#NoSpace\n");

    free(h1);
    free(deep);
    free(no_space);
}

TEST(bare_heading_with_no_title_renders_just_a_newline) {
    char *out = render_whole("##\n", 3);

    CHECK_STREQ(out, "\n");
    free(out);
}

TEST(list_markers_render_dim_marker_then_content) {
    char *unordered = render_whole("- item\n", 7);
    char *ordered = render_whole("1. item\n", 8);

    CHECK_STREQ(unordered, "\x1b[0m\x1b[2m- \x1b[0mitem\n");
    CHECK_STREQ(ordered, "\x1b[0m\x1b[2m1. \x1b[0mitem\n");

    free(unordered);
    free(ordered);
}

TEST(fenced_code_block_hides_markers_and_dims_content) {
    static const char in[] = "```\ncode line\n```\n";
    char *out = render_whole(in, sizeof in - 1);

    CHECK_STREQ(out, "\x1b[0m\x1b[2mcode line\n");
    free(out);
}

TEST(fence_content_markdown_syntax_is_not_interpreted) {
    static const char in[] = "```\n**not bold**\n```\n";
    char *out = render_whole(in, sizeof in - 1);

    CHECK_STREQ(out, "\x1b[0m\x1b[2m**not bold**\n");
    free(out);
}

TEST(tilde_fence_also_works) {
    static const char in[] = "~~~\ncode\n~~~\n";
    char *out = render_whole(in, sizeof in - 1);

    CHECK_STREQ(out, "\x1b[0m\x1b[2mcode\n");
    free(out);
}

TEST(short_closing_run_does_not_close_the_fence) {
    static const char in[] = "```\ncode\n``\nmore\n```\n";
    char *out = render_whole(in, sizeof in - 1);

    CHECK_STREQ(out,
               "\x1b[0m\x1b[2mcode\n\x1b[0m\x1b[2m``\n\x1b[0m\x1b[2mmore\n");
    free(out);
}

TEST(blank_line_separates_paragraphs) {
    static const char in[] = "one\n\ntwo\n";
    char *out = render_whole(in, sizeof in - 1);

    CHECK_STREQ(out, "one\n\ntwo\n");
    free(out);
}

TEST(cap_overflow_falls_back_to_literal_and_recovers) {
    size_t filler_len = OI_CLI_MARKDOWN_LINE_CAP + 100;
    size_t total_len = filler_len + 1 /* '\n' */ + 5 /* "safe\n" */;
    char *in = malloc(total_len);
    char *out;

    CHECK(in != NULL);
    if (in == NULL) {
        return;
    }
    memset(in, 'A', filler_len);
    in[filler_len] = '\n';
    memcpy(in + filler_len + 1, "safe\n", 5);

    out = render_whole(in, total_len);
    /* The overlong line is emitted literally (unstyled), and "safe" on
     * the next line renders as its own ordinary unstyled paragraph. */
    CHECK(out != NULL);
    if (out != NULL) {
        size_t out_len = strlen(out);
        CHECK(out_len == filler_len + 1 + 5);
        CHECK(memcmp(out + out_len - 5, "safe\n", 5) == 0);
    }

    free(in);
    free(out);
}

TEST(finish_flushes_an_unterminated_final_line) {
    struct oi_cli_markdown_block block;
    FILE *out = tmpfile();
    int style_active = 0;
    char *result;
    static const char in[] = "partial";

    oi_cli_markdown_block_init(&block);
    CHECK_EQ(oi_cli_markdown_block_feed(&block, in, sizeof in - 1, out,
                                        &style_active),
             OI_OK);
    CHECK_EQ(oi_cli_markdown_block_finish(&block, out, &style_active), OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "partial\n");
    /* finish() is idempotent: nothing left to flush the second time. */
    CHECK_EQ(oi_cli_markdown_block_finish(&block, out, &style_active), OI_OK);
    free(result);
    result = read_stream(out);
    CHECK_STREQ(result, "partial\n");

    free(result);
    oi_cli_markdown_block_free(&block);
    fclose(out);
}

TEST(bold_line_is_chunk_boundary_safe) {
    static const char in[] = "**bold**\n";

    check_split_invariant(in, sizeof in - 1);
}

TEST(heading_line_is_chunk_boundary_safe) {
    static const char in[] = "# Title\n";

    check_split_invariant(in, sizeof in - 1);
}

TEST(fence_markers_are_chunk_boundary_safe) {
    static const char in[] = "```\ncode\n```\n";

    check_split_invariant(in, sizeof in - 1);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_markdown_block block;
    FILE *out = tmpfile();
    int style_active = 0;
    char byte = 'a';

    oi_cli_markdown_block_init(&block);
    CHECK_EQ(oi_cli_markdown_block_feed(NULL, &byte, 1, out, &style_active),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_block_feed(&block, NULL, 1, out, &style_active),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_block_feed(&block, &byte, 1, NULL,
                                        &style_active),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_block_feed(&block, &byte, 1, out, NULL),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_block_finish(NULL, out, &style_active),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_block_finish(&block, NULL, &style_active),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_markdown_block_finish(&block, out, NULL), OI_ERR_INVAL);

    oi_cli_markdown_block_free(&block);
    fclose(out);
}

int main(void) {
    RUN(plain_line_renders_unstyled);
    RUN(heading_levels_require_trailing_space);
    RUN(bare_heading_with_no_title_renders_just_a_newline);
    RUN(list_markers_render_dim_marker_then_content);
    RUN(fenced_code_block_hides_markers_and_dims_content);
    RUN(fence_content_markdown_syntax_is_not_interpreted);
    RUN(tilde_fence_also_works);
    RUN(short_closing_run_does_not_close_the_fence);
    RUN(blank_line_separates_paragraphs);
    RUN(cap_overflow_falls_back_to_literal_and_recovers);
    RUN(finish_flushes_an_unterminated_final_line);
    RUN(bold_line_is_chunk_boundary_safe);
    RUN(heading_line_is_chunk_boundary_safe);
    RUN(fence_markers_are_chunk_boundary_safe);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
