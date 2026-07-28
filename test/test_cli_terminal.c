#include "cli_terminal.h"
#include "test.h"

#include <fcntl.h>
#include <pty.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static int read_exact(int fd, char *data, size_t len) {
    size_t received = 0;

    while (received < len) {
        ssize_t result = read(fd, data + received, len - received);
        if (result <= 0) {
            return 0;
        }
        received += (size_t)result;
    }
    return 1;
}

TEST(enable_sets_raw_mode_and_restore_reinstates_it) {
    static const char paste_on[] = "\x1b[?2004h";
    static const char paste_off[] = "\x1b[?2004l";
    struct oi_cli_terminal terminal;
    struct termios original;
    struct termios raw;
    struct termios restored;
    char output[sizeof paste_on - 1];
    int master_fd = -1;
    int slave_fd = -1;

    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    if (master_fd < 0 || slave_fd < 0) {
        return;
    }
    CHECK_EQ(tcgetattr(slave_fd, &original), 0);
    oi_cli_terminal_init(&terminal);

    CHECK_EQ(oi_cli_terminal_enable(&terminal, slave_fd, slave_fd), OI_OK);
    CHECK(terminal.active);
    CHECK(read_exact(master_fd, output, sizeof output));
    CHECK(memcmp(output, paste_on, sizeof output) == 0);
    CHECK_EQ(tcgetattr(slave_fd, &raw), 0);
    CHECK_EQ(raw.c_lflag & (ECHO | ICANON | IEXTEN | ISIG), 0);
    CHECK_EQ(raw.c_iflag & (BRKINT | ICRNL | INPCK | ISTRIP | IXON), 0);
    CHECK_EQ(raw.c_oflag & OPOST, 0);
    CHECK_EQ(raw.c_cc[VMIN], 1);
    CHECK_EQ(raw.c_cc[VTIME], 0);

    CHECK_EQ(oi_cli_terminal_restore(&terminal), OI_OK);
    CHECK(!terminal.active);
    CHECK(read_exact(master_fd, output, sizeof output));
    CHECK(memcmp(output, paste_off, sizeof output) == 0);
    CHECK_EQ(tcgetattr(slave_fd, &restored), 0);
    CHECK_EQ(restored.c_iflag, original.c_iflag);
    CHECK_EQ(restored.c_oflag, original.c_oflag);
    CHECK_EQ(restored.c_cflag, original.c_cflag);
    CHECK_EQ(restored.c_lflag, original.c_lflag);
    CHECK(memcmp(restored.c_cc, original.c_cc, sizeof original.c_cc) == 0);
    CHECK_EQ(oi_cli_terminal_restore(&terminal), OI_OK);

    close(slave_fd);
    close(master_fd);
}

TEST(non_terminal_descriptors_are_refused) {
    struct oi_cli_terminal terminal;
    int pipe_fds[2] = {-1, -1};

    CHECK_EQ(pipe(pipe_fds), 0);
    if (pipe_fds[0] < 0 || pipe_fds[1] < 0) {
        return;
    }
    oi_cli_terminal_init(&terminal);
    CHECK_EQ(oi_cli_terminal_enable(&terminal, pipe_fds[0], pipe_fds[1]),
             OI_ERR_INVAL);
    CHECK(!terminal.active);
    CHECK_EQ(oi_cli_terminal_enable(NULL, pipe_fds[0], pipe_fds[1]),
             OI_ERR_INVAL);
    CHECK_EQ(oi_cli_terminal_restore(NULL), OI_ERR_INVAL);

    close(pipe_fds[0]);
    close(pipe_fds[1]);
}

int main(void) {
    RUN(enable_sets_raw_mode_and_restore_reinstates_it);
    RUN(non_terminal_descriptors_are_refused);
    return oi_test_report();
}
