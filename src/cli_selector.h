#ifndef OI_CLI_SELECTOR_H
#define OI_CLI_SELECTOR_H

#include <stddef.h>

#include "cli_input.h"
#include "oi/status.h"

enum oi_cli_selector_action {
    OI_CLI_SELECTOR_ACTION_NONE = 0,
    OI_CLI_SELECTOR_ACTION_REDRAW,
    OI_CLI_SELECTOR_ACTION_CONFIRM,
    OI_CLI_SELECTOR_ACTION_CANCEL
};

oi_status oi_cli_selector_apply(const struct oi_cli_input_event *event,
                                size_t option_count, size_t *selected,
                                enum oi_cli_selector_action *out_action);

#endif
