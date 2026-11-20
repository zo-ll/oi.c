# cli_bytebuf: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_bytebuf.h`.

## `oi_cli_bytebuf` (src/cli_bytebuf.h)

A growable byte buffer with doubling growth. Shared by the sanitize, UTF-8 stream, and Markdown modules so each doesn't reimplement its own append-with-overflow-check logic.

## `oi_cli_bytebuf_reset` (src/cli_bytebuf.h)

Sets len to 0 without releasing the underlying allocation.
