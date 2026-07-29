#ifndef OI_CLI_SELECTOR_H
#define OI_CLI_SELECTOR_H

#include <stddef.h>

#include "cli_input.h"
#include "oi/status.h"

/*
 * Reported by oi_cli_selector_apply: the effect (if any) of one decoded
 * input event on a modal, arrow-key-driven selection among a fixed number
 * of options. NONE means the event had no visible effect and the caller
 * should not redraw.
 */
enum oi_cli_selector_action {
    OI_CLI_SELECTOR_ACTION_NONE = 0,
    OI_CLI_SELECTOR_ACTION_REDRAW,
    OI_CLI_SELECTOR_ACTION_CONFIRM,
    OI_CLI_SELECTOR_ACTION_CANCEL
};

/*
 * Pure selection logic, decoupled from any specific set of options (unlike
 * cli_prompt_state.c's command-menu handling, which fuses this same shape
 * of arrow-key/Enter/Escape logic directly into the command registry and
 * the editor -- a modal selector needs it standalone, with no editor
 * coupling at all). `*selected` is read and, on REDRAW/CONFIRM, written in
 * place; `option_count` must be nonzero and `*selected` must already be a
 * valid index (< option_count) on entry.
 *
 * UP/DOWN move the selection with wraparound (a no-op, reported NONE, when
 * option_count == 1, since there's nothing to move to). ENTER reports
 * CONFIRM without changing `*selected`. ESCAPE and CTRL_C report CANCEL.
 * A single ASCII digit '1'..'9' (an OI_CLI_INPUT_TEXT event of length 1)
 * jumps directly to that option and reports CONFIRM, for a fast
 * press-a-number-to-choose path; a digit beyond option_count is ignored
 * (NONE). Every other event type is NONE.
 */
oi_status oi_cli_selector_apply(const struct oi_cli_input_event *event,
                                size_t option_count, size_t *selected,
                                enum oi_cli_selector_action *out_action);

#endif
