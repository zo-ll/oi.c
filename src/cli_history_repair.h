#ifndef OI_CLI_HISTORY_REPAIR_H
#define OI_CLI_HISTORY_REPAIR_H

#include "cli_history.h"
#include "cli_history_replay.h"
#include "oi/status.h"

/*
 * Builds the append-only records needed to complete an interrupted turn.
 * `out_repairs` must be initialized and is replaced only on success.
 */
oi_status oi_cli_history_build_repairs(
    const struct oi_cli_history_replay_state *state,
    struct oi_cli_history *out_repairs);

#endif
