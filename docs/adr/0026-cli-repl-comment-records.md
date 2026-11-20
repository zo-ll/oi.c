# cli_repl: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_repl.c`, `src/cli_repl.h`.

## `repl_setting_context.model_origin` (src/cli_repl.c)

Live provenance of *current_model, seeded from config->model_origin and updated by whichever thing replaces the model: /model here, and a /session switch adopting the target's own restored value. Kept beside the model rather than in the status snapshot's own state, so the value and the story about where it came from can never be updated separately.

## `oi_cli_repl_pending_kind` (src/cli_repl.c)

The one-slot pending item, owned entirely by cli_repl.c. cli_conversation only ever receives a bare "steer" signal, never queued content, and cli_history only encodes/replays the durable record shape; deciding what is queued belongs to the REPL. Reset to NONE at the top of oi_cli_repl_run's turn loop each time it is consumed or discarded.

## `oi_cli_repl_pending.record_id` (src/cli_repl.c)

Opaque durable record id from persist_queued_input, passed back verbatim to persist_queue_resolved; 0 if persistence is disabled (ephemeral session) or failed benignly under a store==NULL config.

## `repl_status_context` (src/cli_repl.c)

What the /status snapshot is assembled from. Holds no state of its own beyond `cwd` scratch space and one run-local counter, because everything else is already owned somewhere: borrowing keeps a single copy of each fact rather than a second one that can drift out of step with it.

Every member is borrowed and outlives any single dispatch, which is what makes it safe for repl_status_snapshot to hand dispatch pointers straight out of here.

## `repl_status_context.conversation` (src/cli_repl.c)

Points at oi_cli_repl_run's own conversation slot, not at a conversation: it is NULL before the first message and is replaced by a /session switch, and /status must report whichever one is live now.

## `repl_status_context.checkpoints_applied` (src/cli_repl.c)

Checkpoints /compact spliced into active context this run. Reset by a /session switch, which replaces that context wholesale.

## `repl_status_context.cwd` (src/cli_repl.c)

Scratch for the snapshot's borrowed `cwd`. Owned here rather than heap-allocated per call so assembling a snapshot cannot fail for want of memory, and so the borrowed pointer's lifetime is plainly this context's.

## `repl_session_state` (src/cli_repl.c)

Which durable session, if any, this run has. Derived rather than stored: a /session switch can turn a run with no session into one with an active session, and only the live callbacks know that. `id` is the already- resolved current id, passed in rather than fetched again so the state and the id in the same report can never come from two different reads.

No id and no lazy-creation hook means no session will ever exist this run -- distinct from an automatic session that simply has not been created yet, and worth saying so, since nothing is being persisted.

## `repl_queue_state` (src/cli_repl.c)

The REPL always knows its own slot, so there is no unknown to report here: an unoccupied slot is genuinely empty.

## `repl_status_snapshot` (src/cli_repl.c)

Assembles the /status snapshot. Registered as the dispatch context's status callback at both call sites -- the idle prompt and the mid-turn read-only path -- because /status must answer for a turn that is running right now, and everything read here is either an accessor or a borrowed pointer, so nothing about doing it mid-turn is unsafe.

Never fails: an unresolvable working directory is reported as unknown rather than turning /status into an error, since the rest of the report is still accurate and useful.

Every field is assigned, so nothing is left at the unknown the caller's oi_cli_status_snapshot_init established -- the REPL genuinely knows all of this. The one exception is the checkpoint's durable half, which belongs to a nullable callback.

Active context begins with a checkpoint summary if either a durable checkpoint was replayed into it or /compact spliced one in this run. The second case is the only one an ephemeral session can have.

## `repl_turn_signal_context.terminate_signal` (src/cli_repl.c)

0, or the signal (SIGTERM/SIGHUP) that requested the whole REPL terminate once this turn finishes unwinding.

## `repl_turn_signal_context.resize_pending` (src/cli_repl.c)

Set on SIGWINCH; the turn loop checks this after each step to decide whether the composer's frame needs redrawing -- see repl_turn_input_context.dirty for why this can't just redraw unconditionally on every step.

## `repl_turn_input_context.status_context` (src/cli_repl.c)

Shared with the idle path: /status must report the same snapshot whether it was typed at the prompt or during a turn.

## `repl_turn_input_context.escape_timer` (src/cli_repl.c)

Owned by this context: cancelled unconditionally when the turn ends (whichever way), since a still-pending timer firing after this stack-local context goes out of scope would be a use-after-free.

## `repl_turn_input_context.status` (src/cli_repl.c)

First composer error seen (e.g. OI_ERR_CLOSED if input_fd hit EOF); checked by the turn loop after each step alongside the reactor's own step_status, since this callback has no return value of its own.

## `repl_turn_input_context.dirty` (src/cli_repl.c)

Set whenever input was actually decoded this step; the turn loop checks this (and resize_pending above) after each step to decide whether to redraw the composer's frame at all. Redrawing unconditionally on every step -- regardless of whether *this* step's ready fds had anything to do with the composer -- would interleave its escape sequences into the turn's own streamed output (config->out and config->output_fd are the same fd) whenever a multi-chunk reply and typing land in different steps, corrupting normal token-by-token streaming, not just risking flicker. Only redrawing when something composer-relevant actually happened this step avoids that entirely.

## `repl_turn_input_context.awaiting_permission` (src/cli_repl.c)

True from the moment an AWAITING_PERMISSION event arrives until the decision is resolved one way or another -- while set, handle_turn_input/handle_turn_escape_timeout route decoded input through the permission selector instead of the composer's normal editor-applying path, and the turn loop's redraw draws the selector instead of the ordinary prompt frame.

## `repl_turn_input_context.permission_selected` (src/cli_repl.c)

0 = allow once, 1 = allow for process, 2 = deny -- reset to deny each time a new AWAITING_PERMISSION event arrives.

## `repl_turn_input_context.permission_tool_line` (src/cli_repl.c)

The sanitized "Tool: <name>" line and the one-line review hint, rebuilt fresh by build_permission_header for each AWAITING_PERMISSION event -- copied immediately rather than keeping the event's own borrowed pointers, which aren't guaranteed to outlive the call.

## `repl_turn_input_context.panel` (src/cli_repl.c)

Live tool-execution presentation, driven by repl_conversation_event (TOOL_STARTING/_OUTPUT/a tool-role MESSAGE). Fresh (inactive) for each new turn via this struct's own zero-initialization; stays active through the rest of the turn once a tool call starts, so its last state (e.g. "shell: completed") remains visible until either another call starts in the same turn or the turn ends.

## `build_permission_header` (src/cli_repl.c)

Builds the "Tool: <name>" header line shown above the permission selector, sanitized like every other untrusted string on the terminal. The selector redraws these lines, so they stay one row each: the full request goes to scrollback once, through cli_present, before the first draw. Copied because `name` is event-scoped.

## `repl_event_context.turn_input` (src/cli_repl.c)

Set to the current turn's context at the start of each turn, and only ever dereferenced while a turn is genuinely active (conversation events can't fire otherwise) -- see oi_cli_repl_run.

## `tool_panel_status_for_outcome` (src/cli_repl.c)

Maps the conversation's durable-plus-presentation outcome to the live panel's final label. FAILED and DENIED deliberately remain distinct here even though persistence collapses them into the existing completed and not-executed history outcomes respectively.

## `repl_conversation_event` (src/cli_repl.c)

Enter on a fresh prompt must not run anything.

## `permission_selector_key` (src/cli_repl.c)

Applies one decoded event to the permission selector (via oi_cli_selector_apply) instead of the editor -- called through oi_cli_composer_feed_raw/_resolve_escape_raw, which never touch composer->state, so the in-progress draft can't be corrupted by keys being intercepted here. Returns nonzero once the decision is settled (confirmed or cancelled), matching feed_raw's "stop at the first decisive action" contract.

A signal callback earlier in the same reactor batch may already have cancelled the pending call. Stale selector input must not elevate process policy, but it also must not turn that successful cancellation into a new error.

"Allow for process" is itself the explicit confirming act -- distinctly labeled from "allow once" in the selector, so no further confirmation is layered on top here. Apply the elevation only after the staged decision was accepted.

Dismissing the prompt (Escape or Ctrl+C while the selector is up) must never leave the tool call hanging -- resolve as deny, exactly like explicitly selecting "Deny" would.

## `handle_busy_submit` (src/cli_repl.c)

Handles Enter pressed while a turn is active. /help and /status (the only OI_CLI_COMMAND_READ_ONLY commands) dispatch immediately regardless of the pending slot; everything else -- a plain message, or any other command (including /exit) -- goes through the one-slot rule: refused non-destructively if the slot is already occupied (the draft is only ever committed/cleared once the submission is accepted, one way or the other), else committed and durably persisted as QUEUED_INPUT, to be steered into the turn at the next safe boundary.

Read-only by construction (checked above): /help and /status never return EXIT_REPL or mutate model/cwd, so command_result needs no further handling here.

## `oi_cli_conversation_steer` (src/cli_repl.c)

Now that something is queued, no new tool call (or follow-up model round) should start ahead of it -- whatever's already in flight (the model request, or a running tool) still finishes naturally.

## `discard_pending_on_cancel` (src/cli_repl.c)

Ctrl+C during a turn always cancels it (handled by the caller before this runs); this handles the one additional thing that's true only when something is also queued: the pending item must not silently survive to run after a turn the user just asked to stop entirely, so it's discarded (persisted QUEUE_RESOLVED(DISCARDED), matching the schema -- there's no "abandoned" resolution) and handed back verbatim as the live draft, rather than dropped, since Ctrl+C is "stop", not "delete what I typed".

## `handle_turn_input` (src/cli_repl.c)

Keys are intercepted by the selector entirely -- never reach the editor -- for as long as a decision is outstanding.

## `dispatch_set_model` (src/cli_repl.c)

Set with the value, not after the whole change lands: the name is already the active one at this point, so leaving the provenance behind would make /status describe a model that is no longer there.

## `dispatch_set_cwd` (src/cli_repl.c)

Nothing was durably written yet: roll the live chdir back rather than leaving live and durable state disagreeing. A failed rollback has no further fallback -- `status` (the persist failure) is still the right thing to report.

## `repl_session_switch_outcome.terminate` (src/cli_repl.c)

a terminating signal arrived during a selector

## `repl_session_switch_outcome.switched` (src/cli_repl.c)

Set only once the switch has committed: the caller's run-local reporting state describes the session that was just replaced, so it has to be told, and only the committed path knows the difference.

## `repl_confirm` (src/cli_repl.c)

Runs a two-option confirmation selector and reports the choice.

A cancel counts as choosing the safe first option, per oi_cli_composer_select's contract. Erases the selector frame afterwards so the ordinary idle prompt is what remains on screen.

## `repl_print_replay` (src/cli_repl.c)

Dumps a restored conversation so the user can see what they switched into.

A deliberately plain role-tagged dump rather than the streaming Markdown renderer: that renderer is built around incremental state for a turn that is actively arriving, and driving it over a static list would mean fabricating turn boundaries it does not have. Legibility here is worth more than styling.

## `repl_handle_session_switch` (src/cli_repl.c)

Swaps the live conversation over to another session.

The ordering is the whole point. The conversation is destroyed *before* switch_session is called, because a successful switch destroys the old session's arena and the conversation allocates from it. That means this function is briefly holding no conversation at all, so every path out of here must either build a new one or rebuild the old one -- which is why the current messages are cloned up front, before anything is torn down.

Returns non-OK only for a structural failure the REPL should end on.

Clone the live messages before anything is destroyed: this is the only way back if the switch fails.

There may be no conversation yet -- an automatic session is created lazily on the first message, so switching before then is legitimate and has nothing to tear down or restore. `had_conversation` records which case this is, because on the failure path an absent conversation must be left absent: `*active_arena` is still NULL until the first one is built, so there would be nothing to rebuild it on.

Offer the restored conversation, since a bare prompt gives no hint of what was just loaded.

## `repl_handle_session_switch.message_status` (src/cli_repl.c)

Nothing durable changed and the old arena is untouched, so rebuild the conversation exactly as it was. A conversation holds no state across destroy/create beyond the arena and the message list, both of which are still here.

## `oi_cli_message_list_free` (src/cli_repl.c)

Committed. Adopt the new session's arena, model, context, and input history, then build the conversation on top of them.

## `oi_cli_input_history_free` (src/cli_repl.c)

Input history is per-session: rebuild it from the user messages of the session just opened.

## `repl_handle_session_import` (src/cli_repl.c)

The remainder is a path taken verbatim: it may contain spaces, so it is never tokenized further.

## `oi_cli_repl_run.active_arena` (src/cli_repl.c)

The arena the live conversation allocates from. Loop-scoped rather than block-local to the lazy-create path, because /session switch replaces it: a successful switch destroys the old session's arena and adopts the new session's, and a failed switch has to rebuild the conversation on the unchanged old one.

## `oi_cli_repl_run.active_context` (src/cli_repl.c)

The context the conversation is (re)built from. Starts as the caller's startup context and is replaced by a switch, so the lazy-create path below and the switch path agree on what "current context" means.

## `oi_cli_repl_run` (src/cli_repl.c)

Idle waits run the turn reactor too, so a background token renewal finishes and releases its lock while the prompt waits for input.

Graceful degradation, never a hard failure: if blocking the signals or creating the signalfd fails for any reason, signal_fd stays -1, which oi_cli_composer_wait_submit already treats as "signal handling unavailable" -- the REPL still works exactly as it does without this feature (just without live resize, and without Ctrl+C/SIGTERM/SIGHUP being caught cleanly).

conversation_config.model is set fresh just before each oi_cli_conversation_create call below (not here): a /model command before the first message reallocates current_model->data, and capturing the pointer this early would leave it dangling.

No turn is active at this point in the loop (we're between turns, about to read the next prompt), so a terminate signal here needs no conversation cancellation -- just end cleanly, exactly like Ctrl+D.

/compact never reaches cli_command_dispatch.c: dispatch has no access to the conversation, durable history store, or LLM client by design, and this command genuinely needs all three. Handled entirely here, at the same idle/safe-boundary label a freshly-typed and a dequeued pending command both funnel through -- matching the /permissions allow gate right below.

OI_CLI_COMPACT_OK: durable append must fully succeed before anything live changes, so a failure here leaves both the durable log and the live conversation completely untouched -- see cli.c's persist_checkpoint.

Allocation-free ownership transfer: compact_result already owns the summary buffer, so after the durable append there is no resource-exhaustion failure that can leave durable and live state out of lock-step.

Counted only after the live splice succeeded, so /status cannot claim a compacted context that was never spliced. This is what lets a session with no durable storage report its compaction at all -- there is no record to read it back from.

/session switch, delete, and import cannot go through cli_command_dispatch: switch destroys and rebuilds the live conversation, and delete and import need the composer's confirmation flow. Handled here at the same idle/safe-boundary label everything else funnels through, so a dequeued pending command behaves identically to a freshly-typed one.

/permissions allow must not silently elevate the process- wide tool policy. Gated here rather than in cli_command_dispatch.c (which has no composer/terminal access by design and should not gain any) or in dispatch_permissions itself, so this one insertion covers both a freshly-typed command and a dequeued pending one -- the one-slot queue funnels through this same have_parsed_command label. /permissions ask/deny, and a redundant allow when already allowed, skip this gate and dispatch directly.

Only the first-ever creation goes through prepare; after a switch the arena is already the new session's.

Read current_model->data only now: config->prepare (the lazy automatic-session path) may have just resolved a restored/rebuilt value into it.

SIGTERM/SIGHUP: the conversation was already cancelled above; clean up and exit regardless of anything else. Report a clean exit (OI_OK), matching the idle-prompt SIGTERM/SIGHUP path (oi_cli_composer_wait_submit's own terminate_signal out-param leaves its return status OI_OK) rather than leaking the cancellation's own

OI_ERR_CLOSED as if the turn itself had failed.

Ctrl+C during this turn: always recoverable, regardless of whatever oi_status got attached to it -- but only when nothing else went wrong. A genuine REPL-level failure (e.g. discard_pending_on_cancel's own persist call failing) must not be masked as an ordinary cancel; the turn_input_context.status != OI_OK case falls through to the generic status != OI_OK handling below instead, which reports it properly.

## `oi_cli_repl_run.have_message` (src/cli_repl.c)

Already known to be a plain message as of queueing time (any leading "//" already stripped then, too) -- skip straight past command parsing/dispatch instead of re-parsing it, which would wrongly re-trigger command handling for message content that happens to start with a bare "/".

## `oi_cli_render_erase` (src/cli_repl.c)

Clear the composer's frame (if drawn) before the step, then redraw fresh once it settles: the turn's own streamed output and any decoded keystrokes both land on the same fd, and interleaving them without this erase/ redraw bracket would corrupt both writers' row-count assumptions (see oi_cli_render_erase's own doc comment). The redraw itself only happens when this step actually touched the composer (input decoded, or a resize) -- redrawing unconditionally on every step, including ones that were purely model/tool activity, would inject the composer's escape sequences into the *middle* of a multi-chunk streamed reply whenever that reply and some unrelated composer-relevant step happen to land in separate oi_reactor_step calls, corrupting normal token-by-token streaming outright, not just risking flicker. The erase, in contrast, is always safe to run unconditionally: it's a no-op whenever nothing is currently drawn.

## `oi_status` (src/cli_repl.h)

Opens a different session and hands over everything the REPL needs to adopt it.

Hard precondition: the caller must have destroyed the live conversation before invoking this, because a successful switch destroys the old session's arena -- which the conversation was allocating from. Nothing may still reference that arena.

Returns non-OK only for a structural failure. Every refusal a user can provoke arrives as `out_result->outcome`, so a switch that cannot happen never ends the REPL. On any non-OK outcome the implementation has already rolled itself back and the previous session remains open and usable.

Permanently deletes an already-trashed session. Only ever called after the REPL has obtained an explicit confirmation.

Imports a legacy log as a new session, reporting its id. Only ever called after the REPL has obtained an explicit confirmation.

Durable persistence for a /model or /cwd change, implemented by the caller (cli.c owns filesystem paths and metadata; the REPL controller does not). NULL means nothing durable exists yet to persist to (no session created); the live update still applies regardless.

Durable persistence for the one-slot queued-input mechanism, implemented by the caller (cli.c owns the record/turn_id bookkeeping; the REPL controller only ever holds the opaque record id handed back to it). Both nullable, matching persist_model/persist_cwd's existing ephemeral- session skip. persist_queued_input must be called (and must succeed) before the REPL treats a message as durably queued, per the issue's own "persist before acknowledging" requirement; *out_record_id is the id to later pass back to persist_queue_resolved. `consumed` is true if the queued message is becoming the next turn's real user message, false if it's being discarded (refused, cancelled, or a queued command, which the durable schema can only ever resolve as discarded -- see cli_history_replay.c's queue_consumed gate).

Durable persistence for /compact, implemented by the caller (cli.c owns the replay state's context array, the only place a live message index can be translated into real record ids). `prefix_message_count` is a count into the REPL's own live message list (oi_cli_conversation_messages), never a record id itself -- cli.c must read the checkpoint's source range directly off replay_state->context[0..prefix_message_count-1]'s own record_id fields, never infer it arithmetically, since a live message index and a durable record id are not the same number once any prior checkpoint has already collapsed part of the history. Nullable, matching persist_queued_input/persist_queue_resolved's existing ephemeral-session skip -- an ephemeral session has nothing to persist this into, but /compact's live-side effect still applies regardless.

## `int` (src/cli_repl.h)

True only if durable session storage itself has failed (cli.c owns the one authoritative signal for this, persistence.last_error, already checked once at final process exit) -- distinguishes a genuine structural failure the REPL cannot recover from from an ordinary recoverable turn error (timeout, tool denial, model error), which should instead print a message and return to a working prompt. Always non-NULL: an ephemeral session (no --session, no automatic session) never exercises the persistence path at all, so it naturally, permanently reports "not failed".

## `void` (src/cli_repl.h)

Reports the durable checkpoint state behind /status. cli.c owns the replay state this reads, and re-reads it on every call rather than caching: a live /compact, and a /session switch, both replace it. Nullable -- a session with no durable storage has no durable checkpoint, and the REPL then reports only what it applied itself this run.

`*out_checkpoint` arrives zeroed. Fill in only the durable fields; the REPL owns `applied_this_run` and `context_compacted`, since only it knows what it spliced into a live conversation.

## `oi_cli_repl_config.endpoint` (src/cli_repl.h)

The resolved LLM endpoint and request deadline, for /status only -- the REPL never opens a connection itself, and the client it is handed deliberately does not publish what it was configured with. `endpoint` is borrowed and must outlive the run; it carries host/port/path and the TLS flag, and structurally cannot carry a credential. /login updates it in place.

## `oi_cli_repl_config.login` (src/cli_repl.h)

Borrowed; runs /login against the same client and endpoint.

## `oi_cli_repl_config.model_origin` (src/cli_repl.h)

Where `model` came from at startup. The REPL updates its own copy of this as /model and /session switch change the active model.

## `oi_cli_repl_config.pending_model` (src/cli_repl.h)

Shared with the caller's lazy automatic-session preparation, so a /model issued before the first message is honored when the session is eventually created. NULL when no automatic-session concept applies (an explicit --session ID is already fully resolved before the REPL starts).

## `oi_cli_repl_config.initial_draft` (src/cli_repl.h)

Crash-recovery text found by the caller's own replay (an interrupted queue consumption) to seed as the composer's initial draft -- never auto-run, just handed back as an editable line. NULL/0 (the default) means no seeding; the caller is responsible for having already closed out the durable crash-recovery window (QUEUE_RESOLVED) before this runs, since oi_cli_repl_run itself has no replay state to do so.

## `oi_cli_repl_config.session_ops` (src/cli_repl.h)

/session's read-only and low-risk subcommands, passed straight through to cli_command_dispatch. All-NULL leaves /session reporting that it is unavailable rather than misbehaving.

## `oi_cli_repl_config.switch_session` (src/cli_repl.h)

The three subcommands dispatch cannot serve, because they need the live conversation or the composer's confirmation flow. Each is NULL when unavailable.
