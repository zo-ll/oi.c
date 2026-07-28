#ifndef OI_CLI_UTF8_STREAM_H
#define OI_CLI_UTF8_STREAM_H

#include <stddef.h>

#include "cli_bytebuf.h"
#include "oi/status.h"

/*
 * Incremental UTF-8 fixer: carries a partial trailing multi-byte sequence
 * across feed() calls and emits deterministic U+FFFD replacement for
 * malformed or truncated input, without ever reading past the bytes it was
 * given. Output is always well-formed UTF-8.
 */
struct oi_cli_utf8_stream {
    unsigned char pending[4];
    size_t pending_len;
    size_t expected_len;
};

void oi_cli_utf8_stream_init(struct oi_cli_utf8_stream *stream);

/*
 * Appends well-formed UTF-8 to *out. A valid sequence split across calls is
 * held in `pending` until it completes. A byte that can't extend what's
 * currently held in `pending` causes one U+FFFD to be emitted for the
 * held prefix; that new byte is then reprocessed from a clean state (it is
 * not itself dropped unless it is independently invalid). A syntactically
 * complete but illegal sequence (overlong/surrogate/out-of-range) also
 * emits one U+FFFD, consuming all of its bytes.
 */
oi_status oi_cli_utf8_stream_feed(struct oi_cli_utf8_stream *stream,
                                  const unsigned char *in, size_t in_len,
                                  struct oi_cli_bytebuf *out);

/*
 * End of input: if a sequence is still pending (truncated), emits exactly
 * one U+FFFD and clears it. Safe to call more than once.
 */
oi_status oi_cli_utf8_stream_finish(struct oi_cli_utf8_stream *stream,
                                    struct oi_cli_bytebuf *out);

#endif
