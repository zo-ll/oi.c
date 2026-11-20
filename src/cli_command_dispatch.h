#ifndef OI_CLI_COMMAND_DISPATCH_H
#define OI_CLI_COMMAND_DISPATCH_H

#include <stdio.h>

#include "cli_commands.h"
#include "cli_sessions.h"
#include "cli_status.h"
#include "cli_tools.h"
#include "oi/status.h"

enum oi_cli_command_result {
    OI_CLI_COMMAND_CONTINUE = 0,
    OI_CLI_COMMAND_EXIT_REPL
};

typedef oi_status (*oi_cli_command_set_model_cb)(void *user_data,
                                                  const char *name,
                                                  size_t name_len);
typedef oi_status (*oi_cli_command_set_cwd_cb)(void *user_data,
                                               const char *path,
                                               size_t path_len);

typedef oi_status (*oi_cli_command_status_cb)(
    void *user_data, struct oi_cli_status_snapshot *out_snapshot);

struct oi_cli_command_session_current {
    const char *id;
    const char *path;
    int healthy;
};

struct oi_cli_command_session_ops {
    void *user_data;
    oi_status (*list)(void *user_data, struct oi_cli_session_list *out_list);
    oi_status (*list_trashed)(void *user_data,
                              struct oi_cli_session_list *out_list);
    oi_status (*current)(void *user_data,
                         struct oi_cli_command_session_current *out_current);
    oi_status (*rename)(void *user_data, const char *id, size_t id_len,
                        const char *name, size_t name_len,
                        char **out_error_detail);
    oi_status (*trash)(void *user_data, const char *id, size_t id_len,
                       char **out_error_detail);
    oi_status (*restore)(void *user_data, const char *id, size_t id_len,
                         char **out_error_detail);
};

struct oi_cli_command_context {
    FILE *out;
    FILE *err;
    const char *model;
    struct oi_cli_permission *permission;
    const char *session_id;
    oi_cli_command_set_model_cb set_model;
    void *set_model_user_data;
    oi_cli_command_set_cwd_cb set_cwd;
    void *set_cwd_user_data;
    oi_cli_command_status_cb status;
    void *status_user_data;
    struct oi_cli_command_session_ops session;
};

oi_status oi_cli_command_dispatch(
    const struct oi_cli_command_parse *command,
    struct oi_cli_command_context *context,
    enum oi_cli_command_result *out_result);

#endif
