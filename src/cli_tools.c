#define _GNU_SOURCE

#include "cli_tools.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "oi/arena.h"
#include "oi/json.h"

#define WRITE(expr)                                                           \
    do {                                                                      \
        oi_status write_status = (expr);                                      \
        if (write_status != OI_OK) {                                          \
            return write_status;                                              \
        }                                                                     \
    } while (0)

struct tool_property {
    const char *name;
    const char *type;
    int required;
};

struct tool_definition {
    const char *name;
    const char *description;
    const struct tool_property *properties;
    size_t property_count;
};

static const struct tool_property read_properties[] = {
    {"path", "string", 1},
    {"offset", "integer", 0},
    {"limit", "integer", 0},
};

static const struct tool_property edit_properties[] = {
    {"path", "string", 1},
    {"old_text", "string", 1},
    {"new_text", "string", 1},
};

static const struct tool_property write_properties[] = {
    {"path", "string", 1},
    {"content", "string", 1},
};

static const struct tool_property bash_properties[] = {
    {"command", "string", 1},
};

#define PROPERTIES(list) list, sizeof list / sizeof list[0]

static const struct tool_definition tools[] = {
    {"read",
     "Read lines from an existing file. Each line starts with its line "
     "number. offset is the first line to read (default 1). limit is the "
     "maximum number of lines (default 2000).",
     PROPERTIES(read_properties)},
    {"edit",
     "Replace an exact block of text in an existing file. If old_text is "
     "absent or occurs more than once, the file does not change.",
     PROPERTIES(edit_properties)},
    {"write", "Create a file or replace its full contents.",
     PROPERTIES(write_properties)},
    {"bash",
     "Execute a command with bash -c in the session directory. Returns the "
     "command output and exit status.",
     PROPERTIES(bash_properties)},
};

static const char *tool_program;

static oi_status write_parameters(oi_json_writer *writer,
                                  const struct tool_definition *tool) {
    WRITE(oi_json_write_object_begin(writer));
    WRITE(oi_json_write_object_key(writer, "type", 4));
    WRITE(oi_json_write_string(writer, "object", 6));
    WRITE(oi_json_write_object_key(writer, "properties", 10));
    WRITE(oi_json_write_object_begin(writer));
    for (size_t i = 0; i < tool->property_count; i++) {
        const struct tool_property *property = &tool->properties[i];
        WRITE(oi_json_write_object_key(writer, property->name,
                                       strlen(property->name)));
        WRITE(oi_json_write_object_begin(writer));
        WRITE(oi_json_write_object_key(writer, "type", 4));
        WRITE(oi_json_write_string(writer, property->type,
                                   strlen(property->type)));
        WRITE(oi_json_write_object_end(writer));
    }
    WRITE(oi_json_write_object_end(writer));
    WRITE(oi_json_write_object_key(writer, "required", 8));
    WRITE(oi_json_write_array_begin(writer));
    for (size_t i = 0; i < tool->property_count; i++) {
        const struct tool_property *property = &tool->properties[i];
        if (property->required) {
            WRITE(oi_json_write_string(writer, property->name,
                                       strlen(property->name)));
        }
    }
    WRITE(oi_json_write_array_end(writer));
    WRITE(oi_json_write_object_key(writer, "additionalProperties", 20));
    WRITE(oi_json_write_bool(writer, 0));
    return oi_json_write_object_end(writer);
}

static oi_status tool_argv(const oi_json_value *args, oi_arena *arena,
                           void *user_data, char ***out_argv) {
    char **argv = oi_arena_alloc(arena, 4 * sizeof *argv);

    (void)args;
    if (argv == NULL) {
        return OI_ERR_NOMEM;
    }
    argv[0] = (char *)tool_program;
    argv[1] = (char *)"--run-tool";
    argv[2] = user_data;
    argv[3] = NULL;
    *out_argv = argv;
    return OI_OK;
}

oi_status oi_cli_tools_register(oi_tool_registry *registry,
                                const char *program) {
    tool_program = program;
    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
        oi_json_writer *schema = oi_json_writer_create();
        oi_status status;

        if (schema == NULL) {
            return OI_ERR_NOMEM;
        }
        status = write_parameters(schema, &tools[i]);
        if (status == OI_OK) {
            status = oi_tool_registry_add(registry, tools[i].name,
                                          oi_json_writer_data(schema, NULL),
                                          tool_argv, (void *)tools[i].name);
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
        WRITE(write_parameters(writer, &tools[i]));
        WRITE(oi_json_write_object_end(writer));
        WRITE(oi_json_write_object_end(writer));
    }
    return OI_OK;
}

static int resolve_existing(const char *path, char *resolved) {
    char *slash;
    char parent[PATH_MAX];

    if (realpath(path, resolved) != NULL) {
        return 1;
    }
    if (errno != ENOENT || strlen(path) >= sizeof parent) {
        return 0;
    }
    strcpy(parent, path);
    slash = strrchr(parent, '/');
    if (slash == NULL) {
        strcpy(parent, ".");
    } else if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    return realpath(parent, resolved) != NULL;
}

static int path_in_project(const char *path) {
    char project[PATH_MAX];
    char resolved[PATH_MAX];
    size_t project_len;

    if (getcwd(project, sizeof project) == NULL ||
        !resolve_existing(path, resolved)) {
        return 0;
    }
    project_len = strlen(project);
    if (project_len == 1) {
        return 1;
    }
    return strncmp(resolved, project, project_len) == 0 &&
           (resolved[project_len] == '\0' || resolved[project_len] == '/');
}

enum oi_cli_tool_category oi_cli_tool_category_of(const char *tool_name,
                                                  const oi_json_value *args) {
    const char *path;
    size_t path_len;

    if (strcmp(tool_name, "read") == 0) {
        if (oi_json_get_string(oi_json_object_get(args, "path"), &path,
                               &path_len) == OI_OK &&
            path_len > 0 && strlen(path) == path_len &&
            path_in_project(path)) {
            return OI_CLI_TOOL_READ_IN_PROJECT;
        }
        return OI_CLI_TOOL_FILE_ACCESS;
    }
    if (strcmp(tool_name, "edit") == 0 || strcmp(tool_name, "write") == 0) {
        return OI_CLI_TOOL_FILE_ACCESS;
    }
    return OI_CLI_TOOL_COMMAND;
}

void oi_cli_permission_set_policy(struct oi_cli_permission *permission,
                                  oi_cli_tool_policy policy) {
    permission->policy = policy;
    permission->file_access_allowed = 0;
    permission->commands_allowed = 0;
}

void oi_cli_permission_allow_category(struct oi_cli_permission *permission,
                                      enum oi_cli_tool_category category) {
    switch (category) {
    case OI_CLI_TOOL_READ_IN_PROJECT:
        break;
    case OI_CLI_TOOL_FILE_ACCESS:
        permission->file_access_allowed = 1;
        break;
    case OI_CLI_TOOL_COMMAND:
        permission->commands_allowed = 1;
        break;
    }
}

static oi_tool_decision decide(const struct oi_cli_permission *permission,
                               const char *tool_name,
                               const oi_json_value *args,
                               oi_tool_decision unapproved) {
    switch (permission->policy) {
    case OI_CLI_TOOLS_ALLOW:
        return OI_TOOL_ALLOW;
    case OI_CLI_TOOLS_DENY:
        return OI_TOOL_DENY;
    case OI_CLI_TOOLS_ASK:
        break;
    }
    switch (oi_cli_tool_category_of(tool_name, args)) {
    case OI_CLI_TOOL_READ_IN_PROJECT:
        return OI_TOOL_ALLOW;
    case OI_CLI_TOOL_FILE_ACCESS:
        return permission->file_access_allowed ? OI_TOOL_ALLOW : unapproved;
    case OI_CLI_TOOL_COMMAND:
        return permission->commands_allowed ? OI_TOOL_ALLOW : unapproved;
    }
    return OI_TOOL_DENY;
}

oi_tool_decision oi_cli_tool_permission(const char *tool_name,
                                         const oi_json_value *args,
                                         void *user_data) {
    return decide(user_data, tool_name, args, OI_TOOL_ASK);
}

oi_tool_decision oi_cli_tool_permission_noninteractive(
    const char *tool_name, const oi_json_value *args, void *user_data) {
    return decide(user_data, tool_name, args, OI_TOOL_DENY);
}
