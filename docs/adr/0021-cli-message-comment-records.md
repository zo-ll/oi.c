# cli_message: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_message.h`.

## `oi_cli_string_set` (src/cli_message.h)

`string` must be zero-initialized or previously managed by these helpers.

## `oi_cli_message` (src/cli_message.h)

Heap-owned model-visible message. Strings carry explicit lengths and also have a trailing NUL for APIs that accept ordinary C strings.

## `oi_cli_message_list_append_take` (src/cli_message.h)

On success ownership moves into the list and `message` is reset. On failure the caller retains ownership.
