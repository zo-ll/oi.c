#ifndef OI_CLI_UTF8_H
#define OI_CLI_UTF8_H

#include <stddef.h>

#include "oi/status.h"

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
