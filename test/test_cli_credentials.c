#include "cli_credentials.h"
#include "test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[64];
static char path[128];

static void make_root(void) {
    strcpy(root, "/tmp/oi-credentials-XXXXXX");
    CHECK(mkdtemp(root) != NULL);
    snprintf(path, sizeof path, "%s/a/oi/credentials.json", root);
}

static void remove_root(void) {
    char command[128];
    snprintf(command, sizeof command, "rm -rf %s", root);
    CHECK_EQ(system(command), 0);
}

static void write_raw(const char *text, mode_t mode) {
    char parent[128];
    snprintf(parent, sizeof parent, "%s/a", root);
    CHECK_EQ(mkdir(parent, 0700), 0);
    snprintf(parent, sizeof parent, "%s/a/oi", root);
    CHECK_EQ(mkdir(parent, 0700), 0);
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    fputs(text, file);
    fclose(file);
    CHECK_EQ(chmod(path, mode), 0);
}

TEST(round_trip_keeps_every_field_in_a_private_file) {
    make_root();
    struct oi_cli_credentials saved;
    oi_cli_credentials_init(&saved);
    saved.active = OI_CLI_PROVIDER_CUSTOM;
    saved.custom_port = 8443;
    saved.custom_use_tls = 1;
    CHECK_EQ(oi_cli_credentials_set(&saved.openai_api_key, "sk-a\"b"), OI_OK);
    CHECK_EQ(oi_cli_credentials_set(&saved.custom_host, "h.test"), OI_OK);
    CHECK_EQ(oi_cli_credentials_set(&saved.custom_path, "/v1/x"), OI_OK);
    CHECK_EQ(oi_cli_credentials_set(&saved.custom_api_key, "ck"), OI_OK);
    CHECK_EQ(oi_cli_credentials_write(path, &saved), OI_OK);
    oi_cli_credentials_free(&saved);

    struct stat info;
    CHECK_EQ(stat(path, &info), 0);
    CHECK_EQ(info.st_mode & 0777, 0600);
    char parent[128];
    snprintf(parent, sizeof parent, "%s/a/oi", root);
    CHECK_EQ(stat(parent, &info), 0);
    CHECK_EQ(info.st_mode & 0777, 0700);

    struct oi_cli_credentials loaded;
    CHECK_EQ(oi_cli_credentials_read(path, &loaded), OI_OK);
    CHECK_EQ(loaded.active, OI_CLI_PROVIDER_CUSTOM);
    CHECK_STREQ(loaded.openai_api_key, "sk-a\"b");
    CHECK_STREQ(loaded.custom_host, "h.test");
    CHECK_STREQ(loaded.custom_path, "/v1/x");
    CHECK_STREQ(loaded.custom_api_key, "ck");
    CHECK_EQ(loaded.custom_port, 8443);
    CHECK_EQ(loaded.custom_use_tls, 1);
    oi_cli_credentials_free(&loaded);
    remove_root();
}

TEST(a_second_write_replaces_the_file) {
    make_root();
    struct oi_cli_credentials saved;
    oi_cli_credentials_init(&saved);
    saved.active = OI_CLI_PROVIDER_OPENAI;
    CHECK_EQ(oi_cli_credentials_set(&saved.openai_api_key, "one"), OI_OK);
    CHECK_EQ(oi_cli_credentials_write(path, &saved), OI_OK);
    CHECK_EQ(oi_cli_credentials_set(&saved.openai_api_key, "two"), OI_OK);
    CHECK_EQ(oi_cli_credentials_write(path, &saved), OI_OK);
    oi_cli_credentials_free(&saved);

    struct oi_cli_credentials loaded;
    CHECK_EQ(oi_cli_credentials_read(path, &loaded), OI_OK);
    CHECK_STREQ(loaded.openai_api_key, "two");
    oi_cli_credentials_free(&loaded);
    remove_root();
}

TEST(a_missing_file_is_not_found) {
    make_root();
    struct oi_cli_credentials loaded;
    CHECK_EQ(oi_cli_credentials_read(path, &loaded), OI_ERR_NOTFOUND);
    remove_root();
}

TEST(a_file_other_users_can_reach_is_refused) {
    make_root();
    write_raw("{\"version\":1,\"active\":\"openai\"}", 0644);
    struct oi_cli_credentials loaded;
    CHECK_EQ(oi_cli_credentials_read(path, &loaded), OI_ERR_DENIED);
    remove_root();
}

TEST(malformed_files_are_rejected) {
    const char *bad[] = {
        "not json",
        "{}",
        "{\"version\":2,\"active\":\"openai\"}",
        "{\"version\":1,\"active\":\"robot\"}",
        "{\"version\":1,\"active\":\"custom\"}",
        "{\"version\":1,\"active\":\"openai\",\"openai\":{\"api_key\":1}}",
        ("{\"version\":1,\"active\":\"custom\",\"custom\":{\"host\":\"h\","
         "\"path\":\"/p\",\"port\":0,\"use_tls\":true}}"),
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        make_root();
        write_raw(bad[i], 0600);
        struct oi_cli_credentials loaded;
        CHECK_EQ(oi_cli_credentials_read(path, &loaded), OI_ERR_PARSE);
        remove_root();
    }
}

TEST(default_path_prefers_xdg_config_home) {
    char *found = NULL;
    setenv("XDG_CONFIG_HOME", "/x/cfg", 1);
    CHECK_EQ(oi_cli_credentials_default_path(&found), OI_OK);
    CHECK_STREQ(found, "/x/cfg/oi/credentials.json");
    free(found);
    unsetenv("XDG_CONFIG_HOME");
    setenv("HOME", "/x/home", 1);
    CHECK_EQ(oi_cli_credentials_default_path(&found), OI_OK);
    CHECK_STREQ(found, "/x/home/.config/oi/credentials.json");
    free(found);
}

static void check_url(const char *url, const char *host, int port, int use_tls,
                      const char *expected_path) {
    char *got_host = NULL;
    char *got_path = NULL;
    int got_port = 0;
    int got_tls = -1;
    CHECK_EQ(oi_cli_credentials_parse_url(url, strlen(url), &got_host,
                                          &got_port, &got_tls, &got_path),
             OI_OK);
    CHECK_STREQ(got_host, host);
    CHECK_EQ(got_port, port);
    CHECK_EQ(got_tls, use_tls);
    CHECK_STREQ(got_path, expected_path);
    free(got_host);
    free(got_path);
}

TEST(urls_resolve_to_endpoint_parts) {
    check_url("https://h.test", "h.test", 443, 1, "/v1/chat/completions");
    check_url("http://h.test/", "h.test", 80, 0, "/v1/chat/completions");
    check_url("http://127.0.0.1:8080", "127.0.0.1", 8080, 0,
              "/v1/chat/completions");
    check_url("https://h.test/api/v1", "h.test", 443, 1,
              "/api/v1/chat/completions");
    check_url("https://h.test/api/v1/", "h.test", 443, 1,
              "/api/v1/chat/completions");
    check_url("https://h.test/v9/chat/completions", "h.test", 443, 1,
              "/v9/chat/completions");
    check_url("https://h.test/x?api-version=1", "h.test", 443, 1,
              "/x?api-version=1");
}

TEST(unsafe_urls_are_rejected) {
    const char *bad[] = {
        "",
        "h.test",
        "ftp://h.test",
        "https://",
        "https://:443",
        "https://u:p@h.test",
        "https://[::1]:8080",
        "https://h.test:0",
        "https://h.test:65536",
        "https://h.test:abc",
        "https://h.test/a b",
        "https://h.test/a\r\nX: y",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char *host = NULL;
        char *url_path = NULL;
        int port = 0;
        int use_tls = 0;
        CHECK_EQ(oi_cli_credentials_parse_url(bad[i], strlen(bad[i]), &host,
                                              &port, &use_tls, &url_path),
                 OI_ERR_PARSE);
    }
}

int main(void) {
    RUN(round_trip_keeps_every_field_in_a_private_file);
    RUN(a_second_write_replaces_the_file);
    RUN(a_missing_file_is_not_found);
    RUN(a_file_other_users_can_reach_is_refused);
    RUN(malformed_files_are_rejected);
    RUN(default_path_prefers_xdg_config_home);
    RUN(urls_resolve_to_endpoint_parts);
    RUN(unsafe_urls_are_rejected);
    return oi_test_report();
}
