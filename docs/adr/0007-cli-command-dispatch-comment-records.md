# cli_command_dispatch: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_command_dispatch.c`, `src/cli_command_dispatch.h`, `test/test_cli_command_dispatch.c`.

## `OI_CLI_COMMAND_MODEL_MAX_LEN` (src/cli_command_dispatch.c)

UX-level bound for /model NAME, generous for any real model id -- on top of the shared 4096-byte storage ceiling (OI_CLI_HISTORY_MAX_SETTING_VALUE) enforced further down the stack.

## `print_help` (src/cli_command_dispatch.c)

Deliberately the whole binding set, not a sample: /help is the only in-terminal reference, and docs/CLI.md's key table is checked against this list by test_cli_docs so the two cannot drift apart.

## `print_status` (src/cli_command_dispatch.c)

Every field comes from the one snapshot the owning modules assembled -- dispatch deliberately does not mix in `context->model`, `->session_id`, or a getcwd() of its own, even though it has all three to hand. A report stitched from two sources can disagree with itself (a /model change applied to the conversation but not yet to this context, say), and there would be no way to tell which half was right.

## `oi_cli_status_snapshot_init` (src/cli_command_dispatch.c)

The semantic initializer, not a memset: a zeroed snapshot would claim a pile of real states (no session, ask, disabled deadlines, idle, empty queue) that the callee never asserted.

## `fputs` (src/cli_command_dispatch.c)

A bad name is a user-input problem, not a structural failure: report it and stay in the REPL, matching /permissions' usage-error handling.

An invalid/missing/non-directory path is a user-input problem, not a structural failure: report it and stay in the REPL.

## `print_cwd` (src/cli_command_dispatch.c)

A successful set_cwd always leaves the process's real cwd at the new canonical location, so getcwd() is always correct here -- no display value needs threading through the callback.

## `split_first_token` (src/cli_command_dispatch.c)

Splits `text` at its first run of whitespace into a leading token and a left-trimmed remainder. `text` arrives already right-trimmed from oi_cli_command_parse_text, so the remainder needs no further trimming -- which matters for /session import, whose argument is a path that may legitimately contain spaces and must survive verbatim.

## `format_timestamp` (src/cli_command_dispatch.c)

Formats one epoch timestamp for a selector row; 0 means unknown.

## `print_session_list` (src/cli_command_dispatch.c)

A leading marker beats a trailing "(current)": it lines the rows up so the ids stay scannable.

## `fprintf` (src/cli_command_dispatch.c)

Health is resolved live on every call rather than cached from startup: metadata can be deleted or corrupted by something outside oi at any point during a long-running session.

## `dispatch_session_admin` (src/cli_command_dispatch.c)

An id is a single bounded token, so trailing words are a mistake worth naming rather than silently ignoring.

## `oi_status` (src/cli_command_dispatch.h)

/model and /cwd request changes through these callbacks rather than touching files or the conversation object directly -- cli_repl.c/cli.c own what a change actually does (live conversation update, durable history append, metadata refresh); dispatch only validates bounds and reports the result.

Fills in the /status snapshot. Assembled on demand rather than handed in pre-built, for two reasons: /status must be able to report a turn that is active *right now*, and the snapshot's borrowed strings (a working directory in particular) would otherwise have to stay valid across every other command's dispatch too.

Every pointer written into `*out_snapshot` is borrowed from the callee and valid only until the next call through the same callback -- the same contract as oi_cli_command_session_ops::current. Dispatch runs oi_cli_status_snapshot_init before calling, so any field the callee leaves alone renders as explicitly unknown rather than as a stale value or a plausible-looking default.

Returning non-OK means no snapshot could be assembled; dispatch reports that and stays in the REPL.

## `oi_cli_command_session_current` (src/cli_command_dispatch.h)

What /session current reports. Both strings are borrowed and valid only until the next call through the same ops.

## `oi_cli_command_session_current.id` (src/cli_command_dispatch.h)

NULL when no durable session exists

## `oi_cli_command_session_current.path` (src/cli_command_dispatch.h)

the session directory or log; may be NULL

## `oi_cli_command_session_current.healthy` (src/cli_command_dispatch.h)

selector metadata present and owned by this id

## `oi_cli_command_session_ops` (src/cli_command_dispatch.h)

/session delegates every filesystem operation through these, for the same reason /model and /cwd do: dispatch validates grammar and renders the result, while cli.c owns what an operation actually does. Grouped rather than spread across a dozen flat fields because they share one owner, one lifetime, and one `user_data`.

`list`/`list_trashed` fill a caller-initialized list that dispatch then formats and frees. `rename`, `trash`, and `restore` report a specific cause through `*out_error_detail` (caller-owned, may be left NULL).

Any callback may be NULL, meaning that operation is unavailable in this context; dispatch says so rather than crashing. /session switch, delete, and import are absent by design -- they need the composer's confirmation flow or the live conversation, so cli_repl.c intercepts them before dispatch is reached.

## `fake_status` (test/test_cli_command_dispatch.c)

Stands in for cli_repl's assembler. The strings handed back are static, so the borrowed-pointer contract ("valid until the next call through the same callback") holds trivially -- which is the point: dispatch must not need to copy anything.

## `status_reports_the_assembled_snapshot.context` (test/test_cli_command_dispatch.c)

`model`, `session_id`, and `permission` are deliberately set to values the snapshot contradicts: /status must report the snapshot and nothing else, so a stale context field cannot leak into the report.

## `status_reports_unknown_for_everything_a_callback_leaves_alone` (test/test_cli_command_dispatch.c)

Dispatch must hand the callback an honestly-unknown snapshot, not a zeroed one: a callback that reports nothing must produce a report that claims nothing. Asserted here, at the boundary that owns the initialization, and not only in the renderer's own tests.
