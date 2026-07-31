#include "../test.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cli_session_metadata.h"
#include "cli_session_metadata_store.h"
#include "cli_session_switch.h"
#include "cli_sessions.h"
#include "oi/sesslog.h"

/*
 * Exercises oi_cli_session_switch directly against a real registry, real
 * logs, and a real second process holding a lock -- no REPL, no PTY, no
 * mock server. The REPL-level wiring is covered separately in test_cli.c.
 */

/* Wraps a plain user or assistant message into a durable record, the same
 * way the REPL's own persistence callbacks do. */
static void set_user_record(struct oi_cli_history_record *record,
                            uint64_t record_id, uint64_t turn_id,
                            const char *content) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    CHECK_EQ(oi_cli_message_set_user(&message, content, strlen(content)),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 record, record_id, turn_id, &message,
                 OI_CLI_HISTORY_MESSAGE_NORMAL, NULL, 0,
                 OI_CLI_HISTORY_TOOL_OUTCOME_NONE, NULL, 0, 0),
             OI_OK);
    oi_cli_message_free(&message);
}

static void set_assistant_record(struct oi_cli_history_record *record,
                                 uint64_t record_id, uint64_t turn_id,
                                 const char *content, const char *model) {
    struct oi_cli_message message;
    oi_cli_message_init(&message);
    CHECK_EQ(oi_cli_message_set_assistant(&message, content, strlen(content)),
             OI_OK);
    CHECK_EQ(oi_cli_history_record_set_message(
                 record, record_id, turn_id, &message,
                 OI_CLI_HISTORY_MESSAGE_NORMAL, model,
                 model == NULL ? 0 : strlen(model),
                 OI_CLI_HISTORY_TOOL_OUTCOME_NONE, NULL, 0, 0),
             OI_OK);
    oi_cli_message_free(&message);
}

static char *test_root(const char *suffix) {
    char path[256];
    char *copy;
    snprintf(path, sizeof path, "/tmp/oi-switch-%ld-%s", (long)getpid(),
             suffix);
    copy = strdup(path);
    CHECK(copy != NULL);
    return copy;
}

/* Builds a session directory with a real log carrying `turns` completed
 * user/assistant exchanges plus durable model and cwd settings. */
static char *seed_session(const char *root, const char *model,
                          const char *cwd, unsigned turns) {
    struct oi_cli_session_location location;
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_history_record record;
    oi_sesslog *log = NULL;
    char *id;
    unsigned turn;

    oi_cli_session_location_init(&location);
    CHECK_EQ(oi_cli_session_location_create(root, &location), OI_OK);
    CHECK_EQ(oi_sesslog_open(location.history_path, &log), OI_OK);
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

    for (turn = 0; turn < turns; turn++) {
        char text[64];
        uint64_t turn_id = state.next_turn_id;

        snprintf(text, sizeof text, "question %u", turn);
        oi_cli_history_record_init(&record);
        set_user_record(&record, state.next_record_id, turn_id, text);
        CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
        oi_cli_history_record_free(&record);

        snprintf(text, sizeof text, "answer %u", turn);
        oi_cli_history_record_init(&record);
        set_assistant_record(&record, state.next_record_id, turn_id, text,
                             model);
        CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
        oi_cli_history_record_free(&record);
    }

    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);

    id = strdup(location.id);
    CHECK(id != NULL);
    oi_cli_session_location_free(&location);
    return id;
}

static void remove_session(const char *root, const char *id) {
    char path[384];
    snprintf(path, sizeof path, "%s/%s/history.oilog", root, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s/metadata.json", root, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s/metadata.json.lock", root, id);
    unlink(path);
    snprintf(path, sizeof path, "%s/%s", root, id);
    rmdir(path);
}

static size_t registry_size(oi_session_registry *registry, const char *id) {
    return oi_session_lookup(registry, id) != NULL ? 1u : 0u;
}

TEST(switch_opens_replays_and_restores_the_target) {
    char *root = test_root("ok");
    char *original_cwd = getcwd(NULL, 0);
    char target_cwd[256];
    oi_session_registry *registry = oi_session_registry_create();
    struct oi_cli_session_switch_result result;
    char *id;

    CHECK(registry != NULL);
    CHECK(original_cwd != NULL);
    snprintf(target_cwd, sizeof target_cwd, "%s-cwd", root);
    CHECK(mkdir(target_cwd, 0700) == 0 || errno == EEXIST);
    id = seed_session(root, "gpt-target", target_cwd, 2);

    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, "some-other-session", id,
                                   strlen(id), "gpt-default", original_cwd,
                                   NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_OK);
    CHECK(result.session != NULL);
    CHECK_STREQ(oi_session_id(result.session), id);
    CHECK(oi_session_arena(result.session) != NULL);

    /* The target's durable settings win over the process defaults. */
    CHECK_STREQ(result.model.data, "gpt-target");
    CHECK_STREQ(result.cwd.data, target_cwd);
    /*
     * And the provenance restore_settings decided is propagated out, not
     * recomputed or dropped: the seeded target carries a model setting
     * record, so history is what won. A switch passes no override at all, so
     * this can never be OI_CLI_SESSION_MODEL_EXPLICIT -- that is structural,
     * not incidental, and the REPL adopts whatever arrives here as the
     * newly-active model's story.
     */
    CHECK_EQ(result.model_origin, OI_CLI_SESSION_MODEL_HISTORY);
    /* ...and the working directory really moved. */
    {
        char now[256];
        CHECK(getcwd(now, sizeof now) != NULL);
        CHECK_STREQ(now, target_cwd);
    }

    /* Full model-visible context came back, in order: two exchanges. */
    CHECK_EQ(result.initial_context.len, (size_t)4);
    CHECK_STREQ(result.initial_context.items[0].content.data, "question 0");
    CHECK_STREQ(result.initial_context.items[1].content.data, "answer 0");
    CHECK_STREQ(result.initial_context.items[3].content.data, "answer 1");
    {
        char expected_path[384];
        snprintf(expected_path, sizeof expected_path, "%s/%s", root, id);
        CHECK_STREQ(result.path, expected_path);
    }
    CHECK(result.metadata_path != NULL);
    CHECK(strstr(result.metadata_path, "metadata.json") != NULL);
    /* The store/state moved out and are usable by the caller. */
    CHECK(result.store.log == oi_session_log(result.session));
    CHECK(result.state.next_turn_id > 0);

    CHECK_EQ(chdir(original_cwd), 0);
    oi_session_destroy(registry, result.session);
    result.session = NULL;
    oi_cli_session_switch_result_free(&result);
    oi_session_registry_destroy(registry);
    remove_session(root, id);
    rmdir(root);
    rmdir(target_cwd);
    free(id);
    free(root);
    free(original_cwd);
}

TEST(switch_to_the_same_session_is_a_no_op) {
    char *root = test_root("same");
    char *cwd = getcwd(NULL, 0);
    oi_session_registry *registry = oi_session_registry_create();
    struct oi_cli_session_switch_result result;

    CHECK(registry != NULL);
    oi_cli_session_switch_result_init(&result);
    /* Reported before any I/O, and without registering anything. */
    CHECK_EQ(oi_cli_session_switch(registry, root, "sess-a", "sess-a", 6,
                                   "gpt-default", cwd, NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_SAME);
    CHECK(result.session == NULL);
    CHECK_EQ(registry_size(registry, "sess-a"), 0u);

    oi_cli_session_switch_result_free(&result);
    oi_session_registry_destroy(registry);
    free(root);
    free(cwd);
}

TEST(switch_rejects_unsafe_ids_and_missing_sessions) {
    char *root = test_root("missing");
    char *cwd = getcwd(NULL, 0);
    oi_session_registry *registry = oi_session_registry_create();
    struct oi_cli_session_switch_result result;

    CHECK(registry != NULL);
    CHECK(mkdir(root, 0700) == 0 || errno == EEXIST);

    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, "..", 2,
                                   "gpt-default", cwd, NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_INVALID);
    oi_cli_session_switch_result_free(&result);

    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, "a/b", 3,
                                   "gpt-default", cwd, NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_INVALID);
    oi_cli_session_switch_result_free(&result);

    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, "absent", 6,
                                   "gpt-default", cwd, NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_NOT_FOUND);
    CHECK(result.session == NULL);
    oi_cli_session_switch_result_free(&result);

    /* A symlink standing in for a session directory is not switchable. */
    {
        char link_path[320];
        snprintf(link_path, sizeof link_path, "%s/linked", root);
        unlink(link_path);
        CHECK_EQ(symlink("/tmp", link_path), 0);
        oi_cli_session_switch_result_init(&result);
        CHECK_EQ(oi_cli_session_switch(registry, root, NULL, "linked", 6,
                                       "gpt-default", cwd, NULL, &result),
                 OI_OK);
        CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_NOT_FOUND);
        oi_cli_session_switch_result_free(&result);
        CHECK_EQ(unlink(link_path), 0);
    }

    /* Bad arguments are the caller's bug, not a business outcome. */
    CHECK_EQ(oi_cli_session_switch(NULL, root, NULL, "x", 1, "m", cwd, NULL,
                                   &result),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, "x", 1, "", cwd, NULL,
                                   &result),
             OI_ERR_INVAL);

    oi_session_registry_destroy(registry);
    rmdir(root);
    free(root);
    free(cwd);
}

TEST(switch_to_a_session_held_elsewhere_reports_busy_and_registers_nothing) {
    char *root = test_root("busy");
    char *cwd = getcwd(NULL, 0);
    oi_session_registry *registry = oi_session_registry_create();
    struct oi_cli_session_switch_result result;
    char history_path[384];
    char *id;
    int to_child[2];
    int to_parent[2];
    pid_t child;

    CHECK(registry != NULL);
    id = seed_session(root, "gpt-held", cwd, 1);
    snprintf(history_path, sizeof history_path, "%s/%s/history.oilog", root,
             id);

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

    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, id, strlen(id),
                                   "gpt-default", cwd, NULL, &result),
             OI_OK);
    /* Busy is an ordinary outcome, not an error that ends the REPL. */
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_BUSY);
    CHECK(result.session == NULL);
    /* And nothing was left behind in the registry to leak or collide. */
    CHECK_EQ(registry_size(registry, id), 0u);
    oi_cli_session_switch_result_free(&result);

    CHECK_EQ(write(to_child[1], "x", 1), 1);
    {
        int wait_status = 0;
        CHECK_EQ(waitpid(child, &wait_status, 0), child);
        CHECK(WIFEXITED(wait_status));
        CHECK_EQ(WEXITSTATUS(wait_status), 0);
    }
    close(to_child[1]);
    close(to_parent[0]);

    /* Once released, the very same switch succeeds. */
    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, id, strlen(id),
                                   "gpt-default", cwd, NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_OK);
    CHECK_EQ(registry_size(registry, id), 1u);
    CHECK_EQ(chdir(cwd), 0);
    oi_session_destroy(registry, result.session);
    result.session = NULL;
    oi_cli_session_switch_result_free(&result);

    oi_session_registry_destroy(registry);
    remove_session(root, id);
    rmdir(root);
    free(id);
    free(root);
    free(cwd);
}

TEST(switch_recovers_a_truncated_tail_but_refuses_an_undecodable_log) {
    char *root = test_root("corrupt");
    char *cwd = getcwd(NULL, 0);
    oi_session_registry *registry = oi_session_registry_create();
    struct oi_cli_session_switch_result result;
    char history_path[384];
    char *recoverable;
    char *undecodable;

    CHECK(registry != NULL);
    recoverable = seed_session(root, "gpt-recover", cwd, 1);
    undecodable = seed_session(root, "gpt-broken", cwd, 1);

    /*
     * A record left incomplete by a crash is recovered, not rejected --
     * oi_sesslog_open truncates the fragment and everything before it is
     * preserved, so the session is still switchable.
     */
    snprintf(history_path, sizeof history_path, "%s/%s/history.oilog", root,
             recoverable);
    {
        static const unsigned char fragment[] = {0xff, 0xff, 0xff, 0xff, 'p'};
        FILE *appender = fopen(history_path, "ab");
        CHECK(appender != NULL);
        CHECK_EQ(fwrite(fragment, 1, sizeof fragment, appender),
                 sizeof fragment);
        CHECK_EQ(fclose(appender), 0);
    }
    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, recoverable,
                                   strlen(recoverable), "gpt-default", cwd,
                                   NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_OK);
    CHECK_EQ(result.initial_context.len, (size_t)2);
    CHECK_EQ(chdir(cwd), 0);
    oi_session_destroy(registry, result.session);
    result.session = NULL;
    oi_cli_session_switch_result_free(&result);

    /* A log whose header is not an oi log at all cannot be opened, and is
     * reported as corrupt rather than crashing or ending the REPL. */
    snprintf(history_path, sizeof history_path, "%s/%s/history.oilog", root,
             undecodable);
    {
        FILE *garbage = fopen(history_path, "w");
        CHECK(garbage != NULL);
        CHECK(fputs("not an oilog header", garbage) >= 0);
        CHECK_EQ(fclose(garbage), 0);
    }
    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, undecodable,
                                   strlen(undecodable), "gpt-default", cwd,
                                   NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_CORRUPT);
    CHECK(result.session == NULL);
    CHECK_EQ(registry_size(registry, undecodable), 0u);
    oi_cli_session_switch_result_free(&result);

    oi_session_registry_destroy(registry);
    remove_session(root, recoverable);
    remove_session(root, undecodable);
    rmdir(root);
    free(recoverable);
    free(undecodable);
    free(root);
    free(cwd);
}

TEST(a_failed_switch_leaves_the_working_directory_alone) {
    char *root = test_root("cwd-rollback");
    char *original_cwd = getcwd(NULL, 0);
    oi_session_registry *registry = oi_session_registry_create();
    struct oi_cli_session_switch_result result;
    char *id;

    CHECK(registry != NULL);
    id = seed_session(root, "gpt-a", original_cwd, 1);

    /*
     * Restoring a target's settings chdir()s the process. A switch that
     * fails afterwards must not leave the caller -- which keeps using the
     * old session -- standing in the target's directory.
     *
     * Here the failure is provoked before any chdir (an undecodable log),
     * which is the case that must clearly not move it.
     */
    {
        char history_path[384];
        FILE *garbage;
        snprintf(history_path, sizeof history_path, "%s/%s/history.oilog",
                 root, id);
        garbage = fopen(history_path, "w");
        CHECK(garbage != NULL);
        CHECK(fputs("garbage", garbage) >= 0);
        CHECK_EQ(fclose(garbage), 0);
    }
    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, id, strlen(id),
                                   "gpt-default", original_cwd, NULL,
                                   &result),
             OI_OK);
    CHECK(result.outcome != OI_CLI_SESSION_SWITCH_OK);
    {
        char now[512];
        CHECK(getcwd(now, sizeof now) != NULL);
        CHECK_STREQ(now, original_cwd);
    }
    oi_cli_session_switch_result_free(&result);

    oi_session_registry_destroy(registry);
    remove_session(root, id);
    rmdir(root);
    free(id);
    free(root);
    free(original_cwd);
}

TEST(a_switch_failing_after_the_target_cwd_is_applied_still_rolls_back) {
    char *root = test_root("cwd-after");
    char *start_dir = test_root("cwd-after-start");
    char *target_dir = test_root("cwd-after-target");
    char *original_cwd = getcwd(NULL, 0);
    oi_session_registry *registry = oi_session_registry_create();
    struct oi_cli_session_switch_result result;
    char *oversized_model;
    char *id;

    CHECK(registry != NULL);
    CHECK(original_cwd != NULL);
    CHECK(mkdir(start_dir, 0700) == 0 || errno == EEXIST);
    CHECK(mkdir(target_dir, 0700) == 0 || errno == EEXIST);

    /*
     * A session with a transition but no durable settings and no metadata
     * cache, so both model and cwd resolve to the defaults passed in.
     */
    {
        struct oi_cli_session_location location;
        struct oi_cli_history_store store;
        struct oi_cli_history_replay_state state;
        struct oi_cli_history_record record;
        oi_sesslog *log = NULL;

        oi_cli_session_location_init(&location);
        CHECK_EQ(oi_cli_session_location_create(root, &location), OI_OK);
        CHECK_EQ(oi_sesslog_open(location.history_path, &log), OI_OK);
        oi_cli_history_store_init(&store);
        oi_cli_history_replay_state_init(&state);
        CHECK_EQ(oi_cli_history_store_load(log, &store, &state), OI_OK);
        oi_cli_history_record_init(&record);
        CHECK_EQ(oi_cli_history_record_set_transition(&record,
                                                     state.next_record_id, 0),
                 OI_OK);
        CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
        oi_cli_history_record_free(&record);
        oi_cli_history_store_free(&store);
        oi_cli_history_replay_state_free(&state);
        oi_sesslog_close(log);
        id = strdup(location.id);
        CHECK(id != NULL);
        oi_cli_session_location_free(&location);
    }

    /*
     * A default model longer than a durable setting value may be. Settings
     * restoration chdir()s into the resolved cwd *first* and only then
     * appends the model record, so this fails strictly after the working
     * directory has already moved -- the case the earlier rollback test,
     * which fails before any chdir, does not reach.
     */
    oversized_model = malloc(OI_CLI_HISTORY_MAX_SETTING_VALUE + 2);
    CHECK(oversized_model != NULL);
    memset(oversized_model, 'm', OI_CLI_HISTORY_MAX_SETTING_VALUE + 1);
    oversized_model[OI_CLI_HISTORY_MAX_SETTING_VALUE + 1] = '\0';

    CHECK_EQ(chdir(start_dir), 0);
    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, id, strlen(id),
                                   oversized_model, target_dir, NULL,
                                   &result),
             OI_OK);
    CHECK(result.outcome != OI_CLI_SESSION_SWITCH_OK);
    CHECK(result.session == NULL);
    /* Rolled back out of target_dir, all the way to where we started. */
    {
        char now[512];
        CHECK(getcwd(now, sizeof now) != NULL);
        CHECK_STREQ(now, start_dir);
    }
    /* And nothing was left registered. */
    CHECK_EQ(registry_size(registry, id), 0u);
    oi_cli_session_switch_result_free(&result);

    CHECK_EQ(chdir(original_cwd), 0);
    free(oversized_model);
    oi_session_registry_destroy(registry);
    remove_session(root, id);
    rmdir(root);
    rmdir(start_dir);
    rmdir(target_dir);
    free(id);
    free(root);
    free(start_dir);
    free(target_dir);
    free(original_cwd);
}

TEST(switch_repairs_an_interrupted_turn_in_the_target) {
    char *root = test_root("repair");
    char *cwd = getcwd(NULL, 0);
    oi_session_registry *registry = oi_session_registry_create();
    struct oi_cli_session_switch_result result;
    struct oi_cli_session_location location;
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_history_record record;
    oi_sesslog *log = NULL;
    char *id;

    CHECK(registry != NULL);
    /* A session whose last turn has a user message and no reply -- exactly
     * what a crash mid-turn leaves behind. */
    oi_cli_session_location_init(&location);
    CHECK_EQ(oi_cli_session_location_create(root, &location), OI_OK);
    CHECK_EQ(oi_sesslog_open(location.history_path, &log), OI_OK);
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
    set_user_record(&record, state.next_record_id, state.next_turn_id,
                    "unanswered");
    CHECK_EQ(oi_cli_history_store_append(&store, &record, &state), OI_OK);
    oi_cli_history_record_free(&record);
    oi_cli_history_store_free(&store);
    oi_cli_history_replay_state_free(&state);
    oi_sesslog_close(log);
    id = strdup(location.id);
    CHECK(id != NULL);
    oi_cli_session_location_free(&location);

    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, id, strlen(id),
                                   "gpt-default", cwd, NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_OK);
    /* The interrupted turn was repaired durably during the switch, so the
     * replay state no longer asks for it. */
    CHECK(!result.state.needs_repair);

    CHECK_EQ(chdir(cwd), 0);
    oi_session_destroy(registry, result.session);
    result.session = NULL;
    oi_cli_session_switch_result_free(&result);

    /* Reopening sees the repair already on disk rather than needing it
     * again -- the switch's appends were durable, not in-memory. */
    oi_cli_session_switch_result_init(&result);
    CHECK_EQ(oi_cli_session_switch(registry, root, NULL, id, strlen(id),
                                   "gpt-default", cwd, NULL, &result),
             OI_OK);
    CHECK_EQ(result.outcome, OI_CLI_SESSION_SWITCH_OK);
    CHECK(!result.state.needs_repair);
    CHECK_EQ(chdir(cwd), 0);
    oi_session_destroy(registry, result.session);
    result.session = NULL;
    oi_cli_session_switch_result_free(&result);

    oi_session_registry_destroy(registry);
    remove_session(root, id);
    rmdir(root);
    free(id);
    free(root);
    free(cwd);
}

int main(void) {
    RUN(switch_opens_replays_and_restores_the_target);
    RUN(switch_to_the_same_session_is_a_no_op);
    RUN(switch_rejects_unsafe_ids_and_missing_sessions);
    RUN(switch_to_a_session_held_elsewhere_reports_busy_and_registers_nothing);
    RUN(switch_recovers_a_truncated_tail_but_refuses_an_undecodable_log);
    RUN(a_failed_switch_leaves_the_working_directory_alone);
    RUN(a_switch_failing_after_the_target_cwd_is_applied_still_rolls_back);
    RUN(switch_repairs_an_interrupted_turn_in_the_target);
    return oi_test_report();
}
