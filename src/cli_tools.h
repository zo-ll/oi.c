#ifndef OI_CLI_TOOLS_H
#define OI_CLI_TOOLS_H

#include "oi/json.h"
#include "oi/status.h"
#include "oi/tool.h"

typedef enum {
    OI_CLI_TOOLS_ASK,
    OI_CLI_TOOLS_ALLOW,
    OI_CLI_TOOLS_DENY
} oi_cli_tool_policy;

enum oi_cli_tool_category {
    OI_CLI_TOOL_READ_IN_PROJECT,
    OI_CLI_TOOL_FILE_ACCESS,
    OI_CLI_TOOL_COMMAND
};

struct oi_cli_permission {
    oi_cli_tool_policy policy;
    int file_access_allowed;
    int commands_allowed;
};

oi_status oi_cli_tools_register(oi_tool_registry *registry,
                                const char *program);

oi_status oi_cli_tools_write_definitions(oi_json_writer *writer);

enum oi_cli_tool_category oi_cli_tool_category_of(const char *tool_name,
                                                  const oi_json_value *args);

void oi_cli_permission_set_policy(struct oi_cli_permission *permission,
                                  oi_cli_tool_policy policy);

void oi_cli_permission_allow_category(struct oi_cli_permission *permission,
                                      enum oi_cli_tool_category category);

oi_tool_decision oi_cli_tool_permission(const char *tool_name,
                                         const oi_json_value *args,
                                         void *user_data);

oi_tool_decision oi_cli_tool_permission_noninteractive(
    const char *tool_name, const oi_json_value *args, void *user_data);

#endif
