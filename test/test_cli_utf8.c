#include "cli_utf8.h"
#include "test.h"

#include <stddef.h>

TEST(valid_sequences_report_their_lengths) {
    static const unsigned char ascii[] = {'A'};
    static const unsigned char two[] = {0xc2, 0xa2};
    static const unsigned char three[] = {0xe2, 0x82, 0xac};
    static const unsigned char four[] = {0xf0, 0x9f, 0x98, 0x80};
    size_t len = 0;

    CHECK_EQ(oi_cli_utf8_sequence_length(ascii, sizeof ascii, &len), OI_OK);
    CHECK_EQ(len, 1);
    CHECK_EQ(oi_cli_utf8_sequence_length(two, sizeof two, &len), OI_OK);
    CHECK_EQ(len, 2);
    CHECK_EQ(oi_cli_utf8_sequence_length(three, sizeof three, &len), OI_OK);
    CHECK_EQ(len, 3);
    CHECK_EQ(oi_cli_utf8_sequence_length(four, sizeof four, &len), OI_OK);
    CHECK_EQ(len, 4);
}

TEST(validation_accepts_text_and_embedded_nul) {
    static const char text[] = {'A', '\0', (char)0xc2, (char)0xa2,
                                (char)0xe2, (char)0x82, (char)0xac,
                                (char)0xf0, (char)0x9f, (char)0x98,
                                (char)0x80};

    CHECK_EQ(oi_cli_utf8_validate(NULL, 0), OI_OK);
    CHECK_EQ(oi_cli_utf8_validate(text, sizeof text), OI_OK);
}

TEST(validation_rejects_malformed_sequences) {
    static const char overlong_two[] = {(char)0xc0, (char)0x80};
    static const char overlong_three[] = {(char)0xe0, (char)0x80, (char)0x80};
    static const char surrogate[] = {(char)0xed, (char)0xa0, (char)0x80};
    static const char too_large[] = {(char)0xf4, (char)0x90, (char)0x80,
                                    (char)0x80};
    static const char truncated[] = {(char)0xe2, (char)0x82};
    static const char stray[] = {(char)0x80};

    CHECK_EQ(oi_cli_utf8_validate(overlong_two, sizeof overlong_two),
             OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_validate(overlong_three, sizeof overlong_three),
             OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_validate(surrogate, sizeof surrogate), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_validate(too_large, sizeof too_large), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_validate(truncated, sizeof truncated), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_validate(stray, sizeof stray), OI_ERR_PARSE);
}

TEST(boundaries_move_across_mixed_text) {
    static const char text[] = {'A', (char)0xc2, (char)0xa2,
                                (char)0xe2, (char)0x82, (char)0xac,
                                (char)0xf0, (char)0x9f, (char)0x98,
                                (char)0x80};
    static const size_t boundaries[] = {0, 1, 3, 6, 10};
    size_t offset = 0;
    size_t i;

    for (i = 1; i < sizeof boundaries / sizeof boundaries[0]; i++) {
        CHECK_EQ(oi_cli_utf8_next_boundary(text, sizeof text, offset, &offset),
                 OI_OK);
        CHECK_EQ(offset, boundaries[i]);
    }
    for (i = sizeof boundaries / sizeof boundaries[0] - 1; i > 0; i--) {
        CHECK_EQ(
            oi_cli_utf8_previous_boundary(text, sizeof text, offset, &offset),
            OI_OK);
        CHECK_EQ(offset, boundaries[i - 1]);
    }
}

TEST(boundary_functions_reject_bad_arguments_and_offsets) {
    static const char text[] = {(char)0xe2, (char)0x82, (char)0xac};
    size_t offset = 99;

    CHECK_EQ(oi_cli_utf8_next_boundary(NULL, 1, 0, &offset), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_next_boundary(text, sizeof text, 0, NULL),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_next_boundary(text, sizeof text, sizeof text, &offset),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_next_boundary(text, sizeof text, 1, &offset),
             OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_previous_boundary(text, sizeof text, 0, &offset),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_previous_boundary(text, sizeof text, 2, &offset),
             OI_ERR_PARSE);
    CHECK_EQ(
        oi_cli_utf8_previous_boundary(text, sizeof text, sizeof text + 1,
                                      &offset),
        OI_ERR_INVAL);
}

TEST(lead_length_classifies_every_byte_class) {
    size_t len = 0;

    CHECK_EQ(oi_cli_utf8_lead_length(0x00, &len), OI_OK);
    CHECK_EQ(len, 1);
    CHECK_EQ(oi_cli_utf8_lead_length(0x7f, &len), OI_OK);
    CHECK_EQ(len, 1);
    CHECK_EQ(oi_cli_utf8_lead_length(0xc2, &len), OI_OK);
    CHECK_EQ(len, 2);
    CHECK_EQ(oi_cli_utf8_lead_length(0xdf, &len), OI_OK);
    CHECK_EQ(len, 2);
    CHECK_EQ(oi_cli_utf8_lead_length(0xe0, &len), OI_OK);
    CHECK_EQ(len, 3);
    CHECK_EQ(oi_cli_utf8_lead_length(0xef, &len), OI_OK);
    CHECK_EQ(len, 3);
    CHECK_EQ(oi_cli_utf8_lead_length(0xf0, &len), OI_OK);
    CHECK_EQ(len, 4);
    CHECK_EQ(oi_cli_utf8_lead_length(0xf4, &len), OI_OK);
    CHECK_EQ(len, 4);

    CHECK_EQ(oi_cli_utf8_lead_length(0x80, &len), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_lead_length(0xc0, &len), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_lead_length(0xc1, &len), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_lead_length(0xf5, &len), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_lead_length(0xff, &len), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_lead_length(0x41, NULL), OI_ERR_INVAL);
}

TEST(sequence_function_rejects_bad_arguments) {
    static const unsigned char text[] = {'A'};
    size_t len = 0;

    CHECK_EQ(oi_cli_utf8_sequence_length(NULL, 1, &len), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_sequence_length(text, sizeof text, NULL),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_sequence_length(text, 0, &len), OI_ERR_PARSE);
    CHECK_EQ(oi_cli_utf8_validate(NULL, 1), OI_ERR_INVAL);
}

int main(void) {
    RUN(valid_sequences_report_their_lengths);
    RUN(validation_accepts_text_and_embedded_nul);
    RUN(validation_rejects_malformed_sequences);
    RUN(boundaries_move_across_mixed_text);
    RUN(boundary_functions_reject_bad_arguments_and_offsets);
    RUN(lead_length_classifies_every_byte_class);
    RUN(sequence_function_rejects_bad_arguments);
    return oi_test_report();
}
