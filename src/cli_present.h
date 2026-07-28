#ifndef OI_CLI_PRESENT_H
#define OI_CLI_PRESENT_H

#include <stddef.h>
#include <stdio.h>

#include "cli_conversation.h"
#include "oi/status.h"

struct oi_cli_present {
    FILE *out;
    FILE *err;
    char *assistant;
    size_t assistant_len;
    size_t assistant_cap;
    int capture_assistant;
    int done;
    oi_status status;
    oi_cli_conversation_event_cb external_event;
    void *external_user_data;
};

oi_status oi_cli_present_init(
    struct oi_cli_present *present, FILE *out, FILE *err,
    int capture_assistant, oi_cli_conversation_event_cb external_event,
    void *external_user_data);
void oi_cli_present_free(struct oi_cli_present *present);
void oi_cli_present_reset_turn(struct oi_cli_present *present);

oi_status oi_cli_present_event(
    const struct oi_cli_conversation_event *event, void *user_data);

/* Moves the captured assistant text to the caller. */
oi_status oi_cli_present_take_assistant(struct oi_cli_present *present,
                                        char **out_text, size_t *out_len);

#endif
