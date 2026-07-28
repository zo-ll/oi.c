#include "cli_history_codec.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oi/json.h"

static oi_status write_key(oi_json_writer *writer, const char *key,
                           size_t key_len) {
    return oi_json_write_object_key(writer, key, key_len);
}

static oi_status write_uint64_string(oi_json_writer *writer, uint64_t value) {
    char number[32];
    int len = snprintf(number, sizeof number, "%" PRIu64, value);
    if (len < 0 || (size_t)len >= sizeof number) {
        return OI_ERR_INVAL;
    }
    return oi_json_write_string(writer, number, (size_t)len);
}

static const char *record_kind_name(enum oi_cli_history_record_kind kind,
                                    size_t *out_len) {
    const char *name;
    switch (kind) {
    case OI_CLI_HISTORY_RECORD_TRANSITION:
        name = "transition";
        break;
    case OI_CLI_HISTORY_RECORD_MESSAGE:
        name = "message";
        break;
    case OI_CLI_HISTORY_RECORD_TOOL_STARTED:
        name = "tool_started";
        break;
    case OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT:
        name = "partial_assistant";
        break;
    case OI_CLI_HISTORY_RECORD_QUEUED_INPUT:
        name = "queued_input";
        break;
    case OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED:
        name = "queue_resolved";
        break;
    case OI_CLI_HISTORY_RECORD_CHECKPOINT:
        name = "checkpoint";
        break;
    case OI_CLI_HISTORY_RECORD_NONE:
        return NULL;
    }
    *out_len = strlen(name);
    return name;
}

static const char *message_role_name(enum oi_cli_message_role role,
                                     size_t *out_len) {
    const char *name;
    switch (role) {
    case OI_CLI_MESSAGE_USER:
        name = "user";
        break;
    case OI_CLI_MESSAGE_ASSISTANT:
        name = "assistant";
        break;
    case OI_CLI_MESSAGE_TOOL:
        name = "tool";
        break;
    case OI_CLI_MESSAGE_NONE:
        return NULL;
    }
    *out_len = strlen(name);
    return name;
}

static const char *tool_outcome_name(enum oi_cli_history_tool_outcome outcome,
                                     size_t *out_len) {
    const char *name;
    switch (outcome) {
    case OI_CLI_HISTORY_TOOL_COMPLETED:
        name = "completed";
        break;
    case OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN:
        name = "outcome_unknown";
        break;
    case OI_CLI_HISTORY_TOOL_NOT_EXECUTED:
        name = "not_executed";
        break;
    case OI_CLI_HISTORY_TOOL_OUTCOME_NONE:
        return NULL;
    }
    *out_len = strlen(name);
    return name;
}

static oi_status base64_encode(const unsigned char *data, size_t len,
                               char **out, size_t *out_len) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (len > (SIZE_MAX - 2) / 3) {
        return OI_ERR_NOMEM;
    }
    size_t groups = (len + 2) / 3;
    if (groups > (SIZE_MAX - 1) / 4) {
        return OI_ERR_NOMEM;
    }
    size_t encoded_len = groups * 4;
    char *encoded = malloc(encoded_len + 1);
    if (encoded == NULL) {
        return OI_ERR_NOMEM;
    }

    size_t input = 0;
    size_t output = 0;
    while (input + 3 <= len) {
        uint32_t value = ((uint32_t)data[input] << 16) |
                         ((uint32_t)data[input + 1] << 8) |
                         (uint32_t)data[input + 2];
        encoded[output++] = alphabet[(value >> 18) & 0x3fu];
        encoded[output++] = alphabet[(value >> 12) & 0x3fu];
        encoded[output++] = alphabet[(value >> 6) & 0x3fu];
        encoded[output++] = alphabet[value & 0x3fu];
        input += 3;
    }
    size_t remaining = len - input;
    if (remaining > 0) {
        uint32_t value = (uint32_t)data[input] << 16;
        if (remaining == 2) {
            value |= (uint32_t)data[input + 1] << 8;
        }
        encoded[output++] = alphabet[(value >> 18) & 0x3fu];
        encoded[output++] = alphabet[(value >> 12) & 0x3fu];
        encoded[output++] =
            remaining == 2 ? alphabet[(value >> 6) & 0x3fu] : '=';
        encoded[output++] = '=';
    }
    encoded[output] = '\0';
    *out = encoded;
    *out_len = output;
    return OI_OK;
}

static oi_status write_tool_calls(
    oi_json_writer *writer, const struct oi_cli_message *message) {
    oi_status st = write_key(writer, "tool_calls", 10);
    if (st == OI_OK) {
        st = oi_json_write_array_begin(writer);
    }
    for (size_t i = 0; st == OI_OK && i < message->tool_calls_len; i++) {
        const struct oi_cli_tool_call_value *call = &message->tool_calls[i];
        st = oi_json_write_object_begin(writer);
        if (st == OI_OK) {
            st = write_key(writer, "id", 2);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer, call->id.data, call->id.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "name", 4);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer, call->name.data,
                                      call->name.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "arguments", 9);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer, call->arguments.data,
                                      call->arguments.len);
        }
        if (st == OI_OK) {
            st = oi_json_write_object_end(writer);
        }
    }
    if (st == OI_OK) {
        st = oi_json_write_array_end(writer);
    }
    return st;
}

static oi_status write_message(oi_json_writer *writer,
                               const struct oi_cli_history_message *history) {
    const struct oi_cli_message *message = &history->value;
    size_t name_len;
    const char *name = message_role_name(message->role, &name_len);
    if (name == NULL) {
        return OI_ERR_INVAL;
    }

    oi_status st = write_key(writer, "role", 4);
    if (st == OI_OK) {
        st = oi_json_write_string(writer, name, name_len);
    }
    if (st == OI_OK) {
        st = write_key(writer, "content", 7);
    }
    if (st == OI_OK) {
        st = oi_json_write_string(writer, message->content.data,
                                  message->content.len);
    }
    if (st == OI_OK) {
        st = write_key(writer, "source", 6);
    }
    if (st == OI_OK) {
        name = history->source == OI_CLI_HISTORY_MESSAGE_NORMAL ? "normal"
                                                                : "repair";
        st = oi_json_write_string(writer, name, strlen(name));
    }
    if (st == OI_OK && history->model.data != NULL) {
        st = write_key(writer, "model", 5);
        if (st == OI_OK) {
            st = oi_json_write_string(writer, history->model.data,
                                      history->model.len);
        }
    }
    if (st == OI_OK && message->role == OI_CLI_MESSAGE_ASSISTANT &&
        message->tool_calls_len > 0) {
        st = write_tool_calls(writer, message);
    }
    if (st == OI_OK && message->role == OI_CLI_MESSAGE_TOOL) {
        st = write_key(writer, "tool_call_id", 12);
        if (st == OI_OK) {
            st = oi_json_write_string(writer, message->tool_call_id.data,
                                      message->tool_call_id.len);
        }
        name = tool_outcome_name(history->tool_outcome, &name_len);
        if (st == OI_OK && name == NULL) {
            st = OI_ERR_INVAL;
        }
        if (st == OI_OK) {
            st = write_key(writer, "tool_outcome", 12);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer, name, name_len);
        }
        if (st == OI_OK && history->has_raw_tool_output) {
            char *encoded = NULL;
            size_t encoded_len = 0;
            st = base64_encode(history->raw_tool_output.data,
                               history->raw_tool_output.len, &encoded,
                               &encoded_len);
            if (st == OI_OK) {
                st = write_key(writer, "raw_output_base64", 17);
            }
            if (st == OI_OK) {
                st = oi_json_write_string(writer, encoded, encoded_len);
            }
            free(encoded);
        }
    }
    return st;
}

static oi_status write_payload(oi_json_writer *writer,
                               const struct oi_cli_history_record *record) {
    oi_status st = OI_OK;
    switch (record->kind) {
    case OI_CLI_HISTORY_RECORD_TRANSITION:
        st = write_key(writer, "legacy_record_count", 19);
        if (st == OI_OK) {
            st = write_uint64_string(
                writer, record->as.transition.legacy_record_count);
        }
        break;
    case OI_CLI_HISTORY_RECORD_MESSAGE:
        st = write_message(writer, &record->as.message);
        break;
    case OI_CLI_HISTORY_RECORD_TOOL_STARTED:
        st = write_key(writer, "tool_call_id", 12);
        if (st == OI_OK) {
            st = oi_json_write_string(
                writer, record->as.tool_started.tool_call_id.data,
                record->as.tool_started.tool_call_id.len);
        }
        break;
    case OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT:
        st = write_key(writer, "content", 7);
        if (st == OI_OK) {
            st = oi_json_write_string(
                writer, record->as.partial_assistant.content.data,
                record->as.partial_assistant.content.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "model", 5);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(
                writer, record->as.partial_assistant.model.data,
                record->as.partial_assistant.model.len);
        }
        break;
    case OI_CLI_HISTORY_RECORD_QUEUED_INPUT:
        st = write_key(writer, "content", 7);
        if (st == OI_OK) {
            st = oi_json_write_string(writer,
                                      record->as.queued_input.content.data,
                                      record->as.queued_input.content.len);
        }
        break;
    case OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED: {
        st = write_key(writer, "queued_record_id", 16);
        if (st == OI_OK) {
            st = write_uint64_string(
                writer, record->as.queue_resolved.queued_record_id);
        }
        if (st == OI_OK) {
            st = write_key(writer, "resolution", 10);
        }
        const char *resolution =
            record->as.queue_resolved.resolution ==
                    OI_CLI_HISTORY_QUEUE_CONSUMED
                ? "consumed"
                : "discarded";
        if (st == OI_OK) {
            st = oi_json_write_string(writer, resolution,
                                      strlen(resolution));
        }
        break;
    }
    case OI_CLI_HISTORY_RECORD_CHECKPOINT:
        st = write_key(writer, "summary", 7);
        if (st == OI_OK) {
            st = oi_json_write_string(writer,
                                      record->as.checkpoint.summary.data,
                                      record->as.checkpoint.summary.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "model", 5);
        }
        if (st == OI_OK) {
            st = oi_json_write_string(writer,
                                      record->as.checkpoint.model.data,
                                      record->as.checkpoint.model.len);
        }
        if (st == OI_OK) {
            st = write_key(writer, "source_first_record_id", 22);
        }
        if (st == OI_OK) {
            st = write_uint64_string(
                writer, record->as.checkpoint.source_first_record_id);
        }
        if (st == OI_OK) {
            st = write_key(writer, "source_last_record_id", 21);
        }
        if (st == OI_OK) {
            st = write_uint64_string(
                writer, record->as.checkpoint.source_last_record_id);
        }
        break;
    case OI_CLI_HISTORY_RECORD_NONE:
        st = OI_ERR_INVAL;
        break;
    }
    return st;
}

oi_status oi_cli_history_record_encode(
    const struct oi_cli_history_record *record, char **out_json,
    size_t *out_json_len) {
    if (!oi_cli_history_record_is_valid(record) || out_json == NULL ||
        out_json_len == NULL) {
        return OI_ERR_INVAL;
    }
    *out_json = NULL;
    *out_json_len = 0;

    size_t kind_len;
    const char *kind = record_kind_name(record->kind, &kind_len);
    if (kind == NULL) {
        return OI_ERR_INVAL;
    }
    oi_json_writer *writer = oi_json_writer_create();
    if (writer == NULL) {
        return OI_ERR_NOMEM;
    }

    oi_status st = oi_json_write_object_begin(writer);
    if (st == OI_OK) {
        st = write_key(writer, "version", 7);
    }
    if (st == OI_OK) {
        st = oi_json_write_number(writer, record->version);
    }
    if (st == OI_OK) {
        st = write_key(writer, "record_id", 9);
    }
    if (st == OI_OK) {
        st = write_uint64_string(writer, record->record_id);
    }
    if (st == OI_OK) {
        st = write_key(writer, "turn_id", 7);
    }
    if (st == OI_OK) {
        st = write_uint64_string(writer, record->turn_id);
    }
    if (st == OI_OK) {
        st = write_key(writer, "type", 4);
    }
    if (st == OI_OK) {
        st = oi_json_write_string(writer, kind, kind_len);
    }
    if (st == OI_OK) {
        st = write_payload(writer, record);
    }
    if (st == OI_OK) {
        st = oi_json_write_object_end(writer);
    }

    size_t json_len = 0;
    const char *json = oi_json_writer_data(writer, &json_len);
    if (st == OI_OK && json_len > OI_CLI_HISTORY_MAX_RECORD) {
        st = OI_ERR_NOMEM;
    }
    char *copy = NULL;
    if (st == OI_OK) {
        if (json_len == SIZE_MAX) {
            st = OI_ERR_NOMEM;
        } else {
            copy = malloc(json_len + 1);
            if (copy == NULL) {
                st = OI_ERR_NOMEM;
            }
        }
    }
    if (st == OI_OK) {
        memcpy(copy, json, json_len + 1);
        *out_json = copy;
        *out_json_len = json_len;
    }
    oi_json_writer_destroy(writer);
    return st;
}
