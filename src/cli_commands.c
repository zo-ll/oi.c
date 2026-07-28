#include "cli_commands.h"

#include <string.h>

static const struct oi_cli_command_definition commands[] = {
    {OI_CLI_COMMAND_HELP, "/help", "/help",
     "Show commands and key bindings", OI_CLI_COMMAND_READ_ONLY},
    {OI_CLI_COMMAND_EXIT, "/exit", "/exit",
     "Exit after the current safe boundary", OI_CLI_COMMAND_PROCESS_SCOPED},
    {OI_CLI_COMMAND_SESSION, "/session", "/session [subcommand]",
     "Inspect or manage sessions", OI_CLI_COMMAND_SESSION_SCOPED},
    {OI_CLI_COMMAND_MODEL, "/model", "/model [name]",
     "Show or change the session model", OI_CLI_COMMAND_SESSION_SCOPED},
    {OI_CLI_COMMAND_PERMISSIONS, "/permissions",
     "/permissions [ask|allow|deny]",
     "Show or change tool permissions", OI_CLI_COMMAND_PROCESS_SCOPED},
    {OI_CLI_COMMAND_STATUS, "/status", "/status",
     "Show current runtime and session status", OI_CLI_COMMAND_READ_ONLY},
    {OI_CLI_COMMAND_COMPACT, "/compact", "/compact [turns]",
     "Compact older model context", OI_CLI_COMMAND_SESSION_SCOPED},
    {OI_CLI_COMMAND_CWD, "/cwd", "/cwd [path]",
     "Show or change the session working directory",
     OI_CLI_COMMAND_SESSION_SCOPED},
};

static int is_space(char value) {
    return value == ' ' || value == '\t' || value == '\r' ||
           value == '\n';
}

size_t oi_cli_command_count(void) {
    return sizeof commands / sizeof commands[0];
}

const struct oi_cli_command_definition *oi_cli_command_at(size_t index) {
    return index < oi_cli_command_count() ? &commands[index] : NULL;
}

size_t oi_cli_command_filter(const char *text, size_t text_len,
                             size_t *out_indices, size_t capacity) {
    size_t matches = 0;
    size_t i;

    if (text == NULL || text_len == 0 || text[0] != '/' ||
        (text_len > 1 && text[1] == '/')) {
        return 0;
    }
    for (i = 0; i < text_len; i++) {
        if (is_space(text[i])) {
            return 0;
        }
    }
    for (i = 0; i < oi_cli_command_count(); i++) {
        size_t name_len = strlen(commands[i].name);

        if (text_len <= name_len &&
            memcmp(commands[i].name, text, text_len) == 0) {
            if (matches < capacity && out_indices != NULL) {
                out_indices[matches] = i;
            }
            matches++;
        }
    }
    return matches;
}

oi_status oi_cli_command_parse_text(const char *text, size_t text_len,
                                    struct oi_cli_command_parse *out_parse) {
    size_t token_len;
    size_t arguments_start;
    size_t arguments_end;
    size_t i;

    if ((text == NULL && text_len != 0) || out_parse == NULL) {
        return OI_ERR_INVAL;
    }
    memset(out_parse, 0, sizeof *out_parse);
    if (text_len == 0 || text[0] != '/') {
        out_parse->kind = OI_CLI_COMMAND_PARSE_MESSAGE;
        return OI_OK;
    }
    if (text_len > 1 && text[1] == '/') {
        out_parse->kind = OI_CLI_COMMAND_PARSE_LITERAL_SLASH;
        out_parse->arguments = text + 1;
        out_parse->arguments_len = text_len - 1;
        return OI_OK;
    }

    token_len = 0;
    while (token_len < text_len && !is_space(text[token_len])) {
        token_len++;
    }
    for (i = 0; i < oi_cli_command_count(); i++) {
        size_t name_len = strlen(commands[i].name);
        if (name_len == token_len &&
            memcmp(commands[i].name, text, token_len) == 0) {
            out_parse->command = &commands[i];
            break;
        }
    }
    if (out_parse->command == NULL) {
        out_parse->kind = OI_CLI_COMMAND_PARSE_UNKNOWN;
        return OI_OK;
    }

    arguments_start = token_len;
    while (arguments_start < text_len && is_space(text[arguments_start])) {
        arguments_start++;
    }
    arguments_end = text_len;
    while (arguments_end > arguments_start &&
           is_space(text[arguments_end - 1])) {
        arguments_end--;
    }
    out_parse->kind = OI_CLI_COMMAND_PARSE_COMMAND;
    out_parse->arguments = text + arguments_start;
    out_parse->arguments_len = arguments_end - arguments_start;
    return OI_OK;
}
