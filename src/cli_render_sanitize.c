#include "cli_render_sanitize.h"

static void check_cap(struct oi_cli_sanitize_state *state) {
    if (state->sequence_bytes >= OI_CLI_SANITIZE_MAX_SEQUENCE) {
        state->phase = OI_CLI_SANITIZE_NORMAL;
        state->sequence_bytes = 0;
    }
}

static int is_string_introducer(unsigned char byte) {
    return byte == 'P' || byte == ']' || byte == 'X' || byte == '^' ||
           byte == '_';
}

void oi_cli_sanitize_init(struct oi_cli_sanitize_state *state) {
    state->phase = OI_CLI_SANITIZE_NORMAL;
    state->sequence_bytes = 0;
}

static int is_plain_passthrough(unsigned char byte) {
    if (byte == 0x1bU || byte == 0xc2U) {
        return 0;
    }
    if (byte == '\n' || byte == '\t') {
        return 1;
    }
    if (byte < 0x20U || byte == 0x7fU) {
        return 0;
    }
    return 1;
}

/*
 * Consumes one byte. Sets *reprocess when the byte was not consumed (a
 * held 0xC2 turned out not to be a C1 lead) -- the caller must call this
 * again with the same byte, now that state has advanced.
 */
static oi_status feed_one(struct oi_cli_sanitize_state *state,
                          unsigned char byte, struct oi_cli_bytebuf *out,
                          int *reprocess) {
    *reprocess = 0;

    switch (state->phase) {
    case OI_CLI_SANITIZE_NORMAL:
        if (byte == 0x1bU) {
            state->phase = OI_CLI_SANITIZE_ESCAPE;
            state->sequence_bytes = 1;
            return OI_OK;
        }
        if (byte == 0xc2U) {
            state->phase = OI_CLI_SANITIZE_SAW_C2;
            return OI_OK;
        }
        if (is_plain_passthrough(byte)) {
            return oi_cli_bytebuf_append(out, &byte, 1);
        }
        return OI_OK; /* stripped C0 control or DEL */

    case OI_CLI_SANITIZE_SAW_C2: {
        static const unsigned char lead_c2 = 0xc2U;
        oi_status status;

        state->phase = OI_CLI_SANITIZE_NORMAL;
        if (byte >= 0x80U && byte <= 0x9fU) {
            /* C1 control encoded as UTF-8 (0xC2 0x80-0xC2 0x9F); drop
             * both bytes. */
            return OI_OK;
        }
        status = oi_cli_bytebuf_append(out, &lead_c2, 1);
        if (status != OI_OK) {
            return status;
        }
        *reprocess = 1;
        return OI_OK;
    }

    case OI_CLI_SANITIZE_ESCAPE:
        if (byte == '[') {
            state->phase = OI_CLI_SANITIZE_CSI;
            state->sequence_bytes++;
            check_cap(state);
            return OI_OK;
        }
        if (is_string_introducer(byte)) {
            state->phase = OI_CLI_SANITIZE_STRING;
            state->sequence_bytes++;
            check_cap(state);
            return OI_OK;
        }
        if (byte >= 0x20U && byte <= 0x7eU) {
            /* Simple two-byte escape (ESC c, ESC =, ...), consumed. */
            state->phase = OI_CLI_SANITIZE_NORMAL;
            state->sequence_bytes = 0;
            return OI_OK;
        }
        /* Incomplete/malformed escape: drop the lone ESC and reprocess
         * this byte from a clean state. */
        state->phase = OI_CLI_SANITIZE_NORMAL;
        state->sequence_bytes = 0;
        *reprocess = 1;
        return OI_OK;

    case OI_CLI_SANITIZE_CSI:
        state->sequence_bytes++;
        if (byte >= 0x40U && byte <= 0x7eU) {
            state->phase = OI_CLI_SANITIZE_NORMAL;
            state->sequence_bytes = 0;
            return OI_OK;
        }
        check_cap(state);
        return OI_OK;

    case OI_CLI_SANITIZE_STRING:
        if (byte == 0x07U) {
            state->phase = OI_CLI_SANITIZE_NORMAL;
            state->sequence_bytes = 0;
            return OI_OK;
        }
        state->sequence_bytes++;
        if (byte == 0x1bU) {
            state->phase = OI_CLI_SANITIZE_STRING_ESC;
        }
        check_cap(state);
        return OI_OK;

    case OI_CLI_SANITIZE_STRING_ESC:
        state->sequence_bytes++;
        if (byte == '\\') {
            state->phase = OI_CLI_SANITIZE_NORMAL;
            state->sequence_bytes = 0;
            return OI_OK;
        }
        state->phase = OI_CLI_SANITIZE_STRING;
        check_cap(state);
        return OI_OK;
    }

    return OI_ERR_INVAL;
}

oi_status oi_cli_sanitize_feed(struct oi_cli_sanitize_state *state,
                               const unsigned char *in, size_t in_len,
                               struct oi_cli_bytebuf *out) {
    size_t i = 0;

    if (state == NULL || out == NULL || (in == NULL && in_len != 0)) {
        return OI_ERR_INVAL;
    }

    while (i < in_len) {
        if (state->phase == OI_CLI_SANITIZE_NORMAL &&
            is_plain_passthrough(in[i])) {
            size_t start = i;
            oi_status status;

            while (i < in_len && is_plain_passthrough(in[i])) {
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
            oi_status status = feed_one(state, in[i], out, &reprocess);

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

oi_status oi_cli_sanitize_finish(struct oi_cli_sanitize_state *state) {
    if (state == NULL) {
        return OI_ERR_INVAL;
    }
    state->phase = OI_CLI_SANITIZE_NORMAL;
    state->sequence_bytes = 0;
    return OI_OK;
}
