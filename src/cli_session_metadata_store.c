#include "cli_session_metadata_store.h"

#include "cli_session_metadata_codec.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static oi_status join_tmp_path(const char *metadata_path,
                               char **out_tmp_path) {
    static const char suffix[] = ".tmp";
    size_t len = strlen(metadata_path);
    char *path = malloc(len + sizeof suffix);
    if (path == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(path, metadata_path, len);
    memcpy(path + len, suffix, sizeof suffix);
    *out_tmp_path = path;
    return OI_OK;
}

oi_status oi_cli_session_metadata_store_read(
    const char *metadata_path, struct oi_cli_session_metadata *out_metadata) {
    if (metadata_path == NULL || out_metadata == NULL) {
        return OI_ERR_INVAL;
    }
    int fd = open(metadata_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? OI_ERR_NOTFOUND : OI_ERR_IO;
    }

    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        return OI_ERR_IO;
    }
    if (info.st_size < 0 ||
        (size_t)info.st_size > OI_CLI_SESSION_METADATA_MAX_FILE) {
        close(fd);
        return OI_ERR_PARSE;
    }
    size_t size = (size_t)info.st_size;
    if (size == 0) {
        close(fd);
        return OI_ERR_PARSE;
    }

    char *buffer = malloc(size);
    if (buffer == NULL) {
        close(fd);
        return OI_ERR_NOMEM;
    }
    size_t total = 0;
    oi_status st = OI_OK;
    while (total < size) {
        ssize_t n = read(fd, buffer + total, size - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            st = OI_ERR_PARSE; /* short read: file changed under us */
            break;
        }
    }
    close(fd);
    if (st == OI_OK) {
        st = oi_cli_session_metadata_decode(buffer, size, out_metadata);
    }
    free(buffer);
    return st;
}

oi_status oi_cli_session_metadata_store_write(
    const char *metadata_path,
    const struct oi_cli_session_metadata *metadata) {
    if (metadata_path == NULL) {
        return OI_ERR_INVAL;
    }
    char *tmp_path = NULL;
    oi_status st = join_tmp_path(metadata_path, &tmp_path);
    if (st != OI_OK) {
        return st;
    }

    char *json = NULL;
    size_t json_len = 0;
    st = oi_cli_session_metadata_encode(metadata, &json, &json_len);
    if (st != OI_OK) {
        free(tmp_path);
        return st;
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(json);
        free(tmp_path);
        return OI_ERR_IO;
    }
    size_t total = 0;
    while (total < json_len) {
        ssize_t n = write(fd, json + total, json_len - total);
        if (n > 0) {
            total += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            st = OI_ERR_IO;
            break;
        }
    }
    free(json);
    if (close(fd) != 0 && st == OI_OK) {
        st = OI_ERR_IO;
    }
    if (st == OI_OK && rename(tmp_path, metadata_path) != 0) {
        st = OI_ERR_IO;
    }
    if (st != OI_OK) {
        unlink(tmp_path);
    }
    free(tmp_path);
    return st;
}
