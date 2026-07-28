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
    if (fwrite(prefix, 1, sizeof prefix - 1, stream) != sizeof prefix - 1 ||
        (name->len > 0 &&
         fwrite(name->data, 1, name->len, stream) != name->len) ||
        putc('\n', stream) == EOF || fflush(stream) != 0) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

static oi_status write_model_error(
    FILE *stream, const struct oi_cli_conversation_event *event) {
    char header[96];
    int header_len = snprintf(header, sizeof header,
                              "oi: model error body (http=%d): ",
                              event->as.model_error.http_status);
    if (header_len < 0 || (size_t)header_len >= sizeof header ||
        fwrite(header, 1, (size_t)header_len, stream) !=
            (size_t)header_len ||
        (event->as.model_error.body_len > 0 &&
         fwrite(event->as.model_error.body, 1,
                event->as.model_error.body_len, stream) !=
             event->as.model_error.body_len) ||
        putc('\n', stream) == EOF) {
        return OI_ERR_IO;
    }
    return OI_OK;
}

oi_status oi_cli_present_init(
    struct oi_cli_present *present, FILE *out, FILE *err,
    int capture_assistant, oi_cli_conversation_event_cb external_event,
    void *external_user_data) {
    if (present == NULL || out == NULL || err == NULL) {
        return OI_ERR_INVAL;
    }
    memset(present, 0, sizeof *present);
    present->out = out;
    present->err = err;
    present->capture_assistant = capture_assistant != 0;
    present->status = OI_OK;
    present->external_event = external_event;
    present->external_user_data = external_user_data;
    return OI_OK;
}

void oi_cli_present_free(struct oi_cli_present *present) {
    if (present == NULL) {
        return;
    }
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
        if ((event->as.bytes.len > 0 &&
             fwrite(event->as.bytes.data, 1, event->as.bytes.len,
                    present->out) != event->as.bytes.len) ||
            fflush(present->out) != 0) {
            return OI_ERR_IO;
        }
        return assistant_append(present, event->as.bytes.data,
                                event->as.bytes.len);
    case OI_CLI_CONVERSATION_EVENT_TOOL_STARTING:
        return write_tool_start(present->err, event->as.tool_starting.name);
    case OI_CLI_CONVERSATION_EVENT_RESPONSE_DONE:
        return putc('\n', present->out) == EOF ? OI_ERR_IO : OI_OK;
    case OI_CLI_CONVERSATION_EVENT_MODEL_ERROR:
        return write_model_error(present->err, event);
    case OI_CLI_CONVERSATION_EVENT_TURN_DONE:
        present->status = event->as.turn_done.status;
        present->done = 1;
        return OI_OK;
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
