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

/*
 * /model and /cwd request changes through these callbacks rather than
 * touching files or the conversation object directly -- cli_repl.c/cli.c
 * own what a change actually does (live conversation update, durable
 * history append, metadata refresh); dispatch only validates bounds and
 * reports the result.
 */
typedef oi_status (*oi_cli_command_set_model_cb)(void *user_data,
                                                  const char *name,
                                                  size_t name_len);
typedef oi_status (*oi_cli_command_set_cwd_cb)(void *user_data,
                                               const char *path,
                                               size_t path_len);

struct oi_cli_command_context {
    FILE *out;
    FILE *err;
    const char *model;
    struct oi_cli_permission *permission;
    const char *session_id;
    oi_cli_command_set_model_cb set_model; /* NULL if unavailable */
    void *set_model_user_data;
    oi_cli_command_set_cwd_cb set_cwd; /* NULL if unavailable */
    void *set_cwd_user_data;
};

oi_status oi_cli_command_dispatch(
    const struct oi_cli_command_parse *command,
    struct oi_cli_command_context *context,
    enum oi_cli_command_result *out_result);

#endif
