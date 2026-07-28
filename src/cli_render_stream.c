#include "cli_render_stream.h"

#include "cli_render_style.h"

oi_status oi_cli_render_stream_init(struct oi_cli_render_stream *stream,
                                    FILE *out, int styling_enabled) {
    if (stream == NULL || out == NULL) {
        return OI_ERR_INVAL;
    }
    stream->out = out;
    stream->styling_enabled = styling_enabled != 0;
    oi_cli_utf8_stream_init(&stream->utf8);
    oi_cli_sanitize_init(&stream->sanitize);
    oi_cli_markdown_block_init(&stream->markdown);
    oi_cli_bytebuf_init(&stream->fixed);
    oi_cli_bytebuf_init(&stream->sanitized);
    stream->style_active = 0;
    return OI_OK;
}

static oi_status write_passthrough(struct oi_cli_render_stream *stream) {
    if (stream->styling_enabled) {
        return oi_cli_markdown_block_feed(&stream->markdown,
                                          stream->sanitized.data,
                                          stream->sanitized.len, stream->out,
                                          &stream->style_active);
    }
    if (stream->sanitized.len != 0 &&
        fwrite(stream->sanitized.data, 1, stream->sanitized.len,
               stream->out) != stream->sanitized.len) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_cli_render_stream_feed(struct oi_cli_render_stream *stream,
                                    const char *data, size_t len) {
    oi_status status;

    if (stream == NULL || stream->out == NULL || (data == NULL && len != 0)) {
        return OI_ERR_INVAL;
    }

    oi_cli_bytebuf_reset(&stream->fixed);
    oi_cli_bytebuf_reset(&stream->sanitized);

    status = oi_cli_utf8_stream_feed(&stream->utf8,
                                     (const unsigned char *)data, len,
                                     &stream->fixed);
    if (status != OI_OK) {
        return status;
    }
    status = oi_cli_sanitize_feed(
        &stream->sanitize, (const unsigned char *)stream->fixed.data,
        stream->fixed.len, &stream->sanitized);
    if (status != OI_OK) {
        return status;
    }
    status = write_passthrough(stream);
    if (status != OI_OK) {
        return status;
    }
    return fflush(stream->out) == 0 ? OI_OK : OI_ERR_IO;
}

oi_status oi_cli_render_stream_finish(struct oi_cli_render_stream *stream) {
    oi_status status;

    if (stream == NULL || stream->out == NULL) {
        return OI_ERR_INVAL;
    }

    oi_cli_bytebuf_reset(&stream->fixed);
    oi_cli_bytebuf_reset(&stream->sanitized);

    status = oi_cli_utf8_stream_finish(&stream->utf8, &stream->fixed);
    if (status != OI_OK) {
        return status;
    }
    if (stream->fixed.len != 0) {
        status = oi_cli_sanitize_feed(
            &stream->sanitize, (const unsigned char *)stream->fixed.data,
            stream->fixed.len, &stream->sanitized);
        if (status != OI_OK) {
            return status;
        }
    }
    status = oi_cli_sanitize_finish(&stream->sanitize);
    if (status != OI_OK) {
        return status;
    }
    status = write_passthrough(stream);
    if (status != OI_OK) {
        return status;
    }
    if (stream->styling_enabled) {
        status = oi_cli_markdown_block_finish(&stream->markdown, stream->out,
                                              &stream->style_active);
        if (status != OI_OK) {
            return status;
        }
    }
    status = oi_cli_render_style_reset(stream->out, &stream->style_active);
    if (status != OI_OK) {
        return status;
    }
    return fflush(stream->out) == 0 ? OI_OK : OI_ERR_IO;
}

void oi_cli_render_stream_free(struct oi_cli_render_stream *stream) {
    if (stream == NULL) {
        return;
    }
    oi_cli_markdown_block_free(&stream->markdown);
    oi_cli_bytebuf_free(&stream->fixed);
    oi_cli_bytebuf_free(&stream->sanitized);
}

oi_status oi_cli_render_sanitize_write(FILE *out, const char *data,
                                       size_t len) {
    struct oi_cli_utf8_stream utf8;
    struct oi_cli_sanitize_state sanitize;
    struct oi_cli_bytebuf fixed;
    struct oi_cli_bytebuf sanitized;
    oi_status status;

    if (out == NULL || (data == NULL && len != 0)) {
        return OI_ERR_INVAL;
    }

    oi_cli_utf8_stream_init(&utf8);
    oi_cli_sanitize_init(&sanitize);
    oi_cli_bytebuf_init(&fixed);
    oi_cli_bytebuf_init(&sanitized);

    status = oi_cli_utf8_stream_feed(&utf8, (const unsigned char *)data, len,
                                     &fixed);
    if (status == OI_OK) {
        status = oi_cli_utf8_stream_finish(&utf8, &fixed);
    }
    if (status == OI_OK && fixed.len != 0) {
        status = oi_cli_sanitize_feed(
            &sanitize, (const unsigned char *)fixed.data, fixed.len,
            &sanitized);
    }
    if (status == OI_OK) {
        status = oi_cli_sanitize_finish(&sanitize);
    }
    if (status == OI_OK && sanitized.len != 0 &&
        fwrite(sanitized.data, 1, sanitized.len, out) != sanitized.len) {
        status = OI_ERR_IO;
    }

    oi_cli_bytebuf_free(&fixed);
    oi_cli_bytebuf_free(&sanitized);
    return status;
}
