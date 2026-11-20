#ifndef OI_CLI_SESSION_METADATA_CODEC_H
#define OI_CLI_SESSION_METADATA_CODEC_H

#include <stddef.h>

#include "cli_session_metadata.h"
#include "oi/status.h"

oi_status oi_cli_session_metadata_encode(
    const struct oi_cli_session_metadata *metadata, char **out_json,
    size_t *out_json_len);

oi_status oi_cli_session_metadata_decode(
    const char *json, size_t json_len,
    struct oi_cli_session_metadata *out_metadata);

#endif
