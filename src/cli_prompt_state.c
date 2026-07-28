#include "cli_prompt_state.h"

#include <stdlib.h>
#include <string.h>

static void refresh_commands(struct oi_cli_prompt_state *state) {
    state->command_match_count = oi_cli_command_filter(
        oi_cli_editor_data(&state->editor),
        oi_cli_editor_length(&state->editor), state->command_matches,
        sizeof state->command_matches / sizeof state->command_matches[0]);
    if (state->command_match_count >
        sizeof state->command_matches / sizeof state->command_matches[0]) {
        state->command_match_count =
            sizeof state->command_matches / sizeof state->command_matches[0];
    }
    if (state->command_selection >= state->command_match_count) {
        state->command_selection = 0;
    }
}

static oi_status complete_selected_command(
    struct oi_cli_prompt_state *state) {
    const struct oi_cli_command_definition *command;
    size_t name_len;
    oi_status status;

    if (state->command_match_count == 0) {
        return OI_ERR_NOTFOUND;
    }
    command = oi_cli_command_at(
        state->command_matches[state->command_selection]);
    if (command == NULL) {
        return OI_ERR_INVAL;
    }
    name_len = strlen(command->name);
    status = oi_cli_editor_set(&state->editor, command->name, name_len);
    if (status == OI_OK) {
        status = oi_cli_editor_insert(&state->editor, " ", 1);
    }
    if (status == OI_OK) {
        refresh_commands(state);
    }
    return status;
}

static oi_status apply_history(struct oi_cli_prompt_state *state, int previous,
                               enum oi_cli_prompt_action *out_action) {
    const char *text;
    size_t text_len;
    oi_status status;

    if (previous) {
        status = oi_cli_input_history_previous(
            state->history, oi_cli_editor_data(&state->editor),
            oi_cli_editor_length(&state->editor), &text, &text_len);
    } else {
        status =
            oi_cli_input_history_next(state->history, &text, &text_len);
    }
    if (status == OI_ERR_NOTFOUND) {
        *out_action = OI_CLI_PROMPT_ACTION_NONE;
        return OI_OK;
    }
    if (status != OI_OK) {
        return status;
    }
    status = oi_cli_editor_set(&state->editor, text, text_len);
    if (status == OI_OK) {
        refresh_commands(state);
        *out_action = OI_CLI_PROMPT_ACTION_REDRAW;
    }
    return status;
}

oi_status oi_cli_prompt_state_init(struct oi_cli_prompt_state *state,
                                   struct oi_cli_input_history *history) {
    if (state == NULL || history == NULL) {
        return OI_ERR_INVAL;
    }
    oi_cli_editor_init(&state->editor);
    state->history = history;
    state->pasting = 0;
    state->command_match_count = 0;
    state->command_selection = 0;
    return OI_OK;
}

void oi_cli_prompt_state_free(struct oi_cli_prompt_state *state) {
    if (state == NULL) {
        return;
    }
    oi_cli_editor_free(&state->editor);
    state->history = NULL;
    state->pasting = 0;
    state->command_match_count = 0;
    state->command_selection = 0;
}

oi_status oi_cli_prompt_state_apply(
    struct oi_cli_prompt_state *state,
    const struct oi_cli_input_event *event,
    enum oi_cli_prompt_action *out_action) {
    oi_status status = OI_OK;

    if (state == NULL || state->history == NULL || event == NULL ||
        out_action == NULL) {
        return OI_ERR_INVAL;
    }
    *out_action = OI_CLI_PROMPT_ACTION_NONE;

    switch (event->type) {
    case OI_CLI_INPUT_TEXT:
        if (event->text_len == 0 ||
            event->text_len > sizeof event->text) {
            return OI_ERR_INVAL;
        }
        status = oi_cli_editor_insert(&state->editor,
                                      (const char *)event->text,
                                      event->text_len);
        if (status == OI_OK) {
            refresh_commands(state);
        }
        if (status == OI_OK && !state->pasting) {
            *out_action = OI_CLI_PROMPT_ACTION_REDRAW;
        }
        return status;
    case OI_CLI_INPUT_ENTER:
        if (state->command_match_count != 0) {
            const struct oi_cli_command_definition *command =
                oi_cli_command_at(state->command_matches[
                    state->command_selection]);
            size_t current_len =
                oi_cli_editor_length(&state->editor);
            if (command != NULL &&
                current_len == strlen(command->name) &&
                memcmp(oi_cli_editor_data(&state->editor),
                       command->name, current_len) == 0) {
                *out_action = OI_CLI_PROMPT_ACTION_SUBMIT;
                return OI_OK;
            }
            status = complete_selected_command(state);
            if (status == OI_OK) {
                *out_action = OI_CLI_PROMPT_ACTION_REDRAW;
            }
            return status;
        }
        if (oi_cli_editor_length(&state->editor) != 0) {
            *out_action = OI_CLI_PROMPT_ACTION_SUBMIT;
        }
        return OI_OK;
    case OI_CLI_INPUT_NEWLINE:
        status = oi_cli_editor_insert(&state->editor, "\n", 1);
        if (status == OI_OK) {
            refresh_commands(state);
        }
        break;
    case OI_CLI_INPUT_LEFT:
        status = oi_cli_editor_move_left(&state->editor);
        break;
    case OI_CLI_INPUT_RIGHT:
        status = oi_cli_editor_move_right(&state->editor);
        break;
    case OI_CLI_INPUT_UP:
        if (state->command_match_count != 0) {
            if (state->command_selection == 0) {
                state->command_selection =
                    state->command_match_count - 1;
            } else {
                state->command_selection--;
            }
            *out_action = OI_CLI_PROMPT_ACTION_REDRAW;
            return OI_OK;
        }
        return apply_history(state, 1, out_action);
    case OI_CLI_INPUT_DOWN:
        if (state->command_match_count != 0) {
            state->command_selection =
                (state->command_selection + 1) %
                state->command_match_count;
            *out_action = OI_CLI_PROMPT_ACTION_REDRAW;
            return OI_OK;
        }
        return apply_history(state, 0, out_action);
    case OI_CLI_INPUT_HOME:
        oi_cli_editor_move_line_start(&state->editor);
        break;
    case OI_CLI_INPUT_END:
        oi_cli_editor_move_line_end(&state->editor);
        break;
    case OI_CLI_INPUT_BACKSPACE:
        status = oi_cli_editor_backspace(&state->editor);
        if (status == OI_OK) {
            refresh_commands(state);
        }
        break;
    case OI_CLI_INPUT_DELETE:
        status = oi_cli_editor_delete(&state->editor);
        if (status == OI_OK) {
            refresh_commands(state);
        }
        break;
    case OI_CLI_INPUT_TAB:
        status = complete_selected_command(state);
        if (status == OI_ERR_NOTFOUND) {
            return OI_OK;
        }
        break;
    case OI_CLI_INPUT_CTRL_C:
        oi_cli_editor_clear(&state->editor);
        state->pasting = 0;
        refresh_commands(state);
        *out_action = OI_CLI_PROMPT_ACTION_REDRAW;
        return OI_OK;
    case OI_CLI_INPUT_CTRL_D:
        if (oi_cli_editor_length(&state->editor) == 0) {
            *out_action = OI_CLI_PROMPT_ACTION_EXIT;
            return OI_OK;
        }
        status = oi_cli_editor_delete(&state->editor);
        if (status == OI_OK) {
            refresh_commands(state);
        }
        break;
    case OI_CLI_INPUT_PASTE_BEGIN:
        state->pasting = 1;
        return OI_OK;
    case OI_CLI_INPUT_PASTE_END:
        state->pasting = 0;
        *out_action = OI_CLI_PROMPT_ACTION_REDRAW;
        return OI_OK;
    case OI_CLI_INPUT_ESCAPE:
    case OI_CLI_INPUT_INVALID:
    case OI_CLI_INPUT_NONE:
        return OI_OK;
    }

    if (status == OI_OK && !state->pasting) {
        *out_action = OI_CLI_PROMPT_ACTION_REDRAW;
    }
    return status;
}

oi_status oi_cli_prompt_state_commit(struct oi_cli_prompt_state *state,
                                     char **out_text, size_t *out_len) {
    const char *data;
    size_t len;
    char *copy;
    oi_status status;

    if (state == NULL || state->history == NULL || out_text == NULL ||
        out_len == NULL) {
        return OI_ERR_INVAL;
    }
    len = oi_cli_editor_length(&state->editor);
    if (len == 0) {
        return OI_ERR_INVAL;
    }
    data = oi_cli_editor_data(&state->editor);
    copy = malloc(len + 1);
    if (copy == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(copy, data, len);
    copy[len] = '\0';

    status = oi_cli_input_history_append(state->history, data, len);
    if (status != OI_OK) {
        free(copy);
        return status;
    }
    oi_cli_editor_clear(&state->editor);
    refresh_commands(state);
    *out_text = copy;
    *out_len = len;
    return OI_OK;
}
