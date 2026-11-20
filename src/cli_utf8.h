#ifndef OI_CLI_UTF8_H
#define OI_CLI_UTF8_H

#include <stddef.h>
#include <stdint.h>

#include "oi/status.h"

oi_status oi_cli_utf8_lead_length(unsigned char first, size_t *out_len);

oi_status oi_cli_utf8_sequence_length(const unsigned char *data, size_t len,
                                      size_t *out_len);

oi_status oi_cli_utf8_validate(const char *data, size_t len);

oi_status oi_cli_utf8_next_boundary(const char *data, size_t len,
                                    size_t offset, size_t *out_offset);
oi_status oi_cli_utf8_previous_boundary(const char *data, size_t len,
                                        size_t offset, size_t *out_offset);

oi_status oi_cli_utf8_decode(const char *data, size_t len,
                             uint32_t *out_codepoint);

size_t oi_cli_utf8_codepoint_width(uint32_t codepoint);

#endif
