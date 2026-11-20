#ifndef OI_CLI_REPL_H
#define OI_CLI_REPL_H

#include <stdint.h>
#include <stdio.h>

#include "cli_command_dispatch.h"
#include "cli_conversation.h"
#include "cli_login.h"
#include "cli_message.h"
#include "cli_session_switch.h"
#include "cli_status.h"
#include "cli_tools.h"
#include "oi/arena.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"
#include "oi/tool.h"

typedef oi_status (*oi_cli_repl_prepare_cb)(void *user_data,
                                            oi_arena **out_arena);

typedef oi_status (*oi_cli_repl_switch_session_cb)(
    void *user_data, const char *id, size_t id_len,
    struct oi_cli_session_switch_result *out_result);

typedef oi_status (*oi_cli_repl_delete_session_cb)(void *user_data,
                                                   const char *id,
                                                   size_t id_len,
                                                   char **out_error_detail);

typedef oi_status (*oi_cli_repl_import_session_cb)(void *user_data,
                                                   const char *path,
                                                   size_t path_len,
                                                   char **out_new_id,
                                                   char **out_error_detail);

typedef oi_status (*oi_cli_repl_persist_model_cb)(void *user_data,
                                                  const char *name,
                                                  size_t name_len);
typedef oi_status (*oi_cli_repl_persist_cwd_cb)(void *user_data,
                                                const char *path,
                                                size_t path_len);

typedef int (*oi_cli_repl_is_durably_failed_cb)(void *user_data);

typedef oi_status (*oi_cli_repl_persist_queued_input_cb)(
    void *user_data, const char *content, size_t content_len,
    uint64_t *out_record_id);
typedef oi_status (*oi_cli_repl_persist_queue_resolved_cb)(
    void *user_data, uint64_t queued_record_id, int consumed);

typedef oi_status (*oi_cli_repl_persist_checkpoint_cb)(
    void *user_data, size_t prefix_message_count, const char *summary,
    size_t summary_len, const char *model, size_t model_len);

typedef void (*oi_cli_repl_status_checkpoint_cb)(
    void *user_data, struct oi_cli_status_checkpoint *out_checkpoint);

struct oi_cli_repl_config {
    const char *model;
    int max_turns;
    int tool_timeout_ms;
    size_t tool_output_limit;
    struct oi_cli_status_endpoint *endpoint;
    struct oi_cli_login *login;
    int request_timeout_ms;
    enum oi_cli_session_model_origin model_origin;
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
    struct oi_cli_string *pending_model;
    oi_cli_repl_persist_model_cb persist_model;
    void *persist_model_user_data;
    oi_cli_repl_persist_cwd_cb persist_cwd;
    void *persist_cwd_user_data;
    oi_cli_repl_is_durably_failed_cb is_durably_failed;
    void *is_durably_failed_user_data;
    oi_cli_repl_persist_queued_input_cb persist_queued_input;
    void *persist_queued_input_user_data;
    oi_cli_repl_persist_queue_resolved_cb persist_queue_resolved;
    void *persist_queue_resolved_user_data;
    oi_cli_repl_persist_checkpoint_cb persist_checkpoint;
    void *persist_checkpoint_user_data;
    oi_cli_repl_status_checkpoint_cb status_checkpoint;
    void *status_checkpoint_user_data;
    const char *initial_draft;
    size_t initial_draft_len;

    struct oi_cli_command_session_ops session_ops;

    oi_cli_repl_switch_session_cb switch_session;
    void *switch_session_user_data;
    oi_cli_repl_delete_session_cb delete_session;
    void *delete_session_user_data;
    oi_cli_repl_import_session_cb import_session;
    void *import_session_user_data;
};

oi_status oi_cli_repl_run(oi_llm_client *client, oi_reactor *reactor,
                          oi_arena *arena, oi_tool_registry *tools,
                          const struct oi_cli_repl_config *config);

#endif
