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
 * Resolves the sessions root a lifecycle operation should act on: a
 * caller-owned copy of `root_override` when it is non-NULL, else the
 * platform default. OI_ERR_INVAL for an empty override.
 */
oi_status oi_cli_sessions_root(const char *root_override, char **out_root);

/*
 * Builds the caller-owned path of the authoritative history log inside a
 * private session directory. Keeps that file name a single fact owned by
 * this module rather than a string every caller repeats.
 */
oi_status oi_cli_session_history_path(const char *directory,
                                      char **out_history_path);

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

/*
 * Whether another process currently holds a session log's flock. Probed
 * read-only (see oi_cli_sessions_enumerate) -- a busy session is a normal
 * selectable-but-not-openable state, never an error.
 */
enum oi_cli_session_lock_state {
    OI_CLI_SESSION_LOCK_FREE = 0,
    OI_CLI_SESSION_LOCK_BUSY,
    OI_CLI_SESSION_LOCK_UNKNOWN /* the probe itself failed */
};

/*
 * One selectable session, as shown by /session list.
 *
 * These fields are presentational, deliberately kept separate from
 * struct oi_cli_session_metadata: that struct's validity invariant
 * requires a non-empty model and cwd, which a session whose metadata is
 * missing and whose history cannot be replayed genuinely does not have.
 * Such a session must still be listed (so the user can trash it), so
 * every field here is individually optional -- `model`/`cwd` may be
 * empty (data == NULL) and the timestamps may be 0, meaning "unknown".
 * Only `id` is always present.
 */
struct oi_cli_session_list_entry {
    char *id; /* owned; always non-NULL */
    /* User-chosen label; empty means unset, so display the id instead.
     * Never recoverable from history -- it lives only in the cache -- so
     * a degraded entry always reports it empty. */
    struct oi_cli_string display_name;
    struct oi_cli_string model; /* empty if unknown */
    struct oi_cli_string cwd;   /* empty if unknown */
    int64_t created_at;         /* Unix epoch seconds; 0 if unknown */
    int64_t updated_at;         /* 0 if unknown */
    int degraded; /* metadata.json was missing or malformed */
    enum oi_cli_session_lock_state lock_state;
};

struct oi_cli_session_list {
    struct oi_cli_session_list_entry *entries; /* owned */
    size_t len;
    size_t cap;
};

void oi_cli_session_list_init(struct oi_cli_session_list *list);
void oi_cli_session_list_free(struct oi_cli_session_list *list);

/*
 * Enumerates every live session directly under the sessions root
 * (`root_override`, or the platform default when NULL), most recently
 * updated first. `out_list` must be initialized, and is replaced only on
 * success.
 *
 * Entries in the root that are not sessions -- anything failing
 * oi_cli_session_id_is_safe (dotfiles and the ".trash" subdirectory
 * included) and anything that is not a plain directory -- are skipped
 * silently: they are not oi's data, so they are not the user's problem.
 *
 * Cost is bounded per entry and never replays a healthy session's log:
 * the fast path is one flock probe plus one bounded metadata.json read,
 * and it never opens the log, so listing does not write to it.
 *
 * A session whose metadata.json is missing or malformed is marked
 * `degraded` and rebuilt by replaying that one session's history -- so
 * the worst case is one replay per damaged session, never a replay of
 * every log. Note that this path does open the log, and so performs
 * oi_sesslog_open's documented crash recovery on it (truncating an
 * incomplete trailing record); that is the same repair a real open would
 * do, and only damaged sessions reach it. A degraded session that is also
 * busy cannot be replayed at all (another process holds the lock) and is
 * listed with whatever is known, which may be nothing beyond its id.
 *
 * A missing root is not an error: it means no sessions exist yet, and
 * yields an empty list.
 */
oi_status oi_cli_sessions_enumerate(const char *root_override,
                                    struct oi_cli_session_list *out_list);

/*
 * Sets a session's display name, leaving its directory and history
 * untouched.
 *
 * The safe directory id is permanent by design. Every path in the storage
 * model is derived from it -- history.oilog, metadata.json, the registry
 * entry of a live session, and anything outside oi pointing at the
 * directory -- so renaming the directory to change a cosmetic label would
 * mean updating all of them atomically. A name is a label; it is not
 * worth that.
 *
 * Only live sessions can be renamed; a trashed one is not addressable
 * here (restore it first). Rebuilds a missing or malformed metadata cache
 * from history before writing, so naming a damaged session repairs it
 * rather than failing.
 *
 * Needs no lock: it writes only metadata.json, which is already safe to
 * write while another process appends to the log -- the same reason
 * /model and /cwd take no lock to refresh the cache.
 *
 * `*out_error_detail`, when non-NULL on return, is a caller-owned string
 * naming the specific cause and is set only on failure.
 *
 *   OI_ERR_INVAL    unsafe id, or a name that is empty, too long, or
 *                   contains a control byte
 *   OI_ERR_NOTFOUND no live session with that id
 *   OI_ERR_IO       the metadata write failed, or the session has neither a
 *                   cache nor a readable history to rebuild one from, so
 *                   there is nowhere to record a name
 */
oi_status oi_cli_session_rename(const char *root_override, const char *id,
                                size_t id_len, const char *new_name,
                                size_t new_name_len,
                                char **out_error_detail);

/*
 * Trash, restore, and permanent delete.
 *
 * Trashing renames a session's whole private directory from <root>/<id> to
 * <root>/.trash/<id>. A directory rename was chosen over a "trashed" flag
 * in the cache for three reasons: rename(2) within a filesystem is atomic,
 * so no partial state exists; a trashed session simply is not in <root>
 * anymore, which keeps oi_cli_sessions_enumerate correct with no filtering
 * (".trash" already fails oi_cli_session_id_is_safe); and the trash lives
 * under the same root as every session, so the rename is same-filesystem
 * by construction.
 *
 * Cross-device is therefore not expected, but is detected and reported
 * distinctly rather than folded into a generic I/O error -- a relocated or
 * bind-mounted .trash would otherwise fail inexplicably.
 *
 * Delete only ever operates on an already-trashed session; there is no
 * one-step hard delete of a live one. That makes "refuses the current
 * session" and "refuses a session open elsewhere" structural rather than
 * merely checked: a live session is not in .trash, so delete cannot reach
 * it at all.
 *
 * `current_session_id` may be NULL when no session is active. All three
 * set `*out_error_detail` (caller-owned) on failure when it is non-NULL.
 *
 *   OI_ERR_INVAL    unsafe id, or trashing the active session
 *   OI_ERR_NOTFOUND no such session in the relevant location
 *   OI_ERR_EXISTS   held by another process, or (restore) the id is live
 *   OI_ERR_IO       the rename or removal failed, whether the session is in
 *                   use could not be determined, or (delete) the directory
 *                   holds something oi did not put there; detail says which
 */
oi_status oi_cli_session_trash(const char *root_override, const char *id,
                               size_t id_len, const char *current_session_id,
                               char **out_error_detail);
oi_status oi_cli_session_restore_trashed(const char *root_override,
                                         const char *id, size_t id_len,
                                         char **out_error_detail);
oi_status oi_cli_session_delete(const char *root_override, const char *id,
                                size_t id_len, char **out_error_detail);

/*
 * Enumerates trashed sessions, so /session restore can offer a choice.
 * Same contract and cost model as oi_cli_sessions_enumerate; an absent
 * trash directory yields an empty list rather than an error.
 */
oi_status oi_cli_sessions_enumerate_trash(
    const char *root_override, struct oi_cli_session_list *out_list);

/*
 * Imports an existing ".oilog" file -- typically a legacy project-local
 * one -- as a brand-new private session, and reports its id.
 *
 * The source is never opened for writing, never renamed, and never
 * unlinked: import copies out of it and leaves it exactly as it was. That
 * is structural here, not a convention to be careful about.
 *
 * The source must be a plain regular file. A symlink is refused (checked
 * via lstat, so it is never followed), as is a file already inside the
 * sessions root -- that is an oi-managed session already, and /session
 * switch is the operation the user wanted.
 *
 * The copy is validated before it is adopted, by opening it as a session
 * log and replaying it with the very code a real open would use. Only then
 * is a session directory created and the validated copy moved into place.
 * A file that fails validation leaves nothing behind.
 *
 * No transition or settings record is appended here. That already happens
 * for any session the first time it is actually opened, so import stays a
 * pure validate-and-relocate step with no duplicated durable-write logic.
 *
 * On OI_OK, `*out_new_id` is a caller-owned id string. On failure it is
 * untouched and `*out_error_detail` (when non-NULL) names the cause.
 */
oi_status oi_cli_session_import(const char *root_override,
                                const char *source_path,
                                size_t source_path_len, char **out_new_id,
                                char **out_error_detail);

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
 * `explicit_model` (may be NULL) always wins. Otherwise replayed history
 * wins: `state`'s last-known model/cwd; then, only if history records no
 * such setting at all, valid metadata whose session_id matches
 * `session_id`; then `default_model`/`default_cwd`.
 *
 * History outranking the cache is load-bearing. The cache is refreshed
 * best-effort *after* a change is already durable in history, so a failed
 * refresh leaves it stale. Were the cache consulted first, that one failed
 * write would make the next open resolve the stale value and then append
 * it back into the authoritative log -- an unwritable cache would silently
 * rewrite history. The cache's only unique contribution is `created_at`.
 *
 * A missing/unreachable resolved cwd
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
