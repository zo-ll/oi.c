#include "cli_command_dispatch.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    if (fputs("\nKeys: Enter submit, Ctrl+J newline, Ctrl+C clear, "
              "Ctrl+D exit\n",
              out) == EOF ||
        fflush(out) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static oi_status print_status(struct oi_cli_command_context *context) {
    char *cwd = getcwd(NULL, 0);
    oi_status status = OI_OK;

    if (cwd == NULL) {
        return OI_ERR_IO;
    }
    if (fprintf(context->out,
                "Session: %s\nModel: %s\nPermissions: %s\nCWD: %s\n",
                context->session_id == NULL ? "(not created)"
                                            : context->session_id,
                context->model,
                permission_name(context->permission->policy), cwd) < 0 ||
        fflush(context->out) != 0) {
        status = OI_ERR_IO;
    }
    free(cwd);
    return status;
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
                   : print_deferred(command->command, context->err);
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
    case OI_CLI_COMMAND_SESSION:
    case OI_CLI_COMMAND_MODEL:
    case OI_CLI_COMMAND_COMPACT:
    case OI_CLI_COMMAND_CWD:
        return print_deferred(command->command, context->err);
    }
    return OI_ERR_INVAL;
}
