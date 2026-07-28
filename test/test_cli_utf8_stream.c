#include "cli_utf8_stream.h"
#include "test.h"

#include <string.h>

static const unsigned char fffd[3] = {0xef, 0xbf, 0xbd};

static void expect_bytes(const struct oi_cli_bytebuf *out, const void *data,
                         size_t len) {
    CHECK_EQ(out->len, len);
    if (out->len == len && len != 0) {
        CHECK(memcmp(out->data, data, len) == 0);
    }
}

TEST(valid_sequences_pass_through_unchanged) {
    static const unsigned char ascii[] = {'A'};
    static const unsigned char two[] = {0xc2, 0xa2};
    static const unsigned char three[] = {0xe2, 0x82, 0xac};
    static const unsigned char four[] = {0xf0, 0x9f, 0x98, 0x80};
    struct oi_cli_utf8_stream stream;
    struct oi_cli_bytebuf out;

    oi_cli_utf8_stream_init(&stream);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, ascii, sizeof ascii, &out),
             OI_OK);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, two, sizeof two, &out), OI_OK);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, three, sizeof three, &out),
             OI_OK);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, four, sizeof four, &out),
             OI_OK);
    CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
    CHECK_EQ(out.len, sizeof ascii + sizeof two + sizeof three + sizeof four);
    CHECK(memcmp(out.data, "A", 1) == 0);
    CHECK(memcmp(out.data + 1, two, sizeof two) == 0);
    CHECK(memcmp(out.data + 1 + sizeof two, three, sizeof three) == 0);
    CHECK(memcmp(out.data + 1 + sizeof two + sizeof three, four, sizeof four) ==
          0);

    oi_cli_bytebuf_free(&out);
}

TEST(four_byte_sequence_split_at_every_offset) {
    static const unsigned char emoji[] = {0xf0, 0x9f, 0x98, 0x80};
    size_t split;

    for (split = 1; split < sizeof emoji; split++) {
        struct oi_cli_utf8_stream stream;
        struct oi_cli_bytebuf out;

        oi_cli_utf8_stream_init(&stream);
        oi_cli_bytebuf_init(&out);
        CHECK_EQ(oi_cli_utf8_stream_feed(&stream, emoji, split, &out), OI_OK);
        CHECK_EQ(oi_cli_utf8_stream_feed(&stream, emoji + split,
                                         sizeof emoji - split, &out),
                 OI_OK);
        CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
        expect_bytes(&out, emoji, sizeof emoji);

        oi_cli_bytebuf_free(&out);
    }
}

TEST(byte_at_a_time_feed_matches_whole_buffer_feed) {
    static const unsigned char mixed[] = {
        'h', 'i', 0xc2, 0xa2, 'x', 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80,
        'y'};
    struct oi_cli_utf8_stream stream;
    struct oi_cli_bytebuf out;
    size_t i;

    oi_cli_utf8_stream_init(&stream);
    oi_cli_bytebuf_init(&out);
    for (i = 0; i < sizeof mixed; i++) {
        CHECK_EQ(oi_cli_utf8_stream_feed(&stream, mixed + i, 1, &out), OI_OK);
    }
    CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
    expect_bytes(&out, mixed, sizeof mixed);

    oi_cli_bytebuf_free(&out);
}

TEST(truncated_final_sequence_emits_one_replacement) {
    static const unsigned char truncated[] = {0xe2, 0x82};
    struct oi_cli_utf8_stream stream;
    struct oi_cli_bytebuf out;

    oi_cli_utf8_stream_init(&stream);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, truncated, sizeof truncated,
                                     &out),
             OI_OK);
    CHECK_EQ(out.len, (size_t)0);
    CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
    expect_bytes(&out, fffd, sizeof fffd);
    CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
    expect_bytes(&out, fffd, sizeof fffd);

    oi_cli_bytebuf_free(&out);
}

TEST(stray_continuation_byte_emits_replacement_and_recovers) {
    static const unsigned char input[] = {0x80, 'A'};
    struct oi_cli_utf8_stream stream;
    struct oi_cli_bytebuf out;
    unsigned char expected[4];

    memcpy(expected, fffd, 3);
    expected[3] = 'A';

    oi_cli_utf8_stream_init(&stream);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, input, sizeof input, &out),
             OI_OK);
    CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
    expect_bytes(&out, expected, sizeof expected);

    oi_cli_bytebuf_free(&out);
}

TEST(structurally_illegal_sequences_emit_one_replacement) {
    static const unsigned char overlong_three[] = {0xe0, 0x80, 0x80};
    static const unsigned char surrogate[] = {0xed, 0xa0, 0x80};
    static const unsigned char too_large[] = {0xf4, 0x90, 0x80, 0x80};
    const unsigned char *cases[] = {overlong_three, surrogate, too_large};
    const size_t case_lens[] = {sizeof overlong_three, sizeof surrogate,
                                sizeof too_large};
    size_t i;

    for (i = 0; i < 3; i++) {
        struct oi_cli_utf8_stream stream;
        struct oi_cli_bytebuf out;

        oi_cli_utf8_stream_init(&stream);
        oi_cli_bytebuf_init(&out);
        CHECK_EQ(
            oi_cli_utf8_stream_feed(&stream, cases[i], case_lens[i], &out),
            OI_OK);
        CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
        expect_bytes(&out, fffd, sizeof fffd);

        oi_cli_bytebuf_free(&out);
    }
}

TEST(invalid_lead_byte_and_stray_byte_each_get_their_own_replacement) {
    static const unsigned char overlong_two[] = {0xc0, 0x80};
    unsigned char expected[6];
    struct oi_cli_utf8_stream stream;
    struct oi_cli_bytebuf out;

    memcpy(expected, fffd, 3);
    memcpy(expected + 3, fffd, 3);

    oi_cli_utf8_stream_init(&stream);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, overlong_two,
                                     sizeof overlong_two, &out),
             OI_OK);
    CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
    expect_bytes(&out, expected, sizeof expected);

    oi_cli_bytebuf_free(&out);
}

TEST(embedded_nul_passes_through_unchanged) {
    static const unsigned char input[] = {'a', 0x00, 'b'};
    struct oi_cli_utf8_stream stream;
    struct oi_cli_bytebuf out;

    oi_cli_utf8_stream_init(&stream);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, input, sizeof input, &out),
             OI_OK);
    CHECK_EQ(oi_cli_utf8_stream_finish(&stream, &out), OI_OK);
    expect_bytes(&out, input, sizeof input);

    oi_cli_bytebuf_free(&out);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_utf8_stream stream;
    struct oi_cli_bytebuf out;
    unsigned char byte = 'A';

    oi_cli_utf8_stream_init(&stream);
    oi_cli_bytebuf_init(&out);
    CHECK_EQ(oi_cli_utf8_stream_feed(NULL, &byte, 1, &out), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, &byte, 1, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_stream_feed(&stream, NULL, 1, &out), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_stream_finish(NULL, &out), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_stream_finish(&stream, NULL), OI_ERR_INVAL);

    oi_cli_bytebuf_free(&out);
}

int main(void) {
    RUN(valid_sequences_pass_through_unchanged);
    RUN(four_byte_sequence_split_at_every_offset);
    RUN(byte_at_a_time_feed_matches_whole_buffer_feed);
    RUN(truncated_final_sequence_emits_one_replacement);
    RUN(stray_continuation_byte_emits_replacement_and_recovers);
    RUN(structurally_illegal_sequences_emit_one_replacement);
    RUN(invalid_lead_byte_and_stray_byte_each_get_their_own_replacement);
    RUN(embedded_nul_passes_through_unchanged);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
