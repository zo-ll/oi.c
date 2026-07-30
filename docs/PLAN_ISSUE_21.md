# Implementation Plan: GitHub Issue #21 — `/session` lifecycle and legacy import

Status: **not started** (research + planning only; no implementation code written yet). Written on branch `issue-21-session-lifecycle`, off `main` at commit `21c61f6` (issue #27, `/compact`, just closed). See `docs/HANDOFF_ISSUE_21.md` in this same branch for how to pick this up.

**This plan is fully self-contained.** It assumes zero prior conversation context. Read this document top to bottom before writing any code.

**Verification note:** the file:line references throughout were checked against `main` at the commit above via targeted `grep`, and the ones spot-checked (`have_parsed_command:`, `argument_equals`, `print_deferred`, the `OI_CLI_COMMAND_SESSION` stub case, the lazy-conversation-create block, `oi_cli_conversation_destroy`, `is_space`) all matched exactly or within a line or two. They were **not** exhaustively re-verified line-by-line beyond that spot check — re-`grep` anything you're about to edit before trusting its cited line number, standard practice for any plan document that outlives the session that wrote it.

## 0. Context restated

`oi` is a C11 CLI chat agent (single-threaded epoll reactor, durable append-only per-session history log). A prior series of issues (#20–#27) built: private timestamped session directories, atomic `metadata.json` selector cache, model/cwd restore-on-open, the interactive REPL with a one-slot command queue, `/permissions`, `/model`, `/cwd`, `/compact`, and a live tool panel. `/session` is registered in the command table but **stubbed** — it always prints "not implemented" (`src/cli_command_dispatch.c:239-240`).

This issue implements the full `/session` grammar: `list`, `current`, `switch`, `rename`, `trash`, `restore`, `delete`, `import`. Repository convention for this series: **EnterPlanMode-style small commits**, each independently buildable and verified with `make check`; sanitizers (ASan/UBSan) and valgrind at flagged high-risk commits; full verification + GitHub issue closeout at the end. Follow that convention here.

**Read `docs/REPL_PLAN.md` first** (already in the repo) — it documents the overall REPL contract, storage model, and module boundaries this issue must respect. Update its `## Commands` section (currently lines 44-63) to spell out the full `/session` grammar as part of this issue's final documentation commit.

### Module boundaries (mandatory, from the issue)
- `cli_sessions.c`/`.h`: discovery, safe IDs, directories, metadata cache, import, rename, trash, restore, delete. May depend on `cli_history_store`/`cli_history_replay` (it already does, for `oi_cli_session_restore_settings`).
- `cli_repl.c`: idle/safe-boundary orchestration, swapping the active conversation/session, all composer/confirmation UI.
- `cli_history_store`/repair modules: replay/repair only, never directory enumeration.
- `cli_command_dispatch.c`: grammar validation and user-facing text only, filesystem work via callbacks.
- **No public library ABI change.** Everything here is CLI-private (`src/`, not `include/oi/`). In particular, do **not** change `include/oi/sesslog.h`'s `oi_sesslog_open` signature — this constrains the symlink-TOCTOU design below.

---

## 1. Safe-ID validation — exact rule

New function in `src/cli_sessions.c`, declared in `src/cli_sessions.h`:

```c
/* Syntax-only check: does NOT touch the filesystem. */
int oi_cli_session_id_is_safe(const char *id, size_t id_len);
```

Rules (decisive, no menu):
- Length: `1 <= id_len <= 128` (new constant `OI_CLI_SESSION_SAFE_ID_MAX_LEN`, independent of the existing internal `OI_CLI_SESSION_ID_CAP 64U` buffer in `cli_sessions.c:14`, which is only for generating new IDs).
- Charset: **only** `[A-Za-z0-9_-]`. No `.` at all. This single rule structurally rejects `.`, `..`, hidden/dotfiles, and (combined with rejecting `/`) any path separator — no special-casing needed. It also means the `.trash/` convention (section 4) can never collide with a real ID during enumeration, because `.trash` itself fails this same validator.
- Reject empty string.
- No leading-dash special case needed beyond the charset rule itself (a leading `-` is syntactically legal under this charset today, but since IDs are never fed to any argv/getopt-style parser, this is safe; note it in the code comment so a future reviewer doesn't need to re-derive it).

**Symlink/type check happens at USE time, not at validation time** — right before the directory is actually opened, in a second function:

```c
/* Filesystem check: id must already pass oi_cli_session_id_is_safe.
 * Builds root_join(root, id), lstat()s it, and returns:
 *  - OI_OK with *out_directory set, if it's a real directory (not a symlink).
 *  - OI_ERR_NOTFOUND if nothing exists there.
 *  - OI_ERR_INVAL if it exists but isn't a plain directory (symlink,
 *    regular file, device, etc. -- covers symlink-escape attempts).
 */
oi_status oi_cli_session_resolve(const char *root, const char *id,
                                  size_t id_len, char **out_directory);
```

Implementation: `lstat()` (not `stat()`) on the joined path; require `S_ISDIR(st.st_mode)` on the **lstat** result itself (lstat never follows the final symlink, so a symlinked directory entry fails this check outright, which is exactly "reject symlink escapes").

Residual TOCTOU: strictly, `openat(..., O_NOFOLLOW|O_DIRECTORY)` immediately followed by relative opens would close the tiny race between this `lstat` and the subsequent `oi_sesslog_open`/`fopen` calls. **Decision: do not do this.** `oi_sesslog_open`, `oi_cli_session_metadata_store_read/write` all take plain path strings (public-ish CLI-internal API, but changing them to take a dirfd would ripple into every existing call site built across issues #20–#27, and `oi_sesslog_open` itself lives in a header (`include/oi/sesslog.h`) that must not change). Document this explicitly as an accepted same-privilege-local-attacker residual risk; do not attempt to close it in this issue.

This same `oi_cli_session_resolve` is reused for `switch`/`rename`/`trash`/`delete` target resolution against the live root, and — with a second variant/parameter for the trash root — for `restore`.

Add `oi_cli_session_id_is_safe` argument-order symmetry with existing `oi_cli_session_metadata_path_for_log` style (status-returning, `out_*` param last).

**Commit 1** (see section 8): add these two functions + unit tests only. No other files touch this yet.

---

## 2. Enumeration (`/session list`)

New function in `cli_sessions.c`/`.h`:

```c
struct oi_cli_session_list_entry {
    char *id;                                  /* owned */
    struct oi_cli_session_metadata metadata;    /* owned; may be "empty" if degraded */
    int degraded;         /* metadata.json missing/corrupt; rebuilt from replay */
    enum oi_cli_session_lock_state {
        OI_CLI_SESSION_LOCK_FREE = 0,
        OI_CLI_SESSION_LOCK_BUSY,
        OI_CLI_SESSION_LOCK_UNKNOWN  /* probe itself failed for another reason */
    } lock_state;
};
struct oi_cli_session_list { struct oi_cli_session_list_entry *entries; size_t len; };

void oi_cli_session_list_init/free(struct oi_cli_session_list *);
oi_status oi_cli_sessions_enumerate(const char *root_override,
                                     struct oi_cli_session_list *out_list);
```

**Scanning approach:** `opendir()`/`readdir()` over the sessions root (from `oi_cli_sessions_default_root` or `root_override`, same resolution `oi_cli_session_location_create` already uses at `cli_sessions.c:173-184`). For each `d_name`:
1. Skip `"."`, `".."` (readdir gives these) and skip `".trash"` — but note per section 1, `.trash` and any dotfile is **already** rejected by `oi_cli_session_id_is_safe` since it starts with `.`, which isn't in the allowed charset. So the loop is just: `if (!oi_cli_session_id_is_safe(d_name, strlen(d_name))) continue;` (silent skip — these are never our own data, not worth alarming the user about).
2. `lstat` the joined path; `if (!S_ISDIR) continue;` (silent skip — same reasoning, covers stray files/symlinks placed in the sessions root).
3. Read `metadata.json` via the **existing** `oi_cli_session_metadata_store_read` (`cli_session_metadata_store.c:27-77`) — already bounded at 64 KiB, already does no `.oilog` replay. This is the "bounded metadata without replay" path from the acceptance criteria, reused unmodified.
4. **On success:** entry not degraded, use the read metadata as-is.
5. **On `OI_ERR_NOTFOUND`/`OI_ERR_PARSE`:** entry **is still listed**, not skipped — mark `degraded = 1`, and rebuild by opening `<dir>/history.oilog` with `oi_sesslog_open` + `oi_cli_history_store_load` (**only this one session's log**, never every log — satisfies "bounded... without replaying every log" literally: the bound is per-entry, not zero-replay). Populate `metadata.model`/`.cwd`/`.created_at` from `replay_state.last_model`/`.last_cwd` and file `stat()` mtime as a created_at fallback. `oi_sesslog_close` immediately after — this is a read, and per point 6 below this happens to double as the lock probe.
6. **Busy/lock detection:** reuse `oi_sesslog_open`'s own flock behavior as the probe, no new locking code. For an entry not already opened in step 5 (i.e., the non-degraded/fast path), do a lightweight probe: `oi_sesslog_open(history_path, &log)`. `OI_ERR_EXISTS` → `lock_state = BUSY`, and metadata already known from step 3/4, so this is cheap. `OI_OK` → `lock_state = FREE`, then `oi_sesslog_close(log)` **immediately** (non-destructive: opens, sees no lock held, releases). Any other status → `UNKNOWN`, still list the entry (it already has whatever metadata step 3/4 gave it).
   - Note: `oi_sesslog_open` creates a fresh header only for an empty/nonexistent file (`sesslog.c` doc comment) — every listed session directory has an existing non-empty `history.oilog` by construction (created via `oi_cli_session_location_create`), so this probe never has a create side effect in practice; still, this is a pre-existing, already-tested code path, nothing new to trust.
7. Sort entries by `metadata.updated_at` descending (most-recently-active first) before returning — small, cheap, in `oi_cli_sessions_enumerate` itself with a plain insertion sort or `qsort`.

**Commit 2**: `oi_cli_sessions_enumerate` + unit tests (skip-invalid-entry, degraded-rebuild, busy-detection via a real second process holding the flock — fork a helper subprocess in the test that opens and sleeps, matching the `start_silent_server`-style helper pattern already used in `test/integration/test_cli_compact.c:33-80`, or simpler: fork, child opens+flocks+pauses on a pipe read, parent enumerates, parent writes to the pipe to release the child, `waitpid`).

---

## 3. `/session current` and status text

Add one new field to `struct oi_cli_command_context` (`src/cli_command_dispatch.h:29-39`):

```c
const char *(*session_metadata_path)(void *user_data); /* NULL if unavailable */
void *session_metadata_path_user_data;
```

Wired in `cli_repl.c`'s `command_context` construction (`cli_repl.c:1226-1240`) from `cli.c`'s already-existing `persistence.metadata_path` (set at `cli.c:263`/`933`) via a tiny new accessor mirroring `current_session_id` (`cli.c:270-273`).

`/session current` (and bare `/session`/`/session list`, section 7) live in `cli_command_dispatch.c`'s new `dispatch_session` — **not** intercepted early in `cli_repl.c`, since they need no conversation/composer access.

Text:
```
Session: <id>
Path: <directory or explicit .oilog path>
Status: healthy
```
or
```
Status: degraded (metadata rebuilt from history)
```

"healthy" vs "degraded" is determined **live, on every invocation** (not a cached flag from startup): call `oi_cli_session_metadata_store_read(metadata_path, &meta)`; healthy iff it returns `OI_OK` **and** `meta.session_id` matches `context->session_id`. This is a single bounded read (same primitive as section 2), always accurate, and needs no new persistent state threaded through the REPL loop — deliberately not reusing `oi_cli_session_restore::metadata_missing_or_corrupt` from startup, since that's a one-time startup fact and metadata could be deleted/corrupted by something external later in a long-running session.

If `context->session_id == NULL` (ephemeral, no durable session — e.g. `--session` not given and not interactive-automatic, shouldn't actually happen for `/session` since it's only reachable from the interactive REPL, but defend anyway): print `Session: (not created)` and skip status.

---

## 4. Trash/restore/delete mechanics

**Decision: directory-rename convention**, not a metadata flag. A `.trash/` subdirectory under the sessions root (`<root>/.trash/<id>/`). Trashing a session = `rename(2)` its whole private directory from `<root>/<id>` to `<root>/.trash/<id>`. Restoring = `rename(2)` back. This is chosen over a `trashed_at` metadata flag because:
- It keeps `/session list`'s enumeration loop (section 2) trivially correct with **zero extra filtering logic**: a trashed directory is simply not in `<root>` anymore, so the existing readdir loop over `<root>` never sees it, and `.trash` itself is already excluded by the safe-ID charset rule (section 1) — no special-case code path in the hot enumeration loop.
- It's failure-atomic within a filesystem: `rename(2)` on the same filesystem is atomic; partial failure state is impossible (either the whole directory moved or it didn't).
- `.trash/` lives **inside the same sessions root** as every private session directory (both created by the identical `ensure_directory`/`mkdir` logic already in `cli_sessions.c:49-88`), so trash/restore renames are always same-filesystem in the automatic-session (private-directory) layout this issue targets — cross-device `EXDEV` is structurally not expected in the common case, but must still be **detected and reported explicitly** (defensive: e.g. a sessions root that itself is a bind-mount boundary, or a user manually relocating `.trash`). On `rename() == -1 && errno == EXDEV`, report a specific message: `"oi: cannot trash session '<id>': trash directory is on a different filesystem"` rather than a generic I/O error.

New functions in `cli_sessions.c`/`.h`:
```c
oi_status oi_cli_session_trash(const char *root_override, const char *id,
                                size_t id_len, const char *current_session_id,
                                char **out_error_detail);
oi_status oi_cli_session_restore(const char *root_override, const char *id,
                                  size_t id_len, char **out_error_detail);
oi_status oi_cli_session_delete(const char *root_override, const char *id,
                                 size_t id_len, char **out_error_detail);
```

**Trash** (`oi_cli_session_trash`):
1. `oi_cli_session_id_is_safe` → else `OI_ERR_INVAL` ("invalid session id").
2. Refuse **current**: `if (current_session_id != NULL && strcmp(id, current_session_id) == 0) return OI_ERR_INVAL` with detail "cannot trash the active session — switch away first". (`current_session_id` is passed in by the dispatch callback from `context->session_id`; this is the "refuse ambiguous/current" requirement — comparing by exact string match, no ambiguity possible since IDs are unique.)
3. Resolve live directory via `oi_cli_session_resolve(root, id, ...)` → `OI_ERR_NOTFOUND` if it doesn't exist as a live session.
4. **Busy check** (non-destructive probe, exactly as section 2's enumeration): `oi_sesslog_open(history_path, &log)`; `OI_ERR_EXISTS` → refuse with detail "session is open in another process"; else `oi_sesslog_close(log)` immediately and proceed.
5. `ensure_directory(<root>/.trash)` (reuse existing static `ensure_directory` from `cli_sessions.c:49-88` — will need to stop being `static` or get a small non-static wrapper exposed internally; simplest: keep it `static` in `cli_sessions.c` and just call it directly since trash logic lives in the same file).
6. `rename(<root>/<id>, <root>/.trash/<id>)`. On `EXDEV`, return a distinct status (reuse `OI_ERR_IO` but populate `*out_error_detail` with the exact cross-device message per above). On any other rename failure, `OI_ERR_IO` with `strerror(errno)` in the detail.

**Restore** (`oi_cli_session_restore`): symmetric — resolve under `<root>/.trash/<id>` (new resolve call with `.trash`-joined root), refuse `OI_ERR_NOTFOUND` if absent, refuse if a live directory of the same ID **already exists** at `<root>/<id>` (shouldn't happen given globally-unique generated IDs, but check to avoid silently clobbering — return `OI_ERR_EXISTS` with detail "a live session with this id already exists"), lock-probe defensively the same way (a trashed session should never be lockable by another process, but check anyway — same code, no special-casing), then `rename(<root>/.trash/<id>, <root>/<id>)`.

**Delete** (`oi_cli_session_delete`) — **only ever operates on an already-trashed session** (this is the decisive design: there is no direct hard-delete of a live session in one step; you must `/session trash` first). This makes "refuses current" and "refuses locked-by-another-process-while-active" **structural**, not just checked: a session that is current is by definition live, not in `.trash`, so it can never even be found by this function's `.trash`-scoped resolve, and a live/busy session likewise can't be found there either.
1. Resolve under `<root>/.trash/<id>`; `OI_ERR_NOTFOUND` if absent (this alone naturally covers "refuses current" and "refuses locked" without extra checks).
2. Defensive lock-probe anyway (same code as trash/restore) — refuse if somehow locked.
3. Recursively remove `<root>/.trash/<id>` (`nftw`/manual `opendir`+`unlink` each file + `rmdir`, since the directory contains `history.oilog` + `metadata.json` [+ possibly its own stray `.tmp` from a prior interrupted metadata write] — bounded, known file set, no need for a generic recursive-delete library routine: just enumerate the directory's actual entries and `unlink` each regular file, then `rmdir` the now-empty directory. If an unexpected entry type is found (e.g., another directory nested inside — shouldn't happen), fail closed with `OI_ERR_IO` rather than doing anything recursive/generic — deletion should never delete something it didn't put there itself).

**Confirmation flow for `/session delete`**: `cli_command_dispatch.c` has no composer/terminal access (by design, same limitation documented for `/permissions allow` at `cli_repl.c:987-994` and `1167-1225`). So `/session delete ID` is **fully intercepted in `cli_repl.c`'s `have_parsed_command:`**, exactly like `/compact` (`cli_repl.c:995-1166`) and the `/permissions allow` elevation gate (`cli_repl.c:1178-1225`) — never reaching `oi_cli_command_dispatch` at all for this specific subcommand. Sequence: parse `ID` out of `parsed.arguments`; if `config->delete_session == NULL` (a new callback, see below) print "not available"; else show a **confirm selector** via the existing `oi_cli_composer_select` primitive (same API `/permissions allow` already uses at `cli_repl.c:1197-1199`), with options `["Cancel", "Permanently delete session <ID> (cannot be undone)"]`; on cancel, print "oi: deletion cancelled" and continue; on confirm, call `config->delete_session(user_data, id, id_len, &error_detail)`; print result.

`/session trash` and `/session restore` do **not** need this confirm gate (trash is recoverable and low-risk; restore is always safe) — both go through ordinary `cli_command_dispatch.c` callbacks.

---

## 5. Rename semantics — pure metadata display name, directory ID fixed forever

**Decision** (per the issue's own warning): `/session rename ID NEW_NAME` **never** touches the directory or `history.oilog`. The safe directory ID is permanent. Justification: every existing path-derivation function (`oi_cli_session_metadata_path_for_log`, `oi_cli_session_location_create`'s `history_path`/`metadata_path`, and the new enumeration in section 2) derives paths from the directory ID; renaming the directory would require simultaneously updating every in-flight reference to it (the currently-open `oi_session`'s registered `id` in `oi_session_registry`, any external tooling pointing at the old path, backup scripts, etc.) for a purely cosmetic feature. Not worth the risk for a display label.

Add one field to `struct oi_cli_session_metadata` (`cli_session_metadata.h:24-32`):
```c
struct oi_cli_string display_name; /* empty (data==NULL) means "unset, show the ID" */
```
Bump `OI_CLI_SESSION_METADATA_SCHEMA_VERSION` from `1` to `2` (`cli_session_metadata.h:19`). Extend `oi_cli_session_metadata_is_valid` (`cli_session_metadata.c:21-44`) to allow `display_name.len == 0` (valid/unset) or a non-empty value bounded the same way as `model`/`cwd` (`<= OI_CLI_HISTORY_MAX_SETTING_VALUE`, reject embedded control bytes/newlines since it's echoed in `/session list` output — validate this rejection in the setter, not just the decoder). Extend `oi_cli_session_metadata_set` (`:46-79`) with a `display_name`/`display_name_len` parameter (every existing call site — `refresh_metadata` in `cli_sessions.c:307-332` — needs updating to pass through whatever the current name is, defaulting to empty for the two call sites that don't know a name, i.e. anything that isn't literally the rename path).

**Backward compatibility, decisive:** `oi_cli_session_metadata_decode` (`cli_session_metadata_codec.h:17-26`, implemented in `cli_session_metadata_codec.c` — not yet read in this research pass, **verify its exact shape during implementation**) must accept both:
- version `1` payloads with no `display_name` key → decode with `display_name` empty.
- version `2` payloads with an optional `display_name` key → decode as given.
`oi_cli_session_metadata_encode` always **writes version 2** going forward (write-side unconditionally upgrades). This is additive-only schema evolution, consistent with the project's self-healing-metadata philosophy (a metadata read failure of any kind already degrades gracefully to a history replay rebuild, so even a hypothetical decode regression here is non-fatal).

`oi_cli_session_rename` in `cli_sessions.c`:
```c
oi_status oi_cli_session_rename(const char *root_override, const char *id,
                                 size_t id_len, const char *new_name,
                                 size_t new_name_len, char **out_error_detail);
```
1. `oi_cli_session_id_is_safe(id, ...)` else `OI_ERR_INVAL`.
2. Validate `new_name`: bound (e.g. `<= 256` bytes — new constant), reject empty (empty name = "clear the custom name", allow this as a **distinct** explicit case: `/session rename ID ""`? No — simplest: require non-empty; there is no separate "clear name" subcommand in this issue's scope, out of scope for now, note as a follow-up), reject any byte `< 0x20` (control bytes/newlines, since it's printed directly in list output).
3. Resolve live directory via `oi_cli_session_resolve`. `OI_ERR_NOTFOUND` if absent (rename only targets live sessions — a trashed session isn't independently addressable for rename in this issue's scope; note as a deliberate simplification).
4. Read existing `metadata.json`; if missing/corrupt, **rebuild first** exactly like `oi_cli_session_restore_settings` already does on open (open `history.oilog` read-only via `oi_sesslog_open`, `oi_cli_history_store_load`, use `replay_state.last_model`/`.last_cwd`/created_at-fallback) — same bounded-per-session-replay pattern as section 2's degraded-listing path (worth factoring into one small shared static helper `rebuild_metadata_from_history(dir, id, out_metadata)` used by both `oi_cli_sessions_enumerate`'s degraded path and `oi_cli_session_rename`'s missing-metadata path).
5. Call `oi_cli_session_metadata_set(..., new_name, new_name_len, ...)`, write via existing `oi_cli_session_metadata_store_write` (atomic temp+rename, `cli_session_metadata_store.c:79-129`, unchanged).

No lock/busy check needed for rename — it only ever touches `metadata.json`, which is already safe to write concurrently with another process holding the `history.oilog` flock (existing `persist_model_setting`/`persist_cwd_setting` already write metadata.json without taking any session-level lock).

Rename is dispatched through ordinary `cli_command_dispatch.c` (new callback, no composer/confirm needed — a display-name change is low-risk and instantly reversible by renaming again).

---

## 6. The live session-switch mechanism (highest-risk commit)

### 6.1 Why today's `prepare` can't be reused

`config->prepare` (`oi_cli_repl_config.prepare`, `cli_repl.h:95-96`) is invoked **exactly once**, lazily, the first time `conversation == NULL` in the main loop (`cli_repl.c:1257-1276`), and its one implementation (`prepare_automatic_session`, `cli.c:194-268`) hard-asserts `*context->session != NULL` is **already** an error (`cli.c:206`) — it only ever creates a brand-new timestamped session, never re-opens an existing one, and has no path back to "and now tear down the previous one." `conversation`, `history_store`, `replay_state`, and the active `oi_sesslog` (owned inside `oi_session`) are otherwise **never** destroyed and recreated mid-process anywhere in this codebase today — `conversation` is destroyed exactly once at `cli_repl.c:1464`, at the very end of `oi_cli_repl_run`.

### 6.2 New callback shape

Add to `struct oi_cli_repl_config` (`cli_repl.h:83-125`):
```c
enum oi_cli_repl_switch_outcome {
    OI_CLI_REPL_SWITCH_OK = 0,
    OI_CLI_REPL_SWITCH_SAME,       /* target == current session id, no-op */
    OI_CLI_REPL_SWITCH_NOT_FOUND,
    OI_CLI_REPL_SWITCH_BUSY,       /* locked by another process */
    OI_CLI_REPL_SWITCH_INVALID,    /* bad id syntax */
    OI_CLI_REPL_SWITCH_CORRUPT     /* log exists but fails to load/repair */
};
struct oi_cli_repl_switch_result {
    enum oi_cli_repl_switch_outcome outcome;
    oi_arena *arena;                            /* valid only if outcome==OK */
    struct oi_cli_message_list initial_context; /* malloc-owned; caller (cli_repl.c) frees */
    struct oi_cli_string model;                 /* malloc-owned */
};
typedef oi_status (*oi_cli_repl_switch_session_cb)(
    void *user_data, const char *id, size_t id_len,
    struct oi_cli_repl_switch_result *out_result);
oi_cli_repl_switch_session_cb switch_session; /* NULL if unavailable */
void *switch_session_user_data;
```
Return-value convention, mirroring `oi_cli_compact_result`'s `outcome` field precedent (`cli_compact.h`, used at `cli_repl.c:995-1166`): the `oi_status` return is reserved for genuine structural failure (OOM only, in practice); every **business-logic** failure (not found, busy, invalid syntax, corrupt target, already-current) is `OI_OK` + `out_result->outcome` set, so a failed switch **never** breaks the REPL loop — it's handled exactly like `/compact`'s `OI_CLI_COMPACT_FAILED`/`CANCELLED` branches (`cli_repl.c:1096-1108`): print a message, `continue`.

**Hard precondition, documented on the callback declaration**: the caller (`cli_repl.c`) must have already destroyed the current `conversation` (set to `NULL`) **before** calling `switch_session` — the callback's implementation is allowed to destroy the *old* session's arena as part of a successful switch, and must never be called while anything still references that arena.

### 6.3 New testable module: `src/cli_session_switch.c` / `.h`

`cli.c`'s `main()` owns `persistence_context`/`automatic_session_context` as **static structs private to `cli.c`** (not in any header) — a separately-linkable, unit-testable module cannot reference them by type. Rather than restructure those (high blast radius across #20–#27's existing code), put the actual registry/log/replay orchestration in a new small CLI-internal module using only pre-existing public-to-CLI types, and let `cli.c`'s own (thin, still-static) `switch_session` callback wrap it and update its own private `persistence`/`history_store`/`replay_state` locals:

```c
/* src/cli_session_switch.h */
struct oi_cli_session_switch_result {
    enum oi_cli_repl_switch_outcome outcome;    /* reuse the same enum */
    oi_session *new_session;                    /* already created+registered; NULL unless OK */
    struct oi_cli_history_store store;          /* moved out; caller now owns it */
    struct oi_cli_history_replay_state state;   /* moved out; caller now owns it */
    struct oi_cli_message_list initial_context; /* malloc-owned clone, from state.context[] */
    struct oi_cli_string model;
    struct oi_cli_string cwd;
    char *metadata_path;                        /* malloc-owned */
};
void oi_cli_session_switch_result_free(struct oi_cli_session_switch_result *);

/* Never touches/destroys the OLD session -- purely additive to the registry
 * until outcome==OK, at which point *out_result->new_session is fully open,
 * loaded, repaired, and settings-restored, and it is the CALLER's job (in
 * cli.c) to then destroy the old session and adopt these fields. On any
 * non-OK outcome, this function has already rolled back anything it
 * registered (oi_session_destroy on its own partially-created entry), so
 * the registry is exactly as it was on entry either way. */
oi_status oi_cli_session_switch(
    oi_session_registry *registry, const char *root_override,
    const char *current_session_id, const char *target_id,
    size_t target_id_len, const char *default_cwd, FILE *diagnostics,
    struct oi_cli_session_switch_result *out_result);
```

Implementation of `oi_cli_session_switch` (mirrors the existing explicit-`--session` open path at `cli.c:842-935` almost exactly, but targeting a private-directory-layout session instead of a flat `--session-dir` one):
1. `if (target_id_len == current_session_id-length && memcmp(...) == 0) → outcome = SAME, return OI_OK` (cheap, no I/O).
2. `oi_cli_session_id_is_safe` → else `outcome = INVALID`.
3. `oi_cli_session_resolve(root, target_id, ...)` → `OI_ERR_NOTFOUND` → `outcome = NOT_FOUND`.
4. Build `history_path`/`metadata_path` the same way `oi_cli_session_location_create` does (reuse its private-directory naming convention — `<dir>/history.oilog`, `metadata.json` via `oi_cli_session_metadata_path_for_log(path, /*is_private_directory=*/1, ...)`).
5. `oi_session_create(registry, target_id, history_path, 0, &new_session)`. `OI_ERR_EXISTS` (another process holds the flock, per `sesslog.c:70-146`'s `flock(LOCK_EX|LOCK_NB)`) → `outcome = BUSY`, return `OI_OK` (registry untouched — `oi_session_create` never partially registers on failure, confirmed at `session.c:58-116`: the slot is only appended after every step succeeds).
6. `oi_cli_history_store_load(oi_session_log(new_session), &local_store, &local_state)`. On failure: `oi_session_destroy(registry, new_session)` (rolls back step 5 cleanly), `outcome = CORRUPT`, return `OI_OK`.
7. If `local_state.needs_transition`/`needs_repair`: append transition/repair records exactly as `cli.c:862-883` does today, into `local_store`/`local_state`. On failure at this step: same rollback as step 6, `outcome = CORRUPT`.
8. Clone `local_state.context[]` into a malloc-owned `struct oi_cli_message_list` (same loop as `cli.c:894-902`). On OOM: rollback, propagate a **real** `oi_status` (this is the one genuine structural-failure path — OOM, not business logic).
9. `oi_cli_session_restore_settings(&local_store, &local_state, metadata_path, target_id, /*is_new_session=*/ (local_store.typed_history.len == 0 captured before step 7's own appends, same convention as `cli.c:861`), /*explicit_model=*/NULL, default_model /* pass process's configured default in, or pass NULL and have this function fall through to state's own last_model like the existing function already does */, default_cwd, diagnostics, &restore)`. On failure: rollback, `outcome = CORRUPT` (a durable-append failure while restoring settings on an otherwise-loadable target — treat as corrupt/unusable rather than a hard REPL-ending error, since the OLD session must remain usable regardless).
10. Success: populate `out_result`: `new_session`, move `local_store`/`local_state` into `out_result->store`/`->state` (plain struct assignment — confirmed safe, see 6.4), `initial_context` (from step 8), `model`/`cwd` (from `restore`), `metadata_path` (owned copy), `outcome = OK`. Return `OI_OK`.

### 6.4 Struct-move safety (verified)

`struct oi_cli_history_store` (`cli_history_store.h:10-14`) and `struct oi_cli_history_replay_state` (`cli_history_replay.h:21-45`) are both plain-old-data with only owned heap pointers (`legacy_messages`, `typed_history`, `context`, various `oi_cli_string`s) and one **borrowed** pointer (`store->log`, not owned) — no self-referential pointers, no pointer stored elsewhere pointing back into these structs. A byte-copy struct assignment (`out->store = local_store;`) followed by re-`init`-ing (never freeing) the source local is a safe "move." This is the same idiom `oi_cli_history_store_load` itself already uses internally at `cli_history_store.c:122-129`.

### 6.5 `cli.c`'s thin wrapper callback

```c
struct switch_context { /* holds pointers to main()'s locals */
    oi_session_registry *registry;
    oi_session **session;                 /* &session in main() */
    const char *root_override;
    const char *default_cwd;
    struct oi_cli_history_store *store;   /* &history_store in main() */
    struct oi_cli_history_replay_state *state;
    struct persistence_context *persistence;
    char **active_metadata_path;          /* NEW: &explicit_metadata_path-equivalent,
                                            * freed+replaced on every switch, freed
                                            * once more at cleanup: */
};
static oi_status switch_session_cb(void *user_data, const char *id,
                                    size_t id_len,
                                    struct oi_cli_repl_switch_result *out) {
    struct switch_context *ctx = user_data;
    struct oi_cli_session_switch_result result;
    oi_status status = oi_cli_session_switch(
        ctx->registry, ctx->root_override, oi_session_id(*ctx->session),
        id, id_len, ctx->default_cwd, stderr, &result);
    out->outcome = (enum oi_cli_repl_switch_outcome)result.outcome; /* same enum values */
    if (status != OI_OK || result.outcome != OI_CLI_SESSION_SWITCH_OK) {
        oi_cli_session_switch_result_free(&result);
        return status;
    }
    /* Commit point: caller (cli_repl.c) has ALREADY destroyed the old
     * conversation before invoking this whole path -- safe to destroy the
     * old session's arena now. */
    oi_session_destroy(ctx->registry, *ctx->session);
    oi_cli_history_store_free(ctx->store);
    oi_cli_history_replay_state_free(ctx->state);
    *ctx->store = result.store;                 /* move */
    *ctx->state = result.state;                 /* move */
    *ctx->session = result.new_session;
    free(*ctx->active_metadata_path);
    *ctx->active_metadata_path = result.metadata_path; /* transfer ownership */
    result.metadata_path = NULL;                /* don't let _free below touch it */
    ctx->persistence->store = ctx->store;
    ctx->persistence->state = ctx->state;
    ctx->persistence->turn_id = ctx->state->next_turn_id;
    ctx->persistence->metadata_path = *ctx->active_metadata_path;
    ctx->persistence->session_id = oi_session_id(*ctx->session);
    ctx->persistence->last_error = OI_OK;
    status = oi_cli_string_set(&ctx->persistence->model, result.model.data, result.model.len);
    out->arena = oi_session_arena(*ctx->session);
    out->initial_context = result.initial_context;  /* transfer */
    result.initial_context = (struct oi_cli_message_list){0}; /* prevent double free */
    out->model = result.model;
    result.model = (struct oi_cli_string){0};
    /* result.cwd/state/store already consumed/moved; free() is then a no-op
     * for those fields. Verify oi_cli_session_switch_result_free()'s exact
     * field ownership handling matches this transfer pattern during
     * implementation -- write it to tolerate double-init cleanly. */
    oi_cli_session_switch_result_free(&result);
    return status;
}
```
(The exact ownership-transfer bookkeeping above is intentionally spelled out at this level of detail because it is the single highest-risk piece of this whole issue — get the struct-free double-free/leak semantics right, and cover them with ASan.)

### 6.6 `cli_repl.c`'s orchestration at `have_parsed_command:`

New loop-scoped variable in `oi_cli_repl_run` (alongside existing `conversation`, `current_model`): `oi_arena *active_arena = arena;` (initialized to the function's `arena` parameter, updated every time a conversation is (re)created — both in the existing lazy-`config->prepare` path at `cli_repl.c:1257-1276`, where the local `conversation_arena` becomes this loop-scoped variable instead of a block-local one, and in the new switch path).

New branch in `have_parsed_command:`, alongside the existing `/compact` interception (`cli_repl.c:995` onward), keyed on `parsed.command->id == OI_CLI_COMMAND_SESSION` **and** first-token == `"switch"` (use the same subcommand-tokenizer described in section 7 — factor it into a small shared static helper usable both here and in `cli_command_dispatch.c`, or simplest: duplicate the ~10-line first-whitespace-token split locally in `cli_repl.c` too, since it's trivial and these are different translation units):

1. Parse `ID` (second token). Usage error if missing/extra tokens → print `"oi: usage: /session switch ID"`, continue.
2. If `config->switch_session == NULL` → print "not available", continue.
3. **Snapshot for rollback**: if `conversation != NULL`, clone its current message list into a local malloc-owned `struct oi_cli_message_list rollback_context` via the same loop already used at `cli.c:894-902` (`oi_cli_conversation_messages(conversation)` → `oi_cli_message_list_append_clone` per item). On OOM here, treat as a structural failure (break), since the following step needs a safe fallback and this failing means rollback wouldn't be reliable either.
4. **Destroy the current conversation now**: `oi_cli_conversation_destroy(conversation); conversation = NULL;` — required precondition before calling `switch_session` (section 6.2).
5. Call `config->switch_session(config->switch_session_user_data, id, id_len, &switch_result)`.
6. **On real `oi_status` failure** (OOM path): treat like every other structural failure in this loop — `free(prompt); break;` (matches `/compact`'s own structural-failure handling, e.g. `cli_repl.c:1061-1064`). Note: at this point `conversation == NULL` and the old arena/session is untouched (callback never reached its commit point), but the loop is ending anyway, so no rebuild is needed.
7. **On `outcome != OK`** (SAME/NOT_FOUND/BUSY/INVALID/CORRUPT): print the matching user-facing message (`"oi: already on session <id>"` / `"oi: no such session: <id>"` / `"oi: session <id> is open in another process"` / `"oi: invalid session id"` / `"oi: session <id> could not be loaded"`), then **rebuild the conversation from the rollback snapshot using the still-valid old `active_arena`** (unchanged — the callback never touched it) and the **unchanged** `current_model`: `oi_cli_conversation_create(client, reactor, active_arena, tools, &conversation_config, &rollback_context, &conversation)`. Free `rollback_context`. `continue`. This is exactly how "a failed switch leaves the original session active and usable" is satisfied mechanically — the same arena, same messages, same model, just a freshly-constructed `oi_cli_conversation` object over them (conversations don't retain state that survives destroy/recreate beyond what's in the arena + the message list already captured).
8. **On `outcome == OK`**: `active_arena = switch_result.arena;` `oi_cli_string_set(current_model, switch_result.model.data, switch_result.model.len);` rebuild input history: `oi_cli_input_history_free(&input_history); oi_cli_input_history_init(&input_history); seed_input_history(&input_history, &switch_result.initial_context);` (reusing the existing static `seed_input_history` at `cli_repl.c:28-51` unmodified). Create the new conversation immediately (eager, not lazy — the issue requires restoring model/CWD/context right away, and optionally offering full replay): `oi_cli_conversation_create(client, reactor, active_arena, tools, &conversation_config, &switch_result.initial_context, &conversation)`. Free `rollback_context` (unused on the success path) and `switch_result`'s owned fields. Print `"oi: switched to session <id>"`.
9. **Optional full visible replay** (issue says "optionally offer"): after a successful switch, use the same `oi_cli_composer_select` confirm pattern as `/permissions allow`/`/session delete` with options `["No", "Show full conversation"]`; on "yes", iterate `switch_result.initial_context.items[]` and print each via the existing `oi_cli_present`/render pipeline (reuse whatever helper already renders a restored message on startup replay — **verify during implementation** whether one already exists for the non-interactive `--session` resume path, or whether this needs a small new plain-print loop over `oi_cli_message` role/content). This is a nice-to-have UX step; if it proves nontrivial to wire against `oi_cli_present`'s incremental-Markdown streaming state cleanly, it is acceptable to scope it down to a plain `fprintf` dump per message (role tag + content) rather than full styled rendering — note this explicitly in the commit message if descoped, do not silently skip it.

**Commit boundary**: this entire section 6 is **one commit** (`oi_cli_session_switch` + its unit tests) followed by **one more commit** (the `cli_repl.c`/`cli.c` wiring + its own tests) — see section 8. **Both get dedicated ASan+UBSan+valgrind runs**, not just the standard `make check`, matching this repo's convention for lifetime/concurrency-risk commits (comparable to issue #27's compact-then-live-atomicity commit, per the task description).

---

## 7. Command grammar wiring

### 7.1 Subcommand tokenizer

`oi_cli_command_parse_text` (`cli_commands.c:67-119`) already gives `command->arguments`/`arguments_len` as the **entire trimmed remainder** after `/session` (e.g. `"switch abc123"` for `/session switch abc123`) — no existing multi-word split (`argument_equals` at `cli_command_dispatch.c:68-73` only compares the whole blob, used by `/model`/`/permissions`/`/cwd` which are single-argument commands). Add a small new static helper in `cli_command_dispatch.c`:
```c
static void split_first_token(const char *text, size_t len,
                               const char **out_token, size_t *out_token_len,
                               const char **out_rest, size_t *out_rest_len);
```
Splits at the first run of whitespace (reuse the same `is_space` predicate `cli_commands.c:26-29` uses — either export it or duplicate the 3-line check), left-trims `out_rest`. No right-trim needed on `out_rest` since `command->arguments` is already right-trimmed by `oi_cli_command_parse_text`.

### 7.2 `dispatch_session` in `cli_command_dispatch.c`

```c
static oi_status dispatch_session(const struct oi_cli_command_parse *command,
                                   struct oi_cli_command_context *context);
```
Called from `oi_cli_command_dispatch`'s `switch` at `cli_command_dispatch.c:239-240`, replacing the `print_deferred` stub — **but only reached at all** for `list`/`current`/`rename`/`trash`/`restore` (bare `/session` and `/session list` both mean "list"; `switch`/`delete`/`import` never reach here, per section 6.6/4/6). Structure:
```c
if (arguments_len == 0) → list
split_first_token(...)
if token == "list" && rest empty → list
else if token == "current" && rest empty → current
else if token == "rename" → split rest again into ID/NEW_NAME, call context->rename_session
else if token == "trash" → rest is ID, call context->trash_session
else if token == "restore" → rest is ID, call context->restore_session
else if token in {"switch","delete","import"} → these MUST have been intercepted earlier;
    reaching here means either config wiring is missing (non-interactive context, e.g.
    the dispatch unit tests) or a real bug -- print a clear
    "oi: /session <subcommand> requires the interactive REPL" message, do not crash.
else → usage error listing all subcommands
```
New callbacks on `struct oi_cli_command_context` (`cli_command_dispatch.h:29-39`), added alongside `set_model`/`set_cwd`, all following the exact same `(void *user_data, ...) -> oi_status` + separate `_user_data` field convention already established:
```c
oi_status (*list_sessions)(void *user_data, FILE *out);            /* NULL-safe: prints "not available" */
const char *(*session_metadata_path)(void *user_data);              /* section 3 */
oi_status (*rename_session)(void *user_data, const char *id, size_t id_len,
                             const char *name, size_t name_len, char **out_error_detail);
oi_status (*trash_session)(void *user_data, const char *id, size_t id_len,
                            char **out_error_detail);
oi_status (*restore_session)(void *user_data, const char *id, size_t id_len,
                             char **out_error_detail);
```
Each `_cb` implementation, wired in `cli.c`, is a thin wrapper calling the corresponding `cli_sessions.c` function with `automatic_context.root_override`/session-dir captured at startup (store this root in a small persistent struct alongside `persistence`/`automatic_context`, e.g. extend `struct persistence_context` with a `const char *sessions_root_override;` field, or add a new tiny `struct session_admin_context { const char *root_override; oi_session **session; };`).

`list_sessions`'s callback prints something like:
```
Sessions:
  20260728-091500-1234-000  (current, healthy)   model: gpt-...  updated 2026-07-28 09:20
  20260727-114500-5678-000  busy (open elsewhere) model: gpt-...  updated 2026-07-27 11:50
  20260726-...              degraded (rebuilt)    model: gpt-...  updated 2026-07-26 ...
```
using `oi_cli_sessions_enumerate` (section 2), marking the row matching `context->session_id` as `(current, ...)`.

### 7.3 `import` grammar

`/session import PATH` is intercepted in `cli_repl.c`, same tier as `switch`/`delete` (needs the composer confirm gate). Flow:
1. Parse `PATH` = the whole remainder after `import ` (paths may contain spaces; do **not** further tokenize past the first split — `split_first_token("import /some path/with spaces.oilog")` → token=`"import"`, rest=`"/some path/with spaces.oilog"` verbatim).
2. Show confirm selector: `["Cancel", "Copy '<PATH>' into a new session"]`.
3. On confirm, call `config->import_session(user_data, path, path_len, &new_id, &error_detail)` (new callback, same shape family as `switch_session`, but simpler — no arena/conversation swap involved since import never touches the active session).
4. Print result (`"oi: imported '<PATH>' as session <new_id> (use /session switch <new_id> to open it)"` or the error).

`oi_cli_session_import` in `cli_sessions.c`/`.h`:
```c
oi_status oi_cli_session_import(const char *root_override,
                                 const char *source_path, size_t source_path_len,
                                 char **out_new_id, char **out_error_detail);
```
1. `realpath(source_path, ...)`; must resolve to an existing plain regular file (`lstat` + `S_ISREG`, reject a symlink source too — same "no symlink following" posture as section 1's directory checks — via checking the **lstat**, not `stat`, result: `!S_ISLNK && S_ISREG`).
2. Reject if the resolved real path already lives under `<sessions_root>/...` (compare prefix against `oi_cli_sessions_default_root`/`root_override`'s own realpath) — "already an oi-managed session; use /session switch instead."
3. Copy byte-for-byte into a scratch temp file **directly under the sessions root** (e.g. `<root>/.import-<pid>-<random 8 hex chars from /dev/urandom or a counter>.tmp` — same-filesystem as the eventual destination, matters for step 5's `rename`). Read/write loop, `O_RDONLY` source / `O_WRONLY|O_CREAT|O_EXCL|0600` destination-tmp (never overwrite an existing scratch file — `O_EXCL` failure is a hard error, don't retry-with-new-name-loop here, just fail closed and let the user retry the command).
4. **Validate the copy** exactly as the issue asks: `oi_sesslog_open(tmp_path, &log)` (validates header/magic/version and recovers a truncated tail, reused unmodified) then `oi_cli_history_store_load(log, &store, &state)` (reused unmodified — this is "is this a legacy oilog" validation via the exact same code that would load it for real use). `oi_sesslog_close(log)` either way. On **either** call failing: `unlink(tmp_path)`, return a clear "not a valid oi session log" error, **source file untouched** (never opened for writing, never modified).
5. On success: `oi_cli_session_location_create(root_override, &location)` (existing function, generates a brand-new unique private directory + `history_path`). `rename(tmp_path, location.history_path)` (same filesystem, atomic). **Deliberately do not** append a transition record or run `restore_settings` here — that already happens automatically the first time this session is actually opened via `oi_session_create` (both `prepare_automatic_session` at `cli.c:221-233` and the explicit-`--session` path at `cli.c:862-883` already do this universally for *any* session opened by *any* path), so import correctly stays a pure "validate + relocate" operation with no duplicated logic. `*out_new_id = strdup(location.id)`.

Note the source file is **never** opened for writing, **never** unlinked, **never** modified at any point in this flow — "preserves the source file" is satisfied structurally, not just by convention.

---

## 8. Commit sequence

Each commit independently buildable and green under `make check` before moving to the next. Commits 9 and 10 are the highest-risk and get dedicated `make asan` + `make ubsan` + `make valgrind` runs beyond the standard suite (this repo's convention for lifetime/concurrency/durable-mutation-ordering commits — see `git log` for issue #27's equivalent).

1. **`oi_cli_session_id_is_safe` + `oi_cli_session_resolve`** in `cli_sessions.c`/`.h`, plus unit tests in `test/test_cli_sessions.c`: valid IDs, empty, overlong, `.`/`..`, embedded `/`, embedded NUL-adjacent-length games, disallowed chars, symlink-escape (`symlink()` a directory in the test, confirm rejection), stray regular file in place of a directory, nonexistent ID.
2. **`oi_cli_sessions_enumerate`** + tests: normal listing, mixed-in stray dotfile/non-directory entries (skipped silently), missing `metadata.json` (degraded + rebuilt), corrupt `metadata.json` (degraded + rebuilt), busy detection (fork a helper holding the flock via `oi_sesslog_open` + blocking read on a pipe, parent enumerates, then releases the child).
3. **Metadata schema v2 (`display_name`)**: extend `cli_session_metadata.h/.c`, `cli_session_metadata_codec.c` (bump version, encode always-v2, decode accepts v1-without-field and v2-with-field), update every existing call site of `oi_cli_session_metadata_set` (`cli_sessions.c`'s `refresh_metadata`). Tests in `test_cli_session_metadata.c`/`test_cli_session_metadata_codec.c`: v1-file-still-decodes, v2-round-trip, invalid display_name (control byte) rejected.
4. **`oi_cli_session_rename`** + tests (rebuild-from-missing-metadata path, bad name rejection, nonexistent ID).
5. **`oi_cli_session_trash` / `oi_cli_session_restore` / `oi_cli_session_delete`** (all three together, since restore/delete are trivial once trash's `.trash/` convention and lock-probe helper exist) + tests: trash-then-restore round trip, trash-refuses-current, trash-refuses-busy (fork helper again), delete-refuses-live-target (never trashed), delete-actually-removes, cross-device `EXDEV` path (can simulate by mocking `rename` return via a small seam **only if the existing codebase has a precedent for that kind of test seam — verify during implementation**; otherwise it's acceptable to leave `EXDEV` handling code-reviewed-only and note it as a documented gap, since reliably provoking a real `EXDEV` in a portable test is impractical).
6. **`oi_cli_session_import`** + tests: valid legacy log imports correctly (source untouched, new dir created, right content), malformed source rejected (source untouched, no new dir left behind), source-is-a-symlink rejected, source-already-inside-sessions-root rejected.
7. **`cli_command_dispatch.c`: `split_first_token` + `dispatch_session` (list/current/rename/trash/restore only)** + new `oi_cli_command_context` callback fields + unit tests in `test_cli_command_dispatch.c` (usage errors, each subcommand happy-path with a fake callback, `switch`/`delete`/`import` tokens reaching dispatch print the "requires interactive REPL" message rather than crashing).
8. **`docs/REPL_PLAN.md` grammar update** (small, can ride with commit 7 or stand alone) — document the full `/session` syntax table.
9. **`oi_cli_session_switch` (new `src/cli_session_switch.c`/`.h`) + its own integration-style tests** (link directly against it plus its deps, no REPL/PTY involved — mirrors `test/integration/test_cli_compact.c`'s pattern of testing one orchestration function directly): switch to an existing valid session succeeds and returns correct context/model/cwd; switch to same ID reports `SAME`; switch to nonexistent reports `NOT_FOUND`; switch to a busy (flock-held-by-forked-helper) session reports `BUSY` and leaves the registry containing only the original session; switch to a session whose log has a corrupted trailing record still succeeds (recovery truncation, matches `oi_sesslog_open`'s own documented recovery) while a *structurally* undecodable log reports `CORRUPT`. **Dedicated ASan+UBSan+valgrind run required for this commit** — this is the "genuinely new capability to destroy/recreate registry-owned resources" commit.
10. **`cli_repl.c`/`cli.c` wiring**: new `switch_session` callback shape in `cli_repl.h`, `have_parsed_command:` interception for `/session switch` (section 6.6, including rollback-on-failure), `cli.c`'s `switch_session_cb` wrapper (section 6.5), plus the `/session delete` and `/session import` confirm-gate interceptions (section 4/7.3) with their own `cli.c` callback wrappers around `oi_cli_session_delete`/`oi_cli_session_import`. Tests: extend `test/test_cli.c` (see section 9) with new interactive `TEST()` cases using its existing PTY+mock-server harness. **Dedicated ASan+UBSan+valgrind run required** — this is the "destroys/recreates conversation+history+arena mid-run" commit, the highest-risk commit in the whole issue.
11. **Final verification + GitHub closeout**: full `make check`, `make asan`, `make ubsan`, `make valgrind` (all green), re-read every acceptance-criteria checkbox in the issue text against what was actually built, update `docs/REPL_PLAN.md`'s `## Delivery sequence`/`## Verification gates` sections if anything shifted, close the issue with a summary comment referencing the commits.

---

## 9. Test strategy detail

### 9.1 Unit tests (module-level, no REPL/PTY)
- `test/test_cli_sessions.c`: extend with everything in commits 1, 2, 4, 5, 6 above. Reuse the existing `make_tmp_dir(suffix)` helper already at `test_cli_sessions.c:119` for isolated sessions-root fixtures per test.
- `test/test_cli_session_metadata*.c`: schema v2 additions (commit 3).
- `test/test_cli_command_dispatch.c`: grammar/dispatch-level tests (commit 7) using fake callbacks (the existing test file already does this for `set_model`/`set_cwd` — **verify its exact fake-callback pattern during implementation** and match it).
- New `test/integration/test_cli_session_switch.c` (Makefile: add a dedicated build rule alongside the existing `$(INTEGRATION_BUILD)/test_cli_compact` one at `Makefile:229`, since this needs `cli_session_switch.c` + `cli_sessions.c` + `cli_history_store.c` + friends as extra deps beyond the generic `$(INTEGRATION_BUILD)/%` pattern rule at `Makefile:223`) for commit 9's `oi_cli_session_switch` tests.

### 9.2 Full end-to-end PTY tests — **infrastructure already exists, reuse it**

**Correction to this plan's own prior research pass**: `test/test_cli.c` (4434 lines) **already is** a full PTY-driven, mock-API-backed, end-to-end integration harness for the actual compiled `oi` binary — built via `$(BUILD)/test_cli` (`Makefile:97-98`, linking `-DOI_CLI_BIN=... $(LIB) $(PTY_LIBS)` where `PTY_LIBS = -lutil`, `Makefile:21`). It already exercises `/model`, `/permissions` (including the confirm-selector gate this plan reuses for `/session delete`/`/session import`), the tool panel, `Ctrl+C`, queued commands, and crash recovery, all through a **real PTY** (`openpty()` from `<pty.h>`) with the child process's controlling terminal set up via `setsid()`+`TIOCSCTTY` (`test_cli.c:428-469`) and a forked mock HTTP server (`start_mock_server*` family, `test_cli.c:45-270`). **No new PTY harness needs to be built.** Add new `TEST()` cases directly to this file, reusing its existing statics:
- `start_interactive_cli(port, slave_fd, session_root)` (`test_cli.c:428-469`) — spawns `oi` with `--session-dir <session_root>` (which is exactly `automatic_context.root_override`, section 6/7's `root_override`), so tests get a fully isolated, disposable sessions root per test with zero risk to the real user's `$XDG_STATE_HOME`.
- `interactive_wait_for(master_fd, &result, "text", count)` (`test_cli.c:371-413`) and `write_interactive(fd, data, len)` (`test_cli.c:415-426`) for driving/asserting on the terminal transcript.
- `oilog_records_load`/`oilog_find`/`oilog_count` (`test_cli.c:2051-2162`) for asserting on-disk durable content directly (e.g. confirming a switched-to session's log gained the expected records, or a trashed session's directory really moved).

New `TEST()` cases to add (in `test_cli.c`, following the exact style of `interactive_model_command_changes_the_live_model` at `test_cli.c:1187` and `queued_message_resumes_at_the_safe_boundary_with_correct_turn_ids` at `test_cli.c:2162`):
- `interactive_session_list_shows_current_session`
- `interactive_session_current_reports_healthy`
- `interactive_session_switch_restores_context_and_model` — start with `--session <A>` pre-seeded (reuse `start_interactive_cli_with_session`, `test_cli.c:562-604`), send a message, then a **second** pre-existing session directory `<B>` (built by the test directly on disk before spawning, or via a first short-lived `oi` invocation using `oi_cli_loop_run`'s one-shot mode against `<B>`), then `/session switch <B>`, then assert the transcript shows `<B>`'s restored content and a follow-up message lands in `<B>`'s `.oilog` (via `oilog_records_load`), **not** `<A>`'s.
- `interactive_session_switch_to_busy_session_reports_busy_and_keeps_original_active` — hold `<B>`'s flock via a forked helper (open `<B>`'s log via a tiny helper `TEST`-local child, matching the `start_silent_server` fork-and-hold pattern from `test_cli_compact.c:33-80`), attempt `/session switch <B>` from the interactive CLI, assert the "busy" message appears **and** a follow-up plain message still lands correctly in `<A>`'s original log.
- `interactive_session_switch_to_nonexistent_reports_not_found`
- `interactive_session_trash_and_restore_round_trip`
- `interactive_session_delete_requires_confirmation` — send `/session delete <id>`, assert the confirm-selector text appears, send the "Cancel" key, assert nothing was deleted; repeat and confirm, assert the directory is gone from disk.
- `interactive_session_import_requires_confirmation_and_preserves_source` — write a legacy-format `.oilog` file to a scratch path (raw bytes matching whatever the pre-transition legacy record shape is — **verify the exact byte format expected by `oi_cli_history_store_load`'s legacy path during implementation**, likely reusable from an existing fixture in `test_cli_history_store.c`), send `/session import <path>`, confirm, assert a new session appears in `/session list` and the original file at `<path>` is byte-identical to before.

If any of these interactive PTY cases prove disproportionately time-consuming to stabilize (PTY tests are inherently more flake-prone under CI timing than pure unit tests — see the generous 20-second `interactive_wait_for` timeout comment at `test_cli.c:376-379` acknowledging this), it is acceptable to **descope only the flakiest one or two** at the final closeout, provided every acceptance-criteria case is still covered by an equivalent non-interactive unit/integration test from section 9.1 — do not silently drop coverage, note explicitly in the closeout comment which PTY case (if any) was descoped and why, and what the substitute coverage is.

---

## Open items to verify during implementation (flagged, not blocking the plan)

- Exact byte-level shape of `oi_cli_session_metadata_codec.c`'s encode/decode (not read during this research pass) — confirm the v1→v2 additive-field decode logic fits its existing strict-decode style (`cli_session_metadata_codec.h:21-23` says it currently "rejects unknown/missing/extra fields" — this must be relaxed specifically for the new optional `display_name` key without weakening rejection of genuinely unknown/typo'd keys).
- Whether an existing helper already renders a restored message list for the non-interactive `--session` resume path, reusable for section 6.6's "optional full replay" step, or whether that needs a small new function.
- Exact legacy-`.oilog` byte fixture already used by `test_cli_history_store.c` for its own legacy-replay tests — reuse it verbatim for the import test's "valid legacy source" fixture rather than hand-rolling a new one.
- Whether a cross-device (`EXDEV`) rename path can be exercised in a real test at all in this environment (see commit 5's note) — if not practically testable, code-review it carefully instead and say so explicitly at closeout.

### Critical Files for Implementation
- /home/az/Projects/oi.c/src/cli_sessions.c
- /home/az/Projects/oi.c/src/cli_sessions.h
- /home/az/Projects/oi.c/src/cli_repl.c
- /home/az/Projects/oi.c/src/cli.c
- /home/az/Projects/oi.c/src/cli_command_dispatch.c
- /home/az/Projects/oi.c/src/cli_session_metadata.h
- /home/az/Projects/oi.c/test/test_cli.c
