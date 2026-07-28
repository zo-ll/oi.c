#include "oi/config.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_path[256];

static const char *fresh_path(void) {
    static int counter = 0;
    snprintf(g_path, sizeof g_path, "/tmp/oi_config_test_%d_%d.conf",
             (int)getpid(), counter++);
    unlink(g_path);
    return g_path;
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    fputs(content, f);
    fclose(f);
}

/* --- defaults --- */

TEST(defaults_are_sane) {
    oi_config cfg;
    CHECK_EQ(oi_config_init_defaults(&cfg), OI_OK);
    CHECK(cfg.api_key == NULL);
    CHECK(cfg.ca_file == NULL);
    CHECK_STREQ(cfg.host, "api.openai.com");
    CHECK_EQ(cfg.port, 443);
    CHECK_EQ(cfg.use_tls, 1);
    CHECK_STREQ(cfg.path, "/v1/chat/completions");
    CHECK(cfg.model != NULL);
    CHECK(cfg.timeout_ms > 0);
    oi_config_free(&cfg);
}

TEST(init_defaults_rejects_null) {
    CHECK_EQ(oi_config_init_defaults(NULL), OI_ERR_INVAL);
}

/* --- oi_config_set --- */

TEST(set_string_fields) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);

    CHECK_EQ(oi_config_set(&cfg, "host", "localhost"), OI_OK);
    CHECK_STREQ(cfg.host, "localhost");

    CHECK_EQ(oi_config_set(&cfg, "api_key", "sk-test"), OI_OK);
    CHECK_STREQ(cfg.api_key, "sk-test");

    CHECK_EQ(oi_config_set(&cfg, "ca_file", "/etc/ca.pem"), OI_OK);
    CHECK_STREQ(cfg.ca_file, "/etc/ca.pem");

    CHECK_EQ(oi_config_set(&cfg, "path", "/v2/x"), OI_OK);
    CHECK_STREQ(cfg.path, "/v2/x");

    CHECK_EQ(oi_config_set(&cfg, "model", "gpt-5"), OI_OK);
    CHECK_STREQ(cfg.model, "gpt-5");

    oi_config_free(&cfg);
}

TEST(set_overwrites_and_frees_prior_value) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_set(&cfg, "host", "first.example.com"), OI_OK);
    CHECK_EQ(oi_config_set(&cfg, "host", "second.example.com"), OI_OK);
    CHECK_STREQ(cfg.host, "second.example.com"); /* leak of "first" checked by valgrind */
    oi_config_free(&cfg);
}

TEST(set_int_fields) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_set(&cfg, "port", "8080"), OI_OK);
    CHECK_EQ(cfg.port, 8080);
    CHECK_EQ(oi_config_set(&cfg, "timeout_ms", "5000"), OI_OK);
    CHECK_EQ(cfg.timeout_ms, 5000);
    CHECK_EQ(oi_config_set(&cfg, "port", "-1"), OI_ERR_PARSE);
    CHECK_EQ(oi_config_set(&cfg, "port", "0"), OI_ERR_PARSE);
    CHECK_EQ(oi_config_set(&cfg, "port", "65536"), OI_ERR_PARSE);
    CHECK_EQ(cfg.port, 8080);
    CHECK_EQ(oi_config_set(&cfg, "timeout_ms", "0"), OI_ERR_PARSE);
    CHECK_EQ(oi_config_set(&cfg, "timeout_ms", "-1"), OI_ERR_PARSE);
    CHECK_EQ(cfg.timeout_ms, 5000);
    oi_config_free(&cfg);
}

TEST(set_int_rejects_garbage) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_set(&cfg, "port", "not-a-number"), OI_ERR_PARSE);
    CHECK_EQ(oi_config_set(&cfg, "port", "80x0"), OI_ERR_PARSE);
    CHECK_EQ(oi_config_set(&cfg, "port", ""), OI_ERR_PARSE);
    CHECK_EQ(oi_config_set(&cfg, "port", "80 "), OI_ERR_PARSE); /* trailing garbage */
    CHECK_EQ(cfg.port, 443); /* unchanged on rejection */
    oi_config_free(&cfg);
}

TEST(set_bool_field) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);

    static const char *truthy[] = {"true", "TRUE", "1", "yes", "Yes"};
    for (size_t i = 0; i < sizeof truthy / sizeof truthy[0]; i++) {
        cfg.use_tls = -1;
        CHECK_EQ(oi_config_set(&cfg, "use_tls", truthy[i]), OI_OK);
        CHECK_EQ(cfg.use_tls, 1);
    }
    static const char *falsy[] = {"false", "FALSE", "0", "no", "No"};
    for (size_t i = 0; i < sizeof falsy / sizeof falsy[0]; i++) {
        cfg.use_tls = -1;
        CHECK_EQ(oi_config_set(&cfg, "use_tls", falsy[i]), OI_OK);
        CHECK_EQ(cfg.use_tls, 0);
    }
    CHECK_EQ(oi_config_set(&cfg, "use_tls", "maybe"), OI_ERR_PARSE);

    oi_config_free(&cfg);
}

TEST(set_rejects_unknown_key) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_set(&cfg, "bogus_key", "x"), OI_ERR_NOTFOUND);
    oi_config_free(&cfg);
}

TEST(set_rejects_null_args) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_set(NULL, "host", "x"), OI_ERR_INVAL);
    CHECK_EQ(oi_config_set(&cfg, NULL, "x"), OI_ERR_INVAL);
    CHECK_EQ(oi_config_set(&cfg, "host", NULL), OI_ERR_INVAL);
    oi_config_free(&cfg);
}

/* --- env --- */

TEST(load_env_reads_api_key) {
    setenv("OI_API_KEY", "sk-from-env", 1);
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_env(&cfg), OI_OK);
    CHECK_STREQ(cfg.api_key, "sk-from-env");
    oi_config_free(&cfg);
    unsetenv("OI_API_KEY");
}

TEST(load_env_unset_is_noop) {
    unsetenv("OI_API_KEY");
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_env(&cfg), OI_OK);
    CHECK(cfg.api_key == NULL);
    oi_config_free(&cfg);
}

TEST(load_env_overwrites_prior_value) {
    setenv("OI_API_KEY", "sk-env-value", 1);
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_set(&cfg, "api_key", "sk-stale"), OI_OK);
    CHECK_EQ(oi_config_load_env(&cfg), OI_OK);
    CHECK_STREQ(cfg.api_key, "sk-env-value");
    oi_config_free(&cfg);
    unsetenv("OI_API_KEY");
}

/* --- file --- */

TEST(load_file_applies_settings) {
    const char *path = fresh_path();
    write_file(path,
               "# a comment\n"
               "\n"
               "host = my.host.example\n"
               "port=9090\n"
               "  model = custom-model  \n"
               "use_tls = false\n");

    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_file(&cfg, path), OI_OK);
    CHECK_STREQ(cfg.host, "my.host.example");
    CHECK_EQ(cfg.port, 9090);
    CHECK_STREQ(cfg.model, "custom-model");
    CHECK_EQ(cfg.use_tls, 0);

    oi_config_free(&cfg);
    unlink(path);
}

TEST(load_file_forbids_api_key) {
    const char *path = fresh_path();
    write_file(path, "host = ok.example\napi_key = sk-leaked\n");

    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_file(&cfg, path), OI_ERR_DENIED);
    /* the earlier "host" line, before the offending one, still applied */
    CHECK_STREQ(cfg.host, "ok.example");
    CHECK(cfg.api_key == NULL); /* the forbidden line must not have applied */

    oi_config_free(&cfg);
    unlink(path);
}

TEST(load_file_malformed_line_stops_parsing) {
    const char *path = fresh_path();
    write_file(path, "host = ok.example\nthis line has no equals sign\nmodel = never-reached\n");

    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_file(&cfg, path), OI_ERR_PARSE);
    CHECK_STREQ(cfg.host, "ok.example");
    CHECK(strcmp(cfg.model, "never-reached") != 0);

    oi_config_free(&cfg);
    unlink(path);
}

TEST(load_file_unknown_key_stops_parsing) {
    const char *path = fresh_path();
    write_file(path, "not_a_real_setting = x\n");
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_file(&cfg, path), OI_ERR_NOTFOUND);
    oi_config_free(&cfg);
    unlink(path);
}

TEST(load_file_bad_value_stops_parsing) {
    const char *path = fresh_path();
    write_file(path, "port = not-an-int\n");
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_file(&cfg, path), OI_ERR_PARSE);
    oi_config_free(&cfg);
    unlink(path);
}

TEST(load_file_missing_is_not_found) {
    const char *path = fresh_path(); /* unlinked, never written */
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_file(&cfg, path), OI_ERR_NOTFOUND);
    oi_config_free(&cfg);
}

TEST(load_file_rejects_null_args) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_file(NULL, "/tmp/x"), OI_ERR_INVAL);
    CHECK_EQ(oi_config_load_file(&cfg, NULL), OI_ERR_INVAL);
    oi_config_free(&cfg);
}

TEST(load_file_empty_and_comment_only_is_ok) {
    const char *path = fresh_path();
    write_file(path, "# nothing here\n\n   \n");
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    CHECK_EQ(oi_config_load_file(&cfg, path), OI_OK);
    CHECK_STREQ(cfg.host, "api.openai.com"); /* untouched */
    oi_config_free(&cfg);
    unlink(path);
}

/* --- free --- */

TEST(free_null_safe) { oi_config_free(NULL); }

TEST(free_on_zeroed_struct_is_safe) {
    oi_config cfg;
    memset(&cfg, 0, sizeof cfg);
    oi_config_free(&cfg); /* every field NULL -- must not crash */
}

TEST(free_nulls_out_fields) {
    oi_config cfg;
    oi_config_init_defaults(&cfg);
    oi_config_set(&cfg, "api_key", "sk-x");
    oi_config_set(&cfg, "ca_file", "/tmp/ca.pem");
    oi_config_free(&cfg);
    CHECK(cfg.api_key == NULL);
    CHECK(cfg.host == NULL);
    CHECK(cfg.ca_file == NULL);
    CHECK(cfg.path == NULL);
    CHECK(cfg.model == NULL);
    oi_config_free(&cfg); /* double-free-safe now that fields are NULL */
}

/* --- full precedence: defaults -> file -> env -> "CLI" (a second set) --- */

TEST(full_precedence_chain) {
    const char *path = fresh_path();
    write_file(path, "host = file.example\nmodel = file-model\nport = 1111\n");
    setenv("OI_API_KEY", "sk-env", 1);

    oi_config cfg;
    CHECK_EQ(oi_config_init_defaults(&cfg), OI_OK);
    CHECK_EQ(oi_config_load_file(&cfg, path), OI_OK);
    CHECK_EQ(oi_config_load_env(&cfg), OI_OK);
    /* simulates a CLI flag "--host cli.example", applied last */
    CHECK_EQ(oi_config_set(&cfg, "host", "cli.example"), OI_OK);

    CHECK_STREQ(cfg.host, "cli.example");   /* CLI beat the file */
    CHECK_STREQ(cfg.model, "file-model");   /* file beat the default */
    CHECK_EQ(cfg.port, 1111);
    CHECK_STREQ(cfg.api_key, "sk-env");     /* env, nothing else set it */

    oi_config_free(&cfg);
    unlink(path);
    unsetenv("OI_API_KEY");
}

int main(void) {
    RUN(defaults_are_sane);
    RUN(init_defaults_rejects_null);
    RUN(set_string_fields);
    RUN(set_overwrites_and_frees_prior_value);
    RUN(set_int_fields);
    RUN(set_int_rejects_garbage);
    RUN(set_bool_field);
    RUN(set_rejects_unknown_key);
    RUN(set_rejects_null_args);
    RUN(load_env_reads_api_key);
    RUN(load_env_unset_is_noop);
    RUN(load_env_overwrites_prior_value);
    RUN(load_file_applies_settings);
    RUN(load_file_forbids_api_key);
    RUN(load_file_malformed_line_stops_parsing);
    RUN(load_file_unknown_key_stops_parsing);
    RUN(load_file_bad_value_stops_parsing);
    RUN(load_file_missing_is_not_found);
    RUN(load_file_rejects_null_args);
    RUN(load_file_empty_and_comment_only_is_ok);
    RUN(free_null_safe);
    RUN(free_on_zeroed_struct_is_safe);
    RUN(free_nulls_out_fields);
    RUN(full_precedence_chain);
    return oi_test_report();
}
