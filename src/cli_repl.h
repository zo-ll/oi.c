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

typedef oi_status (*oi_cli_repl_prepare_cb)(void *user_data,
                                            oi_arena **out_arena);

/*
 * Durable persistence for a /model or /cwd change, implemented by the
 * caller (cli.c owns filesystem paths and metadata; the REPL controller
 * does not). NULL means nothing durable exists yet to persist to (no
 * session created); the live update still applies regardless.
 */
typedef oi_status (*oi_cli_repl_persist_model_cb)(void *user_data,
                                                  const char *name,
                                                  size_t name_len);
typedef oi_status (*oi_cli_repl_persist_cwd_cb)(void *user_data,
                                                const char *path,
                                                size_t path_len);

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
    oi_cli_repl_prepare_cb prepare;
    void *prepare_user_data;
    const char *(*session_id)(void *user_data);
    void *session_id_user_data;
    /* Shared with the caller's lazy automatic-session preparation, so a
     * /model issued before the first message is honored when the session
     * is eventually created. NULL when no automatic-session concept
     * applies (an explicit --session ID is already fully resolved before
     * the REPL starts). */
    struct oi_cli_string *pending_model;
    oi_cli_repl_persist_model_cb persist_model;
    void *persist_model_user_data;
    oi_cli_repl_persist_cwd_cb persist_cwd;
    void *persist_cwd_user_data;
};

oi_status oi_cli_repl_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_repl_config *config);

#endif
