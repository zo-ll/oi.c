#include "oi/sesslog.h"
#include "test.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_path[256];

static const char *fresh_path(void) {
    static int counter = 0;
    snprintf(g_path, sizeof g_path, "/tmp/oi_sesslog_test_%d_%d.log",
             (int)getpid(), counter++);
    unlink(g_path);
    return g_path;
}

struct collected {
    char records[32][256];
    size_t lens[32];
    int count;
};

static void collect_cb(const void *data, size_t len, void *ud) {
    struct collected *c = ud;
    CHECK(c->count < 32);
    CHECK(len < sizeof c->records[0]);
    if (len > 0) {
        memcpy(c->records[c->count], data, len);
    }
    c->lens[c->count] = len;
    c->count++;
}

/* --- basic create/append/replay/reopen --- */

TEST(create_open_reopen) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    oi_sesslog_close(log);

    /* Reopening an existing (empty-of-records, header-only) log must
     * succeed and not misinterpret the header as corrupt. */
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    oi_sesslog_close(log);
    unlink(path);
}

TEST(open_rejects_bad_args) {
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(NULL, &log), OI_ERR_INVAL);
    CHECK_EQ(oi_sesslog_open("/tmp/x", NULL), OI_ERR_INVAL);
}

TEST(open_nonexistent_directory_is_io_error) {
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open("/nonexistent/dir/x.log", &log), OI_ERR_IO);
}

TEST(close_null_safe) { oi_sesslog_close(NULL); }

TEST(append_and_replay_round_trip) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);

    CHECK_EQ(oi_sesslog_append(log, "first", 5), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "second-record", 13), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "", 0), OI_OK); /* empty record */
    CHECK_EQ(oi_sesslog_append(log, "third", 5), OI_OK);

    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 4);
    CHECK_EQ(c.lens[0], 5u);
    CHECK(memcmp(c.records[0], "first", 5) == 0);
    CHECK_EQ(c.lens[1], 13u);
    CHECK(memcmp(c.records[1], "second-record", 13) == 0);
    CHECK_EQ(c.lens[2], 0u);
    CHECK_EQ(c.lens[3], 5u);
    CHECK(memcmp(c.records[3], "third", 5) == 0);

    oi_sesslog_close(log);
    unlink(path);
}

TEST(records_persist_across_reopen) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "persisted-one", 13), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "persisted-two", 13), OI_OK);
    oi_sesslog_close(log);

    /* Simulates a process restart: a fresh open() must replay what was
     * written before, exercising the "resume mid-conversation" use
     * case from PLAN.md. */
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 2);
    CHECK(memcmp(c.records[0], "persisted-one", 13) == 0);
    CHECK(memcmp(c.records[1], "persisted-two", 13) == 0);

    /* Further appends after reopening must land after the replayed
     * data, not clobber it. */
    CHECK_EQ(oi_sesslog_append(log, "after-reopen", 12), OI_OK);
    struct collected c2 = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c2), OI_OK);
    CHECK_EQ(c2.count, 3);
    CHECK(memcmp(c2.records[2], "after-reopen", 12) == 0);

    oi_sesslog_close(log);
    unlink(path);
}

TEST(replay_on_empty_log_yields_nothing) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 0);
    oi_sesslog_close(log);
    unlink(path);
}

TEST(replay_with_null_callback_is_safe) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "x", 1), OI_OK);
    CHECK_EQ(oi_sesslog_replay(log, NULL, NULL), OI_OK);
    oi_sesslog_close(log);
    unlink(path);
}

/* --- recovery from a truncated trailing record --- */

TEST(recovery_discards_truncated_length_prefix) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "good-record", 11), OI_OK);
    oi_sesslog_close(log);

    /* Simulate a crash mid-write: append only 2 of the 4 length-prefix
     * bytes for what would have been the next record. */
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    unsigned char partial[2] = {0x05, 0x00};
    CHECK_EQ(write(fd, partial, sizeof partial), (ssize_t)sizeof partial);
    close(fd);

    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK(memcmp(c.records[0], "good-record", 11) == 0);

    /* The truncated bytes must have been physically discarded (not
     * just skipped in memory), so a subsequent append lands cleanly
     * and a later replay doesn't see phantom data. */
    CHECK_EQ(oi_sesslog_append(log, "next", 4), OI_OK);
    struct collected c2 = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c2), OI_OK);
    CHECK_EQ(c2.count, 2);
    CHECK(memcmp(c2.records[1], "next", 4) == 0);

    oi_sesslog_close(log);
    unlink(path);
}

TEST(recovery_discards_truncated_payload) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "good", 4), OI_OK);
    oi_sesslog_close(log);

    /* A complete length prefix claiming 100 bytes, but only 3 bytes of
     * payload actually written before the simulated crash. */
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    unsigned char prefix[4] = {100, 0, 0, 0};
    CHECK_EQ(write(fd, prefix, 4), 4);
    CHECK_EQ(write(fd, "abc", 3), 3);
    close(fd);

    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK(memcmp(c.records[0], "good", 4) == 0);

    oi_sesslog_close(log);
    unlink(path);
}

TEST(recovery_discards_implausible_length_prefix) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "good", 4), OI_OK);
    oi_sesslog_close(log);

    /* A length prefix (0xFFFFFFFF) that could never be a real record:
     * corruption, not a legitimate in-progress write. */
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0);
    unsigned char prefix[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(write(fd, prefix, 4), 4);
    close(fd);

    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK(memcmp(c.records[0], "good", 4) == 0);

    oi_sesslog_close(log);
    unlink(path);
}

/* --- header validation --- */

TEST(bad_magic_rejected) {
    const char *path = fresh_path();
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0);
    const char *garbage = "NOTAVALIDHDR";
    CHECK_EQ(write(fd, garbage, strlen(garbage)), (ssize_t)strlen(garbage));
    close(fd);

    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_ERR_PARSE);
    unlink(path);
}

TEST(bad_version_rejected) {
    const char *path = fresh_path();
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0);
    unsigned char header[12];
    memcpy(header, "OISESLOG", 8);
    header[8] = 99; /* bogus version */
    header[9] = 0;
    header[10] = 0;
    header[11] = 0;
    CHECK_EQ(write(fd, header, sizeof header), (ssize_t)sizeof header);
    close(fd);

    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_ERR_PARSE);
    unlink(path);
}

TEST(file_shorter_than_header_rejected) {
    const char *path = fresh_path();
    int fd = open(path, O_WRONLY | O_CREAT, 0600);
    CHECK(fd >= 0);
    CHECK_EQ(write(fd, "short", 5), 5);
    close(fd);

    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_ERR_PARSE);
    unlink(path);
}

/* --- locking --- */

TEST(second_open_of_same_path_is_locked_out) {
    const char *path = fresh_path();
    oi_sesslog *log1;
    CHECK_EQ(oi_sesslog_open(path, &log1), OI_OK);

    oi_sesslog *log2;
    CHECK_EQ(oi_sesslog_open(path, &log2), OI_ERR_EXISTS);

    oi_sesslog_close(log1);

    /* Once released, a new open() must succeed. */
    oi_sesslog *log3;
    CHECK_EQ(oi_sesslog_open(path, &log3), OI_OK);
    oi_sesslog_close(log3);
    unlink(path);
}

/* --- append boundary --- */

TEST(append_rejects_oversized_record) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);

    size_t big = (size_t)OI_SESSLOG_MAX_RECORD + 1;
    char *buf = calloc(1, big);
    CHECK(buf != NULL);
    CHECK_EQ(oi_sesslog_append(log, buf, big), OI_ERR_INVAL);
    free(buf);

    /* The rejected append must not have written anything. */
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 0);

    oi_sesslog_close(log);
    unlink(path);
}

TEST(append_rejects_bad_args) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(NULL, "x", 1), OI_ERR_INVAL);
    CHECK_EQ(oi_sesslog_append(log, NULL, 5), OI_ERR_INVAL);
    oi_sesslog_close(log);
    unlink(path);
}

static void count_cb(const void *data, size_t len, void *ud) {
    (void)data;
    (void)len;
    int *count = ud;
    (*count)++;
}

TEST(many_records) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);

    char buf[32];
    for (int i = 0; i < 500; i++) {
        int n = snprintf(buf, sizeof buf, "record-%d", i);
        CHECK_EQ(oi_sesslog_append(log, buf, (size_t)n), OI_OK);
    }

    int count = 0;
    CHECK_EQ(oi_sesslog_replay(log, count_cb, &count), OI_OK);
    CHECK_EQ(count, 500);

    oi_sesslog_close(log);
    unlink(path);
}

int main(void) {
    RUN(create_open_reopen);
    RUN(open_rejects_bad_args);
    RUN(open_nonexistent_directory_is_io_error);
    RUN(close_null_safe);
    RUN(append_and_replay_round_trip);
    RUN(records_persist_across_reopen);
    RUN(replay_on_empty_log_yields_nothing);
    RUN(replay_with_null_callback_is_safe);
    RUN(recovery_discards_truncated_length_prefix);
    RUN(recovery_discards_truncated_payload);
    RUN(recovery_discards_implausible_length_prefix);
    RUN(bad_magic_rejected);
    RUN(bad_version_rejected);
    RUN(file_shorter_than_header_rejected);
    RUN(second_open_of_same_path_is_locked_out);
    RUN(append_rejects_oversized_record);
    RUN(append_rejects_bad_args);
    RUN(many_records);
    return oi_test_report();
}
