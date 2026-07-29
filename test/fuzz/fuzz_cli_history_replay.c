/*
 * libFuzzer harness for cli_history_replay.c's decode+replay path.
 *
 * Every other harness in this directory stops one layer below this:
 * fuzz_sesslog.c only exercises raw record framing/truncation-recovery
 * (oi/sesslog.h), never the typed JSON decode + context-reconstruction
 * semantics (queue resolution, transitions, repairs, and now -- issue
 * #27 -- checkpoints) that live in cli_history_replay.c. Since /compact
 * is this issue's first live producer of a checkpoint record, its
 * replay-application invariants deserve the same fuzz coverage as every
 * other record type gets here.
 *
 * Input is split on '\n'; each line is decoded independently via
 * oi_cli_history_record_decode (a line that fails to decode is simply
 * skipped, matching how a real log with one unrelated corruption further
 * along would still want everything before it validated), and every
 * successfully-decoded record is collected into one oi_cli_history and
 * replayed. oi_cli_history_append_take itself rejects a non-sequential
 * record_id, so most fuzzed inputs only ever build a short or
 * single-record history -- that's fine; the property under test is
 * purely "no crash/UB/leak" for whatever sequence does validate,
 * regardless of how adversarial it is (overlapping or out-of-order
 * checkpoint ranges, a checkpoint referencing a record id that was never
 * itself part of the context, etc.), which is exactly the class of
 * input oi_cli_history_replay's own invariant checks exist to reject
 * cleanly rather than misbehave on.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cli_history.h"
#include "cli_history_codec.h"
#include "cli_history_replay.h"

#define FUZZ_MAX_INPUT (64u * 1024)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > FUZZ_MAX_INPUT) {
        return 0;
    }

    struct oi_cli_history history;
    oi_cli_history_init(&history);

    size_t start = 0;
    for (size_t i = 0; i <= size; i++) {
        if (i < size && data[i] != '\n') {
            continue;
        }
        {
            size_t len = i - start;
            struct oi_cli_history_record record;

            oi_cli_history_record_init(&record);
            if (oi_cli_history_record_decode(
                    (const char *)data + start, len, &record) == OI_OK) {
                if (oi_cli_history_append_take(&history, &record) != OI_OK) {
                    oi_cli_history_record_free(&record);
                }
            } else {
                oi_cli_history_record_free(&record);
            }
        }
        start = i + 1;
    }

    struct oi_cli_history_replay_state state;
    oi_cli_history_replay_state_init(&state);
    oi_cli_history_replay(NULL, &history, &state);
    oi_cli_history_replay_state_free(&state);

    oi_cli_history_free(&history);
    return 0;
}
