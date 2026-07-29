#include "cli_utf8.h"

static int is_continuation(unsigned char byte) {
    return (byte & 0xc0U) == 0x80U;
}

oi_status oi_cli_utf8_lead_length(unsigned char first, size_t *out_len) {
    if (out_len == NULL) {
        return OI_ERR_INVAL;
    }
    if (first <= 0x7fU) {
        *out_len = 1;
        return OI_OK;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
        *out_len = 2;
        return OI_OK;
    }
    if (first >= 0xe0U && first <= 0xefU) {
        *out_len = 3;
        return OI_OK;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
        *out_len = 4;
        return OI_OK;
    }
    return OI_ERR_PARSE;
}

oi_status oi_cli_utf8_sequence_length(const unsigned char *data, size_t len,
                                      size_t *out_len) {
    unsigned char first;
    size_t sequence_len;
    oi_status status;

    if (data == NULL || out_len == NULL) {
        return OI_ERR_INVAL;
    }
    if (len == 0) {
        return OI_ERR_PARSE;
    }

    first = data[0];
    status = oi_cli_utf8_lead_length(first, &sequence_len);
    if (status != OI_OK) {
        return status;
    }
    if (sequence_len == 1) {
        *out_len = 1;
        return OI_OK;
    }

    if (len < sequence_len) {
        return OI_ERR_PARSE;
    }
    if (!is_continuation(data[1])) {
        return OI_ERR_PARSE;
    }
    if (sequence_len >= 3 && !is_continuation(data[2])) {
        return OI_ERR_PARSE;
    }
    if (sequence_len == 4 && !is_continuation(data[3])) {
        return OI_ERR_PARSE;
    }

    if (first == 0xe0U && data[1] < 0xa0U) {
        return OI_ERR_PARSE;
    }
    if (first == 0xedU && data[1] > 0x9fU) {
        return OI_ERR_PARSE;
    }
    if (first == 0xf0U && data[1] < 0x90U) {
        return OI_ERR_PARSE;
    }
    if (first == 0xf4U && data[1] > 0x8fU) {
        return OI_ERR_PARSE;
    }

    *out_len = sequence_len;
    return OI_OK;
}

oi_status oi_cli_utf8_validate(const char *data, size_t len) {
    size_t offset = 0;

    if (data == NULL && len != 0) {
        return OI_ERR_INVAL;
    }
    while (offset < len) {
        size_t sequence_len;
        oi_status status = oi_cli_utf8_sequence_length(
            (const unsigned char *)data + offset, len - offset, &sequence_len);
        if (status != OI_OK) {
            return status;
        }
        offset += sequence_len;
    }
    return OI_OK;
}

oi_status oi_cli_utf8_next_boundary(const char *data, size_t len,
                                    size_t offset, size_t *out_offset) {
    size_t sequence_len;
    oi_status status;

    if (out_offset == NULL || offset > len || (data == NULL && len != 0)) {
        return OI_ERR_INVAL;
    }
    if (offset == len) {
        return OI_ERR_INVAL;
    }

    status = oi_cli_utf8_sequence_length(
        (const unsigned char *)data + offset, len - offset, &sequence_len);
    if (status != OI_OK) {
        return status;
    }
    *out_offset = offset + sequence_len;
    return OI_OK;
}

oi_status oi_cli_utf8_previous_boundary(const char *data, size_t len,
                                        size_t offset, size_t *out_offset) {
    size_t start;
    size_t sequence_len;
    oi_status status;

    if (out_offset == NULL || offset > len || (data == NULL && len != 0)) {
        return OI_ERR_INVAL;
    }
    if (offset == 0) {
        return OI_ERR_INVAL;
    }

    start = offset - 1;
    while (start > 0 && is_continuation((unsigned char)data[start])) {
        start--;
    }

    status = oi_cli_utf8_sequence_length(
        (const unsigned char *)data + start, offset - start, &sequence_len);
    if (status != OI_OK) {
        return status;
    }
    if (start + sequence_len != offset) {
        return OI_ERR_PARSE;
    }

    *out_offset = start;
    return OI_OK;
}

oi_status oi_cli_utf8_decode(const char *data, size_t len,
                             uint32_t *out_codepoint) {
    const unsigned char *bytes = (const unsigned char *)data;

    if (data == NULL || out_codepoint == NULL || len < 1 || len > 4) {
        return OI_ERR_INVAL;
    }
    switch (len) {
    case 1:
        *out_codepoint = bytes[0];
        break;
    case 2:
        *out_codepoint = ((uint32_t)(bytes[0] & 0x1fU) << 6) |
                         (uint32_t)(bytes[1] & 0x3fU);
        break;
    case 3:
        *out_codepoint = ((uint32_t)(bytes[0] & 0x0fU) << 12) |
                         ((uint32_t)(bytes[1] & 0x3fU) << 6) |
                         (uint32_t)(bytes[2] & 0x3fU);
        break;
    default:
        *out_codepoint = ((uint32_t)(bytes[0] & 0x07U) << 18) |
                         ((uint32_t)(bytes[1] & 0x3fU) << 12) |
                         ((uint32_t)(bytes[2] & 0x3fU) << 6) |
                         (uint32_t)(bytes[3] & 0x3fU);
        break;
    }
    return OI_OK;
}

static int in_range(uint32_t codepoint, uint32_t low, uint32_t high) {
    return codepoint >= low && codepoint <= high;
}

size_t oi_cli_utf8_codepoint_width(uint32_t codepoint) {
    /* C0 controls and DEL: defensive -- rendering these literally would
     * corrupt the display, and they should never advance the column. */
    if (codepoint <= 0x1fU || codepoint == 0x7fU) {
        return 0;
    }
    /* Combining marks and other zero-width formatting code points. */
    if (in_range(codepoint, 0x0300, 0x036f) ||
        in_range(codepoint, 0x200b, 0x200f) ||
        in_range(codepoint, 0xfe00, 0xfe0f) ||
        in_range(codepoint, 0xfe20, 0xfe2f) ||
        in_range(codepoint, 0x20d0, 0x20ff)) {
        return 0;
    }
    /* East Asian Wide/Fullwidth: a pared-down, documented subset covering
     * common CJK/Hangul ranges -- not a full Unicode UAX#11 table. */
    if (in_range(codepoint, 0x1100, 0x115f) ||
        in_range(codepoint, 0x2e80, 0x303e) ||
        in_range(codepoint, 0x3041, 0x33ff) ||
        in_range(codepoint, 0x3400, 0x4dbf) ||
        in_range(codepoint, 0x4e00, 0x9fff) ||
        in_range(codepoint, 0xa000, 0xa4cf) ||
        in_range(codepoint, 0xac00, 0xd7a3) ||
        in_range(codepoint, 0xf900, 0xfaff) ||
        in_range(codepoint, 0xff00, 0xff60) ||
        in_range(codepoint, 0xffe0, 0xffe6) ||
        in_range(codepoint, 0x20000, 0x2fffd) ||
        in_range(codepoint, 0x30000, 0x3fffd)) {
        return 2;
    }
    return 1;
}
