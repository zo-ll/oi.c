# cli_session_metadata: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_session_metadata.c`, `src/cli_session_metadata.h`, `src/cli_session_metadata_codec.c`, `src/cli_session_metadata_codec.h`, `src/cli_session_metadata_store.h`, `test/test_cli_session_metadata_codec.c`, `test/test_cli_session_metadata_store.c`.

## `display_name_is_valid` (src/cli_session_metadata.c)

An unset display name is valid; a set one must be bounded and free of control bytes, since /session list echoes it to the terminal as-is.

## `file scope` (src/cli_session_metadata.h)

Rebuildable selector metadata for one durable session -- never authoritative (history.oilog is; see docs/REPL_PLAN.md's storage model). Deleting or corrupting this is recoverable by replaying history. No API keys, auth headers, or other secrets are ever in scope for this struct -- enforced structurally: nothing here has access to them.

## `OI_CLI_SESSION_METADATA_SCHEMA_VERSION` (src/cli_session_metadata.h)

Version 2 adds `display_name`. Additive only: a version 1 file still decodes (with no display name), and every write from here on emits version 2 -- see cli_session_metadata_codec.h for the wire shapes.

## `OI_CLI_SESSION_METADATA_MAX_FILE` (src/cli_session_metadata.h)

Hard ceiling on the whole encoded file when reading it back.

## `OI_CLI_SESSION_METADATA_MAX_DISPLAY_NAME` (src/cli_session_metadata.h)

A display name is a label for a selector row, not a path or an id.

## `oi_cli_session_metadata.cwd` (src/cli_session_metadata.h)

bounded by OI_CLI_HISTORY_MAX_SETTING_VALUE, shared with the durable setting records

## `oi_cli_session_metadata.display_name` (src/cli_session_metadata.h)

Optional user-chosen label, shown by /session list in place of the id. `len == 0` means unset, which is the normal state -- callers fall back to the id. Bounded by MAX_DISPLAY_NAME and guaranteed free of control bytes, because it is echoed straight to a terminal.

## `oi_cli_session_metadata.created_at` (src/cli_session_metadata.h)

Unix epoch seconds

## `oi_cli_session_metadata_set` (src/cli_session_metadata.h)

Replaces `metadata` wholesale, or leaves it untouched on failure.

`display_name` may be NULL (or `display_name_len` 0) for "unset". A non-empty one is rejected with OI_ERR_INVAL when it exceeds MAX_DISPLAY_NAME or contains a control byte: it reaches a terminal unescaped, so refusing it here keeps every consumer -- /session list included -- from having to sanitize.

## `oi_cli_session_metadata_encode` (src/cli_session_metadata_codec.c)

Always emitted, "" when unset, keeping a version 2 object at a fixed seven fields.

## `oi_cli_session_metadata_decode` (src/cli_session_metadata_codec.c)

Exact count per version: version 1 has six fields and version 2 adds display_name for seven. Checking the count this way keeps an unknown or misspelled key a parse error instead of something silently ignored, which is the whole point of the strict decode.

Sets the current schema version regardless of what was read, so a version 1 file comes back as an in-memory version 2 struct and the next write emits version 2.

## `oi_cli_session_metadata_encode` (src/cli_session_metadata_codec.h)

JSON shape, version 2 (what every write emits): {"version":2,"session_id":"...","model":"...","cwd":"...", "display_name":"...","created_at":"1785262200", "updated_at":"1785262260"}

Version 1 is the same without `display_name`. `version` is a plain number; `created_at`/`updated_at` are decimal strings (matches the record-ID convention in cli_history_codec.c -- no float-precision risk, and consistent encoding across both codecs).

`display_name` is always written, as "" when unset, so a version 2 object has exactly seven fields and the decoder can keep checking an exact field count per version rather than tolerating a range.

## `oi_cli_session_metadata_decode` (src/cli_session_metadata_codec.h)

Strict: rejects wrong types, a version that is neither 1 nor 2, a field count that does not match the version exactly (so an unknown or misspelled key is still refused rather than ignored), or a decoded struct that fails oi_cli_session_metadata_is_valid.

A version 1 object decodes with no display name and is upgraded in memory to the current schema version, so the next write of it emits version 2. Nothing ever rewrites a version 1 file in place purely to migrate it -- metadata is a rebuildable cache, and the upgrade rides along with whatever change was already being persisted.

## `oi_cli_session_metadata_store_read` (src/cli_session_metadata_store.h)

OI_ERR_NOTFOUND if missing; OI_ERR_PARSE for malformed JSON, a bad version, oversized fields, an empty file, or a file over OI_CLI_SESSION_METADATA_MAX_FILE.

## `oi_cli_session_metadata_store_write` (src/cli_session_metadata_store.h)

Writes to "<metadata_path>.tmp" (a fixed name is safe: sesslog.c already takes an exclusive flock on the co-located history.oilog for the session's lifetime, so a session directory is single-owner), 0600, then rename()s over metadata_path. Cleans up the temp file on every failure path -- including a failed rename -- without touching a pre-existing valid metadata_path. No fsync: matches PLAN.md's stated "no fsync by default" stance for the append-only log (crash-safe against process crash, not power loss); history.oilog stays authoritative regardless of what happens to this file.

## `a_version_1_file_still_decodes_and_upgrades_in_memory` (test/test_cli_session_metadata_codec.c)

A file written before display_name existed must keep working: the schema change is additive, and metadata is a cache users never migrate by hand.

## `failed_rename_leaves_the_destination_untouched_and_cleans_up` (test/test_cli_session_metadata_store.c)

Make the destination an existing directory: rename(2) of a regular file onto it fails with EISDIR, forcing a rename failure without ever affecting whether the temp file could be created (same directory, a distinct filename).
