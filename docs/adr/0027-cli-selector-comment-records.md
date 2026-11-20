# cli_selector: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_selector.h`.

## `oi_cli_selector_action` (src/cli_selector.h)

Reported by oi_cli_selector_apply: the effect (if any) of one decoded input event on a modal, arrow-key-driven selection among a fixed number of options. NONE means the event had no visible effect and the caller should not redraw.

## `oi_cli_selector_apply` (src/cli_selector.h)

Pure selection logic, decoupled from any specific set of options (unlike cli_prompt_state.c's command-menu handling, which fuses this same shape of arrow-key/Enter/Escape logic directly into the command registry and the editor -- a modal selector needs it standalone, with no editor coupling at all). `*selected` is read and, on REDRAW/CONFIRM, written in place; `option_count` must be nonzero and `*selected` must already be a valid index (< option_count) on entry.

UP/DOWN move the selection with wraparound (a no-op, reported NONE, when option_count == 1, since there's nothing to move to). ENTER reports CONFIRM without changing `*selected`. ESCAPE and CTRL_C report CANCEL. A single ASCII digit '1'..'9' (an OI_CLI_INPUT_TEXT event of length 1) jumps directly to that option and reports CONFIRM, for a fast press-a-number-to-choose path; a digit beyond option_count is ignored (NONE). Every other event type is NONE.
