#ifndef OI_CLI_SESSIONS_H
#define OI_CLI_SESSIONS_H

#include <stdio.h>

#include "cli_history.h"
#include "cli_history_replay.h"
#include "cli_history_store.h"
#include "cli_message.h"
#include "oi/status.h"

/*
 * Longest session id the lifecycle commands accept. Deliberately
 * independent of the internal buffer used to *generate* new ids: an id
 * that came back off the filesystem is untrusted input, and only has to
 * be bounded, not necessarily producible by this version of oi.
 */
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

/*
 * Creates a fresh private session directory. `root_override`, when non-NULL,
 * replaces the platform state directory. All returned strings are owned by
 * `out_location`.
 */
oi_status oi_cli_session_location_create(
    const char *root_override,
    struct oi_cli_session_location *out_location);

/* Returns the caller-owned platform default sessions directory. */
oi_status oi_cli_sessions_default_root(char **out_root);

/*
 * Syntax-only check -- never touches the filesystem: is `id` usable as a
 * single path component directly under the sessions root?
 *
 * Accepts 1..OI_CLI_SESSION_SAFE_ID_MAX_LEN bytes drawn only from
 * [A-Za-z0-9_-]. Leaving '.' and '/' out of that charset is what
 * structurally rejects "", ".", "..", every hidden entry (including the
 * ".trash" subdirectory the lifecycle commands keep there), embedded NUL
 * bytes, and every embedded path separator -- so no separate traversal
 * check exists, or is wanted, anywhere above this function.
 *
 * A leading '-' is accepted: session ids are never handed to an
 * argv/getopt-style parser, so there is no option-injection reading for
 * one to be confused with.
 *
 * Returns 1 when safe, 0 otherwise (`id == NULL` included).
 */
int oi_cli_session_id_is_safe(const char *id, size_t id_len);

/*
 * Resolves a session id to its live directory under `root`, applying
 * oi_cli_session_id_is_safe itself -- callers need not pre-validate.
 *
 * The entry is checked with lstat(2), never stat(2), and must satisfy
 * S_ISDIR on that result: a symlink at <root>/<id> therefore fails
 * outright instead of being followed, which is how an attempt to escape
 * the sessions root via a planted symlink is refused.
 *
 * On OI_OK `*out_directory` is a caller-owned string; on every error it
 * is left untouched.
 *
 *   OI_ERR_INVAL    bad arguments, unsafe id, or the entry exists but is
 *                   not a plain directory (symlink, regular file, device)
 *   OI_ERR_NOTFOUND nothing exists at that path
 *   OI_ERR_IO       lstat(2) failed for any other reason, e.g. EACCES
 *
 * Residual TOCTOU, accepted knowingly: a same-privilege local attacker
 * could swap the directory between this check and a later open of a file
 * inside it. Closing that would mean threading a dirfd through
 * oi_sesslog_open and the metadata store -- oi_sesslog_open lives in the
 * public header include/oi/sesslog.h, which CLI-only lifecycle policy
 * must not change. Documented rather than half-closed.
 */
oi_status oi_cli_session_resolve(const char *root, const char *id,
                                 size_t id_len, char **out_directory);

/*
 * Derives a session's metadata path from its ".oilog" history path.
 * `is_private_directory` selects which naming rule applies:
 *  - true: the automatic/lazy private-directory layout, where each
 *    session has its own directory -- the literal "metadata.json" is
 *    unambiguous there (docs/REPL_PLAN.md's storage model).
 *  - false: the flat --session-dir legacy layout, where one directory
 *    can hold multiple "<id>.oilog" files -- derives "<id>.metadata.json"
 *    instead. OI_ERR_INVAL if `log_path` doesn't end in ".oilog".
 */
oi_status oi_cli_session_metadata_path_for_log(
    const char *log_path, int is_private_directory,
    char **out_metadata_path);

struct oi_cli_session_restore {
    struct oi_cli_string model;
    struct oi_cli_string cwd;
    int metadata_missing_or_corrupt; /* diagnostic already printed if so */
    int cwd_fallback_applied;        /* diagnostic already printed if so */
};

void oi_cli_session_restore_init(struct oi_cli_session_restore *restore);
void oi_cli_session_restore_free(struct oi_cli_session_restore *restore);

/*
 * Resolves and applies the effective model/cwd for a session at open
 * time, chdir()ing the process, and durably recording any change (a
 * brand-new session's first-ever values, an explicit_model override, or
 * a cwd fallback correction) through `store`/`state` before refreshing
 * `metadata_path`.
 *
 * `is_new_session` must be true for every automatic session (each
 * process run always creates a fresh directory) and, for an explicit
 * --session ID, only when the load found zero pre-existing records,
 * checked before this run appended anything (including its own
 * transition record).
 *
 * `explicit_model` (may be NULL) always wins. Otherwise: for an existing
 * session, valid metadata whose session_id matches `session_id` wins;
 * else `state`'s last-known model/cwd from replayed history; else
 * `default_model`/`default_cwd`. A missing/unreachable resolved cwd
 * falls back to `default_cwd` (the caller should pass the process's
 * actual current directory, captured before any chdir could have
 * happened) with a diagnostic on `diagnostics` (ignored if NULL), never
 * leaving cwd partially changed.
 *
 * Returns a real error only for a structural/storage failure (the
 * authoritative history append itself failing) -- callers should route
 * that through the existing session-failure path. A metadata.json write
 * failure alone is logged to `diagnostics` and treated as non-fatal: the
 * change is already durable in history by that point, and metadata
 * self-heals on the next open.
 */
oi_status oi_cli_session_restore_settings(
    struct oi_cli_history_store *store,
    struct oi_cli_history_replay_state *state, const char *metadata_path,
    const char *session_id, int is_new_session, const char *explicit_model,
    const char *default_model, const char *default_cwd, FILE *diagnostics,
    struct oi_cli_session_restore *out_restore);

/* Live single-setting change, reused by /model and /cwd. Appends the
 * durable record, refreshes the replay state, then best-effort refreshes
 * metadata.json (a metadata write failure here is not reported to the
 * caller -- the change is already durable in history). */
oi_status oi_cli_session_apply_setting(
    struct oi_cli_history_store *store,
    struct oi_cli_history_replay_state *state, const char *metadata_path,
    const char *session_id, enum oi_cli_history_session_setting_field field,
    const char *value, size_t value_len);

#endif
