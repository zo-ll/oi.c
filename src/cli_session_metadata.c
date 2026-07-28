#include "cli_session_metadata.h"

#include <string.h>

void oi_cli_session_metadata_init(struct oi_cli_session_metadata *metadata) {
    if (metadata != NULL) {
        memset(metadata, 0, sizeof *metadata);
    }
}

void oi_cli_session_metadata_free(struct oi_cli_session_metadata *metadata) {
    if (metadata == NULL) {
        return;
    }
    oi_cli_string_free(&metadata->session_id);
    oi_cli_string_free(&metadata->model);
    oi_cli_string_free(&metadata->cwd);
    memset(metadata, 0, sizeof *metadata);
}

int oi_cli_session_metadata_is_valid(
    const struct oi_cli_session_metadata *metadata) {
    if (metadata == NULL ||
        metadata->version != OI_CLI_SESSION_METADATA_SCHEMA_VERSION) {
        return 0;
    }
    if (metadata->session_id.data == NULL || metadata->session_id.len == 0 ||
        metadata->session_id.len > OI_CLI_SESSION_METADATA_MAX_SESSION_ID) {
        return 0;
    }
    if (metadata->model.data == NULL || metadata->model.len == 0 ||
        metadata->model.len > OI_CLI_HISTORY_MAX_SETTING_VALUE) {
        return 0;
    }
    if (metadata->cwd.data == NULL || metadata->cwd.len == 0 ||
        metadata->cwd.len > OI_CLI_HISTORY_MAX_SETTING_VALUE) {
        return 0;
    }
    if (metadata->created_at < 0 || metadata->updated_at < 0 ||
        metadata->updated_at < metadata->created_at) {
        return 0;
    }
    return 1;
}

oi_status oi_cli_session_metadata_set(
    struct oi_cli_session_metadata *metadata, const char *session_id,
    size_t session_id_len, const char *model, size_t model_len,
    const char *cwd, size_t cwd_len, int64_t created_at,
    int64_t updated_at) {
    struct oi_cli_session_metadata replacement;
    oi_status st;

    if (metadata == NULL) {
        return OI_ERR_INVAL;
    }
    oi_cli_session_metadata_init(&replacement);
    replacement.version = OI_CLI_SESSION_METADATA_SCHEMA_VERSION;
    replacement.created_at = created_at;
    replacement.updated_at = updated_at;
    st = oi_cli_string_set(&replacement.session_id, session_id,
                           session_id_len);
    if (st == OI_OK) {
        st = oi_cli_string_set(&replacement.model, model, model_len);
    }
    if (st == OI_OK) {
        st = oi_cli_string_set(&replacement.cwd, cwd, cwd_len);
    }
    if (st == OI_OK && !oi_cli_session_metadata_is_valid(&replacement)) {
        st = OI_ERR_INVAL;
    }
    if (st != OI_OK) {
        oi_cli_session_metadata_free(&replacement);
        return st;
    }
    oi_cli_session_metadata_free(metadata);
    *metadata = replacement;
    return OI_OK;
}
