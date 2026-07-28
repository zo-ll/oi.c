#include "cli_sessions.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cli_session_metadata.h"
#include "cli_session_metadata_store.h"

#define OI_CLI_SESSION_ID_CAP 64U

static oi_status join_path(const char *left, const char *right,
                           char **out_path) {
    size_t left_len;
    size_t right_len;
    int needs_separator;
    size_t len;
    char *path;

    if (left == NULL || left[0] == '\0' || right == NULL ||
        right[0] == '\0' || out_path == NULL) {
        return OI_ERR_INVAL;
    }
    left_len = strlen(left);
    right_len = strlen(right);
    needs_separator = left[left_len - 1] != '/';
    if (right_len > (size_t)-1 - left_len - (size_t)needs_separator - 1) {
        return OI_ERR_NOMEM;
    }
    len = left_len + (size_t)needs_separator + right_len;
    path = malloc(len + 1);
    if (path == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(path, left, left_len);
    if (needs_separator) {
        path[left_len] = '/';
    }
    memcpy(path + left_len + (size_t)needs_separator, right, right_len);
    path[len] = '\0';
    *out_path = path;
    return OI_OK;
}

static oi_status ensure_directory(const char *path) {
    char *copy;
    char *cursor;

    if (path == NULL || path[0] == '\0') {
        return OI_ERR_INVAL;
    }
    copy = strdup(path);
    if (copy == NULL) {
        return OI_ERR_NOMEM;
    }
    cursor = copy + (copy[0] == '/' ? 1 : 0);
    for (;;) {
        char saved;
        struct stat info;

        while (*cursor != '/' && *cursor != '\0') {
            cursor++;
        }
        saved = *cursor;
        *cursor = '\0';
        if (copy[0] != '\0') {
            if (mkdir(copy, 0700) != 0 && errno != EEXIST) {
                free(copy);
                return OI_ERR_IO;
            }
            if (stat(copy, &info) != 0 || !S_ISDIR(info.st_mode)) {
                free(copy);
                return OI_ERR_IO;
            }
        }
        *cursor = saved;
        if (saved == '\0') {
            break;
        }
        cursor++;
    }
    free(copy);
    return OI_OK;
}

static oi_status home_root(char **out_root) {
    const char *home = getenv("HOME");
    char *base = NULL;
    oi_status status;

    if (home == NULL || home[0] == '\0') {
        return OI_ERR_NOTFOUND;
    }
    status = join_path(home, ".local/state", &base);
    if (status == OI_OK) {
        status = join_path(base, "oi/sessions", out_root);
    }
    free(base);
    return status;
}

void oi_cli_session_location_init(
    struct oi_cli_session_location *location) {
    if (location == NULL) {
        return;
    }
    location->id = NULL;
    location->directory = NULL;
    location->history_path = NULL;
    location->metadata_path = NULL;
}

void oi_cli_session_location_free(
    struct oi_cli_session_location *location) {
    if (location == NULL) {
        return;
    }
    free(location->id);
    free(location->directory);
    free(location->history_path);
    free(location->metadata_path);
    oi_cli_session_location_init(location);
}

oi_status oi_cli_session_metadata_path_for_log(
    const char *log_path, int is_private_directory,
    char **out_metadata_path) {
    if (log_path == NULL || out_metadata_path == NULL) {
        return OI_ERR_INVAL;
    }
    if (is_private_directory) {
        static const char name[] = "metadata.json";
        const char *slash = strrchr(log_path, '/');
        size_t dir_len = slash == NULL ? 0 : (size_t)(slash - log_path) + 1;
        char *path = malloc(dir_len + sizeof name);
        if (path == NULL) {
            return OI_ERR_NOMEM;
        }
        if (dir_len > 0) {
            memcpy(path, log_path, dir_len);
        }
        memcpy(path + dir_len, name, sizeof name);
        *out_metadata_path = path;
        return OI_OK;
    }
    {
        static const char suffix[] = ".oilog";
        static const char meta_suffix[] = ".metadata.json";
        size_t suffix_len = sizeof suffix - 1;
        size_t log_len = strlen(log_path);
        size_t stem_len;
        char *path;
        if (log_len <= suffix_len ||
            strcmp(log_path + log_len - suffix_len, suffix) != 0) {
            return OI_ERR_INVAL;
        }
        stem_len = log_len - suffix_len;
        path = malloc(stem_len + sizeof meta_suffix);
        if (path == NULL) {
            return OI_ERR_NOMEM;
        }
        memcpy(path, log_path, stem_len);
        memcpy(path + stem_len, meta_suffix, sizeof meta_suffix);
        *out_metadata_path = path;
        return OI_OK;
    }
}

oi_status oi_cli_sessions_default_root(char **out_root) {
    if (out_root == NULL) {
        return OI_ERR_INVAL;
    }
    {
        const char *xdg = getenv("XDG_STATE_HOME");
        if (xdg != NULL && xdg[0] == '/') {
            return join_path(xdg, "oi/sessions", out_root);
        }
    }
    return home_root(out_root);
}

oi_status oi_cli_session_location_create(
    const char *root_override,
    struct oi_cli_session_location *out_location) {
    struct oi_cli_session_location location;
    char *root = NULL;
    struct tm time_parts;
    time_t now;
    unsigned attempt;
    oi_status status;

    if (out_location == NULL ||
        (root_override != NULL && root_override[0] == '\0')) {
        return OI_ERR_INVAL;
    }
    oi_cli_session_location_init(&location);
    if (root_override != NULL) {
        root = strdup(root_override);
        status = root == NULL ? OI_ERR_NOMEM : OI_OK;
    } else {
        status = oi_cli_sessions_default_root(&root);
    }
    if (status != OI_OK) {
        return status;
    }
    status = ensure_directory(root);
    if (status != OI_OK) {
        free(root);
        return status;
    }

    now = time(NULL);
    if (now == (time_t)-1 || localtime_r(&now, &time_parts) == NULL) {
        free(root);
        return OI_ERR_IO;
    }
    for (attempt = 0; attempt < 1000; attempt++) {
        char id[OI_CLI_SESSION_ID_CAP];
        int id_len = snprintf(
            id, sizeof id, "%04d%02d%02d-%02d%02d%02d-%ld-%03u",
            time_parts.tm_year + 1900, time_parts.tm_mon + 1,
            time_parts.tm_mday, time_parts.tm_hour, time_parts.tm_min,
            time_parts.tm_sec, (long)getpid(), attempt);

        if (id_len < 0 || (size_t)id_len >= sizeof id) {
            status = OI_ERR_IO;
            break;
        }
        free(location.id);
        free(location.directory);
        location.id = strdup(id);
        location.directory = NULL;
        if (location.id == NULL) {
            status = OI_ERR_NOMEM;
            break;
        }
        status = join_path(root, id, &location.directory);
        if (status != OI_OK) {
            break;
        }
        if (mkdir(location.directory, 0700) == 0) {
            status = join_path(location.directory, "history.oilog",
                               &location.history_path);
            break;
        }
        if (errno != EEXIST) {
            status = OI_ERR_IO;
            break;
        }
    }
    free(root);
    if (status != OI_OK || location.history_path == NULL) {
        oi_cli_session_location_free(&location);
        return status == OI_OK ? OI_ERR_EXISTS : status;
    }
    status = oi_cli_session_metadata_path_for_log(
        location.history_path, 1, &location.metadata_path);
    if (status != OI_OK) {
        oi_cli_session_location_free(&location);
        return status;
    }
    *out_location = location;
    return OI_OK;
}

void oi_cli_session_restore_init(struct oi_cli_session_restore *restore) {
    if (restore != NULL) {
        memset(restore, 0, sizeof *restore);
    }
}

void oi_cli_session_restore_free(struct oi_cli_session_restore *restore) {
    if (restore == NULL) {
        return;
    }
    oi_cli_string_free(&restore->model);
    oi_cli_string_free(&restore->cwd);
    memset(restore, 0, sizeof *restore);
}

static int directory_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static oi_status append_setting_record(
    struct oi_cli_history_store *store,
    struct oi_cli_history_replay_state *state,
    enum oi_cli_history_session_setting_field field, const char *value,
    size_t value_len) {
    struct oi_cli_history_record record;
    oi_status status;
    oi_cli_history_record_init(&record);
    status = oi_cli_history_record_set_session_setting(
        &record, state->next_record_id, field, value, value_len);
    if (status == OI_OK) {
        status = oi_cli_history_store_append(store, &record, state);
    }
    oi_cli_history_record_free(&record);
    return status;
}

static void refresh_metadata(const char *metadata_path,
                             const char *session_id, const char *model,
                             size_t model_len, const char *cwd,
                             size_t cwd_len, int64_t created_at,
                             FILE *diagnostics) {
    struct oi_cli_session_metadata metadata;
    oi_status status;
    time_t now = time(NULL);
    int64_t updated_at = now == (time_t)-1 ? created_at : (int64_t)now;

    oi_cli_session_metadata_init(&metadata);
    status = oi_cli_session_metadata_set(
        &metadata, session_id, strlen(session_id), model, model_len, cwd,
        cwd_len, created_at, updated_at < created_at ? created_at
                                                     : updated_at);
    if (status == OI_OK) {
        status = oi_cli_session_metadata_store_write(metadata_path,
                                                      &metadata);
    }
    if (status != OI_OK && diagnostics != NULL) {
        fprintf(diagnostics,
                "oi: failed to write session metadata (status=%d)\n",
                (int)status);
    }
    oi_cli_session_metadata_free(&metadata);
}

oi_status oi_cli_session_restore_settings(
    struct oi_cli_history_store *store,
    struct oi_cli_history_replay_state *state, const char *metadata_path,
    const char *session_id, int is_new_session, const char *explicit_model,
    const char *default_model, const char *default_cwd, FILE *diagnostics,
    struct oi_cli_session_restore *out_restore) {
    struct oi_cli_session_metadata meta;
    int metadata_valid = 0;
    int64_t created_at;
    time_t now;
    const char *resolved_model;
    size_t resolved_model_len;
    const char *resolved_cwd;
    size_t resolved_cwd_len;
    oi_status status;

    if (store == NULL || state == NULL || metadata_path == NULL ||
        session_id == NULL || default_model == NULL ||
        default_model[0] == '\0' || default_cwd == NULL ||
        default_cwd[0] == '\0' || out_restore == NULL) {
        return OI_ERR_INVAL;
    }
    oi_cli_session_restore_init(out_restore);
    oi_cli_session_metadata_init(&meta);

    if (!is_new_session) {
        oi_status read_status =
            oi_cli_session_metadata_store_read(metadata_path, &meta);
        if (read_status == OI_OK) {
            if (meta.session_id.len == strlen(session_id) &&
                memcmp(meta.session_id.data, session_id,
                       meta.session_id.len) == 0) {
                metadata_valid = 1;
            } else {
                out_restore->metadata_missing_or_corrupt = 1;
                if (diagnostics != NULL) {
                    fprintf(diagnostics,
                            "oi: session metadata belongs to a different "
                            "session id; rebuilding from history\n");
                }
            }
        } else if (read_status == OI_ERR_NOTFOUND) {
            out_restore->metadata_missing_or_corrupt = 1;
            if (diagnostics != NULL) {
                fprintf(diagnostics,
                        "oi: session metadata missing; rebuilding from "
                        "history\n");
            }
        } else if (read_status == OI_ERR_PARSE) {
            out_restore->metadata_missing_or_corrupt = 1;
            if (diagnostics != NULL) {
                fprintf(diagnostics,
                        "oi: session metadata malformed; rebuilding from "
                        "history\n");
            }
        } else {
            oi_cli_session_metadata_free(&meta);
            return read_status;
        }
    }

    if (explicit_model != NULL) {
        resolved_model = explicit_model;
        resolved_model_len = strlen(explicit_model);
    } else if (metadata_valid) {
        resolved_model = meta.model.data;
        resolved_model_len = meta.model.len;
    } else if (state->last_model.data != NULL) {
        resolved_model = state->last_model.data;
        resolved_model_len = state->last_model.len;
    } else {
        resolved_model = default_model;
        resolved_model_len = strlen(default_model);
    }

    if (metadata_valid) {
        resolved_cwd = meta.cwd.data;
        resolved_cwd_len = meta.cwd.len;
    } else if (state->last_cwd.data != NULL) {
        resolved_cwd = state->last_cwd.data;
        resolved_cwd_len = state->last_cwd.len;
    } else {
        resolved_cwd = default_cwd;
        resolved_cwd_len = strlen(default_cwd);
    }

    if (!directory_exists(resolved_cwd)) {
        out_restore->cwd_fallback_applied = 1;
        if (diagnostics != NULL) {
            fprintf(diagnostics,
                    "oi: session working directory \"%s\" is no longer "
                    "available; using \"%s\"\n",
                    resolved_cwd, default_cwd);
        }
        resolved_cwd = default_cwd;
        resolved_cwd_len = strlen(default_cwd);
    }
    if (chdir(resolved_cwd) != 0) {
        oi_cli_session_metadata_free(&meta);
        return OI_ERR_IO;
    }

    now = time(NULL);
    created_at = metadata_valid ? meta.created_at
                                : (now == (time_t)-1 ? 0 : (int64_t)now);

    /* Copy into out_restore now: appending durable records below refreshes
     * `state` in place (freeing its previous last_model/last_cwd), which
     * resolved_model/resolved_cwd may currently alias. */
    status = oi_cli_string_set(&out_restore->model, resolved_model,
                               resolved_model_len);
    if (status == OI_OK) {
        status = oi_cli_string_set(&out_restore->cwd, resolved_cwd,
                                   resolved_cwd_len);
    }
    oi_cli_session_metadata_free(&meta);
    if (status != OI_OK) {
        return status;
    }

    if (is_new_session) {
        status = append_setting_record(
            store, state, OI_CLI_HISTORY_SESSION_SETTING_MODEL,
            out_restore->model.data, out_restore->model.len);
        if (status == OI_OK) {
            status = append_setting_record(
                store, state, OI_CLI_HISTORY_SESSION_SETTING_CWD,
                out_restore->cwd.data, out_restore->cwd.len);
        }
    } else {
        int model_changed =
            state->last_model.data == NULL ||
            state->last_model.len != out_restore->model.len ||
            memcmp(state->last_model.data, out_restore->model.data,
                   out_restore->model.len) != 0;
        int cwd_changed =
            state->last_cwd.data == NULL ||
            state->last_cwd.len != out_restore->cwd.len ||
            memcmp(state->last_cwd.data, out_restore->cwd.data,
                   out_restore->cwd.len) != 0;
        status = OI_OK;
        if (model_changed) {
            status = append_setting_record(
                store, state, OI_CLI_HISTORY_SESSION_SETTING_MODEL,
                out_restore->model.data, out_restore->model.len);
        }
        if (status == OI_OK && cwd_changed) {
            status = append_setting_record(
                store, state, OI_CLI_HISTORY_SESSION_SETTING_CWD,
                out_restore->cwd.data, out_restore->cwd.len);
        }
    }
    if (status != OI_OK) {
        return status;
    }

    refresh_metadata(metadata_path, session_id, out_restore->model.data,
                     out_restore->model.len, out_restore->cwd.data,
                     out_restore->cwd.len, created_at, diagnostics);
    return OI_OK;
}

oi_status oi_cli_session_apply_setting(
    struct oi_cli_history_store *store,
    struct oi_cli_history_replay_state *state, const char *metadata_path,
    const char *session_id,
    enum oi_cli_history_session_setting_field field, const char *value,
    size_t value_len) {
    oi_status status;
    int64_t created_at;
    time_t now;

    if (store == NULL || state == NULL || metadata_path == NULL ||
        session_id == NULL || value == NULL) {
        return OI_ERR_INVAL;
    }
    status = append_setting_record(store, state, field, value, value_len);
    if (status != OI_OK) {
        return status;
    }

    now = time(NULL);
    created_at = now == (time_t)-1 ? 0 : (int64_t)now;
    {
        struct oi_cli_session_metadata existing;
        oi_cli_session_metadata_init(&existing);
        if (oi_cli_session_metadata_store_read(metadata_path, &existing) ==
                OI_OK &&
            existing.session_id.len == strlen(session_id) &&
            memcmp(existing.session_id.data, session_id,
                   existing.session_id.len) == 0) {
            created_at = existing.created_at;
        }
        oi_cli_session_metadata_free(&existing);
    }

    refresh_metadata(
        metadata_path, session_id,
        field == OI_CLI_HISTORY_SESSION_SETTING_MODEL
            ? value
            : state->last_model.data,
        field == OI_CLI_HISTORY_SESSION_SETTING_MODEL
            ? value_len
            : state->last_model.len,
        field == OI_CLI_HISTORY_SESSION_SETTING_CWD ? value
                                                     : state->last_cwd.data,
        field == OI_CLI_HISTORY_SESSION_SETTING_CWD ? value_len
                                                     : state->last_cwd.len,
        created_at, NULL);
    return OI_OK;
}
