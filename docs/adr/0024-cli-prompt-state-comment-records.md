# cli_prompt_state: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_prompt_state.h`.

## `oi_cli_prompt_state` (src/cli_prompt_state.h)

Pure prompt interaction state. It owns the current editor and borrows the process-scoped history for its entire lifetime.

## `oi_cli_prompt_state_commit` (src/cli_prompt_state.h)

Copies the current non-empty input into caller-owned storage, appends it to process history, and clears the editor. On failure the editor is unchanged.
