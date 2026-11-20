# cli_editor: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_editor.h`.

## `oi_cli_editor` (src/cli_editor.h)

Pure editable-text state. Cursor positions are byte offsets at UTF-8 code point boundaries. Terminal input decoding and rendering live elsewhere.
