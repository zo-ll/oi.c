#include "cli_command_dispatch.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
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

static int is_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

/*
 * Splits `text` at its first run of whitespace into a leading token and a
 * left-trimmed remainder. `text` arrives already right-trimmed from
 * oi_cli_command_parse_text, so the remainder needs no further trimming --
 * which matters for /session import, whose argument is a path that may
 * legitimately contain spaces and must survive verbatim.
 */
static void split_first_token(const char *text, size_t len,
                              const char **out_token, size_t *out_token_len,
                              const char **out_rest, size_t *out_rest_len) {
    size_t token_len = 0;
    size_t rest_start;

    while (token_len < len && !is_space(text[token_len])) {
        token_len++;
    }
    rest_start = token_len;
    while (rest_start < len && is_space(text[rest_start])) {
        rest_start++;
    }
    *out_token = text;
    *out_token_len = token_len;
    *out_rest = text + rest_start;
    *out_rest_len = len - rest_start;
}

static int token_equals(const char *token, size_t token_len,
                        const char *value) {
    size_t value_len = strlen(value);
    return token_len == value_len && memcmp(token, value, value_len) == 0;
}

static oi_status print_session_usage(FILE *err) {
    return fputs("oi: usage: /session [list|trash-list|current|switch ID|"
                 "rename ID NAME|trash ID|restore ID|delete ID|import PATH]\n",
                 err) == EOF
               ? OI_ERR_IO
               : OI_OK;
}

/* Formats one epoch timestamp for a selector row; 0 means unknown. */
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
        /* A leading marker beats a trailing "(current)": it lines the rows
         * up so the ids stay scannable. */
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
    /* Health is resolved live on every call rather than cached from
     * startup: metadata can be deleted or corrupted by something outside
     * oi at any point during a long-running session. */
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

/* Reports the outcome of a rename/trash/restore uniformly. */
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

    split_first_token(rest, rest_len, &id, &id_len, &name, &name_len);
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
    /* An id is a single bounded token, so trailing words are a mistake
     * worth naming rather than silently ignoring. */
    {
        const char *id;
        const char *extra;
        size_t id_len;
        size_t extra_len;
        split_first_token(rest, rest_len, &id, &id_len, &extra, &extra_len);
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

    /* Bare /session means "show me what I can pick from". */
    if (command->arguments_len == 0) {
        return dispatch_session_list(context, 0);
    }
    split_first_token(command->arguments, command->arguments_len, &token,
                      &token_len, &rest, &rest_len);

    if (token_equals(token, token_len, "list")) {
        return rest_len == 0 ? dispatch_session_list(context, 0)
                             : print_session_usage(context->err);
    }
    if (token_equals(token, token_len, "trash-list")) {
        return rest_len == 0 ? dispatch_session_list(context, 1)
                             : print_session_usage(context->err);
    }
    if (token_equals(token, token_len, "current")) {
        return rest_len == 0 ? dispatch_session_current(context)
                             : print_session_usage(context->err);
    }
    if (token_equals(token, token_len, "rename")) {
        return dispatch_session_rename(context, rest, rest_len);
    }
    if (token_equals(token, token_len, "trash")) {
        return dispatch_session_admin(context, rest, rest_len, "trash",
                                      context->session.trash,
                                      "Trashed session %.*s\n");
    }
    if (token_equals(token, token_len, "restore")) {
        return dispatch_session_admin(context, rest, rest_len, "restore",
                                      context->session.restore,
                                      "Restored session %.*s\n");
    }
    /*
     * These three need the composer's confirmation flow or the live
     * conversation, so cli_repl.c intercepts them before dispatch runs.
     * Reaching here means no interactive REPL is present -- a
     * non-interactive context or these unit tests -- so say so plainly
     * rather than pretending the command does not exist.
     */
    if (token_equals(token, token_len, "switch") ||
        token_equals(token, token_len, "delete") ||
        token_equals(token, token_len, "import")) {
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
        return dispatch_session(command, context);
    case OI_CLI_COMMAND_COMPACT:
        /* Never actually reached from the interactive REPL: cli_repl.c
         * intercepts /compact at have_parsed_command: before dispatch is
         * ever called, since this command needs the conversation, the
         * durable history store, and the LLM client, none of which
         * oi_cli_command_context has access to by design. Only reachable
         * here via oi_cli_command_dispatch's own unit tests. */
        return print_deferred(command->command, context->err);
    }
    return OI_ERR_INVAL;
}
