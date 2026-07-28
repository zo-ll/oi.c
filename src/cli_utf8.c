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
