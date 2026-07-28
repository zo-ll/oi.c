#include "cli_history.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int bytes_are_valid(const void *data, size_t len) {
    return data != NULL || len == 0;
}

static oi_status bytes_set(struct oi_cli_bytes *destination, const void *data,
                           size_t len) {
    if (destination == NULL || !bytes_are_valid(data, len)) {
        return OI_ERR_INVAL;
    }
    unsigned char *copy = malloc(len > 0 ? len : 1);
    if (copy == NULL) {
        return OI_ERR_NOMEM;
    }
    if (len > 0) {
        memcpy(copy, data, len);
    }
    free(destination->data);
    destination->data = copy;
    destination->len = len;
    return OI_OK;
}

static void bytes_free(struct oi_cli_bytes *bytes) {
    free(bytes->data);
    memset(bytes, 0, sizeof *bytes);
}

static void record_replace(struct oi_cli_history_record *destination,
                           struct oi_cli_history_record *replacement) {
    oi_cli_history_record_free(destination);
    *destination = *replacement;
    oi_cli_history_record_init(replacement);
}

static oi_status record_begin(struct oi_cli_history_record *record,
                              uint64_t record_id, uint64_t turn_id,
                              enum oi_cli_history_record_kind kind) {
    if (record == NULL || record_id == 0) {
        return OI_ERR_INVAL;
    }
    oi_cli_history_record_init(record);
    record->version = OI_CLI_HISTORY_SCHEMA_VERSION;
    record->record_id = record_id;
    record->turn_id = turn_id;
    record->kind = kind;
    return OI_OK;
}

void oi_cli_history_record_init(struct oi_cli_history_record *record) {
    if (record != NULL) {
        memset(record, 0, sizeof *record);
    }
}

void oi_cli_history_record_free(struct oi_cli_history_record *record) {
    if (record == NULL) {
        return;
    }
    switch (record->kind) {
    case OI_CLI_HISTORY_RECORD_MESSAGE:
        oi_cli_message_free(&record->as.message.value);
        oi_cli_string_free(&record->as.message.model);
        bytes_free(&record->as.message.raw_tool_output);
        break;
    case OI_CLI_HISTORY_RECORD_TOOL_STARTED:
        oi_cli_string_free(&record->as.tool_started.tool_call_id);
        break;
    case OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT:
        oi_cli_string_free(&record->as.partial_assistant.content);
        oi_cli_string_free(&record->as.partial_assistant.model);
        break;
    case OI_CLI_HISTORY_RECORD_QUEUED_INPUT:
        oi_cli_string_free(&record->as.queued_input.content);
        break;
    case OI_CLI_HISTORY_RECORD_CHECKPOINT:
        oi_cli_string_free(&record->as.checkpoint.summary);
        oi_cli_string_free(&record->as.checkpoint.model);
        break;
    case OI_CLI_HISTORY_RECORD_NONE:
    case OI_CLI_HISTORY_RECORD_TRANSITION:
    case OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED:
        break;
    }
    memset(record, 0, sizeof *record);
}

static int optional_string_is_valid(const struct oi_cli_string *string) {
    return string->data != NULL || string->len == 0;
}

static int history_message_is_valid(
    const struct oi_cli_history_message *message) {
    if (!oi_cli_message_is_valid(&message->value) ||
        !optional_string_is_valid(&message->model) ||
        message->value.content.len > OI_CLI_HISTORY_MAX_CONTENT ||
        message->model.len > OI_CLI_HISTORY_MAX_CONTENT ||
        (message->source != OI_CLI_HISTORY_MESSAGE_NORMAL &&
         message->source != OI_CLI_HISTORY_MESSAGE_REPAIR)) {
        return 0;
    }
    for (size_t i = 0; i < message->value.tool_calls_len; i++) {
        if (message->value.tool_calls[i].arguments.len >
            OI_CLI_HISTORY_MAX_CONTENT) {
            return 0;
        }
    }
    if (message->value.role == OI_CLI_MESSAGE_USER) {
        return message->source == OI_CLI_HISTORY_MESSAGE_NORMAL &&
               message->model.data == NULL && message->model.len == 0 &&
               message->tool_outcome == OI_CLI_HISTORY_TOOL_OUTCOME_NONE &&
               !message->has_raw_tool_output &&
               message->raw_tool_output.data == NULL;
    }
    if (message->value.role == OI_CLI_MESSAGE_ASSISTANT) {
        int attribution_is_valid =
            (message->source == OI_CLI_HISTORY_MESSAGE_NORMAL &&
             message->model.data != NULL && message->model.len > 0) ||
            (message->source == OI_CLI_HISTORY_MESSAGE_REPAIR &&
             message->model.data == NULL && message->model.len == 0);
        return attribution_is_valid &&
               message->tool_outcome == OI_CLI_HISTORY_TOOL_OUTCOME_NONE &&
               !message->has_raw_tool_output &&
               message->raw_tool_output.data == NULL;
    }
    if (message->model.data != NULL || message->model.len != 0) {
        return 0;
    }
    if (message->tool_outcome != OI_CLI_HISTORY_TOOL_COMPLETED &&
        message->tool_outcome != OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN &&
        message->tool_outcome != OI_CLI_HISTORY_TOOL_NOT_EXECUTED) {
        return 0;
    }
    if (message->tool_outcome == OI_CLI_HISTORY_TOOL_COMPLETED) {
        return message->source == OI_CLI_HISTORY_MESSAGE_NORMAL &&
               message->has_raw_tool_output &&
               message->raw_tool_output.data != NULL &&
               message->raw_tool_output.len <= OI_CLI_HISTORY_MAX_CONTENT;
    }
    return message->source == OI_CLI_HISTORY_MESSAGE_REPAIR &&
           !message->has_raw_tool_output &&
           message->raw_tool_output.data == NULL;
}

int oi_cli_history_record_is_valid(
    const struct oi_cli_history_record *record) {
    if (record == NULL ||
        record->version != OI_CLI_HISTORY_SCHEMA_VERSION ||
        record->record_id == 0) {
        return 0;
    }
    switch (record->kind) {
    case OI_CLI_HISTORY_RECORD_TRANSITION:
        return record->turn_id == 0 &&
               record->as.transition.legacy_record_count < UINT64_MAX &&
               record->record_id ==
                   record->as.transition.legacy_record_count + 1;
    case OI_CLI_HISTORY_RECORD_MESSAGE:
        return record->turn_id > 0 &&
               history_message_is_valid(&record->as.message);
    case OI_CLI_HISTORY_RECORD_TOOL_STARTED:
        return record->turn_id > 0 &&
               record->as.tool_started.tool_call_id.data != NULL &&
               record->as.tool_started.tool_call_id.len > 0 &&
               record->as.tool_started.tool_call_id.len <=
                   OI_CLI_HISTORY_MAX_CONTENT;
    case OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT:
        return record->turn_id > 0 &&
               record->as.partial_assistant.content.data != NULL &&
               record->as.partial_assistant.content.len <=
                   OI_CLI_HISTORY_MAX_CONTENT &&
               record->as.partial_assistant.model.data != NULL &&
               record->as.partial_assistant.model.len > 0 &&
               record->as.partial_assistant.model.len <=
                   OI_CLI_HISTORY_MAX_CONTENT &&
               optional_string_is_valid(
                   &record->as.partial_assistant.model);
    case OI_CLI_HISTORY_RECORD_QUEUED_INPUT:
        return record->turn_id > 0 &&
               record->as.queued_input.content.data != NULL &&
               record->as.queued_input.content.len <=
                   OI_CLI_HISTORY_MAX_CONTENT;
    case OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED:
        return record->turn_id > 0 &&
               record->as.queue_resolved.queued_record_id > 0 &&
               record->as.queue_resolved.queued_record_id <
                   record->record_id &&
               (record->as.queue_resolved.resolution ==
                    OI_CLI_HISTORY_QUEUE_CONSUMED ||
                record->as.queue_resolved.resolution ==
                    OI_CLI_HISTORY_QUEUE_DISCARDED);
    case OI_CLI_HISTORY_RECORD_CHECKPOINT:
        return record->turn_id == 0 &&
               record->as.checkpoint.summary.data != NULL &&
               record->as.checkpoint.summary.len <=
                   OI_CLI_HISTORY_MAX_CONTENT &&
               record->as.checkpoint.model.data != NULL &&
               record->as.checkpoint.model.len > 0 &&
               record->as.checkpoint.model.len <=
                   OI_CLI_HISTORY_MAX_CONTENT &&
               optional_string_is_valid(&record->as.checkpoint.model) &&
               record->as.checkpoint.source_first_record_id > 0 &&
               record->as.checkpoint.source_first_record_id <=
                   record->as.checkpoint.source_last_record_id &&
               record->as.checkpoint.source_last_record_id < record->record_id;
    case OI_CLI_HISTORY_RECORD_NONE:
        return 0;
    }
    return 0;
}

oi_status oi_cli_history_record_set_transition(
    struct oi_cli_history_record *record, uint64_t record_id,
    uint64_t legacy_record_count) {
    if (record == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_record replacement;
    oi_status st = record_begin(&replacement, record_id, 0,
                                OI_CLI_HISTORY_RECORD_TRANSITION);
    if (st != OI_OK) {
        return st;
    }
    replacement.as.transition.legacy_record_count = legacy_record_count;
    record_replace(record, &replacement);
    return OI_OK;
}

oi_status oi_cli_history_record_set_message(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    const struct oi_cli_message *message,
    enum oi_cli_history_message_source source, const char *model,
    size_t model_len, enum oi_cli_history_tool_outcome tool_outcome,
    const void *raw_tool_output, size_t raw_tool_output_len,
    int has_raw_tool_output) {
    if (record == NULL || message == NULL || (!has_raw_tool_output &&
                            (raw_tool_output != NULL ||
                             raw_tool_output_len != 0)) ||
        (has_raw_tool_output &&
         !bytes_are_valid(raw_tool_output, raw_tool_output_len))) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_record replacement;
    oi_status st = record_begin(&replacement, record_id, turn_id,
                                OI_CLI_HISTORY_RECORD_MESSAGE);
    if (st != OI_OK) {
        return st;
    }
    replacement.as.message.source = source;
    replacement.as.message.tool_outcome = tool_outcome;
    replacement.as.message.has_raw_tool_output = has_raw_tool_output != 0;
    st = oi_cli_message_clone(message, &replacement.as.message.value);
    if (st == OI_OK && model != NULL) {
        st = oi_cli_string_set(&replacement.as.message.model, model,
                               model_len);
    } else if (st == OI_OK && model_len != 0) {
        st = OI_ERR_INVAL;
    }
    if (st == OI_OK && has_raw_tool_output) {
        st = bytes_set(&replacement.as.message.raw_tool_output,
                       raw_tool_output, raw_tool_output_len);
    }
    if (st == OI_OK && !oi_cli_history_record_is_valid(&replacement)) {
        st = OI_ERR_INVAL;
    }
    if (st != OI_OK) {
        oi_cli_history_record_free(&replacement);
        return st;
    }
    record_replace(record, &replacement);
    return OI_OK;
}

oi_status oi_cli_history_record_set_tool_started(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    const char *tool_call_id, size_t tool_call_id_len) {
    if (record == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_record replacement;
    oi_status st = record_begin(&replacement, record_id, turn_id,
                                OI_CLI_HISTORY_RECORD_TOOL_STARTED);
    if (st == OI_OK) {
        st = oi_cli_string_set(&replacement.as.tool_started.tool_call_id,
                               tool_call_id, tool_call_id_len);
    }
    if (st == OI_OK && !oi_cli_history_record_is_valid(&replacement)) {
        st = OI_ERR_INVAL;
    }
    if (st != OI_OK) {
        oi_cli_history_record_free(&replacement);
        return st;
    }
    record_replace(record, &replacement);
    return OI_OK;
}

oi_status oi_cli_history_record_set_partial_assistant(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    const char *content, size_t content_len, const char *model,
    size_t model_len) {
    if (record == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_record replacement;
    oi_status st = record_begin(&replacement, record_id, turn_id,
                                OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT);
    if (st == OI_OK) {
        st = oi_cli_string_set(&replacement.as.partial_assistant.content,
                               content, content_len);
    }
    if (st == OI_OK && model != NULL) {
        st = oi_cli_string_set(&replacement.as.partial_assistant.model, model,
                               model_len);
    } else if (st == OI_OK && model_len != 0) {
        st = OI_ERR_INVAL;
    }
    if (st == OI_OK && !oi_cli_history_record_is_valid(&replacement)) {
        st = OI_ERR_INVAL;
    }
    if (st != OI_OK) {
        oi_cli_history_record_free(&replacement);
        return st;
    }
    record_replace(record, &replacement);
    return OI_OK;
}

oi_status oi_cli_history_record_set_queued_input(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    const char *content, size_t content_len) {
    if (record == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_record replacement;
    oi_status st = record_begin(&replacement, record_id, turn_id,
                                OI_CLI_HISTORY_RECORD_QUEUED_INPUT);
    if (st == OI_OK) {
        st = oi_cli_string_set(&replacement.as.queued_input.content, content,
                               content_len);
    }
    if (st == OI_OK && !oi_cli_history_record_is_valid(&replacement)) {
        st = OI_ERR_INVAL;
    }
    if (st != OI_OK) {
        oi_cli_history_record_free(&replacement);
        return st;
    }
    record_replace(record, &replacement);
    return OI_OK;
}

oi_status oi_cli_history_record_set_queue_resolved(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    uint64_t queued_record_id,
    enum oi_cli_history_queue_resolution resolution) {
    if (record == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_record replacement;
    oi_status st = record_begin(&replacement, record_id, turn_id,
                                OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED);
    if (st != OI_OK) {
        return st;
    }
    replacement.as.queue_resolved.queued_record_id = queued_record_id;
    replacement.as.queue_resolved.resolution = resolution;
    if (!oi_cli_history_record_is_valid(&replacement)) {
        return OI_ERR_INVAL;
    }
    record_replace(record, &replacement);
    return OI_OK;
}

oi_status oi_cli_history_record_set_checkpoint(
    struct oi_cli_history_record *record, uint64_t record_id,
    const char *summary, size_t summary_len, const char *model,
    size_t model_len, uint64_t source_first_record_id,
    uint64_t source_last_record_id) {
    if (record == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_record replacement;
    oi_status st = record_begin(&replacement, record_id, 0,
                                OI_CLI_HISTORY_RECORD_CHECKPOINT);
    if (st == OI_OK) {
        st = oi_cli_string_set(&replacement.as.checkpoint.summary, summary,
                               summary_len);
    }
    if (st == OI_OK && model != NULL) {
        st = oi_cli_string_set(&replacement.as.checkpoint.model, model,
                               model_len);
    } else if (st == OI_OK && model_len != 0) {
        st = OI_ERR_INVAL;
    }
    replacement.as.checkpoint.source_first_record_id =
        source_first_record_id;
    replacement.as.checkpoint.source_last_record_id = source_last_record_id;
    if (st == OI_OK && !oi_cli_history_record_is_valid(&replacement)) {
        st = OI_ERR_INVAL;
    }
    if (st != OI_OK) {
        oi_cli_history_record_free(&replacement);
        return st;
    }
    record_replace(record, &replacement);
    return OI_OK;
}

oi_status oi_cli_history_record_clone(
    const struct oi_cli_history_record *source,
    struct oi_cli_history_record *destination) {
    if (!oi_cli_history_record_is_valid(source) || destination == NULL ||
        source == destination) {
        return OI_ERR_INVAL;
    }
    oi_status st;
    switch (source->kind) {
    case OI_CLI_HISTORY_RECORD_TRANSITION:
        st = oi_cli_history_record_set_transition(
            destination, source->record_id,
            source->as.transition.legacy_record_count);
        break;
    case OI_CLI_HISTORY_RECORD_MESSAGE: {
        const struct oi_cli_history_message *message = &source->as.message;
        st = oi_cli_history_record_set_message(
            destination, source->record_id, source->turn_id, &message->value,
            message->source, message->model.data, message->model.len,
            message->tool_outcome, message->raw_tool_output.data,
            message->raw_tool_output.len, message->has_raw_tool_output);
        break;
    }
    case OI_CLI_HISTORY_RECORD_TOOL_STARTED:
        st = oi_cli_history_record_set_tool_started(
            destination, source->record_id, source->turn_id,
            source->as.tool_started.tool_call_id.data,
            source->as.tool_started.tool_call_id.len);
        break;
    case OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT:
        st = oi_cli_history_record_set_partial_assistant(
            destination, source->record_id, source->turn_id,
            source->as.partial_assistant.content.data,
            source->as.partial_assistant.content.len,
            source->as.partial_assistant.model.data,
            source->as.partial_assistant.model.len);
        break;
    case OI_CLI_HISTORY_RECORD_QUEUED_INPUT:
        st = oi_cli_history_record_set_queued_input(
            destination, source->record_id, source->turn_id,
            source->as.queued_input.content.data,
            source->as.queued_input.content.len);
        break;
    case OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED:
        st = oi_cli_history_record_set_queue_resolved(
            destination, source->record_id, source->turn_id,
            source->as.queue_resolved.queued_record_id,
            source->as.queue_resolved.resolution);
        break;
    case OI_CLI_HISTORY_RECORD_CHECKPOINT:
        st = oi_cli_history_record_set_checkpoint(
            destination, source->record_id,
            source->as.checkpoint.summary.data,
            source->as.checkpoint.summary.len,
            source->as.checkpoint.model.data,
            source->as.checkpoint.model.len,
            source->as.checkpoint.source_first_record_id,
            source->as.checkpoint.source_last_record_id);
        break;
    case OI_CLI_HISTORY_RECORD_NONE:
        st = OI_ERR_INVAL;
        break;
    default:
        st = OI_ERR_INVAL;
        break;
    }
    return st;
}

void oi_cli_history_init(struct oi_cli_history *history) {
    if (history != NULL) {
        memset(history, 0, sizeof *history);
    }
}

void oi_cli_history_free(struct oi_cli_history *history) {
    if (history == NULL) {
        return;
    }
    for (size_t i = 0; i < history->len; i++) {
        oi_cli_history_record_free(&history->records[i]);
    }
    free(history->records);
    memset(history, 0, sizeof *history);
}

oi_status oi_cli_history_append_take(struct oi_cli_history *history,
                                     struct oi_cli_history_record *record) {
    if (history == NULL || !oi_cli_history_record_is_valid(record)) {
        return OI_ERR_INVAL;
    }
    if (history->len > 0) {
        uint64_t previous =
            history->records[history->len - 1].record_id;
        if (previous == UINT64_MAX || record->record_id != previous + 1) {
            return OI_ERR_INVAL;
        }
    }
    if (history->len == SIZE_MAX / sizeof *history->records) {
        return OI_ERR_NOMEM;
    }
    if (history->len == history->cap) {
        size_t cap = history->cap == 0 ? 16 : history->cap;
        if (history->cap != 0) {
            if (cap > SIZE_MAX / 2) {
                return OI_ERR_NOMEM;
            }
            cap *= 2;
        }
        if (cap > SIZE_MAX / sizeof *history->records) {
            cap = SIZE_MAX / sizeof *history->records;
        }
        struct oi_cli_history_record *records =
            realloc(history->records, cap * sizeof *records);
        if (records == NULL) {
            return OI_ERR_NOMEM;
        }
        history->records = records;
        history->cap = cap;
    }
    history->records[history->len++] = *record;
    oi_cli_history_record_init(record);
    return OI_OK;
}

oi_status oi_cli_history_append_clone(
    struct oi_cli_history *history,
    const struct oi_cli_history_record *record) {
    if (history == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_cli_history_record clone;
    oi_cli_history_record_init(&clone);
    oi_status st = oi_cli_history_record_clone(record, &clone);
    if (st == OI_OK) {
        st = oi_cli_history_append_take(history, &clone);
    }
    oi_cli_history_record_free(&clone);
    return st;
}
