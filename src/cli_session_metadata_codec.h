#ifndef OI_CLI_SESSION_METADATA_CODEC_H
#define OI_CLI_SESSION_METADATA_CODEC_H

#include <stddef.h>

#include "cli_session_metadata.h"
#include "oi/status.h"

/*
 * JSON shape, version 2 (what every write emits):
 * {"version":2,"session_id":"...","model":"...","cwd":"...",
 *  "display_name":"...","created_at":"1785262200",
 *  "updated_at":"1785262260"}
 *
 * Version 1 is the same without `display_name`. `version` is a plain
 * number; `created_at`/`updated_at` are decimal strings (matches the
 * record-ID convention in cli_history_codec.c -- no float-precision
 * risk, and consistent encoding across both codecs).
 *
 * `display_name` is always written, as "" when unset, so a version 2
 * object has exactly seven fields and the decoder can keep checking an
 * exact field count per version rather than tolerating a range.
 */
oi_status oi_cli_session_metadata_encode(
    const struct oi_cli_session_metadata *metadata, char **out_json,
    size_t *out_json_len);

/*
 * Strict: rejects wrong types, a version that is neither 1 nor 2, a
 * field count that does not match the version exactly (so an unknown or
 * misspelled key is still refused rather than ignored), or a decoded
 * struct that fails oi_cli_session_metadata_is_valid.
 *
 * A version 1 object decodes with no display name and is upgraded in
 * memory to the current schema version, so the next write of it emits
 * version 2. Nothing ever rewrites a version 1 file in place purely to
 * migrate it -- metadata is a rebuildable cache, and the upgrade rides
 * along with whatever change was already being persisted.
 */
oi_status oi_cli_session_metadata_decode(
    const char *json, size_t json_len,
    struct oi_cli_session_metadata *out_metadata);

#endif
