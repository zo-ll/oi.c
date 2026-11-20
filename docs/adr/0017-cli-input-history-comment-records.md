# cli_input_history: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_input_history.h`.

## `oi_cli_input_history` (src/cli_input_history.h)

Process-scoped submitted-input history. Navigation returns borrowed text valid until the history is mutated or freed.
