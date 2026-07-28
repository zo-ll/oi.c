#include "cli_command_dispatch.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* UX-level bound for /model NAME, generous for any real model id -- on
 * top of the shared 4096-byte storage ceiling (OI_CLI_HISTORY_MAX_SETTING_VALUE)
 * enforced further down the stack. */
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
        /* A bad name is a user-input problem, not a structural failure:
         * report it and stay in the REPL, matching /permissions'
         * usage-error handling. */
        return fputs("oi: could not change the model\n", context->err) ==
                       EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (status != OI_OK) {
        return status; /* structural/storage failure: existing
                        * session-failure path handles this */
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
        /* An invalid/missing/non-directory path is a user-input problem,
         * not a structural failure: report it and stay in the REPL. */
        return fputs("oi: could not change the working directory\n",
                     context->err) == EOF
                   ? OI_ERR_IO
                   : OI_OK;
    }
    if (status != OI_OK) {
        return status; /* structural/storage failure: existing
                        * session-failure path handles this */
    }
    /* A successful set_cwd always leaves the process's real cwd at the
     * new canonical location, so getcwd() is always correct here --
     * no display value needs threading through the callback. */
    return print_cwd(context->out);
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
    case OI_CLI_COMMAND_MODEL:
        return dispatch_model(command, context);
    case OI_CLI_COMMAND_CWD:
        return dispatch_cwd(command, context);
    case OI_CLI_COMMAND_SESSION:
    case OI_CLI_COMMAND_COMPACT:
        return print_deferred(command->command, context->err);
    }
    return OI_ERR_INVAL;
}
