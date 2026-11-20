#ifndef OI_CLI_UTF8_STREAM_H
#define OI_CLI_UTF8_STREAM_H

#include <stddef.h>

#include "cli_bytebuf.h"
#include "oi/status.h"

struct oi_cli_utf8_stream {
    unsigned char pending[4];
    size_t pending_len;
    size_t expected_len;
};

void oi_cli_utf8_stream_init(struct oi_cli_utf8_stream *stream);

oi_status oi_cli_utf8_stream_feed(struct oi_cli_utf8_stream *stream,
                                  const unsigned char *in, size_t in_len,
                                  struct oi_cli_bytebuf *out);

oi_status oi_cli_utf8_stream_finish(struct oi_cli_utf8_stream *stream,
                                    struct oi_cli_bytebuf *out);

#endif
