#include "cli_command_dispatch.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static char *read_stream(FILE *stream) {
    long len;
    char *text;

    CHECK_EQ(fflush(stream), 0);
    CHECK_EQ(fseek(stream, 0, SEEK_END), 0);
    len = ftell(stream);
    CHECK(len >= 0);
    CHECK_EQ(fseek(stream, 0, SEEK_SET), 0);
    text = malloc((size_t)len + 1);
    CHECK(text != NULL);
    if (text == NULL) {
        return NULL;
    }
    CHECK_EQ(fread(text, 1, (size_t)len, stream), (size_t)len);
    text[len] = '\0';
    return text;
}

static oi_status dispatch(
    const char *text, struct oi_cli_command_context *context,
    enum oi_cli_command_result *result) {
    struct oi_cli_command_parse parsed;
    oi_status status =
        oi_cli_command_parse_text(text, strlen(text), &parsed);
    return status == OI_OK
               ? oi_cli_command_dispatch(&parsed, context, result)
               : status;
}

TEST(help_lists_commands_and_exit_requests_exit) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        out, err, "test-model", &permission, NULL,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK(out != NULL);
    CHECK(err != NULL);
    CHECK_EQ(dispatch("/help", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_CONTINUE);
    output = read_stream(out);
    CHECK(strstr(output, "/permissions") != NULL);
    CHECK(strstr(output, "Ctrl+J") != NULL);
    free(output);
    CHECK_EQ(dispatch("/exit", &context, &result), OI_OK);
    CHECK_EQ(result, OI_CLI_COMMAND_EXIT_REPL);
    fclose(out);
    fclose(err);
}

TEST(permissions_are_process_scoped_and_validated) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        out, err, "test-model", &permission, "session-1",
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/permissions allow", &context, &result), OI_OK);
    CHECK_EQ(permission.policy, OI_CLI_TOOLS_ALLOW);
    CHECK_EQ(dispatch("/permissions invalid", &context, &result), OI_OK);
    CHECK_EQ(permission.policy, OI_CLI_TOOLS_ALLOW);
    errors = read_stream(err);
    CHECK(strstr(errors, "usage") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

TEST(status_reports_runtime_without_secrets) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_DENY};
    struct oi_cli_command_context context = {
        out, err, "test-model", &permission, NULL,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/status", &context, &result), OI_OK);
    output = read_stream(out);
    CHECK(strstr(output, "Session: (not created)") != NULL);
    CHECK(strstr(output, "Model: test-model") != NULL);
    CHECK(strstr(output, "Permissions: deny") != NULL);
    CHECK(strstr(output, "CWD:") != NULL);
    free(output);
    fclose(out);
    fclose(err);
}

int main(void) {
    RUN(help_lists_commands_and_exit_requests_exit);
    RUN(permissions_are_process_scoped_and_validated);
    RUN(status_reports_runtime_without_secrets);
    return oi_test_report();
}
