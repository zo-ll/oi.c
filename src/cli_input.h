#ifndef OI_CLI_INPUT_H
#define OI_CLI_INPUT_H

#include <stddef.h>

#include "oi/status.h"

enum oi_cli_input_event_type {
    OI_CLI_INPUT_NONE = 0,
    OI_CLI_INPUT_TEXT,
    OI_CLI_INPUT_ENTER,
    OI_CLI_INPUT_NEWLINE,
    OI_CLI_INPUT_LEFT,
    OI_CLI_INPUT_RIGHT,
    OI_CLI_INPUT_UP,
    OI_CLI_INPUT_DOWN,
    OI_CLI_INPUT_HOME,
    OI_CLI_INPUT_END,
    OI_CLI_INPUT_BACKSPACE,
    OI_CLI_INPUT_DELETE,
    OI_CLI_INPUT_TAB,
    OI_CLI_INPUT_CTRL_C,
    OI_CLI_INPUT_CTRL_D,
    OI_CLI_INPUT_ESCAPE,
    OI_CLI_INPUT_PASTE_BEGIN,
    OI_CLI_INPUT_PASTE_END,
    OI_CLI_INPUT_INVALID
};

struct oi_cli_input_event {
    enum oi_cli_input_event_type type;
    unsigned char text[4];
    size_t text_len;
    int pasted;
};

struct oi_cli_input_decoder {
    int pasting;
};

void oi_cli_input_decoder_init(struct oi_cli_input_decoder *decoder);

/*
 * Decodes one event from the start of `data`. OI_ERR_AGAIN means the bytes are
 * a valid but incomplete sequence and must be retained for the next read.
 * Otherwise `out_consumed` is always positive.
 */
oi_status oi_cli_input_decode(struct oi_cli_input_decoder *decoder,
                              const unsigned char *data, size_t len,
                              size_t *out_consumed,
                              struct oi_cli_input_event *out_event);

/* Resolves a buffered lone Escape after the caller's input timeout. */
oi_status oi_cli_input_decode_escape(size_t *out_consumed,
                                     struct oi_cli_input_event *out_event);

#endif
