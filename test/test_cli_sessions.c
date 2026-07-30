#include "cli_sessions.h"
#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli_session_metadata.h"
#include "cli_session_metadata_store.h"
#include "oi/sesslog.h"

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
                                         12, 1, 1),
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
    RUN(default_root_uses_xdg_state_home);
    RUN(create_makes_private_timestamped_session);
    RUN(two_creations_are_unique);
    RUN(bad_arguments_are_rejected);
    RUN(metadata_path_derivation_rules);
    RUN(safe_ids_accept_only_bounded_portable_components);
    RUN(resolve_accepts_a_real_directory_and_refuses_everything_else);
    RUN(fresh_session_records_initial_model_and_cwd);
    RUN(unchanged_resume_writes_no_new_records_but_refreshes_metadata);
    RUN(apply_setting_writes_a_record_and_persists_through_restore);
    RUN(missing_metadata_rebuilds_from_history_with_a_diagnostic);
    RUN(mismatched_session_id_in_metadata_is_treated_as_corrupt);
    RUN(missing_prior_cwd_falls_back_with_a_diagnostic);
    RUN(explicit_model_override_wins_and_persists);
    RUN(restore_settings_rejects_bad_arguments);
    return oi_test_report();
}
