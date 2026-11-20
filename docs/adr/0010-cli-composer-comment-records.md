# cli_composer: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_composer.c`, `src/cli_composer.h`, `test/test_cli_composer.c`.

## `_GNU_SOURCE` (src/cli_composer.c)

struct signalfd_siginfo is a GNU/Linux extension; needs _GNU_SOURCE defined before any header pulls in its own feature-test macros, matching the precedent in reactor_epoll.c for other GNU fd primitives (timerfd, pidfd).

## `oi_cli_composer_signals` (src/cli_composer.c)

Drains every currently-readable signalfd record, classifying what was seen. Each of SIGWINCH/SIGINT/SIGTERM/SIGHUP is a standard (non-realtime) signal, so at most one of each is ever actually pending -- looping here is defensive, not load-bearing. SIGTERM/SIGHUP win over a same-drain SIGWINCH: there is no point redrawing a frame the caller is about to tear down.

## `composer_poll` (src/cli_composer.c)

poll() on the caller's descriptors that also runs `composer->reactor`. It returns like poll() for the caller's descriptors only: reactor work alone runs and the wait goes on, until one of them is ready or `timeout_ms` has passed. Never fails with EINTR.

## `oi_cli_composer_wait_submit.pending` (src/cli_composer.c)

The draw above already used a freshly-read width; discard any resize notification queued before this call started so it doesn't trigger an immediate, redundant second redraw. A terminate signal queued in that same window must not be lost, though -- honor it immediately instead.

## `oi_cli_composer_wait_submit` (src/cli_composer.c)

A lone buffered byte that's a valid but incomplete sequence (a standalone Escape keypress with nothing following yet, or the first byte of one) breaks the loop above with OI_ERR_AGAIN -- that's an expected "nothing more to decode without further bytes" signal, not a real error, and must not be mistaken for one here: the whole point of falling through to the poll() below is to wait for either more bytes or the escape timeout to resolve it. Without this, a genuinely standalone Escape press would incorrectly end this call with OI_ERR_AGAIN instead of ever reaching that timeout.

## `classify_event` (src/cli_composer.c)

Applies one already-decoded event to the editor (mutating state exactly like the idle path does), but never draws and never acts on SUBMIT/EXIT/CTRL_C itself -- just classifies which of those (if any) this event produced, for a busy-mode caller to decide what to do with. oi_cli_prompt_state_apply's own SUBMIT/EXIT handling never mutates the editor by itself (SUBMIT is only actually committed by a later, separate oi_cli_prompt_state_commit call; EXIT only fires when the editor was already empty), so it's safe to let it run unconditionally here and classify the result afterward -- CTRL_C is the one exception (it clears the draft immediately as part of applying it), which is fine: a busy-mode Ctrl+C clearing the in-progress draft alongside cancelling the turn matches the idle Ctrl+C behavior of clearing on its own.

## `oi_cli_composer_select.pending` (src/cli_composer.c)

Discard a resize queued before this call started, then draw with the current width. This drain must precede the draw: draining afterward can swallow a resize delivered between the draw and the drain, leaving the selector rendered at a stale width until some unrelated input causes another redraw.

## `oi_cli_composer_select` (src/cli_composer.c)

Same normalization as oi_cli_composer_wait_submit's identical inner/outer loop shape: OI_ERR_AGAIN here just means "nothing more to decode without further bytes," not a real error -- a standalone Escape press must fall through to the poll() below to reach its timeout-based resolution, not end this call early.

## `wipe_line_entry.text` (src/cli_composer.c)

Volatile so the compiler cannot drop a store to a dying object.

## `OI_CLI_COMPOSER_ESCAPE_TIMEOUT_MS` (src/cli_composer.h)

How long a lone buffered Escape byte waits for a follow-up (e.g. the rest of an arrow-key sequence) before oi_cli_composer_escape_pending callers should treat it as a real, standalone Escape keypress. Shared so a reactor-driven caller (oi_reactor_timer) and the idle poll()-based path use the same window.

## `oi_cli_composer.reactor` (src/cli_composer.h)

Borrowed; NULL by default. While a wait below blocks for input, it also runs this reactor's work, so background work such as a token renewal does not stop at the prompt. A wait must never run from inside a callback of this reactor.

## `oi_cli_composer_init` (src/cli_composer.h)

Enables raw input/bracketed paste and initializes all composing state. Both descriptors must refer to terminals (same requirement as oi_cli_terminal_enable).

## `oi_cli_composer_action` (src/cli_composer.h)

Reported by oi_cli_composer_feed/_resolve_escape below: the first decisive key seen while decoding, without acting on it -- unlike oi_cli_composer_wait_submit's own internal loop, a caller reading input while a turn is active needs to decide what SUBMIT/CTRL_C mean itself (e.g. whether a one-slot queue exists to accept a SUBMIT into).

## `oi_cli_composer_feed` (src/cli_composer.h)

Reads whatever's newly available on input_fd (one read()), decodes and applies as many complete events as it yields to the editor, and reports the first decisive action seen (SUBMIT/EXIT/CTRL_C), stopping there -- any bytes after it stay buffered for the next call. Unlike oi_cli_composer_wait_submit, this never draws: a caller stepping a reactor concurrently with other output on the same fd (e.g. a turn's streamed reply) is expected to erase/redraw around the whole step itself (oi_cli_render_erase, then one oi_cli_render_draw_commands once the step settles), not per keystroke, to avoid the two writers corrupting each other's row-count assumptions. A lone trailing Escape byte is left buffered (see oi_cli_composer_escape_pending) rather than applied immediately, exactly like the idle case's poll() timeout, but the caller must arrange its own timeout (e.g. via oi_reactor_timer) instead of blocking on it here. Returns OI_ERR_CLOSED on EOF.

## `oi_cli_composer_escape_pending` (src/cli_composer.h)

True if a lone Escape byte is currently buffered awaiting the caller's own timeout before oi_cli_composer_resolve_escape can be called.

## `oi_cli_composer_resolve_escape` (src/cli_composer.h)

Resolves a buffered lone Escape once the caller's timeout elapses with nothing further arriving -- same reporting contract as feed().

## `oi_cli_composer_redraw` (src/cli_composer.h)

Re-reads the terminal width and redraws the current frame in place. Used both for an actual resize and, during a turn, as the general post-reactor-step redraw (see oi_cli_render_erase's doc comment) -- the two are indistinguishable from the composer's own perspective, since both just mean "the current editor state, at the current width, is not necessarily what's on screen anymore."

## `oi_cli_composer_commit_draft` (src/cli_composer.h)

Extracts and clears the current draft exactly like a real idle submission would (oi_cli_prompt_state_commit under the hood, including appending it to input history) -- for a caller that decided, based on its own busy/idle policy, that a reported SUBMIT action should actually be accepted rather than left as a no-op.

## `oi_cli_composer_set_draft` (src/cli_composer.h)

Overwrites the current draft outright (e.g. to restore a just-discarded queued item after Ctrl+C) -- unlike commit_draft, this doesn't touch input history and isn't tied to any prior submit; it's a plain replace. Caller is responsible for redrawing afterward (e.g. via oi_cli_composer_redraw).

## `int` (src/cli_composer.h)

Reports one already-decoded input event without touching composer->state (the editor/prompt-state) at all -- unlike oi_cli_composer_feed/_resolve_escape, which always apply an event to the editor via oi_cli_prompt_state_apply. A modal UI (e.g. a permission selector) must never let the in-progress draft be mutated by keys it's intercepting, so it decodes through this path instead for as long as it's showing. Return nonzero from `on_event` to stop feeding further events this call (mirrors oi_cli_composer_feed's "stop at the first decisive action" contract); any further already-buffered bytes stay buffered for the next call.

## `oi_cli_composer_resolve_escape_raw` (src/cli_composer.h)

Same as oi_cli_composer_resolve_escape, but reporting through the raw callback (see oi_cli_composer_feed_raw) instead of the editor.

## `oi_cli_composer_redraw_panel` (src/cli_composer.h)

Draws through the composer's own struct oi_cli_render instance (keeping previous_rows bookkeeping single-threaded regardless of which of these or oi_cli_composer_redraw was used last) -- see oi_cli_render_draw_panel/ _draw_selector for what each actually draws.

## `oi_cli_composer_select` (src/cli_composer.h)

Blocking modal selector for the idle (between-turns) case -- same poll()-loop shape as oi_cli_composer_wait_submit, but decoding directly via oi_cli_input_decode/_decode_escape and applying via oi_cli_selector_apply, entirely bypassing the editor/prompt-state (same principle as oi_cli_composer_feed_raw). `*out_selected` starts at `default_selected` and is updated as the user navigates; on a CANCEL (Escape or Ctrl+C) `*out_cancelled` is set and `*out_selected` must not be trusted as the user's choice -- treat a cancel as choosing the safe default, not as if `*out_selected` had been confirmed. Leaves the selector's own frame drawn (previous_rows tracking it correctly) on return either way; the caller decides what to erase/redraw next, exactly like oi_cli_composer_redraw/_redraw_panel already do.

## `oi_cli_composer_read_line` (src/cli_composer.h)

Blocking one-line text entry for the idle case, bypassing the editor and input history so that a secret never reaches either. With `masked`, only a character count is displayed. A newline inside a paste does not submit, so a pasted key's trailing newline cannot end the entry early.

On a normal Enter, *out_text is a heap copy the caller owns. On a cancel (Escape or Ctrl+C) or a terminate signal it stays NULL. Leaves the frame drawn on return, exactly like oi_cli_composer_select.

## `oi_cli_composer_wait_fd` (src/cli_composer.h)

Blocks with one status line drawn until `fd` is readable (*out_ready), the user presses Escape or Ctrl+C (*out_cancelled), a terminate signal arrives, or `timeout_ms` passes (none of the three set).

## `run_prompt` (test/test_cli_composer.c)

A plain fork() inherits the test harness's own session/process group, not one attached to this pty -- a resize test's TIOCSWINSZ-triggered SIGWINCH has nowhere to go without a real controlling terminal established here first.

## `a_standalone_escape_keypress_does_not_end_the_read_with_an_error` (test/test_cli_composer.c)

A genuinely standalone Escape keypress -- nothing follows it at all in this write, unlike every other test here that sends a complete escape sequence in one burst -- must resolve via the composer's own 40ms escape timeout (decode() returns OI_ERR_AGAIN for a lone 0x1b byte, since it could be the start of an arrow key) rather than being mistaken for a real error and ending the read early.

## `resize_redraws_at_the_new_width_and_preserves_submitted_text` (test/test_cli_composer.c)

Reset the capture and resize: a fresh "\x1b[J" appearing without any further keystroke proves the redraw was driven by the resize signal itself, not by continued typing.

## `resolve_escape_raw_reports_a_buffered_lone_escape` (test/test_cli_composer.c)

A lone Escape byte alone is a valid but incomplete sequence (it might be the start of an arrow key) -- feed_raw must leave it buffered rather than reporting it, exactly like oi_cli_composer_feed does.

## `select_typing_plain_text_does_not_leak_into_the_draft` (test/test_cli_composer.c)

"x"/"z" are not digit shortcuts (no visible effect) and the editor is never touched by the selector at all -- the pre-seeded draft must still be there, untouched, after the modal resolves.
