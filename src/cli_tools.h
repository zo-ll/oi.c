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

/*
 * Permission callback used by the interactive REPL: OI_CLI_TOOLS_ASK maps
 * to OI_TOOL_ASK, deferring the actual decision to the embedder (resolved
 * later via oi_cli_conversation_resolve_permission). No terminal I/O of
 * its own -- cli_tools defines policy only, per issue #26's own
 * module-boundary rule; requesting a decision is the REPL's job.
 */
oi_tool_decision oi_cli_tool_permission(const char *tool_name,
                                         const oi_json_value *args,
                                         void *user_data);

/*
 * Permission callback used by the non-interactive one-shot loop
 * (cli_loop.c): OI_CLI_TOOLS_ASK maps to OI_TOOL_DENY outright, since
 * that caller has no way to ever resolve a deferred decision -- using
 * oi_cli_tool_permission there would hang the process waiting for a
 * resolution that will never come.
 */
oi_tool_decision oi_cli_tool_permission_noninteractive(
    const char *tool_name, const oi_json_value *args, void *user_data);

#endif
