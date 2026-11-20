# cli_credentials: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_credentials.c`, `src/cli_credentials.h`.

## `wipe` (src/cli_credentials.c)

Volatile so the compiler cannot drop a store to soon-freed memory.

## `oi_cli_credentials` (src/cli_credentials.h)

Every string is heap-owned, NUL-terminated, and NULL when absent.

## `oi_cli_credentials_free` (src/cli_credentials.h)

Overwrites every secret before releasing it.

## `oi_cli_credentials_set` (src/cli_credentials.h)

Replaces `*slot`, overwriting the secret it held.

## `oi_cli_credentials_default_path` (src/cli_credentials.h)

$XDG_CONFIG_HOME/oi/credentials.json, else ~/.config/oi/credentials.json.

## `oi_cli_credentials_read` (src/cli_credentials.h)

OI_ERR_NOTFOUND when the file does not exist; OI_ERR_DENIED when anyone but the owner can reach it, because a secret that was already exposed must not be used silently; OI_ERR_PARSE when it is malformed or has an unknown version.

## `oi_cli_credentials_write` (src/cli_credentials.h)

Atomically replaces `path` with a 0600 file, creating missing parent directories as 0700, and syncs it: losing this file means signing in again.

## `oi_cli_credentials_parse_url` (src/cli_credentials.h)

Splits an http(s) URL into endpoint parts. A URL that names only a host or an API base gets the Chat Completions route appended, so both "https://h" and "https://h/api/v1" work; a path with a query string is used as given. Rejects a missing scheme, user info, an IPv6 literal, a bad port, and control characters or spaces.
