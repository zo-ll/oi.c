#ifndef OI_CLI_SESSION_METADATA_STORE_H
#define OI_CLI_SESSION_METADATA_STORE_H

#include "cli_session_metadata.h"
#include "oi/status.h"

/* OI_ERR_NOTFOUND if missing; OI_ERR_PARSE for malformed JSON, a bad
 * version, oversized fields, an empty file, or a file over
 * OI_CLI_SESSION_METADATA_MAX_FILE. */
oi_status oi_cli_session_metadata_store_read(
    const char *metadata_path, struct oi_cli_session_metadata *out_metadata);

/*
 * Writes to "<metadata_path>.tmp" (a fixed name is safe: sesslog.c
 * already takes an exclusive flock on the co-located history.oilog for
 * the session's lifetime, so a session directory is single-owner),
 * 0600, then rename()s over metadata_path. Cleans up the temp file on
 * every failure path -- including a failed rename -- without touching a
 * pre-existing valid metadata_path. No fsync: matches PLAN.md's stated
 * "no fsync by default" stance for the append-only log (crash-safe
 * against process crash, not power loss); history.oilog stays
 * authoritative regardless of what happens to this file.
 */
oi_status oi_cli_session_metadata_store_write(
    const char *metadata_path, const struct oi_cli_session_metadata *metadata);

#endif
