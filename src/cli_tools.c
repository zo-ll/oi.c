#include "cli_tools.h"

#include <stdio.h>
#include <string.h>

#include "oi/arena.h"
#include "oi/json.h"

#define SHELL_SCHEMA                                                           \
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}}," \
    "\"required\":[\"command\"],\"additionalProperties\":false}"

static char *arena_copy(oi_arena *arena, const char *data, size_t len) {
    char *copy = oi_arena_alloc(arena, len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

static oi_status shell_argv(const oi_json_value *args, oi_arena *arena,
                            void *user_data, char ***out_argv) {
    (void)user_data;
    oi_json_value *command_value = oi_json_object_get(args, "command");
    const char *command;
    size_t command_len;
    if (oi_json_type_of(args) != OI_JSON_OBJECT ||
        oi_json_get_string(command_value, &command, &command_len) != OI_OK ||
        command_len == 0) {
        return OI_ERR_INVAL;
    }

    char **argv = oi_arena_alloc(arena, 4 * sizeof *argv);
    char *command_copy = arena_copy(arena, command, command_len);
    if (argv == NULL || command_copy == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)"sh";
    argv[1] = (char *)"-c";
    argv[2] = command_copy;
    argv[3] = NULL;
    *out_argv = argv;
    return OI_OK;
}

oi_status oi_cli_tools_register(oi_tool_registry *registry) {
    return oi_tool_registry_add(registry, "shell", SHELL_SCHEMA, shell_argv,
                                NULL);
}

oi_tool_decision oi_cli_tool_permission(const char *tool_name,
                                         const oi_json_value *args,
                                         void *user_data) {
    (void)args;
    struct oi_cli_permission *permission = user_data;
    if (permission->policy == OI_CLI_TOOLS_ALLOW) {
        return OI_TOOL_ALLOW;
    }
    if (permission->policy == OI_CLI_TOOLS_DENY) {
        return OI_TOOL_DENY;
    }

    FILE *tty = fopen("/dev/tty", "r+");
    if (tty == NULL) {
        return OI_TOOL_DENY;
    }
    fprintf(tty, "oi: allow tool '%s'? [y/N] ", tool_name);
    fflush(tty);
    char answer[16];
    int allowed =
        fgets(answer, sizeof answer, tty) != NULL &&
        (answer[0] == 'y' || answer[0] == 'Y');
    fclose(tty);
    return allowed ? OI_TOOL_ALLOW : OI_TOOL_DENY;
}
