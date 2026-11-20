# cli_status: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_status.c`, `src/cli_status.h`, `test/test_cli_status.c`.

## `status_writer` (src/cli_status.c)

The report under assembly, plus the scratch every sanitized field reuses. One buffer pair for the whole report rather than one per field: fewer allocations, and exactly one place that frees them.

## `append_number.digits` (src/cli_status.c)

Widest unsigned long long is 20 digits; 32 leaves room to spare.

## `replacement_char` (src/cli_status.c)

U+FFFD, the same replacement the UTF-8 repair pass emits for a byte it cannot show -- reused here rather than inventing a second marker.

## `append_flattened` (src/cli_status.c)

Appends already-sanitized bytes, replacing the only control bytes the sanitize pass deliberately lets through. It preserves '\n' and '\t' because its usual callers render multi-line streamed output; a `Key: value` line must not inherit that, or an attacker-supplied value could forge a whole extra status line (or shift one into another column).

Written against the full C0 range plus DEL rather than just those two, so it stays correct if the sanitize pass' own passthrough set ever widens.

## `append_untrusted` (src/cli_status.c)

Appends one untrusted borrowed string: UTF-8 repair, then the shared escape/control stripper, then line flattening. Bounded first, so a pathological value (a forged model name in a tampered log) cannot produce an unbounded line; the bound is on input bytes and the marker is added only when something was actually dropped.

Truncation cuts raw bytes, which can split a multi-byte sequence -- the UTF-8 repair pass then turns that tail into a single U+FFFD, which is exactly what it is for. There is deliberately no attempt to find a character boundary here.

## `model_origin_text` (src/cli_status.c)

NULL means "say nothing about provenance", which is the only honest rendering of an origin nobody tracked.

## `oi_cli_status_snapshot_init` (src/cli_status.c)

The zero value is already "unknown" for every enum and every borrowed pointer here; only the deadlines need saying, since 0 is a real state for them (disabled). Still routed through this function rather than left to the caller's memset, so there is one place to update when a field with a meaningful zero is added.

## `begin_line` (src/cli_status.c)

Every line goes through this pair, so no field can silently skip its own failure check and every key is spelled once.

## `append_endpoint` (src/cli_status.c)

host/port/path plus TLS on/off, assembled here rather than as a URL: a URL invites a userinfo component, and this line must never be able to carry a credential.

## `append_conversation` (src/cli_status.c)

A fixed vocabulary from oi/status.h, not borrowed text.

## `append_queue` (src/cli_status.c)

Reports the kind and size of what is queued, never its text. A message queued mid-turn is ordinary user input, and /status is exactly the sort of thing someone runs with a shoulder-surfer or a shared screen recording present.

## `append_checkpoint` (src/cli_status.c)

Live-only: a session with no durable storage still has its active context spliced by /compact, and saying "none" would be wrong.

## `append_line` (src/cli_status.c)

Its own line rather than a qualifier on the one above: "is my context compacted" is the question a user actually has, and it must be answerable without knowing whether a checkpoint got persisted.

## `file scope` (src/cli_status.h)

The typed snapshot behind /status.

Why a snapshot rather than a set of accessors reached from command dispatch: /status has to report facts owned by four different modules (the REPL's queue, the conversation's turn state, cli.c's endpoint and durable-session state, the replay state's checkpoint range), and cli_command_dispatch has direct access to none of them by design. Each owner fills in its own fields, the assembler hands the finished struct to oi_cli_status_write, and the renderer never dereferences a private struct or asks anyone a follow-up question.

Ownership and lifetime: every pointer here is *borrowed*. The snapshot owns nothing and frees nothing, and every string it points at must stay valid until rendering finishes -- which is what oi_cli_command_status_cb promises by keeping them valid until the next call through the same callback.

That is strictly longer than an assembler's own call. A snapshot is filled in by a callback and rendered after that callback has returned, so pointing a field at storage local to the assembler -- a buffer in its own stack frame, say -- leaves a dangling pointer for the renderer to read. Scratch for a derived value (a working directory read at assembly time is the one that needs it) belongs in the callback's own context object, which outlives the call; see repl_status_context in cli_repl.c.

A borrowed string that is unavailable is NULL, never a placeholder or an invalid pointer, and oi_cli_status_write renders every NULL/unknown case explicitly.

Every field has an explicit "nobody reported this" value, established by oi_cli_status_snapshot_init and *not* by zeroing: a zeroed struct would otherwise claim a pile of real states (no session, ask, disabled deadlines, idle, empty queue) that no assembler ever asserted. Assemblers must start from that initializer, so a partially-filled snapshot reports what is actually unavailable instead of a plausible-looking default.

Secrets are structurally absent: there is no field for an API key, an authorization header, a CA file, or a request body, so no assembler can pass one in and no renderer can print one. Queued input is reported as a kind and a byte count, never as content.

Borrowed strings are untrusted. A model name can come from a tampered session log, a working directory can contain any byte a filesystem accepts, and a host or session id is whatever the command line said. The renderer therefore repairs and sanitizes each of them rather than trusting the assembler to have done it -- see oi_cli_status_write.

## `oi_cli_status_session_state` (src/cli_status.h)

An automatic session that will be created on the first message.

No durable session at all this run: nothing is being persisted. The `oi` CLI never reaches this -- bare interactive startup always creates an automatic session -- but an embedder that runs the REPL on its own arena with no session hook does, and telling it "not created" would promise a session that is never coming.

## `oi_cli_status_permission` (src/cli_status.h)

The permission policy as /status reports it. A separate type from oi_cli_tool_policy on purpose: that one gates whether a tool may run, so every value it has must be a real decision and it must never grow an "unknown". Reporting does need one. Convert with oi_cli_status_permission_from_policy.

## `oi_cli_status_endpoint` (src/cli_status.h)

host/port/path and TLS on/off, and deliberately nothing else.

## `oi_cli_status_endpoint.host` (src/cli_status.h)

NULL when unknown, which makes the whole endpoint unknown: a port and a TLS flag say nothing on their own, so the renderer never reports them without a host.

## `oi_cli_status_checkpoint.known` (src/cli_status.h)

Somebody reported checkpoint state. Cleared by oi_cli_status_snapshot_init, since "no checkpoint" and "nobody looked" are different answers and only the assembler can tell them apart. Every other field here is meaningless while this is 0.

## `oi_cli_status_checkpoint.has_durable_checkpoint` (src/cli_status.h)

A checkpoint exists in durable history. The range is meaningful only then, and is expressed in record ids, matching what the checkpoint record itself stores.

## `oi_cli_status_checkpoint.applied_this_run` (src/cli_status.h)

/compact applied a checkpoint to the live conversation this run. True without has_durable_checkpoint only for a session with no durable storage, where /compact still splices active context.

## `oi_cli_status_checkpoint.context_compacted` (src/cli_status.h)

Active context currently begins with a checkpoint summary rather than the session's oldest real message.

## `oi_cli_status_snapshot.session_id` (src/cli_status.h)

Borrowed; non-NULL only for ACTIVE/FAILED.

## `oi_cli_status_snapshot.request_timeout_ms` (src/cli_status.h)

Milliseconds. 0 means the deadline is disabled (matching oi_llm_config.timeout_ms); negative means unknown, which is what oi_cli_status_snapshot_init leaves behind.

## `oi_cli_status_snapshot.conversation_status` (src/cli_status.h)

The conversation's status when `conversation` is OI_CLI_CONVERSATION_ACTIVITY_FAILED; ignored otherwise.

## `oi_cli_status_snapshot.steering` (src/cli_status.h)

A queued item has asked the active turn to stop at its next safe boundary. Reported only when set, so unlike the fields above this one needs no unknown: saying nothing is already the right answer when nobody knows.

## `oi_cli_status_snapshot.queue_bytes` (src/cli_status.h)

Length of the queued text. The text itself is deliberately not part of this struct: see the header comment.

## `oi_cli_status_snapshot_init` (src/cli_status.h)

Establishes the "nobody reported anything" state: every field explicitly unknown, every borrowed pointer NULL, both deadlines negative. This is the only correct starting point for an assembler, and the only correct thing for a caller to hand a callback that might fill in nothing at all -- a plain memset would instead assert a handful of real states.

## `oi_cli_status_write` (src/cli_status.h)

Renders `snapshot` to `out` as one `Key: value` line per field, in a fixed order, and flushes. The output carries no terminal control sequences and no styling, so it reads the same on a TTY and in a redirected stream, and is stable enough to assert on.

Every borrowed string is repaired to well-formed UTF-8 (malformed bytes become U+FFFD), stripped of escape sequences and control bytes by the shared cli_render_sanitize pass, and then flattened so the two control bytes that pass deliberately preserves ('\n' and '\t') cannot forge an extra line or a column. Ordinary UTF-8 text survives unchanged. A value longer than OI_CLI_STATUS_MAX_FIELD_BYTES is truncated with a trailing "..." so no single field can produce an unbounded line.

The report is assembled in memory and written with one fwrite, so an allocation failure writes nothing at all rather than half a report.

OI_ERR_INVAL if either argument is NULL, OI_ERR_NOMEM if the report cannot be assembled, OI_ERR_IO if the write or the flush fails.

## `OI_CLI_STATUS_MAX_FIELD_BYTES` (src/cli_status.h)

Bound on the input bytes of one borrowed string. Matches OI_CLI_HISTORY_MAX_SETTING_VALUE, the ceiling a durable model or working directory is already stored under, so no legitimate value is ever truncated.

## `render` (test/test_cli_status.c)

Renders `snapshot` into a fresh temporary stream and returns the text. The report never contains a NUL byte, so reading it back as a C string loses nothing -- the sanitize pass strips NUL along with every other C0 control.

## `init_active` (test/test_cli_status.c)

A fully-populated snapshot, so each test can vary one field and leave the rest realistic rather than unknown. Built on the real initializer, which is what every assembler must do.

## `output_is_deterministic_and_ordered` (test/test_cli_status.c)

The order is part of the contract: a redirected /status is something scripts and tests read, and reordering the report would break them silently. Asserted as one exact string rather than field by field.

## `initialized_snapshot_reports_everything_as_unknown` (test/test_cli_status.c)

What dispatch hands a callback that fills nothing in. Every field must read as explicitly unknown -- not as a guessed default, and not as a dereferenced NULL. This is the whole reason the initializer exists rather than a memset: a zeroed struct would claim no session, ask, disabled deadlines, idle, and an empty queue, none of which anyone asserted.

## `queue_states_report_size_without_content` (test/test_cli_status.c)

The queue reports a kind and a size. Content is structurally absent from the snapshot, so this asserts what a caller can learn -- not that one particular string was withheld.

## `render_with_untrusted` (test/test_cli_status.c)

The renderer's terminal-safety contract holds for values it cannot vet: a model name from a tampered session log, a working directory containing whatever a filesystem accepted, a session id straight off the command line. Each case below puts the hostile value in every untrusted field at once and then asserts the two properties that matter -- the report is still exactly its contracted number of lines, and not one raw control byte or escape sequence reached the stream.

## `newlines_and_carriage_returns_cannot_forge_a_line` (test/test_cli_status.c)

The forged key survives as *text inside a value*, but it cannot start a line of its own: the line count is unchanged, and no line begins with the forged key.

## `tabs_and_other_control_bytes_are_replaced.text` (test/test_cli_status.c)

A tab would shift a value into another column; the C1 range encoded as UTF-8 carries an 8-bit CSI. Both are removed or replaced, never passed.

## `invalid_utf8_becomes_replacement_characters.text` (test/test_cli_status.c)

A lone continuation byte, a truncated three-byte lead, an overlong encoding, and a surrogate -- each becomes U+FFFD rather than reaching the terminal as an undecodable byte.

## `report_is_exactly_the_contracted_lines` (test/test_cli_status.c)

Secrets are kept out of /status structurally -- the snapshot simply has no field for one -- so the thing worth asserting here is the inventory: the report is exactly the eleven contracted lines, with nothing extra that a later change could have quietly started emitting. (That a real API key never reaches the terminal is asserted end to end against the actual CLI, with a sentinel key, in test_cli.c.)
