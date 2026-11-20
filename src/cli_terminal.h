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

oi_status oi_cli_terminal_enable(struct oi_cli_terminal *terminal,
                                 int input_fd, int output_fd);

oi_status oi_cli_terminal_set_isig(struct oi_cli_terminal *terminal,
                                   int enabled);

oi_status oi_cli_terminal_restore(struct oi_cli_terminal *terminal);

#endif
