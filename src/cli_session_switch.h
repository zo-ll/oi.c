#ifndef OI_CLI_SESSION_SWITCH_H
#define OI_CLI_SESSION_SWITCH_H

#include <stdio.h>

#include "cli_history_replay.h"
#include "cli_history_store.h"
#include "cli_message.h"
#include "cli_sessions.h"
#include "oi/session.h"
#include "oi/status.h"

/*
 * Why every business-logic failure is an outcome rather than a status: a
 * /session switch that cannot happen must never end the REPL. Only a
 * genuine structural failure (allocation) comes back as a non-OK
 * oi_status; everything a user can provoke lands here instead.
 */
enum oi_cli_session_switch_outcome {
    OI_CLI_SESSION_SWITCH_OK = 0,
    OI_CLI_SESSION_SWITCH_SAME,      /* already on that session */
    OI_CLI_SESSION_SWITCH_NOT_FOUND, /* no such session directory */
    OI_CLI_SESSION_SWITCH_BUSY,      /* another process holds the log */
    OI_CLI_SESSION_SWITCH_INVALID,   /* the id is not a safe id */
    OI_CLI_SESSION_SWITCH_CORRUPT    /* exists, but cannot be loaded or
                                      * prepared for use */
};

/*
 * What a successful switch hands over. On OI_CLI_SESSION_SWITCH_OK the
 * caller adopts every field: `session` is already open and registered,
 * `store`/`state` are moved out and owned by the caller, and the remaining
 * allocations are the caller's to release.
 *
 * `session` is never owned by this struct -- on success the caller owns it,
 * and on any other outcome it is NULL because the switch already rolled its
 * own registration back.
 *
 * A caller that takes ownership of a field must zero or re-init that field
 * before calling oi_cli_session_switch_result_free, which frees whatever is
 * still present. Calling free on a moved-from result is otherwise a double
 * free.
 */
struct oi_cli_session_switch_result {
    enum oi_cli_session_switch_outcome outcome;
    oi_session *session;
    struct oi_cli_history_store store;
    struct oi_cli_history_replay_state state;
    struct oi_cli_message_list initial_context;
    struct oi_cli_string model;
    struct oi_cli_string cwd;
    char *path;          /* owned private session directory */
    char *metadata_path; /* owned selector-cache path */
};

void oi_cli_session_switch_result_init(
    struct oi_cli_session_switch_result *result);
void oi_cli_session_switch_result_free(
    struct oi_cli_session_switch_result *result);

/*
 * Opens, replays, repairs, and settings-restores the session `target_id`
 * under the sessions root, without touching the currently-active session.
 *
 * Purely additive to the registry until the outcome is OK: on every other
 * outcome anything registered here has already been destroyed, so the
 * registry is exactly as it was on entry. That is what lets a failed switch
 * leave the original session active and usable -- this function never has
 * the old session to damage in the first place.
 *
 * Restoring the target's working directory chdir()s the process. If a later
 * step then fails, the original working directory is restored before
 * returning, so a failed switch does not leave the process sitting in the
 * target's directory while the caller keeps using the old session.
 *
 * That guarantee is upheld rather than attempted. If the current directory
 * cannot be read up front, the switch is refused with OI_ERR_IO before any
 * chdir happens, since a rollback would then be impossible. If the rollback
 * chdir itself fails, the result is OI_ERR_IO too, not a recoverable
 * outcome: the caller must not be told to carry on with the old session
 * from a directory it never chose.
 *
 * `default_model` and `default_cwd` must both be non-empty; they are the
 * fallbacks used when neither the target's metadata nor its replayed
 * history names one. `diagnostics` may be NULL. `current_session_id` may be
 * NULL when no session is active.
 *
 * Returns non-OK only for a structural failure. Check
 * `out_result->outcome` on OI_OK.
 */
oi_status oi_cli_session_switch(
    oi_session_registry *registry, const char *root_override,
    const char *current_session_id, const char *target_id,
    size_t target_id_len, const char *default_model, const char *default_cwd,
    FILE *diagnostics, struct oi_cli_session_switch_result *out_result);

#endif
