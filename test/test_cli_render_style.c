#include "cli_render_style.h"
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

TEST(plain_span_has_no_escapes) {
    static const char text[] = "hello";
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;
    char *result;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, sizeof text - 1, 0), OI_OK);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0,
                                            text, sizeof text - 1, &spans,
                                            &style_active),
             OI_OK);
    CHECK_EQ(style_active, 0);
    result = read_stream(out);
    CHECK_STREQ(result, "hello");

    free(result);
    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

TEST(bold_span_wraps_with_reset_and_code_1) {
    static const char text[] = "bold";
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;
    char *result;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, sizeof text - 1,
                                        OI_CLI_MD_STYLE_BOLD),
             OI_OK);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0,
                                            text, sizeof text - 1, &spans,
                                            &style_active),
             OI_OK);
    CHECK_EQ(style_active, 1);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m\x1b[1mbold");

    free(result);
    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

TEST(italic_and_bold_italic_combined_codes) {
    static const char text[] = "ab";
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;
    char *result;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 1, OI_CLI_MD_STYLE_ITALIC),
             OI_OK);
    CHECK_EQ(oi_cli_md_span_list_append(
                 &spans, 1, 1, OI_CLI_MD_STYLE_BOLD | OI_CLI_MD_STYLE_ITALIC),
             OI_OK);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0,
                                            text, sizeof text - 1, &spans,
                                            &style_active),
             OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m\x1b[3ma\x1b[0m\x1b[1;3mb");

    free(result);
    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

TEST(code_span_uses_cyan) {
    static const char text[] = "x";
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;
    char *result;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 1, OI_CLI_MD_STYLE_CODE),
             OI_OK);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0,
                                            text, sizeof text - 1, &spans,
                                            &style_active),
             OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m\x1b[36mx");

    free(result);
    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

TEST(heading_wraps_whole_line_even_plain_spans) {
    static const char text[] = "Title";
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;
    char *result;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, sizeof text - 1, 0), OI_OK);
    CHECK_EQ(oi_cli_render_style_write_line(
                 out, OI_CLI_MD_BLOCK_STYLE_HEADING, 1, text, sizeof text - 1,
                 &spans, &style_active),
             OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m\x1b[1;4mTitle");

    free(result);
    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

TEST(deep_heading_level_has_no_underline) {
    static const char text[] = "Sub";
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;
    char *result;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, sizeof text - 1, 0), OI_OK);
    CHECK_EQ(oi_cli_render_style_write_line(
                 out, OI_CLI_MD_BLOCK_STYLE_HEADING, 3, text, sizeof text - 1,
                 &spans, &style_active),
             OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m\x1b[1mSub");

    free(result);
    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

TEST(list_style_has_no_extra_wrapper) {
    static const char text[] = "item";
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;
    char *result;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, sizeof text - 1, 0), OI_OK);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_LIST, 0,
                                            text, sizeof text - 1, &spans,
                                            &style_active),
             OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "item");

    free(result);
    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

TEST(verbatim_dim_and_plain) {
    FILE *out = tmpfile();
    int style_active = 0;
    char *result;

    CHECK_EQ(oi_cli_render_style_write_verbatim(out, "code", 4, 1,
                                                &style_active),
             OI_OK);
    CHECK_EQ(style_active, 1);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m\x1b[2mcode");
    free(result);
    fclose(out);

    out = tmpfile();
    style_active = 0;
    CHECK_EQ(oi_cli_render_style_write_verbatim(out, "plain", 5, 0,
                                                &style_active),
             OI_OK);
    CHECK_EQ(style_active, 0);
    result = read_stream(out);
    CHECK_STREQ(result, "plain");
    free(result);
    fclose(out);
}

TEST(reset_is_idempotent_and_only_active_emits_bytes) {
    FILE *out = tmpfile();
    int style_active = 0;
    char *result;

    CHECK_EQ(oi_cli_render_style_reset(out, &style_active), OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "");
    free(result);
    fclose(out);

    out = tmpfile();
    style_active = 1;
    CHECK_EQ(oi_cli_render_style_reset(out, &style_active), OI_OK);
    CHECK_EQ(style_active, 0);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m");
    free(result);
    CHECK_EQ(oi_cli_render_style_reset(out, &style_active), OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m");
    free(result);
    fclose(out);
}

TEST(style_active_persists_across_calls) {
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;
    char *result;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_md_span_list_append(&spans, 0, 1, OI_CLI_MD_STYLE_BOLD),
             OI_OK);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0,
                                            "a", 1, &spans, &style_active),
             OI_OK);
    CHECK_EQ(style_active, 1);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0,
                                            "b", 1, &spans, &style_active),
             OI_OK);
    CHECK_EQ(style_active, 1);
    result = read_stream(out);
    CHECK_STREQ(result, "\x1b[0m\x1b[1ma\x1b[0m\x1b[1mb");

    free(result);
    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

TEST(bad_arguments_are_rejected) {
    FILE *out = tmpfile();
    struct oi_cli_md_span_list spans;
    int style_active = 0;

    oi_cli_md_span_list_init(&spans);
    CHECK_EQ(oi_cli_render_style_write_line(NULL, OI_CLI_MD_BLOCK_STYLE_NONE,
                                            0, "a", 1, &spans, &style_active),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0,
                                            "a", 1, NULL, &style_active),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_style_write_line(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0,
                                            "a", 1, &spans, NULL),
             OI_ERR_INVAL);
    CHECK_EQ(
        oi_cli_render_style_write_verbatim(NULL, "a", 1, 0, &style_active),
        OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_style_write_verbatim(out, NULL, 1, 0, &style_active),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_style_reset(NULL, &style_active), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_render_style_reset(out, NULL), OI_ERR_INVAL);

    oi_cli_md_span_list_free(&spans);
    fclose(out);
}

int main(void) {
    RUN(plain_span_has_no_escapes);
    RUN(bold_span_wraps_with_reset_and_code_1);
    RUN(italic_and_bold_italic_combined_codes);
    RUN(code_span_uses_cyan);
    RUN(heading_wraps_whole_line_even_plain_spans);
    RUN(deep_heading_level_has_no_underline);
    RUN(list_style_has_no_extra_wrapper);
    RUN(verbatim_dim_and_plain);
    RUN(reset_is_idempotent_and_only_active_emits_bytes);
    RUN(style_active_persists_across_calls);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
