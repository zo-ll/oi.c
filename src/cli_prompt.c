/* struct signalfd_siginfo is a GNU/Linux extension; needs _GNU_SOURCE
 * defined before any header pulls in its own feature-test macros,
 * matching the precedent in reactor_epoll.c for other GNU fd primitives
 * (timerfd, pidfd). */
#define _GNU_SOURCE

#include "cli_prompt.h"

#include "cli_input.h"
#include "cli_prompt_state.h"
#include "cli_render.h"
#include "cli_terminal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define OI_CLI_PROMPT_INPUT_CAP 4096U
#define OI_CLI_ESCAPE_TIMEOUT_MS 40

static size_t terminal_columns(int fd) {
    struct winsize size;

    memset(&size, 0, sizeof size);
    if (ioctl(fd, TIOCGWINSZ, &size) == 0 && size.ws_col >= 3) {
        return size.ws_col;
    }
    return 80;
}

static void consume_input(unsigned char *input, size_t *input_len,
                          size_t consumed) {
    if (consumed < *input_len) {
        memmove(input, input + consumed, *input_len - consumed);
    }
    *input_len -= consumed;
}

/*
 * Drains every currently-readable signalfd record, classifying what was
 * seen. Each of SIGWINCH/SIGINT/SIGTERM/SIGHUP is a standard (non-realtime)
 * signal, so at most one of each is ever actually pending -- looping here
 * is defensive, not load-bearing. SIGTERM/SIGHUP win over a same-drain
 * SIGWINCH: there is no point redrawing a frame the caller is about to
 * tear down.
 */
struct oi_cli_prompt_signals {
    int resize;
    int terminate_signal;
};

static void drain_signals(int signal_fd, struct oi_cli_prompt_signals *out) {
    struct signalfd_siginfo info;

    memset(out, 0, sizeof *out);
    while (read(signal_fd, &info, sizeof info) == (ssize_t)sizeof info) {
        switch (info.ssi_signo) {
        case SIGWINCH:
            out->resize = 1;
            break;
        case SIGTERM:
        case SIGHUP:
        case SIGINT:
            /* SIGINT can't ordinarily fire here (raw mode clears ISIG),
             * but treat a pending one defensively the same as
             * SIGTERM/SIGHUP rather than silently dropping it. */
            out->terminate_signal = (int)info.ssi_signo;
            break;
        default:
            break;
        }
    }
}

static oi_status handle_resize(int output_fd, struct oi_cli_render *render,
                               const struct oi_cli_prompt_state *state) {
    oi_cli_render_set_columns(render, terminal_columns(output_fd));
    return oi_cli_render_draw_commands(
        render, &state->editor, state->command_matches,
        state->command_match_count, state->command_selection);
}

static oi_status apply_event(struct oi_cli_prompt_state *state,
                             struct oi_cli_render *render,
                             const struct oi_cli_input_event *event,
                             int *out_done, char **out_text, size_t *out_len,
                             int *out_exit) {
    enum oi_cli_prompt_action action;
    oi_status status = oi_cli_prompt_state_apply(state, event, &action);

    if (status != OI_OK) {
        return status;
    }
    switch (action) {
    case OI_CLI_PROMPT_ACTION_NONE:
        return OI_OK;
    case OI_CLI_PROMPT_ACTION_REDRAW:
        return oi_cli_render_draw_commands(
            render, &state->editor, state->command_matches,
            state->command_match_count, state->command_selection);
    case OI_CLI_PROMPT_ACTION_SUBMIT:
        status = oi_cli_prompt_state_commit(state, out_text, out_len);
        if (status == OI_OK) {
            *out_done = 1;
        }
        return status;
    case OI_CLI_PROMPT_ACTION_EXIT:
        *out_exit = 1;
        *out_done = 1;
        return OI_OK;
    }
    return OI_ERR_INVAL;
}

oi_status oi_cli_prompt_read(int input_fd, int output_fd, int signal_fd,
                             struct oi_cli_input_history *history,
                             char **out_text, size_t *out_len, int *out_exit,
                             int *out_terminate_signal) {
    struct oi_cli_terminal terminal;
    struct oi_cli_input_decoder decoder;
    struct oi_cli_prompt_state state;
    struct oi_cli_render render;
    unsigned char input[OI_CLI_PROMPT_INPUT_CAP];
    size_t input_len = 0;
    int state_initialized = 0;
    int render_initialized = 0;
    int done = 0;
    oi_status status;

    if (input_fd < 0 || output_fd < 0 || history == NULL ||
        out_text == NULL || out_len == NULL || out_exit == NULL ||
        out_terminate_signal == NULL) {
        return OI_ERR_INVAL;
    }
    *out_text = NULL;
    *out_len = 0;
    *out_exit = 0;
    *out_terminate_signal = 0;
    oi_cli_terminal_init(&terminal);
    oi_cli_input_decoder_init(&decoder);

    status = oi_cli_terminal_enable(&terminal, input_fd, output_fd);
    if (status != OI_OK) {
        return status;
    }
    status = oi_cli_prompt_state_init(&state, history);
    if (status != OI_OK) {
        goto cleanup;
    }
    state_initialized = 1;
    status =
        oi_cli_render_init(&render, output_fd, terminal_columns(output_fd));
    if (status != OI_OK) {
        goto cleanup;
    }
    render_initialized = 1;
    status = oi_cli_render_draw(&render, &state.editor);
    if (status != OI_OK) {
        goto cleanup;
    }
    if (signal_fd >= 0) {
        /* The draw above already used a freshly-read width; discard any
         * resize notification queued before this call started so it
         * doesn't trigger an immediate, redundant second redraw. A
         * terminate signal queued in that same window must not be lost,
         * though -- honor it immediately instead. */
        struct oi_cli_prompt_signals pending;

        drain_signals(signal_fd, &pending);
        if (pending.terminate_signal != 0) {
            *out_terminate_signal = pending.terminate_signal;
            goto cleanup;
        }
    }

    while (!done) {
        while (input_len != 0) {
            struct oi_cli_input_event event;
            size_t consumed;

            status = oi_cli_input_decode(&decoder, input, input_len,
                                         &consumed, &event);
            if (status == OI_ERR_AGAIN) {
                break;
            }
            if (status != OI_OK) {
                goto cleanup;
            }
            consume_input(input, &input_len, consumed);
            status = apply_event(&state, &render, &event, &done, out_text,
                                 out_len, out_exit);
            if (status != OI_OK || done) {
                break;
            }
        }
        if (status != OI_OK || done) {
            break;
        }

        {
            struct pollfd descriptors[2];
            nfds_t ndescriptors = 1;
            int timeout =
                input_len != 0 && input[0] == '\x1b' && !decoder.pasting
                    ? OI_CLI_ESCAPE_TIMEOUT_MS
                    : -1;
            int ready;

            descriptors[0].fd = input_fd;
            descriptors[0].events = POLLIN;
            descriptors[0].revents = 0;
            if (signal_fd >= 0) {
                descriptors[1].fd = signal_fd;
                descriptors[1].events = POLLIN;
                descriptors[1].revents = 0;
                ndescriptors = 2;
            }

            do {
                ready = poll(descriptors, ndescriptors, timeout);
            } while (ready < 0 && errno == EINTR);
            if (ready < 0) {
                status = OI_ERR_IO;
                break;
            }
            if (ready == 0) {
                struct oi_cli_input_event event;
                size_t consumed;

                status = oi_cli_input_decode_escape(&consumed, &event);
                if (status != OI_OK) {
                    break;
                }
                consume_input(input, &input_len, consumed);
                status = apply_event(&state, &render, &event, &done,
                                     out_text, out_len, out_exit);
                continue;
            }
            if (ndescriptors == 2 &&
                (descriptors[1].revents & POLLIN) != 0) {
                struct oi_cli_prompt_signals signals;

                drain_signals(signal_fd, &signals);
                if (signals.terminate_signal != 0) {
                    *out_terminate_signal = signals.terminate_signal;
                    done = 1;
                    continue;
                }
                if (signals.resize) {
                    status = handle_resize(output_fd, &render, &state);
                    if (status != OI_OK) {
                        break;
                    }
                }
                continue;
            }
            if ((descriptors[0].revents & (POLLERR | POLLNVAL)) != 0) {
                status = OI_ERR_IO;
                break;
            }
            if ((descriptors[0].revents & (POLLIN | POLLHUP)) != 0) {
                ssize_t read_len;

                if (input_len == sizeof input) {
                    status = OI_ERR_PARSE;
                    break;
                }
                do {
                    read_len = read(input_fd, input + input_len,
                                    sizeof input - input_len);
                } while (read_len < 0 && errno == EINTR);
                if (read_len > 0) {
                    input_len += (size_t)read_len;
                } else if (read_len == 0) {
                    status = OI_ERR_CLOSED;
                    break;
                } else {
                    status = OI_ERR_IO;
                    break;
                }
            }
        }
    }

cleanup:
    if (render_initialized &&
        oi_cli_render_finish(&render) != OI_OK && status == OI_OK) {
        status = OI_ERR_IO;
    }
    if (state_initialized) {
        oi_cli_prompt_state_free(&state);
    }
    if (oi_cli_terminal_restore(&terminal) != OI_OK && status == OI_OK) {
        status = OI_ERR_IO;
    }
    if (status != OI_OK) {
        free(*out_text);
        *out_text = NULL;
        *out_len = 0;
        *out_exit = 0;
    }
    return status;
}
