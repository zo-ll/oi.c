#ifndef OI_CLI_PROMPT_H
#define OI_CLI_PROMPT_H

#include <stddef.h>

#include "cli_input_history.h"
#include "oi/status.h"

/*
 * Reads one interactive submission. The returned text is caller-owned.
 * `out_exit` is set when Ctrl+D was pressed at an empty prompt.
 *
 * `signal_fd` is an optional signalfd already registered for SIGWINCH,
 * SIGINT, SIGTERM, and SIGHUP (SFD_NONBLOCK), owned and blocked/created by
 * the caller; pass -1 to disable signal handling entirely (the terminal
 * width is still read once at the start of the call either way). When
 * >= 0: a pending SIGWINCH re-reads the terminal width and redraws the
 * current frame in place, preserving the edit buffer, cursor, and
 * command-menu selection; a pending SIGTERM/SIGHUP (or, defensively,
 * SIGINT -- it cannot ordinarily fire here since raw mode clears ISIG, but
 * a pending one could in principle already be queued from before raw mode
 * was entered) ends the read cleanly and sets `*out_terminate_signal` to
 * the signal number, distinct from `*out_exit` (Ctrl+D). The terminal is
 * restored before returning either way, as it already is on any exit path.
 */
oi_status oi_cli_prompt_read(int input_fd, int output_fd, int signal_fd,
                             struct oi_cli_input_history *history,
                             char **out_text, size_t *out_len, int *out_exit,
                             int *out_terminate_signal);

#endif
