#ifndef OI_CLI_RENDER_SANITIZE_H
#define OI_CLI_RENDER_SANITIZE_H

#include <stddef.h>

#include "cli_bytebuf.h"
#include "oi/status.h"

/*
 * Default-deny terminal control-byte/escape-sequence stripper. Assumes
 * input is already valid UTF-8 (run oi_cli_utf8_stream first). Strips C0
 * controls other than '\n'/'\t' (including bare '\r' and NUL), the C1
 * control range encoded as UTF-8 (0xC2 0x80-0xC2 0x9F), and every
 * ESC-introduced sequence (CSI, OSC/DCS/SOS/PM/APC control strings, and
 * simple two-byte escapes) unconditionally -- never an allowlist of "safe"
 * subtypes. Everything else passes through unchanged.
 */
enum oi_cli_sanitize_phase {
    OI_CLI_SANITIZE_NORMAL = 0,
    OI_CLI_SANITIZE_SAW_C2,
    OI_CLI_SANITIZE_ESCAPE,
    OI_CLI_SANITIZE_CSI,
    OI_CLI_SANITIZE_STRING,
    OI_CLI_SANITIZE_STRING_ESC
};

/*
 * Bound on how many bytes an in-flight escape/control-string sequence may
 * swallow before it is abandoned and scanning resumes in NORMAL. Guards
 * against a malformed or never-terminated sequence forcing unbounded
 * suppression of subsequent legitimate text.
 */
#define OI_CLI_SANITIZE_MAX_SEQUENCE 4096u

struct oi_cli_sanitize_state {
    enum oi_cli_sanitize_phase phase;
    size_t sequence_bytes;
};

void oi_cli_sanitize_init(struct oi_cli_sanitize_state *state);

oi_status oi_cli_sanitize_feed(struct oi_cli_sanitize_state *state,
                               const unsigned char *in, size_t in_len,
                               struct oi_cli_bytebuf *out);

/*
 * End of input: any in-flight, never-forwarded sequence (including a held
 * 0xC2 lead byte awaiting its C1 lookahead) is simply abandoned; nothing
 * is appended. Safe to call more than once.
 */
oi_status oi_cli_sanitize_finish(struct oi_cli_sanitize_state *state);

#endif
