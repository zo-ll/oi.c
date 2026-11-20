# cli_conversation: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_conversation.c`, `src/cli_conversation.h`, `test/integration/test_cli_conversation.c`.

## `oi_cli_conversation.assistant_committed` (src/cli_conversation.c)

False from the moment start_model_request begins awaiting a new assistant reply (the very first model round of a turn, or a later round following resolved tool results) until on_llm_done commits one -- lets close_out_incomplete_turn tell whether assistant_message_index already refers to this round's reply (and so whether its tool_calls, if any, might need repairing) or is still a stale index from a previous round.

## `oi_cli_conversation.steering` (src/cli_conversation.c)

Set by oi_cli_conversation_steer, consulted only at the top of start_next_tool's loop body -- unlike cancel, steering never touches request/tool directly; it just changes what happens the next time that loop runs (whether entered fresh from on_llm_done or re-entered from on_tool_done), letting whatever is already running finish naturally.

## `oi_cli_conversation.pending_permission_tool` (src/cli_conversation.c)

Set only between request_tool_call staging a call whose policy decision is OI_TOOL_ASK and oi_cli_conversation_resolve_permission supplying the eventual decision -- mutually exclusive with `tool` (only ever set once a process has actually spawned) and `request` (only ever set while awaiting a model reply): exactly one of the three, or none, is non-NULL at any moment while busy.

## `oi_cli_conversation.cancelling` (src/cli_conversation.c)

Set for the duration of oi_cli_conversation_cancel's own unwind -- from before the partial-response event it emits until finish_turn clears it. Purely for reporting: the events emitted during that window (PARTIAL_ASSISTANT, then each repair MESSAGE) run callbacks that can ask what the conversation is doing, and "streaming" or "running a tool" would both be wrong answers by then, since cancel has already detached whatever was in flight.

## `emit_message.attribute_model` (src/cli_conversation.c)

A repair placeholder isn't attributable to any real model reply -- history_message_is_valid requires a REPAIR-sourced assistant message to carry no model, matching cli_history_repair.c's own append_interruption_marker.

## `repair_dangling_tool_calls` (src/cli_conversation.c)

Restores protocol validity after cancelling a running (or about-to-run) tool: the assistant message committed by on_llm_done already carries the full tool_calls array, but only calls before tool_index have a matching tool-result message. Commits a placeholder for the call at tool_index -- OUTCOME_UNKNOWN (with whatever raw output it had already produced) if `first_call_may_have_started`, else NOT_EXECUTED, matching whether a process was actually spawned for it -- and a NOT_EXECUTED placeholder for each later call that never started. Makes conversation->messages immediately safe to reuse for a next turn without touching the durable-replay repair machinery at all. Captures tool_calls/tool_calls_len once, up front: each commit_tool_text call below can append to and reallocate conversation->messages.items, which would invalidate a pointer held directly into that array, but not the assistant message's own (separately-allocated, ownership-copied) tool_calls array.

is_repair=0: history_message_is_valid requires a REPAIR-sourced tool message to never carry raw output, but this placeholder's whole value over the durable crash-repair equivalent (cli_history_repair.c's append_repair_tool, which always passes NULL/0) is carrying whatever was already buffered -- NORMAL is the source that permits that for an OUTCOME_UNKNOWN tool result.

## `skip_steered_tool_calls` (src/cli_conversation.c)

Steering's analog of repair_dangling_tool_calls, deliberately not a reuse of it: that function's wording ("cancelled while it may have already started") is simply false here. By construction, steering is only ever consulted at the top of start_next_tool's loop -- the call actively running (if any) always finishes normally through the ordinary on_tool_done path first, so every call from tool_index onward here was never started. Unlike a cancelled turn, the assistant reply that produced these tool_calls already completed normally (RESPONSE_DONE already fired), so there is no interrupted-turn bookend to add either -- the caller finishes the turn with OI_OK once this returns OI_OK.

## `close_out_steered_turn` (src/cli_conversation.c)

Steering's own turn-closing bookend -- needed for exactly the reason repair_interrupted_turn's own doc comment gives (durable replay requires an assistant message with no tool_calls to return its phase machine to "expect user" before anything else can validly follow), but with accurate wording: unlike a cancelled/aborted turn, this assistant reply already completed normally (RESPONSE_DONE already fired) -- it's steering's own decision not to make the follow-up model round that would ordinarily supply that bookend, so nothing else ever will. Skipping this (as the original steering design assumed it could, reasoning "the reply already completed, so there's no interrupted-turn bookend to add") leaves the durable log with an unresolved phase the moment all of a round's tool_calls happen to have already resolved normally when steering is consulted -- the very next durable write (e.g. resolving a queued item into its own turn) then fails replay validation.

## `resolve_permission_body` (src/cli_conversation.c)

Today's synchronous ALLOW tail (TOOL_STARTING emit, the reentrant-cancel- during-emit guard, conversation->tool = staged, oi_tool_call_resolve, set_timeout/close_stdin) and today's synchronous DENY handling, unified behind one `allow` bool -- called either immediately (a synchronous OI_TOOL_ALLOW/OI_TOOL_DENY decision) or arbitrarily later, from oi_cli_conversation_resolve_permission, once an OI_TOOL_ASK decision's answer finally arrives. `call` must be freshly derived from conversation->tool_index/assistant_message_index by the caller, never a pointer held across the (potentially indefinite) async gap: committing other messages in between can reallocate conversation->messages.items.

Committed directly (not via repair_dangling_tool_calls) so this specific call gets its own distinct wording -- a denial is not the same thing as a genuine cancel-before-start, which is what repair_dangling_tool_calls' shared text otherwise means. Any later tool_calls in the same round that never got this far are still exactly a "cancelled before it started" case, so bumping tool_index past just this one and letting the ordinary repair path handle the rest (via finish_turn_with_repair below) is correct for them.

## `request_tool_call` (src/cli_conversation.c)

Stages a tool call (already done by the caller) and asks the embedder's policy what to do with it. OI_TOOL_ASK defers the actual decision: conversation->pending_permission_tool holds the staged call, an AWAITING_PERMISSION event is emitted, and this returns without resolving anything -- the turn stays busy until oi_cli_conversation_resolve_permission is called, arbitrarily later, from anywhere. ALLOW/DENY resolve immediately via resolve_permission_body, exactly as before this split.

Reentrant cancel from within the AWAITING_PERMISSION emit above, mirroring the same TOOL_STARTING reentrancy pattern documented in resolve_permission_body -- oi_cli_conversation_cancel's own pending_permission_tool branch already cleared it and cancelled staged.

## `start_next_tool.st` (src/cli_conversation.c)

Consulted here and only here -- entered fresh from on_llm_done (tool_index == 0) or re-entered from on_tool_done (tool_index already past whatever just finished normally) -- so it doesn't matter whether steering was requested mid-stream, mid-tool, or between rounds: any remaining tool_calls this round are skipped, and no new model round starts either (one more round would only produce more tool_calls that get skipped right back here on the next re-entry, at the cost of a wasted request). Either way, close_out_steered_turn's bookend is still required -- skipping some calls doesn't produce it, and when there was nothing left to skip (the round's only tool_call already resolved normally), nothing else would supply it at all.

## `start_model_request` (src/cli_conversation.c)

About to await a new assistant reply (the turn's first model round, or a later one following resolved tool results): nothing commits it until on_llm_done runs, so close_out_incomplete_turn must not treat assistant_message_index as belonging to this round until then.

## `repair_interrupted_turn` (src/cli_conversation.c)

Closes out the turn's own assistant slot after a cancel, whether or not any content had streamed in yet: oi_cli_conversation_start always commits the user message before start_model_request ever runs, so durable replay is already expecting a matching assistant message for this turn the instant a turn becomes busy -- cancelling without ever supplying one leaves replay stuck expecting an assistant message forever (the next turn's user message is then itself invalid). Mirrors cli_history_repair.c's append_interruption_marker, the equivalent repair applied at process-restart replay time for a turn a crash left dangling; marked as a repair (not a real model reply) since replay requires that tag on the assistant message that closes out a turn for which a PARTIAL_ASSISTANT record was already written.

## `close_out_incomplete_turn` (src/cli_conversation.c)

The single place that decides what (if anything) needs repairing before a turn ends any way other than a normal, fully-resolved completion -- cancellation, tool denial, a mid-loop failure, or anything else. Whether this round's assistant message has been committed yet (assistant_committed) and, if so, whether all of its tool_calls already have a matching result (tool_index vs. tool_calls_len) together say exactly how much of the turn's protocol shape is still open; closing it is always repair_dangling_tool_calls (if anything there is still open) followed by repair_interrupted_turn (unconditionally, since neither an uncommitted assistant reply nor a pending tool loop leaves replay back at REPLAY_EXPECT_USER on its own). Returns the first repair-commit failure, if any -- a genuine structural problem that should replace the turn's original status rather than being masked by it, matching how oi_cli_conversation_cancel already treated its own repair failures.

## `finish_turn_with_repair` (src/cli_conversation.c)

Convenience wrapper for the common case: repair, then finish with whichever of the repair failure or the turn's own status is worse (repair failure wins, since it's the more structural problem).

## `oi_cli_conversation_cancel` (src/cli_conversation.c)

Before the first event this unwind emits, so no callback can see a conversation that still claims to be streaming or running a tool.

first_call_may_have_started stays 0: nothing was ever spawned for a call still awaiting a permission decision.

## `oi_cli_conversation_activity` (src/cli_conversation.c)

A completed cancel is idle, not failed, even though it leaves a non-OK last_status behind (OI_ERR_CLOSED): the same reasoning cli_repl.c applies when it treats a cancelled turn as always recoverable. Reporting the status the cancel happened to carry would describe a working prompt as broken.

Checked before the three in-flight kinds: cancel detaches whichever one was set before emitting anything, so by the time a callback can observe this they are all NULL anyway -- ordering it first states the intent rather than relying on that.

## `oi_cli_conversation_resolve_permission` (src/cli_conversation.c)

Freshly derived, not a pointer carried across the async gap since this was staged: committing other messages in between (e.g. a steered sibling call's placeholder) can reallocate conversation->messages.items, but tool_index/assistant_message_index themselves stay valid the whole time nothing has resolved yet.

## `oi_cli_conversation_tool_outcome` (src/cli_conversation.h)

The tool ran, but exited nonzero or abnormally. Durable history still records this as a completed execution; this distinction is for accurate live presentation.

The staged call was explicitly denied. Durable history records this as not executed; live presentation must not call it cancelled.

## `oi_cli_conversation_activity` (src/cli_conversation.h)

Where a turn currently is, as one value rather than a handful of booleans a reader has to combine correctly. Exactly one of the three in-flight kinds applies at a time (see the request/tool/ pending_permission_tool invariant in cli_conversation.c), so this is a faithful enumeration, not a lossy summary.

Nobody asked. Never returned by oi_cli_conversation_activity -- it exists so a reporting struct that carries this enum can distinguish "not filled in" from IDLE, which is a real and common answer. The zero value for exactly that reason.

No turn is active and the last one, if any, completed normally.

A cancel is unwinding the turn. Observable from an event callback during the cancel's own partial-response and repair events.

Busy, but nothing is in flight: between a model round and the tool loop, or committing a message.

No turn is active and the last one ended with a non-OK status.

The current activity, for reporting (/status) rather than control flow -- the existing is_busy/is_steering/last_status predicates remain the ones the REPL steers on. A NULL conversation reads as IDLE, which is what a REPL that has not lazily created one yet genuinely is.

## `oi_cli_conversation_event.is_repair` (src/cli_conversation.h)

True for a placeholder committed by a live cancel repair (cancelled_tool_unknown_text/cancelled_tool_not_executed_text/ interrupted_turn_text), not a real model or tool result -- mirrors OI_CLI_HISTORY_MESSAGE_REPAIR, which durable replay requires on the assistant message that closes out a turn after a PARTIAL_ASSISTANT record was already written for it.

## `oi_cli_conversation_event` (src/cli_conversation.h)

Same borrowed-string shape as tool_starting -- emitted instead of (not in addition to) it when the tool's policy decision is OI_TOOL_ASK, before anything is spawned. The call stays staged and the turn stays busy until oi_cli_conversation_resolve_permission is called; TOOL_STARTING (if the decision is to allow) fires only once that happens.

## `oi_status` (src/cli_conversation.h)

Called synchronously at conversation boundaries. Returning non-OK aborts the active turn. TOOL_STARTING is emitted before process spawn, MESSAGE before the message enters model-visible context.

## `oi_cli_conversation_set_model` (src/cli_conversation.h)

Replaces the conversation's own model copy (validated like oi_cli_conversation_create: non-NULL, non-empty). Affects only the next request this conversation starts; does not touch in-flight state.

## `oi_cli_conversation_start` (src/cli_conversation.h)

Starts one user turn and returns without stepping the reactor.

## `oi_cli_conversation_steer` (src/cli_conversation.h)

Requests that the current turn stop starting anything new (no further tool calls, no further model rounds) as soon as it reaches its own next safe point -- unlike oi_cli_conversation_cancel, this never touches whatever request/tool is already in flight; that's left to finish naturally. Idempotent; a no-op if the conversation isn't busy. Reset at the start of the next oi_cli_conversation_start.

## `oi_cli_conversation_resolve_permission` (src/cli_conversation.h)

Resolves a tool call left staged and awaiting a decision (the most recently emitted event on this conversation was AWAITING_PERMISSION and the turn is still busy). `allow` nonzero proceeds exactly as a synchronous OI_TOOL_ALLOW decision would have (TOOL_STARTING fires next, then the process spawns); zero denies it exactly as a synchronous

OI_TOOL_DENY decision would have (a protocol-valid tool-result message is committed and the turn ends with OI_ERR_DENIED) -- this does not change what a denial means, only how the decision arrives.

OI_ERR_INVAL if the conversation is not currently awaiting a permission decision (never staged one, already resolved, or already cancelled).

## `oi_cli_conversation_was_cancelled` (src/cli_conversation.h)

True only when the most recent finished turn ended via oi_cli_conversation_cancel with its repair (if any) fully succeeding -- i.e. an ordinary, always-recoverable user cancel, distinct from any other cause that might produce the same oi_status (e.g. OI_ERR_CLOSED can also mean a genuinely-closed LLM connection). Reset at the start of the next oi_cli_conversation_start.

## `oi_cli_conversation_arena` (src/cli_conversation.h)

The arena this conversation was created with -- borrowed, valid for as long as the conversation itself is. Lets an embedder (e.g. /compact's one-off summarization request) reuse the exact same arena the conversation's own model requests use, without having to separately track it itself: the arena a lazily-created (automatic-session) conversation actually uses is only known at creation time and is not otherwise retained anywhere outside this object. NULL if `conversation` is NULL.

## `oi_cli_conversation_apply_checkpoint` (src/cli_conversation.h)

Replaces the leading `prefix_count` messages of the live conversation with one synthesized assistant-role message built from `summary` -- a direct structural mirror of apply_checkpoint's splice in cli_history_replay.c, kept in lock-step with it so the live message list and a freshly replayed one can never structurally diverge.

OI_ERR_INVAL if the conversation is busy, `prefix_count` is zero, or `prefix_count` exceeds the current message count. On any failure the conversation's message list is left completely unchanged.

## `oi_cli_conversation_apply_checkpoint_take_summary` (src/cli_conversation.h)

Allocation-free variant for failure-atomic persistence. `*summary` must be a heap-owned, NUL-terminated string. On success ownership moves into the conversation and `*summary` is set to NULL; on failure the caller retains ownership and the conversation is unchanged.

## `event_sink.steer_on_response_done` (test/integration/test_cli_conversation.c)

Reentrantly steers (not cancels) from within the named event, for tests exercising oi_cli_conversation_steer's single check point without needing precise external timing -- mirrors cancel_on_tool_starting's existing reentrancy pattern.

## `event_sink.cancel_on_awaiting_permission` (test/integration/test_cli_conversation.c)

Reentrantly cancels from within the AWAITING_PERMISSION event itself -- mirrors cancel_on_tool_starting's existing reentrancy pattern, for a test exercising a cancel that arrives while a call is staged but not yet resolved, rather than while it's already running.

## `event_sink.activity_probe` (test/integration/test_cli_conversation.c)

What oi_cli_conversation_activity reported at each event, indexed by event type. Sampling from inside the callbacks is the only way to observe the in-flight states at all -- by the time control returns to the test, every turn is at rest. NULL disables sampling.

## `event_sink.cancelling_event_count` (test/integration/test_cli_conversation.c)

Events emitted while the conversation reported CANCELLING -- the cancel unwind's own partial-response and repair events. Counted separately because those events share types with ordinary ones, so a per-type sample cannot distinguish them.

## `collect_event` (test/integration/test_cli_conversation.c)

Sampled before the reentrant cancel/steer hooks below run, so an AWAITING_PERMISSION sample describes the staged call rather than the cancel that this very callback is about to request.

## `start_is_event_driven_and_preserves_context` (test/integration/test_cli_conversation.c)

Nothing has started: the activity of a conversation with no turn is idle, and so is a NULL one -- what a REPL that has not lazily created a conversation yet genuinely has.

The stream's JSON lives in the request's own scratch arena: a turn with no tool call puts nothing in the session arena.

An arriving delta means the request is still open, so the reported activity has to be "streaming" -- and once the turn completes cleanly it has to be idle again.

Committing a message is a busy moment with nothing in flight -- the request has already been released by the time the assistant reply is committed -- so it reports as working rather than as streaming.

## `ask_defers_and_resolve_permission_allow_lets_the_tool_run` (test/integration/test_cli_conversation.c)

A staged call awaiting a decision is its own reported state, and it persists for as long as the decision does -- so /status can report it from outside the callback too.

Resolved genuinely later, from outside the event callback entirely -- not reentrantly -- proving the turn really stayed suspended across an arbitrary gap rather than the decision being available synchronously all along.

Output arriving from the child means the process is genuinely running, which is the one moment "tool running" is the only right answer.

## `ask_defers_and_resolve_permission_deny_produces_a_protocol_valid_result` (test/integration/test_cli_conversation.c)

A resolved, non-pending decision can't be resolved twice.

A denial ends the turn with a real non-OK status and no cancel, which is the shape that must report as failed rather than idle.

## `cancel_while_awaiting_permission_repairs_as_not_executed` (test/integration/test_cli_conversation.c)

A genuine cancel-before-start, not a denial -- resolving it again afterward must be rejected, the same as any other already-settled permission request.

## `cancel_while_streaming_needs_no_repair` (test/integration/test_cli_conversation.c)

Cancel immediately after start, before stepping the reactor at all: nothing async has had a chance to run yet, so this deterministically cancels while conversation->request != NULL and no assistant message has been committed -- no *tool* repair is needed or possible, but the turn still needs an interrupted-turn placeholder assistant message to keep durable replay's user/assistant alternation intact for the next turn's user message.

The unwind is observable: the repair message the cancel commits is emitted while the conversation reports CANCELLING, not while it still claims to be streaming. And once the unwind finishes the conversation is idle, not failed -- despite the OI_ERR_CLOSED the cancel carries, which is exactly the status that must not be reported as a fault.

A clean turn after a cancelled one reports idle for the ordinary reason (OI_OK), not because the cancel flag is still set.

## `cancel_from_within_tool_starting_event` (test/integration/test_cli_conversation.c)

The cancel fired reentrantly from inside the TOOL_STARTING emit, before conversation->tool was ever assigned: the tool must never have actually been spawned (no TOOL_OUTPUT), and the repair must still have happened (NOT_EXECUTED -- nothing was ever running).

## `steer_after_a_tool_completes_skips_the_next_one` (test/integration/test_cli_conversation.c)

Steering fired reentrantly from call-1's own TOOL message commit (before start_next_tool ever re-enters for call-2), so call-1 must have run for real (COMPLETED) while call-2 never started at all.

A steering bookend, not an interrupted-turn one: RESPONSE_DONE already fired for real, unlike a genuine cancel, but durable replay still requires an assistant message with no tool_calls to close out this turn before anything else (e.g. the queued message's own turn) can validly follow.

## `steer_before_any_tool_starts_skips_them_all` (test/integration/test_cli_conversation.c)

Steered before start_next_tool ever ran for the first time (from RESPONSE_DONE, which fires before it): the call never started at all, not even staged.

Steering's own turn-closing bookend -- see steer_after_a_tool_completes_skips_the_next_one's identical check for why durable replay requires this.

## `checkpoint_is_rejected_while_busy` (test/integration/test_cli_conversation.c)

oi_cli_conversation_start sets busy synchronously before returning, so this is checked before the reactor is ever stepped.
