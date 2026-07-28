#ifndef OI_CLI_BYTEBUF_H
#define OI_CLI_BYTEBUF_H

#include <stddef.h>

#include "oi/status.h"

/*
 * A growable byte buffer with doubling growth. Shared by the sanitize,
 * UTF-8 stream, and Markdown modules so each doesn't reimplement its own
 * append-with-overflow-check logic.
 */
struct oi_cli_bytebuf {
    char *data;
    size_t len;
    size_t cap;
};

void oi_cli_bytebuf_init(struct oi_cli_bytebuf *buf);
oi_status oi_cli_bytebuf_append(struct oi_cli_bytebuf *buf, const void *data,
                                size_t len);

/* Sets len to 0 without releasing the underlying allocation. */
void oi_cli_bytebuf_reset(struct oi_cli_bytebuf *buf);
void oi_cli_bytebuf_free(struct oi_cli_bytebuf *buf);

#endif
