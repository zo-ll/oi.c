#ifndef OI_CLI_RENDER_SANITIZE_H
#define OI_CLI_RENDER_SANITIZE_H

#include <stddef.h>

#include "cli_bytebuf.h"
#include "oi/status.h"

enum oi_cli_sanitize_phase {
    OI_CLI_SANITIZE_NORMAL = 0,
    OI_CLI_SANITIZE_SAW_C2,
    OI_CLI_SANITIZE_ESCAPE,
    OI_CLI_SANITIZE_CSI,
    OI_CLI_SANITIZE_STRING,
    OI_CLI_SANITIZE_STRING_ESC
};

#define OI_CLI_SANITIZE_MAX_SEQUENCE 4096u

struct oi_cli_sanitize_state {
    enum oi_cli_sanitize_phase phase;
    size_t sequence_bytes;
};

void oi_cli_sanitize_init(struct oi_cli_sanitize_state *state);

oi_status oi_cli_sanitize_feed(struct oi_cli_sanitize_state *state,
                               const unsigned char *in, size_t in_len,
                               struct oi_cli_bytebuf *out);

oi_status oi_cli_sanitize_finish(struct oi_cli_sanitize_state *state);

#endif
