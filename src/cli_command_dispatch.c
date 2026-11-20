#include "cli_command_dispatch.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OI_CLI_COMMAND_MODEL_MAX_LEN 256u

static const char *permission_name(oi_cli_tool_policy policy) {
    switch (policy) {
    case OI_CLI_TOOLS_ASK:
        return "ask";
    case OI_CLI_TOOLS_ALLOW:
        return "allow";
    case OI_CLI_TOOLS_DENY:
        return "deny";
    }
    return "unknown";
}

static oi_status print_help(FILE *out) {
    size_t i;

    if (fputs("Commands:\n", out) == EOF) {
        return OI_ERR_IO;
    }
    for (i = 0; i < oi_cli_command_count(); i++) {
        const struct oi_cli_command_definition *command =
            oi_cli_command_at(i);
        if (command == NULL ||
            fprintf(out, "  %-30s %s\n", command->usage,
                    command->description) < 0) {
            return OI_ERR_IO;
        }
    }
    if (fputs("\nKeys:\n"
              "  Enter        submit (or complete the selected command)\n"
              "  Ctrl+J       newline\n"
              "  Left/Right   move; Home/End line start/end\n"
              "  Backspace    delete back; Delete deletes forward\n"
              "  Up/Down      input history, or the command menu\n"
              "  Tab          complete the selected command\n"
              "  Ctrl+C       clear the draft, or cancel the current turn\n"
              "  Ctrl+D       exit from an empty prompt\n"
              "\n"
              "/ opens the command menu; //text sends a literal leading "
              "slash.\n",
              out) == EOF ||
        fflush(out) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static oi_status print_status(struct oi_cli_command_context *context) {
    struct oi_cli_status_snapshot snapshot;
    oi_status status;

    if (context->status == NULL) {
        return fputs("oi: /status is not available in this context\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    oi_cli_status_snapshot_init(&snapshot);
    status = context->status(context->status_user_data, &snapshot);
    if (status != OI_OK) {
        return fputs("oi: could not read the current status\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    return oi_cli_status_write(&snapshot, context->out);
}

static int argument_equals(const struct oi_cli_command_parse *command,
                           const char *value) {
    size_t value_len = strlen(value);
    return command->arguments_len == value_len &&
           memcmp(command->arguments, value, value_len) == 0;
}

static oi_status dispatch_permissions(
    const struct oi_cli_command_parse *command,
    struct oi_cli_command_context *context) {
    if (command->arguments_len == 0) {
        return fprintf(context->out, "Permissions: %s\n",
                       permission_name(context->permission->policy)) < 0 ||
                       fflush(context->out) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (argument_equals(command, "ask")) {
        context->permission->policy = OI_CLI_TOOLS_ASK;
    } else if (argument_equals(command, "allow")) {
        context->permission->policy = OI_CLI_TOOLS_ALLOW;
    } else if (argument_equals(command, "deny")) {
        context->permission->policy = OI_CLI_TOOLS_DENY;
    } else {
        return fputs("oi: usage: /permissions [ask|allow|deny]\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    return fprintf(context->out, "Permissions: %s\n",
                   permission_name(context->permission->policy)) < 0 ||
                   fflush(context->out) != 0
               ? OI_ERR_IO
               : OI_OK;
}

static oi_status dispatch_model(
    const struct oi_cli_command_parse *command,
    struct oi_cli_command_context *context) {
    oi_status status;

    if (command->arguments_len == 0) {
        return fprintf(context->out, "Model: %s\n", context->model) < 0 ||
                       fflush(context->out) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (command->arguments_len > OI_CLI_COMMAND_MODEL_MAX_LEN) {
        return fputs("oi: model name is too long\n", context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (context->set_model == NULL) {
        return fputs("oi: /model is not available in this context\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    status = context->set_model(context->set_model_user_data,
                                command->arguments, command->arguments_len);
    if (status == OI_ERR_INVAL) {
        return fputs("oi: could not change the model\n", context->err) ==
                       EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (status != OI_OK) {
        return status;
    }
    return fprintf(context->out, "Model: %.*s\n", (int)command->arguments_len,
                   command->arguments) < 0 ||
                   fflush(context->out) != 0
               ? OI_ERR_IO
               : OI_OK;
}

static oi_status print_cwd(FILE *out) {
    char *cwd = getcwd(NULL, 0);
    oi_status status;

    if (cwd == NULL) {
        return OI_ERR_IO;
    }
    status = fprintf(out, "CWD: %s\n", cwd) < 0 || fflush(out) != 0
                 ? OI_ERR_IO
                 : OI_OK;
    free(cwd);
    return status;
}

static oi_status dispatch_cwd(const struct oi_cli_command_parse *command,
                              struct oi_cli_command_context *context) {
    oi_status status;

    if (command->arguments_len == 0) {
        return print_cwd(context->out);
    }
    if (context->set_cwd == NULL) {
        return fputs("oi: /cwd is not available in this context\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    status = context->set_cwd(context->set_cwd_user_data, command->arguments,
                              command->arguments_len);
    if (status == OI_ERR_INVAL) {
        return fputs("oi: could not change the working directory\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (status != OI_OK) {
        return status;
    }
    return print_cwd(context->out);
}

static oi_status print_session_usage(FILE *err) {
    return fputs("oi: usage: /session [list|trash-list|current|switch ID|"
                 "rename ID NAME|trash ID|restore ID|delete ID|import PATH]\n",
                 err) == EOF
               ? OI_ERR_IO
               : OI_OK;
}

static void format_timestamp(int64_t when, char *buffer, size_t capacity) {
    struct tm parts;
    time_t value = (time_t)when;

    if (when <= 0 || localtime_r(&value, &parts) == NULL ||
        strftime(buffer, capacity, "%Y-%m-%d %H:%M", &parts) == 0) {
        snprintf(buffer, capacity, "unknown");
    }
}

static oi_status print_session_list(struct oi_cli_command_context *context,
                                    const struct oi_cli_session_list *list,
                                    const char *heading) {
    size_t index;

    if (list->len == 0) {
        return fprintf(context->out, "%s: none\n", heading) < 0 ||
                       fflush(context->out) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (fprintf(context->out, "%s (%zu):\n", heading, list->len) < 0) {
        return OI_ERR_IO;
    }
    for (index = 0; index < list->len; index++) {
        const struct oi_cli_session_list_entry *entry = &list->entries[index];
        char updated[32];
        int is_current = context->session_id != NULL &&
                         strcmp(context->session_id, entry->id) == 0;

        format_timestamp(entry->updated_at, updated, sizeof updated);
        if (fprintf(context->out, "%s %s", is_current ? "*" : " ",
                    entry->id) < 0) {
            return OI_ERR_IO;
        }
        if (entry->display_name.len > 0 &&
            fprintf(context->out, "  \"%s\"", entry->display_name.data) < 0) {
            return OI_ERR_IO;
        }
        if (fprintf(context->out, "  model %s  updated %s",
                    entry->model.len > 0 ? entry->model.data : "unknown",
                    updated) < 0) {
            return OI_ERR_IO;
        }
        if (entry->lock_state == OI_CLI_SESSION_LOCK_BUSY &&
            fputs("  [open elsewhere]", context->out) == EOF) {
            return OI_ERR_IO;
        }
        if (entry->degraded &&
            fputs("  [metadata rebuilt]", context->out) == EOF) {
            return OI_ERR_IO;
        }
        if (fputc('\n', context->out) == EOF) {
            return OI_ERR_IO;
        }
    }
    return fflush(context->out) != 0 ? OI_ERR_IO : OI_OK;
}

static oi_status dispatch_session_list(
    struct oi_cli_command_context *context, int trashed) {
    struct oi_cli_session_list list;
    oi_status status;
    oi_status (*fetch)(void *, struct oi_cli_session_list *) =
        trashed ? context->session.list_trashed : context->session.list;

    if (fetch == NULL) {
        return fputs("oi: /session list is not available in this context\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    oi_cli_session_list_init(&list);
    status = fetch(context->session.user_data, &list);
    if (status != OI_OK) {
        oi_cli_session_list_free(&list);
        return fputs("oi: could not read the sessions directory\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    status = print_session_list(context, &list,
                                trashed ? "Trashed sessions" : "Sessions");
    oi_cli_session_list_free(&list);
    return status;
}

static oi_status dispatch_session_current(
    struct oi_cli_command_context *context) {
    struct oi_cli_command_session_current current;
    oi_status status;

    if (context->session.current == NULL) {
        return fputs("oi: /session current is not available in this "
                     "context\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    memset(&current, 0, sizeof current);
    status = context->session.current(context->session.user_data, &current);
    if (status != OI_OK) {
        return fputs("oi: could not inspect the current session\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (current.id == NULL) {
        return fputs("Session: (not created)\n", context->out) == EOF ||
                       fflush(context->out) != 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    return fprintf(context->out, "Session: %s\nPath: %s\nStatus: %s\n",
                   current.id,
                   current.path == NULL ? "(unknown)" : current.path,
                   current.healthy
                       ? "healthy"
                       : "degraded (metadata will be rebuilt from history)") <
                       0 ||
                   fflush(context->out) != 0
               ? OI_ERR_IO
               : OI_OK;
}

static oi_status report_session_operation(
    struct oi_cli_command_context *context, oi_status status,
    char *error_detail, const char *success_format, const char *id,
    size_t id_len) {
    oi_status result = OI_OK;

    if (status == OI_OK) {
        if (fprintf(context->out, success_format, (int)id_len, id) < 0 ||
            fflush(context->out) != 0) {
            result = OI_ERR_IO;
        }
    } else if (fprintf(context->err, "oi: %.*s: %s\n", (int)id_len, id,
                       error_detail != NULL ? error_detail
                                            : "operation failed") < 0) {
        result = OI_ERR_IO;
    }
    free(error_detail);
    return result;
}

static oi_status dispatch_session_rename(
    struct oi_cli_command_context *context, const char *rest,
    size_t rest_len) {
    const char *id;
    const char *name;
    size_t id_len;
    size_t name_len;
    char *detail = NULL;
    oi_status status;

    oi_cli_command_split_first_token(rest, rest_len, &id, &id_len, &name,
                                     &name_len);
    if (id_len == 0 || name_len == 0) {
        return fputs("oi: usage: /session rename ID NAME\n", context->err) ==
                       EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (context->session.rename == NULL) {
        return fputs("oi: /session rename is not available in this context\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    status = context->session.rename(context->session.user_data, id, id_len,
                                     name, name_len, &detail);
    return report_session_operation(context, status, detail,
                                    "Renamed session %.*s\n", id, id_len);
}

static oi_status dispatch_session_admin(
    struct oi_cli_command_context *context, const char *rest, size_t rest_len,
    const char *subcommand,
    oi_status (*operation)(void *, const char *, size_t, char **),
    const char *success_format) {
    char *detail = NULL;
    oi_status status;

    if (rest_len == 0) {
        return fprintf(context->err, "oi: usage: /session %s ID\n",
                       subcommand) < 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    {
        const char *id;
        const char *extra;
        size_t id_len;
        size_t extra_len;
        oi_cli_command_split_first_token(rest, rest_len, &id, &id_len, &extra,
                                         &extra_len);
        if (extra_len != 0) {
            return fprintf(context->err, "oi: usage: /session %s ID\n",
                           subcommand) < 0
                       ? OI_ERR_IO
                       : OI_OK;
        }
        if (operation == NULL) {
            return fprintf(context->err,
                           "oi: /session %s is not available in this "
                           "context\n",
                           subcommand) < 0
                       ? OI_ERR_IO
                       : OI_OK;
        }
        status = operation(context->session.user_data, id, id_len, &detail);
        return report_session_operation(context, status, detail,
                                        success_format, id, id_len);
    }
}

static oi_status dispatch_session(const struct oi_cli_command_parse *command,
                                  struct oi_cli_command_context *context) {
    const char *token;
    const char *rest;
    size_t token_len;
    size_t rest_len;

    if (command->arguments_len == 0) {
        return dispatch_session_list(context, 0);
    }
    oi_cli_command_split_first_token(command->arguments,
                                     command->arguments_len, &token,
                                     &token_len, &rest, &rest_len);

    if (oi_cli_command_token_equals(token, token_len, "list")) {
        return rest_len == 0 ? dispatch_session_list(context, 0)
                             : print_session_usage(context->err);
    }
    if (oi_cli_command_token_equals(token, token_len, "trash-list")) {
        return rest_len == 0 ? dispatch_session_list(context, 1)
                             : print_session_usage(context->err);
    }
    if (oi_cli_command_token_equals(token, token_len, "current")) {
        return rest_len == 0 ? dispatch_session_current(context)
                             : print_session_usage(context->err);
    }
    if (oi_cli_command_token_equals(token, token_len, "rename")) {
        return dispatch_session_rename(context, rest, rest_len);
    }
    if (oi_cli_command_token_equals(token, token_len, "trash")) {
        return dispatch_session_admin(context, rest, rest_len, "trash",
                                      context->session.trash,
                                      "Trashed session %.*s\n");
    }
    if (oi_cli_command_token_equals(token, token_len, "restore")) {
        return dispatch_session_admin(context, rest, rest_len, "restore",
                                      context->session.restore,
                                      "Restored session %.*s\n");
    }
    if (oi_cli_command_token_equals(token, token_len, "switch") ||
        oi_cli_command_token_equals(token, token_len, "delete") ||
        oi_cli_command_token_equals(token, token_len, "import")) {
        return fprintf(context->err,
                       "oi: /session %.*s requires the interactive REPL\n",
                       (int)token_len, token) < 0
                   ? OI_ERR_IO
                   : OI_OK;
    }
    return print_session_usage(context->err);
}

static oi_status print_deferred(
    const struct oi_cli_command_definition *definition, FILE *err) {
    return fprintf(err, "oi: %s is registered but not implemented yet\n",
                   definition->name) < 0
               ? OI_ERR_IO
               : OI_OK;
}

oi_status oi_cli_command_dispatch(
    const struct oi_cli_command_parse *command,
    struct oi_cli_command_context *context,
    enum oi_cli_command_result *out_result) {
    if (command == NULL || command->kind != OI_CLI_COMMAND_PARSE_COMMAND ||
        command->command == NULL || context == NULL || context->out == NULL ||
        context->err == NULL || context->model == NULL ||
        context->permission == NULL || out_result == NULL) {
        return OI_ERR_INVAL;
    }
    *out_result = OI_CLI_COMMAND_CONTINUE;
    switch (command->command->id) {
    case OI_CLI_COMMAND_HELP:
        return command->arguments_len == 0
                   ? print_help(context->out)
                   : fputs("oi: usage: /help\n", context->err) == EOF
                         ? OI_ERR_IO
                         : OI_OK;
    case OI_CLI_COMMAND_EXIT:
        if (command->arguments_len != 0) {
            return fputs("oi: usage: /exit\n", context->err) == EOF
                       ? OI_ERR_IO
                       : OI_OK;
        }
        *out_result = OI_CLI_COMMAND_EXIT_REPL;
        return OI_OK;
    case OI_CLI_COMMAND_STATUS:
        return command->arguments_len == 0
                   ? print_status(context)
                   : fputs("oi: usage: /status\n", context->err) == EOF
                         ? OI_ERR_IO
                         : OI_OK;
    case OI_CLI_COMMAND_PERMISSIONS:
        return dispatch_permissions(command, context);
    case OI_CLI_COMMAND_MODEL:
        return dispatch_model(command, context);
    case OI_CLI_COMMAND_CWD:
        return dispatch_cwd(command, context);
    case OI_CLI_COMMAND_SESSION:
        return dispatch_session(command, context);
    case OI_CLI_COMMAND_COMPACT:
        return print_deferred(command->command, context->err);
    case OI_CLI_COMMAND_LOGIN:
        return print_deferred(command->command, context->err);
    }
    return OI_ERR_INVAL;
}
