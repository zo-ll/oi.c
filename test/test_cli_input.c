#include "cli_input.h"
#include "test.h"

#include <string.h>

static enum oi_cli_input_event_type decode(
    struct oi_cli_input_decoder *decoder, const char *input, size_t input_len,
    size_t *consumed, struct oi_cli_input_event *event) {
    CHECK_EQ(oi_cli_input_decode(decoder, (const unsigned char *)input,
                                 input_len, consumed, event),
             OI_OK);
    return event->type;
}

TEST(text_and_control_keys_are_distinct) {
    struct oi_cli_input_decoder decoder;
    struct oi_cli_input_event event;
    size_t consumed = 0;

    oi_cli_input_decoder_init(&decoder);
    CHECK_EQ(decode(&decoder, "a", 1, &consumed, &event), OI_CLI_INPUT_TEXT);
    CHECK_EQ(consumed, 1);
    CHECK_EQ(event.text_len, 1);
    CHECK_EQ(event.text[0], 'a');
    CHECK_EQ(decode(&decoder, "\r", 1, &consumed, &event),
             OI_CLI_INPUT_ENTER);
    CHECK_EQ(decode(&decoder, "\n", 1, &consumed, &event),
             OI_CLI_INPUT_NEWLINE);
    CHECK_EQ(decode(&decoder, "\x03", 1, &consumed, &event),
             OI_CLI_INPUT_CTRL_C);
    CHECK_EQ(decode(&decoder, "\x04", 1, &consumed, &event),
             OI_CLI_INPUT_CTRL_D);
    CHECK_EQ(decode(&decoder, "\x7f", 1, &consumed, &event),
             OI_CLI_INPUT_BACKSPACE);
    CHECK_EQ(decode(&decoder, "\t", 1, &consumed, &event), OI_CLI_INPUT_TAB);
    CHECK_EQ(decode(&decoder, "\x01", 1, &consumed, &event),
             OI_CLI_INPUT_HOME);
    CHECK_EQ(decode(&decoder, "\x05", 1, &consumed, &event),
             OI_CLI_INPUT_END);
}

TEST(utf8_text_is_emitted_as_one_code_point) {
    static const char euro[] = {(char)0xe2, (char)0x82, (char)0xac};
    struct oi_cli_input_decoder decoder;
    struct oi_cli_input_event event;
    size_t consumed = 0;

    oi_cli_input_decoder_init(&decoder);
    CHECK_EQ(oi_cli_input_decode(&decoder, (const unsigned char *)euro, 2,
                                 &consumed, &event),
             OI_ERR_AGAIN);
    CHECK_EQ(consumed, 0);
    CHECK_EQ(decode(&decoder, euro, sizeof euro, &consumed, &event),
             OI_CLI_INPUT_TEXT);
    CHECK_EQ(consumed, sizeof euro);
    CHECK_EQ(event.text_len, sizeof euro);
    CHECK(memcmp(event.text, euro, sizeof euro) == 0);
}

TEST(arrows_and_editing_sequences_are_decoded) {
    struct oi_cli_input_decoder decoder;
    struct oi_cli_input_event event;
    size_t consumed = 0;

    oi_cli_input_decoder_init(&decoder);
    CHECK_EQ(decode(&decoder, "\x1b[Ax", 4, &consumed, &event),
             OI_CLI_INPUT_UP);
    CHECK_EQ(consumed, 3);
    CHECK_EQ(decode(&decoder, "\x1b[B", 3, &consumed, &event),
             OI_CLI_INPUT_DOWN);
    CHECK_EQ(decode(&decoder, "\x1b[C", 3, &consumed, &event),
             OI_CLI_INPUT_RIGHT);
    CHECK_EQ(decode(&decoder, "\x1b[D", 3, &consumed, &event),
             OI_CLI_INPUT_LEFT);
    CHECK_EQ(decode(&decoder, "\x1b[H", 3, &consumed, &event),
             OI_CLI_INPUT_HOME);
    CHECK_EQ(decode(&decoder, "\x1b[F", 3, &consumed, &event),
             OI_CLI_INPUT_END);
    CHECK_EQ(decode(&decoder, "\x1b[3~", 4, &consumed, &event),
             OI_CLI_INPUT_DELETE);
}

TEST(fragmented_escape_sequence_waits_for_completion) {
    struct oi_cli_input_decoder decoder;
    struct oi_cli_input_event event;
    size_t consumed = 99;

    oi_cli_input_decoder_init(&decoder);
    CHECK_EQ(oi_cli_input_decode(&decoder, (const unsigned char *)"\x1b", 1,
                                 &consumed, &event),
             OI_ERR_AGAIN);
    CHECK_EQ(consumed, 0);
    CHECK_EQ(oi_cli_input_decode(&decoder,
                                 (const unsigned char *)"\x1b[", 2, &consumed,
                                 &event),
             OI_ERR_AGAIN);
    CHECK_EQ(decode(&decoder, "\x1b[D", 3, &consumed, &event),
             OI_CLI_INPUT_LEFT);
    CHECK_EQ(oi_cli_input_decode_escape(&consumed, &event), OI_OK);
    CHECK_EQ(consumed, 1);
    CHECK_EQ(event.type, OI_CLI_INPUT_ESCAPE);
}

TEST(bracketed_paste_suppresses_key_interpretation) {
    struct oi_cli_input_decoder decoder;
    struct oi_cli_input_event event;
    size_t consumed = 0;

    oi_cli_input_decoder_init(&decoder);
    CHECK_EQ(decode(&decoder, "\x1b[200~", 6, &consumed, &event),
             OI_CLI_INPUT_PASTE_BEGIN);
    CHECK(decoder.pasting);
    CHECK_EQ(decode(&decoder, "\n", 1, &consumed, &event),
             OI_CLI_INPUT_TEXT);
    CHECK(event.pasted);
    CHECK_EQ(event.text[0], '\n');
    CHECK_EQ(decode(&decoder, "\x03", 1, &consumed, &event),
             OI_CLI_INPUT_TEXT);
    CHECK(event.pasted);
    CHECK_EQ(oi_cli_input_decode(&decoder,
                                 (const unsigned char *)"\x1b[20", 4,
                                 &consumed, &event),
             OI_ERR_AGAIN);
    CHECK_EQ(decode(&decoder, "\x1b[201~", 6, &consumed, &event),
             OI_CLI_INPUT_PASTE_END);
    CHECK(!decoder.pasting);
}

TEST(invalid_input_is_consumed_without_stalling) {
    static const char malformed[] = {(char)0xc0, (char)0x80};
    struct oi_cli_input_decoder decoder;
    struct oi_cli_input_event event;
    size_t consumed = 0;

    oi_cli_input_decoder_init(&decoder);
    CHECK_EQ(decode(&decoder, malformed, sizeof malformed, &consumed, &event),
             OI_CLI_INPUT_INVALID);
    CHECK_EQ(consumed, 1);
    CHECK_EQ(decode(&decoder, "\x1bX", 2, &consumed, &event),
             OI_CLI_INPUT_INVALID);
    CHECK_EQ(consumed, 1);
    CHECK_EQ(decode(&decoder, "\0", 1, &consumed, &event),
             OI_CLI_INPUT_INVALID);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_input_decoder decoder;
    struct oi_cli_input_event event;
    size_t consumed = 0;

    oi_cli_input_decoder_init(&decoder);
    CHECK_EQ(oi_cli_input_decode(NULL, (const unsigned char *)"a", 1,
                                 &consumed, &event),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_input_decode(&decoder, NULL, 1, &consumed, &event),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_input_decode(&decoder, NULL, 0, &consumed, &event),
             OI_ERR_AGAIN);
    CHECK_EQ(oi_cli_input_decode_escape(NULL, &event), OI_ERR_INVAL);
}

int main(void) {
    RUN(text_and_control_keys_are_distinct);
    RUN(utf8_text_is_emitted_as_one_code_point);
    RUN(arrows_and_editing_sequences_are_decoded);
    RUN(fragmented_escape_sequence_waits_for_completion);
    RUN(bracketed_paste_suppresses_key_interpretation);
    RUN(invalid_input_is_consumed_without_stalling);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
