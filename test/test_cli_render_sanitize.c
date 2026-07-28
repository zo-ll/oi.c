#include "cli_render_sanitize.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static void run_case_whole(const unsigned char *in, size_t in_len,
                           const unsigned char *expected,
                           size_t expected_len) {
    struct oi_cli_sanitize_state state;
    struct oi_cli_bytebuf out;

    oi_cli_sanitize_init(&state);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_sanitize_feed(&state, in, in_len, &out), OI_OK);
    CHECK_EQ(oi_cli_sanitize_finish(&state), OI_OK);
    CHECK_EQ(out.len, expected_len);
    if (out.len == expected_len && expected_len != 0) {
        CHECK(memcmp(out.data, expected, expected_len) == 0);
    }
    oi_cli_bytebuf_free(&out);
}

static void run_case_split(const unsigned char *in, size_t in_len,
                           const unsigned char *expected,
                           size_t expected_len) {
    size_t split;

    for (split = 1; split < in_len; split++) {
        struct oi_cli_sanitize_state state;
        struct oi_cli_bytebuf out;

        oi_cli_sanitize_init(&state);
        oi_cli_bytebuf_init(&out);
        CHECK_EQ(oi_cli_sanitize_feed(&state, in, split, &out), OI_OK);
        CHECK_EQ(
            oi_cli_sanitize_feed(&state, in + split, in_len - split, &out),
            OI_OK);
        CHECK_EQ(oi_cli_sanitize_finish(&state), OI_OK);
        CHECK_EQ(out.len, expected_len);
        if (out.len == expected_len && expected_len != 0) {
            CHECK(memcmp(out.data, expected, expected_len) == 0);
        }
        oi_cli_bytebuf_free(&out);
    }
}

/* Verifies both an unsplit feed and every possible two-call chunk split
 * produce byte-identical output. */
static void run_case(const char *in, size_t in_len, const char *expected,
                     size_t expected_len) {
    run_case_whole((const unsigned char *)in, in_len,
                   (const unsigned char *)expected, expected_len);
    run_case_split((const unsigned char *)in, in_len,
                   (const unsigned char *)expected, expected_len);
}

TEST(plain_text_and_allowed_whitespace_pass_through) {
    static const char text[] = "hello\tworld\n";

    run_case(text, sizeof text - 1, text, sizeof text - 1);
}

TEST(embedded_nul_is_stripped) {
    static const char in[] = {'a', '\0', 'b'};
    static const char expected[] = {'a', 'b'};

    run_case(in, sizeof in, expected, sizeof expected);
}

TEST(bare_cr_is_stripped) {
    static const char in[] = "line one\r\nline two\r";
    static const char expected[] = "line one\nline two";

    run_case(in, sizeof in - 1, expected, sizeof expected - 1);
}

TEST(bare_bel_is_stripped) {
    static const char in[] = {'a', 0x07, 'b'};
    static const char expected[] = {'a', 'b'};

    run_case(in, sizeof in, expected, sizeof expected);
}

TEST(csi_cursor_moves_are_dropped) {
    static const char clear[] = "before\033[2Jafter";
    static const char move[] = "before\033[10Cafter";
    static const char expected[] = "beforeafter";

    run_case(clear, sizeof clear - 1, expected, sizeof expected - 1);
    run_case(move, sizeof move - 1, expected, sizeof expected - 1);
}

TEST(osc8_hyperlink_is_dropped_bel_and_st_terminated) {
    static const char bel[] = "before\033]8;;http://evil\007after";
    static const char st[] = "before\033]8;;http://evil\033\\after";
    static const char expected[] = "beforeafter";

    run_case(bel, sizeof bel - 1, expected, sizeof expected - 1);
    run_case(st, sizeof st - 1, expected, sizeof expected - 1);
}

TEST(osc52_clipboard_write_is_dropped_bel_and_st_terminated) {
    static const char bel[] = "before\033]52;c;ZXZpbA==\007after";
    static const char st[] = "before\033]52;c;ZXZpbA==\033\\after";
    static const char expected[] = "beforeafter";

    run_case(bel, sizeof bel - 1, expected, sizeof expected - 1);
    run_case(st, sizeof st - 1, expected, sizeof expected - 1);
}

TEST(dcs_sos_pm_apc_are_dropped) {
    static const char dcs[] = "before\033Psomething\033\\after";
    static const char sos[] = "before\033Xsomething\033\\after";
    static const char pm[] = "before\033^something\033\\after";
    static const char apc[] = "before\033_something\033\\after";
    static const char expected[] = "beforeafter";

    run_case(dcs, sizeof dcs - 1, expected, sizeof expected - 1);
    run_case(sos, sizeof sos - 1, expected, sizeof expected - 1);
    run_case(pm, sizeof pm - 1, expected, sizeof expected - 1);
    run_case(apc, sizeof apc - 1, expected, sizeof expected - 1);
}

TEST(simple_two_byte_escapes_are_dropped) {
    static const char reset[] = "before\033cafter";
    static const char keypad[] = "before\033=after";
    static const char expected[] = "beforeafter";

    run_case(reset, sizeof reset - 1, expected, sizeof expected - 1);
    run_case(keypad, sizeof keypad - 1, expected, sizeof expected - 1);
}

TEST(unterminated_escape_at_end_of_turn_is_abandoned) {
    static const char in[] = "before\033";
    static const char expected[] = "before";

    run_case(in, sizeof in - 1, expected, sizeof expected - 1);
}

TEST(long_unterminated_control_string_hits_cap_and_recovers) {
    size_t filler_len = OI_CLI_SANITIZE_MAX_SEQUENCE + 100;
    /* ESC + ']' already count as 2 of the cap's budget before the filler
     * body starts accumulating. */
    size_t consumed_before_cap = OI_CLI_SANITIZE_MAX_SEQUENCE - 2;
    size_t leftover = filler_len - consumed_before_cap;
    static const char prefix[] = "before";
    static const char suffix[] = "safe";
    size_t in_len = (sizeof prefix - 1) + 2 + filler_len + (sizeof suffix - 1);
    size_t expected_len = (sizeof prefix - 1) + leftover + (sizeof suffix - 1);
    unsigned char *in = malloc(in_len);
    unsigned char *expected = malloc(expected_len);
    size_t offset;
    struct oi_cli_sanitize_state state;
    struct oi_cli_bytebuf out;

    CHECK(in != NULL);
    CHECK(expected != NULL);
    if (in == NULL || expected == NULL) {
        free(in);
        free(expected);
        return;
    }

    offset = 0;
    memcpy(in + offset, prefix, sizeof prefix - 1);
    offset += sizeof prefix - 1;
    in[offset++] = 0x1b;
    in[offset++] = ']';
    memset(in + offset, 'A', filler_len);
    offset += filler_len;
    memcpy(in + offset, suffix, sizeof suffix - 1);
    offset += sizeof suffix - 1;
    CHECK_EQ(offset, in_len);

    offset = 0;
    memcpy(expected + offset, prefix, sizeof prefix - 1);
    offset += sizeof prefix - 1;
    memset(expected + offset, 'A', leftover);
    offset += leftover;
    memcpy(expected + offset, suffix, sizeof suffix - 1);
    offset += sizeof suffix - 1;
    CHECK_EQ(offset, expected_len);

    oi_cli_sanitize_init(&state);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_sanitize_feed(&state, in, in_len, &out), OI_OK);
    CHECK_EQ(oi_cli_sanitize_finish(&state), OI_OK);
    CHECK_EQ(out.len, expected_len);
    if (out.len == expected_len) {
        CHECK(memcmp(out.data, expected, expected_len) == 0);
    }

    oi_cli_bytebuf_free(&out);
    free(in);
    free(expected);
}

TEST(interleaved_text_and_csi_keeps_only_the_text) {
    static const char in[] = "one\033[1Atwo\033[2Bthree";
    static const char expected[] = "onetwothree";

    run_case(in, sizeof in - 1, expected, sizeof expected - 1);
}

TEST(c1_control_encoded_as_utf8_is_stripped) {
    static const char in[] = {'a', (char)0xc2, (char)0x9b, 'b'};
    static const char expected[] = {'a', 'b'};

    run_case(in, sizeof in, expected, sizeof expected);
}

TEST(c2_lead_byte_for_an_ordinary_codepoint_passes_through) {
    static const char in[] = {'a', (char)0xc2, (char)0xa2, 'b'};

    run_case(in, sizeof in, in, sizeof in);
}

TEST(trailing_c2_at_end_of_turn_is_abandoned) {
    struct oi_cli_sanitize_state state;
    struct oi_cli_bytebuf out;
    static const unsigned char in[] = {'a', 0xc2};
    static const unsigned char expected[] = {'a'};

    oi_cli_sanitize_init(&state);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_sanitize_feed(&state, in, sizeof in, &out), OI_OK);
    CHECK_EQ(oi_cli_sanitize_finish(&state), OI_OK);
    CHECK_EQ(out.len, sizeof expected);
    if (out.len == sizeof expected) {
        CHECK(memcmp(out.data, expected, sizeof expected) == 0);
    }

    oi_cli_bytebuf_free(&out);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_sanitize_state state;
    struct oi_cli_bytebuf out;
    unsigned char byte = 'A';

    oi_cli_sanitize_init(&state);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_sanitize_feed(NULL, &byte, 1, &out), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_sanitize_feed(&state, &byte, 1, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_sanitize_feed(&state, NULL, 1, &out), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_sanitize_finish(NULL), OI_ERR_INVAL);

    oi_cli_bytebuf_free(&out);
}

int main(void) {
    RUN(plain_text_and_allowed_whitespace_pass_through);
    RUN(embedded_nul_is_stripped);
    RUN(bare_cr_is_stripped);
    RUN(bare_bel_is_stripped);
    RUN(csi_cursor_moves_are_dropped);
    RUN(osc8_hyperlink_is_dropped_bel_and_st_terminated);
    RUN(osc52_clipboard_write_is_dropped_bel_and_st_terminated);
    RUN(dcs_sos_pm_apc_are_dropped);
    RUN(simple_two_byte_escapes_are_dropped);
    RUN(unterminated_escape_at_end_of_turn_is_abandoned);
    RUN(long_unterminated_control_string_hits_cap_and_recovers);
    RUN(interleaved_text_and_csi_keeps_only_the_text);
    RUN(c1_control_encoded_as_utf8_is_stripped);
    RUN(c2_lead_byte_for_an_ordinary_codepoint_passes_through);
    RUN(trailing_c2_at_end_of_turn_is_abandoned);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
