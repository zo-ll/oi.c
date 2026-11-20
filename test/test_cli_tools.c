#include "cli_tools.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

static oi_json_value *parse(oi_arena *arena, const char *text) {
    oi_json_parser *parser = oi_json_parser_create(arena);
    oi_json_value *root = NULL;

    CHECK(parser != NULL);
    if (parser == NULL) {
        return NULL;
    }
    CHECK_EQ(oi_json_parser_feed(parser, text, strlen(text)), OI_OK);
    CHECK_EQ(oi_json_parser_finish(parser), OI_OK);
    root = oi_json_parser_root(parser);
    oi_json_parser_destroy(parser);
    return root;
}

TEST(advertised_tools_match_the_registered_tools) {
    static const char *const names[] = {"read", "edit", "write", "bash"};
    oi_tool_registry *registry = oi_tool_registry_create();
    oi_json_writer *writer = oi_json_writer_create();
    oi_arena *arena = oi_arena_create(0);
    oi_json_value *advertised;

    CHECK(registry != NULL);
    CHECK(writer != NULL);
    CHECK_EQ(oi_cli_tools_register(registry, "oi"), OI_OK);
    CHECK_EQ(oi_json_write_array_begin(writer), OI_OK);
    CHECK_EQ(oi_cli_tools_write_definitions(writer), OI_OK);
    CHECK_EQ(oi_json_write_array_end(writer), OI_OK);
    advertised = parse(arena, oi_json_writer_data(writer, NULL));
    CHECK_EQ(oi_json_array_len(advertised), 4);

    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const oi_json_value *function =
            oi_json_object_get(oi_json_array_get(advertised, i), "function");
        const char *name;
        size_t name_len;
        const char *schema = oi_tool_registry_schema(registry, names[i]);
        char parameters[1024];

        CHECK_EQ(oi_json_get_string(oi_json_object_get(function, "name"),
                                    &name, &name_len),
                 OI_OK);
        CHECK_EQ(name_len, strlen(names[i]));
        CHECK(memcmp(name, names[i], name_len) == 0);
        CHECK(oi_json_object_get(function, "description") != NULL);
        CHECK(schema != NULL);
        snprintf(parameters, sizeof parameters,
                 "\"name\":\"%s\",\"description\":", names[i]);
        CHECK(strstr(oi_json_writer_data(writer, NULL), parameters) != NULL);
        snprintf(parameters, sizeof parameters, "\"parameters\":%s}}",
                 schema != NULL ? schema : "");
        CHECK(strstr(oi_json_writer_data(writer, NULL), parameters) != NULL);
    }
    CHECK_STREQ(oi_tool_registry_schema(registry, "bash"),
                "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":"
                "\"string\"}},\"required\":[\"command\"],"
                "\"additionalProperties\":false}");
    CHECK(oi_tool_registry_schema(registry, "shell") == NULL);

    oi_arena_destroy(arena);
    oi_json_writer_destroy(writer);
    oi_tool_registry_destroy(registry);
}

TEST(tools_fall_into_categories) {
    oi_arena *arena = oi_arena_create(0);

    CHECK_EQ(oi_cli_tool_category_of(
                 "read", parse(arena, "{\"path\":\"Makefile\"}")),
             OI_CLI_TOOL_READ_IN_PROJECT);
    CHECK_EQ(oi_cli_tool_category_of(
                 "read", parse(arena, "{\"path\":\"src/missing.c\"}")),
             OI_CLI_TOOL_READ_IN_PROJECT);
    CHECK_EQ(oi_cli_tool_category_of(
                 "read", parse(arena, "{\"path\":\"/etc/passwd\"}")),
             OI_CLI_TOOL_FILE_ACCESS);
    CHECK_EQ(oi_cli_tool_category_of(
                 "read", parse(arena, "{\"path\":\"../outside\"}")),
             OI_CLI_TOOL_FILE_ACCESS);
    CHECK_EQ(oi_cli_tool_category_of("read", parse(arena, "{}")),
             OI_CLI_TOOL_FILE_ACCESS);
    CHECK_EQ(oi_cli_tool_category_of(
                 "edit", parse(arena, "{\"path\":\"Makefile\"}")),
             OI_CLI_TOOL_FILE_ACCESS);
    CHECK_EQ(oi_cli_tool_category_of(
                 "write", parse(arena, "{\"path\":\"Makefile\"}")),
             OI_CLI_TOOL_FILE_ACCESS);
    CHECK_EQ(oi_cli_tool_category_of(
                 "bash", parse(arena, "{\"command\":\"true\"}")),
             OI_CLI_TOOL_COMMAND);
    oi_arena_destroy(arena);
}

TEST(file_approval_does_not_approve_commands) {
    oi_arena *arena = oi_arena_create(0);
    oi_json_value *read_inside = parse(arena, "{\"path\":\"Makefile\"}");
    oi_json_value *read_outside = parse(arena, "{\"path\":\"/etc/passwd\"}");
    oi_json_value *edit = parse(arena, "{\"path\":\"Makefile\"}");
    oi_json_value *command = parse(arena, "{\"command\":\"true\"}");
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};

    CHECK_EQ(oi_cli_tool_permission("read", read_inside, &permission),
             OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission_noninteractive("read", read_inside,
                                                   &permission),
             OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission("read", read_outside, &permission),
             OI_TOOL_ASK);
    CHECK_EQ(oi_cli_tool_permission("edit", edit, &permission), OI_TOOL_ASK);
    CHECK_EQ(oi_cli_tool_permission("bash", command, &permission),
             OI_TOOL_ASK);

    oi_cli_permission_allow_category(&permission, OI_CLI_TOOL_FILE_ACCESS);
    CHECK_EQ(oi_cli_tool_permission("edit", edit, &permission),
             OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission("write", edit, &permission),
             OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission("read", read_outside, &permission),
             OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission("bash", command, &permission),
             OI_TOOL_ASK);
    CHECK_EQ(oi_cli_tool_permission_noninteractive("bash", command,
                                                   &permission),
             OI_TOOL_DENY);

    oi_cli_permission_set_policy(&permission, OI_CLI_TOOLS_ASK);
    oi_cli_permission_allow_category(&permission, OI_CLI_TOOL_COMMAND);
    CHECK_EQ(oi_cli_tool_permission("bash", command, &permission),
             OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission("edit", edit, &permission), OI_TOOL_ASK);
    oi_arena_destroy(arena);
}

TEST(interactive_permission_defers_ask_to_the_embedder) {
    oi_arena *arena = oi_arena_create(0);
    oi_json_value *command = parse(arena, "{\"command\":\"true\"}");
    oi_json_value *read_inside = parse(arena, "{\"path\":\"Makefile\"}");
    struct oi_cli_permission allow = {.policy = OI_CLI_TOOLS_ALLOW};
    struct oi_cli_permission deny = {.policy = OI_CLI_TOOLS_DENY};
    struct oi_cli_permission ask = {.policy = OI_CLI_TOOLS_ASK};

    CHECK_EQ(oi_cli_tool_permission("bash", command, &allow), OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission("bash", command, &deny), OI_TOOL_DENY);
    CHECK_EQ(oi_cli_tool_permission("bash", command, &ask), OI_TOOL_ASK);
    CHECK_EQ(oi_cli_tool_permission("read", read_inside, &deny),
             OI_TOOL_DENY);
    oi_arena_destroy(arena);
}

TEST(noninteractive_permission_never_defers) {
    oi_arena *arena = oi_arena_create(0);
    oi_json_value *command = parse(arena, "{\"command\":\"true\"}");
    struct oi_cli_permission allow = {.policy = OI_CLI_TOOLS_ALLOW};
    struct oi_cli_permission deny = {.policy = OI_CLI_TOOLS_DENY};
    struct oi_cli_permission ask = {.policy = OI_CLI_TOOLS_ASK};

    CHECK_EQ(oi_cli_tool_permission_noninteractive("bash", command, &allow),
             OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission_noninteractive("bash", command, &deny),
             OI_TOOL_DENY);
    CHECK_EQ(oi_cli_tool_permission_noninteractive("bash", command, &ask),
             OI_TOOL_DENY);
    oi_arena_destroy(arena);
}

TEST(setting_a_policy_clears_the_process_approvals) {
    struct oi_cli_permission permission = {.policy = OI_CLI_TOOLS_ASK};

    oi_cli_permission_allow_category(&permission, OI_CLI_TOOL_FILE_ACCESS);
    oi_cli_permission_allow_category(&permission, OI_CLI_TOOL_COMMAND);
    oi_cli_permission_set_policy(&permission, OI_CLI_TOOLS_ASK);
    CHECK_EQ(permission.file_access_allowed, 0);
    CHECK_EQ(permission.commands_allowed, 0);
}

int main(void) {
    RUN(advertised_tools_match_the_registered_tools);
    RUN(tools_fall_into_categories);
    RUN(file_approval_does_not_approve_commands);
    RUN(interactive_permission_defers_ask_to_the_embedder);
    RUN(noninteractive_permission_never_defers);
    RUN(setting_a_policy_clears_the_process_approvals);
    return oi_test_report();
}
