#ifndef OI_CLI_HISTORY_CODEC_H
#define OI_CLI_HISTORY_CODEC_H

#include <stddef.h>

#include "cli_history.h"
#include "oi/status.h"

oi_status oi_cli_history_record_encode(
    const struct oi_cli_history_record *record, char **out_json,
    size_t *out_json_len);

oi_status oi_cli_history_record_decode(
    const char *json, size_t json_len,
    struct oi_cli_history_record *out_record);

#endif
