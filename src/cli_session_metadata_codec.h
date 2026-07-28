#ifndef OI_CLI_SESSION_METADATA_CODEC_H
#define OI_CLI_SESSION_METADATA_CODEC_H

#include <stddef.h>

#include "cli_session_metadata.h"
#include "oi/status.h"

/*
 * JSON shape:
 * {"version":1,"session_id":"...","model":"...","cwd":"...",
 *  "created_at":"1785262200","updated_at":"1785262260"}
 * `version` is a plain number; `created_at`/`updated_at` are decimal
 * strings (matches the record-ID convention in cli_history_codec.c --
 * no float-precision risk, and consistent encoding across both codecs).
 */
oi_status oi_cli_session_metadata_encode(
    const struct oi_cli_session_metadata *metadata, char **out_json,
    size_t *out_json_len);

/* Strict: rejects unknown/missing/extra fields, wrong types, a version
 * other than OI_CLI_SESSION_METADATA_SCHEMA_VERSION, or a decoded
 * metadata struct that fails oi_cli_session_metadata_is_valid. */
oi_status oi_cli_session_metadata_decode(
    const char *json, size_t json_len,
    struct oi_cli_session_metadata *out_metadata);

#endif
