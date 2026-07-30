#ifndef OI_CLI_COMMAND_DISPATCH_H
#define OI_CLI_COMMAND_DISPATCH_H

#include <stdio.h>

#include "cli_commands.h"
#include "cli_sessions.h"
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

/*
 * What /session current reports. Both strings are borrowed and valid only
 * until the next call through the same ops.
 */
struct oi_cli_command_session_current {
    const char *id;   /* NULL when no durable session exists */
    const char *path; /* the session directory or log; may be NULL */
    int healthy;      /* selector metadata present and owned by this id */
};

/*
 * /session delegates every filesystem operation through these, for the
 * same reason /model and /cwd do: dispatch validates grammar and renders
 * the result, while cli.c owns what an operation actually does. Grouped
 * rather than spread across a dozen flat fields because they share one
 * owner, one lifetime, and one `user_data`.
 *
 * `list`/`list_trashed` fill a caller-initialized list that dispatch then
 * formats and frees. `rename`, `trash`, and `restore` report a specific
 * cause through `*out_error_detail` (caller-owned, may be left NULL).
 *
 * Any callback may be NULL, meaning that operation is unavailable in this
 * context; dispatch says so rather than crashing. /session switch, delete,
 * and import are absent by design -- they need the composer's confirmation
 * flow or the live conversation, so cli_repl.c intercepts them before
 * dispatch is reached.
 */
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
    oi_cli_command_set_model_cb set_model; /* NULL if unavailable */
    void *set_model_user_data;
    oi_cli_command_set_cwd_cb set_cwd; /* NULL if unavailable */
    void *set_cwd_user_data;
    struct oi_cli_command_session_ops session; /* all-NULL if unavailable */
};

oi_status oi_cli_command_dispatch(
    const struct oi_cli_command_parse *command,
    struct oi_cli_command_context *context,
    enum oi_cli_command_result *out_result);

#endif
