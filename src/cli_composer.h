#ifndef OI_CLI_COMPOSER_H
#define OI_CLI_COMPOSER_H

#include <stddef.h>

#include "cli_input.h"
#include "cli_input_history.h"
#include "cli_prompt_state.h"
#include "cli_render.h"
#include "cli_selector.h"
#include "cli_terminal.h"
#include "oi/status.h"

#define OI_CLI_COMPOSER_INPUT_CAP 4096U
/* How long a lone buffered Escape byte waits for a follow-up (e.g. the
 * rest of an arrow-key sequence) before oi_cli_composer_escape_pending
 * callers should treat it as a real, standalone Escape keypress. Shared
 * so a reactor-driven caller (oi_reactor_timer) and the idle poll()-based
 * path use the same window. */
#define OI_CLI_COMPOSER_ESCAPE_TIMEOUT_MS 40

/*
 * Owns the whole interactive editing surface (raw terminal mode, bracketed
 * paste, the input decoder, prompt/editor state, and the ANSI renderer) for
 * the entire process lifetime -- created once by the REPL controller before
 * its first prompt and destroyed once at the very end, unlike this state's
 * previous per-prompt-read lifetime (the old oi_cli_prompt_read). This is
 * what lets terminal input keep being decoded and redrawn while a turn is
 * active: raw mode (and with it, the loss of ISIG-based keyboard SIGINT)
 * has to span turns too, not just idle prompts, for that to be possible at
 * all. A real kill-sent SIGINT (via signalfd) is unaffected either way.
 */
struct oi_cli_composer {
    struct oi_cli_terminal terminal;
    struct oi_cli_input_decoder decoder;
    struct oi_cli_prompt_state state;
    struct oi_cli_render render;
    unsigned char input[OI_CLI_COMPOSER_INPUT_CAP];
    size_t input_len;
    int input_fd;
    int output_fd;
};

/* Enables raw input/bracketed paste and initializes all composing state.
 * Both descriptors must refer to terminals (same requirement as
 * oi_cli_terminal_enable). */
oi_status oi_cli_composer_init(struct oi_cli_composer *composer, int input_fd,
                               int output_fd,
                               struct oi_cli_input_history *history);
/* Restores the terminal and frees composing state. Does not draw a final
 * newline -- oi_cli_composer_wait_submit already does that unconditionally
 * before returning, on every exit path, exactly as the old
 * oi_cli_prompt_read did per call. */
void oi_cli_composer_free(struct oi_cli_composer *composer);

/*
 * Blocks (via poll()) until one line is submitted, Ctrl+D is pressed at an
 * empty prompt, or a terminate signal arrives -- semantically identical to
 * the old oi_cli_prompt_read, but reusing an already-initialized composer
 * rather than re-enabling/restoring raw mode on every call.
 *
 * `signal_fd` is an optional signalfd already registered for SIGWINCH,
 * SIGINT, SIGTERM, and SIGHUP (SFD_NONBLOCK), owned and blocked/created by
 * the caller; pass -1 to disable signal handling entirely (the terminal
 * width is still re-read once at the start of the call either way). When
 * >= 0: a pending SIGWINCH re-reads the terminal width and redraws the
 * current frame in place, preserving the edit buffer, cursor, and
 * command-menu selection; a pending SIGTERM/SIGHUP (or, defensively,
 * SIGINT -- it cannot ordinarily fire here since raw mode clears ISIG, but
 * a pending one could in principle already be queued from before raw mode
 * was entered) ends the read cleanly and sets `*out_terminate_signal` to
 * the signal number, distinct from `*out_exit` (Ctrl+D).
 */
oi_status oi_cli_composer_wait_submit(struct oi_cli_composer *composer,
                                      int signal_fd, char **out_text,
                                      size_t *out_len, int *out_exit,
                                      int *out_terminate_signal);

/*
 * Reported by oi_cli_composer_feed/_resolve_escape below: the first
 * decisive key seen while decoding, without acting on it -- unlike
 * oi_cli_composer_wait_submit's own internal loop, a caller reading input
 * while a turn is active needs to decide what SUBMIT/CTRL_C mean itself
 * (e.g. whether a one-slot queue exists to accept a SUBMIT into).
 */
enum oi_cli_composer_action {
    OI_CLI_COMPOSER_ACTION_NONE = 0,
    OI_CLI_COMPOSER_ACTION_SUBMIT,
    OI_CLI_COMPOSER_ACTION_EXIT,
    OI_CLI_COMPOSER_ACTION_CTRL_C
};

/*
 * Reads whatever's newly available on input_fd (one read()), decodes and
 * applies as many complete events as it yields to the editor, and reports
 * the first decisive action seen (SUBMIT/EXIT/CTRL_C), stopping there --
 * any bytes after it stay buffered for the next call. Unlike
 * oi_cli_composer_wait_submit, this never draws: a caller stepping a
 * reactor concurrently with other output on the same fd (e.g. a turn's
 * streamed reply) is expected to erase/redraw around the whole step
 * itself (oi_cli_render_erase, then one oi_cli_render_draw_commands once
 * the step settles), not per keystroke, to avoid the two writers
 * corrupting each other's row-count assumptions. A lone trailing Escape
 * byte is left buffered (see oi_cli_composer_escape_pending) rather than
 * applied immediately, exactly like the idle case's poll() timeout, but
 * the caller must arrange its own timeout (e.g. via oi_reactor_timer)
 * instead of blocking on it here. Returns OI_ERR_CLOSED on EOF.
 */
oi_status oi_cli_composer_feed(struct oi_cli_composer *composer,
                               enum oi_cli_composer_action *out_action);

/* True if a lone Escape byte is currently buffered awaiting the caller's
 * own timeout before oi_cli_composer_resolve_escape can be called. */
int oi_cli_composer_escape_pending(const struct oi_cli_composer *composer);

/* Resolves a buffered lone Escape once the caller's timeout elapses with
 * nothing further arriving -- same reporting contract as feed(). */
oi_status oi_cli_composer_resolve_escape(
    struct oi_cli_composer *composer,
    enum oi_cli_composer_action *out_action);

/* Re-reads the terminal width and redraws the current frame in place.
 * Used both for an actual resize and, during a turn, as the general
 * post-reactor-step redraw (see oi_cli_render_erase's doc comment) -- the
 * two are indistinguishable from the composer's own perspective, since
 * both just mean "the current editor state, at the current width, is not
 * necessarily what's on screen anymore." */
oi_status oi_cli_composer_redraw(struct oi_cli_composer *composer);

/*
 * Extracts and clears the current draft exactly like a real idle
 * submission would (oi_cli_prompt_state_commit under the hood, including
 * appending it to input history) -- for a caller that decided, based on
 * its own busy/idle policy, that a reported SUBMIT action should actually
 * be accepted rather than left as a no-op.
 */
oi_status oi_cli_composer_commit_draft(struct oi_cli_composer *composer,
                                       char **out_text, size_t *out_len);

/*
 * Overwrites the current draft outright (e.g. to restore a just-discarded
 * queued item after Ctrl+C) -- unlike commit_draft, this doesn't touch
 * input history and isn't tied to any prior submit; it's a plain replace.
 * Caller is responsible for redrawing afterward (e.g. via
 * oi_cli_composer_redraw).
 */
oi_status oi_cli_composer_set_draft(struct oi_cli_composer *composer,
                                    const char *text, size_t text_len);

/*
 * Reports one already-decoded input event without touching
 * composer->state (the editor/prompt-state) at all -- unlike
 * oi_cli_composer_feed/_resolve_escape, which always apply an event to
 * the editor via oi_cli_prompt_state_apply. A modal UI (e.g. a permission
 * selector) must never let the in-progress draft be mutated by keys it's
 * intercepting, so it decodes through this path instead for as long as
 * it's showing. Return nonzero from `on_event` to stop feeding further
 * events this call (mirrors oi_cli_composer_feed's "stop at the first
 * decisive action" contract); any further already-buffered bytes stay
 * buffered for the next call.
 */
typedef int (*oi_cli_composer_raw_event_cb)(
    const struct oi_cli_input_event *event, void *user_data);

oi_status oi_cli_composer_feed_raw(struct oi_cli_composer *composer,
                                   oi_cli_composer_raw_event_cb on_event,
                                   void *user_data);
/* Same as oi_cli_composer_resolve_escape, but reporting through the raw
 * callback (see oi_cli_composer_feed_raw) instead of the editor. */
oi_status oi_cli_composer_resolve_escape_raw(
    struct oi_cli_composer *composer,
    oi_cli_composer_raw_event_cb on_event, void *user_data);

/* Draws through the composer's own struct oi_cli_render instance (keeping
 * previous_rows bookkeeping single-threaded regardless of which of these
 * or oi_cli_composer_redraw was used last) -- see oi_cli_render_draw_panel/
 * _draw_selector for what each actually draws. */
oi_status oi_cli_composer_redraw_panel(
    struct oi_cli_composer *composer,
    const struct oi_cli_render_line *header_lines, size_t header_count);
oi_status oi_cli_composer_draw_selector(
    struct oi_cli_composer *composer,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t selected_option);

/*
 * Blocking modal selector for the idle (between-turns) case -- same
 * poll()-loop shape as oi_cli_composer_wait_submit, but decoding directly
 * via oi_cli_input_decode/_decode_escape and applying via
 * oi_cli_selector_apply, entirely bypassing the editor/prompt-state
 * (same principle as oi_cli_composer_feed_raw). `*out_selected` starts at
 * `default_selected` and is updated as the user navigates; on a CANCEL
 * (Escape or Ctrl+C) `*out_cancelled` is set and `*out_selected` must not
 * be trusted as the user's choice -- treat a cancel as choosing the safe
 * default, not as if `*out_selected` had been confirmed. Leaves the
 * selector's own frame drawn (previous_rows tracking it correctly) on
 * return either way; the caller decides what to erase/redraw next, exactly
 * like oi_cli_composer_redraw/_redraw_panel already do.
 */
oi_status oi_cli_composer_select(
    struct oi_cli_composer *composer, int signal_fd,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t default_selected, size_t *out_selected, int *out_cancelled,
    int *out_terminate_signal);

#endif
