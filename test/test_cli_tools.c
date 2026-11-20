#include "cli_tools.h"
#include "test.h"

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

int main(void) {
    RUN(interactive_permission_defers_ask_to_the_embedder);
    RUN(noninteractive_permission_never_defers);
    return oi_test_report();
}
