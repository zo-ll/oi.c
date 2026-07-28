#include "cli_present.h"
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

static oi_status feed_delta(struct oi_cli_present *present, const char *data,
                            size_t len) {
    struct oi_cli_conversation_event event;

    memset(&event, 0, sizeof event);
    event.type = OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA;
    event.as.bytes.data = data;
    event.as.bytes.len = len;
    return oi_cli_present_event(&event, present);
}

static oi_status feed_response_done(struct oi_cli_present *present) {
    struct oi_cli_conversation_event event;

    memset(&event, 0, sizeof event);
    event.type = OI_CLI_CONVERSATION_EVENT_RESPONSE_DONE;
    return oi_cli_present_event(&event, present);
}

static oi_status feed_turn_done(struct oi_cli_present *present,
                                oi_status status) {
    struct oi_cli_conversation_event event;

    memset(&event, 0, sizeof event);
    event.type = OI_CLI_CONVERSATION_EVENT_TURN_DONE;
    event.as.turn_done.status = status;
    return oi_cli_present_event(&event, present);
}

static oi_status feed_tool_starting(struct oi_cli_present *present,
                                    const char *name, size_t name_len) {
    struct oi_cli_conversation_event event;
    struct oi_cli_string name_string;

    name_string.data = (char *)name;
    name_string.len = name_len;
    memset(&event, 0, sizeof event);
    event.type = OI_CLI_CONVERSATION_EVENT_TOOL_STARTING;
    event.as.tool_starting.name = &name_string;
    return oi_cli_present_event(&event, present);
}

static oi_status feed_model_error(struct oi_cli_present *present,
                                  const char *body, size_t body_len) {
    struct oi_cli_conversation_event event;

    memset(&event, 0, sizeof event);
    event.type = OI_CLI_CONVERSATION_EVENT_MODEL_ERROR;
    event.as.model_error.http_status = 500;
    event.as.model_error.body = body;
    event.as.model_error.body_len = body_len;
    return oi_cli_present_event(&event, present);
}

TEST(injected_csi_never_reaches_the_terminal) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_present present;
    static const char in[] = "before\x1b[2Jafter";
    char *result;

    CHECK_EQ(oi_cli_present_init(&present, out, err, 1, 0, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_delta(&present, in, sizeof in - 1), OI_OK);
    CHECK_EQ(feed_response_done(&present), OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "beforeafter\n");

    free(result);
    oi_cli_present_free(&present);
    fclose(out);
    fclose(err);
}

TEST(osc_injection_in_assistant_text_is_stripped) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_present present;
    static const char in[] = "safe\x1b]52;c;ZXZpbA==\x07text";
    char *result;

    CHECK_EQ(oi_cli_present_init(&present, out, err, 1, 0, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_delta(&present, in, sizeof in - 1), OI_OK);
    CHECK_EQ(feed_response_done(&present), OI_OK);
    result = read_stream(out);
    CHECK_STREQ(result, "safetext\n");

    free(result);
    oi_cli_present_free(&present);
    fclose(out);
    fclose(err);
}

TEST(captured_assistant_text_is_raw_not_sanitized) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_present present;
    static const char in[] = "before\x1b[2Jafter";
    char *text = NULL;
    size_t text_len = 0;

    CHECK_EQ(oi_cli_present_init(&present, out, err, 1, 0, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_delta(&present, in, sizeof in - 1), OI_OK);
    CHECK_EQ(feed_response_done(&present), OI_OK);
    CHECK_EQ(oi_cli_present_take_assistant(&present, &text, &text_len), OI_OK);
    CHECK_EQ(text_len, sizeof in - 1);
    CHECK(memcmp(text, in, sizeof in - 1) == 0);

    free(text);
    oi_cli_present_free(&present);
    fclose(out);
    fclose(err);
}

TEST(styling_enabled_differs_from_disabled_for_the_same_input) {
    FILE *out_plain = tmpfile();
    FILE *out_styled = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_present plain;
    struct oi_cli_present styled;
    static const char in[] = "**bold**";
    char *plain_text;
    char *styled_text;

    CHECK_EQ(oi_cli_present_init(&plain, out_plain, err, 0, 0, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_delta(&plain, in, sizeof in - 1), OI_OK);
    CHECK_EQ(feed_response_done(&plain), OI_OK);
    plain_text = read_stream(out_plain);
    CHECK_STREQ(plain_text, "**bold**\n");

    CHECK_EQ(oi_cli_present_init(&styled, out_styled, err, 0, 1, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_delta(&styled, in, sizeof in - 1), OI_OK);
    CHECK_EQ(feed_response_done(&styled), OI_OK);
    styled_text = read_stream(out_styled);
    CHECK(strcmp(styled_text, plain_text) != 0);
    CHECK(strstr(styled_text, "\x1b[1m") != NULL);

    free(plain_text);
    free(styled_text);
    oi_cli_present_free(&plain);
    oi_cli_present_free(&styled);
    fclose(out_plain);
    fclose(out_styled);
    fclose(err);
}

TEST(tool_name_sanitization_leaves_trusted_prefix_untouched) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_present present;
    static const char name[] = "evil\x1b]52;c;ZXZpbA==\x07tool";
    char *result;

    CHECK_EQ(oi_cli_present_init(&present, out, err, 0, 0, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_tool_starting(&present, name, sizeof name - 1), OI_OK);
    result = read_stream(err);
    CHECK_STREQ(result, "oi: running tool eviltool\n");

    free(result);
    oi_cli_present_free(&present);
    fclose(out);
    fclose(err);
}

TEST(model_error_body_sanitization_leaves_trusted_header_untouched) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_present present;
    static const char body[] = "bad\x1b[2Jbody";
    char *result;

    CHECK_EQ(oi_cli_present_init(&present, out, err, 0, 0, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_model_error(&present, body, sizeof body - 1), OI_OK);
    result = read_stream(err);
    CHECK_STREQ(result, "oi: model error body (http=500): badbody\n");

    free(result);
    oi_cli_present_free(&present);
    fclose(out);
    fclose(err);
}

TEST(response_done_emits_trailing_reset_for_an_unclosed_bold_span) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_present present;
    static const char in[] = "**bold";
    char *result;

    CHECK_EQ(oi_cli_present_init(&present, out, err, 0, 1, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_delta(&present, in, sizeof in - 1), OI_OK);
    CHECK_EQ(feed_response_done(&present), OI_OK);
    result = read_stream(out);
    /* Unclosed "**bold" never matched, so it renders literal; the point
     * of this test is that RESPONSE_DONE's render_stream_finish() ran
     * (safe/no-op here since nothing was ever actually styled) before the
     * trailing newline. */
    CHECK_STREQ(result, "**bold\n");

    free(result);
    oi_cli_present_free(&present);
    fclose(out);
    fclose(err);
}

TEST(turn_done_without_response_done_still_flushes_render_state) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_present present;
    static const char in[] = "**bold**";
    char *result;

    CHECK_EQ(oi_cli_present_init(&present, out, err, 0, 1, NULL, NULL),
             OI_OK);
    CHECK_EQ(feed_delta(&present, in, sizeof in - 1), OI_OK);
    /* A turn that fails/cancels before RESPONSE_DONE ever fires must
     * still flush any pending styled line, not leave it in the markdown
     * module's buffer forever. */
    CHECK_EQ(feed_turn_done(&present, OI_ERR_TIMEOUT), OI_OK);
    CHECK_EQ(present.status, OI_ERR_TIMEOUT);
    result = read_stream(out);
    CHECK(strstr(result, "\x1b[1mbold\n") != NULL);

    free(result);
    oi_cli_present_free(&present);
    fclose(out);
    fclose(err);
}

int main(void) {
    RUN(injected_csi_never_reaches_the_terminal);
    RUN(osc_injection_in_assistant_text_is_stripped);
    RUN(captured_assistant_text_is_raw_not_sanitized);
    RUN(styling_enabled_differs_from_disabled_for_the_same_input);
    RUN(tool_name_sanitization_leaves_trusted_prefix_untouched);
    RUN(model_error_body_sanitization_leaves_trusted_header_untouched);
    RUN(response_done_emits_trailing_reset_for_an_unclosed_bold_span);
    RUN(turn_done_without_response_done_still_flushes_render_state);
    return oi_test_report();
}
