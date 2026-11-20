#include "oi/sesslog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#define OI_SESSLOG_MAGIC "OISESLOG"
#define OI_SESSLOG_MAGIC_LEN 8
#define OI_SESSLOG_VERSION_LENGTH_ONLY 1u
#define OI_SESSLOG_VERSION 2u
#define OI_SESSLOG_HEADER_LEN (OI_SESSLOG_MAGIC_LEN + 4)

struct oi_sesslog {
    int fd;
    uint32_t version;
    size_t damaged_bytes;
    off_t size;
};

static size_t record_head_len(uint32_t version) {
    return version == OI_SESSLOG_VERSION_LENGTH_ONLY ? 4 : 8;
}

static uint32_t crc32_update(uint32_t crc, const unsigned char *data,
                             size_t len) {
    static const uint32_t table[16] = {
        0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
        0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
        0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
        0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu,
    };
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ table[crc & 15u];
        crc = (crc >> 4) ^ table[crc & 15u];
    }
    return ~crc;
}

static oi_status crc32_of_range(int fd, off_t pos, size_t len,
                                uint32_t *out_crc) {
    unsigned char chunk[16384];
    uint32_t crc = 0;
    while (len > 0) {
        size_t want = len < sizeof chunk ? len : sizeof chunk;
        ssize_t n = pread(fd, chunk, want, pos);
        if (n <= 0) {
            return OI_ERR_IO;
        }
        crc = crc32_update(crc, chunk, (size_t)n);
        pos += n;
        len -= (size_t)n;
    }
    *out_crc = crc;
    return OI_OK;
}

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

static void best_effort_truncate(int fd, off_t size) {
    if (ftruncate(fd, size) != 0) {
        return;
    }
}

static oi_status scan_for_valid_end(int fd, uint32_t version,
                                     off_t file_size, off_t *out_valid_end,
                                     int *out_damaged) {
    size_t head_len = record_head_len(version);
    off_t pos = OI_SESSLOG_HEADER_LEN;
    *out_damaged = 0;
    while (pos <= file_size && (size_t)(file_size - pos) >= head_len) {
        unsigned char head[8];
        ssize_t n = pread(fd, head, head_len, pos);
        if (n < 0) {
            return OI_ERR_IO;
        }
        if ((size_t)n < head_len) {
            break;
        }
        uint32_t rec_len = decode_u32le(head);
        if (rec_len > OI_SESSLOG_MAX_RECORD) {
            *out_damaged = 1;
            break;
        }
        if ((off_t)rec_len > file_size - pos - (off_t)head_len) {
            break;
        }
        if (version != OI_SESSLOG_VERSION_LENGTH_ONLY) {
            uint32_t crc;
            oi_status st =
                crc32_of_range(fd, pos + (off_t)head_len, rec_len, &crc);
            if (st != OI_OK) {
                return st;
            }
            if (crc != decode_u32le(head + 4)) {
                *out_damaged = 1;
                break;
            }
        }
        pos += (off_t)head_len + (off_t)rec_len;
    }
    *out_valid_end = pos;
    return OI_OK;
}

static oi_status keep_damaged_copy(int fd, const char *path,
                                   off_t file_size) {
    size_t path_len = strlen(path);
    char *copy_path = malloc(path_len + sizeof ".corrupt");
    if (copy_path == NULL) {
        return OI_ERR_NOMEM;
    }
    memcpy(copy_path, path, path_len);
    memcpy(copy_path + path_len, ".corrupt", sizeof ".corrupt");
    int out = open(copy_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                                  O_NOFOLLOW,
                   0600);
    free(copy_path);
    if (out < 0) {
        return OI_ERR_IO;
    }
    unsigned char chunk[16384];
    off_t pos = 0;
    oi_status st = OI_OK;
    while (st == OI_OK && pos < file_size) {
        ssize_t n = pread(fd, chunk, sizeof chunk, pos);
        if (n <= 0) {
            st = OI_ERR_IO;
            break;
        }
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(out, chunk + off, (size_t)(n - off));
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                st = OI_ERR_IO;
                break;
            }
            off += w;
        }
        pos += n;
    }
    if (st == OI_OK && fsync(out) != 0) {
        st = OI_ERR_IO;
    }
    if (close(out) != 0 && st == OI_OK) {
        st = OI_ERR_IO;
    }
    return st;
}

oi_status oi_sesslog_open(const char *path, oi_sesslog **out_log) {
    if (path == NULL || out_log == NULL) {
        return OI_ERR_INVAL;
    }

    int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
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

    uint32_t version = OI_SESSLOG_VERSION;
    size_t damaged_bytes = 0;
    off_t size = OI_SESSLOG_HEADER_LEN;
    if (st.st_size == 0) {
        unsigned char header[OI_SESSLOG_HEADER_LEN];
        memcpy(header, OI_SESSLOG_MAGIC, OI_SESSLOG_MAGIC_LEN);
        encode_u32le(header + OI_SESSLOG_MAGIC_LEN, OI_SESSLOG_VERSION);
        ssize_t n = write(fd, header, sizeof header);
        if (n != (ssize_t)sizeof header) {
            best_effort_truncate(fd, 0);
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
        version = decode_u32le(header + OI_SESSLOG_MAGIC_LEN);
        if (version != OI_SESSLOG_VERSION &&
            version != OI_SESSLOG_VERSION_LENGTH_ONLY) {
            close(fd);
            return OI_ERR_PARSE;
        }

        off_t valid_end;
        int damaged;
        oi_status st2 =
            scan_for_valid_end(fd, version, st.st_size, &valid_end, &damaged);
        if (st2 == OI_OK && damaged) {
            st2 = keep_damaged_copy(fd, path, st.st_size);
            damaged_bytes = (size_t)(st.st_size - valid_end);
        }
        if (st2 != OI_OK) {
            close(fd);
            return st2;
        }
        size = valid_end;
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
    log->version = version;
    log->damaged_bytes = damaged_bytes;
    log->size = size;
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

    unsigned char head[8];
    size_t head_len = record_head_len(log->version);
    encode_u32le(head, (uint32_t)len);
    if (head_len == 8) {
        encode_u32le(head + 4, crc32_update(0, data, len));
    }
    struct iovec parts[2] = {
        {head, head_len},
        {(void *)data, len},
    };
    size_t total = head_len + len;

    ssize_t n;
    do {
        n = writev(log->fd, parts, len > 0 ? 2 : 1);
    } while (n < 0 && errno == EINTR);

    if (n < 0 || (size_t)n != total) {
        best_effort_truncate(log->fd, log->size);
        return OI_ERR_IO;
    }
    log->size += (off_t)total;
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

    size_t head_len = record_head_len(log->version);
    off_t pos = OI_SESSLOG_HEADER_LEN;
    while (pos <= st.st_size && (size_t)(st.st_size - pos) >= head_len) {
        unsigned char head[8];
        ssize_t n = pread(log->fd, head, head_len, pos);
        if (n != (ssize_t)head_len) {
            return OI_ERR_IO;
        }
        uint32_t rec_len = decode_u32le(head);
        if ((off_t)rec_len > st.st_size - pos - (off_t)head_len) {
            return OI_ERR_IO;
        }

        void *payload = NULL;
        if (rec_len > 0) {
            payload = malloc(rec_len);
            if (payload == NULL) {
                return OI_ERR_NOMEM;
            }
            ssize_t pn =
                pread(log->fd, payload, rec_len, pos + (off_t)head_len);
            if (pn != (ssize_t)rec_len) {
                free(payload);
                return OI_ERR_IO;
            }
        }
        if (head_len == 8 &&
            crc32_update(0, payload, rec_len) != decode_u32le(head + 4)) {
            free(payload);
            return OI_ERR_IO;
        }
        if (cb) {
            cb(payload, rec_len, user_data);
        }
        free(payload);

        pos += (off_t)head_len + (off_t)rec_len;
    }
    return OI_OK;
}

size_t oi_sesslog_damaged_bytes(const oi_sesslog *log) {
    return log != NULL ? log->damaged_bytes : 0;
}
