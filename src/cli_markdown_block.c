#include "cli_markdown_block.h"

#include "cli_markdown.h"
#include "cli_markdown_inline.h"
#include "cli_render_style.h"

void oi_cli_markdown_block_init(struct oi_cli_markdown_block *block) {
    oi_cli_bytebuf_init(&block->buffer);
    block->mode = OI_CLI_MD_BLOCK_NONE;
    block->fence_char = 0;
    block->fence_len = 0;
    block->overflowed = 0;
}

void oi_cli_markdown_block_free(struct oi_cli_markdown_block *block) {
    oi_cli_bytebuf_free(&block->buffer);
}

static int is_fence_char(char c) { return c == '`' || c == '~'; }

static oi_status write_newline(FILE *out) {
    return fputc('\n', out) == EOF ? OI_ERR_IO : OI_OK;
}

static oi_status write_inline(FILE *out, enum oi_cli_md_block_style style,
                              int heading_level, const char *text, size_t len,
                              int *style_active) {
    struct oi_cli_bytebuf out_text;
    struct oi_cli_md_span_list spans;
    oi_status status;

    oi_cli_bytebuf_init(&out_text);
    oi_cli_md_span_list_init(&spans);
    status = oi_cli_markdown_inline_parse(text, len, &out_text, &spans);
    if (status == OI_OK) {
        status = oi_cli_render_style_write_line(out, style, heading_level,
                                                out_text.data, out_text.len,
                                                &spans, style_active);
    }
    oi_cli_md_span_list_free(&spans);
    oi_cli_bytebuf_free(&out_text);
    if (status != OI_OK) {
        return status;
    }
    return write_newline(out);
}

static oi_status resolve_fence_line(struct oi_cli_markdown_block *block,
                                    const char *buf, size_t buf_len,
                                    FILE *out, int *style_active) {
    size_t k;
    int is_close = buf_len >= block->fence_len;

    for (k = 0; is_close && k < buf_len; k++) {
        if (buf[k] != block->fence_char) {
            is_close = 0;
        }
    }
    if (is_close) {
        block->mode = OI_CLI_MD_BLOCK_NONE;
        return OI_OK;
    }
    {
        oi_status status =
            oi_cli_render_style_write_verbatim(out, buf, buf_len, 1,
                                               style_active);
        if (status != OI_OK) {
            return status;
        }
    }
    return write_newline(out);
}

static oi_status resolve_line(struct oi_cli_markdown_block *block, FILE *out,
                              int *style_active) {
    const char *buf = block->buffer.data;
    size_t buf_len = block->buffer.len;

    if (block->mode == OI_CLI_MD_BLOCK_FENCE) {
        return resolve_fence_line(block, buf, buf_len, out, style_active);
    }

    if (buf_len == 0) {
        return write_newline(out);
    }

    if (is_fence_char(buf[0])) {
        char fc = buf[0];
        size_t n = 0;

        while (n < buf_len && buf[n] == fc) {
            n++;
        }
        if (n >= 3) {
            block->mode = OI_CLI_MD_BLOCK_FENCE;
            block->fence_char = fc;
            block->fence_len = n;
            return OI_OK;
        }
    }

    if (buf[0] == '#') {
        size_t n = 0;

        while (n < buf_len && n < 7 && buf[n] == '#') {
            n++;
        }
        if (n >= 1 && n <= 6 && (n == buf_len || buf[n] == ' ')) {
            size_t content_start = n < buf_len ? n + 1 : n;

            return write_inline(out, OI_CLI_MD_BLOCK_STYLE_HEADING, (int)n,
                                buf + content_start,
                                buf_len - content_start, style_active);
        }
    }

    {
        size_t marker_end = 0;
        int is_list = 0;

        if ((buf[0] == '-' || buf[0] == '*' || buf[0] == '+') &&
            buf_len > 1 && buf[1] == ' ') {
            marker_end = 2;
            is_list = 1;
        } else {
            size_t n = 0;

            while (n < buf_len && n < 9 && buf[n] >= '0' && buf[n] <= '9') {
                n++;
            }
            if (n >= 1 && n < buf_len && (buf[n] == '.' || buf[n] == ')') &&
                n + 1 < buf_len && buf[n + 1] == ' ') {
                marker_end = n + 2;
                is_list = 1;
            }
        }
        if (is_list) {
            oi_status status = oi_cli_render_style_write_verbatim(
                out, buf, marker_end, 1, style_active);

            if (status != OI_OK) {
                return status;
            }
            return write_inline(out, OI_CLI_MD_BLOCK_STYLE_LIST, 0,
                                buf + marker_end, buf_len - marker_end,
                                style_active);
        }
    }

    return write_inline(out, OI_CLI_MD_BLOCK_STYLE_NONE, 0, buf, buf_len,
                        style_active);
}

static oi_status flush_line_and_reset(struct oi_cli_markdown_block *block,
                                      FILE *out, int *style_active) {
    oi_status status = resolve_line(block, out, style_active);

    oi_cli_bytebuf_reset(&block->buffer);
    block->overflowed = 0;
    return status;
}

oi_status oi_cli_markdown_block_feed(struct oi_cli_markdown_block *block,
                                     const char *data, size_t len, FILE *out,
                                     int *style_active) {
    size_t i;

    if (block == NULL || (data == NULL && len != 0) || out == NULL ||
        style_active == NULL) {
        return OI_ERR_INVAL;
    }

    for (i = 0; i < len; i++) {
        char c = data[i];
        oi_status status;

        if (c == '\n') {
            status = flush_line_and_reset(block, out, style_active);
            if (status != OI_OK) {
                return status;
            }
            continue;
        }
        if (block->overflowed) {
            if (fputc((unsigned char)c, out) == EOF) {
                return OI_ERR_IO;
            }
            continue;
        }
        if (block->buffer.len >= OI_CLI_MARKDOWN_LINE_CAP) {
            if (block->buffer.len != 0 &&
                fwrite(block->buffer.data, 1, block->buffer.len, out) !=
                    block->buffer.len) {
                return OI_ERR_IO;
            }
            oi_cli_bytebuf_reset(&block->buffer);
            block->overflowed = 1;
            if (fputc((unsigned char)c, out) == EOF) {
                return OI_ERR_IO;
            }
            continue;
        }
        status = oi_cli_bytebuf_append(&block->buffer, &c, 1);
        if (status != OI_OK) {
            return status;
        }
    }
    return OI_OK;
}

oi_status oi_cli_markdown_block_finish(struct oi_cli_markdown_block *block,
                                       FILE *out, int *style_active) {
    if (block == NULL || out == NULL || style_active == NULL) {
        return OI_ERR_INVAL;
    }
    if (block->buffer.len == 0) {
        return OI_OK;
    }
    return flush_line_and_reset(block, out, style_active);
}
