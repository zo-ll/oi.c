# cli_oauth: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_oauth.c`, `src/cli_oauth.h`.

## `oi_cli_oauth_servers` (src/cli_oauth.c)

The refresh token goes to this origin, and any environment (an .envrc in a cloned repository) can set the variable: only a loopback origin replaces the production servers.

## `decode_tokens` (src/cli_oauth.c)

The token endpoint's refusal shape is undocumented: accept a string "error", a nested "error.code", and a top-level "code".

## `void` (src/cli_oauth.c)

Runs once, when the call ends. NULL for the blocking call, which reads `done` from its own loop.

## `http_finish` (src/cli_oauth.c)

The connection callbacks never touch `call` after this, so `finished` may free it.

## `http_on_body` (src/cli_oauth.c)

Runs inside the parser, which must outlive this call: http_on_data ends the call after the feed returns.

## `oi_cli_oauth_refresh_call.http` (src/cli_oauth.c)

first: http_finish receives its address

## `oi_cli_oauth_refresh_call.request` (src/cli_oauth.c)

holds the refresh token; wiped before free

## `OI_CLI_OAUTH_REDIRECT_PORT` (src/cli_oauth.h)

"Sign in with ChatGPT" for open-source clients: authorization code with PKCE and a loopback redirect. Tokens are bound to the Responses API.

## `oi_cli_oauth_servers` (src/cli_oauth.h)

The production servers, unless OI_CHATGPT_TEST_BASE names one http(s) loopback origin that replaces both; that variable exists only for tests, and any other host in it is ignored. `out_is_test` reports the override, which also disables opening a browser.

## `oi_cli_oauth_parse_tokens` (src/cli_oauth.h)

On OI_OK, either `access_token` or `error` is set. OI_ERR_PARSE means the body was neither a token set nor a recognizable refusal.

## `oi_cli_oauth_error_is_terminal` (src/cli_oauth.h)

True for the refusals that mean the saved refresh token can never work.

## `oi_cli_oauth_check_id_token` (src/cli_oauth.h)

Checks the claims of an ID token received directly from the token endpoint over TLS: nonce (when `nonce` is non-NULL), audience, expiry, and subject. Does not verify the JWKS signature. OI_ERR_DENIED means a claim did not match. `*out_email` is NULL when the token has none.

## `oi_cli_oauth_http` (src/cli_oauth.h)

One blocking HTTP exchange on a private reactor, so that it is safe from inside another reactor's callback. `bearer` and `form` are optional; a `form` body makes the Content-Type form-encoded. Non-2xx responses still return OI_OK with *out_http_status set. The caller frees *out_body.

## `oi_cli_oauth_refresh.oi_cli_oauth_refresh_call` (src/cli_oauth.h)

oi_cli_oauth_refresh without blocking: the exchange runs on `reactor`, and `done` fires once with the result. `done` owns `*tokens` (free them with oi_cli_oauth_tokens_free; all members are NULL unless the status is OI_OK), and the call handle is already freed when `done` runs. The exchange times out after 20 seconds. oi_cli_oauth_refresh_cancel stops it without calling `done`; NULL-safe.

## `oi_cli_oauth_refresh_sent` (src/cli_oauth.h)

True once the refresh request is queued on the connection. From then on the server can rotate the refresh token, so a cancel can lose the only copy of the new one: let such a call finish instead.
