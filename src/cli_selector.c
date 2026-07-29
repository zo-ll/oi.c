#include "cli_selector.h"

oi_status oi_cli_selector_apply(const struct oi_cli_input_event *event,
                                size_t option_count, size_t *selected,
                                enum oi_cli_selector_action *out_action) {
    if (event == NULL || selected == NULL || out_action == NULL ||
        option_count == 0 || *selected >= option_count) {
        return OI_ERR_INVAL;
    }
    *out_action = OI_CLI_SELECTOR_ACTION_NONE;

    switch (event->type) {
    case OI_CLI_INPUT_UP:
        if (option_count > 1) {
            *selected = *selected == 0 ? option_count - 1 : *selected - 1;
            *out_action = OI_CLI_SELECTOR_ACTION_REDRAW;
        }
        break;
    case OI_CLI_INPUT_DOWN:
        if (option_count > 1) {
            *selected = (*selected + 1) % option_count;
            *out_action = OI_CLI_SELECTOR_ACTION_REDRAW;
        }
        break;
    case OI_CLI_INPUT_ENTER:
        *out_action = OI_CLI_SELECTOR_ACTION_CONFIRM;
        break;
    case OI_CLI_INPUT_ESCAPE:
    case OI_CLI_INPUT_CTRL_C:
        *out_action = OI_CLI_SELECTOR_ACTION_CANCEL;
        break;
    case OI_CLI_INPUT_TEXT:
        if (event->text_len == 1 && event->text[0] >= '1' &&
            event->text[0] <= '9') {
            size_t index = (size_t)(event->text[0] - '1');

            if (index < option_count) {
                *selected = index;
                *out_action = OI_CLI_SELECTOR_ACTION_CONFIRM;
            }
        }
        break;
    case OI_CLI_INPUT_NONE:
    case OI_CLI_INPUT_NEWLINE:
    case OI_CLI_INPUT_LEFT:
    case OI_CLI_INPUT_RIGHT:
    case OI_CLI_INPUT_HOME:
    case OI_CLI_INPUT_END:
    case OI_CLI_INPUT_BACKSPACE:
    case OI_CLI_INPUT_DELETE:
    case OI_CLI_INPUT_TAB:
    case OI_CLI_INPUT_CTRL_D:
    case OI_CLI_INPUT_PASTE_BEGIN:
    case OI_CLI_INPUT_PASTE_END:
    case OI_CLI_INPUT_INVALID:
        break;
    }
    return OI_OK;
}
