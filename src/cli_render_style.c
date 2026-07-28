#include "cli_render_style.h"

#include <string.h>

static oi_status apply_combined(FILE *out, int bold, int italic,
                                int underline, int code, int dim,
                                int *style_active) {
    char params[16];
    size_t len = 0;

    if (!bold && !italic && !underline && !code && !dim) {
        if (!*style_active) {
            return OI_OK;
        }
        if (fwrite("\x1b[0m", 1, 4, out) != 4) {
            return OI_ERR_IO;
        }
        *style_active = 0;
        return OI_OK;
    }

#define OI_APPEND_CODE(code_str)                                            \
    do {                                                                    \
        size_t clen = strlen(code_str);                                    \
        if (len != 0) {                                                    \
            params[len++] = ';';                                           \
        }                                                                   \
        memcpy(params + len, code_str, clen);                              \
        len += clen;                                                       \
    } while (0)

    if (bold) {
        OI_APPEND_CODE("1");
    }
    if (italic) {
        OI_APPEND_CODE("3");
    }
    if (underline) {
        OI_APPEND_CODE("4");
    }
    if (dim) {
        OI_APPEND_CODE("2");
    }
    if (code) {
        OI_APPEND_CODE("36");
    }
#undef OI_APPEND_CODE
    params[len] = '\0';

    {
        char seq[32];
        int n = snprintf(seq, sizeof seq, "\x1b[0m\x1b[%sm", params);

        if (n < 0 || (size_t)n >= sizeof seq) {
            return OI_ERR_INVAL;
        }
        if (fwrite(seq, 1, (size_t)n, out) != (size_t)n) {
            return OI_ERR_IO;
        }
    }
    *style_active = 1;
    return OI_OK;
}

oi_status oi_cli_render_style_write_line(
    FILE *out, enum oi_cli_md_block_style block_style, int heading_level,
    const char *text, size_t text_len,
    const struct oi_cli_md_span_list *spans, int *style_active) {
    int heading_bold = block_style == OI_CLI_MD_BLOCK_STYLE_HEADING;
    int heading_underline = heading_bold && heading_level <= 2;
    size_t i;

    if (out == NULL || (text == NULL && text_len != 0) || spans == NULL ||
        style_active == NULL) {
        return OI_ERR_INVAL;
    }

    for (i = 0; i < spans->len; i++) {
        const struct oi_cli_md_run *run = &spans->runs[i];
        int bold = heading_bold || (run->style_bits & OI_CLI_MD_STYLE_BOLD);
        int italic = (run->style_bits & OI_CLI_MD_STYLE_ITALIC) != 0;
        int code = (run->style_bits & OI_CLI_MD_STYLE_CODE) != 0;
        oi_status status = apply_combined(out, bold, italic, heading_underline,
                                          code, 0, style_active);

        if (status != OI_OK) {
            return status;
        }
        if (run->len != 0 &&
            fwrite(text + run->start, 1, run->len, out) != run->len) {
            return OI_ERR_IO;
        }
    }
    return OI_OK;
}

oi_status oi_cli_render_style_write_verbatim(FILE *out, const char *text,
                                             size_t len, int dim,
                                             int *style_active) {
    oi_status status;

    if (out == NULL || (text == NULL && len != 0) || style_active == NULL) {
        return OI_ERR_INVAL;
    }
    status = apply_combined(out, 0, 0, 0, 0, dim != 0, style_active);
    if (status != OI_OK) {
        return status;
    }
    if (len != 0 && fwrite(text, 1, len, out) != len) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_cli_render_style_reset(FILE *out, int *style_active) {
    if (out == NULL || style_active == NULL) {
        return OI_ERR_INVAL;
    }
    if (!*style_active) {
        return OI_OK;
    }
    if (fwrite("\x1b[0m", 1, 4, out) != 4) {
        return OI_ERR_IO;
    }
    *style_active = 0;
    return OI_OK;
}
