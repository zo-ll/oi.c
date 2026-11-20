# cli_docs: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `test/test_cli_docs.c`.

## `file scope` (test/test_cli_docs.c)

Documentation drift guard.

The user-facing docs restate two things the code owns: the slash-command registry and the key bindings /help prints. Neither restatement can be generated (they are prose with examples around them), so they are checked instead: every command name, usage string, and description in cli_commands.c must appear in docs/CLI.md, and so must every Ctrl+key binding /help mentions. Adding or renaming a command therefore fails the suite until the guide is updated.

Doc paths are repository-relative, like OI_CLI_BIN in the Makefile: the test binaries are run from the repository root.

## `read_flattened` (test/test_cli_docs.c)

Reads a text file with every run of whitespace collapsed to one space, so a phrase the guide wrapped across two lines still matches the single-line string the registry holds.
