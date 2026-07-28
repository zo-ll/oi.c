#include "cli_render_stream.h"
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

static char *run_whole(const char *data, size_t len, int styling_enabled) {
    struct oi_cli_render_stream stream;
    FILE *out = tmpfile();
    char *result;

    CHECK_EQ(oi_cli_render_stream_init(&stream, out, styling_enabled), OI_OK);
    CHECK_EQ(oi_cli_render_stream_feed(&stream, data, len), OI_OK);
    CHECK_EQ(oi_cli_render_stream_finish(&stream), OI_OK);
    result = read_stream(out);
    oi_cli_render_stream_free(&stream);
    fclose(out);
    return result;
}

static void check_split_invariant(const char *data, size_t len,
                                  int styling_enabled) {
    size_t split;
    char *whole = run_whole(data, len, styling_enabled);

    for (split = 1; split < len; split++) {
        struct oi_cli_render_stream stream;
        FILE *out = tmpfile();
        char *result;

        CHECK_EQ(oi_cli_render_stream_init(&stream, out, styling_enabled),
                 OI_OK);
        CHECK_EQ(oi_cli_render_stream_feed(&stream, data, split), OI_OK);
        CHECK_EQ(oi_cli_render_stream_feed(&stream, data + split,
                                           len - split),
                 OI_OK);
        CHECK_EQ(oi_cli_render_stream_finish(&stream), OI_OK);
        result = read_stream(out);
        CHECK_STREQ(result, whole);

        free(result);
        oi_cli_render_stream_free(&stream);
        fclose(out);
    }
    free(whole);
}

TEST(styling_disabled_passes_clean_ascii_through_unchanged) {
    static const char in[] = "hello, plain world";
    char *out = run_whole(in, sizeof in - 1, 0);

    CHECK_STREQ(out, in);
    free(out);
}

TEST(sanitization_applies_even_when_styling_is_disabled) {
    static const char in[] = "hello\x1b[2Jworld";
    char *out = run_whole(in, sizeof in - 1, 0);

    CHECK_STREQ(out, "helloworld");
    free(out);
}

TEST(styling_enabled_renders_bold) {
    static const char in[] = "**bold**";
    char *out = run_whole(in, sizeof in - 1, 1);

    CHECK_STREQ(out, "\x1b[0m\x1b[1mbold\n\x1b[0m");
    free(out);
}

TEST(styling_disabled_leaves_markdown_syntax_literal) {
    static const char in[] = "**not bold**";
    char *out = run_whole(in, sizeof in - 1, 0);

    CHECK_STREQ(out, "**not bold**");
    free(out);
}

TEST(bold_is_chunk_boundary_safe) {
    static const char in[] = "**bold**";

    check_split_invariant(in, sizeof in - 1, 1);
}

TEST(multibyte_utf8_inside_styled_text_is_chunk_boundary_safe) {
    static const char in[] = "**bol\xc3\xa9**";

    check_split_invariant(in, sizeof in - 1, 1);
}

TEST(csi_split_across_feed_calls_is_still_stripped) {
    struct oi_cli_render_stream stream;
    FILE *out = tmpfile();
    char *result;
    static const char part1[] = "before\x1b";
    static const char part2[] = "[2Jafter";

    CHECK_EQ(oi_cli_render_stream_init(&stream, out, 0), OI_OK);
    CHECK_EQ(oi_cli_render_stream_feed(&stream, part1, sizeof part1 - 1),
             OI_OK);
    CHECK_EQ(oi_cli_render_stream_feed(&stream, part2, sizeof part2 - 1),
             OI_OK);
    CHECK_EQ(oi_cli_render_stream_finish(&stream), OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "beforeafter");

    free(result);
    oi_cli_render_stream_free(&stream);
    fclose(out);
}

TEST(truncated_utf8_at_end_of_turn_emits_one_replacement) {
    static const unsigned char in[] = {0xe2, 0x82};
    struct oi_cli_render_stream stream;
    FILE *out = tmpfile();
    char *result;

    CHECK_EQ(oi_cli_render_stream_init(&stream, out, 0), OI_OK);
    CHECK_EQ(oi_cli_render_stream_feed(&stream, (const char *)in, sizeof in),
             OI_OK);
    CHECK_EQ(oi_cli_render_stream_finish(&stream), OI_OK);
    result = read_stream(out);
    CHECK_EQ(strlen(result), (size_t)3);
    CHECK(memcmp(result, "\xef\xbf\xbd", 3) == 0);
    /* finish() is idempotent: calling again appends nothing further. */
    CHECK_EQ(oi_cli_render_stream_finish(&stream), OI_OK);
    free(result);
    result = read_stream(out);
    CHECK_EQ(strlen(result), (size_t)3);

    free(result);
    oi_cli_render_stream_free(&stream);
    fclose(out);
}

TEST(unclosed_bold_at_finish_is_literal_plus_safety_reset) {
    static const char in[] = "**bold";
    char *out = run_whole(in, sizeof in - 1, 1);

    /* No closing "**" ever arrived: the whole thing stays literal text,
     * finish() still guarantees a trailing reset call happened (a no-op
     * here since nothing was ever actually styled). */
    CHECK_STREQ(out, "**bold\n");
    free(out);
}

TEST(long_unterminated_control_string_recovers_before_later_markdown) {
    size_t filler_len = 5000;
    size_t total_len = 6 /* "before" */ + 2 /* ESC ] */ + filler_len +
                       1 /* '\n' */ + 8 /* "**bold**" */;
    char *in = malloc(total_len);
    struct oi_cli_render_stream stream;
    FILE *out = tmpfile();
    char *result;
    size_t offset;

    CHECK(in != NULL);
    if (in == NULL) {
        return;
    }
    offset = 0;
    memcpy(in + offset, "before", 6);
    offset += 6;
    in[offset++] = 0x1b;
    in[offset++] = ']';
    memset(in + offset, 'A', filler_len);
    offset += filler_len;
    in[offset++] = '\n';
    memcpy(in + offset, "**bold**", 8);
    offset += 8;
    CHECK_EQ(offset, total_len);

    CHECK_EQ(oi_cli_render_stream_init(&stream, out, 1), OI_OK);
    CHECK_EQ(oi_cli_render_stream_feed(&stream, in, total_len), OI_OK);
    CHECK_EQ(oi_cli_render_stream_finish(&stream), OI_OK);
    result = read_stream(out);
    /* Whatever happened with the runaway control string, the later
     * "**bold**" line must still render correctly styled. */
    CHECK(strstr(result, "\x1b[0m\x1b[1mbold\n") != NULL);

    free(result);
    free(in);
    oi_cli_render_stream_free(&stream);
    fclose(out);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_render_stream stream;
    FILE *out = tmpfile();
    char byte = 'a';

    CHECK_EQ(oi_cli_render_stream_init(NULL, out, 0), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_stream_init(&stream, NULL, 0), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_stream_init(&stream, out, 0), OI_OK);
    CHECK_EQ(oi_cli_render_stream_feed(NULL, &byte, 1), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_stream_feed(&stream, NULL, 1), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_stream_finish(NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_sanitize_write(NULL, &byte, 1), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_sanitize_write(out, NULL, 1), OI_ERR_INVAL);

    oi_cli_render_stream_free(&stream);
    fclose(out);
}

TEST(sanitize_write_strips_injection_from_a_complete_string) {
    static const char in[] = "tool\x1b]8;;http://evil\x07name";
    FILE *out = tmpfile();
    char *result;

    CHECK_EQ(oi_cli_render_sanitize_write(out, in, sizeof in - 1), OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "toolname");

    free(result);
    fclose(out);
}

int main(void) {
    RUN(styling_disabled_passes_clean_ascii_through_unchanged);
    RUN(sanitization_applies_even_when_styling_is_disabled);
    RUN(styling_enabled_renders_bold);
    RUN(styling_disabled_leaves_markdown_syntax_literal);
    RUN(bold_is_chunk_boundary_safe);
    RUN(multibyte_utf8_inside_styled_text_is_chunk_boundary_safe);
    RUN(csi_split_across_feed_calls_is_still_stripped);
    RUN(truncated_utf8_at_end_of_turn_emits_one_replacement);
    RUN(unclosed_bold_at_finish_is_literal_plus_safety_reset);
    RUN(long_unterminated_control_string_recovers_before_later_markdown);
    RUN(bad_arguments_are_rejected);
    RUN(sanitize_write_strips_injection_from_a_complete_string);
    return oi_test_report();
}
