#include "oi/sesslog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define OI_SESSLOG_MAGIC "OISESLOG"
#define OI_SESSLOG_MAGIC_LEN 8
#define OI_SESSLOG_VERSION 1u
#define OI_SESSLOG_HEADER_LEN (OI_SESSLOG_MAGIC_LEN + 4)

struct oi_sesslog {
    int fd;
};

static void encode_u32le(unsigned char *buf, uint32_t v) {
    buf[0] = (unsigned char)(v & 0xFFu);
    buf[1] = (unsigned char)((v >> 8) & 0xFFu);
    buf[2] = (unsigned char)((v >> 16) & 0xFFu);
    buf[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static uint32_t decode_u32le(const unsigned char *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

/* Walks records from just after the header, bounding every offset
 * against the file's actual size (rather than relying on read() EOF
 * detection), so a length prefix claiming more than what's really
 * there -- whether from a mid-write crash or corruption -- is detected
 * without ever reading past the real end of file. */
static oi_status scan_for_valid_end(int fd, off_t file_size,
                                     off_t *out_valid_end) {
    off_t pos = OI_SESSLOG_HEADER_LEN;
    while (pos + 4 <= file_size) {
        unsigned char len_buf[4];
        ssize_t n = pread(fd, len_buf, sizeof len_buf, pos);
        if (n < 0) {
            return OI_ERR_IO;
        }
        if ((size_t)n < sizeof len_buf) {
            break; /* truncated length prefix */
        }
        uint32_t rec_len = decode_u32le(len_buf);
        if (rec_len > OI_SESSLOG_MAX_RECORD) {
            break; /* implausible: treat as corruption, stop before it */
        }
        off_t record_end = pos + 4 + (off_t)rec_len;
        if (record_end > file_size) {
            break; /* truncated payload */
        }
        pos = record_end;
    }
    *out_valid_end = pos;
    return OI_OK;
}

oi_status oi_sesslog_open(const char *path, oi_sesslog **out_log) {
    if (path == NULL || out_log == NULL) {
        return OI_ERR_INVAL;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return OI_ERR_IO;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return OI_ERR_EXISTS;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return OI_ERR_IO;
    }

    if (st.st_size == 0) {
        unsigned char header[OI_SESSLOG_HEADER_LEN];
        memcpy(header, OI_SESSLOG_MAGIC, OI_SESSLOG_MAGIC_LEN);
        encode_u32le(header + OI_SESSLOG_MAGIC_LEN, OI_SESSLOG_VERSION);
        ssize_t n = write(fd, header, sizeof header);
        if (n != (ssize_t)sizeof header) {
            close(fd);
            return OI_ERR_IO;
        }
    } else {
        if (st.st_size < OI_SESSLOG_HEADER_LEN) {
            close(fd);
            return OI_ERR_PARSE;
        }
        unsigned char header[OI_SESSLOG_HEADER_LEN];
        ssize_t n = pread(fd, header, sizeof header, 0);
        if (n != (ssize_t)sizeof header) {
            close(fd);
            return OI_ERR_IO;
        }
        if (memcmp(header, OI_SESSLOG_MAGIC, OI_SESSLOG_MAGIC_LEN) != 0) {
            close(fd);
            return OI_ERR_PARSE;
        }
        uint32_t version = decode_u32le(header + OI_SESSLOG_MAGIC_LEN);
        if (version != OI_SESSLOG_VERSION) {
            close(fd);
            return OI_ERR_PARSE;
        }

        off_t valid_end;
        oi_status st2 = scan_for_valid_end(fd, st.st_size, &valid_end);
        if (st2 != OI_OK) {
            close(fd);
            return st2;
        }
        if (valid_end < st.st_size) {
            if (ftruncate(fd, valid_end) != 0) {
                close(fd);
                return OI_ERR_IO;
            }
        }
    }

    oi_sesslog *log = malloc(sizeof *log);
    if (log == NULL) {
        close(fd);
        return OI_ERR_NOMEM;
    }
    log->fd = fd;
    *out_log = log;
    return OI_OK;
}

void oi_sesslog_close(oi_sesslog *log) {
    if (log == NULL) {
        return;
    }
    flock(log->fd, LOCK_UN);
    close(log->fd);
    free(log);
}

oi_status oi_sesslog_append(oi_sesslog *log, const void *data, size_t len) {
    if (log == NULL || (data == NULL && len > 0)) {
        return OI_ERR_INVAL;
    }
    if (len > OI_SESSLOG_MAX_RECORD) {
        return OI_ERR_INVAL;
    }

    size_t total = 4 + len;
    unsigned char *buf = malloc(total);
    if (buf == NULL) {
        return OI_ERR_NOMEM;
    }
    encode_u32le(buf, (uint32_t)len);
    if (len > 0) {
        memcpy(buf + 4, data, len);
    }

    ssize_t n;
    do {
        n = write(log->fd, buf, total);
    } while (n < 0 && errno == EINTR);
    free(buf);

    if (n < 0 || (size_t)n != total) {
        return OI_ERR_IO; /* including a short write: not a whole record */
    }
    return OI_OK;
}

oi_status oi_sesslog_replay(oi_sesslog *log, oi_sesslog_record_cb cb,
                             void *user_data) {
    if (log == NULL) {
        return OI_ERR_INVAL;
    }

    struct stat st;
    if (fstat(log->fd, &st) != 0) {
        return OI_ERR_IO;
    }

    off_t pos = OI_SESSLOG_HEADER_LEN;
    while (pos + 4 <= st.st_size) {
        unsigned char len_buf[4];
        ssize_t n = pread(log->fd, len_buf, sizeof len_buf, pos);
        if (n != (ssize_t)sizeof len_buf) {
            return OI_ERR_IO; /* open() already validated/truncated the file */
        }
        uint32_t rec_len = decode_u32le(len_buf);
        if (pos + 4 + (off_t)rec_len > st.st_size) {
            return OI_ERR_IO;
        }

        void *payload = NULL;
        if (rec_len > 0) {
            payload = malloc(rec_len);
            if (payload == NULL) {
                return OI_ERR_NOMEM;
            }
            ssize_t pn = pread(log->fd, payload, rec_len, pos + 4);
            if (pn != (ssize_t)rec_len) {
                free(payload);
                return OI_ERR_IO;
            }
        }
        if (cb) {
            cb(payload, rec_len, user_data);
        }
        free(payload);

        pos += 4 + (off_t)rec_len;
    }
    return OI_OK;
}
