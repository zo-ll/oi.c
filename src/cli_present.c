#include "cli_present.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static oi_status assistant_append(struct oi_cli_present *present,
                                  const char *data, size_t len) {
    size_t needed;

    if (!present->capture_assistant) {
        return OI_OK;
    }
    if (len > SIZE_MAX - present->assistant_len - 1) {
        return OI_ERR_NOMEM;
    }
    needed = present->assistant_len + len + 1;
    if (needed > present->assistant_cap) {
        size_t cap = present->assistant_cap == 0 ? 256
                                                : present->assistant_cap;
        char *next;

        while (cap < needed) {
            if (cap > SIZE_MAX / 2) {
                return OI_ERR_NOMEM;
            }
            cap *= 2;
        }
        next = realloc(present->assistant, cap);
        if (next == NULL) {
            return OI_ERR_NOMEM;
        }
        present->assistant = next;
        present->assistant_cap = cap;
    }
    if (len != 0) {
        memcpy(present->assistant + present->assistant_len, data, len);
    }
    present->assistant_len += len;
    present->assistant[present->assistant_len] = '\0';
    return OI_OK;
}

static oi_status write_tool_start(FILE *stream,
                                  const struct oi_cli_string *name) {
    static const char prefix[] = "oi: running tool ";
    oi_status status;

    if (fwrite(prefix, 1, sizeof prefix - 1, stream) != sizeof prefix - 1) {
        return OI_ERR_IO;
    }
    if (name->len > 0) {
        status = oi_cli_render_sanitize_write(stream, name->data, name->len);
        if (status != OI_OK) {
            return status;
        }
    }
    if (putc('\n', stream) == EOF || fflush(stream) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

/* The byte length of the bidirectional control character at `text`
 * (U+061C, U+200E-U+200F, U+202A-U+202E, U+2066-U+2069), else 0. */
static size_t bidi_control_at(const char *text, size_t len,
                              unsigned *out_codepoint) {
    const unsigned char *b = (const unsigned char *)text;
    if (len >= 2 && b[0] == 0xD8 && b[1] == 0x9C) {
        *out_codepoint = 0x061C;
        return 2;
    }
    if (len >= 3 && b[0] == 0xE2 && (b[1] == 0x80 || b[1] == 0x81)) {
        unsigned codepoint = 0x2000u + ((b[1] & 0x3Fu) << 6) + (b[2] & 0x3Fu);
        if (codepoint == 0x200E || codepoint == 0x200F ||
            (codepoint >= 0x202A && codepoint <= 0x202E) ||
            (codepoint >= 0x2066 && codepoint <= 0x2069)) {
            *out_codepoint = codepoint;
            return 3;
        }
    }
    return 0;
}

/* Sanitized like other untrusted text, and with each bidirectional control
 * as a visible <U+XXXX>: such a character can make the terminal show a
 * command in a different order than the shell runs it. */
static oi_status write_request_text(FILE *stream, const char *text,
                                    size_t len) {
    size_t start = 0;
    size_t i = 0;
    while (i <= len) {
        unsigned codepoint = 0;
        size_t control =
            i < len ? bidi_control_at(text + i, len - i, &codepoint) : 0;
        if (i < len && control == 0) {
            i++;
            continue;
        }
        if (i > start) {
            oi_status status =
                oi_cli_render_sanitize_write(stream, text + start, i - start);
            if (status != OI_OK) {
                return status;
            }
        }
        if (i == len) {
            break;
        }
        if (fprintf(stream, "<U+%04X>", codepoint) < 0) {
            return OI_ERR_IO;
        }
        i += control;
        start = i;
    }
    return OI_OK;
}

/*
 * Writes what the user approves, in full and before the selector: the
 * decoded `command` string when there is one, else the raw arguments. The
 * terminal wraps it and keeps it in scrollback, so no length or JSON
 * escape can hide a part of it.
 */
static oi_status write_permission_request(
    FILE *stream, const struct oi_cli_conversation_event *event) {
    static const char prefix[] = "oi: approve tool ";
    const struct oi_cli_string *name = event->as.awaiting_permission.name;
    const char *text = event->as.awaiting_permission.arguments->data;
    size_t text_len = event->as.awaiting_permission.arguments->len;
    const char *command = NULL;
    size_t command_len = 0;
    oi_status status;

    if (oi_json_get_string(
            oi_json_object_get(event->as.awaiting_permission.parsed_arguments,
                               "command"),
            &command, &command_len) == OI_OK) {
        text = command;
        text_len = command_len;
    }
    if (fwrite(prefix, 1, sizeof prefix - 1, stream) != sizeof prefix - 1) {
        return OI_ERR_IO;
    }
    if (name->len > 0) {
        status = oi_cli_render_sanitize_write(stream, name->data, name->len);
        if (status != OI_OK) {
            return status;
        }
    }
    if (fputs(command != NULL ? ", command:\n" : ", arguments:\n", stream) ==
        EOF) {
        return OI_ERR_IO;
    }
    status = write_request_text(stream, text, text_len);
    if (status != OI_OK) {
        return status;
    }
    if (putc('\n', stream) == EOF || fflush(stream) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static oi_status write_model_error(
    FILE *stream, const struct oi_cli_conversation_event *event) {
    char header[96];
    /* http_status 0: no response arrived, and the body names the
     * connection failure. */
    int header_len =
        event->as.model_error.http_status == 0
            ? snprintf(header, sizeof header, "oi: connection failed: ")
            : snprintf(header, sizeof header,
                       "oi: model error body (http=%d): ",
                       event->as.model_error.http_status);
    oi_status status;

    if (header_len < 0 || (size_t)header_len >= sizeof header ||
        fwrite(header, 1, (size_t)header_len, stream) !=
            (size_t)header_len) {
        return OI_ERR_IO;
    }
    if (event->as.model_error.body_len > 0) {
        status = oi_cli_render_sanitize_write(
            stream, event->as.model_error.body,
            event->as.model_error.body_len);
        if (status != OI_OK) {
            return status;
        }
    }
    if (putc('\n', stream) == EOF) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_cli_present_init(
    struct oi_cli_present *present, FILE *out, FILE *err,
    int capture_assistant, int styling_enabled,
    oi_cli_conversation_event_cb external_event,
    void *external_user_data) {
    oi_status status;

    if (present == NULL || out == NULL || err == NULL) {
        return OI_ERR_INVAL;
    }
    memset(present, 0, sizeof *present);
    status = oi_cli_render_stream_init(&present->assistant_stream, out,
                                       styling_enabled);
    if (status != OI_OK) {
        return status;
    }
    present->out = out;
    present->err = err;
    present->capture_assistant = capture_assistant != 0;
    present->styling_enabled = styling_enabled != 0;
    present->status = OI_OK;
    present->external_event = external_event;
    present->external_user_data = external_user_data;
    return OI_OK;
}

void oi_cli_present_free(struct oi_cli_present *present) {
    if (present == NULL) {
        return;
    }
    oi_cli_render_stream_free(&present->assistant_stream);
    free(present->assistant);
    memset(present, 0, sizeof *present);
}

void oi_cli_present_reset_turn(struct oi_cli_present *present) {
    if (present == NULL) {
        return;
    }
    present->done = 0;
    present->status = OI_OK;
    present->assistant_len = 0;
    if (present->assistant != NULL) {
        present->assistant[0] = '\0';
    }
}

oi_status oi_cli_present_event(
    const struct oi_cli_conversation_event *event, void *user_data) {
    struct oi_cli_present *present = user_data;
    oi_status status;

    if (event == NULL || present == NULL || present->out == NULL ||
        present->err == NULL) {
        return OI_ERR_INVAL;
    }
    if (present->external_event != NULL) {
        status = present->external_event(event,
                                         present->external_user_data);
        if (status != OI_OK) {
            return status;
        }
    }
    switch (event->type) {
    case OI_CLI_CONVERSATION_EVENT_ASSISTANT_DELTA:
        status = oi_cli_render_stream_feed(&present->assistant_stream,
                                           event->as.bytes.data,
                                           event->as.bytes.len);
        if (status != OI_OK) {
            return status;
        }
        return assistant_append(present, event->as.bytes.data,
                                event->as.bytes.len);
    case OI_CLI_CONVERSATION_EVENT_TOOL_STARTING:
        return write_tool_start(present->err, event->as.tool_starting.name);
    case OI_CLI_CONVERSATION_EVENT_RESPONSE_DONE:
        status = oi_cli_render_stream_finish(&present->assistant_stream);
        if (status != OI_OK) {
            return status;
        }
        /* Styled (REPL) output already ends with exactly one newline per
         * line via cli_markdown_block, including a synthesized one for a
         * final line the model didn't itself terminate -- an unconditional
         * newline here would add a spurious blank line. One-shot's plain
         * passthrough has no such per-line bookkeeping, so it still needs
         * this to guarantee the response ends on its own line. */
        if (present->styling_enabled) {
            return OI_OK;
        }
        return putc('\n', present->out) == EOF ? OI_ERR_IO : OI_OK;
    case OI_CLI_CONVERSATION_EVENT_MODEL_ERROR:
        return write_model_error(present->err, event);
    case OI_CLI_CONVERSATION_EVENT_TURN_DONE: {
        oi_status finish_status =
            oi_cli_render_stream_finish(&present->assistant_stream);

        present->status = event->as.turn_done.status != OI_OK
                               ? event->as.turn_done.status
                               : finish_status;
        present->done = 1;
        return finish_status;
    }
    case OI_CLI_CONVERSATION_EVENT_AWAITING_PERMISSION:
        return write_permission_request(present->err, event);
    case OI_CLI_CONVERSATION_EVENT_MESSAGE:
    case OI_CLI_CONVERSATION_EVENT_TOOL_OUTPUT:
    case OI_CLI_CONVERSATION_EVENT_PARTIAL_ASSISTANT:
        return OI_OK;
    }
    return OI_ERR_INVAL;
}

oi_status oi_cli_present_take_assistant(struct oi_cli_present *present,
                                        char **out_text, size_t *out_len) {
    oi_status status;

    if (present == NULL || out_text == NULL || out_len == NULL ||
        !present->capture_assistant) {
        return OI_ERR_INVAL;
    }
    if (present->assistant == NULL) {
        status = assistant_append(present, "", 0);
        if (status != OI_OK) {
            return status;
        }
    }
    *out_text = present->assistant;
    *out_len = present->assistant_len;
    present->assistant = NULL;
    present->assistant_len = 0;
    present->assistant_cap = 0;
    return OI_OK;
}
