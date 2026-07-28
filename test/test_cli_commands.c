#include "cli_commands.h"
#include "test.h"

#include <string.h>

TEST(registry_contains_the_agreed_commands) {
    static const char *expected[] = {
        "/help",        "/exit",   "/session", "/model",
        "/permissions", "/status", "/compact", "/cwd",
    };
    size_t i;

    CHECK_EQ(oi_cli_command_count(), sizeof expected / sizeof expected[0]);
    for (i = 0; i < sizeof expected / sizeof expected[0]; i++) {
        const struct oi_cli_command_definition *command =
            oi_cli_command_at(i);
        CHECK(command != NULL);
        CHECK_STREQ(command->name, expected[i]);
        CHECK(command->usage != NULL);
        CHECK(command->description != NULL);
    }
    CHECK(oi_cli_command_at(oi_cli_command_count()) == NULL);
}

TEST(filter_matches_prefixes_only_in_the_first_token) {
    size_t matches[8];
    size_t count;

    count = oi_cli_command_filter("/", 1, matches, 8);
    CHECK_EQ(count, oi_cli_command_count());
    count = oi_cli_command_filter("/s", 2, matches, 8);
    CHECK_EQ(count, 2);
    CHECK_STREQ(oi_cli_command_at(matches[0])->name, "/session");
    CHECK_STREQ(oi_cli_command_at(matches[1])->name, "/status");
    count = oi_cli_command_filter("/per", 4, matches, 8);
    CHECK_EQ(count, 1);
    CHECK_STREQ(oi_cli_command_at(matches[0])->name, "/permissions");
    CHECK_EQ(oi_cli_command_filter("//literal", 9, matches, 8), 0);
    CHECK_EQ(oi_cli_command_filter("/model arg", 10, matches, 8), 0);
    CHECK_EQ(oi_cli_command_filter("plain", 5, matches, 8), 0);
}

TEST(parse_distinguishes_messages_commands_and_literal_slashes) {
    struct oi_cli_command_parse parsed;

    CHECK_EQ(oi_cli_command_parse_text("hello", 5, &parsed), OI_OK);
    CHECK_EQ(parsed.kind, OI_CLI_COMMAND_PARSE_MESSAGE);
    CHECK_EQ(oi_cli_command_parse_text("//help", 6, &parsed), OI_OK);
    CHECK_EQ(parsed.kind, OI_CLI_COMMAND_PARSE_LITERAL_SLASH);
    CHECK_EQ(parsed.arguments_len, 5);
    CHECK(memcmp(parsed.arguments, "/help", 5) == 0);
    CHECK_EQ(oi_cli_command_parse_text("/unknown", 8, &parsed), OI_OK);
    CHECK_EQ(parsed.kind, OI_CLI_COMMAND_PARSE_UNKNOWN);
}

TEST(parse_returns_trimmed_borrowed_arguments) {
    static const char text[] = "/model   gpt-test \t";
    struct oi_cli_command_parse parsed;

    CHECK_EQ(oi_cli_command_parse_text(text, sizeof text - 1, &parsed),
             OI_OK);
    CHECK_EQ(parsed.kind, OI_CLI_COMMAND_PARSE_COMMAND);
    CHECK_EQ(parsed.command->id, OI_CLI_COMMAND_MODEL);
    CHECK_EQ(parsed.arguments_len, 8);
    CHECK(memcmp(parsed.arguments, "gpt-test", 8) == 0);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_command_parse parsed;

    CHECK_EQ(oi_cli_command_parse_text(NULL, 1, &parsed), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_command_parse_text("", 0, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_command_filter(NULL, 0, NULL, 0), 0);
}

int main(void) {
    RUN(registry_contains_the_agreed_commands);
    RUN(filter_matches_prefixes_only_in_the_first_token);
    RUN(parse_distinguishes_messages_commands_and_literal_slashes);
    RUN(parse_returns_trimmed_borrowed_arguments);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
