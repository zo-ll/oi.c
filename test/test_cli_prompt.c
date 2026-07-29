/* signalfd(2) is a GNU/Linux extension, needed by the resize test below to
 * create its own SIGWINCH signalfd (mirroring what cli_repl.c owns in
 * production). */
#define _GNU_SOURCE

#include "cli_prompt.h"
#include "test.h"

#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

struct prompt_result {
    int status;
    int exit_requested;
    size_t text_len;
    char text[128];
};

static int write_all(int fd, const void *data, size_t len) {
    const char *bytes = data;
    size_t written = 0;

    while (written < len) {
        ssize_t result = write(fd, bytes + written, len - written);
        if (result <= 0) {
            return 0;
        }
        written += (size_t)result;
    }
    return 1;
}

static int read_all(int fd, void *data, size_t len) {
    char *bytes = data;
    size_t received = 0;

    while (received < len) {
        ssize_t result = read(fd, bytes + received, len - received);
        if (result <= 0) {
            return 0;
        }
        received += (size_t)result;
    }
    return 1;
}

static void wait_for_prompt_output(int master_fd) {
    struct pollfd descriptor = {
        .fd = master_fd,
        .events = POLLIN,
    };
    char output[256];

    CHECK(poll(&descriptor, 1, 2000) > 0);
    CHECK(read(master_fd, output, sizeof output) > 0);
}

static struct prompt_result run_prompt(const char *input, size_t input_len,
                                       struct termios *out_original,
                                       struct termios *out_restored) {
    struct prompt_result result;
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);
    if (master_fd < 0 || slave_fd < 0 || result_pipe[0] < 0 ||
        result_pipe[1] < 0) {
        result.status = OI_ERR_IO;
        return result;
    }
    CHECK_EQ(tcgetattr(slave_fd, out_original), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        char *text = NULL;
        size_t text_len = 0;
        int exit_requested = 0;
        oi_status status;

        close(master_fd);
        close(result_pipe[0]);
        /* A plain fork() inherits the test harness's own session/process
         * group, not one attached to this pty -- a resize test's
         * TIOCSWINSZ-triggered SIGWINCH has nowhere to go without a real
         * controlling terminal established here first. */
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            close(result_pipe[1]);
            close(slave_fd);
            _exit(1);
        }
        oi_cli_input_history_init(&history);
        status = oi_cli_prompt_read(slave_fd, slave_fd, -1, &history, &text,
                                    &text_len, &exit_requested);
        result.status = status;
        result.exit_requested = exit_requested;
        result.text_len = text_len;
        if (text_len != 0 && text_len <= sizeof result.text) {
            memcpy(result.text, text, text_len);
        }
        (void)write_all(result_pipe[1], &result, sizeof result);
        free(text);
        oi_cli_input_history_free(&history);
        close(result_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(result_pipe[1]);
    wait_for_prompt_output(master_fd);
    CHECK(write_all(master_fd, input, input_len));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(tcgetattr(slave_fd, out_restored), 0);
    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
    return result;
}

static void check_restored(const struct termios *original,
                           const struct termios *restored) {
    CHECK_EQ(restored->c_iflag, original->c_iflag);
    CHECK_EQ(restored->c_oflag, original->c_oflag);
    CHECK_EQ(restored->c_cflag, original->c_cflag);
    CHECK_EQ(restored->c_lflag, original->c_lflag);
    CHECK(memcmp(restored->c_cc, original->c_cc, sizeof original->c_cc) == 0);
}

TEST(cursor_editing_submits_expected_text_and_restores_terminal) {
    static const char input[] = "ab\x1b[DX\r";
    struct termios original;
    struct termios restored;
    struct prompt_result result =
        run_prompt(input, sizeof input - 1, &original, &restored);

    CHECK_EQ(result.status, OI_OK);
    CHECK(!result.exit_requested);
    CHECK_EQ(result.text_len, 3);
    CHECK(memcmp(result.text, "aXb", 3) == 0);
    check_restored(&original, &restored);
}

TEST(bracketed_multiline_paste_is_one_submission) {
    static const char input[] = "\x1b[200~one\ntwo\x1b[201~\r";
    struct termios original;
    struct termios restored;
    struct prompt_result result =
        run_prompt(input, sizeof input - 1, &original, &restored);

    CHECK_EQ(result.status, OI_OK);
    CHECK_EQ(result.text_len, 7);
    CHECK(memcmp(result.text, "one\ntwo", 7) == 0);
    check_restored(&original, &restored);
}

TEST(ctrl_c_clears_and_ctrl_d_exits) {
    static const char input[] = "discard\x03\x04";
    struct termios original;
    struct termios restored;
    struct prompt_result result =
        run_prompt(input, sizeof input - 1, &original, &restored);

    CHECK_EQ(result.status, OI_OK);
    CHECK(result.exit_requested);
    CHECK_EQ(result.text_len, 0);
    check_restored(&original, &restored);
}

struct captured_output {
    char data[2048];
    size_t len;
};

static int wait_for_text(int fd, struct captured_output *out,
                         const char *needle, int timeout_ms) {
    while (out->len < sizeof out->data - 1) {
        struct pollfd descriptor = {
            .fd = fd,
            .events = POLLIN,
        };
        ssize_t n;

        if (strstr(out->data, needle) != NULL) {
            return 1;
        }
        if (poll(&descriptor, 1, timeout_ms) <= 0) {
            return 0;
        }
        n = read(fd, out->data + out->len, sizeof out->data - 1 - out->len);
        if (n <= 0) {
            return 0;
        }
        out->len += (size_t)n;
        out->data[out->len] = '\0';
    }
    return 0;
}

TEST(resize_redraws_at_the_new_width_and_preserves_submitted_text) {
    struct prompt_result result;
    struct captured_output output;
    struct winsize initial = {0};
    struct winsize resized = {0};
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    pid_t child;

    memset(&result, 0, sizeof result);
    memset(&output, 0, sizeof output);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);

    initial.ws_row = 24;
    initial.ws_col = 20;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &initial), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        sigset_t winch;
        char *text = NULL;
        size_t text_len = 0;
        int exit_requested = 0;
        int resize_fd;
        oi_status status;

        close(master_fd);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            close(result_pipe[1]);
            close(slave_fd);
            _exit(1);
        }
        sigemptyset(&winch);
        sigaddset(&winch, SIGWINCH);
        if (sigprocmask(SIG_BLOCK, &winch, NULL) != 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            close(result_pipe[1]);
            close(slave_fd);
            _exit(1);
        }
        resize_fd = signalfd(-1, &winch, SFD_NONBLOCK | SFD_CLOEXEC);
        oi_cli_input_history_init(&history);
        status = oi_cli_prompt_read(slave_fd, slave_fd, resize_fd, &history,
                                    &text, &text_len, &exit_requested);
        result.status = status;
        result.exit_requested = exit_requested;
        result.text_len = text_len;
        if (text_len != 0 && text_len <= sizeof result.text) {
            memcpy(result.text, text, text_len);
        }
        (void)write_all(result_pipe[1], &result, sizeof result);
        free(text);
        oi_cli_input_history_free(&history);
        if (resize_fd >= 0) {
            close(resize_fd);
        }
        close(result_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(result_pipe[1]);
    wait_for_prompt_output(master_fd);
    CHECK(write_all(master_fd, "abcdefghij", 10));
    CHECK(wait_for_text(master_fd, &output, "abcdefghij", 2000));

    /* Reset the capture and resize: a fresh "\x1b[J" appearing without any
     * further keystroke proves the redraw was driven by the resize signal
     * itself, not by continued typing. */
    memset(&output, 0, sizeof output);
    resized.ws_row = 24;
    resized.ws_col = 8;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &resized), 0);
    CHECK(wait_for_text(master_fd, &output, "\x1b[J", 2000));

    CHECK(write_all(master_fd, "\r", 1));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK(!result.exit_requested);
    CHECK_EQ(result.text_len, 10);
    CHECK(memcmp(result.text, "abcdefghij", 10) == 0);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

int main(void) {
    RUN(cursor_editing_submits_expected_text_and_restores_terminal);
    RUN(bracketed_multiline_paste_is_one_submission);
    RUN(ctrl_c_clears_and_ctrl_d_exits);
    RUN(resize_redraws_at_the_new_width_and_preserves_submitted_text);
    return oi_test_report();
}
