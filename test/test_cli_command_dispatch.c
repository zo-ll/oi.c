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
        out, err, "test-model", &permission, NULL, NULL, NULL, NULL, NULL,
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
        out, err, "test-model", &permission, "session-1", NULL, NULL, NULL,
        NULL,
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
        out, err, "test-model", &permission, NULL, NULL, NULL, NULL, NULL,
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

struct model_cb_call {
    char name[64];
    size_t name_len;
    int call_count;
    oi_status result;
};

static oi_status record_set_model(void *user_data, const char *name,
                                  size_t name_len) {
    struct model_cb_call *call = user_data;
    call->call_count++;
    CHECK(name_len < sizeof call->name);
    memcpy(call->name, name, name_len);
    call->name[name_len] = '\0';
    call->name_len = name_len;
    return call->result;
}

TEST(model_with_no_argument_prints_the_active_model) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        out, err, "current-model", &permission, NULL, NULL, NULL, NULL, NULL,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/model", &context, &result), OI_OK);
    output = read_stream(out);
    CHECK_STREQ(output, "Model: current-model\n");
    free(output);
    fclose(out);
    fclose(err);
}

TEST(model_with_no_callback_reports_unavailable) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        out, err, "current-model", &permission, NULL, NULL, NULL, NULL, NULL,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/model new-model", &context, &result), OI_OK);
    errors = read_stream(err);
    CHECK(strstr(errors, "not available") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

TEST(model_with_argument_invokes_the_callback_with_exact_bytes) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct model_cb_call call = {.result = OI_OK};
    struct oi_cli_command_context context = {
        out,  err, "current-model", &permission, NULL,
        record_set_model, &call, NULL, NULL,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/model gpt-new", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);
    CHECK_STREQ(call.name, "gpt-new");
    output = read_stream(out);
    CHECK_STREQ(output, "Model: gpt-new\n");
    free(output);
    fclose(out);
    fclose(err);
}

TEST(model_callback_failure_is_reported_and_stays_in_the_repl) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct model_cb_call call = {.result = OI_ERR_INVAL};
    struct oi_cli_command_context context = {
        out,  err, "current-model", &permission, NULL,
        record_set_model, &call, NULL, NULL,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/model bad-model", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);
    errors = read_stream(err);
    CHECK(strstr(errors, "could not change the model") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

TEST(model_callback_structural_failure_propagates) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct model_cb_call call = {.result = OI_ERR_IO};
    struct oi_cli_command_context context = {
        out,  err, "current-model", &permission, NULL,
        record_set_model, &call, NULL, NULL,
    };
    enum oi_cli_command_result result;

    CHECK_EQ(dispatch("/model gpt-new", &context, &result), OI_ERR_IO);
    fclose(out);
    fclose(err);
}

TEST(model_rejects_an_oversized_name_without_calling_back) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct model_cb_call call = {.result = OI_OK};
    struct oi_cli_command_context context = {
        out,  err, "current-model", &permission, NULL,
        record_set_model, &call, NULL, NULL,
    };
    enum oi_cli_command_result result;
    char oversized[300] = "/model ";
    char *errors;
    size_t i;

    for (i = strlen(oversized); i < sizeof oversized - 1; i++) {
        oversized[i] = 'a';
    }
    oversized[sizeof oversized - 1] = '\0';

    CHECK_EQ(dispatch(oversized, &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 0);
    errors = read_stream(err);
    CHECK(strstr(errors, "too long") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

struct cwd_cb_call {
    char path[512];
    size_t path_len;
    int call_count;
    oi_status result;
};

static oi_status record_set_cwd(void *user_data, const char *path,
                                size_t path_len) {
    struct cwd_cb_call *call = user_data;
    call->call_count++;
    CHECK(path_len < sizeof call->path);
    memcpy(call->path, path, path_len);
    call->path[path_len] = '\0';
    call->path_len = path_len;
    return call->result;
}

TEST(cwd_with_no_argument_prints_the_process_cwd) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        out, err, "current-model", &permission, NULL, NULL, NULL, NULL, NULL,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/cwd", &context, &result), OI_OK);
    output = read_stream(out);
    CHECK(strstr(output, "CWD:") != NULL);
    free(output);
    fclose(out);
    fclose(err);
}

TEST(cwd_with_no_callback_reports_unavailable) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct oi_cli_command_context context = {
        out, err, "current-model", &permission, NULL, NULL, NULL, NULL, NULL,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/cwd /tmp", &context, &result), OI_OK);
    errors = read_stream(err);
    CHECK(strstr(errors, "not available") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

TEST(cwd_with_argument_invokes_the_callback_with_exact_bytes) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct cwd_cb_call call = {.result = OI_OK};
    struct oi_cli_command_context context = {
        out, err, "current-model", &permission, NULL, NULL, NULL,
        record_set_cwd, &call,
    };
    enum oi_cli_command_result result;
    char *output;

    CHECK_EQ(dispatch("/cwd /tmp/project", &context, &result), OI_OK);
    CHECK_EQ(call.call_count, 1);
    CHECK_STREQ(call.path, "/tmp/project");
    output = read_stream(out);
    CHECK(strstr(output, "CWD:") != NULL);
    free(output);
    fclose(out);
    fclose(err);
}

TEST(cwd_callback_failure_is_reported_and_stays_in_the_repl) {
    FILE *out = tmpfile();
    FILE *err = tmpfile();
    struct oi_cli_permission permission = {OI_CLI_TOOLS_ASK};
    struct cwd_cb_call call = {.result = OI_ERR_INVAL};
    struct oi_cli_command_context context = {
        out, err, "current-model", &permission, NULL, NULL, NULL,
        record_set_cwd, &call,
    };
    enum oi_cli_command_result result;
    char *errors;

    CHECK_EQ(dispatch("/cwd /nonexistent", &context, &result), OI_OK);
    errors = read_stream(err);
    CHECK(strstr(errors, "could not change the working directory") != NULL);
    free(errors);
    fclose(out);
    fclose(err);
}

int main(void) {
    RUN(help_lists_commands_and_exit_requests_exit);
    RUN(permissions_are_process_scoped_and_validated);
    RUN(status_reports_runtime_without_secrets);
    RUN(model_with_no_argument_prints_the_active_model);
    RUN(model_with_no_callback_reports_unavailable);
    RUN(model_with_argument_invokes_the_callback_with_exact_bytes);
    RUN(model_callback_failure_is_reported_and_stays_in_the_repl);
    RUN(model_callback_structural_failure_propagates);
    RUN(model_rejects_an_oversized_name_without_calling_back);
    RUN(cwd_with_no_argument_prints_the_process_cwd);
    RUN(cwd_with_no_callback_reports_unavailable);
    RUN(cwd_with_argument_invokes_the_callback_with_exact_bytes);
    RUN(cwd_callback_failure_is_reported_and_stays_in_the_repl);
    return oi_test_report();
}
