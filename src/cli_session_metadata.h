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
/*
 * Version 2 adds `display_name`. Additive only: a version 1 file still
 * decodes (with no display name), and every write from here on emits
 * version 2 -- see cli_session_metadata_codec.h for the wire shapes.
 */
#define OI_CLI_SESSION_METADATA_SCHEMA_VERSION 2u
#define OI_CLI_SESSION_METADATA_MAX_SESSION_ID 512u
/* Hard ceiling on the whole encoded file when reading it back. */
#define OI_CLI_SESSION_METADATA_MAX_FILE (64u * 1024u)
/* A display name is a label for a selector row, not a path or an id. */
#define OI_CLI_SESSION_METADATA_MAX_DISPLAY_NAME 256u

struct oi_cli_session_metadata {
    unsigned int version;
    struct oi_cli_string session_id;
    struct oi_cli_string model;
    struct oi_cli_string cwd; /* bounded by OI_CLI_HISTORY_MAX_SETTING_VALUE,
                                 shared with the durable setting records */
    /*
     * Optional user-chosen label, shown by /session list in place of the
     * id. `len == 0` means unset, which is the normal state -- callers
     * fall back to the id. Bounded by MAX_DISPLAY_NAME and guaranteed
     * free of control bytes, because it is echoed straight to a terminal.
     */
    struct oi_cli_string display_name;
    int64_t created_at; /* Unix epoch seconds */
    int64_t updated_at;
};

void oi_cli_session_metadata_init(struct oi_cli_session_metadata *metadata);
void oi_cli_session_metadata_free(struct oi_cli_session_metadata *metadata);
int oi_cli_session_metadata_is_valid(
    const struct oi_cli_session_metadata *metadata);

/*
 * Replaces `metadata` wholesale, or leaves it untouched on failure.
 *
 * `display_name` may be NULL (or `display_name_len` 0) for "unset". A
 * non-empty one is rejected with OI_ERR_INVAL when it exceeds
 * MAX_DISPLAY_NAME or contains a control byte: it reaches a terminal
 * unescaped, so refusing it here keeps every consumer -- /session list
 * included -- from having to sanitize.
 */
oi_status oi_cli_session_metadata_set(
    struct oi_cli_session_metadata *metadata, const char *session_id,
    size_t session_id_len, const char *model, size_t model_len,
    const char *cwd, size_t cwd_len, const char *display_name,
    size_t display_name_len, int64_t created_at, int64_t updated_at);

#endif
