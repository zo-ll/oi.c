#ifndef OI_CLI_LOOP_H
#define OI_CLI_LOOP_H

#include <stddef.h>
#include <stdio.h>

#include "cli_conversation.h"
#include "cli_message.h"
#include "cli_tools.h"
#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"
#include "oi/tool.h"

struct oi_cli_loop_config {
    const char *model;
    int max_turns;
    int tool_timeout_ms;
    struct oi_cli_permission *permission;
    FILE *out;
    FILE *err;
    const struct oi_cli_message_list *initial_context;
    oi_cli_conversation_event_cb on_event;
    void *event_user_data;
};

struct oi_cli_loop_result {
    char *assistant_text; /* caller-owned; free with free() */
    size_t assistant_text_len;
};

/*
 * Runs one user turn through zero or more model-requested tool calls.
 * The passed reactor, arena, client, and registry are borrowed.
 */
oi_status oi_cli_loop_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_loop_config *config,
                          const char *prompt,
                          struct oi_cli_loop_result *out_result);

#endif
