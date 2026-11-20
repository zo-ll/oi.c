#ifndef OI_CLI_SESSION_METADATA_H
#define OI_CLI_SESSION_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "cli_history.h"
#include "cli_message.h"
#include "oi/status.h"

#define OI_CLI_SESSION_METADATA_SCHEMA_VERSION 2u
#define OI_CLI_SESSION_METADATA_MAX_SESSION_ID 512u
#define OI_CLI_SESSION_METADATA_MAX_FILE (64u * 1024u)
#define OI_CLI_SESSION_METADATA_MAX_DISPLAY_NAME 256u

struct oi_cli_session_metadata {
    unsigned int version;
    struct oi_cli_string session_id;
    struct oi_cli_string model;
    struct oi_cli_string cwd;
    struct oi_cli_string display_name;
    int64_t created_at;
    int64_t updated_at;
};

void oi_cli_session_metadata_init(struct oi_cli_session_metadata *metadata);
void oi_cli_session_metadata_free(struct oi_cli_session_metadata *metadata);
int oi_cli_session_metadata_is_valid(
    const struct oi_cli_session_metadata *metadata);

oi_status oi_cli_session_metadata_set(
    struct oi_cli_session_metadata *metadata, const char *session_id,
    size_t session_id_len, const char *model, size_t model_len,
    const char *cwd, size_t cwd_len, const char *display_name,
    size_t display_name_len, int64_t created_at, int64_t updated_at);

#endif
