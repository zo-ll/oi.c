/* signalfd(2) is a GNU/Linux extension, needed by the resize test below to
 * create its own SIGWINCH signalfd (mirroring what cli_repl.c owns in
 * production). */
#define _GNU_SOURCE

#include "cli_composer.h"
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
#include <time.h>
#include <unistd.h>

struct prompt_result {
    int status;
    int exit_requested;
    int terminate_signal;
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
        {
            struct oi_cli_composer composer;
            int terminate_signal = 0;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_wait_submit(
                    &composer, -1, &text, &text_len, &exit_requested,
                    &terminate_signal);
                oi_cli_composer_free(&composer);
            }
        }
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

TEST(a_standalone_escape_keypress_does_not_end_the_read_with_an_error) {
    struct termios original;
    struct termios restored;
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    struct prompt_result result;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);
    CHECK_EQ(tcgetattr(slave_fd, &original), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        char *text = NULL;
        size_t text_len = 0;
        int exit_requested = 0;
        int terminate_signal = 0;
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
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_wait_submit(
                    &composer, -1, &text, &text_len, &exit_requested,
                    &terminate_signal);
                oi_cli_composer_free(&composer);
            }
        }
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
    /* A genuinely standalone Escape keypress -- nothing follows it at all
     * in this write, unlike every other test here that sends a complete
     * escape sequence in one burst -- must resolve via the composer's own
     * 40ms escape timeout (decode() returns OI_ERR_AGAIN for a lone 0x1b
     * byte, since it could be the start of an arrow key) rather than
     * being mistaken for a real error and ending the read early. */
    CHECK(write_all(master_fd, "\x1b", 1));
    {
        struct timespec delay = {0, (OI_CLI_COMPOSER_ESCAPE_TIMEOUT_MS + 40) *
                                        1000000L};
        nanosleep(&delay, NULL);
    }
    CHECK(write_all(master_fd, "hi\r", 3));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK(!result.exit_requested);
    CHECK_EQ(result.text_len, 2);
    CHECK(memcmp(result.text, "hi", 2) == 0);
    CHECK_EQ(tcgetattr(slave_fd, &restored), 0);
    check_restored(&original, &restored);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
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
        {
            struct oi_cli_composer composer;
            int terminate_signal = 0;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_wait_submit(
                    &composer, resize_fd, &text, &text_len, &exit_requested,
                    &terminate_signal);
                oi_cli_composer_free(&composer);
            }
        }
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

TEST(sigterm_terminates_cleanly_and_restores_the_terminal) {
    struct prompt_result result;
    struct termios original;
    struct termios restored;
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);
    CHECK_EQ(tcgetattr(slave_fd, &original), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        sigset_t signals;
        char *text = NULL;
        size_t text_len = 0;
        int exit_requested = 0;
        int terminate_signal = 0;
        int signal_fd;
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
        sigemptyset(&signals);
        sigaddset(&signals, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &signals, NULL) != 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            close(result_pipe[1]);
            close(slave_fd);
            _exit(1);
        }
        signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_wait_submit(
                    &composer, signal_fd, &text, &text_len, &exit_requested,
                    &terminate_signal);
                oi_cli_composer_free(&composer);
            }
        }
        result.status = status;
        result.exit_requested = exit_requested;
        result.terminate_signal = terminate_signal;
        result.text_len = text_len;
        if (text_len != 0 && text_len <= sizeof result.text) {
            memcpy(result.text, text, text_len);
        }
        (void)write_all(result_pipe[1], &result, sizeof result);
        free(text);
        oi_cli_input_history_free(&history);
        if (signal_fd >= 0) {
            close(signal_fd);
        }
        close(result_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(result_pipe[1]);
    wait_for_prompt_output(master_fd);
    CHECK_EQ(kill(child, SIGTERM), 0);
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK(!result.exit_requested);
    CHECK_EQ(result.terminate_signal, SIGTERM);
    CHECK_EQ(tcgetattr(slave_fd, &restored), 0);
    check_restored(&original, &restored);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
        CHECK_EQ(WEXITSTATUS(child_status), 0);
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

/* --- feed_raw / resolve_escape_raw / select: the modal-safe decode path,
 * used by a future permission selector that must never mutate the editor
 * (composer->state) while it's showing. --- */

struct raw_event_capture {
    enum oi_cli_input_event_type type;
    size_t text_len;
    unsigned char text[4];
    int count;
};

static int capture_and_stop(const struct oi_cli_input_event *event,
                            void *user_data) {
    struct raw_event_capture *capture = user_data;

    capture->type = event->type;
    capture->text_len = event->text_len;
    memcpy(capture->text, event->text, sizeof capture->text);
    capture->count++;
    return 1;
}

struct raw_test_result {
    oi_status status;
    enum oi_cli_input_event_type observed_type;
    int observed_count;
    size_t editor_len;
    char editor_data[32];
};

TEST(feed_raw_reports_the_event_without_touching_the_editor) {
    struct raw_test_result result;
    int ready_pipe[2] = {-1, -1};
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    char ready_byte = 0;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(ready_pipe), 0);
    CHECK_EQ(pipe(result_pipe), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        struct raw_event_capture capture = {0};
        oi_status status;

        close(master_fd);
        close(ready_pipe[0]);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_set_draft(&composer, "keepme", 6);
            }
            if (status == OI_OK) {
                (void)write_all(ready_pipe[1], &ready_byte, 1);
                status = oi_cli_composer_feed_raw(&composer, capture_and_stop,
                                                  &capture);
            }
            if (status == OI_OK) {
                result.editor_len =
                    oi_cli_editor_length(&composer.state.editor);
                if (result.editor_len <= sizeof result.editor_data) {
                    memcpy(result.editor_data,
                          oi_cli_editor_data(&composer.state.editor),
                          result.editor_len);
                }
            }
            oi_cli_composer_free(&composer);
        }
        result.status = status;
        result.observed_type = capture.type;
        result.observed_count = capture.count;
        (void)write_all(result_pipe[1], &result, sizeof result);
        oi_cli_input_history_free(&history);
        close(result_pipe[1]);
        close(ready_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(ready_pipe[1]);
    close(result_pipe[1]);
    CHECK(read_all(ready_pipe[0], &ready_byte, 1));
    CHECK(write_all(master_fd, "\r", 1));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK_EQ(result.observed_count, 1);
    CHECK_EQ(result.observed_type, OI_CLI_INPUT_ENTER);
    /* The whole point of feed_raw: the draft seeded before this call is
     * still exactly what it was -- ENTER was reported, not applied. */
    CHECK_EQ(result.editor_len, 6);
    CHECK(memcmp(result.editor_data, "keepme", 6) == 0);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(ready_pipe[0]);
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

TEST(resolve_escape_raw_reports_a_buffered_lone_escape) {
    struct raw_test_result result;
    int ready_pipe[2] = {-1, -1};
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    char ready_byte = 0;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(ready_pipe), 0);
    CHECK_EQ(pipe(result_pipe), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        struct raw_event_capture capture = {0};
        oi_status status;

        close(master_fd);
        close(ready_pipe[0]);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                (void)write_all(ready_pipe[1], &ready_byte, 1);
                /* A lone Escape byte alone is a valid but incomplete
                 * sequence (it might be the start of an arrow key) --
                 * feed_raw must leave it buffered rather than reporting
                 * it, exactly like oi_cli_composer_feed does. */
                status = oi_cli_composer_feed_raw(&composer, capture_and_stop,
                                                  &capture);
            }
            if (status == OI_OK) {
                CHECK_EQ(capture.count, 0);
                CHECK(oi_cli_composer_escape_pending(&composer));
                status = oi_cli_composer_resolve_escape_raw(
                    &composer, capture_and_stop, &capture);
            }
            oi_cli_composer_free(&composer);
        }
        result.status = status;
        result.observed_type = capture.type;
        result.observed_count = capture.count;
        (void)write_all(result_pipe[1], &result, sizeof result);
        oi_cli_input_history_free(&history);
        close(result_pipe[1]);
        close(ready_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(ready_pipe[1]);
    close(result_pipe[1]);
    CHECK(read_all(ready_pipe[0], &ready_byte, 1));
    CHECK(write_all(master_fd, "\x1b", 1));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK_EQ(result.observed_count, 1);
    CHECK_EQ(result.observed_type, OI_CLI_INPUT_ESCAPE);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(ready_pipe[0]);
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

struct select_result {
    oi_status status;
    size_t selected;
    int cancelled;
    int terminate_signal;
    size_t editor_len;
    char editor_data[32];
};

static const struct oi_cli_render_line select_options[] = {
    {"Allow once", 10},
    {"Allow for process", 18},
    {"Deny", 4},
};

TEST(select_arrow_keys_move_and_enter_confirms) {
    static const char input[] = "\x1b[B\x1b[B\r"; /* Down, Down, Enter */
    struct select_result result;
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        size_t selected = 0;
        int cancelled = 0;
        int terminate_signal = 0;
        oi_status status;

        close(master_fd);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_select(
                    &composer, -1, NULL, 0, select_options, 3, 0, &selected,
                    &cancelled, &terminate_signal);
            }
            oi_cli_composer_free(&composer);
        }
        result.status = status;
        result.selected = selected;
        result.cancelled = cancelled;
        result.terminate_signal = terminate_signal;
        (void)write_all(result_pipe[1], &result, sizeof result);
        oi_cli_input_history_free(&history);
        close(result_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(result_pipe[1]);
    wait_for_prompt_output(master_fd);
    CHECK(write_all(master_fd, input, sizeof input - 1));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK(!result.cancelled);
    CHECK_EQ(result.selected, 2);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

TEST(select_escape_cancels) {
    static const char input[] = "\x1b";
    struct select_result result;
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        size_t selected = 1;
        int cancelled = 0;
        int terminate_signal = 0;
        oi_status status;

        close(master_fd);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_select(
                    &composer, -1, NULL, 0, select_options, 3, 1, &selected,
                    &cancelled, &terminate_signal);
            }
            oi_cli_composer_free(&composer);
        }
        result.status = status;
        result.selected = selected;
        result.cancelled = cancelled;
        result.terminate_signal = terminate_signal;
        (void)write_all(result_pipe[1], &result, sizeof result);
        oi_cli_input_history_free(&history);
        close(result_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(result_pipe[1]);
    wait_for_prompt_output(master_fd);
    CHECK(write_all(master_fd, input, sizeof input - 1));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK(result.cancelled);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

TEST(select_ctrl_c_cancels) {
    static const char input[] = "\x03";
    struct select_result result;
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        size_t selected = 0;
        int cancelled = 0;
        int terminate_signal = 0;
        oi_status status;

        close(master_fd);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_select(
                    &composer, -1, NULL, 0, select_options, 3, 0, &selected,
                    &cancelled, &terminate_signal);
            }
            oi_cli_composer_free(&composer);
        }
        result.status = status;
        result.selected = selected;
        result.cancelled = cancelled;
        result.terminate_signal = terminate_signal;
        (void)write_all(result_pipe[1], &result, sizeof result);
        oi_cli_input_history_free(&history);
        close(result_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(result_pipe[1]);
    wait_for_prompt_output(master_fd);
    CHECK(write_all(master_fd, input, sizeof input - 1));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK(result.cancelled);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

TEST(select_typing_plain_text_does_not_leak_into_the_draft) {
    static const char input[] = "xz\r"; /* not digit shortcuts; confirms
                                          * at the default selection */
    struct select_result result;
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        size_t selected = 0;
        int cancelled = 0;
        int terminate_signal = 0;
        oi_status status;

        close(master_fd);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_set_draft(&composer, "keepme", 6);
            }
            if (status == OI_OK) {
                status = oi_cli_composer_select(
                    &composer, -1, NULL, 0, select_options, 3, 0, &selected,
                    &cancelled, &terminate_signal);
            }
            if (status == OI_OK) {
                result.editor_len =
                    oi_cli_editor_length(&composer.state.editor);
                if (result.editor_len <= sizeof result.editor_data) {
                    memcpy(result.editor_data,
                          oi_cli_editor_data(&composer.state.editor),
                          result.editor_len);
                }
            }
            oi_cli_composer_free(&composer);
        }
        result.status = status;
        result.selected = selected;
        result.cancelled = cancelled;
        (void)write_all(result_pipe[1], &result, sizeof result);
        oi_cli_input_history_free(&history);
        close(result_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(result_pipe[1]);
    wait_for_prompt_output(master_fd);
    CHECK(write_all(master_fd, input, sizeof input - 1));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK(!result.cancelled);
    CHECK_EQ(result.selected, 0);
    /* "x"/"z" are not digit shortcuts (no visible effect) and the editor
     * is never touched by the selector at all -- the pre-seeded draft
     * must still be there, untouched, after the modal resolves. */
    CHECK_EQ(result.editor_len, 6);
    CHECK(memcmp(result.editor_data, "keepme", 6) == 0);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

TEST(select_resize_redraws_the_selector) {
    struct select_result result;
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
    initial.ws_col = 40;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &initial), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        sigset_t winch;
        size_t selected = 0;
        int cancelled = 0;
        int terminate_signal = 0;
        int resize_fd;
        oi_status status;

        close(master_fd);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        sigemptyset(&winch);
        sigaddset(&winch, SIGWINCH);
        if (sigprocmask(SIG_BLOCK, &winch, NULL) != 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        resize_fd = signalfd(-1, &winch, SFD_NONBLOCK | SFD_CLOEXEC);
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_select(
                    &composer, resize_fd, NULL, 0, select_options, 3, 0,
                    &selected, &cancelled, &terminate_signal);
            }
            oi_cli_composer_free(&composer);
        }
        result.status = status;
        result.selected = selected;
        result.cancelled = cancelled;
        result.terminate_signal = terminate_signal;
        (void)write_all(result_pipe[1], &result, sizeof result);
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

    resized.ws_row = 24;
    resized.ws_col = 10;
    CHECK_EQ(ioctl(master_fd, TIOCSWINSZ, &resized), 0);
    /* A fresh "\x1b[J" appearing with no further keystroke proves the
     * redraw was driven by the resize signal, not by typing. */
    CHECK(wait_for_text(master_fd, &output, "\x1b[J", 2000));

    CHECK(write_all(master_fd, "\r", 1));
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK(!result.cancelled);
    CHECK_EQ(result.selected, 0);

    {
        int child_status = 0;
        CHECK_EQ(waitpid(child, &child_status, 0), child);
        CHECK(WIFEXITED(child_status));
    }
    close(result_pipe[0]);
    close(slave_fd);
    close(master_fd);
}

TEST(select_terminate_signal_is_reported) {
    struct select_result result;
    int result_pipe[2] = {-1, -1};
    int master_fd = -1;
    int slave_fd = -1;
    pid_t child;

    memset(&result, 0, sizeof result);
    CHECK_EQ(openpty(&master_fd, &slave_fd, NULL, NULL, NULL), 0);
    CHECK_EQ(pipe(result_pipe), 0);

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        struct oi_cli_input_history history;
        sigset_t signals;
        size_t selected = 0;
        int cancelled = 0;
        int terminate_signal = 0;
        int signal_fd;
        oi_status status;

        close(master_fd);
        close(result_pipe[0]);
        if (setsid() < 0 || ioctl(slave_fd, TIOCSCTTY, 0) < 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        sigemptyset(&signals);
        sigaddset(&signals, SIGTERM);
        if (sigprocmask(SIG_BLOCK, &signals, NULL) != 0) {
            result.status = OI_ERR_IO;
            (void)write_all(result_pipe[1], &result, sizeof result);
            _exit(1);
        }
        signal_fd = signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
        oi_cli_input_history_init(&history);
        {
            struct oi_cli_composer composer;

            status = oi_cli_composer_init(&composer, slave_fd, slave_fd,
                                          &history);
            if (status == OI_OK) {
                status = oi_cli_composer_select(
                    &composer, signal_fd, NULL, 0, select_options, 3, 0,
                    &selected, &cancelled, &terminate_signal);
            }
            oi_cli_composer_free(&composer);
        }
        result.status = status;
        result.selected = selected;
        result.cancelled = cancelled;
        result.terminate_signal = terminate_signal;
        (void)write_all(result_pipe[1], &result, sizeof result);
        oi_cli_input_history_free(&history);
        if (signal_fd >= 0) {
            close(signal_fd);
        }
        close(result_pipe[1]);
        close(slave_fd);
        _exit(0);
    }

    close(result_pipe[1]);
    wait_for_prompt_output(master_fd);
    CHECK_EQ(kill(child, SIGTERM), 0);
    CHECK(read_all(result_pipe[0], &result, sizeof result));
    CHECK_EQ(result.status, OI_OK);
    CHECK_EQ(result.terminate_signal, SIGTERM);
    CHECK(!result.cancelled);

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
    /* A regression that hangs a child must fail this binary, not CI. */
    oi_test_set_deadline(900);
    RUN(cursor_editing_submits_expected_text_and_restores_terminal);
    RUN(bracketed_multiline_paste_is_one_submission);
    RUN(ctrl_c_clears_and_ctrl_d_exits);
    RUN(a_standalone_escape_keypress_does_not_end_the_read_with_an_error);
    RUN(resize_redraws_at_the_new_width_and_preserves_submitted_text);
    RUN(sigterm_terminates_cleanly_and_restores_the_terminal);
    RUN(feed_raw_reports_the_event_without_touching_the_editor);
    RUN(resolve_escape_raw_reports_a_buffered_lone_escape);
    RUN(select_arrow_keys_move_and_enter_confirms);
    RUN(select_escape_cancels);
    RUN(select_ctrl_c_cancels);
    RUN(select_typing_plain_text_does_not_leak_into_the_draft);
    RUN(select_resize_redraws_the_selector);
    RUN(select_terminate_signal_is_reported);
    return oi_test_report();
}
