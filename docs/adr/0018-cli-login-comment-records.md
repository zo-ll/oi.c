# cli_login: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_login.c`, `src/cli_login.h`, `test/test_cli_login.c`.

## `REFRESH_MARGIN_SECONDS` (src/cli_login.c)

Renew this long before expiry, so that a long turn keeps a valid token.

## `USABLE_SECONDS` (src/cli_login.c)

A request does not start with a token that has less time than this left; it waits for the renewal instead.

## `log_in_with_key` (src/cli_login.c)

Reports through `message`; a non-OK return is a structural failure.

## `lock_credentials` (src/cli_login.c)

Takes the credentials lock without waiting: -1 with EAGAIN or EACCES when another process holds it, which means that process is renewing.

## `refresh_unavailable` (src/cli_login.c)

A transient failure is harmless while the current token still works.

## `login_renewal` (src/cli_login.c)

One renewal on the main reactor. It holds the credentials lock from start to finish, because the server rotates the refresh token and a second process must not reuse the old one. Its result waits here until the next request start adopts it: oi_llm_client_reconfigure is not allowed while a request is in flight, and the before_request hook is the point where none is.

## `renewal_done` (src/cli_login.c)

Runs on the reactor, possibly while a request is in flight: it saves the outcome and leaves the client alone.

## `start_renewal` (src/cli_login.c)

Another process is renewing: read its result at a later start.

Another process renewed the token before this one took the lock.

## `finish_sent_renewal` (src/cli_login.c)

Steps the reactor until a sent renewal ends, so that its rotated tokens reach the credentials file. The exchange's own 20 s timeout bounds the wait. Ctrl+C ends the wait early and leaves the renewal for the caller to drop, so the file keeps the tokens it had: the REPL keeps SIGINT blocked for the life of the process, so the wait reads it from its own signalfd; where SIGINT is not blocked, it ends the process, and the file is replaced only by rename, so it stays whole.

## `token_usable` (src/cli_login.c)

A renewal runs, here or in another process: use the current token while it still has time left, else make the request wait.

## `open_browser` (src/cli_login.c)

Tries the usual openers; the URL is on screen if none of them works.

## `read_request_head` (src/cli_login.c)

Reads through the composer wait, so Esc and signals work while a client is slow. One deadline covers the whole head: a client that sends one byte at a time cannot hold the listener for longer than HEAD_TIMEOUT_MS.

## `wait_for_callback` (src/cli_login.c)

Any local process or web page can reach the listener, so only a callback that carries this attempt's `state` ends the wait.

## `prepare_chatgpt_flow` (src/cli_login.c)

The host identity must exist before the first registration.

## `oi_cli_login_run.isig` (src/cli_login.c)

A renewal belongs to the login it started under. A sent one finishes first, so that its rotated tokens are saved; then it is dropped, so it cannot write that login's credentials over the new ones, nor adopt its token after them, and it releases the credentials lock. The raw prompt turns Ctrl+C into a byte; ISIG makes it the SIGINT that ends the wait. The wait runs without ISIG too: the save must not depend on the terminal.

## `oi_cli_login_free` (src/cli_login.h)

Finishes a sent token renewal on `login->reactor`, so its rotated tokens are saved, then frees what `login` owns. It prints a line while it waits (at most 20 s), and Ctrl+C ends the wait with the credentials file unchanged. The reactor must stay alive until this call returns.

## `oi_cli_login_apply_saved` (src/cli_login.h)

Fills `cfg` from the saved credentials. A key already present in `cfg` wins. Saved credentials apply only to the default endpoint: a --host or config file pointing elsewhere must never receive a key saved for another server. A missing file is normal; an unusable one is reported on `err` and ignored.

## `oi_cli_login_before_request` (src/cli_login.h)

An oi_llm_before_request_cb. While an OAuth login is active and the access token is close to expiry, starts a renewal on `reactor` and returns at once; a later call adopts the new token. It returns OI_ERR_AGAIN, so the request waits on the reactor and stays cancellable, only when the token has less than 30 seconds left and a renewal is still running. The renewal holds a file lock, because the server rotates the refresh token and a second process must not reuse the old one; a lock that another process holds means that process is renewing. The renewal advances only while `reactor` runs, so a caller that waits for input between turns must run it too (see the `reactor` member of struct oi_cli_composer).

## `oi_cli_login_run` (src/cli_login.h)

Runs the /login dialog on an idle REPL: provider, then method, then the details that method needs. A success updates the live client at once and saves the credentials. Returns non-OK only for a structural failure; every refusal a user can provoke is printed and leaves the REPL running.

## `login_finishes_a_sent_renewal_when_isig_cannot_be_set` (test/test_cli_login.c)

/login finishes a sent renewal also when the terminal refuses ISIG, so the rotated refresh token reaches the credentials file. The terminal's input descriptor is /dev/null, where tcgetattr fails; the composer still reads the pty, and an Escape there cancels the dialog.
