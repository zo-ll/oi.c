#ifndef OI_CLI_REPL_H
#define OI_CLI_REPL_H

#include <stdio.h>

#include "cli_conversation.h"
#include "cli_message.h"
#include "cli_tools.h"
#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"
#include "oi/tool.h"

struct oi_cli_repl_config {
    const char *model;
    int max_turns;
    int tool_timeout_ms;
    struct oi_cli_permission *permission;
    int input_fd;
    int output_fd;
    FILE *out;
    FILE *err;
    const struct oi_cli_message_list *initial_context;
    oi_cli_conversation_event_cb on_event;
    void *event_user_data;
};

oi_status oi_cli_repl_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_repl_config *config);

#endif
