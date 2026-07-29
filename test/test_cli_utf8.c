#include "cli_utf8.h"
#include "test.h"

#include <stddef.h>
#include <stdint.h>

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

TEST(decode_reports_the_code_point_for_every_sequence_length) {
    static const unsigned char ascii[] = {'A'};
    static const unsigned char two[] = {0xc2, 0xa2};        /* U+00A2 */
    static const unsigned char three[] = {0xe4, 0xb8, 0xad}; /* U+4E2D */
    static const unsigned char four[] = {0xf0, 0x9f, 0x98, 0x80}; /* U+1F600 */
    uint32_t codepoint = 0;

    CHECK_EQ(oi_cli_utf8_decode((const char *)ascii, 1, &codepoint), OI_OK);
    CHECK_EQ(codepoint, 0x41U);
    CHECK_EQ(oi_cli_utf8_decode((const char *)two, 2, &codepoint), OI_OK);
    CHECK_EQ(codepoint, 0xa2U);
    CHECK_EQ(oi_cli_utf8_decode((const char *)three, 3, &codepoint), OI_OK);
    CHECK_EQ(codepoint, 0x4e2dU);
    CHECK_EQ(oi_cli_utf8_decode((const char *)four, 4, &codepoint), OI_OK);
    CHECK_EQ(codepoint, 0x1f600U);
}

TEST(decode_rejects_bad_arguments) {
    static const unsigned char ascii[] = {'A'};
    uint32_t codepoint = 0;

    CHECK_EQ(oi_cli_utf8_decode(NULL, 1, &codepoint), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_decode((const char *)ascii, 1, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_decode((const char *)ascii, 0, &codepoint),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_utf8_decode((const char *)ascii, 5, &codepoint),
             OI_ERR_INVAL);
}

TEST(codepoint_width_follows_the_documented_policy) {
    /* ASCII and other unremarkable code points default to width 1. */
    CHECK_EQ(oi_cli_utf8_codepoint_width('A'), 1U);
    CHECK_EQ(oi_cli_utf8_codepoint_width(0xa2), 1U); /* CENT SIGN */

    /* C0 controls and DEL never advance the column. */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x09), 0U); /* TAB */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x1f), 0U);
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x7f), 0U); /* DEL */

    /* Combining marks and zero-width formatting code points are width 0. */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x0301), 0U); /* COMBINING ACUTE */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x200d), 0U); /* ZWJ */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0xfe0f), 0U); /* VARIATION SEL-16 */

    /* CJK/Hangul code points are wide (width 2). */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x4e2d), 2U); /* CJK 中 */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0xac00), 2U); /* HANGUL 가 */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x30ab), 2U); /* KATAKANA KA */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0xff21), 2U); /* FULLWIDTH A */

    /* Boundaries of the documented ranges. */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x4dff), 1U); /* just below CJK */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x4e00), 2U); /* CJK block start */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0x9fff), 2U); /* CJK block end */
    CHECK_EQ(oi_cli_utf8_codepoint_width(0xa4d0), 1U); /* between Yi and
                                                          Hangul Syllables */
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
    RUN(decode_reports_the_code_point_for_every_sequence_length);
    RUN(decode_rejects_bad_arguments);
    RUN(codepoint_width_follows_the_documented_policy);
    return oi_test_report();
}
