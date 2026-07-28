#include "cli_input.h"

#include "cli_utf8.h"

#include <string.h>

struct escape_mapping {
    const char *sequence;
    size_t len;
    enum oi_cli_input_event_type type;
};

static const struct escape_mapping escape_mappings[] = {
    {"\x1b[A", 3, OI_CLI_INPUT_UP},
    {"\x1b[B", 3, OI_CLI_INPUT_DOWN},
    {"\x1b[C", 3, OI_CLI_INPUT_RIGHT},
    {"\x1b[D", 3, OI_CLI_INPUT_LEFT},
    {"\x1b[H", 3, OI_CLI_INPUT_HOME},
    {"\x1b[F", 3, OI_CLI_INPUT_END},
    {"\x1b[1~", 4, OI_CLI_INPUT_HOME},
    {"\x1b[4~", 4, OI_CLI_INPUT_END},
    {"\x1b[7~", 4, OI_CLI_INPUT_HOME},
    {"\x1b[8~", 4, OI_CLI_INPUT_END},
    {"\x1b[3~", 4, OI_CLI_INPUT_DELETE},
    {"\x1b[200~", 6, OI_CLI_INPUT_PASTE_BEGIN},
};

static const unsigned char paste_end[] = "\x1b[201~";

static void set_simple_event(struct oi_cli_input_event *event,
                             enum oi_cli_input_event_type type) {
    memset(event, 0, sizeof *event);
    event->type = type;
}

static oi_status decode_text(const unsigned char *data, size_t len,
                             int pasted, size_t *out_consumed,
                             struct oi_cli_input_event *out_event) {
    size_t expected;
    oi_status status;

    if (data[0] <= 0x7fU) {
        expected = 1;
    } else if (data[0] >= 0xc2U && data[0] <= 0xdfU) {
        expected = 2;
    } else if (data[0] >= 0xe0U && data[0] <= 0xefU) {
        expected = 3;
    } else if (data[0] >= 0xf0U && data[0] <= 0xf4U) {
        expected = 4;
    } else {
        *out_consumed = 1;
        set_simple_event(out_event, OI_CLI_INPUT_INVALID);
        return OI_OK;
    }
    if (len < expected) {
        return OI_ERR_AGAIN;
    }

    status = oi_cli_utf8_sequence_length(data, len, &expected);
    if (status != OI_OK) {
        *out_consumed = 1;
        set_simple_event(out_event, OI_CLI_INPUT_INVALID);
        return OI_OK;
    }
    set_simple_event(out_event, OI_CLI_INPUT_TEXT);
    memcpy(out_event->text, data, expected);
    out_event->text_len = expected;
    out_event->pasted = pasted;
    *out_consumed = expected;
    return OI_OK;
}

static oi_status decode_escape(const unsigned char *data, size_t len,
                               size_t *out_consumed,
                               struct oi_cli_input_event *out_event,
                               struct oi_cli_input_decoder *decoder) {
    size_t i;
    int has_prefix = 0;

    for (i = 0; i < sizeof escape_mappings / sizeof escape_mappings[0]; i++) {
        const struct escape_mapping *mapping = &escape_mappings[i];
        size_t compared = len < mapping->len ? len : mapping->len;

        if (memcmp(data, mapping->sequence, compared) != 0) {
            continue;
        }
        if (len < mapping->len) {
            has_prefix = 1;
            continue;
        }
        set_simple_event(out_event, mapping->type);
        *out_consumed = mapping->len;
        if (mapping->type == OI_CLI_INPUT_PASTE_BEGIN) {
            decoder->pasting = 1;
        }
        return OI_OK;
    }
    if (has_prefix || len == 1) {
        return OI_ERR_AGAIN;
    }

    set_simple_event(out_event, OI_CLI_INPUT_INVALID);
    *out_consumed = 1;
    return OI_OK;
}

static oi_status decode_pasted(struct oi_cli_input_decoder *decoder,
                               const unsigned char *data, size_t len,
                               size_t *out_consumed,
                               struct oi_cli_input_event *out_event) {
    if (data[0] == '\x1b') {
        if (len < sizeof paste_end - 1 &&
            memcmp(data, paste_end, len) == 0) {
            return OI_ERR_AGAIN;
        }
        if (len >= sizeof paste_end - 1 &&
            memcmp(data, paste_end, sizeof paste_end - 1) == 0) {
            decoder->pasting = 0;
            *out_consumed = sizeof paste_end - 1;
            set_simple_event(out_event, OI_CLI_INPUT_PASTE_END);
            return OI_OK;
        }
    }
    if (data[0] == '\0') {
        *out_consumed = 1;
        set_simple_event(out_event, OI_CLI_INPUT_INVALID);
        return OI_OK;
    }
    return decode_text(data, len, 1, out_consumed, out_event);
}

void oi_cli_input_decoder_init(struct oi_cli_input_decoder *decoder) {
    if (decoder != NULL) {
        decoder->pasting = 0;
    }
}

oi_status oi_cli_input_decode(struct oi_cli_input_decoder *decoder,
                              const unsigned char *data, size_t len,
                              size_t *out_consumed,
                              struct oi_cli_input_event *out_event) {
    if (decoder == NULL || out_consumed == NULL || out_event == NULL ||
        (data == NULL && len != 0)) {
        return OI_ERR_INVAL;
    }
    *out_consumed = 0;
    set_simple_event(out_event, OI_CLI_INPUT_NONE);
    if (len == 0) {
        return OI_ERR_AGAIN;
    }
    if (decoder->pasting) {
        return decode_pasted(decoder, data, len, out_consumed, out_event);
    }

    switch (data[0]) {
    case '\x1b':
        return decode_escape(data, len, out_consumed, out_event, decoder);
    case '\r':
        set_simple_event(out_event, OI_CLI_INPUT_ENTER);
        break;
    case '\n':
        set_simple_event(out_event, OI_CLI_INPUT_NEWLINE);
        break;
    case '\x03':
        set_simple_event(out_event, OI_CLI_INPUT_CTRL_C);
        break;
    case '\x04':
        set_simple_event(out_event, OI_CLI_INPUT_CTRL_D);
        break;
    case '\x7f':
    case '\x08':
        set_simple_event(out_event, OI_CLI_INPUT_BACKSPACE);
        break;
    case '\t':
        set_simple_event(out_event, OI_CLI_INPUT_TAB);
        break;
    case '\x01':
        set_simple_event(out_event, OI_CLI_INPUT_HOME);
        break;
    case '\x05':
        set_simple_event(out_event, OI_CLI_INPUT_END);
        break;
    default:
        if (data[0] < 0x20U) {
            set_simple_event(out_event, OI_CLI_INPUT_INVALID);
            *out_consumed = 1;
            return OI_OK;
        }
        return decode_text(data, len, 0, out_consumed, out_event);
    }
    *out_consumed = 1;
    return OI_OK;
}

oi_status oi_cli_input_decode_escape(size_t *out_consumed,
                                     struct oi_cli_input_event *out_event) {
    if (out_consumed == NULL || out_event == NULL) {
        return OI_ERR_INVAL;
    }
    *out_consumed = 1;
    set_simple_event(out_event, OI_CLI_INPUT_ESCAPE);
    return OI_OK;
}
