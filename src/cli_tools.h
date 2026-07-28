#ifndef OI_CLI_TOOLS_H
#define OI_CLI_TOOLS_H

#include "oi/status.h"
#include "oi/tool.h"

typedef enum {
    OI_CLI_TOOLS_ASK,
    OI_CLI_TOOLS_ALLOW,
    OI_CLI_TOOLS_DENY
} oi_cli_tool_policy;

struct oi_cli_permission {
    oi_cli_tool_policy policy;
};

/* Registers the CLI-owned built-in tools. */
oi_status oi_cli_tools_register(oi_tool_registry *registry);

/* Permission callback used by the CLI loop. */
oi_tool_decision oi_cli_tool_permission(const char *tool_name,
                                         const oi_json_value *args,
                                         void *user_data);

#endif
