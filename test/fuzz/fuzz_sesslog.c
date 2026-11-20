
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "oi/sesslog.h"

#define FUZZ_MAX_INPUT (64u * 1024)

static const char probe_record[] = "oi-fuzz-probe";

struct replay_ctx {
    size_t count;
    uint64_t digest;
    char last[sizeof probe_record];
    size_t last_len;
    int last_truncated;
};

static uint64_t digest_record(uint64_t digest, const void *data, size_t len) {
    const unsigned char *bytes = data;
    digest = (digest ^ len) * 1099511628211ull;
    for (size_t i = 0; i < len; i++) {
        digest = (digest ^ bytes[i]) * 1099511628211ull;
    }
    return digest;
}

static void on_record(const void *data, size_t len, void *user_data) {
    struct replay_ctx *ctx = user_data;

    ctx->count++;
    ctx->digest = digest_record(ctx->digest, data, len);

    if (len <= sizeof ctx->last) {
        if (len > 0) {
            memcpy(ctx->last, data, len);
        }
        ctx->last_len = len;
        ctx->last_truncated = 0;
    } else {
        ctx->last_len = 0;
        ctx->last_truncated = 1;
    }
}

static const char *log_path(void) {
    static char path[64];
    if (path[0] == '\0') {
        snprintf(path, sizeof path, "/tmp/oi_fuzz_sesslog_%ld.log",
                 (long)getpid());
    }
    return path;
}

static int write_file(const char *path, const uint8_t *data, size_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return 0;
    }
    size_t off = 0;
    while (off < size) {
        ssize_t n = write(fd, data + off, size - off);
        if (n <= 0) {
            close(fd);
            return 0;
        }
        off += (size_t)n;
    }
    close(fd);
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > FUZZ_MAX_INPUT) {
        return 0;
    }

    const char *path = log_path();
    if (!write_file(path, data, size)) {
        return 0;
    }

    oi_sesslog *log = NULL;
    if (oi_sesslog_open(path, &log) == OI_OK) {
        struct replay_ctx before = {0};
        before.digest = 14695981039346656037ull;
        if (oi_sesslog_replay(log, on_record, &before) == OI_OK) {
            if (oi_sesslog_append(log, probe_record, sizeof probe_record) ==
                OI_OK) {
                struct replay_ctx after = {0};
                after.digest = 14695981039346656037ull;
                if (oi_sesslog_replay(log, on_record, &after) == OI_OK) {
                    assert(after.count == before.count + 1);
                    assert(after.digest ==
                           digest_record(before.digest, probe_record,
                                         sizeof probe_record));
                    assert(!after.last_truncated);
                    assert(after.last_len == sizeof probe_record);
                    assert(memcmp(after.last, probe_record,
                                  sizeof probe_record) == 0);
                }
            }
        }
        oi_sesslog_close(log);
    }

    unlink(path);
    return 0;
}
