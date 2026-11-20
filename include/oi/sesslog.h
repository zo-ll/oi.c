#ifndef OI_SESSLOG_H
#define OI_SESSLOG_H

#include <stddef.h>

#include "oi/status.h"

typedef struct oi_sesslog oi_sesslog;

#define OI_SESSLOG_MAX_RECORD (64u * 1024 * 1024)

oi_status oi_sesslog_open(const char *path, oi_sesslog **out_log);

void oi_sesslog_close(oi_sesslog *log);

oi_status oi_sesslog_append(oi_sesslog *log, const void *data, size_t len);

typedef void (*oi_sesslog_record_cb)(const void *data, size_t len,
                                      void *user_data);
oi_status oi_sesslog_replay(oi_sesslog *log, oi_sesslog_record_cb cb,
                             void *user_data);

size_t oi_sesslog_damaged_bytes(const oi_sesslog *log);

#endif
