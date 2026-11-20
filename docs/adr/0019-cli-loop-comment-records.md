# cli_loop: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_loop.h`.

## `oi_cli_loop_run` (src/cli_loop.h)

Runs one user turn through zero or more model-requested tool calls. The passed reactor, arena, client, and registry are borrowed.
