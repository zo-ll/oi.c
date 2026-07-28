#ifndef OI_CLI_COMMAND_DISPATCH_H
#define OI_CLI_COMMAND_DISPATCH_H

#include <stdio.h>

#include "cli_commands.h"
#include "cli_tools.h"
#include "oi/status.h"

enum oi_cli_command_result {
    OI_CLI_COMMAND_CONTINUE = 0,
    OI_CLI_COMMAND_EXIT_REPL
};

struct oi_cli_command_context {
    FILE *out;
    FILE *err;
    const char *model;
    struct oi_cli_permission *permission;
    const char *session_id;
};

oi_status oi_cli_command_dispatch(
    const struct oi_cli_command_parse *command,
    struct oi_cli_command_context *context,
    enum oi_cli_command_result *out_result);

#endif
