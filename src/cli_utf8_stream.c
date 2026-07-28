#include "cli_utf8_stream.h"

#include "cli_utf8.h"

static const unsigned char replacement[3] = {0xef, 0xbf, 0xbd};

static oi_status emit_replacement(struct oi_cli_bytebuf *out) {
    return oi_cli_bytebuf_append(out, replacement, sizeof replacement);
}

static int is_continuation(unsigned char byte) {
    return (byte & 0xc0U) == 0x80U;
}

void oi_cli_utf8_stream_init(struct oi_cli_utf8_stream *stream) {
    stream->pending_len = 0;
    stream->expected_len = 0;
}

static oi_status flush_pending(struct oi_cli_utf8_stream *stream,
                               struct oi_cli_bytebuf *out) {
    size_t sequence_len;
    oi_status status = oi_cli_utf8_sequence_length(
        stream->pending, stream->pending_len, &sequence_len);

    if (status != OI_OK || sequence_len != stream->pending_len) {
        stream->pending_len = 0;
        return emit_replacement(out);
    }
    status = oi_cli_bytebuf_append(out, stream->pending, stream->pending_len);
    stream->pending_len = 0;
    return status;
}

/*
 * Consumes one byte. Sets *reprocess when the byte was not consumed (a
 * pending sequence broke, and this byte must be reconsidered from a clean
 * state) -- the caller must call this again with the same byte.
 */
static oi_status feed_one(struct oi_cli_utf8_stream *stream,
                          unsigned char byte, struct oi_cli_bytebuf *out,
                          int *reprocess) {
    *reprocess = 0;

    if (stream->pending_len == 0) {
        size_t lead_len;
        oi_status status = oi_cli_utf8_lead_length(byte, &lead_len);

        if (status != OI_OK) {
            return emit_replacement(out);
        }
        if (lead_len == 1) {
            return oi_cli_bytebuf_append(out, &byte, 1);
        }
        stream->pending[0] = byte;
        stream->pending_len = 1;
        stream->expected_len = lead_len;
        return OI_OK;
    }

    if (!is_continuation(byte)) {
        oi_status status;

        stream->pending_len = 0;
        status = emit_replacement(out);
        if (status != OI_OK) {
            return status;
        }
        *reprocess = 1;
        return OI_OK;
    }

    stream->pending[stream->pending_len] = byte;
    stream->pending_len++;
    if (stream->pending_len < stream->expected_len) {
        return OI_OK;
    }
    return flush_pending(stream, out);
}

oi_status oi_cli_utf8_stream_feed(struct oi_cli_utf8_stream *stream,
                                  const unsigned char *in, size_t in_len,
                                  struct oi_cli_bytebuf *out) {
    size_t i = 0;

    if (stream == NULL || out == NULL || (in == NULL && in_len != 0)) {
        return OI_ERR_INVAL;
    }

    while (i < in_len) {
        if (stream->pending_len == 0 && in[i] < 0x80U) {
            size_t start = i;
            oi_status status;

            while (i < in_len && in[i] < 0x80U) {
                i++;
            }
            status = oi_cli_bytebuf_append(out, in + start, i - start);
            if (status != OI_OK) {
                return status;
            }
            continue;
        }

        {
            int reprocess;
            oi_status status = feed_one(stream, in[i], out, &reprocess);

            if (status != OI_OK) {
                return status;
            }
            if (!reprocess) {
                i++;
            }
        }
    }
    return OI_OK;
}

oi_status oi_cli_utf8_stream_finish(struct oi_cli_utf8_stream *stream,
                                    struct oi_cli_bytebuf *out) {
    if (stream == NULL || out == NULL) {
        return OI_ERR_INVAL;
    }
    if (stream->pending_len == 0) {
        return OI_OK;
    }
    stream->pending_len = 0;
    return emit_replacement(out);
}
