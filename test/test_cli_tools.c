#include "cli_tools.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

TEST(interactive_permission_defers_ask_to_the_embedder) {
    struct oi_cli_permission allow = {OI_CLI_TOOLS_ALLOW};
    struct oi_cli_permission deny = {OI_CLI_TOOLS_DENY};
    struct oi_cli_permission ask = {OI_CLI_TOOLS_ASK};

    CHECK_EQ(oi_cli_tool_permission("shell", NULL, &allow), OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission("shell", NULL, &deny), OI_TOOL_DENY);
    CHECK_EQ(oi_cli_tool_permission("shell", NULL, &ask), OI_TOOL_ASK);
}

TEST(noninteractive_permission_never_defers) {
    struct oi_cli_permission allow = {OI_CLI_TOOLS_ALLOW};
    struct oi_cli_permission deny = {OI_CLI_TOOLS_DENY};
    struct oi_cli_permission ask = {OI_CLI_TOOLS_ASK};

    CHECK_EQ(oi_cli_tool_permission_noninteractive("shell", NULL, &allow),
             OI_TOOL_ALLOW);
    CHECK_EQ(oi_cli_tool_permission_noninteractive("shell", NULL, &deny),
             OI_TOOL_DENY);
    CHECK_EQ(oi_cli_tool_permission_noninteractive("shell", NULL, &ask),
             OI_TOOL_DENY);
}

TEST(advertised_tools_match_the_registered_tools) {
    static const char expected[] =
        "[{\"type\":\"function\",\"function\":{\"name\":\"shell\","
        "\"description\":\"Execute a shell command\",\"parameters\":"
        "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":"
        "\"string\"}},\"required\":[\"command\"],"
        "\"additionalProperties\":false}}}]";
    oi_tool_registry *registry = oi_tool_registry_create();
    oi_json_writer *writer = oi_json_writer_create();
    const char *schema;
    char parameters[512];

    CHECK(registry != NULL);
    CHECK(writer != NULL);
    CHECK_EQ(oi_cli_tools_register(registry), OI_OK);
    CHECK_EQ(oi_json_write_array_begin(writer), OI_OK);
    CHECK_EQ(oi_cli_tools_write_definitions(writer), OI_OK);
    CHECK_EQ(oi_json_write_array_end(writer), OI_OK);
    CHECK_STREQ(oi_json_writer_data(writer, NULL), expected);

    schema = oi_tool_registry_schema(registry, "shell");
    CHECK(schema != NULL);
    snprintf(parameters, sizeof parameters, "\"parameters\":%s}}",
             schema != NULL ? schema : "");
    CHECK(strstr(oi_json_writer_data(writer, NULL), parameters) != NULL);
    CHECK(oi_tool_registry_schema(registry, "bash") == NULL);

    oi_json_writer_destroy(writer);
    oi_tool_registry_destroy(registry);
}

int main(void) {
    RUN(interactive_permission_defers_ask_to_the_embedder);
    RUN(noninteractive_permission_never_defers);
    RUN(advertised_tools_match_the_registered_tools);
    return oi_test_report();
}
