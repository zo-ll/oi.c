#ifndef OI_CLI_TERMINAL_H
#define OI_CLI_TERMINAL_H

#include <termios.h>

#include "oi/status.h"

struct oi_cli_terminal {
    int input_fd;
    int output_fd;
    int active;
    struct termios original;
};

void oi_cli_terminal_init(struct oi_cli_terminal *terminal);

/* Both descriptors must refer to terminals. Enables raw input and paste mode. */
oi_status oi_cli_terminal_enable(struct oi_cli_terminal *terminal,
                                 int input_fd, int output_fd);

/*
 * Toggles signal-generating control characters while leaving the rest of
 * raw mode intact. Useful around a synchronous operation that watches a
 * blocked SIGINT through signalfd but must preserve ordinary type-ahead.
 */
oi_status oi_cli_terminal_set_isig(struct oi_cli_terminal *terminal,
                                   int enabled);

/* Safe to call repeatedly. The first call restores the captured terminal. */
oi_status oi_cli_terminal_restore(struct oi_cli_terminal *terminal);

#endif
