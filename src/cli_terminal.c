#include "cli_terminal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static const char enable_bracketed_paste[] = "\x1b[?2004h";
static const char disable_bracketed_paste[] = "\x1b[?2004l";

static oi_status write_all(int fd, const char *data, size_t len) {
    size_t written = 0;

    while (written < len) {
        ssize_t result = write(fd, data + written, len - written);
        if (result > 0) {
            written += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return OI_ERR_IO;
    }
    return OI_OK;
}

void oi_cli_terminal_init(struct oi_cli_terminal *terminal) {
    if (terminal == NULL) {
        return;
    }
    memset(terminal, 0, sizeof *terminal);
    terminal->input_fd = -1;
    terminal->output_fd = -1;
}

oi_status oi_cli_terminal_enable(struct oi_cli_terminal *terminal,
                                 int input_fd, int output_fd) {
    struct termios raw;
    oi_status status;

    if (terminal == NULL || terminal->active || input_fd < 0 ||
        output_fd < 0) {
        return OI_ERR_INVAL;
    }
    if (!isatty(input_fd) || !isatty(output_fd)) {
        return OI_ERR_INVAL;
    }
    if (tcgetattr(input_fd, &terminal->original) != 0) {
        return OI_ERR_IO;
    }

    raw = terminal->original;
    raw.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t) ~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(input_fd, TCSAFLUSH, &raw) != 0) {
        return OI_ERR_IO;
    }
    terminal->input_fd = input_fd;
    terminal->output_fd = output_fd;
    terminal->active = 1;

    status = write_all(output_fd, enable_bracketed_paste,
                       sizeof enable_bracketed_paste - 1);
    if (status != OI_OK) {
        (void)tcsetattr(input_fd, TCSAFLUSH, &terminal->original);
        terminal->active = 0;
        terminal->input_fd = -1;
        terminal->output_fd = -1;
        return status;
    }
    return OI_OK;
}

oi_status oi_cli_terminal_set_isig(struct oi_cli_terminal *terminal,
                                   int enabled) {
    struct termios attributes;

    if (terminal == NULL || !terminal->active) {
        return OI_ERR_INVAL;
    }
    if (tcgetattr(terminal->input_fd, &attributes) != 0) {
        return OI_ERR_IO;
    }
    if (enabled) {
        attributes.c_lflag |= ISIG;
    } else {
        attributes.c_lflag &= (tcflag_t)~ISIG;
    }
    return tcsetattr(terminal->input_fd, TCSANOW, &attributes) == 0
               ? OI_OK
               : OI_ERR_IO;
}

oi_status oi_cli_terminal_restore(struct oi_cli_terminal *terminal) {
    oi_status status = OI_OK;

    if (terminal == NULL) {
        return OI_ERR_INVAL;
    }
    if (!terminal->active) {
        return OI_OK;
    }

    if (write_all(terminal->output_fd, disable_bracketed_paste,
                  sizeof disable_bracketed_paste - 1) != OI_OK) {
        status = OI_ERR_IO;
    }
    if (tcsetattr(terminal->input_fd, TCSAFLUSH, &terminal->original) != 0) {
        status = OI_ERR_IO;
    }
    terminal->active = 0;
    terminal->input_fd = -1;
    terminal->output_fd = -1;
    return status;
}
