#ifndef OI_CLI_SESSIONS_H
#define OI_CLI_SESSIONS_H

#include <stdio.h>

#include "cli_history.h"
#include "cli_history_replay.h"
#include "cli_history_store.h"
#include "cli_message.h"
#include "oi/status.h"

#define OI_CLI_SESSION_SAFE_ID_MAX_LEN 128u

struct oi_cli_session_location {
    char *id;
    char *directory;
    char *history_path;
    char *metadata_path;
};

void oi_cli_session_location_init(
    struct oi_cli_session_location *location);
void oi_cli_session_location_free(
    struct oi_cli_session_location *location);

oi_status oi_cli_session_location_create(
    const char *root_override,
    struct oi_cli_session_location *out_location);

oi_status oi_cli_sessions_default_root(char **out_root);

oi_status oi_cli_sessions_root(const char *root_override, char **out_root);

oi_status oi_cli_session_history_path(const char *directory,
                                      char **out_history_path);

int oi_cli_session_id_is_safe(const char *id, size_t id_len);

oi_status oi_cli_session_resolve(const char *root, const char *id,
                                 size_t id_len, char **out_directory);

oi_status oi_cli_session_metadata_path_for_log(
    const char *log_path, int is_private_directory,
    char **out_metadata_path);

enum oi_cli_session_lock_state {
    OI_CLI_SESSION_LOCK_FREE = 0,
    OI_CLI_SESSION_LOCK_BUSY,
    OI_CLI_SESSION_LOCK_UNKNOWN
};

struct oi_cli_session_list_entry {
    char *id;
    struct oi_cli_string display_name;
    struct oi_cli_string model;
    struct oi_cli_string cwd;
    int64_t created_at;
    int64_t updated_at;
    int degraded;
    enum oi_cli_session_lock_state lock_state;
};

struct oi_cli_session_list {
    struct oi_cli_session_list_entry *entries;
    size_t len;
    size_t cap;
};

void oi_cli_session_list_init(struct oi_cli_session_list *list);
void oi_cli_session_list_free(struct oi_cli_session_list *list);

oi_status oi_cli_sessions_enumerate(const char *root_override,
                                    struct oi_cli_session_list *out_list);

oi_status oi_cli_session_rename(const char *root_override, const char *id,
                                size_t id_len, const char *new_name,
                                size_t new_name_len,
                                char **out_error_detail);

oi_status oi_cli_session_trash(const char *root_override, const char *id,
                               size_t id_len, const char *current_session_id,
                               char **out_error_detail);
oi_status oi_cli_session_restore_trashed(const char *root_override,
                                         const char *id, size_t id_len,
                                         char **out_error_detail);
oi_status oi_cli_session_delete(const char *root_override, const char *id,
                                size_t id_len, char **out_error_detail);

oi_status oi_cli_sessions_enumerate_trash(
    const char *root_override, struct oi_cli_session_list *out_list);

oi_status oi_cli_session_import(const char *root_override,
                                const char *source_path,
                                size_t source_path_len, char **out_new_id,
                                char **out_error_detail);

enum oi_cli_session_model_origin {
    OI_CLI_SESSION_MODEL_UNKNOWN = 0,
    OI_CLI_SESSION_MODEL_DEFAULT,
    OI_CLI_SESSION_MODEL_EXPLICIT,
    OI_CLI_SESSION_MODEL_HISTORY,
    OI_CLI_SESSION_MODEL_METADATA,
    OI_CLI_SESSION_MODEL_COMMAND
};

struct oi_cli_session_restore {
    struct oi_cli_string model;
    struct oi_cli_string cwd;
    enum oi_cli_session_model_origin model_origin;
    int metadata_missing_or_corrupt;
    int cwd_fallback_applied;
};

void oi_cli_session_restore_init(struct oi_cli_session_restore *restore);
void oi_cli_session_restore_free(struct oi_cli_session_restore *restore);

oi_status oi_cli_session_restore_settings(
    struct oi_cli_history_store *store,
    struct oi_cli_history_replay_state *state, const char *metadata_path,
    const char *session_id, int is_new_session, const char *explicit_model,
    const char *default_model, const char *default_cwd, FILE *diagnostics,
    struct oi_cli_session_restore *out_restore);

oi_status oi_cli_session_apply_setting(
    struct oi_cli_history_store *store,
    struct oi_cli_history_replay_state *state, const char *metadata_path,
    const char *session_id, enum oi_cli_history_session_setting_field field,
    const char *value, size_t value_len);

#endif
