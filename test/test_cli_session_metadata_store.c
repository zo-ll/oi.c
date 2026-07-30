#include "cli_session_metadata_store.h"
#include "test.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *tmp_metadata_path(void) {
    char path[160];
    snprintf(path, sizeof path, "/tmp/oi-metadata-store-%ld-%d", (long)getpid(),
             rand());
    return strdup(path);
}

static char *tmp_suffixed_path(const char *base, const char *suffix) {
    size_t len = strlen(base) + strlen(suffix) + 1;
    char *path = malloc(len);
    snprintf(path, len, "%s%s", base, suffix);
    return path;
}

static int path_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

TEST(write_then_read_round_trips) {
    struct oi_cli_session_metadata metadata;
    struct oi_cli_session_metadata read_back;
    char *path = tmp_metadata_path();
    char *tmp_path = tmp_suffixed_path(path, ".tmp");
    oi_cli_session_metadata_init(&metadata);
    oi_cli_session_metadata_init(&read_back);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/home/az/project", 16, NULL,
                                         0, 100, 200),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_store_write(path, &metadata), OI_OK);
    CHECK(!path_exists(tmp_path));
    CHECK_EQ(oi_cli_session_metadata_store_read(path, &read_back), OI_OK);
    CHECK_STREQ(read_back.session_id.data, "sess-1");
    CHECK_STREQ(read_back.model.data, "gpt-test");
    CHECK_STREQ(read_back.cwd.data, "/home/az/project");

    unlink(path);
    free(path);
    free(tmp_path);
    oi_cli_session_metadata_free(&metadata);
    oi_cli_session_metadata_free(&read_back);
}

TEST(missing_file_reports_notfound) {
    struct oi_cli_session_metadata metadata;
    char *path = tmp_metadata_path();
    oi_cli_session_metadata_init(&metadata);

    unlink(path);
    CHECK_EQ(oi_cli_session_metadata_store_read(path, &metadata),
             OI_ERR_NOTFOUND);

    free(path);
    oi_cli_session_metadata_free(&metadata);
}

TEST(malformed_on_disk_content_is_rejected) {
    struct oi_cli_session_metadata metadata;
    char *path = tmp_metadata_path();
    FILE *f = fopen(path, "w");
    oi_cli_session_metadata_init(&metadata);

    CHECK(f != NULL);
    fputs("not json at all", f);
    fclose(f);
    CHECK_EQ(oi_cli_session_metadata_store_read(path, &metadata),
             OI_ERR_PARSE);

    unlink(path);
    free(path);
    oi_cli_session_metadata_free(&metadata);
}

TEST(empty_file_is_rejected) {
    struct oi_cli_session_metadata metadata;
    char *path = tmp_metadata_path();
    FILE *f = fopen(path, "w");
    oi_cli_session_metadata_init(&metadata);

    CHECK(f != NULL);
    fclose(f);
    CHECK_EQ(oi_cli_session_metadata_store_read(path, &metadata),
             OI_ERR_PARSE);

    unlink(path);
    free(path);
    oi_cli_session_metadata_free(&metadata);
}

TEST(oversized_file_is_rejected) {
    struct oi_cli_session_metadata metadata;
    char *path = tmp_metadata_path();
    FILE *f = fopen(path, "w");
    size_t i;
    oi_cli_session_metadata_init(&metadata);

    CHECK(f != NULL);
    for (i = 0; i < OI_CLI_SESSION_METADATA_MAX_FILE + 1; i++) {
        CHECK_EQ(fputc(' ', f), ' ');
    }
    fclose(f);
    CHECK_EQ(oi_cli_session_metadata_store_read(path, &metadata),
             OI_ERR_PARSE);

    unlink(path);
    free(path);
    oi_cli_session_metadata_free(&metadata);
}

TEST(preexisting_garbage_tmp_file_is_safely_clobbered) {
    struct oi_cli_session_metadata metadata;
    struct oi_cli_session_metadata read_back;
    char *path = tmp_metadata_path();
    char *tmp_path = tmp_suffixed_path(path, ".tmp");
    FILE *f = fopen(tmp_path, "w");
    oi_cli_session_metadata_init(&metadata);
    oi_cli_session_metadata_init(&read_back);

    CHECK(f != NULL);
    fputs("leftover garbage from a crashed write", f);
    fclose(f);

    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, 1, 1),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_store_write(path, &metadata), OI_OK);
    CHECK(!path_exists(tmp_path));
    CHECK_EQ(oi_cli_session_metadata_store_read(path, &read_back), OI_OK);
    CHECK_STREQ(read_back.session_id.data, "sess-1");

    unlink(path);
    free(path);
    free(tmp_path);
    oi_cli_session_metadata_free(&metadata);
    oi_cli_session_metadata_free(&read_back);
}

TEST(failed_rename_leaves_the_destination_untouched_and_cleans_up) {
    struct oi_cli_session_metadata metadata;
    char *path = tmp_metadata_path();
    char *tmp_path = tmp_suffixed_path(path, ".tmp");
    oi_cli_session_metadata_init(&metadata);

    /* Make the destination an existing directory: rename(2) of a regular
     * file onto it fails with EISDIR, forcing a rename failure without
     * ever affecting whether the temp file could be created (same
     * directory, a distinct filename). */
    CHECK_EQ(mkdir(path, 0700), 0);
    CHECK_EQ(oi_cli_session_metadata_set(&metadata, "sess-1", 6, "gpt-test",
                                         8, "/tmp", 4, NULL, 0, 1, 1),
             OI_OK);
    CHECK_EQ(oi_cli_session_metadata_store_write(path, &metadata),
             OI_ERR_IO);
    CHECK(!path_exists(tmp_path));
    {
        struct stat info;
        CHECK_EQ(stat(path, &info), 0);
        CHECK(S_ISDIR(info.st_mode));
    }

    rmdir(path);
    free(path);
    free(tmp_path);
    oi_cli_session_metadata_free(&metadata);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_session_metadata metadata;
    oi_cli_session_metadata_init(&metadata);

    CHECK_EQ(oi_cli_session_metadata_store_read(NULL, &metadata),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_session_metadata_store_write(NULL, &metadata),
             OI_ERR_INVAL);

    oi_cli_session_metadata_free(&metadata);
}

int main(void) {
    RUN(write_then_read_round_trips);
    RUN(missing_file_reports_notfound);
    RUN(malformed_on_disk_content_is_rejected);
    RUN(empty_file_is_rejected);
    RUN(oversized_file_is_rejected);
    RUN(preexisting_garbage_tmp_file_is_safely_clobbered);
    RUN(failed_rename_leaves_the_destination_untouched_and_cleans_up);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
