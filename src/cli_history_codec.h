#ifndef OI_CLI_HISTORY_CODEC_H
#define OI_CLI_HISTORY_CODEC_H

#include <stddef.h>

#include "cli_history.h"
#include "oi/status.h"

/*
 * Encodes one record as a deterministic JSON object. The returned buffer is
 * heap-owned by the caller and must be freed with free().
 */
oi_status oi_cli_history_record_encode(
    const struct oi_cli_history_record *record, char **out_json,
    size_t *out_json_len);

#endif
