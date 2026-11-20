
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
