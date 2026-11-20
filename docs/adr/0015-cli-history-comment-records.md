# cli_history: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_history.h`, `src/cli_history_codec.h`, `src/cli_history_repair.h`, `src/cli_history_replay.c`, `src/cli_history_replay.h`, `src/cli_history_store.h`.

## `OI_CLI_HISTORY_MAX_SETTING_VALUE` (src/cli_history.h)

Shared ceiling for a durable session setting's value (model name or working directory). Doubles as metadata.json's field ceiling.

## `oi_cli_history_session_setting_field` (src/cli_history.h)

Which durable session setting a session_setting record carries. Values are stable on-disk identifiers (encoded as strings, not these ints, but kept nonzero so a zeroed/uninitialized field is never mistaken for a real one).

## `oi_cli_history_record_init` (src/cli_history.h)

Records must be initialized before being passed to a set function.

## `oi_cli_history_record_encode` (src/cli_history_codec.h)

Encodes one record as a deterministic JSON object. The returned buffer is heap-owned by the caller and must be freed with free().

## `oi_cli_history_record_decode` (src/cli_history_codec.h)

Strictly decodes one version-1 record. Unknown, duplicate, or missing fields; non-canonical IDs/base64; and invalid record shapes are rejected. `out_record` must already be initialized.

## `oi_cli_history_build_repairs` (src/cli_history_repair.h)

Builds the append-only records needed to complete an interrupted turn. `out_repairs` must be initialized and is replaced only on success.

## `apply_checkpoint` (src/cli_history_replay.c)

Recorded only now that the splice has committed, so a rejected or failed checkpoint never leaves the state claiming a compacted context it does not have.

## `oi_cli_history_replay_state.last_model` (src/cli_history_replay.h)

Last-known durable session settings, reconstructed from session_setting records. Empty (data == NULL) if never set.

## `oi_cli_history_replay_state.checkpoint_source_first_record_id` (src/cli_history_replay.h)

The range collapsed by the most recent checkpoint applied while rebuilding `context`, so a reporting caller (/status) can name what replaced the active context's prefix without re-reading the log or reaching into the store's record array. Taken from the checkpoint record itself rather than derived from `context` -- once a second checkpoint has subsumed the first, only the record still states what it covered. Both are 0 until a checkpoint is applied, and `has_checkpoint` being set is exactly what makes context[0] a summary rather than the session's oldest real message.

## `oi_cli_history_replay` (src/cli_history_replay.h)

Validates the complete typed history and rebuilds model-visible context. `legacy_messages`, when non-NULL, contains the old alternating records in their original order. The output is replaced only on success.

## `oi_cli_history_store_load` (src/cli_history_store.h)

Loads, decodes, and strictly replays a session log. The store borrows `log`; neither free nor a failed load closes it. Both outputs must be initialized.

## `oi_cli_history_store_append` (src/cli_history_store.h)

Validates and durably appends one typed record, then publishes the rebuilt replay state. The store remains unchanged if the append itself fails.
