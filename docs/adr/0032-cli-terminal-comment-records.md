# cli_terminal: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_terminal.h`.

## `oi_cli_terminal_enable` (src/cli_terminal.h)

Both descriptors must refer to terminals. Enables raw input and paste mode.

## `oi_cli_terminal_set_isig` (src/cli_terminal.h)

Toggles signal-generating control characters while leaving the rest of raw mode intact. Useful around a synchronous operation that watches a blocked SIGINT through signalfd but must preserve ordinary type-ahead.

## `oi_cli_terminal_restore` (src/cli_terminal.h)

Safe to call repeatedly. The first call restores the captured terminal.
