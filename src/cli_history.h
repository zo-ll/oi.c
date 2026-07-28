#ifndef OI_CLI_HISTORY_H
#define OI_CLI_HISTORY_H

#include <stddef.h>
#include <stdint.h>

#include "cli_message.h"
#include "oi/status.h"

#define OI_CLI_HISTORY_SCHEMA_VERSION 1u
#define OI_CLI_HISTORY_MAX_CONTENT (1024u * 1024u)
#define OI_CLI_HISTORY_MAX_RECORD (8u * 1024u * 1024u)

enum oi_cli_history_record_kind {
    OI_CLI_HISTORY_RECORD_NONE = 0,
    OI_CLI_HISTORY_RECORD_TRANSITION,
    OI_CLI_HISTORY_RECORD_MESSAGE,
    OI_CLI_HISTORY_RECORD_TOOL_STARTED,
    OI_CLI_HISTORY_RECORD_PARTIAL_ASSISTANT,
    OI_CLI_HISTORY_RECORD_QUEUED_INPUT,
    OI_CLI_HISTORY_RECORD_QUEUE_RESOLVED,
    OI_CLI_HISTORY_RECORD_CHECKPOINT
};

enum oi_cli_history_message_source {
    OI_CLI_HISTORY_MESSAGE_NORMAL = 0,
    OI_CLI_HISTORY_MESSAGE_REPAIR
};

enum oi_cli_history_tool_outcome {
    OI_CLI_HISTORY_TOOL_OUTCOME_NONE = 0,
    OI_CLI_HISTORY_TOOL_COMPLETED,
    OI_CLI_HISTORY_TOOL_OUTCOME_UNKNOWN,
    OI_CLI_HISTORY_TOOL_NOT_EXECUTED
};

enum oi_cli_history_queue_resolution {
    OI_CLI_HISTORY_QUEUE_CONSUMED = 1,
    OI_CLI_HISTORY_QUEUE_DISCARDED
};

struct oi_cli_bytes {
    unsigned char *data;
    size_t len;
};

struct oi_cli_history_message {
    struct oi_cli_message value;
    enum oi_cli_history_message_source source;
    enum oi_cli_history_tool_outcome tool_outcome;
    struct oi_cli_string model;
    struct oi_cli_bytes raw_tool_output;
    int has_raw_tool_output;
};

struct oi_cli_history_record {
    unsigned int version;
    uint64_t record_id;
    uint64_t turn_id;
    enum oi_cli_history_record_kind kind;
    union {
        struct {
            uint64_t legacy_record_count;
        } transition;
        struct oi_cli_history_message message;
        struct {
            struct oi_cli_string tool_call_id;
        } tool_started;
        struct {
            struct oi_cli_string content;
            struct oi_cli_string model;
        } partial_assistant;
        struct {
            struct oi_cli_string content;
        } queued_input;
        struct {
            uint64_t queued_record_id;
            enum oi_cli_history_queue_resolution resolution;
        } queue_resolved;
        struct {
            struct oi_cli_string summary;
            struct oi_cli_string model;
            uint64_t source_first_record_id;
            uint64_t source_last_record_id;
        } checkpoint;
    } as;
};

struct oi_cli_history {
    struct oi_cli_history_record *records;
    size_t len;
    size_t cap;
};

/* Records must be initialized before being passed to a set function. */
void oi_cli_history_record_init(struct oi_cli_history_record *record);
void oi_cli_history_record_free(struct oi_cli_history_record *record);
int oi_cli_history_record_is_valid(
    const struct oi_cli_history_record *record);
oi_status oi_cli_history_record_clone(
    const struct oi_cli_history_record *source,
    struct oi_cli_history_record *destination);

oi_status oi_cli_history_record_set_transition(
    struct oi_cli_history_record *record, uint64_t record_id,
    uint64_t legacy_record_count);
oi_status oi_cli_history_record_set_message(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    const struct oi_cli_message *message,
    enum oi_cli_history_message_source source, const char *model,
    size_t model_len, enum oi_cli_history_tool_outcome tool_outcome,
    const void *raw_tool_output, size_t raw_tool_output_len,
    int has_raw_tool_output);
oi_status oi_cli_history_record_set_tool_started(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    const char *tool_call_id, size_t tool_call_id_len);
oi_status oi_cli_history_record_set_partial_assistant(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    const char *content, size_t content_len, const char *model,
    size_t model_len);
oi_status oi_cli_history_record_set_queued_input(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    const char *content, size_t content_len);
oi_status oi_cli_history_record_set_queue_resolved(
    struct oi_cli_history_record *record, uint64_t record_id, uint64_t turn_id,
    uint64_t queued_record_id,
    enum oi_cli_history_queue_resolution resolution);
oi_status oi_cli_history_record_set_checkpoint(
    struct oi_cli_history_record *record, uint64_t record_id,
    const char *summary, size_t summary_len, const char *model,
    size_t model_len, uint64_t source_first_record_id,
    uint64_t source_last_record_id);

void oi_cli_history_init(struct oi_cli_history *history);
void oi_cli_history_free(struct oi_cli_history *history);
oi_status oi_cli_history_reserve(struct oi_cli_history *history,
                                 size_t capacity);
oi_status oi_cli_history_append_take(struct oi_cli_history *history,
                                     struct oi_cli_history_record *record);
oi_status oi_cli_history_append_clone(
    struct oi_cli_history *history,
    const struct oi_cli_history_record *record);

#endif
