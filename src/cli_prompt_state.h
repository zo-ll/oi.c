#ifndef OI_CLI_PROMPT_STATE_H
#define OI_CLI_PROMPT_STATE_H

#include <stddef.h>

#include "cli_editor.h"
#include "cli_input.h"
#include "cli_input_history.h"
#include "oi/status.h"

enum oi_cli_prompt_action {
    OI_CLI_PROMPT_ACTION_NONE = 0,
    OI_CLI_PROMPT_ACTION_REDRAW,
    OI_CLI_PROMPT_ACTION_SUBMIT,
    OI_CLI_PROMPT_ACTION_EXIT
};

/*
 * Pure prompt interaction state. It owns the current editor and borrows the
 * process-scoped history for its entire lifetime.
 */
struct oi_cli_prompt_state {
    struct oi_cli_editor editor;
    struct oi_cli_input_history *history;
    int pasting;
};

oi_status oi_cli_prompt_state_init(struct oi_cli_prompt_state *state,
                                   struct oi_cli_input_history *history);
void oi_cli_prompt_state_free(struct oi_cli_prompt_state *state);

oi_status oi_cli_prompt_state_apply(
    struct oi_cli_prompt_state *state,
    const struct oi_cli_input_event *event,
    enum oi_cli_prompt_action *out_action);

/*
 * Copies the current non-empty input into caller-owned storage, appends it to
 * process history, and clears the editor. On failure the editor is unchanged.
 */
oi_status oi_cli_prompt_state_commit(struct oi_cli_prompt_state *state,
                                     char **out_text, size_t *out_len);

#endif
