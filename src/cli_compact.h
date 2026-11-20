#ifndef OI_CLI_COMPACT_H
#define OI_CLI_COMPACT_H

#include <stddef.h>

#include "cli_message.h"
#include "oi/json.h"
#include "oi/llm.h"
#include "oi/reactor.h"
#include "oi/status.h"

oi_status oi_cli_compact_parse_turns(const char *arguments, size_t len,
                                     size_t *out_turns, int *out_has_value);

oi_status oi_cli_compact_select_prefix(
    const struct oi_cli_message_list *messages, size_t keep_turns,
    size_t *out_prefix_count, size_t *out_total_turns);

oi_status oi_cli_compact_build_request(
    const struct oi_cli_message_list *messages, size_t prefix_count,
    const char *model, size_t model_len, oi_json_writer **out_writer);

enum oi_cli_compact_outcome {
    OI_CLI_COMPACT_OK,
    OI_CLI_COMPACT_CANCELLED,
    OI_CLI_COMPACT_FAILED
};

struct oi_cli_compact_result {
    enum oi_cli_compact_outcome outcome;
    oi_status status;
    int http_status;
    int terminate_signal;
    char *summary;
    size_t summary_len;
};

oi_status oi_cli_compact_run(oi_llm_client *client, oi_reactor *reactor,
                             oi_arena *arena, int signal_fd, const char *body,
                             size_t body_len,
                             struct oi_cli_compact_result *out_result);

void oi_cli_compact_result_free(struct oi_cli_compact_result *result);

#endif
