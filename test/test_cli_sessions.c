#include "cli_sessions.h"
#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cli_session_metadata.h"
#include "cli_session_metadata_store.h"
#include "oi/sesslog.h"

/* Reads a whole file for byte-for-byte comparisons. */
static void read_whole_file(const char *path, char **out_data,
                           size_t *out_len) {
    FILE *file = fopen(path, "rb");
    long size;
    CHECK(file != NULL);
    CHECK_EQ(fseek(file, 0, SEEK_END), 0);
    size = ftell(file);
    CHECK(size >= 0);
    CHECK_EQ(fseek(file, 0, SEEK_SET), 0);
    *out_data = malloc((size_t)size);
    CHECK(*out_data != NULL);
    CHECK_EQ(fread(*out_data, 1, (size_t)size, file), (size_t)size);
    CHECK_EQ(fclose(file), 0);
    *out_len = (size_t)size;
}

TEST(default_root_uses_xdg_state_home) {
    char state_home[128];
    char *root = NULL;

    snprintf(state_home, sizeof state_home, "/tmp/oi-state-root-%ld",
             (long)getpid());
    CHECK_EQ(setenv("XDG_STATE_HOME", state_home, 1), 0);
    CHECK_EQ(oi_cli_sessions_default_root(&root), OI_OK);
    {
        char expected[160];
        snprintf(expected, sizeof expected, "%s/oi/sessions", state_home);
        CHECK_STREQ(root, expected);
    }
    free(root);
}

TEST(create_makes_private_timestamped_session) {
    char root[128];
    struct oi_cli_session_location location;
    struct stat info;

    snprintf(root, sizeof root, "/tmp/oi-session-location-%ld",
             (long)getpid());
    oi_cli_session_location_init(&location);
    CHECK_EQ(oi_cli_session_location_create(root, &location), OI_OK);
    CHECK(location.id != NULL);
    CHECK(strlen(location.id) < 64);
    CHECK(strstr(location.id, "/") == NULL);
    CHECK(strstr(location.history_path, "history.oilog") != NULL);
    CHECK(strstr(location.metadata_path, "metadata.json") != NULL);
    CHECK(strstr(location.metadata_path, "history.") == NULL);
    CHECK_EQ(stat(location.directory, &info), 0);
    CHECK(S_ISDIR(info.st_mode));
    CHECK_EQ(info.st_mode & 077, 0);
    CHECK(access(location.history_path, F_OK) != 0);

    CHECK_EQ(rmdir(location.directory), 0);
    oi_cli_session_location_free(&location);
    CHECK_EQ(rmdir(root), 0);
}

TEST(two_creations_are_unique) {
    char root[128];
    struct oi_cli_session_location first;
    struct oi_cli_session_location second;

    snprintf(root, sizeof root, "/tmp/oi-session-unique-%ld",
             (long)getpid());
    oi_cli_session_location_init(&first);
    oi_cli_session_location_init(&second);
    CHECK_EQ(oi_cli_session_location_create(root, &first), OI_OK);
    CHECK_EQ(oi_cli_session_location_create(root, &second), OI_OK);
    CHECK(strcmp(first.id, second.id) != 0);

    CHECK_EQ(rmdir(first.directory), 0);
    CHECK_EQ(rmdir(second.directory), 0);
    oi_cli_session_location_free(&first);
    oi_cli_session_location_free(&second);
    CHECK_EQ(rmdir(root), 0);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_session_location location;

    oi_cli_session_location_init(&location);
    CHECK_EQ(oi_cli_sessions_default_root(NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_location_create(NULL, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_location_create("", &location), OI_ERR_INVAL);
    oi_cli_session_location_free(&location);
    oi_cli_session_location_free(&location);
}

TEST(metadata_path_derivation_rules) {
    char *path = NULL;

    CHECK_EQ(oi_cli_session_metadata_path_for_log(
                 "/root/sess/history.oilog", 1, &path),
             OI_OK);
    CHECK_STREQ(path, "/root/sess/metadata.json");
    free(path);
    path = NULL;

    CHECK_EQ(
        oi_cli_session_metadata_path_for_log("/root/abc.oilog", 0, &path),
        OI_OK);
    CHECK_STREQ(path, "/root/abc.metadata.json");
    free(path);
    path = NULL;

    CHECK_EQ(oi_cli_session_metadata_path_for_log("/root/abc.log", 0, &path),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_path_for_log(NULL, 0, &path),
             OI_ERR_INVAL);
}

/* --- oi_cli_session_id_is_safe / oi_cli_session_resolve --- */

TEST(safe_ids_accept_only_bounded_portable_components) {
    char overlong[OI_CLI_SESSION_SAFE_ID_MAX_LEN + 2];
    char at_limit[OI_CLI_SESSION_SAFE_ID_MAX_LEN + 1];

    /* The shape oi actually generates. */
    CHECK_EQ(oi_cli_session_id_is_safe("20260730-091500-1234-000", 24), 1);
    CHECK_EQ(oi_cli_session_id_is_safe("a", 1), 1);
    CHECK_EQ(oi_cli_session_id_is_safe("A_b-9", 5), 1);
    /* A leading dash is legal: ids never reach an option parser. */
    CHECK_EQ(oi_cli_session_id_is_safe("-lead", 5), 1);

    CHECK_EQ(oi_cli_session_id_is_safe(NULL, 4), 0);
    CHECK_EQ(oi_cli_session_id_is_safe("", 0), 0);

    /* Traversal and separators fall out of the charset rule alone. */
    CHECK_EQ(oi_cli_session_id_is_safe(".", 1), 0);
    CHECK_EQ(oi_cli_session_id_is_safe("..", 2), 0);
    CHECK_EQ(oi_cli_session_id_is_safe("../escape", 9), 0);
    CHECK_EQ(oi_cli_session_id_is_safe("a/b", 3), 0);
    CHECK_EQ(oi_cli_session_id_is_safe("/abs", 4), 0);
    CHECK_EQ(oi_cli_session_id_is_safe(".trash", 6), 0);
    CHECK_EQ(oi_cli_session_id_is_safe(".hidden", 7), 0);
    /* No '.' at all, so a plausible-looking dotted id is still refused. */
    CHECK_EQ(oi_cli_session_id_is_safe("history.oilog", 13), 0);

    /* Anything outside [A-Za-z0-9_-]. */
    CHECK_EQ(oi_cli_session_id_is_safe("has space", 9), 0);
    CHECK_EQ(oi_cli_session_id_is_safe("semi;colon", 10), 0);
    CHECK_EQ(oi_cli_session_id_is_safe("new\nline", 8), 0);
    CHECK_EQ(oi_cli_session_id_is_safe("\x7f", 1), 0);
    /* High-bit bytes are rejected regardless of char's signedness. */
    CHECK_EQ(oi_cli_session_id_is_safe("\xc3\xa9", 2), 0);

    /* An embedded NUL cannot be smuggled past by over-reporting id_len:
     * '\0' simply isn't in the allowed charset. */
    CHECK_EQ(oi_cli_session_id_is_safe("ok\0hidden", 9), 0);
    /* ...and the honest prefix of that same buffer is still fine. */
    CHECK_EQ(oi_cli_session_id_is_safe("ok\0hidden", 2), 1);

    memset(at_limit, 'a', sizeof at_limit - 1);
    at_limit[sizeof at_limit - 1] = '\0';
    CHECK_EQ(oi_cli_session_id_is_safe(at_limit,
                                       OI_CLI_SESSION_SAFE_ID_MAX_LEN),
             1);
    memset(overlong, 'a', sizeof overlong - 1);
    overlong[sizeof overlong - 1] = '\0';
    CHECK_EQ(oi_cli_session_id_is_safe(overlong,
                                       OI_CLI_SESSION_SAFE_ID_MAX_LEN + 1),
             0);
}

TEST(resolve_accepts_a_real_directory_and_refuses_everything_else) {
    char root[160];
    char path[256];
    char *directory = NULL;

    snprintf(root, sizeof root, "/tmp/oi-session-resolve-%ld", (long)getpid());
    CHECK(mkdir(root, 0700) == 0 || errno == EEXIST);

    /* A genuine session directory resolves to its joined path. */
    snprintf(path, sizeof path, "%s/live", root);
    CHECK(mkdir(path, 0700) == 0 || errno == EEXIST);
    CHECK_EQ(oi_cli_session_resolve(root, "live", 4, &directory), OI_OK);
    CHECK_STREQ(directory, path);
    free(directory);
    directory = NULL;

    /* Nothing there at all. */
    CHECK_EQ(oi_cli_session_resolve(root, "absent", 6, &directory),
             OI_ERR_NOTFOUND);
    CHECK(directory == NULL);

    /* A symlink pointing at a real directory is refused, not followed --
     * this is the symlink-escape case. */
    snprintf(path, sizeof path, "%s/link", root);
    unlink(path);
    CHECK_EQ(symlink("/tmp", path), 0);
    CHECK_EQ(oi_cli_session_resolve(root, "link", 4, &directory),
             OI_ERR_INVAL);
    CHECK(directory == NULL);
    CHECK_EQ(unlink(path), 0);

    /* A dangling symlink is refused as a wrong type, not reported
     * missing: lstat succeeds on the link itself. */
    snprintf(path, sizeof path, "%s/dangling", root);
    unlink(path);
    CHECK_EQ(symlink("/tmp/oi-does-not-exist-anywhere", path), 0);
    CHECK_EQ(oi_cli_session_resolve(root, "dangling", 8, &directory),
             OI_ERR_INVAL);
    CHECK_EQ(unlink(path), 0);

    /* A stray regular file where a directory belongs. */
    snprintf(path, sizeof path, "%s/plainfile", root);
    {
        FILE *stray = fopen(path, "w");
        CHECK(stray != NULL);
        CHECK_EQ(fclose(stray), 0);
    }
    CHECK_EQ(oi_cli_session_resolve(root, "plainfile", 9, &directory),
             OI_ERR_INVAL);
    CHECK_EQ(unlink(path), 0);

    /* Unsafe ids never reach the filesystem. */
    CHECK_EQ(oi_cli_session_resolve(root, "..", 2, &directory), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_resolve(root, "a/b", 3, &directory),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_resolve(root, "", 0, &directory), OI_ERR_INVAL);

    /* Bad arguments. */
    CHECK_EQ(oi_cli_session_resolve(NULL, "live", 4, &directory),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_resolve("", "live", 4, &directory),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_resolve(root, "live", 4, NULL), OI_ERR_INVAL);

    snprintf(path, sizeof path, "%s/live", root);
    CHECK_EQ(rmdir(path), 0);
    CHECK_EQ(rmdir(root), 0);
}

/* --- oi_cli_sessions_enumerate --- */

/*
 * Builds a session directory the way a real run leaves one behind:
 * a history.oilog carrying a transition plus durable model/cwd settings,
 * and (unless `write_metadata` is 0) a matching metadata.json cache.
 */
static void seed_session(const char *root, const char *id, const char *model,
                         const char *cwd, int64_t created_at,
                         int64_t updated_at, int write_metadata) {
    char directory[320];
    char history_path[384];
    char metadata_path[384];
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_history_record record;
    oi_sesslog *log = NULL;

    CHECK(mkdir(root, 0700) == 0 || errno == EEXIST);
    snprintf(directory, sizeof directory, "%s/%s", root, id);
    CHECK(mkdir(directory, 0700) == 0 || errno == EEXIST);
    snprintf(history_path, sizeof history_path, "%s/history.oilog",
             directory);
    snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
             directory);

    CHECK_EQ(oi_sesslog_open(history_path, &log), OI_OK);
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);

    oi_cli_history_record_init(&record);
    CHECK_EQ(oi_cli_history_record_set_transition(&record,
                                                  state.next_record_id, 0),
             OI_OK);
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
    oi_cli_history_record_free(&record);

    oi_cli_history_record_init(&record);
    CHECK_EQ(oi_cli_history_record_set_session_setting(
                 &record, state.next_record_id,
                 OI_CLI_HISTORY_SESSION_SETTING_MODEL, model, strlen(model)),
             OI_OK);
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
    oi_cli_history_record_free(&record);

    oi_cli_history_record_init(&record);
    CHECK_EQ(oi_cli_history_record_set_session_setting(
                 &record, state.next_record_id,
                 OI_CLI_HISTORY_SESSION_SETTING_CWD, cwd, strlen(cwd)),
             OI_OK);
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
    oi_cli_history_record_free(&record);

    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);

    if (write_metadata) {
        struct oi_cli_session_metadata metadata;
        oi_cli_session_metadata_init(&metadata);
        CHECK_EQ(oi_cli_session_metadata_set(&metadata, id, strlen(id), model,
                                             strlen(model), cwd, strlen(cwd),
                                             NULL, 0, created_at, updated_at),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_store_write(metadata_path,
                                                     &metadata),
                 OI_OK);
        oi_cli_session_metadata_free(&metadata);
    }
}

static void remove_session(const char *root, const char *id) {
    char path[384];
    snprintf(path, sizeof path, "%s/%s/history.oilog", root, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s/metadata.json", root, id);
    unlink(path);
    /* Left behind by any metadata update that took the lock. */
    snprintf(path, sizeof path, "%s/%s/metadata.json.lock", root, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s", root, id);
    rmdir(path);
}

static const struct oi_cli_session_list_entry *find_entry(
    const struct oi_cli_session_list *list, const char *id) {
    size_t index;
    for (index = 0; index < list->len; index++) {
        if (strcmp(list->entries[index].id, id) == 0) {
            return &list->entries[index];
        }
    }
    return NULL;
}

TEST(enumerate_lists_sessions_newest_first_and_skips_non_sessions) {
    char root[192];
    char path[320];
    struct oi_cli_session_list list;

    snprintf(root, sizeof root, "/tmp/oi-session-enum-%ld", (long)getpid());
    seed_session(root, "sess-older", "gpt-a", "/tmp", 100, 200, 1);
    seed_session(root, "sess-newer", "gpt-b", "/tmp", 300, 400, 1);

    /* Entries that are not oi sessions must be skipped in silence. */
    snprintf(path, sizeof path, "%s/.trash", root);
    CHECK(mkdir(path, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof path, "%s/.hidden", root);
    CHECK(mkdir(path, 0700) == 0 || errno == EEXIST);
    snprintf(path, sizeof path, "%s/stray-file", root);
    {
        FILE *stray = fopen(path, "w");
        CHECK(stray != NULL);
        CHECK_EQ(fclose(stray), 0);
    }
    snprintf(path, sizeof path, "%s/a-symlink", root);
    unlink(path);
    CHECK_EQ(symlink("/tmp", path), 0);

    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 2);
    /* Most recently updated first. */
    CHECK_STREQ(list.entries[0].id, "sess-newer");
    CHECK_STREQ(list.entries[1].id, "sess-older");
    CHECK_STREQ(list.entries[0].model.data, "gpt-b");
    CHECK_STREQ(list.entries[0].cwd.data, "/tmp");
    CHECK_EQ(list.entries[0].created_at, 300);
    CHECK_EQ(list.entries[0].updated_at, 400);
    CHECK_EQ(list.entries[0].degraded, 0);
    CHECK_EQ(list.entries[0].lock_state, OI_CLI_SESSION_LOCK_FREE);
    oi_cli_session_list_free(&list);

    snprintf(path, sizeof path, "%s/a-symlink", root);
    CHECK_EQ(unlink(path), 0);
    snprintf(path, sizeof path, "%s/stray-file", root);
    CHECK_EQ(unlink(path), 0);
    snprintf(path, sizeof path, "%s/.trash", root);
    CHECK_EQ(rmdir(path), 0);
    snprintf(path, sizeof path, "%s/.hidden", root);
    CHECK_EQ(rmdir(path), 0);
    remove_session(root, "sess-older");
    remove_session(root, "sess-newer");
    CHECK_EQ(rmdir(root), 0);
}

TEST(enumerate_reports_an_empty_list_for_a_root_that_does_not_exist) {
    char root[192];
    struct oi_cli_session_list list;

    snprintf(root, sizeof root, "/tmp/oi-session-enum-absent-%ld",
             (long)getpid());
    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 0);
    oi_cli_session_list_free(&list);

    CHECK_EQ(oi_cli_sessions_enumerate(root, NULL), OI_ERR_INVAL);
}

TEST(enumerate_rebuilds_missing_and_malformed_metadata_from_history) {
    char root[192];
    char metadata_path[384];
    struct oi_cli_session_list list;
    const struct oi_cli_session_list_entry *entry;

    snprintf(root, sizeof root, "/tmp/oi-session-enum-degraded-%ld",
             (long)getpid());
    seed_session(root, "no-metadata", "gpt-recovered", "/tmp", 1, 2, 0);
    seed_session(root, "bad-metadata", "gpt-salvaged", "/tmp", 3, 4, 1);

    /* Corrupt the second session's cache. */
    snprintf(metadata_path, sizeof metadata_path, "%s/bad-metadata/"
             "metadata.json", root);
    {
        FILE *corrupt = fopen(metadata_path, "w");
        CHECK(corrupt != NULL);
        CHECK(fputs("{ this is not valid json", corrupt) >= 0);
        CHECK_EQ(fclose(corrupt), 0);
    }

    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 2);

    /* Both are still listed, both flagged, both recovered from the log --
     * which is the authoritative record the cache only ever mirrored. */
    entry = find_entry(&list, "no-metadata");
    CHECK(entry != NULL);
    CHECK_EQ(entry->degraded, 1);
    CHECK_STREQ(entry->model.data, "gpt-recovered");
    CHECK_STREQ(entry->cwd.data, "/tmp");
    /* mtime stands in for the timestamps the cache would have held. */
    CHECK(entry->updated_at > 0);

    entry = find_entry(&list, "bad-metadata");
    CHECK(entry != NULL);
    CHECK_EQ(entry->degraded, 1);
    CHECK_STREQ(entry->model.data, "gpt-salvaged");
    oi_cli_session_list_free(&list);

    remove_session(root, "no-metadata");
    remove_session(root, "bad-metadata");
    CHECK_EQ(rmdir(root), 0);
}

TEST(enumerate_distrusts_metadata_belonging_to_another_session) {
    char root[192];
    char metadata_path[384];
    struct oi_cli_session_list list;

    snprintf(root, sizeof root, "/tmp/oi-session-enum-mismatch-%ld",
             (long)getpid());
    seed_session(root, "real-session", "gpt-real", "/tmp", 100, 200, 1);
    CHECK_EQ(oi_cli_session_rename(root, "real-session", 12, "real name", 9,
                                   NULL),
             OI_OK);

    /*
     * A perfectly valid cache that names a *different* session -- a stray
     * copy, an interrupted manual move, a restored backup. It parses, so
     * without an ownership check this session would be listed as healthy
     * while displaying another session's model, cwd, and name.
     */
    snprintf(metadata_path, sizeof metadata_path,
             "%s/real-session/metadata.json", root);
    {
        struct oi_cli_session_metadata impostor;
        oi_cli_session_metadata_init(&impostor);
        CHECK_EQ(oi_cli_session_metadata_set(&impostor, "some-other-session",
                                             18, "gpt-wrong", 9,
                                             "/wrong/place", 12,
                                             "wrong name", 10, 1, 2),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_store_write(metadata_path,
                                                     &impostor),
                 OI_OK);
        oi_cli_session_metadata_free(&impostor);
    }

    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 1);
    CHECK_STREQ(list.entries[0].id, "real-session");
    /* Flagged, not trusted. */
    CHECK_EQ(list.entries[0].degraded, 1);
    /* Rebuilt from the history actually in this directory... */
    CHECK_STREQ(list.entries[0].model.data, "gpt-real");
    CHECK_STREQ(list.entries[0].cwd.data, "/tmp");
    /* ...and none of the impostor's values leaked into the row. */
    CHECK(list.entries[0].display_name.len == 0);
    oi_cli_session_list_free(&list);

    remove_session(root, "real-session");
    CHECK_EQ(rmdir(root), 0);
}

TEST(enumerate_lists_an_unreplayable_session_with_only_its_id) {
    char root[192];
    char history_path[384];
    struct oi_cli_session_list list;

    snprintf(root, sizeof root, "/tmp/oi-session-enum-garbage-%ld",
             (long)getpid());
    seed_session(root, "garbage-log", "gpt-x", "/tmp", 1, 2, 0);

    /* A log whose header is not an oi log at all: nothing is recoverable,
     * but the session must remain listable so it can be trashed. */
    snprintf(history_path, sizeof history_path, "%s/garbage-log/"
             "history.oilog", root);
    {
        FILE *garbage = fopen(history_path, "w");
        CHECK(garbage != NULL);
        CHECK(fputs("definitely not an oilog header", garbage) >= 0);
        CHECK_EQ(fclose(garbage), 0);
    }

    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 1);
    CHECK_STREQ(list.entries[0].id, "garbage-log");
    CHECK_EQ(list.entries[0].degraded, 1);
    CHECK(list.entries[0].model.data == NULL);
    CHECK(list.entries[0].cwd.data == NULL);
    oi_cli_session_list_free(&list);

    remove_session(root, "garbage-log");
    CHECK_EQ(rmdir(root), 0);
}

TEST(enumerate_reports_a_session_held_by_another_process_as_busy) {
    char root[192];
    char history_path[384];
    struct oi_cli_session_list list;
    int to_child[2];
    int to_parent[2];
    pid_t child;

    snprintf(root, sizeof root, "/tmp/oi-session-enum-busy-%ld",
             (long)getpid());
    seed_session(root, "held-session", "gpt-held", "/tmp", 10, 20, 1);
    seed_session(root, "free-session", "gpt-free", "/tmp", 30, 40, 1);
    snprintf(history_path, sizeof history_path, "%s/held-session/"
             "history.oilog", root);

    CHECK_EQ(pipe(to_child), 0);
    CHECK_EQ(pipe(to_parent), 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        /* Hold the log's lock exactly as a second live oi would, tell the
         * parent it is held, then wait to be released. */
        oi_sesslog *log = NULL;
        char byte = 0;
        close(to_child[1]);
        close(to_parent[0]);
        if (oi_sesslog_open(history_path, &log) != OI_OK) {
            _exit(1);
        }
        if (write(to_parent[1], "x", 1) != 1) {
            _exit(1);
        }
        if (read(to_child[0], &byte, 1) != 1) {
            _exit(1);
        }
        oi_sesslog_close(log);
        _exit(0);
    }
    close(to_child[0]);
    close(to_parent[1]);
    {
        char byte = 0;
        CHECK_EQ(read(to_parent[0], &byte, 1), 1);
    }

    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 2);
    /* Busy is a normal selectable state, not an error and not an omission. */
    CHECK_EQ(find_entry(&list, "held-session")->lock_state,
             OI_CLI_SESSION_LOCK_BUSY);
    CHECK_EQ(find_entry(&list, "held-session")->degraded, 0);
    CHECK_STREQ(find_entry(&list, "held-session")->model.data, "gpt-held");
    CHECK_EQ(find_entry(&list, "free-session")->lock_state,
             OI_CLI_SESSION_LOCK_FREE);
    oi_cli_session_list_free(&list);

    /* Release the holder; the same session now probes as free. */
    CHECK_EQ(write(to_child[1], "x", 1), 1);
    {
        int wait_status = 0;
        CHECK_EQ(waitpid(child, &wait_status, 0), child);
        CHECK(WIFEXITED(wait_status));
        CHECK_EQ(WEXITSTATUS(wait_status), 0);
    }
    close(to_child[1]);
    close(to_parent[0]);

    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(find_entry(&list, "held-session")->lock_state,
             OI_CLI_SESSION_LOCK_FREE);
    oi_cli_session_list_free(&list);

    remove_session(root, "held-session");
    remove_session(root, "free-session");
    CHECK_EQ(rmdir(root), 0);
}

TEST(enumerate_does_not_rewrite_the_logs_it_lists) {
    char root[192];
    char history_path[384];
    struct oi_cli_session_list list;
    struct stat before;
    struct stat after;

    snprintf(root, sizeof root, "/tmp/oi-session-enum-readonly-%ld",
             (long)getpid());
    seed_session(root, "untouched", "gpt-a", "/tmp", 1, 2, 1);
    snprintf(history_path, sizeof history_path, "%s/untouched/history.oilog",
             root);
    CHECK_EQ(stat(history_path, &before), 0);

    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 1);
    oi_cli_session_list_free(&list);

    /* Listing is a read: the healthy path takes no exclusive lock and
     * must leave every byte and the mtime alone. */
    CHECK_EQ(stat(history_path, &after), 0);
    CHECK_EQ(before.st_size, after.st_size);
    CHECK_EQ(before.st_mtime, after.st_mtime);

    /*
     * The case that actually distinguishes a read-only flock probe from
     * opening the log through oi_sesslog_open: a record left incomplete
     * by a crash. oi_sesslog_open recovers such a log by truncating that
     * trailing fragment, which is right when a session is being opened
     * for use and wrong as a side effect of listing. With the cache
     * intact there is no reason to touch the log at all, so the fragment
     * must survive being listed.
     */
    {
        /* A length prefix claiming far more than the bytes that follow --
         * exactly what a crash mid-append leaves behind. */
        static const unsigned char fragment[] = {
            0xff, 0xff, 0xff, 0xff, 'p', 'a', 'r', 't'};
        FILE *appender = fopen(history_path, "ab");
        CHECK(appender != NULL);
        CHECK_EQ(fwrite(fragment, 1, sizeof fragment, appender),
                 sizeof fragment);
        CHECK_EQ(fclose(appender), 0);
    }
    CHECK_EQ(stat(history_path, &before), 0);

    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 1);
    /* Cache still valid, so the entry reads clean and needs no replay. */
    CHECK_EQ(list.entries[0].degraded, 0);
    oi_cli_session_list_free(&list);

    CHECK_EQ(stat(history_path, &after), 0);
    CHECK_EQ(before.st_size, after.st_size);

    remove_session(root, "untouched");
    CHECK_EQ(rmdir(root), 0);
}

/* --- oi_cli_session_rename --- */

static struct oi_cli_string read_display_name(const char *root,
                                              const char *id) {
    char metadata_path[384];
    struct oi_cli_session_metadata metadata;
    struct oi_cli_string name;

    memset(&name, 0, sizeof name);
    snprintf(metadata_path, sizeof metadata_path, "%s/%s/metadata.json", root,
             id);
    oi_cli_session_metadata_init(&metadata);
    if (oi_cli_session_metadata_store_read(metadata_path, &metadata) ==
            OI_OK &&
        metadata.display_name.len > 0) {
        CHECK_EQ(oi_cli_string_set(&name, metadata.display_name.data,
                                   metadata.display_name.len),
                 OI_OK);
    }
    oi_cli_session_metadata_free(&metadata);
    return name;
}

TEST(rename_sets_a_display_name_without_moving_the_directory) {
    char root[192];
    char directory[320];
    struct oi_cli_session_list list;
    struct oi_cli_string name;
    struct stat info;

    snprintf(root, sizeof root, "/tmp/oi-session-rename-%ld", (long)getpid());
    seed_session(root, "sess-rename", "gpt-a", "/tmp", 100, 200, 1);
    snprintf(directory, sizeof directory, "%s/sess-rename", root);

    CHECK_EQ(oi_cli_session_rename(root, "sess-rename", 11, "my refactor", 11,
                                   NULL),
             OI_OK);

    /* The name is recorded... */
    name = read_display_name(root, "sess-rename");
    CHECK_STREQ(name.data, "my refactor");
    oi_cli_string_free(&name);
    /* ...and the directory keeps its original id, because every derived
     * path depends on it. */
    CHECK_EQ(stat(directory, &info), 0);
    CHECK(S_ISDIR(info.st_mode));

    /* Renaming again replaces the previous name. */
    CHECK_EQ(oi_cli_session_rename(root, "sess-rename", 11, "second", 6,
                                   NULL),
             OI_OK);
    name = read_display_name(root, "sess-rename");
    CHECK_STREQ(name.data, "second");
    oi_cli_string_free(&name);

    /* And the model/cwd the cache carried are untouched by all of this. */
    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 1);
    CHECK_STREQ(list.entries[0].display_name.data, "second");
    CHECK_STREQ(list.entries[0].model.data, "gpt-a");
    CHECK_STREQ(list.entries[0].cwd.data, "/tmp");
    CHECK_EQ(list.entries[0].created_at, 100);
    oi_cli_session_list_free(&list);

    remove_session(root, "sess-rename");
    CHECK_EQ(rmdir(root), 0);
}

TEST(rename_repairs_a_missing_metadata_cache_from_history) {
    char root[192];
    struct oi_cli_string name;

    snprintf(root, sizeof root, "/tmp/oi-session-rename-repair-%ld",
             (long)getpid());
    /* No metadata.json at all: the name still has to land somewhere, so
     * the cache is rebuilt from history first. */
    seed_session(root, "no-cache", "gpt-recovered", "/tmp", 0, 0, 0);

    CHECK_EQ(oi_cli_session_rename(root, "no-cache", 8, "named anyway", 12,
                                   NULL),
             OI_OK);
    name = read_display_name(root, "no-cache");
    CHECK_STREQ(name.data, "named anyway");
    oi_cli_string_free(&name);
    {
        /* Repaired, not invented: model and cwd came from the log. */
        struct oi_cli_session_list list;
        oi_cli_session_list_init(&list);
        CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
        CHECK_EQ(list.len, 1);
        CHECK_EQ(list.entries[0].degraded, 0);
        CHECK_STREQ(list.entries[0].model.data, "gpt-recovered");
        oi_cli_session_list_free(&list);
    }

    remove_session(root, "no-cache");
    CHECK_EQ(rmdir(root), 0);
}

TEST(rename_merges_with_the_current_cache_not_a_stale_copy) {
    char root[192];
    char metadata_path[384];
    struct oi_cli_string name;

    snprintf(root, sizeof root, "/tmp/oi-session-rename-merge-%ld",
             (long)getpid());
    seed_session(root, "sess-merge", "gpt-first", "/tmp", 100, 200, 1);
    snprintf(metadata_path, sizeof metadata_path,
             "%s/sess-merge/metadata.json", root);

    CHECK_EQ(oi_cli_session_rename(root, "sess-merge", 10, "first name", 10,
                                   NULL),
             OI_OK);

    /* Another process changes the model, the way /model does. */
    {
        struct oi_cli_session_metadata changed;
        oi_cli_session_metadata_init(&changed);
        CHECK_EQ(oi_cli_session_metadata_store_read(metadata_path, &changed),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_set(
                     &changed, "sess-merge", 10, "gpt-second", 10, "/tmp", 4,
                     changed.display_name.data, changed.display_name.len,
                     changed.created_at, changed.updated_at),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_store_write(metadata_path, &changed),
                 OI_OK);
        oi_cli_session_metadata_free(&changed);
    }

    /* A second rename must merge against what is on disk now, not against
     * anything it read earlier, so the new model survives. */
    CHECK_EQ(oi_cli_session_rename(root, "sess-merge", 10, "second name", 11,
                                   NULL),
             OI_OK);
    {
        struct oi_cli_session_metadata after;
        oi_cli_session_metadata_init(&after);
        CHECK_EQ(oi_cli_session_metadata_store_read(metadata_path, &after),
                 OI_OK);
        CHECK_STREQ(after.model.data, "gpt-second");
        CHECK_STREQ(after.display_name.data, "second name");
        oi_cli_session_metadata_free(&after);
    }
    name = read_display_name(root, "sess-merge");
    CHECK_STREQ(name.data, "second name");
    oi_cli_string_free(&name);

    remove_session(root, "sess-merge");
    CHECK_EQ(rmdir(root), 0);
}

/* Waits a beat, long enough that a rename which was never going to block
 * would certainly have finished. */
static void short_pause(void) {
    struct timespec pause;
    pause.tv_sec = 0;
    pause.tv_nsec = 300L * 1000L * 1000L;
    (void)nanosleep(&pause, NULL);
}

TEST(a_rename_waits_for_a_settings_update_and_merges_its_value) {
    char root[192];
    char metadata_path[384];
    char lock_path[420];
    int lock_fd;
    pid_t child;

    /*
     * Both updaters read metadata.json, merge one field, and replace the
     * file. Interleaved, each discards the other's field -- a rename that
     * read before a concurrent /model landed writes the stale model back.
     *
     * The window is far too narrow to hit reliably by hammering, so this
     * tests the property that closes it instead: a rename cannot read until
     * whoever is mid-update has finished. The test holds the metadata lock
     * itself -- reaching into an implementation detail deliberately,
     * because mutual exclusion is exactly what is being asserted.
     */
    snprintf(root, sizeof root, "/tmp/oi-session-race-%ld", (long)getpid());
    seed_session(root, "racer", "gpt-initial", "/tmp", 100, 200, 1);
    snprintf(metadata_path, sizeof metadata_path, "%s/racer/metadata.json",
             root);
    snprintf(lock_path, sizeof lock_path, "%s.lock", metadata_path);

    /* Stand in for a live process partway through refreshing settings: hold
     * the lock, then publish a new model while still holding it. */
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    CHECK(lock_fd >= 0);
    CHECK_EQ(flock(lock_fd, LOCK_EX), 0);
    {
        struct oi_cli_session_metadata updated;
        oi_cli_session_metadata_init(&updated);
        CHECK_EQ(oi_cli_session_metadata_set(&updated, "racer", 5,
                                             "gpt-committed", 13, "/tmp", 4,
                                             NULL, 0, 100, 300),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_store_write(metadata_path, &updated),
                 OI_OK);
        oi_cli_session_metadata_free(&updated);
    }

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(lock_fd);
        _exit(oi_cli_session_rename(root, "racer", 5, "chosen name", 11,
                                    NULL) == OI_OK
                  ? 0
                  : 1);
    }

    /* While the lock is held the rename must not have landed. */
    short_pause();
    {
        struct oi_cli_session_metadata midway;
        oi_cli_session_metadata_init(&midway);
        CHECK_EQ(oi_cli_session_metadata_store_read(metadata_path, &midway),
                 OI_OK);
        CHECK_EQ(midway.display_name.len, (size_t)0);
        oi_cli_session_metadata_free(&midway);
    }

    /* Release, and let the rename proceed. */
    CHECK_EQ(flock(lock_fd, LOCK_UN), 0);
    close(lock_fd);
    {
        int wait_status = 0;
        CHECK_EQ(waitpid(child, &wait_status, 0), child);
        CHECK(WIFEXITED(wait_status));
        CHECK_EQ(WEXITSTATUS(wait_status), 0);
    }

    /*
     * Both fields survive. The model assertion is the anti-lost-update one:
     * the rename read after acquiring the lock, so it saw "gpt-committed"
     * and carried it forward. Reading before locking would have merged the
     * pre-update model and silently reverted the change.
     */
    {
        struct oi_cli_session_metadata final_state;
        oi_cli_session_metadata_init(&final_state);
        CHECK_EQ(oi_cli_session_metadata_store_read(metadata_path,
                                                    &final_state),
                 OI_OK);
        CHECK_STREQ(final_state.session_id.data, "racer");
        CHECK_STREQ(final_state.model.data, "gpt-committed");
        CHECK_STREQ(final_state.display_name.data, "chosen name");
        oi_cli_session_metadata_free(&final_state);
    }

    remove_session(root, "racer");
    CHECK_EQ(rmdir(root), 0);
}

TEST(a_planted_lock_symlink_is_refused_and_never_followed) {
    char root[192];
    char history_path[384];
    char metadata_path[384];
    char lock_path[420];
    char decoy_path[192];
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    oi_sesslog *log = NULL;
    char *detail = NULL;

    /*
     * The lock path is the one path in this module built by appending to a
     * name rather than resolved through oi_cli_session_resolve, so it needs
     * its own symlink defence: followed, a planted "metadata.json.lock"
     * would be opened O_RDWR|O_CREAT at whatever it pointed to.
     */
    snprintf(root, sizeof root, "/tmp/oi-session-locksym-%ld",
             (long)getpid());
    seed_session(root, "sess-lock", "gpt-before", "/tmp", 100, 200, 1);
    snprintf(history_path, sizeof history_path, "%s/sess-lock/history.oilog",
             root);
    snprintf(metadata_path, sizeof metadata_path,
             "%s/sess-lock/metadata.json", root);
    snprintf(lock_path, sizeof lock_path, "%s.lock", metadata_path);
    snprintf(decoy_path, sizeof decoy_path, "/tmp/oi-locksym-decoy-%ld",
             (long)getpid());

    {
        FILE *decoy = fopen(decoy_path, "w");
        CHECK(decoy != NULL);
        CHECK(fputs("do not touch", decoy) >= 0);
        CHECK_EQ(fclose(decoy), 0);
    }
    unlink(lock_path);
    CHECK_EQ(symlink(decoy_path, lock_path), 0);

    /* Rename cannot skip: the metadata write is the whole operation, so it
     * must fail rather than proceed unlocked. */
    CHECK_EQ(oi_cli_session_rename(root, "sess-lock", 9, "new name", 8,
                                   &detail),
             OI_ERR_IO);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "lock") != NULL);
    free(detail);
    /* No name was recorded... */
    {
        struct oi_cli_string name = read_display_name(root, "sess-lock");
        CHECK(name.data == NULL);
        oi_cli_string_free(&name);
    }
    /* ...and the symlink target was neither followed nor written. */
    {
        char *contents = NULL;
        size_t len = 0;
        struct stat info;
        read_whole_file(decoy_path, &contents, &len);
        CHECK_EQ(len, (size_t)12);
        CHECK_EQ(memcmp(contents, "do not touch", 12), 0);
        free(contents);
        CHECK_EQ(lstat(lock_path, &info), 0);
        CHECK(S_ISLNK(info.st_mode));
    }

    /*
     * A settings change is the opposite case: history is authoritative and
     * already durable, so the optional cache refresh is skipped rather than
     * raced, and the change itself still succeeds.
     */
    CHECK_EQ(oi_sesslog_open(history_path, &log), OI_OK);
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);
    CHECK_EQ(oi_cli_session_apply_setting(
                 &store, &state, metadata_path, "sess-lock",
                 OI_CLI_HISTORY_SESSION_SETTING_MODEL, "gpt-after", 9),
             OI_OK);
    /* Durable in history... */
    CHECK_STREQ(state.last_model.data, "gpt-after");
    /* ...while the cache was left alone rather than written unlocked. */
    {
        struct oi_cli_session_metadata stale;
        oi_cli_session_metadata_init(&stale);
        CHECK_EQ(oi_cli_session_metadata_store_read(metadata_path, &stale),
                 OI_OK);
        CHECK_STREQ(stale.model.data, "gpt-before");
        oi_cli_session_metadata_free(&stale);
    }
    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);

    /* With the symlink gone, both operations work again and the cache
     * self-heals to the durable truth. */
    CHECK_EQ(unlink(lock_path), 0);
    CHECK_EQ(oi_cli_session_rename(root, "sess-lock", 9, "new name", 8, NULL),
             OI_OK);
    {
        struct oi_cli_string name = read_display_name(root, "sess-lock");
        CHECK_STREQ(name.data, "new name");
        oi_cli_string_free(&name);
    }

    CHECK_EQ(unlink(decoy_path), 0);
    remove_session(root, "sess-lock");
    CHECK_EQ(rmdir(root), 0);
}

TEST(rename_refuses_unnameable_and_invalid_targets) {
    char root[192];
    char history_path[384];
    char *detail = NULL;

    snprintf(root, sizeof root, "/tmp/oi-session-rename-bad-%ld",
             (long)getpid());
    seed_session(root, "sess-ok", "gpt-a", "/tmp", 100, 200, 1);

    /* Unsafe ids never reach the filesystem. */
    CHECK_EQ(oi_cli_session_rename(root, "..", 2, "x", 1, &detail),
             OI_ERR_INVAL);
    CHECK_STREQ(detail, "invalid session id");
    free(detail);
    detail = NULL;
    CHECK_EQ(oi_cli_session_rename(root, "a/b", 3, "x", 1, NULL),
             OI_ERR_INVAL);

    /* A session that does not exist. */
    CHECK_EQ(oi_cli_session_rename(root, "absent", 6, "x", 1, &detail),
             OI_ERR_NOTFOUND);
    CHECK_STREQ(detail, "no such session");
    free(detail);
    detail = NULL;

    /* Names that would be unsafe or useless to display. */
    CHECK_EQ(oi_cli_session_rename(root, "sess-ok", 7, "", 0, &detail),
             OI_ERR_INVAL);
    CHECK_STREQ(detail, "a session name cannot be empty");
    free(detail);
    detail = NULL;
    CHECK_EQ(oi_cli_session_rename(root, "sess-ok", 7, NULL, 0, NULL),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_rename(root, "sess-ok", 7, "two\nlines", 9,
                                   &detail),
             OI_ERR_INVAL);
    CHECK_STREQ(detail,
                "a session name cannot contain control characters");
    free(detail);
    detail = NULL;
    {
        char oversized[OI_CLI_SESSION_METADATA_MAX_DISPLAY_NAME + 1];
        memset(oversized, 'a', sizeof oversized);
        CHECK_EQ(oi_cli_session_rename(root, "sess-ok", 7, oversized,
                                       sizeof oversized, &detail),
                 OI_ERR_INVAL);
        CHECK_STREQ(detail, "session name is too long");
        free(detail);
        detail = NULL;
    }
    /* A rejected rename leaves the session exactly as it was. */
    {
        struct oi_cli_string name = read_display_name(root, "sess-ok");
        CHECK(name.data == NULL);
        oi_cli_string_free(&name);
    }

    /*
     * A session with neither a cache nor a readable log has no model or
     * cwd to write alongside a name, and filling those in with invented
     * values would corrupt what a later open trusts. Refuse instead.
     */
    seed_session(root, "hopeless", "gpt-x", "/tmp", 0, 0, 0);
    snprintf(history_path, sizeof history_path, "%s/hopeless/history.oilog",
             root);
    {
        FILE *garbage = fopen(history_path, "w");
        CHECK(garbage != NULL);
        CHECK(fputs("not an oilog", garbage) >= 0);
        CHECK_EQ(fclose(garbage), 0);
    }
    CHECK_EQ(oi_cli_session_rename(root, "hopeless", 8, "wishful", 7, &detail),
             OI_ERR_IO);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "cannot be read") != NULL);
    free(detail);

    remove_session(root, "sess-ok");
    remove_session(root, "hopeless");
    CHECK_EQ(rmdir(root), 0);
}

/* --- trash / restore / delete --- */

static int path_is_directory(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

static int path_is_directory_of(const char *root, const char *id) {
    char path[384];
    snprintf(path, sizeof path, "%s/%s", root, id);
    return path_is_directory(path);
}


TEST(trash_and_restore_round_trip_preserving_history) {
    char root[192];
    char live[320];
    char trashed[384];
    char history_path[448];
    struct oi_cli_session_list list;
    off_t original_size;

    snprintf(root, sizeof root, "/tmp/oi-session-trash-%ld", (long)getpid());
    seed_session(root, "sess-trash", "gpt-a", "/tmp", 100, 200, 1);
    CHECK_EQ(oi_cli_session_rename(root, "sess-trash", 10, "keep me", 7, NULL),
             OI_OK);
    snprintf(live, sizeof live, "%s/sess-trash", root);
    snprintf(trashed, sizeof trashed, "%s/.trash/sess-trash", root);
    snprintf(history_path, sizeof history_path, "%s/history.oilog", live);
    {
        struct stat info;
        CHECK_EQ(stat(history_path, &info), 0);
        original_size = info.st_size;
    }

    CHECK_EQ(oi_cli_session_trash(root, "sess-trash", 10, NULL, NULL), OI_OK);
    /* The whole directory moved; nothing was copied or rewritten. */
    CHECK(!path_is_directory(live));
    CHECK(path_is_directory(trashed));
    {
        struct stat info;
        snprintf(history_path, sizeof history_path, "%s/history.oilog",
                 trashed);
        CHECK_EQ(stat(history_path, &info), 0);
        CHECK_EQ(info.st_size, original_size);
    }

    /* A trashed session drops out of the live listing with no filtering
     * logic -- it is simply not in the root anymore. */
    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 0);
    oi_cli_session_list_free(&list);
    /* ...and shows up in the trash listing instead, name intact. */
    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate_trash(root, &list), OI_OK);
    CHECK_EQ(list.len, 1);
    CHECK_STREQ(list.entries[0].id, "sess-trash");
    CHECK_STREQ(list.entries[0].display_name.data, "keep me");
    CHECK_STREQ(list.entries[0].model.data, "gpt-a");
    oi_cli_session_list_free(&list);

    CHECK_EQ(oi_cli_session_restore_trashed(root, "sess-trash", 10, NULL),
             OI_OK);
    CHECK(path_is_directory(live));
    CHECK(!path_is_directory(trashed));
    oi_cli_session_list_init(&list);
    CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
    CHECK_EQ(list.len, 1);
    CHECK_STREQ(list.entries[0].display_name.data, "keep me");
    oi_cli_session_list_free(&list);

    remove_session(root, "sess-trash");
    {
        char trash_root[256];
        snprintf(trash_root, sizeof trash_root, "%s/.trash", root);
        rmdir(trash_root);
    }
    CHECK_EQ(rmdir(root), 0);
}

TEST(trash_refuses_the_active_session_and_unknown_ids) {
    char root[192];
    char *detail = NULL;

    snprintf(root, sizeof root, "/tmp/oi-session-trash-refuse-%ld",
             (long)getpid());
    seed_session(root, "sess-active", "gpt-a", "/tmp", 100, 200, 1);

    /* Trashing the session you are sitting in would leave the running
     * process writing into a directory that has moved. */
    CHECK_EQ(oi_cli_session_trash(root, "sess-active", 11, "sess-active",
                                  &detail),
             OI_ERR_INVAL);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "active session") != NULL);
    free(detail);
    detail = NULL;
    CHECK(path_is_directory_of(root, "sess-active"));

    /* A different active session is no obstacle. */
    CHECK_EQ(oi_cli_session_trash(root, "sess-active", 11, "some-other",
                                  &detail),
             OI_OK);
    CHECK_EQ(oi_cli_session_restore_trashed(root, "sess-active", 11, NULL),
             OI_OK);

    CHECK_EQ(oi_cli_session_trash(root, "absent", 6, NULL, &detail),
             OI_ERR_NOTFOUND);
    CHECK_STREQ(detail, "no such session");
    free(detail);
    detail = NULL;
    CHECK_EQ(oi_cli_session_trash(root, "..", 2, NULL, &detail),
             OI_ERR_INVAL);
    CHECK_STREQ(detail, "invalid session id");
    free(detail);

    remove_session(root, "sess-active");
    {
        char trash_root[256];
        snprintf(trash_root, sizeof trash_root, "%s/.trash", root);
        rmdir(trash_root);
    }
    CHECK_EQ(rmdir(root), 0);
}

TEST(trash_refuses_a_session_open_in_another_process) {
    char root[192];
    char history_path[384];
    char *detail = NULL;
    int to_child[2];
    int to_parent[2];
    pid_t child;

    snprintf(root, sizeof root, "/tmp/oi-session-trash-busy-%ld",
             (long)getpid());
    seed_session(root, "sess-held", "gpt-a", "/tmp", 100, 200, 1);
    snprintf(history_path, sizeof history_path, "%s/sess-held/history.oilog",
             root);

    CHECK_EQ(pipe(to_child), 0);
    CHECK_EQ(pipe(to_parent), 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        oi_sesslog *log = NULL;
        char byte = 0;
        close(to_child[1]);
        close(to_parent[0]);
        if (oi_sesslog_open(history_path, &log) != OI_OK) {
            _exit(1);
        }
        if (write(to_parent[1], "x", 1) != 1) {
            _exit(1);
        }
        if (read(to_child[0], &byte, 1) != 1) {
            _exit(1);
        }
        oi_sesslog_close(log);
        _exit(0);
    }
    close(to_child[0]);
    close(to_parent[1]);
    {
        char byte = 0;
        CHECK_EQ(read(to_parent[0], &byte, 1), 1);
    }

    /* Moving a directory out from under a live process is exactly what
     * durable history exists to prevent. */
    CHECK_EQ(oi_cli_session_trash(root, "sess-held", 9, NULL, &detail),
             OI_ERR_EXISTS);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "another process") != NULL);
    free(detail);
    CHECK(path_is_directory_of(root, "sess-held"));

    CHECK_EQ(write(to_child[1], "x", 1), 1);
    {
        int wait_status = 0;
        CHECK_EQ(waitpid(child, &wait_status, 0), child);
        CHECK(WIFEXITED(wait_status));
        CHECK_EQ(WEXITSTATUS(wait_status), 0);
    }
    close(to_child[1]);
    close(to_parent[0]);

    /* Released, so the same call now succeeds. */
    CHECK_EQ(oi_cli_session_trash(root, "sess-held", 9, NULL, NULL), OI_OK);

    CHECK_EQ(oi_cli_session_delete(root, "sess-held", 9, NULL), OI_OK);
    {
        char trash_root[256];
        snprintf(trash_root, sizeof trash_root, "%s/.trash", root);
        CHECK_EQ(rmdir(trash_root), 0);
    }
    CHECK_EQ(rmdir(root), 0);
}

TEST(delete_only_reaches_trashed_sessions_and_removes_them_completely) {
    char root[192];
    char trashed[384];
    char *detail = NULL;

    snprintf(root, sizeof root, "/tmp/oi-session-delete-%ld", (long)getpid());
    seed_session(root, "sess-live", "gpt-a", "/tmp", 100, 200, 1);
    snprintf(trashed, sizeof trashed, "%s/.trash/sess-live", root);

    /*
     * A live session is not in the trash, so delete cannot find it -- that
     * is what makes "refuses the current session" structural rather than a
     * check that could be forgotten.
     */
    CHECK_EQ(oi_cli_session_delete(root, "sess-live", 9, &detail),
             OI_ERR_NOTFOUND);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "trash it") != NULL);
    free(detail);
    detail = NULL;
    CHECK(path_is_directory_of(root, "sess-live"));

    CHECK_EQ(oi_cli_session_trash(root, "sess-live", 9, NULL, NULL), OI_OK);
    CHECK(path_is_directory(trashed));
    CHECK_EQ(oi_cli_session_delete(root, "sess-live", 9, NULL), OI_OK);
    /* Gone: files and directory both. */
    CHECK(!path_is_directory(trashed));
    {
        struct oi_cli_session_list list;
        oi_cli_session_list_init(&list);
        CHECK_EQ(oi_cli_sessions_enumerate_trash(root, &list), OI_OK);
        CHECK_EQ(list.len, 0);
        oi_cli_session_list_free(&list);
    }
    /* And a second delete simply reports nothing to delete. */
    CHECK_EQ(oi_cli_session_delete(root, "sess-live", 9, NULL),
             OI_ERR_NOTFOUND);
    CHECK_EQ(oi_cli_session_delete(root, "..", 2, NULL), OI_ERR_INVAL);

    {
        char trash_root[256];
        snprintf(trash_root, sizeof trash_root, "%s/.trash", root);
        CHECK_EQ(rmdir(trash_root), 0);
    }
    CHECK_EQ(rmdir(root), 0);
}

TEST(delete_fails_closed_on_a_directory_it_did_not_create) {
    char root[192];
    char trashed[384];
    char nested[448];
    char history_path[448];
    char metadata_path[448];
    char *detail = NULL;
    off_t history_size;

    snprintf(root, sizeof root, "/tmp/oi-session-delete-closed-%ld",
             (long)getpid());
    seed_session(root, "sess-odd", "gpt-a", "/tmp", 100, 200, 1);
    CHECK_EQ(oi_cli_session_rename(root, "sess-odd", 8, "keep me", 7, NULL),
             OI_OK);
    CHECK_EQ(oi_cli_session_trash(root, "sess-odd", 8, NULL, NULL), OI_OK);

    snprintf(trashed, sizeof trashed, "%s/.trash/sess-odd", root);
    snprintf(history_path, sizeof history_path, "%s/history.oilog", trashed);
    snprintf(metadata_path, sizeof metadata_path, "%s/metadata.json",
             trashed);
    {
        struct stat info;
        CHECK_EQ(stat(history_path, &info), 0);
        history_size = info.st_size;
    }

    /* Something oi never puts in a session directory. */
    snprintf(nested, sizeof nested, "%s/unexpected", trashed);
    CHECK_EQ(mkdir(nested, 0700), 0);

    /* Rather than recursing into whatever it finds, deletion stops. */
    CHECK_EQ(oi_cli_session_delete(root, "sess-odd", 8, &detail), OI_ERR_IO);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "unexpected entry") != NULL);
    free(detail);
    CHECK(path_is_directory(nested));

    /*
     * Failing closed has to mean the session is still there. Deciding and
     * deleting in one pass used to unlink the history and metadata first and
     * only then hit the unexpected entry, reporting failure over a session
     * that could no longer be restored.
     */
    {
        struct stat info;
        CHECK_EQ(stat(history_path, &info), 0);
        CHECK_EQ(info.st_size, history_size);
        CHECK_EQ(stat(metadata_path, &info), 0);
    }
    /* And it is still genuinely restorable, name and all. */
    CHECK_EQ(rmdir(nested), 0);
    CHECK_EQ(oi_cli_session_restore_trashed(root, "sess-odd", 8, NULL),
             OI_OK);
    {
        struct oi_cli_string name = read_display_name(root, "sess-odd");
        CHECK_STREQ(name.data, "keep me");
        oi_cli_string_free(&name);
    }

    CHECK_EQ(oi_cli_session_trash(root, "sess-odd", 8, NULL, NULL), OI_OK);
    CHECK_EQ(oi_cli_session_delete(root, "sess-odd", 8, NULL), OI_OK);
    {
        char trash_root[256];
        snprintf(trash_root, sizeof trash_root, "%s/.trash", root);
        CHECK_EQ(rmdir(trash_root), 0);
    }
    CHECK_EQ(rmdir(root), 0);
}

TEST(trash_reports_a_cross_device_trash_directory_distinctly) {
    char root[192];
    char trash_link[256];
    char foreign_trash[192];
    char *detail = NULL;
    int reachable;

    snprintf(root, sizeof root, "/tmp/oi-session-exdev-%ld", (long)getpid());
    /*
     * The trash normally lives inside the sessions root, making every
     * trash/restore a same-filesystem rename. A user who relocates it --
     * here, a symlink onto another filesystem -- deserves a specific
     * message rather than a bare I/O error.
     *
     * Skipped where /dev/shm is not a separate filesystem, since EXDEV
     * then genuinely cannot be provoked.
     */
    snprintf(foreign_trash, sizeof foreign_trash, "/dev/shm/oi-exdev-%ld",
             (long)getpid());
    {
        struct stat tmp_info;
        struct stat shm_info;
        reachable = stat("/tmp", &tmp_info) == 0 &&
                    stat("/dev/shm", &shm_info) == 0 &&
                    tmp_info.st_dev != shm_info.st_dev &&
                    mkdir(foreign_trash, 0700) == 0;
    }
    if (!reachable) {
        return;
    }

    seed_session(root, "sess-exdev", "gpt-a", "/tmp", 100, 200, 1);
    snprintf(trash_link, sizeof trash_link, "%s/.trash", root);
    CHECK_EQ(symlink(foreign_trash, trash_link), 0);

    CHECK_EQ(oi_cli_session_trash(root, "sess-exdev", 10, NULL, &detail),
             OI_ERR_IO);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "different filesystem") != NULL);
    free(detail);
    /* Failure-atomic: the session is still exactly where it was. */
    CHECK(path_is_directory_of(root, "sess-exdev"));

    CHECK_EQ(unlink(trash_link), 0);
    remove_session(root, "sess-exdev");
    CHECK_EQ(rmdir(root), 0);
    CHECK_EQ(rmdir(foreign_trash), 0);
}

TEST(restore_refuses_unknown_ids_and_will_not_clobber_a_live_session) {
    char root[192];
    char *detail = NULL;

    snprintf(root, sizeof root, "/tmp/oi-session-restore-bad-%ld",
             (long)getpid());
    seed_session(root, "sess-dup", "gpt-a", "/tmp", 100, 200, 1);

    CHECK_EQ(oi_cli_session_restore_trashed(root, "absent", 6, &detail),
             OI_ERR_NOTFOUND);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "no trashed session") != NULL);
    free(detail);
    detail = NULL;
    CHECK_EQ(oi_cli_session_restore_trashed(root, "..", 2, &detail),
             OI_ERR_INVAL);
    CHECK_STREQ(detail, "invalid session id");
    free(detail);
    detail = NULL;

    /* Contrive the collision generated ids make impossible, and confirm
     * restore refuses rather than renaming over a live session. */
    CHECK_EQ(oi_cli_session_trash(root, "sess-dup", 8, NULL, NULL), OI_OK);
    seed_session(root, "sess-dup", "gpt-live", "/tmp", 300, 400, 1);
    CHECK_EQ(oi_cli_session_restore_trashed(root, "sess-dup", 8, &detail),
             OI_ERR_EXISTS);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "already exists") != NULL);
    free(detail);
    /* The live session is untouched. */
    {
        struct oi_cli_session_list list;
        oi_cli_session_list_init(&list);
        CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
        CHECK_EQ(list.len, 1);
        CHECK_STREQ(list.entries[0].model.data, "gpt-live");
        oi_cli_session_list_free(&list);
    }

    CHECK_EQ(oi_cli_session_delete(root, "sess-dup", 8, NULL), OI_OK);
    remove_session(root, "sess-dup");
    {
        char trash_root[256];
        snprintf(trash_root, sizeof trash_root, "%s/.trash", root);
        CHECK_EQ(rmdir(trash_root), 0);
    }
    CHECK_EQ(rmdir(root), 0);
}

/* --- oi_cli_session_import --- */

/*
 * Writes a legacy-format log: raw alternating user/assistant payloads with
 * no transition record, exactly the shape test_cli_history_store.c uses
 * for its own legacy-replay coverage.
 */
static void write_legacy_log(const char *path) {
    oi_sesslog *log = NULL;
    unlink(path);
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "old question", 12), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "old answer", 10), OI_OK);
    oi_sesslog_close(log);
}

TEST(import_adopts_a_legacy_log_and_leaves_the_source_untouched) {
    char root[192];
    char source[192];
    char *new_id = NULL;
    char *before = NULL;
    char *after = NULL;
    size_t before_len = 0;
    size_t after_len = 0;
    struct stat before_info;
    struct stat after_info;

    snprintf(root, sizeof root, "/tmp/oi-session-import-%ld", (long)getpid());
    snprintf(source, sizeof source, "/tmp/oi-legacy-source-%ld.oilog",
             (long)getpid());
    write_legacy_log(source);
    read_whole_file(source, &before, &before_len);
    CHECK_EQ(stat(source, &before_info), 0);

    CHECK_EQ(oi_cli_session_import(root, source, strlen(source), &new_id,
                                   NULL),
             OI_OK);
    CHECK(new_id != NULL);
    CHECK(oi_cli_session_id_is_safe(new_id, strlen(new_id)));

    /* The source is preserved byte for byte and not even its mtime moved:
     * import only ever reads it. */
    read_whole_file(source, &after, &after_len);
    CHECK_EQ(after_len, before_len);
    CHECK_EQ(memcmp(before, after, before_len), 0);
    CHECK_EQ(stat(source, &after_info), 0);
    CHECK_EQ(before_info.st_mtime, after_info.st_mtime);

    /* The new session is a real, selectable session whose history is the
     * imported content -- replayed here through the ordinary listing path. */
    {
        struct oi_cli_session_list list;
        char imported_path[384];
        char *imported = NULL;
        size_t imported_len = 0;

        oi_cli_session_list_init(&list);
        CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
        CHECK_EQ(list.len, 1);
        CHECK_STREQ(list.entries[0].id, new_id);
        oi_cli_session_list_free(&list);

        snprintf(imported_path, sizeof imported_path, "%s/%s/history.oilog",
                 root, new_id);
        read_whole_file(imported_path, &imported, &imported_len);
        CHECK_EQ(imported_len, before_len);
        CHECK_EQ(memcmp(before, imported, before_len), 0);
        free(imported);
    }
    /* No scratch file left lying around. */
    {
        char scratch[256];
        snprintf(scratch, sizeof scratch, "%s/.import-%ld.tmp", root,
                 (long)getpid());
        CHECK(access(scratch, F_OK) != 0);
    }

    free(before);
    free(after);
    remove_session(root, new_id);
    free(new_id);
    CHECK_EQ(rmdir(root), 0);
    CHECK_EQ(unlink(source), 0);
}

TEST(import_refuses_bad_sources_and_leaves_nothing_behind) {
    char root[192];
    char source[192];
    char link_path[192];
    char *new_id = NULL;
    char *detail = NULL;

    snprintf(root, sizeof root, "/tmp/oi-session-import-bad-%ld",
             (long)getpid());
    snprintf(source, sizeof source, "/tmp/oi-not-a-log-%ld.oilog",
             (long)getpid());

    /* A file that is not a session log at all. */
    {
        FILE *garbage = fopen(source, "w");
        CHECK(garbage != NULL);
        CHECK(fputs("this is not an oilog header", garbage) >= 0);
        CHECK_EQ(fclose(garbage), 0);
    }
    CHECK_EQ(oi_cli_session_import(root, source, strlen(source), &new_id,
                                   &detail),
             OI_ERR_PARSE);
    CHECK(new_id == NULL);
    CHECK_STREQ(detail, "not a valid oi session log");
    free(detail);
    detail = NULL;
    /* Rejected cleanly: no session directory, no scratch file, source
     * still there. */
    {
        struct oi_cli_session_list list;
        oi_cli_session_list_init(&list);
        CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
        CHECK_EQ(list.len, 0);
        oi_cli_session_list_free(&list);
    }
    {
        char scratch[256];
        snprintf(scratch, sizeof scratch, "%s/.import-%ld.tmp", root,
                 (long)getpid());
        CHECK(access(scratch, F_OK) != 0);
    }
    CHECK_EQ(access(source, F_OK), 0);
    CHECK_EQ(unlink(source), 0);

    /* A symlink source is refused rather than followed. */
    write_legacy_log(source);
    snprintf(link_path, sizeof link_path, "/tmp/oi-legacy-link-%ld.oilog",
             (long)getpid());
    unlink(link_path);
    CHECK_EQ(symlink(source, link_path), 0);
    CHECK_EQ(oi_cli_session_import(root, link_path, strlen(link_path),
                                   &new_id, &detail),
             OI_ERR_INVAL);
    CHECK_STREQ(detail, "refusing to import a symlink");
    free(detail);
    detail = NULL;
    CHECK_EQ(unlink(link_path), 0);

    /* A directory is not a log. */
    CHECK_EQ(oi_cli_session_import(root, "/tmp", 4, &new_id, &detail),
             OI_ERR_INVAL);
    CHECK_STREQ(detail, "not a regular file");
    free(detail);
    detail = NULL;

    /* Nothing there. */
    CHECK_EQ(oi_cli_session_import(root, "/tmp/oi-absolutely-absent", 24,
                                   &new_id, &detail),
             OI_ERR_NOTFOUND);
    free(detail);
    detail = NULL;

    CHECK_EQ(oi_cli_session_import(root, NULL, 0, &new_id, &detail),
             OI_ERR_INVAL);
    free(detail);

    CHECK(new_id == NULL);
    CHECK_EQ(unlink(source), 0);
    rmdir(root);
}

TEST(import_works_around_a_stale_scratch_file) {
    char root[192];
    char source[192];
    char stale[256];
    char *new_id = NULL;

    snprintf(root, sizeof root, "/tmp/oi-session-import-stale-%ld",
             (long)getpid());
    snprintf(source, sizeof source, "/tmp/oi-legacy-stale-%ld.oilog",
             (long)getpid());
    write_legacy_log(source);
    CHECK(mkdir(root, 0700) == 0 || errno == EEXIST);

    /*
     * A scratch file left behind by a killed earlier run. Since the copy is
     * created O_EXCL to avoid ever overwriting anything, a fixed name would
     * let this block every future import from a process that reused the
     * same pid.
     */
    snprintf(stale, sizeof stale, "%s/.import-%ld-0.tmp", root,
             (long)getpid());
    {
        FILE *leftover = fopen(stale, "w");
        CHECK(leftover != NULL);
        CHECK(fputs("junk from a crashed run", leftover) >= 0);
        CHECK_EQ(fclose(leftover), 0);
    }

    CHECK_EQ(oi_cli_session_import(root, source, strlen(source), &new_id,
                                   NULL),
             OI_OK);
    CHECK(new_id != NULL);
    /* The stale file was stepped over, not overwritten. */
    CHECK_EQ(access(stale, F_OK), 0);
    {
        char *contents = NULL;
        size_t len = 0;
        read_whole_file(stale, &contents, &len);
        CHECK_EQ(len, (size_t)23);
        CHECK_EQ(memcmp(contents, "junk from a crashed run", 23), 0);
        free(contents);
    }

    CHECK_EQ(unlink(stale), 0);
    remove_session(root, new_id);
    free(new_id);
    CHECK_EQ(rmdir(root), 0);
    CHECK_EQ(unlink(source), 0);
}

TEST(import_refuses_a_source_already_inside_the_sessions_root) {
    char root[192];
    char imported_path[384];
    char *new_id = NULL;
    char *second_id = NULL;
    char *detail = NULL;
    char source[192];

    snprintf(root, sizeof root, "/tmp/oi-session-import-self-%ld",
             (long)getpid());
    snprintf(source, sizeof source, "/tmp/oi-legacy-self-%ld.oilog",
             (long)getpid());
    write_legacy_log(source);
    CHECK_EQ(oi_cli_session_import(root, source, strlen(source), &new_id,
                                   NULL),
             OI_OK);

    /* Re-importing a session oi already manages is a misunderstanding
     * worth naming, not a second copy to make. */
    snprintf(imported_path, sizeof imported_path, "%s/%s/history.oilog", root,
             new_id);
    CHECK_EQ(oi_cli_session_import(root, imported_path, strlen(imported_path),
                                   &second_id, &detail),
             OI_ERR_INVAL);
    CHECK(second_id == NULL);
    CHECK(detail != NULL);
    CHECK(strstr(detail, "already an oi-managed session") != NULL);
    free(detail);

    /* Still exactly one session. */
    {
        struct oi_cli_session_list list;
        oi_cli_session_list_init(&list);
        CHECK_EQ(oi_cli_sessions_enumerate(root, &list), OI_OK);
        CHECK_EQ(list.len, 1);
        oi_cli_session_list_free(&list);
    }

    remove_session(root, new_id);
    free(new_id);
    CHECK_EQ(rmdir(root), 0);
    CHECK_EQ(unlink(source), 0);
}

/* --- oi_cli_session_restore_settings / oi_cli_session_apply_setting --- */

static char *save_cwd(void) { return getcwd(NULL, 0); }

static void restore_cwd(char *saved) {
    CHECK_EQ(chdir(saved), 0);
    free(saved);
}

static char *make_tmp_dir(const char *suffix) {
    char template[160];
    char *path;
    snprintf(template, sizeof template, "/tmp/oi-session-restore-%ld-%s",
             (long)getpid(), suffix);
    CHECK(mkdir(template, 0700) == 0 || errno == EEXIST);
    path = strdup(template);
    return path;
}

static const char *fresh_log_path(const char *suffix) {
    static char path[192];
    snprintf(path, sizeof path, "/tmp/oi-session-restore-log-%ld-%s.oilog",
             (long)getpid(), suffix);
    unlink(path);
    return path;
}

/* Opens a brand-new log, appends its schema transition, and returns
 * everything needed to call oi_cli_session_restore_settings/
 * oi_cli_session_apply_setting against it. */
struct fresh_store {
    oi_sesslog *log;
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    char *metadata_path;
};

static void fresh_store_open(struct fresh_store *fresh, const char *path,
                             int is_private_directory) {
    struct oi_cli_history_record record;
    oi_cli_history_record_init(&record);
    CHECK_EQ(oi_sesslog_open(path, &fresh->log), OI_OK);
    oi_cli_history_store_init(&fresh->store);
    oi_cli_history_replay_state_init(&fresh->state);
    CHECK_EQ(oi_cli_history_store_load(fresh->log, &fresh->store,
                                       &fresh->state),
             OI_OK);
    CHECK(fresh->state.needs_transition);
    CHECK_EQ(oi_cli_history_record_set_transition(&record, 1, 0), OI_OK);
    CHECK_EQ(oi_cli_history_store_append(&fresh->store, &record,
                                         &fresh->state),
             OI_OK);
    oi_cli_history_record_free(&record);
    fresh->metadata_path = NULL;
    CHECK_EQ(oi_cli_session_metadata_path_for_log(path, is_private_directory,
                                                  &fresh->metadata_path),
             OI_OK);
}

static void fresh_store_close(struct fresh_store *fresh) {
    oi_cli_history_store_free(&fresh->store);
    oi_cli_history_replay_state_free(&fresh->state);
    oi_sesslog_close(fresh->log);
    free(fresh->metadata_path);
}

TEST(fresh_session_records_initial_model_and_cwd) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("fresh");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;
    struct oi_cli_session_metadata metadata;

    fresh_store_open(&fresh, fresh_log_path("fresh"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    CHECK_STREQ(restore.model.data, "gpt-default");
    CHECK_STREQ(restore.cwd.data, target_dir);
    /* Nothing to restore from and no override: the fallback branch won, and
     * says so. /status reports this provenance verbatim, so the resolution
     * branch itself is what has to be asserted -- the resolved name alone
     * cannot distinguish a default from an override of the same value. */
    CHECK_EQ(restore.model_origin, OI_CLI_SESSION_MODEL_DEFAULT);
    CHECK(!restore.metadata_missing_or_corrupt);
    CHECK(!restore.cwd_fallback_applied);
    CHECK_EQ(fresh.store.typed_history.len, 3); /* transition + model + cwd */
    {
        char actual_cwd[512];
        CHECK(getcwd(actual_cwd, sizeof actual_cwd) != NULL);
        CHECK_STREQ(actual_cwd, target_dir);
    }

    oi_cli_session_metadata_init(&metadata);
    CHECK_EQ(oi_cli_session_metadata_store_read(fresh.metadata_path,
                                                &metadata),
             OI_OK);
    CHECK_STREQ(metadata.session_id.data, "sess-1");
    CHECK_STREQ(metadata.model.data, "gpt-default");
    CHECK_STREQ(metadata.cwd.data, target_dir);
    oi_cli_session_metadata_free(&metadata);

    oi_cli_session_restore_free(&restore);
    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("fresh"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(unchanged_resume_writes_no_new_records_but_refreshes_metadata) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("unchanged");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;
    size_t history_len_after_first;

    fresh_store_open(&fresh, fresh_log_path("unchanged"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    history_len_after_first = fresh.store.typed_history.len;
    oi_cli_session_restore_free(&restore);

    /* Simulate the metadata cache existing and still matching: resolving
     * again with is_new_session=0 should not append anything new. */
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    CHECK_EQ(fresh.store.typed_history.len, history_len_after_first);
    CHECK_STREQ(restore.model.data, "gpt-default");
    CHECK_STREQ(restore.cwd.data, target_dir);
    /* The same name as the default, but reached through the replayed setting
     * record the first call wrote -- which is a different provenance, and the
     * one a resume must report. */
    CHECK_EQ(restore.model_origin, OI_CLI_SESSION_MODEL_HISTORY);

    oi_cli_session_restore_free(&restore);
    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("unchanged"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(apply_setting_writes_a_record_and_persists_through_restore) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("apply");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;

    fresh_store_open(&fresh, fresh_log_path("apply"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    oi_cli_session_restore_free(&restore);

    CHECK_EQ(oi_cli_session_apply_setting(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 OI_CLI_HISTORY_SESSION_SETTING_MODEL, "gpt-changed", 11),
             OI_OK);
    CHECK_STREQ(fresh.state.last_model.data, "gpt-changed");

    /* A fresh restore (as if the process restarted) picks up the change
     * from metadata without needing to replay history at all. */
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    CHECK_STREQ(restore.model.data, "gpt-changed");
    /* A /model change lands in history first, so a later resume attributes it
     * to history rather than to the cache it also refreshed. */
    CHECK_EQ(restore.model_origin, OI_CLI_SESSION_MODEL_HISTORY);

    oi_cli_session_restore_free(&restore);
    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("apply"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(missing_metadata_rebuilds_from_history_with_a_diagnostic) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("missing-meta");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;
    FILE *diagnostics = tmpfile();

    fresh_store_open(&fresh, fresh_log_path("missing-meta"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    oi_cli_session_restore_free(&restore);

    /* Delete the metadata cache: history alone must still resolve the
     * same effective model/cwd. */
    unlink(fresh.metadata_path);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, NULL, "gpt-default", target_dir, diagnostics, &restore),
             OI_OK);
    CHECK(restore.metadata_missing_or_corrupt);
    CHECK_STREQ(restore.model.data, "gpt-default");
    CHECK_STREQ(restore.cwd.data, target_dir);
    CHECK_EQ(access(fresh.metadata_path, F_OK), 0); /* self-healed */
    {
        long len;
        CHECK_EQ(fflush(diagnostics), 0);
        CHECK(fseek(diagnostics, 0, SEEK_END) == 0);
        len = ftell(diagnostics);
        CHECK(len > 0);
    }

    fclose(diagnostics);
    oi_cli_session_restore_free(&restore);
    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("missing-meta"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(mismatched_session_id_in_metadata_is_treated_as_corrupt) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("mismatch");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;
    struct oi_cli_session_metadata wrong_owner;

    fresh_store_open(&fresh, fresh_log_path("mismatch"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    oi_cli_session_restore_free(&restore);

    /* Overwrite metadata with a plausible-looking file for a *different*
     * session id -- must not be trusted. */
    oi_cli_session_metadata_init(&wrong_owner);
    CHECK_EQ(oi_cli_session_metadata_set(&wrong_owner, "some-other-session",
                                         18, "gpt-wrong", 9, "/nonexistent",
                                         12, NULL, 0, 1, 1),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_store_write(fresh.metadata_path,
                                                 &wrong_owner),
             OI_OK);
    oi_cli_session_metadata_free(&wrong_owner);

    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    CHECK(restore.metadata_missing_or_corrupt);
    CHECK_STREQ(restore.model.data, "gpt-default");

    oi_cli_session_restore_free(&restore);
    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("mismatch"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(missing_prior_cwd_falls_back_with_a_diagnostic) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("cwd-missing");
    char *deleted_dir = make_tmp_dir("cwd-deleted");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;
    FILE *diagnostics = tmpfile();

    fresh_store_open(&fresh, fresh_log_path("cwd-missing"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-default", deleted_dir, NULL, &restore),
             OI_OK);
    oi_cli_session_restore_free(&restore);
    /* The directory the session was durably recorded in is now gone. */
    CHECK_EQ(rmdir(deleted_dir), 0);

    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, NULL, "gpt-default", target_dir, diagnostics, &restore),
             OI_OK);
    CHECK(restore.cwd_fallback_applied);
    CHECK_STREQ(restore.cwd.data, target_dir);
    {
        char actual_cwd[512];
        CHECK(getcwd(actual_cwd, sizeof actual_cwd) != NULL);
        CHECK_STREQ(actual_cwd, target_dir);
    }
    {
        long len;
        CHECK_EQ(fflush(diagnostics), 0);
        CHECK(fseek(diagnostics, 0, SEEK_END) == 0);
        len = ftell(diagnostics);
        CHECK(len > 0);
    }

    fclose(diagnostics);
    oi_cli_session_restore_free(&restore);
    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("cwd-missing"));
    rmdir(target_dir);
    free(target_dir);
    free(deleted_dir);
}

TEST(explicit_model_override_wins_and_persists) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("override");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;

    fresh_store_open(&fresh, fresh_log_path("override"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    oi_cli_session_restore_free(&restore);

    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, "gpt-override", "gpt-default", target_dir, NULL,
                 &restore),
             OI_OK);
    CHECK_STREQ(restore.model.data, "gpt-override");
    /* The override outranks the setting record the first call wrote, and the
     * reported provenance follows the branch that actually won. */
    CHECK_EQ(restore.model_origin, OI_CLI_SESSION_MODEL_EXPLICIT);
    CHECK_STREQ(fresh.state.last_model.data, "gpt-override");
    {
        struct oi_cli_session_metadata metadata;
        oi_cli_session_metadata_init(&metadata);
        CHECK_EQ(oi_cli_session_metadata_store_read(fresh.metadata_path,
                                                    &metadata),
                 OI_OK);
        CHECK_STREQ(metadata.model.data, "gpt-override");
        oi_cli_session_metadata_free(&metadata);
    }

    oi_cli_session_restore_free(&restore);
    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("override"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(a_display_name_survives_setting_changes_and_reopens) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("named");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;

    fresh_store_open(&fresh, fresh_log_path("named"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    oi_cli_session_restore_free(&restore);

    /* Name the session, the way /session rename will. */
    {
        struct oi_cli_session_metadata named;
        oi_cli_session_metadata_init(&named);
        CHECK_EQ(oi_cli_session_metadata_store_read(fresh.metadata_path,
                                                    &named),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_set(
                     &named, "sess-1", 6, named.model.data, named.model.len,
                     named.cwd.data, named.cwd.len, "my session", 10,
                     named.created_at, named.updated_at),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_store_write(fresh.metadata_path,
                                                     &named),
                 OI_OK);
        oi_cli_session_metadata_free(&named);
    }

    /*
     * The name lives only in metadata.json, and both of the paths that
     * refresh that cache rebuild it from scratch -- so each has to carry
     * the name forward or a plain /model change would silently discard it.
     */
    CHECK_EQ(oi_cli_session_apply_setting(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 OI_CLI_HISTORY_SESSION_SETTING_MODEL, "gpt-changed", 11),
             OI_OK);
    {
        struct oi_cli_session_metadata after;
        oi_cli_session_metadata_init(&after);
        CHECK_EQ(oi_cli_session_metadata_store_read(fresh.metadata_path,
                                                    &after),
                 OI_OK);
        CHECK_STREQ(after.model.data, "gpt-changed");
        CHECK_STREQ(after.display_name.data, "my session");
        oi_cli_session_metadata_free(&after);
    }

    /* Same for reopening the session. */
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    oi_cli_session_restore_free(&restore);
    {
        struct oi_cli_session_metadata after;
        oi_cli_session_metadata_init(&after);
        CHECK_EQ(oi_cli_session_metadata_store_read(fresh.metadata_path,
                                                    &after),
                 OI_OK);
        CHECK_STREQ(after.display_name.data, "my session");
        oi_cli_session_metadata_free(&after);
    }

    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("named"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(a_stale_metadata_cache_never_overrides_or_rewrites_history) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("stale-cache");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;
    size_t history_len_before;

    fresh_store_open(&fresh, fresh_log_path("stale-cache"), 0);
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 1, NULL, "gpt-old", target_dir, NULL, &restore),
             OI_OK);
    oi_cli_session_restore_free(&restore);

    /* A durable model change: history is authoritative for it. */
    CHECK_EQ(oi_cli_session_apply_setting(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 OI_CLI_HISTORY_SESSION_SETTING_MODEL, "gpt-new", 7),
             OI_OK);
    CHECK_STREQ(fresh.state.last_model.data, "gpt-new");

    /*
     * Simulate the metadata refresh having failed: the cache still names
     * the old model, and is otherwise perfectly valid and owned by this
     * session, so it would pass every validity check.
     *
     * The cache must not win. If it did, this open would resolve "gpt-old",
     * notice it differs from what history last recorded, and append that
     * stale value back -- letting an unwritable cache silently rewrite the
     * authoritative log.
     */
    {
        struct oi_cli_session_metadata stale;
        oi_cli_session_metadata_init(&stale);
        CHECK_EQ(oi_cli_session_metadata_store_read(fresh.metadata_path,
                                                    &stale),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_set(
                     &stale, "sess-1", 6, "gpt-old", 7, target_dir,
                     strlen(target_dir), NULL, 0, stale.created_at,
                     stale.updated_at),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_store_write(fresh.metadata_path,
                                                     &stale),
                 OI_OK);
        oi_cli_session_metadata_free(&stale);
    }

    history_len_before = fresh.store.typed_history.len;
    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    /* History won. */
    CHECK_STREQ(restore.model.data, "gpt-new");
    CHECK_STREQ(fresh.state.last_model.data, "gpt-new");
    /* And nothing was appended, because nothing actually changed. */
    CHECK_EQ(fresh.store.typed_history.len, history_len_before);
    oi_cli_session_restore_free(&restore);

    /* The refresh also repaired the cache back to the durable truth. */
    {
        struct oi_cli_session_metadata repaired;
        oi_cli_session_metadata_init(&repaired);
        CHECK_EQ(oi_cli_session_metadata_store_read(fresh.metadata_path,
                                                    &repaired),
                 OI_OK);
        CHECK_STREQ(repaired.model.data, "gpt-new");
        oi_cli_session_metadata_free(&repaired);
    }

    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("stale-cache"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(the_metadata_cache_still_supplies_settings_history_never_recorded) {
    char *original_cwd = save_cwd();
    char *target_dir = make_tmp_dir("cache-fallback");
    struct fresh_store fresh;
    struct oi_cli_session_restore restore;

    /* A session whose history has a transition but no setting records --
     * the cache is then the only place a prior model could survive, so it
     * must still be used as a fallback rather than ignored outright. */
    fresh_store_open(&fresh, fresh_log_path("cache-fallback"), 0);
    {
        struct oi_cli_session_metadata cached;
        oi_cli_session_metadata_init(&cached);
        CHECK_EQ(oi_cli_session_metadata_set(&cached, "sess-1", 6,
                                             "gpt-from-cache", 14, target_dir,
                                             strlen(target_dir), NULL, 0, 10,
                                             20),
                 OI_OK);
        CHECK_EQ(oi_cli_session_metadata_store_write(fresh.metadata_path,
                                                     &cached),
                 OI_OK);
        oi_cli_session_metadata_free(&cached);
    }
    CHECK(fresh.state.last_model.data == NULL);

    oi_cli_session_restore_init(&restore);
    CHECK_EQ(oi_cli_session_restore_settings(
                 &fresh.store, &fresh.state, fresh.metadata_path, "sess-1",
                 0, NULL, "gpt-default", target_dir, NULL, &restore),
             OI_OK);
    CHECK_STREQ(restore.model.data, "gpt-from-cache");
    /* History recorded nothing, so the cache branch won -- reported as the
     * cache, not as history, since a rebuilt cache is a weaker claim. */
    CHECK_EQ(restore.model_origin, OI_CLI_SESSION_MODEL_METADATA);
    oi_cli_session_restore_free(&restore);

    unlink(fresh.metadata_path);
    fresh_store_close(&fresh);
    restore_cwd(original_cwd);
    unlink(fresh_log_path("cache-fallback"));
    rmdir(target_dir);
    free(target_dir);
}

TEST(restore_settings_rejects_bad_arguments) {
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_session_restore restore;
    oi_cli_history_store_init(&store);
    oi_cli_history_replay_state_init(&state);
    oi_cli_session_restore_init(&restore);

    CHECK_EQ(oi_cli_session_restore_settings(NULL, &state, "/tmp/x", "id", 1,
                                             NULL, "model", "/tmp", NULL,
                                             &restore),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_restore_settings(&store, &state, "/tmp/x", "id",
                                             1, NULL, "", "/tmp", NULL,
                                             &restore),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_apply_setting(NULL, &state, "/tmp/x", "id",
                                          OI_CLI_HISTORY_SESSION_SETTING_MODEL,
                                          "m", 1),
             OI_ERR_INVAL);

    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
}

int main(void) {
    /* A regression that hangs a child must fail this binary, not CI. */
    oi_test_set_deadline(900);
    RUN(default_root_uses_xdg_state_home);
    RUN(create_makes_private_timestamped_session);
    RUN(two_creations_are_unique);
    RUN(bad_arguments_are_rejected);
    RUN(metadata_path_derivation_rules);
    RUN(safe_ids_accept_only_bounded_portable_components);
    RUN(resolve_accepts_a_real_directory_and_refuses_everything_else);
    RUN(enumerate_lists_sessions_newest_first_and_skips_non_sessions);
    RUN(enumerate_reports_an_empty_list_for_a_root_that_does_not_exist);
    RUN(enumerate_rebuilds_missing_and_malformed_metadata_from_history);
    RUN(enumerate_distrusts_metadata_belonging_to_another_session);
    RUN(enumerate_lists_an_unreplayable_session_with_only_its_id);
    RUN(enumerate_reports_a_session_held_by_another_process_as_busy);
    RUN(enumerate_does_not_rewrite_the_logs_it_lists);
    RUN(rename_sets_a_display_name_without_moving_the_directory);
    RUN(rename_repairs_a_missing_metadata_cache_from_history);
    RUN(rename_merges_with_the_current_cache_not_a_stale_copy);
    RUN(a_rename_waits_for_a_settings_update_and_merges_its_value);
    RUN(a_planted_lock_symlink_is_refused_and_never_followed);
    RUN(rename_refuses_unnameable_and_invalid_targets);
    RUN(trash_and_restore_round_trip_preserving_history);
    RUN(trash_refuses_the_active_session_and_unknown_ids);
    RUN(trash_refuses_a_session_open_in_another_process);
    RUN(delete_only_reaches_trashed_sessions_and_removes_them_completely);
    RUN(delete_fails_closed_on_a_directory_it_did_not_create);
    RUN(trash_reports_a_cross_device_trash_directory_distinctly);
    RUN(restore_refuses_unknown_ids_and_will_not_clobber_a_live_session);
    RUN(import_adopts_a_legacy_log_and_leaves_the_source_untouched);
    RUN(import_refuses_bad_sources_and_leaves_nothing_behind);
    RUN(import_works_around_a_stale_scratch_file);
    RUN(import_refuses_a_source_already_inside_the_sessions_root);
    RUN(fresh_session_records_initial_model_and_cwd);
    RUN(unchanged_resume_writes_no_new_records_but_refreshes_metadata);
    RUN(apply_setting_writes_a_record_and_persists_through_restore);
    RUN(missing_metadata_rebuilds_from_history_with_a_diagnostic);
    RUN(mismatched_session_id_in_metadata_is_treated_as_corrupt);
    RUN(missing_prior_cwd_falls_back_with_a_diagnostic);
    RUN(explicit_model_override_wins_and_persists);
    RUN(a_display_name_survives_setting_changes_and_reopens);
    RUN(a_stale_metadata_cache_never_overrides_or_rewrites_history);
    RUN(the_metadata_cache_still_supplies_settings_history_never_recorded);
    RUN(restore_settings_rejects_bad_arguments);
    return oi_test_report();
}
