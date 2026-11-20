# cli_input: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_input.h`.

## `oi_cli_input_decode` (src/cli_input.h)

Decodes one event from the start of `data`. OI_ERR_AGAIN means the bytes are a valid but incomplete sequence and must be retained for the next read. Otherwise `out_consumed` is always positive.
