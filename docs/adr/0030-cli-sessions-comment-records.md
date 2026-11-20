# cli_sessions: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_sessions.c`, `src/cli_sessions.h`, `test/test_cli_sessions.c`.

## `session_history_name` (src/cli_sessions.c)

The one file name every private session directory holds its authoritative history in (docs/REPL_PLAN.md's storage model).

## `oi_cli_session_id_is_safe.byte` (src/cli_sessions.c)

Unsigned so the comparisons do not depend on whether plain char is signed on this target; a high-bit byte must fail either way.

## `oi_cli_session_resolve` (src/cli_sessions.c)

Bounded by the id_len ceiling oi_cli_session_id_is_safe just enforced, so this cannot overrun `terminated`.

S_ISDIR on the lstat result: a symlink -- even one pointing at a perfectly good directory -- is S_ISLNK here and so refused.

## `set_error_detail` (src/cli_sessions.c)

Records why a lifecycle operation failed, for the command layer to show verbatim. Best effort: a failed strdup just leaves the caller with no detail, which is strictly better than turning a useful error into an out-of-memory one.

The only remaining rejection is a control byte in the name.

## `resolve_root` (src/cli_sessions.c)

Shorthand for the many internal callers that already validated their arguments and only need the root.

## `resolve_trash_root` (src/cli_sessions.c)

The trash lives inside the sessions root, so trash and restore are same-filesystem renames by construction. Its name starts with '.', so oi_cli_session_id_is_safe already excludes it from enumeration and it can never collide with a real session id.

## `metadata_lock_acquire` (src/cli_sessions.c)

Serializes read-modify-write of one session's metadata.json.

Both updaters -- a settings refresh carrying the display name forward, and a rename carrying model/cwd forward -- must read the current cache, merge one field, and replace the file. Without a lock those interleave and lose each other's field: a rename that read before a concurrent /model landed will write the stale model back, and vice versa.

The lock is a sibling "<metadata path>.lock" rather than metadata.json itself, because the store replaces that file via temp+rename, so its inode is not stable enough to lock. The session log's own flock cannot be reused either: the owning process holds it for its whole lifetime, so a rename from another process would wait forever.

Held only across a bounded read and write, so blocking is fine.

Returns -1 if the lock cannot be taken. Callers must not treat that as permission to proceed unlocked, and the two do different things: a rename fails, because the metadata write *is* the operation, while a settings refresh skips the write, because the change it mirrors is already durable in history and the cache rebuilds from it on the next open.

The lock file is opened O_NOFOLLOW and confirmed to be a regular file. It is the one path here derived by appending to a name rather than resolved through oi_cli_session_resolve, so without that a planted "metadata.json.lock" symlink would be followed and opened O_RDWR|O_CREAT, letting an attacker aim it at an arbitrary file or device.

O_NOFOLLOW rejects a symlink at the final component with ELOOP, and still creates a plain file when nothing is there.

Belt and braces: O_NOFOLLOW covers symlinks, this covers a fifo, device, or directory planted under the same name.

## `probe_lock` (src/cli_sessions.c)

Read-only check for whether another process holds `history_path`'s lock, taking flock(2) directly rather than going through oi_sesslog_open. That matters: oi_sesslog_open creates the file when absent and truncates a trailing record left incomplete by a crash, and neither belongs in a probe that merely answers "is this session busy?" -- so a healthy session is never written to just by being listed.

flock(2) needs no write access, so an O_RDONLY descriptor is enough, and the lock is released by closing it.

Only "would have blocked" actually means another process holds it. Anything else -- EINTR included, which says nothing about the lock -- is honestly unknown rather than assumed busy.

## `probe_lock.errno` (src/cli_sessions.c)

No log at all: nothing holds a lock on it either. Any other failure (permissions) leaves the answer genuinely unknown.

## `rebuild_from_history` (src/cli_sessions.c)

Recovers a damaged session's model/cwd by replaying its own history -- the authoritative record metadata.json is only ever a cache of. Replays exactly one session's log, never more.

Best effort by contract: on any failure (the log is locked by another process, absent, or undecodable) the outputs are simply left as the caller initialized them. A session nothing can be recovered for still has to be listable.

Unlike probe_lock, this does open the log through oi_sesslog_open, which takes the exclusive lock and performs that function's documented crash recovery -- truncating a trailing record left incomplete by a crash. That is unavoidable here, since replay needs an open log, and it is the same recovery a real open of this session would do. It is confined to sessions whose cache is already missing or malformed; a healthy session never reaches this path.

Timestamps first: the log's own mtime is a usable stand-in for "last active" and survives even an undecodable log.

## `describe_session` (src/cli_sessions.c)

Probe before any replay: a busy session cannot be replayed, and knowing that up front avoids attempting it.

The cache must prove it belongs to this directory before it is believed. A metadata.json naming a different session -- a stray copy, a half-finished manual move, a restored backup -- parses perfectly well, so without this check a session would be listed as healthy while showing another session's model, cwd, and name. Treat it exactly like a corrupt cache: degraded, and rebuilt from the history that is actually in this directory.

Missing, malformed, or belonging to another session. Either way the session is still real and still selectable -- rebuild what history can tell us and say so.

## `compare_entries` (src/cli_sessions.c)

Most recently updated first; ties broken by id so the order is deterministic (an id encodes its creation time, so this stays chronologically sensible).

## `enumerate_directory` (src/cli_sessions.c)

Scans one directory of session directories -- the live root or the trash, which have identical layout.

An absent directory simply means nothing is in it yet.

Rejects ".", "..", ".trash", and every other non-session entry in one test -- see oi_cli_session_id_is_safe.

Also refuses a symlink standing in for a session directory.

## `load_or_rebuild_metadata` (src/cli_sessions.c)

Produces a complete, writable metadata struct for an existing session: the cache when it is valid and belongs to this session, otherwise one rebuilt from the session's own history.

OI_ERR_PARSE when neither source yields a model and cwd -- the cache's validity rules require both, and inventing placeholders would be worse than failing, since the cache is what a later open trusts to restore them. Such a session can still be listed and trashed.

`out_metadata` must be initialized and empty on entry; it is replaced only on success.

## `oi_cli_session_rename` (src/cli_sessions.c)

No "clear the name" spelling in this grammar: an empty argument is a usage mistake, not a request to unset.

Read, merge, and replace under the metadata lock. A live process refreshing model/cwd does the mirror-image update, so without this the two interleave and one silently discards the other's field -- a rename writing back a model it read before a concurrent /model landed, or a /model writing back an empty name.

Unlike a settings refresh, a rename cannot simply skip when the lock is unavailable: the metadata write *is* the operation. Proceeding unlocked would risk reverting a concurrent model change, so this fails and says so instead of quietly doing something unsafe or nothing at all.

Passing `metadata`'s own fields back in is safe: the setter copies every argument into a replacement struct and only frees the destination once all of those copies have succeeded.

## `refuse_if_busy` (src/cli_sessions.c)

Refuses to touch a session another process has open. Shared by trash, restore, and delete: moving or removing a directory out from under a live process is exactly the kind of surprise durable history exists to prevent.

## `move_session_directory` (src/cli_sessions.c)

Moves a session directory between the live root and the trash. Both callers hand in an already-resolved source and a destination parent.

Atomic within a filesystem: either the whole directory moved or nothing did, so there is no half-trashed state to recover from.

## `oi_cli_session_trash` (src/cli_sessions.c)

Ids are unique, so an exact match is unambiguous.

## `remove_session_directory` (src/cli_sessions.c)

Empties and removes one trashed session directory.

Deliberately not a generic recursive delete: a session directory holds a known, flat set of regular files (history.oilog, metadata.json, and possibly a metadata.json.tmp from an interrupted write). Anything else -- a nested directory, a symlink, a device -- means this is not a directory oi created, so it fails closed instead of recursing. Deletion should never remove something it did not put there.

Pass one: inspect every entry and remove nothing.

Validating and unlinking in a single pass meant an unexpected entry discovered late -- after history.oilog and metadata.json had already been unlinked -- reported failure over a session that no longer existed and could no longer be restored. "Fails closed" has to mean the directory is untouched, so the decision is made in full before any removal begins.

Pass two: everything is a plain regular file, so remove it all.

A racing creation between the passes could still surface here. unlink refuses a directory with EISDIR, so this stays fail-safe rather than removing something unexpected.

## `oi_cli_session_delete` (src/cli_sessions.c)

Scoped to the trash, which is what makes refusing the current session and refusing a session open elsewhere structural: neither is in the trash, so neither can be found here at all.

## `path_is_inside_root` (src/cli_sessions.c)

True when `file_path` lives somewhere under `root`.

Compares device and inode numbers while walking from the file's own directory up through its parents, rather than comparing path strings. That gets symlinks, "..", and redundant separators right for free -- a textual prefix test would need canonical paths, and realpath(3) sits behind a feature macro this project does not enable.

Answers "no" on any stat failure: this decides whether to show a more helpful error, so guessing wrong merely costs a better message.

A directory that is its own parent is the filesystem root.

## `create_scratch_file` (src/cli_sessions.c)

Creates the import scratch file, O_EXCL, under `root`.

O_EXCL never writes over an existing file, so a stale scratch left by a killed earlier run would otherwise block every future import from a process that happened to reuse its pid. Stepping through a bounded sequence of names sidesteps that without ever overwriting anything.

## `copy_into_fd` (src/cli_sessions.c)

Copies `source` into the already-open `out_fd`, which the caller owns.

## `validate_session_log` (src/cli_sessions.c)

Confirms a copied file really is a session log, using exactly the code that would load it for real -- header validation plus a full strict replay. Nothing cheaper would be honest about it.

## `oi_cli_session_import` (src/cli_sessions.c)

lstat, not stat: S_ISREG on this result is false for a symlink, so a symlinked source is refused rather than followed.

Scratch file beside the eventual destination, so adopting it later is a same-filesystem rename. The leading '.' keeps it out of enumeration for as long as it exists.

## `refresh_metadata` (src/cli_sessions.c)

Refreshes the cache's model/cwd while preserving the fields it alone holds -- the display name, and created_at.

The read and the write happen under the metadata lock, so this cannot lose a display name set concurrently by a rename, and a rename cannot lose the model/cwd written here. Re-reading inside the lock is also why no caller has to thread the existing name through: whatever is on disk at write time is what gets carried forward.

`created_at_fallback` is used only when the cache holds nothing usable for this session.

Skip the refresh rather than race it. Whatever this would have mirrored is already durable in history, and the cache rebuilds from history on the next open, so doing nothing costs nothing. Writing unlocked could instead discard a display name a concurrent rename just set.

refresh_metadata re-reads under the metadata lock, so the display name and created_at are carried forward without being threaded through this function.

created_at and the display name are both recovered inside refresh_metadata, under the metadata lock, so a concurrent rename cannot lose either this change or its own.

## `oi_cli_session_restore_settings` (src/cli_sessions.c)

Replayed history outranks the metadata cache for both settings.

This ordering is load-bearing, not a preference. metadata.json is refreshed on a best-effort basis after a change is already durable in history -- oi_cli_session_apply_setting deliberately ignores a metadata write failure because history is authoritative. If the cache were consulted first, a single failed refresh would make the next open resolve the *stale* model or cwd, and then, seeing it differ from what history last recorded, append that stale value back into the authoritative log. A cache that cannot be written would end up silently rewriting history. So the cache is only a fallback for a session whose history records no setting at all, and its unique contribution is created_at, which history never stores.

Copy into out_restore now: appending durable records below refreshes `state` in place (freeing its previous last_model/last_cwd), which resolved_model/resolved_cwd may currently alias.

## `OI_CLI_SESSION_SAFE_ID_MAX_LEN` (src/cli_sessions.h)

Longest session id the lifecycle commands accept. Deliberately independent of the internal buffer used to *generate* new ids: an id that came back off the filesystem is untrusted input, and only has to be bounded, not necessarily producible by this version of oi.

## `oi_cli_session_location_create` (src/cli_sessions.h)

Creates a fresh private session directory. `root_override`, when non-NULL, replaces the platform state directory. All returned strings are owned by `out_location`.

## `oi_cli_sessions_default_root` (src/cli_sessions.h)

Returns the caller-owned platform default sessions directory.

## `oi_cli_sessions_root` (src/cli_sessions.h)

Resolves the sessions root a lifecycle operation should act on: a caller-owned copy of `root_override` when it is non-NULL, else the platform default. OI_ERR_INVAL for an empty override.

## `oi_cli_session_history_path` (src/cli_sessions.h)

Builds the caller-owned path of the authoritative history log inside a private session directory. Keeps that file name a single fact owned by this module rather than a string every caller repeats.

## `oi_cli_session_id_is_safe` (src/cli_sessions.h)

Syntax-only check -- never touches the filesystem: is `id` usable as a single path component directly under the sessions root?

Accepts 1..OI_CLI_SESSION_SAFE_ID_MAX_LEN bytes drawn only from [A-Za-z0-9_-]. Leaving '.' and '/' out of that charset is what structurally rejects "", ".", "..", every hidden entry (including the ".trash" subdirectory the lifecycle commands keep there), embedded NUL bytes, and every embedded path separator -- so no separate traversal check exists, or is wanted, anywhere above this function.

A leading '-' is accepted: session ids are never handed to an argv/getopt-style parser, so there is no option-injection reading for one to be confused with.

Returns 1 when safe, 0 otherwise (`id == NULL` included).

## `oi_cli_session_resolve` (src/cli_sessions.h)

Resolves a session id to its live directory under `root`, applying oi_cli_session_id_is_safe itself -- callers need not pre-validate.

The entry is checked with lstat(2), never stat(2), and must satisfy S_ISDIR on that result: a symlink at <root>/<id> therefore fails outright instead of being followed, which is how an attempt to escape the sessions root via a planted symlink is refused.

On OI_OK `*out_directory` is a caller-owned string; on every error it is left untouched.

OI_ERR_INVAL    bad arguments, unsafe id, or the entry exists but is not a plain directory (symlink, regular file, device)

OI_ERR_NOTFOUND nothing exists at that path

OI_ERR_IO       lstat(2) failed for any other reason, e.g. EACCES

Residual TOCTOU, accepted knowingly: a same-privilege local attacker could swap the directory between this check and a later open of a file inside it. Closing that would mean threading a dirfd through oi_sesslog_open and the metadata store -- oi_sesslog_open lives in the public header include/oi/sesslog.h, which CLI-only lifecycle policy must not change. Documented rather than half-closed.

## `oi_cli_session_metadata_path_for_log` (src/cli_sessions.h)

Derives a session's metadata path from its ".oilog" history path. `is_private_directory` selects which naming rule applies:

- true: the automatic/lazy private-directory layout, where each session has its own directory -- the literal "metadata.json" is unambiguous there (docs/REPL_PLAN.md's storage model).

- false: the flat --session-dir legacy layout, where one directory can hold multiple "<id>.oilog" files -- derives "<id>.metadata.json" instead. OI_ERR_INVAL if `log_path` doesn't end in ".oilog".

## `oi_cli_session_lock_state` (src/cli_sessions.h)

Whether another process currently holds a session log's flock. Probed read-only (see oi_cli_sessions_enumerate) -- a busy session is a normal selectable-but-not-openable state, never an error.

## `oi_cli_session_list_entry` (src/cli_sessions.h)

One selectable session, as shown by /session list.

These fields are presentational, deliberately kept separate from struct oi_cli_session_metadata: that struct's validity invariant requires a non-empty model and cwd, which a session whose metadata is missing and whose history cannot be replayed genuinely does not have. Such a session must still be listed (so the user can trash it), so every field here is individually optional -- `model`/`cwd` may be empty (data == NULL) and the timestamps may be 0, meaning "unknown". Only `id` is always present.

## `oi_cli_session_list_entry.display_name` (src/cli_sessions.h)

User-chosen label; empty means unset, so display the id instead. Never recoverable from history -- it lives only in the cache -- so a degraded entry always reports it empty.

## `oi_cli_sessions_enumerate` (src/cli_sessions.h)

Enumerates every live session directly under the sessions root (`root_override`, or the platform default when NULL), most recently updated first. `out_list` must be initialized, and is replaced only on success.

Entries in the root that are not sessions -- anything failing oi_cli_session_id_is_safe (dotfiles and the ".trash" subdirectory included) and anything that is not a plain directory -- are skipped silently: they are not oi's data, so they are not the user's problem.

Cost is bounded per entry and never replays a healthy session's log: the fast path is one flock probe plus one bounded metadata.json read, and it never opens the log, so listing does not write to it.

A session whose metadata.json is missing or malformed is marked `degraded` and rebuilt by replaying that one session's history -- so the worst case is one replay per damaged session, never a replay of every log. Note that this path does open the log, and so performs oi_sesslog_open's documented crash recovery on it (truncating an incomplete trailing record); that is the same repair a real open would do, and only damaged sessions reach it. A degraded session that is also busy cannot be replayed at all (another process holds the lock) and is listed with whatever is known, which may be nothing beyond its id.

A missing root is not an error: it means no sessions exist yet, and yields an empty list.

## `oi_cli_session_rename` (src/cli_sessions.h)

Sets a session's display name, leaving its directory and history untouched.

The safe directory id is permanent by design. Every path in the storage model is derived from it -- history.oilog, metadata.json, the registry entry of a live session, and anything outside oi pointing at the directory -- so renaming the directory to change a cosmetic label would mean updating all of them atomically. A name is a label; it is not worth that.

Only live sessions can be renamed; a trashed one is not addressable here (restore it first). Rebuilds a missing or malformed metadata cache from history before writing, so naming a damaged session repairs it rather than failing.

Takes no session lock -- it writes only metadata.json, which is safe to write while another process appends to the log. It does take the cache's own advisory lock across its read-merge-write, so a concurrent model/cwd refresh cannot lose the name and this cannot lose the model. If that lock is unavailable the rename fails rather than proceeding: unlike an optional cache refresh, the metadata write is the whole operation here.

`*out_error_detail`, when non-NULL on return, is a caller-owned string naming the specific cause and is set only on failure.

OI_ERR_INVAL    unsafe id, or a name that is empty, too long, or contains a control byte

OI_ERR_NOTFOUND no live session with that id

OI_ERR_IO       the metadata write failed, the cache lock could not be acquired, or the session has neither a cache nor a readable history to rebuild one from, so there is nowhere to record a name

## `oi_cli_session_trash` (src/cli_sessions.h)

Trash, restore, and permanent delete.

Trashing renames a session's whole private directory from <root>/<id> to <root>/.trash/<id>. A directory rename was chosen over a "trashed" flag in the cache for three reasons: rename(2) within a filesystem is atomic, so no partial state exists; a trashed session simply is not in <root> anymore, which keeps oi_cli_sessions_enumerate correct with no filtering (".trash" already fails oi_cli_session_id_is_safe); and the trash lives under the same root as every session, so the rename is same-filesystem by construction.

Cross-device is therefore not expected, but is detected and reported distinctly rather than folded into a generic I/O error -- a relocated or bind-mounted .trash would otherwise fail inexplicably.

Delete only ever operates on an already-trashed session; there is no one-step hard delete of a live one. That makes "refuses the current session" and "refuses a session open elsewhere" structural rather than merely checked: a live session is not in .trash, so delete cannot reach it at all.

`current_session_id` may be NULL when no session is active. All three set `*out_error_detail` (caller-owned) on failure when it is non-NULL.

OI_ERR_INVAL    unsafe id, or trashing the active session

OI_ERR_NOTFOUND no such session in the relevant location

OI_ERR_EXISTS   held by another process, or (restore) the id is live

OI_ERR_IO       the rename or removal failed, whether the session is in use could not be determined, or (delete) the directory holds something oi did not put there; detail says which

## `oi_cli_sessions_enumerate_trash` (src/cli_sessions.h)

Enumerates trashed sessions, so /session restore can offer a choice. Same contract and cost model as oi_cli_sessions_enumerate; an absent trash directory yields an empty list rather than an error.

## `oi_cli_session_import` (src/cli_sessions.h)

Imports an existing ".oilog" file -- typically a legacy project-local one -- as a brand-new private session, and reports its id.

The source is never opened for writing, never renamed, and never unlinked: import copies out of it and leaves it exactly as it was. That is structural here, not a convention to be careful about.

The source must be a plain regular file. A symlink is refused (checked via lstat, so it is never followed), as is a file already inside the sessions root -- that is an oi-managed session already, and /session switch is the operation the user wanted.

The copy is validated before it is adopted, by opening it as a session log and replaying it with the very code a real open would use. Only then is a session directory created and the validated copy moved into place. A file that fails validation leaves nothing behind.

No transition or settings record is appended here. That already happens for any session the first time it is actually opened, so import stays a pure validate-and-relocate step with no duplicated durable-write logic.

On OI_OK, `*out_new_id` is a caller-owned id string. On failure it is untouched and `*out_error_detail` (when non-NULL) names the cause.

## `oi_cli_session_model_origin` (src/cli_sessions.h)

Where an effective model name came from.

Only oi_cli_session_restore_settings can attribute the four open-time branches: the resolved name alone cannot be attributed after the fact, since a restored model and an explicit override can be the same string. COMMAND is the one value this module never produces -- it belongs to a later /model, whose caller replaces both the name and this alongside it. The vocabulary lives here anyway, so provenance has one type rather than one per consumer.

UNKNOWN is the zero value on purpose: a zeroed or partially-filled struct then claims nothing rather than claiming "default".

## `oi_cli_session_restore_settings` (src/cli_sessions.h)

Resolves and applies the effective model/cwd for a session at open time, chdir()ing the process, and durably recording any change (a brand-new session's first-ever values, an explicit_model override, or a cwd fallback correction) through `store`/`state` before refreshing `metadata_path`.

`is_new_session` must be true for every automatic session (each process run always creates a fresh directory) and, for an explicit --session ID, only when the load found zero pre-existing records, checked before this run appended anything (including its own transition record).

`explicit_model` (may be NULL) always wins. Otherwise replayed history wins: `state`'s last-known model/cwd; then, only if history records no such setting at all, valid metadata whose session_id matches `session_id`; then `default_model`/`default_cwd`.

History outranking the cache is load-bearing. The cache is refreshed best-effort *after* a change is already durable in history, so a failed refresh leaves it stale. Were the cache consulted first, that one failed write would make the next open resolve the stale value and then append it back into the authoritative log -- an unwritable cache would silently rewrite history. The cache's only unique contribution is `created_at`.

A missing/unreachable resolved cwd falls back to `default_cwd` (the caller should pass the process's actual current directory, captured before any chdir could have happened) with a diagnostic on `diagnostics` (ignored if NULL), never leaving cwd partially changed.

Returns a real error only for a structural/storage failure (the authoritative history append itself failing) -- callers should route that through the existing session-failure path. A metadata.json write failure alone is logged to `diagnostics` and treated as non-fatal: the change is already durable in history by that point, and metadata self-heals on the next open.

## `oi_cli_session_apply_setting` (src/cli_sessions.h)

Live single-setting change, reused by /model and /cwd. Appends the durable record, refreshes the replay state, then best-effort refreshes metadata.json (a metadata write failure here is not reported to the caller -- the change is already durable in history).

## `seed_session` (test/test_cli_sessions.c)

Builds a session directory the way a real run leaves one behind: a history.oilog carrying a transition plus durable model/cwd settings, and (unless `write_metadata` is 0) a matching metadata.json cache.

A session with neither a cache nor a readable log has no model or cwd to write alongside a name, and filling those in with invented values would corrupt what a later open trusts. Refuse instead.

## `snprintf` (test/test_cli_sessions.c)

A perfectly valid cache that names a *different* session -- a stray copy, an interrupted manual move, a restored backup. It parses, so without an ownership check this session would be listed as healthy while displaying another session's model, cwd, and name.

Both updaters read metadata.json, merge one field, and replace the file. Interleaved, each discards the other's field -- a rename that read before a concurrent /model landed writes the stale model back.

The window is far too narrow to hit reliably by hammering, so this tests the property that closes it instead: a rename cannot read until whoever is mid-update has finished. The test holds the metadata lock itself -- reaching into an implementation detail deliberately, because mutual exclusion is exactly what is being asserted.

The lock path is the one path in this module built by appending to a name rather than resolved through oi_cli_session_resolve, so it needs its own symlink defence: followed, a planted "metadata.json.lock" would be opened O_RDWR|O_CREAT at whatever it pointed to.

The trash normally lives inside the sessions root, making every trash/restore a same-filesystem rename. A user who relocates it -- here, a symlink onto another filesystem -- deserves a specific message rather than a bare I/O error.

Skipped where /dev/shm is not a separate filesystem, since EXDEV then genuinely cannot be provoked.

A scratch file left behind by a killed earlier run. Since the copy is created O_EXCL to avoid ever overwriting anything, a fixed name would let this block every future import from a process that reused the same pid.

## `enumerate_does_not_rewrite_the_logs_it_lists` (test/test_cli_sessions.c)

The case that actually distinguishes a read-only flock probe from opening the log through oi_sesslog_open: a record left incomplete by a crash. oi_sesslog_open recovers such a log by truncating that trailing fragment, which is right when a session is being opened for use and wrong as a side effect of listing. With the cache intact there is no reason to touch the log at all, so the fragment must survive being listed.

## `a_rename_waits_for_a_settings_update_and_merges_its_value` (test/test_cli_sessions.c)

Both fields survive. The model assertion is the anti-lost-update one: the rename read after acquiring the lock, so it saw "gpt-committed" and carried it forward. Reading before locking would have merged the pre-update model and silently reverted the change.

## `a_planted_lock_symlink_is_refused_and_never_followed` (test/test_cli_sessions.c)

A settings change is the opposite case: history is authoritative and already durable, so the optional cache refresh is skipped rather than raced, and the change itself still succeeds.

## `delete_only_reaches_trashed_sessions_and_removes_them_completely` (test/test_cli_sessions.c)

A live session is not in the trash, so delete cannot find it -- that is what makes "refuses the current session" structural rather than a check that could be forgotten.

## `write_legacy_log` (test/test_cli_sessions.c)

Writes a legacy-format log: raw alternating user/assistant payloads with no transition record, exactly the shape test_cli_history_store.c uses for its own legacy-replay coverage.

## `fresh_store` (test/test_cli_sessions.c)

Opens a brand-new log, appends its schema transition, and returns everything needed to call oi_cli_session_restore_settings/ oi_cli_session_apply_setting against it.

## `fresh_session_records_initial_model_and_cwd` (test/test_cli_sessions.c)

Nothing to restore from and no override: the fallback branch won, and says so. /status reports this provenance verbatim, so the resolution branch itself is what has to be asserted -- the resolved name alone cannot distinguish a default from an override of the same value.

## `unchanged_resume_writes_no_new_records_but_refreshes_metadata` (test/test_cli_sessions.c)

The same name as the default, but reached through the replayed setting record the first call wrote -- which is a different provenance, and the one a resume must report.

## `a_display_name_survives_setting_changes_and_reopens` (test/test_cli_sessions.c)

The name lives only in metadata.json, and both of the paths that refresh that cache rebuild it from scratch -- so each has to carry the name forward or a plain /model change would silently discard it.

## `a_stale_metadata_cache_never_overrides_or_rewrites_history` (test/test_cli_sessions.c)

Simulate the metadata refresh having failed: the cache still names the old model, and is otherwise perfectly valid and owned by this session, so it would pass every validity check.

The cache must not win. If it did, this open would resolve "gpt-old", notice it differs from what history last recorded, and append that stale value back -- letting an unwritable cache silently rewrite the authoritative log.

## `fresh_store_open` (test/test_cli_sessions.c)

A session whose history has a transition but no setting records -- the cache is then the only place a prior model could survive, so it must still be used as a fallback rather than ignored outright.
