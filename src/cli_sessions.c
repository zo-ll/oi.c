#include "cli_sessions.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cli_session_metadata.h"
#include "cli_session_metadata_store.h"
#include "oi/sesslog.h"

#define OI_CLI_SESSION_ID_CAP 64U

/* The one file name every private session directory holds its
 * authoritative history in (docs/REPL_PLAN.md's storage model). */
static const char session_history_name[] = "history.oilog";

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

int oi_cli_session_id_is_safe(const char *id, size_t id_len) {
    size_t index;

    if (id == NULL || id_len == 0 ||
        id_len > OI_CLI_SESSION_SAFE_ID_MAX_LEN) {
        return 0;
    }
    for (index = 0; index < id_len; index++) {
        /* Unsigned so the comparisons do not depend on whether plain char
         * is signed on this target; a high-bit byte must fail either way. */
        unsigned char byte = (unsigned char)id[index];
        int allowed = (byte >= 'A' && byte <= 'Z') ||
                      (byte >= 'a' && byte <= 'z') ||
                      (byte >= '0' && byte <= '9') || byte == '_' ||
                      byte == '-';
        if (!allowed) {
            return 0;
        }
    }
    return 1;
}

oi_status oi_cli_session_resolve(const char *root, const char *id,
                                 size_t id_len, char **out_directory) {
    char terminated[OI_CLI_SESSION_SAFE_ID_MAX_LEN + 1];
    char *directory = NULL;
    struct stat info;
    oi_status status;

    if (root == NULL || root[0] == '\0' || out_directory == NULL ||
        !oi_cli_session_id_is_safe(id, id_len)) {
        return OI_ERR_INVAL;
    }
    /* Bounded by the id_len ceiling oi_cli_session_id_is_safe just
     * enforced, so this cannot overrun `terminated`. */
    memcpy(terminated, id, id_len);
    terminated[id_len] = '\0';

    status = join_path(root, terminated, &directory);
    if (status != OI_OK) {
        return status;
    }
    if (lstat(directory, &info) != 0) {
        status = (errno == ENOENT || errno == ENOTDIR) ? OI_ERR_NOTFOUND
                                                       : OI_ERR_IO;
        free(directory);
        return status;
    }
    /* S_ISDIR on the lstat result: a symlink -- even one pointing at a
     * perfectly good directory -- is S_ISLNK here and so refused. */
    if (!S_ISDIR(info.st_mode)) {
        free(directory);
        return OI_ERR_INVAL;
    }
    *out_directory = directory;
    return OI_OK;
}

/*
 * Records why a lifecycle operation failed, for the command layer to show
 * verbatim. Best effort: a failed strdup just leaves the caller with no
 * detail, which is strictly better than turning a useful error into an
 * out-of-memory one.
 */
static void set_error_detail(char **out_error_detail, const char *detail) {
    if (out_error_detail == NULL) {
        return;
    }
    free(*out_error_detail);
    *out_error_detail = strdup(detail);
}

oi_status oi_cli_sessions_root(const char *root_override, char **out_root) {
    if (out_root == NULL) {
        return OI_ERR_INVAL;
    }
    if (root_override != NULL) {
        if (root_override[0] == '\0') {
            return OI_ERR_INVAL;
        }
        *out_root = strdup(root_override);
        return *out_root == NULL ? OI_ERR_NOMEM : OI_OK;
    }
    return oi_cli_sessions_default_root(out_root);
}

/* Shorthand for the many internal callers that already validated their
 * arguments and only need the root. */
static oi_status resolve_root(const char *root_override, char **out_root) {
    return oi_cli_sessions_root(root_override, out_root);
}

oi_status oi_cli_session_history_path(const char *directory,
                                      char **out_history_path) {
    if (directory == NULL || out_history_path == NULL) {
        return OI_ERR_INVAL;
    }
    return join_path(directory, session_history_name, out_history_path);
}

/*
 * The trash lives inside the sessions root, so trash and restore are
 * same-filesystem renames by construction. Its name starts with '.', so
 * oi_cli_session_id_is_safe already excludes it from enumeration and it
 * can never collide with a real session id.
 */
static oi_status resolve_trash_root(const char *root_override,
                                    char **out_trash_root) {
    char *root = NULL;
    oi_status status = resolve_root(root_override, &root);

    if (status != OI_OK) {
        return status;
    }
    status = join_path(root, ".trash", out_trash_root);
    free(root);
    return status;
}

/*
 * Read-only check for whether another process holds `history_path`'s
 * lock, taking flock(2) directly rather than going through
 * oi_sesslog_open. That matters: oi_sesslog_open creates the file when
 * absent and truncates a trailing record left incomplete by a crash, and
 * neither belongs in a probe that merely answers "is this session busy?"
 * -- so a healthy session is never written to just by being listed.
 *
 * flock(2) needs no write access, so an O_RDONLY descriptor is enough,
 * and the lock is released by closing it.
 */
static enum oi_cli_session_lock_state probe_lock(const char *history_path) {
    enum oi_cli_session_lock_state state;
    int fd = open(history_path, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        /* No log at all: nothing holds a lock on it either. Any other
         * failure (permissions) leaves the answer genuinely unknown. */
        return errno == ENOENT ? OI_CLI_SESSION_LOCK_FREE
                               : OI_CLI_SESSION_LOCK_UNKNOWN;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        flock(fd, LOCK_UN);
        state = OI_CLI_SESSION_LOCK_FREE;
    } else {
        /* Only "would have blocked" actually means another process holds
         * it. Anything else -- EINTR included, which says nothing about
         * the lock -- is honestly unknown rather than assumed busy. */
        state = (errno == EWOULDBLOCK || errno == EAGAIN)
                    ? OI_CLI_SESSION_LOCK_BUSY
                    : OI_CLI_SESSION_LOCK_UNKNOWN;
    }
    close(fd);
    return state;
}

/*
 * Recovers a damaged session's model/cwd by replaying its own history --
 * the authoritative record metadata.json is only ever a cache of.
 * Replays exactly one session's log, never more.
 *
 * Best effort by contract: on any failure (the log is locked by another
 * process, absent, or undecodable) the outputs are simply left as the
 * caller initialized them. A session nothing can be recovered for still
 * has to be listable.
 *
 * Unlike probe_lock, this does open the log through oi_sesslog_open, which
 * takes the exclusive lock and performs that function's documented crash
 * recovery -- truncating a trailing record left incomplete by a crash.
 * That is unavoidable here, since replay needs an open log, and it is the
 * same recovery a real open of this session would do. It is confined to
 * sessions whose cache is already missing or malformed; a healthy session
 * never reaches this path.
 */
static void rebuild_from_history(const char *history_path,
                                 struct oi_cli_string *out_model,
                                 struct oi_cli_string *out_cwd,
                                 int64_t *out_created_at,
                                 int64_t *out_updated_at) {
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    oi_sesslog *log = NULL;
    struct stat info;

    /* Timestamps first: the log's own mtime is a usable stand-in for
     * "last active" and survives even an undecodable log. */
    if (stat(history_path, &info) == 0) {
        if (*out_created_at == 0) {
            *out_created_at = (int64_t)info.st_mtime;
        }
        if (*out_updated_at == 0) {
            *out_updated_at = (int64_t)info.st_mtime;
        }
    }
    if (oi_sesslog_open(history_path, &log) != OI_OK) {
        return;
    }
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    if (oi_cli_history_store_load(log, &store, &state) == OI_OK) {
        if (state.last_model.data != NULL) {
            (void)oi_cli_string_set(out_model, state.last_model.data,
                                    state.last_model.len);
        }
        if (state.last_cwd.data != NULL) {
            (void)oi_cli_string_set(out_cwd, state.last_cwd.data,
                                    state.last_cwd.len);
        }
    }
    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);
}

void oi_cli_session_list_init(struct oi_cli_session_list *list) {
    if (list != NULL) {
        list->entries = NULL;
        list->len = 0;
        list->cap = 0;
    }
}

void oi_cli_session_list_free(struct oi_cli_session_list *list) {
    size_t index;

    if (list == NULL) {
        return;
    }
    for (index = 0; index < list->len; index++) {
        free(list->entries[index].id);
        oi_cli_string_free(&list->entries[index].display_name);
        oi_cli_string_free(&list->entries[index].model);
        oi_cli_string_free(&list->entries[index].cwd);
    }
    free(list->entries);
    oi_cli_session_list_init(list);
}

static oi_status session_list_reserve(struct oi_cli_session_list *list) {
    struct oi_cli_session_list_entry *entries;
    size_t cap;

    if (list->len != list->cap) {
        return OI_OK;
    }
    cap = list->cap == 0 ? 8 : list->cap;
    if (list->cap != 0) {
        if (cap > SIZE_MAX / 2) {
            return OI_ERR_NOMEM;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof *entries) {
        return OI_ERR_NOMEM;
    }
    entries = realloc(list->entries, cap * sizeof *entries);
    if (entries == NULL) {
        return OI_ERR_NOMEM;
    }
    list->entries = entries;
    list->cap = cap;
    return OI_OK;
}

/* Builds one entry for `id`, whose directory is `directory`. */
static oi_status describe_session(const char *id, const char *directory,
                                  struct oi_cli_session_list_entry *out) {
    struct oi_cli_session_metadata metadata;
    char *history_path = NULL;
    char *metadata_path = NULL;
    oi_status status;

    memset(out, 0, sizeof *out);
    out->id = strdup(id);
    if (out->id == NULL) {
        return OI_ERR_NOMEM;
    }
    status = join_path(directory, session_history_name, &history_path);
    if (status == OI_OK) {
        status = join_path(directory, "metadata.json", &metadata_path);
    }
    if (status != OI_OK) {
        free(history_path);
        free(out->id);
        out->id = NULL;
        return status;
    }

    /* Probe before any replay: a busy session cannot be replayed, and
     * knowing that up front avoids attempting it. */
    out->lock_state = probe_lock(history_path);

    oi_cli_session_metadata_init(&metadata);
    if (oi_cli_session_metadata_store_read(metadata_path, &metadata) ==
        OI_OK) {
        if (metadata.display_name.len > 0) {
            (void)oi_cli_string_set(&out->display_name,
                                    metadata.display_name.data,
                                    metadata.display_name.len);
        }
        (void)oi_cli_string_set(&out->model, metadata.model.data,
                                metadata.model.len);
        (void)oi_cli_string_set(&out->cwd, metadata.cwd.data,
                                metadata.cwd.len);
        out->created_at = metadata.created_at;
        out->updated_at = metadata.updated_at;
    } else {
        /* Missing or malformed cache. The session is still real and still
         * selectable -- rebuild what history can tell us and say so. */
        out->degraded = 1;
        if (out->lock_state != OI_CLI_SESSION_LOCK_BUSY) {
            rebuild_from_history(history_path, &out->model, &out->cwd,
                                 &out->created_at, &out->updated_at);
        }
    }
    oi_cli_session_metadata_free(&metadata);
    free(history_path);
    free(metadata_path);
    return OI_OK;
}

/* Most recently updated first; ties broken by id so the order is
 * deterministic (an id encodes its creation time, so this stays
 * chronologically sensible). */
static int compare_entries(const void *left, const void *right) {
    const struct oi_cli_session_list_entry *a = left;
    const struct oi_cli_session_list_entry *b = right;

    if (a->updated_at != b->updated_at) {
        return a->updated_at > b->updated_at ? -1 : 1;
    }
    return strcmp(b->id, a->id);
}

/* Scans one directory of session directories -- the live root or the
 * trash, which have identical layout. */
static oi_status enumerate_directory(const char *root,
                                     struct oi_cli_session_list *out_list) {
    struct oi_cli_session_list list;
    struct dirent *entry;
    DIR *dir;
    oi_status status;

    dir = opendir(root);
    if (dir == NULL) {
        /* An absent directory simply means nothing is in it yet. */
        if (errno == ENOENT || errno == ENOTDIR) {
            oi_cli_session_list_free(out_list);
            oi_cli_session_list_init(out_list);
            return OI_OK;
        }
        return OI_ERR_IO;
    }

    oi_cli_session_list_init(&list);
    while ((entry = readdir(dir)) != NULL) {
        char *directory = NULL;

        /* Rejects ".", "..", ".trash", and every other non-session entry
         * in one test -- see oi_cli_session_id_is_safe. */
        if (!oi_cli_session_id_is_safe(entry->d_name,
                                       strlen(entry->d_name))) {
            continue;
        }
        /* Also refuses a symlink standing in for a session directory. */
        if (oi_cli_session_resolve(root, entry->d_name,
                                   strlen(entry->d_name),
                                   &directory) != OI_OK) {
            continue;
        }
        status = session_list_reserve(&list);
        if (status == OI_OK) {
            status = describe_session(entry->d_name, directory,
                                      &list.entries[list.len]);
            if (status == OI_OK) {
                list.len++;
            }
        }
        free(directory);
        if (status != OI_OK) {
            closedir(dir);
            oi_cli_session_list_free(&list);
            return status;
        }
    }
    closedir(dir);

    if (list.len > 1) {
        qsort(list.entries, list.len, sizeof *list.entries, compare_entries);
    }
    oi_cli_session_list_free(out_list);
    *out_list = list;
    return OI_OK;
}

oi_status oi_cli_sessions_enumerate(const char *root_override,
                                    struct oi_cli_session_list *out_list) {
    char *root = NULL;
    oi_status status;

    if (out_list == NULL) {
        return OI_ERR_INVAL;
    }
    status = resolve_root(root_override, &root);
    if (status != OI_OK) {
        return status;
    }
    status = enumerate_directory(root, out_list);
    free(root);
    return status;
}

oi_status oi_cli_sessions_enumerate_trash(
    const char *root_override, struct oi_cli_session_list *out_list) {
    char *trash_root = NULL;
    oi_status status;

    if (out_list == NULL) {
        return OI_ERR_INVAL;
    }
    status = resolve_trash_root(root_override, &trash_root);
    if (status != OI_OK) {
        return status;
    }
    status = enumerate_directory(trash_root, out_list);
    free(trash_root);
    return status;
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

/*
 * Produces a complete, writable metadata struct for an existing session:
 * the cache when it is valid and belongs to this session, otherwise one
 * rebuilt from the session's own history.
 *
 * OI_ERR_PARSE when neither source yields a model and cwd -- the cache's
 * validity rules require both, and inventing placeholders would be worse
 * than failing, since the cache is what a later open trusts to restore
 * them. Such a session can still be listed and trashed.
 *
 * `out_metadata` must be initialized and empty on entry; it is replaced
 * only on success.
 */
static oi_status load_or_rebuild_metadata(
    const char *directory, const char *id, size_t id_len,
    struct oi_cli_session_metadata *out_metadata) {
    struct oi_cli_session_metadata cached;
    struct oi_cli_string model;
    struct oi_cli_string cwd;
    char *history_path = NULL;
    char *metadata_path = NULL;
    int64_t created_at = 0;
    int64_t updated_at = 0;
    oi_status status;

    status = join_path(directory, session_history_name, &history_path);
    if (status == OI_OK) {
        status = join_path(directory, "metadata.json", &metadata_path);
    }
    if (status != OI_OK) {
        free(history_path);
        return status;
    }

    oi_cli_session_metadata_init(&cached);
    if (oi_cli_session_metadata_store_read(metadata_path, &cached) == OI_OK &&
        cached.session_id.len == id_len &&
        memcmp(cached.session_id.data, id, id_len) == 0) {
        free(history_path);
        free(metadata_path);
        *out_metadata = cached;
        return OI_OK;
    }
    oi_cli_session_metadata_free(&cached);

    memset(&model, 0, sizeof model);
    memset(&cwd, 0, sizeof cwd);
    rebuild_from_history(history_path, &model, &cwd, &created_at,
                         &updated_at);
    if (model.len == 0 || cwd.len == 0) {
        status = OI_ERR_PARSE;
    } else {
        status = oi_cli_session_metadata_set(out_metadata, id, id_len,
                                            model.data, model.len, cwd.data,
                                            cwd.len, NULL, 0, created_at,
                                            updated_at < created_at
                                                ? created_at
                                                : updated_at);
    }
    oi_cli_string_free(&model);
    oi_cli_string_free(&cwd);
    free(history_path);
    free(metadata_path);
    return status;
}

oi_status oi_cli_session_rename(const char *root_override, const char *id,
                                size_t id_len, const char *new_name,
                                size_t new_name_len,
                                char **out_error_detail) {
    struct oi_cli_session_metadata metadata;
    char *root = NULL;
    char *directory = NULL;
    char *metadata_path = NULL;
    time_t now;
    int64_t updated_at;
    oi_status status;

    if (out_error_detail != NULL) {
        *out_error_detail = NULL;
    }
    if (!oi_cli_session_id_is_safe(id, id_len)) {
        set_error_detail(out_error_detail, "invalid session id");
        return OI_ERR_INVAL;
    }
    /* No "clear the name" spelling in this grammar: an empty argument is a
     * usage mistake, not a request to unset. */
    if (new_name == NULL || new_name_len == 0) {
        set_error_detail(out_error_detail, "a session name cannot be empty");
        return OI_ERR_INVAL;
    }
    if (new_name_len > OI_CLI_SESSION_METADATA_MAX_DISPLAY_NAME) {
        set_error_detail(out_error_detail, "session name is too long");
        return OI_ERR_INVAL;
    }

    status = resolve_root(root_override, &root);
    if (status != OI_OK) {
        return status;
    }
    status = oi_cli_session_resolve(root, id, id_len, &directory);
    free(root);
    if (status != OI_OK) {
        set_error_detail(out_error_detail,
                         status == OI_ERR_NOTFOUND
                             ? "no such session"
                             : "session directory is not usable");
        return status;
    }

    oi_cli_session_metadata_init(&metadata);
    status = load_or_rebuild_metadata(directory, id, id_len, &metadata);
    if (status != OI_OK) {
        free(directory);
        oi_cli_session_metadata_free(&metadata);
        set_error_detail(out_error_detail,
                         status == OI_ERR_PARSE
                             ? "session metadata is missing and its history "
                               "cannot be read, so a name cannot be recorded"
                             : "session metadata could not be read");
        return status == OI_ERR_PARSE ? OI_ERR_IO : status;
    }

    now = time(NULL);
    updated_at = now == (time_t)-1 ? metadata.updated_at : (int64_t)now;
    if (updated_at < metadata.created_at) {
        updated_at = metadata.created_at;
    }
    /* Passing `metadata`'s own fields back in is safe: the setter copies
     * every argument into a replacement struct and only frees the
     * destination once all of those copies have succeeded. */
    status = oi_cli_session_metadata_set(
        &metadata, id, id_len, metadata.model.data, metadata.model.len,
        metadata.cwd.data, metadata.cwd.len, new_name, new_name_len,
        metadata.created_at, updated_at);
    if (status != OI_OK) {
        /* The only remaining rejection is a control byte in the name. */
        set_error_detail(out_error_detail,
                         "a session name cannot contain control characters");
        free(directory);
        oi_cli_session_metadata_free(&metadata);
        return OI_ERR_INVAL;
    }

    status = join_path(directory, "metadata.json", &metadata_path);
    if (status == OI_OK) {
        status = oi_cli_session_metadata_store_write(metadata_path,
                                                     &metadata);
        if (status != OI_OK) {
            set_error_detail(out_error_detail,
                             "could not write session metadata");
        }
    }
    free(metadata_path);
    free(directory);
    oi_cli_session_metadata_free(&metadata);
    return status;
}

/*
 * Refuses to touch a session another process has open. Shared by trash,
 * restore, and delete: moving or removing a directory out from under a
 * live process is exactly the kind of surprise durable history exists to
 * prevent.
 */
static oi_status refuse_if_busy(const char *directory,
                                char **out_error_detail) {
    char *history_path = NULL;
    oi_status status = join_path(directory, session_history_name,
                                &history_path);

    if (status != OI_OK) {
        return status;
    }
    switch (probe_lock(history_path)) {
    case OI_CLI_SESSION_LOCK_BUSY:
        set_error_detail(out_error_detail,
                         "session is open in another process");
        status = OI_ERR_EXISTS;
        break;
    case OI_CLI_SESSION_LOCK_UNKNOWN:
        set_error_detail(out_error_detail,
                         "cannot determine whether the session is in use");
        status = OI_ERR_IO;
        break;
    case OI_CLI_SESSION_LOCK_FREE:
    default:
        status = OI_OK;
        break;
    }
    free(history_path);
    return status;
}

/*
 * Moves a session directory between the live root and the trash. Both
 * callers hand in an already-resolved source and a destination parent.
 */
static oi_status move_session_directory(const char *source,
                                        const char *destination_parent,
                                        const char *id, size_t id_len,
                                        char **out_error_detail) {
    char terminated[OI_CLI_SESSION_SAFE_ID_MAX_LEN + 1];
    char *destination = NULL;
    oi_status status;

    /* Both callers validate first, but this writes into a fixed buffer, so
     * it proves the bound itself rather than inheriting it by assumption. */
    if (!oi_cli_session_id_is_safe(id, id_len)) {
        return OI_ERR_INVAL;
    }
    memcpy(terminated, id, id_len);
    terminated[id_len] = '\0';
    status = ensure_directory(destination_parent);
    if (status != OI_OK) {
        set_error_detail(out_error_detail,
                         "could not create the destination directory");
        return status;
    }
    status = join_path(destination_parent, terminated, &destination);
    if (status != OI_OK) {
        return status;
    }
    /* Atomic within a filesystem: either the whole directory moved or
     * nothing did, so there is no half-trashed state to recover from. */
    if (rename(source, destination) != 0) {
        if (errno == EXDEV) {
            set_error_detail(
                out_error_detail,
                "the trash directory is on a different filesystem");
        } else {
            set_error_detail(out_error_detail, strerror(errno));
        }
        status = OI_ERR_IO;
    }
    free(destination);
    return status;
}

oi_status oi_cli_session_trash(const char *root_override, const char *id,
                               size_t id_len, const char *current_session_id,
                               char **out_error_detail) {
    char *root = NULL;
    char *trash_root = NULL;
    char *directory = NULL;
    oi_status status;

    if (out_error_detail != NULL) {
        *out_error_detail = NULL;
    }
    if (!oi_cli_session_id_is_safe(id, id_len)) {
        set_error_detail(out_error_detail, "invalid session id");
        return OI_ERR_INVAL;
    }
    /* Ids are unique, so an exact match is unambiguous. */
    if (current_session_id != NULL &&
        strlen(current_session_id) == id_len &&
        memcmp(current_session_id, id, id_len) == 0) {
        set_error_detail(out_error_detail,
                         "cannot trash the active session -- switch away "
                         "from it first");
        return OI_ERR_INVAL;
    }

    status = resolve_root(root_override, &root);
    if (status != OI_OK) {
        return status;
    }
    status = oi_cli_session_resolve(root, id, id_len, &directory);
    if (status != OI_OK) {
        free(root);
        set_error_detail(out_error_detail,
                         status == OI_ERR_NOTFOUND
                             ? "no such session"
                             : "session directory is not usable");
        return status;
    }
    status = refuse_if_busy(directory, out_error_detail);
    if (status == OI_OK) {
        status = join_path(root, ".trash", &trash_root);
    }
    if (status == OI_OK) {
        status = move_session_directory(directory, trash_root, id, id_len,
                                        out_error_detail);
    }
    free(trash_root);
    free(directory);
    free(root);
    return status;
}

oi_status oi_cli_session_restore_trashed(const char *root_override,
                                         const char *id, size_t id_len,
                                         char **out_error_detail) {
    char *root = NULL;
    char *trash_root = NULL;
    char *directory = NULL;
    char *live_directory = NULL;
    oi_status status;

    if (out_error_detail != NULL) {
        *out_error_detail = NULL;
    }
    if (!oi_cli_session_id_is_safe(id, id_len)) {
        set_error_detail(out_error_detail, "invalid session id");
        return OI_ERR_INVAL;
    }
    status = resolve_root(root_override, &root);
    if (status != OI_OK) {
        return status;
    }
    status = join_path(root, ".trash", &trash_root);
    if (status != OI_OK) {
        free(root);
        return status;
    }
    status = oi_cli_session_resolve(trash_root, id, id_len, &directory);
    if (status != OI_OK) {
        set_error_detail(out_error_detail,
                         status == OI_ERR_NOTFOUND
                             ? "no trashed session with that id"
                             : "trashed session directory is not usable");
        free(trash_root);
        free(root);
        return status;
    }
    /* Generated ids are unique, so this should be impossible -- check it
     * anyway rather than let a rename clobber a live session. */
    if (oi_cli_session_resolve(root, id, id_len, &live_directory) == OI_OK) {
        free(live_directory);
        set_error_detail(out_error_detail,
                         "a live session with that id already exists");
        free(directory);
        free(trash_root);
        free(root);
        return OI_ERR_EXISTS;
    }
    /* A trashed session should never be locked; checking costs nothing. */
    status = refuse_if_busy(directory, out_error_detail);
    if (status == OI_OK) {
        status = move_session_directory(directory, root, id, id_len,
                                        out_error_detail);
    }
    free(directory);
    free(trash_root);
    free(root);
    return status;
}

/*
 * Empties and removes one trashed session directory.
 *
 * Deliberately not a generic recursive delete: a session directory holds a
 * known, flat set of regular files (history.oilog, metadata.json, and
 * possibly a metadata.json.tmp from an interrupted write). Anything else --
 * a nested directory, a symlink, a device -- means this is not a directory
 * oi created, so it fails closed instead of recursing. Deletion should
 * never remove something it did not put there.
 */
static oi_status remove_session_directory(const char *directory,
                                          char **out_error_detail) {
    struct dirent *entry;
    DIR *dir = opendir(directory);
    oi_status status = OI_OK;

    if (dir == NULL) {
        set_error_detail(out_error_detail, strerror(errno));
        return OI_ERR_IO;
    }
    while (status == OI_OK && (entry = readdir(dir)) != NULL) {
        char *path = NULL;
        struct stat info;

        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        status = join_path(directory, entry->d_name, &path);
        if (status != OI_OK) {
            break;
        }
        if (lstat(path, &info) != 0) {
            set_error_detail(out_error_detail, strerror(errno));
            status = OI_ERR_IO;
        } else if (!S_ISREG(info.st_mode)) {
            set_error_detail(out_error_detail,
                             "session directory holds an unexpected entry; "
                             "refusing to delete it");
            status = OI_ERR_IO;
        } else if (unlink(path) != 0) {
            set_error_detail(out_error_detail, strerror(errno));
            status = OI_ERR_IO;
        }
        free(path);
    }
    closedir(dir);
    if (status == OI_OK && rmdir(directory) != 0) {
        set_error_detail(out_error_detail, strerror(errno));
        status = OI_ERR_IO;
    }
    return status;
}

oi_status oi_cli_session_delete(const char *root_override, const char *id,
                                size_t id_len, char **out_error_detail) {
    char *trash_root = NULL;
    char *directory = NULL;
    oi_status status;

    if (out_error_detail != NULL) {
        *out_error_detail = NULL;
    }
    if (!oi_cli_session_id_is_safe(id, id_len)) {
        set_error_detail(out_error_detail, "invalid session id");
        return OI_ERR_INVAL;
    }
    status = resolve_trash_root(root_override, &trash_root);
    if (status != OI_OK) {
        return status;
    }
    /*
     * Scoped to the trash, which is what makes refusing the current
     * session and refusing a session open elsewhere structural: neither is
     * in the trash, so neither can be found here at all.
     */
    status = oi_cli_session_resolve(trash_root, id, id_len, &directory);
    free(trash_root);
    if (status != OI_OK) {
        set_error_detail(out_error_detail,
                         status == OI_ERR_NOTFOUND
                             ? "no trashed session with that id -- trash it "
                               "first"
                             : "trashed session directory is not usable");
        return status;
    }
    status = refuse_if_busy(directory, out_error_detail);
    if (status == OI_OK) {
        status = remove_session_directory(directory, out_error_detail);
    }
    free(directory);
    return status;
}

/*
 * True when `file_path` lives somewhere under `root`.
 *
 * Compares device and inode numbers while walking from the file's own
 * directory up through its parents, rather than comparing path strings.
 * That gets symlinks, "..", and redundant separators right for free -- a
 * textual prefix test would need canonical paths, and realpath(3) sits
 * behind a feature macro this project does not enable.
 *
 * Answers "no" on any stat failure: this decides whether to show a more
 * helpful error, so guessing wrong merely costs a better message.
 */
static int path_is_inside_root(const char *file_path, const char *root) {
    struct stat root_info;
    char *walk;
    unsigned depth;
    int inside = 0;

    if (stat(root, &root_info) != 0) {
        return 0;
    }
    walk = strdup(file_path);
    if (walk == NULL) {
        return 0;
    }
    /* Start at the file's containing directory. */
    {
        char *slash = strrchr(walk, '/');
        if (slash == NULL) {
            free(walk);
            walk = strdup(".");
        } else if (slash == walk) {
            walk[1] = '\0'; /* the file sits directly in "/" */
        } else {
            *slash = '\0';
        }
        if (walk == NULL) {
            return 0;
        }
    }

    for (depth = 0; depth < 256u; depth++) {
        struct stat info;
        struct stat parent_info;
        char *parent = NULL;

        if (stat(walk, &info) != 0) {
            break;
        }
        if (info.st_dev == root_info.st_dev &&
            info.st_ino == root_info.st_ino) {
            inside = 1;
            break;
        }
        if (join_path(walk, "..", &parent) != OI_OK) {
            break;
        }
        if (stat(parent, &parent_info) != 0) {
            free(parent);
            break;
        }
        /* A directory that is its own parent is the filesystem root. */
        if (parent_info.st_dev == info.st_dev &&
            parent_info.st_ino == info.st_ino) {
            free(parent);
            break;
        }
        free(walk);
        walk = parent;
    }
    free(walk);
    return inside;
}

/*
 * Creates the import scratch file, O_EXCL, under `root`.
 *
 * O_EXCL never writes over an existing file, so a stale scratch left by a
 * killed earlier run would otherwise block every future import from a
 * process that happened to reuse its pid. Stepping through a bounded
 * sequence of names sidesteps that without ever overwriting anything.
 */
static oi_status create_scratch_file(const char *root, char **out_path,
                                     int *out_fd,
                                     char **out_error_detail) {
    unsigned attempt;

    for (attempt = 0; attempt < 64u; attempt++) {
        char name[64];
        char *path = NULL;
        oi_status status;
        int fd;

        snprintf(name, sizeof name, ".import-%ld-%u.tmp", (long)getpid(),
                 attempt);
        status = join_path(root, name, &path);
        if (status != OI_OK) {
            return status;
        }
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            *out_path = path;
            *out_fd = fd;
            return OI_OK;
        }
        free(path);
        if (errno != EEXIST) {
            set_error_detail(out_error_detail, strerror(errno));
            return OI_ERR_IO;
        }
    }
    set_error_detail(out_error_detail,
                     "could not create a scratch file for the import");
    return OI_ERR_EXISTS;
}

/* Copies `source` into the already-open `out_fd`, which the caller owns. */
static oi_status copy_into_fd(const char *source, int out_fd,
                              char **out_error_detail) {
    char buffer[65536];
    int in_fd;
    oi_status status = OI_OK;

    in_fd = open(source, O_RDONLY | O_CLOEXEC);
    if (in_fd < 0) {
        set_error_detail(out_error_detail, strerror(errno));
        return OI_ERR_IO;
    }
    for (;;) {
        ssize_t read_bytes = read(in_fd, buffer, sizeof buffer);
        size_t written = 0;

        if (read_bytes == 0) {
            break;
        }
        if (read_bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error_detail(out_error_detail, strerror(errno));
            status = OI_ERR_IO;
            break;
        }
        while (written < (size_t)read_bytes) {
            ssize_t wrote = write(out_fd, buffer + written,
                                  (size_t)read_bytes - written);
            if (wrote > 0) {
                written += (size_t)wrote;
            } else if (wrote < 0 && errno == EINTR) {
                continue;
            } else {
                set_error_detail(out_error_detail, strerror(errno));
                status = OI_ERR_IO;
                break;
            }
        }
        if (status != OI_OK) {
            break;
        }
    }
    close(in_fd);
    return status;
}

/*
 * Confirms a copied file really is a session log, using exactly the code
 * that would load it for real -- header validation plus a full strict
 * replay. Nothing cheaper would be honest about it.
 */
static oi_status validate_session_log(const char *path) {
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    oi_sesslog *log = NULL;
    oi_status status = oi_sesslog_open(path, &log);

    if (status != OI_OK) {
        return status;
    }
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    status = oi_cli_history_store_load(log, &store, &state);
    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);
    return status;
}

oi_status oi_cli_session_import(const char *root_override,
                                const char *source_path,
                                size_t source_path_len, char **out_new_id,
                                char **out_error_detail) {
    struct oi_cli_session_location location;
    char *source = NULL;
    char *root = NULL;
    char *scratch_path = NULL;
    int scratch_fd = -1;
    struct stat info;
    oi_status status;

    if (out_error_detail != NULL) {
        *out_error_detail = NULL;
    }
    if (source_path == NULL || source_path_len == 0 || out_new_id == NULL) {
        set_error_detail(out_error_detail, "no path given");
        return OI_ERR_INVAL;
    }
    source = malloc(source_path_len + 1);
    if (source == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(source, source_path, source_path_len);
    source[source_path_len] = '\0';

    /* lstat, not stat: S_ISREG on this result is false for a symlink, so a
     * symlinked source is refused rather than followed. */
    if (lstat(source, &info) != 0) {
        set_error_detail(out_error_detail, strerror(errno));
        free(source);
        return errno == ENOENT ? OI_ERR_NOTFOUND : OI_ERR_IO;
    }
    if (!S_ISREG(info.st_mode)) {
        set_error_detail(out_error_detail,
                         S_ISLNK(info.st_mode)
                             ? "refusing to import a symlink"
                             : "not a regular file");
        free(source);
        return OI_ERR_INVAL;
    }

    status = resolve_root(root_override, &root);
    if (status != OI_OK) {
        free(source);
        return status;
    }
    if (path_is_inside_root(source, root)) {
        set_error_detail(
            out_error_detail,
            "that file is already an oi-managed session; use /session "
            "switch instead");
        free(root);
        free(source);
        return OI_ERR_INVAL;
    }

    /* Scratch file beside the eventual destination, so adopting it later
     * is a same-filesystem rename. The leading '.' keeps it out of
     * enumeration for as long as it exists. */
    status = ensure_directory(root);
    if (status == OI_OK) {
        status = create_scratch_file(root, &scratch_path, &scratch_fd,
                                    out_error_detail);
    }
    free(root);
    if (status != OI_OK) {
        free(source);
        return status;
    }

    status = copy_into_fd(source, scratch_fd, out_error_detail);
    /* The source has now been read and is never touched again. */
    free(source);
    if (close(scratch_fd) != 0 && status == OI_OK) {
        set_error_detail(out_error_detail, strerror(errno));
        status = OI_ERR_IO;
    }
    if (status != OI_OK) {
        unlink(scratch_path);
        free(scratch_path);
        return status;
    }
    if (validate_session_log(scratch_path) != OI_OK) {
        set_error_detail(out_error_detail, "not a valid oi session log");
        unlink(scratch_path);
        free(scratch_path);
        return OI_ERR_PARSE;
    }

    oi_cli_session_location_init(&location);
    status = oi_cli_session_location_create(root_override, &location);
    if (status != OI_OK) {
        set_error_detail(out_error_detail,
                         "could not create a new session directory");
        unlink(scratch_path);
        free(scratch_path);
        return status;
    }
    if (rename(scratch_path, location.history_path) != 0) {
        set_error_detail(out_error_detail, strerror(errno));
        unlink(scratch_path);
        rmdir(location.directory);
        oi_cli_session_location_free(&location);
        free(scratch_path);
        return OI_ERR_IO;
    }
    free(scratch_path);

    *out_new_id = location.id;
    location.id = NULL; /* ownership handed to the caller */
    oi_cli_session_location_free(&location);
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

/*
 * Rewrites the whole metadata cache from the values passed in, so every
 * caller has to hand back the session's existing `display_name`: it lives
 * only in this file, and rebuilding without it would silently drop a
 * user's session name on the next /model or /cwd change. Pass NULL/0 only
 * when the session genuinely has no name.
 */
static void refresh_metadata(const char *metadata_path,
                             const char *session_id, const char *model,
                             size_t model_len, const char *cwd,
                             size_t cwd_len, const char *display_name,
                             size_t display_name_len, int64_t created_at,
                             FILE *diagnostics) {
    struct oi_cli_session_metadata metadata;
    oi_status status;
    time_t now = time(NULL);
    int64_t updated_at = now == (time_t)-1 ? created_at : (int64_t)now;

    oi_cli_session_metadata_init(&metadata);
    status = oi_cli_session_metadata_set(
        &metadata, session_id, strlen(session_id), model, model_len, cwd,
        cwd_len, display_name, display_name_len, created_at,
        updated_at < created_at ? created_at : updated_at);
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
    /* Carries the session's existing name past the point where `meta` is
     * freed, so refreshing the cache below preserves it. */
    struct oi_cli_string preserved_name;
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
    memset(&preserved_name, 0, sizeof preserved_name);

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
    /* Same reason, for the name: it survives only in `meta`, which is
     * about to go away, and refresh_metadata below must not drop it. */
    if (status == OI_OK && metadata_valid && meta.display_name.len > 0) {
        status = oi_cli_string_set(&preserved_name, meta.display_name.data,
                                   meta.display_name.len);
    }
    oi_cli_session_metadata_free(&meta);
    if (status != OI_OK) {
        oi_cli_string_free(&preserved_name);
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
        oi_cli_string_free(&preserved_name);
        return status;
    }

    refresh_metadata(metadata_path, session_id, out_restore->model.data,
                     out_restore->model.len, out_restore->cwd.data,
                     out_restore->cwd.len, preserved_name.data,
                     preserved_name.len, created_at, diagnostics);
    oi_cli_string_free(&preserved_name);
    return OI_OK;
}

oi_status oi_cli_session_apply_setting(
    struct oi_cli_history_store *store,
    struct oi_cli_history_replay_state *state, const char *metadata_path,
    const char *session_id,
    enum oi_cli_history_session_setting_field field, const char *value,
    size_t value_len) {
    struct oi_cli_string preserved_name;
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
    memset(&preserved_name, 0, sizeof preserved_name);
    {
        struct oi_cli_session_metadata existing;
        oi_cli_session_metadata_init(&existing);
        if (oi_cli_session_metadata_store_read(metadata_path, &existing) ==
                OI_OK &&
            existing.session_id.len == strlen(session_id) &&
            memcmp(existing.session_id.data, session_id,
                   existing.session_id.len) == 0) {
            created_at = existing.created_at;
            /* Carry the name forward: a /model or /cwd change must not
             * cost the session the name the user gave it. */
            if (existing.display_name.len > 0) {
                (void)oi_cli_string_set(&preserved_name,
                                        existing.display_name.data,
                                        existing.display_name.len);
            }
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
        preserved_name.data, preserved_name.len, created_at, NULL);
    oi_cli_string_free(&preserved_name);
    return OI_OK;
}
