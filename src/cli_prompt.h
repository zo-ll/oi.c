#ifndef OI_CLI_PROMPT_H
#define OI_CLI_PROMPT_H

#include <stddef.h>

#include "cli_input_history.h"
#include "oi/status.h"

/*
 * Reads one interactive submission. The returned text is caller-owned.
 * `out_exit` is set when Ctrl+D was pressed at an empty prompt.
 */
oi_status oi_cli_prompt_read(int input_fd, int output_fd,
                             struct oi_cli_input_history *history,
                             char **out_text, size_t *out_len, int *out_exit);

#endif
