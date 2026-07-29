#include "cli_tools.h"

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
    struct oi_cli_permission *permission = user_data;

    (void)tool_name;
    (void)args;
    switch (permission->policy) {
    case OI_CLI_TOOLS_ALLOW:
        return OI_TOOL_ALLOW;
    case OI_CLI_TOOLS_DENY:
        return OI_TOOL_DENY;
    case OI_CLI_TOOLS_ASK:
        /* Deferred to the embedder: cli_tools has no terminal I/O of its
         * own anymore (issue #26) -- an interactive caller (cli_repl.c)
         * resolves this asynchronously via
         * oi_cli_conversation_resolve_permission once a real decision
         * arrives through its own UI. */
        return OI_TOOL_ASK;
    }
    return OI_TOOL_DENY;
}

oi_tool_decision oi_cli_tool_permission_noninteractive(
    const char *tool_name, const oi_json_value *args, void *user_data) {
    struct oi_cli_permission *permission = user_data;

    (void)tool_name;
    (void)args;
    switch (permission->policy) {
    case OI_CLI_TOOLS_ALLOW:
        return OI_TOOL_ALLOW;
    case OI_CLI_TOOLS_DENY:
    case OI_CLI_TOOLS_ASK:
        /* Never defers: a caller with no way to ever resolve a deferred
         * ASK (the non-interactive one-shot loop, cli_loop.c) would stage
         * a call and then hang forever waiting for a resolution nothing
         * will ever supply -- deny outright instead, matching "ask
         * without a usable controlling terminal denies safely rather
         * than hanging." */
        return OI_TOOL_DENY;
    }
    return OI_TOOL_DENY;
}
