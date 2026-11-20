#ifndef OI_CLI_BYTEBUF_H
#define OI_CLI_BYTEBUF_H

#include <stddef.h>

#include "oi/status.h"

struct oi_cli_bytebuf {
    char *data;
    size_t len;
    size_t cap;
};

void oi_cli_bytebuf_init(struct oi_cli_bytebuf *buf);
oi_status oi_cli_bytebuf_append(struct oi_cli_bytebuf *buf, const void *data,
                                size_t len);

void oi_cli_bytebuf_reset(struct oi_cli_bytebuf *buf);
void oi_cli_bytebuf_free(struct oi_cli_bytebuf *buf);

#endif
