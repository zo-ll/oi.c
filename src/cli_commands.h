#ifndef OI_CLI_COMMANDS_H
#define OI_CLI_COMMANDS_H

#include <stddef.h>

#include "oi/status.h"

enum oi_cli_command_id {
    OI_CLI_COMMAND_HELP = 0,
    OI_CLI_COMMAND_EXIT,
    OI_CLI_COMMAND_SESSION,
    OI_CLI_COMMAND_MODEL,
    OI_CLI_COMMAND_PERMISSIONS,
    OI_CLI_COMMAND_STATUS,
    OI_CLI_COMMAND_COMPACT,
    OI_CLI_COMMAND_CWD
};

enum oi_cli_command_scope {
    OI_CLI_COMMAND_READ_ONLY = 0,
    OI_CLI_COMMAND_PROCESS_SCOPED,
    OI_CLI_COMMAND_SESSION_SCOPED
};

struct oi_cli_command_definition {
    enum oi_cli_command_id id;
    const char *name;
    const char *usage;
    const char *description;
    enum oi_cli_command_scope scope;
};

enum oi_cli_command_parse_kind {
    OI_CLI_COMMAND_PARSE_MESSAGE = 0,
    OI_CLI_COMMAND_PARSE_LITERAL_SLASH,
    OI_CLI_COMMAND_PARSE_COMMAND,
    OI_CLI_COMMAND_PARSE_UNKNOWN
};

struct oi_cli_command_parse {
    enum oi_cli_command_parse_kind kind;
    const struct oi_cli_command_definition *command;
    const char *arguments;
    size_t arguments_len;
};

size_t oi_cli_command_count(void);
const struct oi_cli_command_definition *oi_cli_command_at(size_t index);

/*
 * Filters command names using the current first token, including its leading
 * slash. Returns the total match count; up to `capacity` indices are written.
 */
size_t oi_cli_command_filter(const char *text, size_t text_len,
                             size_t *out_indices, size_t capacity);

oi_status oi_cli_command_parse_text(const char *text, size_t text_len,
                                    struct oi_cli_command_parse *out_parse);

#endif
