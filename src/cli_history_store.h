#ifndef OI_CLI_HISTORY_STORE_H
#define OI_CLI_HISTORY_STORE_H

#include "cli_history.h"
#include "cli_history_replay.h"
#include "cli_message.h"
#include "oi/sesslog.h"
#include "oi/status.h"

struct oi_cli_history_store {
    oi_sesslog *log; /* borrowed */
    struct oi_cli_message_list legacy_messages;
    struct oi_cli_history typed_history;
};

void oi_cli_history_store_init(struct oi_cli_history_store *store);
void oi_cli_history_store_free(struct oi_cli_history_store *store);

/*
 * Loads, decodes, and strictly replays a session log. The store borrows `log`;
 * neither free nor a failed load closes it. Both outputs must be initialized.
 */
oi_status oi_cli_history_store_load(
    oi_sesslog *log, struct oi_cli_history_store *out_store,
    struct oi_cli_history_replay_state *out_state);

/*
 * Validates and durably appends one typed record, then publishes the rebuilt
 * replay state. The store remains unchanged if the append itself fails.
 */
oi_status oi_cli_history_store_append(
    struct oi_cli_history_store *store,
    const struct oi_cli_history_record *record,
    struct oi_cli_history_replay_state *out_state);

#endif
