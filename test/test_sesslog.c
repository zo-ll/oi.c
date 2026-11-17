#include "oi/sesslog.h"
#include "test.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
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
    unsigned char head[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0};
    CHECK_EQ(write(fd, head, 8), 8);
    close(fd);

    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_damaged_bytes(log), (size_t)8);
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK(memcmp(c.records[0], "good", 4) == 0);

    oi_sesslog_close(log);
    char copy_path[300];
    snprintf(copy_path, sizeof copy_path, "%s.corrupt", path);
    CHECK_EQ(unlink(copy_path), 0);
    unlink(path);
}

/* --- checksums and damage --- */

static size_t read_file(const char *path, unsigned char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0);
    ssize_t n = read(fd, buf, cap);
    close(fd);
    CHECK(n >= 0 && (size_t)n < cap);
    return (size_t)n;
}

static uint32_t u32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

TEST(new_record_carries_its_crc32) {
    const char *path = fresh_path();
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "123456789", 9), OI_OK);
    oi_sesslog_close(log);

    unsigned char bytes[64];
    size_t n = read_file(path, bytes, sizeof bytes);
    CHECK_EQ(n, (size_t)(12 + 8 + 9));
    CHECK_EQ(u32le(bytes + 8), 2u);           /* log version */
    CHECK_EQ(u32le(bytes + 12), 9u);          /* payload length */
    CHECK_EQ(u32le(bytes + 16), 0xCBF43926u); /* CRC-32("123456789") */
    CHECK(memcmp(bytes + 20, "123456789", 9) == 0);
    unlink(path);
}

TEST(flipped_byte_cuts_the_log_and_keeps_a_copy) {
    const char *path = fresh_path();
    char copy_path[300];
    snprintf(copy_path, sizeof copy_path, "%s.corrupt", path);
    unlink(copy_path);
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "first", 5), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "second", 6), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "third", 5), OI_OK);
    oi_sesslog_close(log);

    unsigned char original[128];
    size_t size = read_file(path, original, sizeof original);
    /* One byte inside the payload of "second", in the middle of the log. */
    off_t flip = 12 + 8 + 5 + 8 + 2;
    unsigned char byte = original[flip] ^ 0x20;
    int fd = open(path, O_WRONLY);
    CHECK(fd >= 0);
    CHECK_EQ(pwrite(fd, &byte, 1, flip), 1);
    close(fd);
    unsigned char damaged[128];
    CHECK_EQ(read_file(path, damaged, sizeof damaged), size);

    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_damaged_bytes(log), (size_t)(8 + 6 + 8 + 5));
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 1);
    CHECK(memcmp(c.records[0], "first", 5) == 0);
    oi_sesslog_close(log);

    unsigned char copy[128];
    CHECK_EQ(read_file(copy_path, copy, sizeof copy), size);
    CHECK(memcmp(copy, damaged, size) == 0);
    struct stat info;
    CHECK_EQ(stat(copy_path, &info), 0);
    CHECK_EQ(info.st_mode & 0777, 0600u);
    unlink(copy_path);
    unlink(path);
}

TEST(crash_tail_is_cut_without_a_copy) {
    const char *path = fresh_path();
    char copy_path[300];
    snprintf(copy_path, sizeof copy_path, "%s.corrupt", path);
    unlink(copy_path);
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "good", 4), OI_OK);
    oi_sesslog_close(log);

    /* A head that claims 100 bytes, then 3: a write a crash cut short. */
    int fd = open(path, O_WRONLY | O_APPEND);
    unsigned char head[8] = {100, 0, 0, 0, 1, 2, 3, 4};
    CHECK_EQ(write(fd, head, 8), 8);
    CHECK_EQ(write(fd, "abc", 3), 3);
    close(fd);

    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_damaged_bytes(log), (size_t)0);
    oi_sesslog_close(log);
    CHECK(access(copy_path, F_OK) != 0);
    unlink(path);
}

TEST(failed_copy_keeps_the_log_unchanged) {
    const char *path = fresh_path();
    char copy_path[300];
    snprintf(copy_path, sizeof copy_path, "%s.corrupt", path);
    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "first", 5), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "second", 6), OI_OK);
    oi_sesslog_close(log);

    /* A bad checksum on "first", and a directory where the copy goes. */
    unsigned char zero[4] = {0, 0, 0, 0};
    int fd = open(path, O_WRONLY);
    CHECK_EQ(pwrite(fd, zero, 4, 12 + 4), 4);
    close(fd);
    unsigned char before[128];
    size_t size = read_file(path, before, sizeof before);
    CHECK_EQ(mkdir(copy_path, 0700), 0);

    CHECK_EQ(oi_sesslog_open(path, &log), OI_ERR_IO);
    unsigned char after[128];
    CHECK_EQ(read_file(path, after, sizeof after), size);
    CHECK(memcmp(before, after, size) == 0);
    rmdir(copy_path);
    unlink(path);
}

TEST(version_1_log_keeps_its_record_format) {
    const char *path = fresh_path();
    unsigned char v1[12 + 4 + 3] = {'O', 'I', 'S', 'E', 'S', 'L', 'O', 'G',
                                    1,   0,   0,   0,   3,   0,   0,   0,
                                    'o', 'l', 'd'};
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK_EQ(write(fd, v1, sizeof v1), (ssize_t)sizeof v1);
    close(fd);

    oi_sesslog *log;
    CHECK_EQ(oi_sesslog_open(path, &log), OI_OK);
    CHECK_EQ(oi_sesslog_append(log, "new", 3), OI_OK);
    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(log, collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 2);
    CHECK(memcmp(c.records[0], "old", 3) == 0);
    CHECK(memcmp(c.records[1], "new", 3) == 0);
    oi_sesslog_close(log);

    unsigned char bytes[64];
    CHECK_EQ(read_file(path, bytes, sizeof bytes), sizeof v1 + 4 + 3);
    CHECK_EQ(u32le(bytes + sizeof v1), 3u);
    CHECK(memcmp(bytes + sizeof v1 + 4, "new", 3) == 0);
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
    RUN(new_record_carries_its_crc32);
    RUN(flipped_byte_cuts_the_log_and_keeps_a_copy);
    RUN(crash_tail_is_cut_without_a_copy);
    RUN(failed_copy_keeps_the_log_unchanged);
    RUN(version_1_log_keeps_its_record_format);
    RUN(file_shorter_than_header_rejected);
    RUN(second_open_of_same_path_is_locked_out);
    RUN(append_rejects_oversized_record);
    RUN(append_rejects_bad_args);
    RUN(many_records);
    return oi_test_report();
}
