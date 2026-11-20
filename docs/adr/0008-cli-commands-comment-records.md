# cli_commands: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_commands.h`.

## `oi_cli_command_filter` (src/cli_commands.h)

Filters command names using the current first token, including its leading slash. Returns the total match count; up to `capacity` indices are written.
