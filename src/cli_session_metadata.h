#ifndef OI_CLI_SESSION_METADATA_H
#define OI_CLI_SESSION_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "cli_history.h"
#include "cli_message.h"
#include "oi/status.h"

/*
 * Rebuildable selector metadata for one durable session -- never
 * authoritative (history.oilog is; see docs/REPL_PLAN.md's storage
 * model). Deleting or corrupting this is recoverable by replaying
 * history. No API keys, auth headers, or other secrets are ever in
 * scope for this struct -- enforced structurally: nothing here has
 * access to them.
 */
#define OI_CLI_SESSION_METADATA_SCHEMA_VERSION 1u
#define OI_CLI_SESSION_METADATA_MAX_SESSION_ID 512u
/* Hard ceiling on the whole encoded file when reading it back. */
#define OI_CLI_SESSION_METADATA_MAX_FILE (64u * 1024u)

struct oi_cli_session_metadata {
    unsigned int version;
    struct oi_cli_string session_id;
    struct oi_cli_string model;
    struct oi_cli_string cwd; /* bounded by OI_CLI_HISTORY_MAX_SETTING_VALUE,
                                 shared with the durable setting records */
    int64_t created_at;      /* Unix epoch seconds */
    int64_t updated_at;
};

void oi_cli_session_metadata_init(struct oi_cli_session_metadata *metadata);
void oi_cli_session_metadata_free(struct oi_cli_session_metadata *metadata);
int oi_cli_session_metadata_is_valid(
    const struct oi_cli_session_metadata *metadata);

oi_status oi_cli_session_metadata_set(
    struct oi_cli_session_metadata *metadata, const char *session_id,
    size_t session_id_len, const char *model, size_t model_len,
    const char *cwd, size_t cwd_len, int64_t created_at, int64_t updated_at);

#endif
