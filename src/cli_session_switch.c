#include "cli_session_switch.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli_history_repair.h"

void oi_cli_session_switch_result_init(
    struct oi_cli_session_switch_result *result) {
    if (result == NULL) {
        return;
    }
    result->outcome = OI_CLI_SESSION_SWITCH_OK;
    result->session = NULL;
    oi_cli_history_store_init(&result->store);
    oi_cli_history_replay_state_init(&result->state);
    oi_cli_message_list_init(&result->initial_context);
    memset(&result->model, 0, sizeof result->model);
    memset(&result->cwd, 0, sizeof result->cwd);
    result->path = NULL;
    result->metadata_path = NULL;
}

void oi_cli_session_switch_result_free(
    struct oi_cli_session_switch_result *result) {
    if (result == NULL) {
        return;
    }
    /* Never destroys `session`: on success the caller owns it, and on any
     * other outcome the switch already rolled its registration back. */
    oi_cli_history_store_free(&result->store);
    oi_cli_history_replay_state_free(&result->state);
    oi_cli_message_list_free(&result->initial_context);
    oi_cli_string_free(&result->model);
    oi_cli_string_free(&result->cwd);
    free(result->path);
    free(result->metadata_path);
    oi_cli_session_switch_result_init(result);
}

/* Appends the schema transition and any interrupted-turn repairs the replay
 * asked for -- the same preparation every other session-open path performs,
 * so a switched-to session is indistinguishable from one opened directly. */
static oi_status prepare_history(struct oi_cli_history_store *store,
                                 struct oi_cli_history_replay_state *state) {
    oi_status status = OI_OK;

    if (state->needs_transition) {
        struct oi_cli_history_record transition;

        oi_cli_history_record_init(&transition);
        status = oi_cli_history_record_set_transition(
            &transition, state->next_record_id, store->legacy_messages.len);
        if (status == OI_OK) {
            status = oi_cli_history_store_append(store, &transition, state);
        }
        oi_cli_history_record_free(&transition);
    }
    if (status == OI_OK && state->needs_repair) {
        struct oi_cli_history repairs;
        size_t index;

        oi_cli_history_init(&repairs);
        status = oi_cli_history_build_repairs(state, &repairs);
        for (index = 0; status == OI_OK && index < repairs.len; index++) {
            status = oi_cli_history_store_append(store, &repairs.records[index],
                                                 state);
        }
        oi_cli_history_free(&repairs);
    }
    return status;
}

static oi_status clone_context(const struct oi_cli_history_replay_state *state,
                               struct oi_cli_message_list *out_context) {
    size_t index;

    for (index = 0; index < state->context_len; index++) {
        oi_status status = oi_cli_message_list_append_clone(
            out_context, &state->context[index].message);
        if (status != OI_OK) {
            return status;
        }
    }
    return OI_OK;
}

oi_status oi_cli_session_switch(
    oi_session_registry *registry, const char *root_override,
    const char *current_session_id, const char *target_id,
    size_t target_id_len, const char *default_model, const char *default_cwd,
    FILE *diagnostics, struct oi_cli_session_switch_result *out_result) {
    struct oi_cli_session_switch_result result;
    struct oi_cli_session_restore restore;
    char terminated[OI_CLI_SESSION_SAFE_ID_MAX_LEN + 1];
    char *root = NULL;
    char *directory = NULL;
    char *history_path = NULL;
    char *saved_cwd = NULL;
    int is_new_history;
    oi_status status;

    if (registry == NULL || out_result == NULL || default_model == NULL ||
        default_model[0] == '\0' || default_cwd == NULL ||
        default_cwd[0] == '\0') {
        return OI_ERR_INVAL;
    }
    oi_cli_session_switch_result_init(out_result);
    oi_cli_session_switch_result_init(&result);

    /* Cheapest check first, and no I/O for it. */
    if (current_session_id != NULL &&
        strlen(current_session_id) == target_id_len &&
        memcmp(current_session_id, target_id, target_id_len) == 0) {
        out_result->outcome = OI_CLI_SESSION_SWITCH_SAME;
        return OI_OK;
    }
    if (!oi_cli_session_id_is_safe(target_id, target_id_len)) {
        out_result->outcome = OI_CLI_SESSION_SWITCH_INVALID;
        return OI_OK;
    }
    memcpy(terminated, target_id, target_id_len);
    terminated[target_id_len] = '\0';

    status = oi_cli_sessions_root(root_override, &root);
    if (status != OI_OK) {
        return status;
    }
    status = oi_cli_session_resolve(root, target_id, target_id_len,
                                    &directory);
    free(root);
    if (status != OI_OK) {
        /* Both "absent" and "present but not a plain directory" mean there
         * is nothing here the user can switch to. */
        out_result->outcome = OI_CLI_SESSION_SWITCH_NOT_FOUND;
        return OI_OK;
    }
    result.path = directory;
    directory = NULL;

    status = oi_cli_session_history_path(result.path, &history_path);
    if (status == OI_OK) {
        status = oi_cli_session_metadata_path_for_log(history_path, 1,
                                                      &result.metadata_path);
    }
    if (status != OI_OK) {
        oi_cli_session_switch_result_free(&result);
        free(history_path);
        return status;
    }

    /* From here on, every failure must undo whatever it registered. */
    status = oi_session_create(registry, terminated, history_path, 0,
                               &result.session);
    free(history_path);
    if (status != OI_OK) {
        result.session = NULL;
        oi_cli_session_switch_result_free(&result);
        /* Running out of memory is structural, not a property of the
         * target: reporting it as "corrupt" would blame a session that is
         * perfectly fine and hide a real failure from the caller. */
        if (status == OI_ERR_NOMEM) {
            return status;
        }
        /* OI_ERR_EXISTS is the flock already being held -- either by
         * another process, or by this one if the id is somehow already
         * registered. Either way it is not switchable right now. */
        out_result->outcome = status == OI_ERR_EXISTS
                                  ? OI_CLI_SESSION_SWITCH_BUSY
                                  : OI_CLI_SESSION_SWITCH_CORRUPT;
        return OI_OK;
    }

    status = oi_cli_history_store_load(oi_session_log(result.session),
                                       &result.store, &result.state);
    if (status != OI_OK) {
        oi_session_destroy(registry, result.session);
        result.session = NULL;
        oi_cli_session_switch_result_free(&result);
        out_result->outcome = OI_CLI_SESSION_SWITCH_CORRUPT;
        return OI_OK;
    }
    /* Captured before any append below, so it reflects whether this session
     * had any pre-existing records at all -- the same convention every
     * other open path uses to decide is_new_session. */
    is_new_history = result.store.typed_history.len == 0;

    status = prepare_history(&result.store, &result.state);
    if (status == OI_OK) {
        status = clone_context(&result.state, &result.initial_context);
    }
    if (status != OI_OK) {
        oi_session_destroy(registry, result.session);
        result.session = NULL;
        oi_cli_session_switch_result_free(&result);
        /* An allocation failure here is structural; a durable-append
         * failure means this session is not usable. Distinguish so the
         * REPL does not end over a merely unusable target. */
        if (status == OI_ERR_NOMEM) {
            return status;
        }
        out_result->outcome = OI_CLI_SESSION_SWITCH_CORRUPT;
        return OI_OK;
    }

    /*
     * restore_settings chdir()s into the target's working directory. If it
     * then fails, the caller keeps using the old session, so the process
     * must not be left standing in the target's directory.
     *
     * That rollback is only possible if the current directory is known, so
     * a getcwd failure refuses the switch outright rather than proceeding
     * into a chdir it could not undo. Better to decline a switch than to
     * leave the old session running from an unknown directory.
     */
    saved_cwd = getcwd(NULL, 0);
    if (saved_cwd == NULL) {
        oi_session_destroy(registry, result.session);
        result.session = NULL;
        oi_cli_session_switch_result_free(&result);
        if (diagnostics != NULL) {
            fprintf(diagnostics,
                    "oi: cannot switch sessions without knowing the current "
                    "working directory\n");
        }
        return OI_ERR_IO;
    }
    oi_cli_session_restore_init(&restore);
    status = oi_cli_session_restore_settings(
        &result.store, &result.state, result.metadata_path, terminated,
        is_new_history, /*explicit_model=*/NULL, default_model, default_cwd,
        diagnostics, &restore);
    if (status == OI_OK) {
        status = oi_cli_string_set(&result.model, restore.model.data,
                                   restore.model.len);
    }
    if (status == OI_OK) {
        status = oi_cli_string_set(&result.cwd, restore.cwd.data,
                                   restore.cwd.len);
    }
    oi_cli_session_restore_free(&restore);
    if (status != OI_OK) {
        /*
         * Put the process back where the caller left it. If even that
         * fails, the old session cannot safely continue -- it would be
         * running from the target's directory -- so this stops being a
         * recoverable business outcome and becomes a real error. Reporting
         * it as merely "corrupt target" would tell the caller to carry on
         * in a directory it did not choose.
         */
        int restored = chdir(saved_cwd) == 0;
        if (!restored && diagnostics != NULL) {
            fprintf(diagnostics,
                    "oi: could not return to \"%s\" after a failed session "
                    "switch\n",
                    saved_cwd);
        }
        free(saved_cwd);
        oi_session_destroy(registry, result.session);
        result.session = NULL;
        oi_cli_session_switch_result_free(&result);
        if (!restored) {
            return OI_ERR_IO;
        }
        if (status == OI_ERR_NOMEM) {
            return status;
        }
        out_result->outcome = OI_CLI_SESSION_SWITCH_CORRUPT;
        return OI_OK;
    }
    free(saved_cwd);

    /* Committed. Hand everything over in one move; the source is re-inited
     * rather than freed, since ownership transferred rather than ended. */
    *out_result = result;
    oi_cli_session_switch_result_init(&result);
    out_result->outcome = OI_CLI_SESSION_SWITCH_OK;
    return OI_OK;
}
