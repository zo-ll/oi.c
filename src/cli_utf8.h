#ifndef OI_CLI_UTF8_H
#define OI_CLI_UTF8_H

#include <stddef.h>

#include "oi/status.h"

/*
 * Classifies `first` as an ASCII byte (1), or a multi-byte UTF-8 lead byte
 * (2/3/4), without looking at or requiring any continuation bytes. Returns
 * OI_ERR_PARSE for any byte that can never begin a valid sequence
 * (0x80-0xC1 stray continuation/overlong-lead, 0xF5-0xFF).
 */
oi_status oi_cli_utf8_lead_length(unsigned char first, size_t *out_len);

/*
 * Validates the UTF-8 sequence at the start of `data` and returns its byte
 * length. Embedded U+0000 is valid here; policy about NUL belongs to callers.
 */
oi_status oi_cli_utf8_sequence_length(const unsigned char *data, size_t len,
                                      size_t *out_len);

oi_status oi_cli_utf8_validate(const char *data, size_t len);

/*
 * Moves from one code point boundary to the adjacent boundary. `offset` must
 * be a boundary within the input. Moving beyond either end is invalid.
 */
oi_status oi_cli_utf8_next_boundary(const char *data, size_t len,
                                    size_t offset, size_t *out_offset);
oi_status oi_cli_utf8_previous_boundary(const char *data, size_t len,
                                        size_t offset, size_t *out_offset);

#endif
