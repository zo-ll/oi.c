#include "cli_sessions.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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
}

void oi_cli_session_location_free(
    struct oi_cli_session_location *location) {
    if (location == NULL) {
        return;
    }
    free(location->id);
    free(location->directory);
    free(location->history_path);
    oi_cli_session_location_init(location);
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
    *out_location = location;
    return OI_OK;
}
