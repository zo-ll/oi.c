# Public API reference

This file holds the contract of each function and type in `include/oi/`. The headers do not have comments. Read this file with the header.

The text came from the header comments. Each entry names the symbol that it applies to.

## `oi/status.h`

### `oi_status`

Status codes returned by fallible oi_* functions. 0 is always success; callers that only care about success/failure can test `!= OI_OK`.

### `OI_ERR_IO`

I/O error, see errno at the call site

### `OI_ERR_DENIED`

refused by caller-supplied policy, not a fault

### `oi_status_str`

Human-readable text for an oi_status, for diagnostic messages only -- not part of any wire format.

## `oi/arena.h`

### `oi_arena`

Bump allocator over growable blocks, one arena per session. Allocations within a turn are never individually freed; oi_arena_reset bulk-frees everything allocated since the last reset (or creation) in one step, amortizing away malloc/free overhead on the hot path.

Not thread-safe: an arena must be used from one thread only, matching the reactor's single-threaded model.

### `oi_arena_create`

`block_size` is the size of each underlying block the arena grows by; 0 selects a reasonable default. An allocation larger than one block gets a block of its own. Returns NULL on allocation failure or invalid args.

### `oi_arena_destroy`

NULL-safe. Frees every block owned by the arena.

### `oi_arena_alloc`

Returns a pointer to `size` bytes, aligned to at least `alignof(max_align_t)`, valid until the next oi_arena_reset or oi_arena_destroy. Returns NULL if `size` is 0 or a new block is needed and allocation fails.

### `oi_arena_reset`

Invalidates every pointer previously returned by oi_arena_alloc on `a` and makes that memory available for reuse. Retains (does not free) the arena's regular blocks, so a reset-then-reuse cycle does not re-hit malloc; the blocks of allocations larger than block_size are freed.

### `oi_arena_used`

Total bytes currently handed out via oi_arena_alloc since the last reset. Useful for tests/diagnostics, not required for correct use.

## `oi/json.h`

### `oi_json_parser`

Dependency-free JSON parser and serializer.

The parser is a true incremental push-parser: oi_json_parser_feed may be called with any number of bytes at a time -- including one byte at a time, split in the middle of a string escape, a \uXXXX surrogate pair, or a number literal -- and behavior is identical regardless of how the input is chunked. This is required for parsing SSE/chunked LLM API responses off a non-blocking socket.

Parsed values are allocated on a caller-supplied oi_arena and are valid until that arena is reset or destroyed; the parser itself owns no caller-visible heap allocations beyond its own bookkeeping.

### `oi_json_parser_create`

`arena` is borrowed: the parser does not destroy it. Returns NULL on allocation failure or if `arena` is NULL.

### `oi_json_parser_destroy`

NULL-safe. Does not touch the arena.

### `oi_json_parser_feed`

Feeds `len` bytes of input. Consumes bytes up through the end of the current top-level value; whitespace after that is ignored, but any other byte fed after the value is already complete is a parse error. Returns OI_ERR_PARSE on malformed input, or immediately if the parser is already in an error state -- errors are sticky until oi_json_parser_reset. Returns OI_ERR_NOMEM if the arena or an internal scratch buffer cannot grow.

### `oi_json_parser_finish`

JSON numbers have no explicit terminator, so a bare top-level number (e.g. "42") stays ambiguous -- and oi_json_parser_done stays false -- until either a following byte disambiguates it or the caller calls this to confirm no more input is coming. A no-op if already done; returns OI_ERR_PARSE if the input so far is incomplete for any other reason (open string, unclosed container, or nothing parsed yet).

### `oi_json_parser_root`

Returns the completed root value (arena-owned), or NULL if parsing isn't done yet. Does not reset the parser -- call oi_json_parser_reset before feeding the next value.

### `oi_json_parser_reset`

Clears parse state (done/failed/partial-token buffers) so the parser can parse another top-level value. Does not touch the arena, so previously parsed values remain valid until the arena itself is reset.

### `oi_json_type_of`

`v` must be non-NULL (a programming-error precondition, not a runtime one -- values that might be absent come from oi_json_array_get / oi_json_object_get, which already return NULL for that case).

### `oi_json_get_bool`

The oi_json_get_* functions below return OI_ERR_INVAL if `v` is NULL or not of the expected type.

### `oi_json_get_string`

`*out_ptr` points into arena memory and is valid for `*out_len` bytes. A trailing NUL byte is appended as a convenience for strings with no embedded NUL byte, but `*out_len` is authoritative -- a JSON string may legally contain an embedded NUL via a \u0000 escape.

### `oi_json_array_get`

Returns NULL if `v` is not an array or `index` is out of range.

### `oi_json_object_get`

Linear scan by NUL-terminated key. Returns NULL if not found or `v` is not an object.

### `oi_json_writer`

A minimal streaming writer for outgoing requests/tool results. Tracks container nesting to insert commas correctly and reject structurally invalid call sequences (e.g. a bare value after the top-level value is already complete).

### `oi_json_write_object_key`

Must be called in "expecting a member" position, immediately followed by exactly one value-writing call for that member.

### `oi_json_writer_data`

Returns the NUL-terminated serialized bytes written so far and, if `out_len` is non-NULL, their length excluding the NUL. Valid until the next write call or oi_json_writer_destroy.

## `oi/reactor.h`

### `oi_reactor`

Single-threaded event loop multiplexing non-blocking fds (LLM API sockets, tool subprocess pipes). No locking: every oi_reactor and the fds registered on it must be used from one thread only.

### `OI_EV_ERROR`, `OI_EV_HUP`

Delivered when an interest fires. `revents` is the subset of the fd's registered interest that is ready, plus OI_EV_ERROR/OI_EV_HUP which are always reported regardless of registered interest.

### `oi_reactor_cb`

`revents` is the OI_EV_* bitmask describing what fired. `user_data` is the pointer passed to oi_reactor_add. The callback does not own `fd`; closing it is the caller's responsibility (typically after calling oi_reactor_remove).

### `oi_reactor_destroy`

NULL-safe. Does not close any fds still registered.

### `oi_reactor_add`

Registers `fd` for the given OI_EV_READ/OI_EV_WRITE interest. `fd` must not already be registered on `r` (OI_ERR_EXISTS). The fd itself is not duplicated or owned by the reactor; the caller must keep it open and valid until oi_reactor_remove.

### `oi_reactor_remove`

Deregisters `fd`. Safe to call from within that fd's own callback.

### `oi_reactor_step`

Blocks waiting for I/O (or `timeout_ms`, -1 for indefinite) and dispatches ready callbacks once. Returns the number of callbacks dispatched (>= 0), or a negative-mapped error via *out_status. Safe to call oi_reactor_add/remove/modify from within a callback invoked during this step.

### `oi_reactor_run`

Steps until oi_reactor_stop is called or no fds remain registered. Returns OI_OK on a clean stop, or the first step error encountered.

### `oi_reactor_fd`

For a caller that waits in its own poll() and must not stop the reactor's work meanwhile: oi_reactor_fd is readable when a registered fd or child watch is ready, and oi_reactor_timeout_ms is the time until the next timer is due (0 when one is due now, -1 with no timer). When either says so, call oi_reactor_step(r, 0, ...).

### `oi_reactor_timer_start`

Registers a one-shot monotonic timer. After it fires, the callback runs once and the timer stops consuming reactor interest, but its handle remains caller-owned until oi_reactor_timer_cancel. The callback may cancel its own timer. `timeout_ms` must be positive.

### `oi_reactor_timer_cancel`

Cancels and frees a timer, whether still pending or already fired. NULL-safe. Must be called before destroying its reactor.

## `oi/config.h`

### `oi_config`

Config resolution: built-in defaults, an optional key=value file for non-secret settings, the environment for secrets, and a generic setter any override source (a config file line, a CLI flag) can call through. Deliberately doesn't parse argv itself -- flag *syntax* is the CLI binary's concern; this module only owns turning a resolved (key, value) pair into the right struct field, so both the file parser here and the CLI's flag loop there share one place that knows the field set and how to validate/convert each one.

Precedence (highest to lowest) is achieved by call order, not by this module tracking provenance: apply defaults, then the file and env (a config file never sets secrets and the environment never sets anything else, so those two don't compete), then CLI overrides last.

### `oi_config.api_key`

secret: environment or CLI only, never the file

### `oi_config.ca_file`

NULL = system default trust store

### `oi_config.timeout_ms`

positive end-to-end LLM request deadline

### `oi_config_init_defaults`

Fills in built-in defaults, overwriting any existing content -- call on a zeroed or freshly-allocated struct, not one with prior values that need freeing first (use oi_config_free for that). Every string field is heap-allocated (including these defaults), so oi_config_free can unconditionally free all of them regardless of source.

OI_ERR_NOMEM leaves `cfg` partially filled in; free it either way.

### `oi_config_set`

Sets one named field from a string value, parsing ints/bools as needed. Recognized keys: "api_key", "host", "port", "use_tls", "ca_file", "path", "model", "timeout_ms". Replaces (freeing the old one) any existing value for string-typed keys.

OI_ERR_NOTFOUND for an unrecognized key. OI_ERR_PARSE if `value` doesn't parse for that key's type or range ("port" is 1..65535, "timeout_ms" is positive, "use_tls" needs true/false/1/0/yes/no, case-insensitive).

### `oi_config_load_env`

Reads OI_API_KEY from the environment into cfg->api_key. A no-op (OI_OK) if the variable isn't set.

### `oi_config_load_file`

Parses a `key = value` config file, one setting per line (`#` comments and blank lines ignored), calling oi_config_set for each. The "api_key" key is forbidden here (OI_ERR_DENIED) -- secrets belong in the environment or a CLI flag, not a file that might get committed to version control.

OI_ERR_NOTFOUND if the file doesn't exist -- an optional config file's absence is for the caller to decide whether that's fine (the common case) or, e.g. when the user explicitly pointed at a path, an error. Any other open() failure (permission denied, ...) is OI_ERR_IO. OI_ERR_PARSE for a malformed line (no `=`) or whatever oi_config_set returns for a bad key/value on a well-formed one.

### `oi_config_free`

Frees every string field inside `cfg` (not `cfg` itself). Safe to call on an already-defaulted, partially-loaded, or zeroed config.

## `oi/sesslog.h`

### `oi_sesslog`

Durable append-only log for session state: one writev() system call per record. Each record carries a CRC-32 of its payload. A short/error write is rolled back before returning; a genuine mid-write process crash is recovered by discarding the partial tail on the next open. No fsync by default: crash-safe against the *process* dying, not against a power loss losing the page cache.

Synchronous, local-disk-only I/O; unlike the reactor-driven modules, this one makes no non-blocking-I/O claim (PLAN.md's "no blocking calls anywhere" is scoped to the LLM/tool I/O paths, not local disk).

### `OI_SESSLOG_MAX_RECORD`

Payload length cap per record; a length prefix claiming more than this during recovery/replay is treated as corruption.

### `oi_sesslog_open`

Opens `path`, creating it (with a fresh versioned header) if it doesn't exist or is empty. If it already has content, validates the header -- OI_ERR_PARSE on a bad magic or an unrecognized version -- and recovers by truncating off a trailing record left incomplete by a prior crash; everything before that point is preserved untouched.

A record with a bad checksum or an impossible length is damage, not a crash tail. Open then copies the whole file to "<path>.corrupt" first, cuts the log before the damaged record, and reports the count of bytes cut through oi_sesslog_damaged_bytes. If the copy fails, open fails with OI_ERR_IO (or OI_ERR_NOMEM) and changes nothing.

Takes an exclusive advisory lock (flock) for the life of the handle, so two processes can't both append to the same log concurrently;

OI_ERR_EXISTS if another process already holds it.

### `oi_sesslog_close`

NULL-safe. Releases the lock and closes the underlying fd.

### `oi_sesslog_append`

Appends one record in a single writev() call. OI_ERR_INVAL if `len` exceeds OI_SESSLOG_MAX_RECORD. OI_ERR_IO leaves the prior valid log contents intact whenever the filesystem permits rollback.

### `oi_sesslog_record_cb`

Replays every record currently in the log, in write order, via `cb`. `data` is borrowed, valid only for the duration of one callback call.

### `oi_sesslog_damaged_bytes`

The count of bytes that oi_sesslog_open cut because of damage; 0 when the log was intact or lost only a crash tail. NULL-safe.

## `oi/session.h`

### `oi_session`

The session management surface: identity (a caller-chosen ID), resources (a per-turn arena, a durable on-disk log), and fault containment. Deliberately does not know how to drive an LLM/tool turn loop or interpret log records -- that depends on wire formats and conversation structure this module has no business knowing about. Sessions are fully independent: no inter-session message-passing or shared task queue.

### `oi_session_create`

Creates (or, if `log_path` already has prior records, resumes) a session identified by `id`. `block_size` is passed through to oi_arena_create (0 for a default size). OI_ERR_EXISTS if `id` is already registered; otherwise whatever oi_sesslog_open returns (OI_ERR_EXISTS if another process holds `log_path`, OI_ERR_PARSE on a corrupt header, ...) if opening the log fails.

Resuming means the log is opened (and recovered/truncated per oi_sesslog_open) so its prior records are there to replay -- call oi_sesslog_replay(oi_session_log(session), ...) afterward to rebuild whatever in-memory state the embedder's conversation format needs from them; this module doesn't do that itself.

### `oi_session_destroy`

Removes `session` from `reg` and destroys its resources (arena, sesslog). The caller must have already torn down anything it built on top -- in-flight LLM requests/tool calls reference the session's arena and don't know to stop on their own just because the session did.

### `oi_session_id`

NULL-safe (returns NULL for a NULL session).

### `oi_session_state_of`

NULL-safe: a NULL session reads as OI_SESSION_FAILED.

### `oi_session_arena`

NULL for a NULL session, and once the session has failed (see oi_session_fail).

### `oi_session_fail`

Fault containment: marks `session` FAILED, destroying its arena and closing its log, without touching the registry, the reactor, or any other session. Idempotent, NULL-safe. This is a reporting hook, not a detector -- callers invoke it once *they* determine a session has hit an unrecoverable error (a malformed API response, an allocator failure, ...) while driving its LLM/tool calls; this module has no visibility into those and can't determine that on its own. The session stays in the registry (still look-up-able, e.g. for diagnostics) until an explicit oi_session_destroy.

## `oi/llm.h`

### `oi_llm_delta_cb`

Fires for each streamed assistant-text delta. May fire zero or more times over the life of a request. `text` is borrowed -- copy it if you need it past the callback's return.

### `oi_llm_event`

One borrowed streaming event. Every pointer is valid only until the callback returns. TOOL_CALL fields are fragments from one indexed call: a field with length zero was absent from this delta. Callers that need a complete call append fragments by `index`; the stream is rejected unless every call finishes with a non-empty id/name and JSON-object arguments. A call whose arguments are present but empty gets one final "{}" fragment.

### `oi_llm_done_cb`

Fires exactly once, terminating the request (oi_llm_request itself is freed right after this returns -- don't touch it from inside). `status` is OI_OK on a clean completion of the stream. Otherwise: OI_ERR_IO for a network/TLS failure, OI_ERR_PARSE for a malformed response, or

OI_ERR_TIMEOUT when the configured request deadline expires. `http_status` is the HTTP status code (0 if a response was never received at all). `error_body`/`error_body_len` hold the raw response body when the server returned a non-2xx status, or the raw event when a 2xx stream reported an error (truncated at 64KiB). With http_status 0, they hold one line of text that names the connection failure, such as a DNS error or an untrusted certificate. NULL/0 on success, and when there is nothing to report.

### `struct oi_llm_config.ca_file`

NULL = system default trust store

### `struct oi_llm_config.api_key`

sent as "Authorization: Bearer <key>"; NULL to omit

### `struct oi_llm_config.timeout_ms`

end-to-end request deadline; 0 disables

### `oi_llm_client_create`

Copies everything out of `cfg`; it need not outlive this call. Returns NULL on allocation failure, an invalid port/deadline, a missing/invalid host or path, or control characters in an API key (request-header injection is rejected at this boundary). The returned client must outlive every request started against it.

### `oi_llm_client_create_responses`

Creates a client for the OpenAI Responses API wire format; `cfg->path` names that route, e.g. "/v1/responses". Callers still pass Chat Completions bodies: "model", "messages", and function "tools" are translated before sending and every other member is dropped. A body that cannot be translated fails the request start with OI_ERR_PARSE. Streams surface through the same callbacks. A terminal failure event reports OI_ERR_IO with that event's JSON as the error body.

### `oi_llm_client_reconfigure`

Replaces a client's endpoint, key, and wire format in place, so that a caller holding the pointer sees the new settings on its next request. No request may be in flight. Returns OI_ERR_INVAL for a configuration oi_llm_client_create would refuse, and then leaves the client unchanged.

### `oi_llm_client_set_keep_alive`

Off by default. When on, a request sends "Connection: keep-alive", and after a complete, length-framed response the client keeps the connection, registered on that request's reactor, for its next request on the same reactor. A kept connection that the server has closed is replaced once, before any response byte. Turning it off closes the kept connection. With keep-alive on, destroy the client (or turn keep-alive off) before the reactor of its last request.

### `oi_llm_before_request_cb`

Runs `hook` at the start of every request, before anything is sent, so that a caller can renew a short-lived credential through oi_llm_client_reconfigure. OI_ERR_AGAIN means "not yet": the request starts, waits on its reactor, and runs the hook again every 20 ms until it returns something else. The request does not read the client until the hook returns OI_OK, so the hook may reconfigure the client also while its request waits; the request deadline and oi_llm_request_cancel apply during the wait. Any other non-OK return fails the request: at once from the start call, or through on_done after a wait. The hook survives a reconfigure; NULL removes it.

### `oi_llm_request_start`

Starts one streaming chat-completion request. `body` is a complete JSON request object (model/messages/tools/etc., with "stream":true already set by the caller) -- it's copied, so it need not outlive this call. `arena` must be non-NULL but receives no allocations: the request parses each streamed event into a private arena that it resets before the next event, so a long stream does not grow memory.

Returns the in-flight request via *out_request; `on_delta`/`on_done` fire later, driven by `reactor`.

### `oi_llm_request_start_events`

Event-capable variant of oi_llm_request_start. Text and structured tool-call fragments are delivered through `on_event`; completion and ownership rules are otherwise identical.

### `oi_llm_request_cancel`

Aborts an in-flight request; on_done will NOT fire. NULL-safe and a no-op if the request already finished. Safe to call from within on_delta; never call it from within on_done (the request is already gone by then).

## `oi/tool.h`

### `oi_tool_registry`

Tool declaration (a static registry of name/schema/argv-builder) and tool execution (every call runs as a child process, stdin/stdout registered as non-blocking fds on the reactor -- no blocking calls anywhere). Permission policy is entirely the embedder's: this module only provides the allow/deny/ask hook and, for "ask", a way to defer and later resolve the decision.

### `oi_tool_build_argv`

Builds the argv (execvp-style, NUL-terminated array of NUL-terminated strings) for one invocation from its parsed JSON arguments. Allocated on `arena`. Returning a non-OK status aborts the call before any process is spawned.

### `oi_tool_registry_add`

`name` and `json_schema` are copied. `json_schema` is the tool's parameter schema as a JSON Schema string -- opaque to this module, exposed only via oi_tool_registry_schema for the caller to embed verbatim when building an LLM request's "tools" field. OI_ERR_EXISTS if `name` is already registered.

### `oi_tool_output_cb`

Fires zero or more times as the child's stdout (and, merged in, stderr) bytes arrive. `data` is borrowed.

### `oi_tool_exit_kind.OI_TOOL_EXIT_NORMAL`

process ran; `code` is its exit status

### `oi_tool_exit_kind.OI_TOOL_EXIT_SIGNALED`

process was killed; `code` is the signal

### `oi_tool_exit_kind.OI_TOOL_EXIT_FAILED`

execvp itself failed; `code` is the errno

### `oi_tool_exit_kind.OI_TOOL_EXIT_TIMEOUT`

configured deadline expired; `code` is zero

### `oi_tool_done_cb`

Fires exactly once, terminating the call (oi_tool_call is freed right after this returns -- don't touch it from inside). Fires only once a process has actually been spawned; everything before that is reported synchronously via oi_tool_call_start/oi_tool_call_resolve's return value instead (see below), with no call created and neither this nor on_output ever firing.

### `oi_tool_call_start`

Starts (or, for an OI_TOOL_ASK decision, stages) one invocation of `tool_name` with `args`. `arena` backs the argv built for it and must outlive the call.

Synchronous outcomes are reported via the return value, with *out_call left untouched and neither callback ever firing:

- `OI_ERR_NOTFOUND`: no such tool registered
- `OI_ERR_DENIED`: permission_cb returned OI_TOOL_DENY
- any other status: build_argv/fork/exec-setup failure (see oi_status)

On OI_OK with an OI_TOOL_ALLOW decision, a process has been spawned; on_output/on_done fire later from the reactor. On OI_OK with an

OI_TOOL_ASK decision, *out_call is populated but nothing has run yet -- call oi_tool_call_resolve to proceed, or oi_tool_call_cancel to abandon it; no callback fires until you do.

### `oi_tool_call_resolve`

Resolves a call left pending by an OI_TOOL_ASK decision. `allow` nonzero proceeds to spawn the process (same synchronous-failure contract as oi_tool_call_start); zero denies it. Either way `call` is invalid after this returns -- on denial it's freed immediately (OI_ERR_DENIED, matching oi_tool_call_start's DENY contract); on a spawn failure it's likewise freed and the error returned; only a successful spawn leaves it alive, now running.

OI_ERR_INVAL if `call` isn't currently awaiting a decision.

### `oi_tool_call_write_stdin`

Queues bytes for the child's stdin; non-blocking, returns once queued. OI_ERR_INVAL if the call isn't a running process (e.g. still awaiting an ASK decision, or already finished).

### `oi_tool_call_close_stdin`

Closes the child's stdin so a read() in the child sees EOF. If bytes queued via oi_tool_call_write_stdin haven't been flushed yet, the close is deferred until they have been -- the child always sees everything written before it sees EOF.

### `oi_tool_call_set_timeout`

Sets a one-shot execution deadline. For an ASK-pending call it starts when permission is granted and the child is spawned; for a running call it starts immediately. May be set only once, with a positive value.

### `oi_tool_call_cancel`

Kills the process (if one is running) and frees `call`; on_done will NOT fire. NULL-safe. Safe to call reentrantly from within this call's own on_output (not from within on_done, which means the call is already gone).
