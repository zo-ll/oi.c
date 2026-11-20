#include "cli_tools.h"

#include <string.h>

#include "oi/arena.h"
#include "oi/json.h"

#define WRITE(expr)                                                           \
    do {                                                                      \
        oi_status write_status = (expr);                                      \
        if (write_status != OI_OK) {                                          \
            return write_status;                                              \
        }                                                                     \
    } while (0)

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

static oi_status write_shell_parameters(oi_json_writer *writer) {
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "type", 4));
    WRITE(oi_json_write_string(writer, "object", 6));
    WRITE(oi_json_write_object_key(writer, "properties", 10));
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "command", 7));
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "type", 4));
    WRITE(oi_json_write_string(writer, "string", 6));
    WRITE(oi_json_write_object_end(writer));
    WRITE(oi_json_write_object_end(writer));
    WRITE(oi_json_write_object_key(writer, "required", 8));
    WRITE(oi_json_write_array_begin(writer));
    WRITE(oi_json_write_string(writer, "command", 7));
    WRITE(oi_json_write_array_end(writer));
    WRITE(oi_json_write_object_key(writer, "additionalProperties", 20));
    WRITE(oi_json_write_bool(writer, 0));
    return oi_json_write_object_end(writer);
}

static const struct {
    const char *name;
    const char *description;
    oi_status (*write_parameters)(oi_json_writer *writer);
    oi_tool_build_argv build_argv;
} tools[] = {
    {"shell", "Execute a shell command", write_shell_parameters, shell_argv},
};

oi_status oi_cli_tools_register(oi_tool_registry *registry) {
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
        oi_json_writer *schema = oi_json_writer_create();
        oi_status status;

        if (schema == NULL) {
            return OI_ERR_NOMEM;
        }
        status = tools[i].write_parameters(schema);
        if (status == OI_OK) {
            status = oi_tool_registry_add(registry, tools[i].name,
                                          oi_json_writer_data(schema, NULL),
                                          tools[i].build_argv, NULL);
        }
        oi_json_writer_destroy(schema);
        if (status != OI_OK) {
            return status;
        }
    }
    return OI_OK;
}

oi_status oi_cli_tools_write_definitions(oi_json_writer *writer) {
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
        WRITE(oi_json_write_object_begin(writer));
        WRITE(oi_json_write_object_key(writer, "type", 4));
        WRITE(oi_json_write_string(writer, "function", 8));
        WRITE(oi_json_write_object_key(writer, "function", 8));
        WRITE(oi_json_write_object_begin(writer));
        WRITE(oi_json_write_object_key(writer, "name", 4));
        WRITE(oi_json_write_string(writer, tools[i].name,
                                   strlen(tools[i].name)));
        WRITE(oi_json_write_object_key(writer, "description", 11));
        WRITE(oi_json_write_string(writer, tools[i].description,
                                   strlen(tools[i].description)));
        WRITE(oi_json_write_object_key(writer, "parameters", 10));
        WRITE(tools[i].write_parameters(writer));
        WRITE(oi_json_write_object_end(writer));
        WRITE(oi_json_write_object_end(writer));
    }
    return OI_OK;
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
        return OI_TOOL_DENY;
    }
    return OI_TOOL_DENY;
}
