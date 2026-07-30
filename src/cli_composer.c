/* struct signalfd_siginfo is a GNU/Linux extension; needs _GNU_SOURCE
 * defined before any header pulls in its own feature-test macros,
 * matching the precedent in reactor_epoll.c for other GNU fd primitives
 * (timerfd, pidfd). */
#define _GNU_SOURCE

#include "cli_composer.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

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
struct oi_cli_composer_signals {
    int resize;
    int terminate_signal;
};

static void drain_signals(int signal_fd,
                          struct oi_cli_composer_signals *out) {
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

static oi_status handle_resize(struct oi_cli_composer *composer) {
    oi_cli_render_set_columns(&composer->render,
                              terminal_columns(composer->output_fd));
    return oi_cli_render_draw_commands(
        &composer->render, &composer->state.editor,
        composer->state.command_matches, composer->state.command_match_count,
        composer->state.command_selection);
}

static oi_status apply_event(struct oi_cli_composer *composer,
                             const struct oi_cli_input_event *event,
                             int *out_done, char **out_text, size_t *out_len,
                             int *out_exit) {
    enum oi_cli_prompt_action action;
    oi_status status =
        oi_cli_prompt_state_apply(&composer->state, event, &action);

    if (status != OI_OK) {
        return status;
    }
    switch (action) {
    case OI_CLI_PROMPT_ACTION_NONE:
        return OI_OK;
    case OI_CLI_PROMPT_ACTION_REDRAW:
        return oi_cli_render_draw_commands(
            &composer->render, &composer->state.editor,
            composer->state.command_matches,
            composer->state.command_match_count,
            composer->state.command_selection);
    case OI_CLI_PROMPT_ACTION_SUBMIT:
        status = oi_cli_prompt_state_commit(&composer->state, out_text,
                                            out_len);
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

oi_status oi_cli_composer_init(struct oi_cli_composer *composer, int input_fd,
                               int output_fd,
                               struct oi_cli_input_history *history) {
    oi_status status;

    if (composer == NULL || input_fd < 0 || output_fd < 0 ||
        history == NULL) {
        return OI_ERR_INVAL;
    }
    composer->input_fd = input_fd;
    composer->output_fd = output_fd;
    composer->input_len = 0;
    oi_cli_terminal_init(&composer->terminal);
    oi_cli_input_decoder_init(&composer->decoder);

    status = oi_cli_terminal_enable(&composer->terminal, input_fd, output_fd);
    if (status != OI_OK) {
        return status;
    }
    status = oi_cli_prompt_state_init(&composer->state, history);
    if (status != OI_OK) {
        oi_cli_terminal_restore(&composer->terminal);
        return status;
    }
    status = oi_cli_render_init(&composer->render, output_fd,
                                terminal_columns(output_fd));
    if (status != OI_OK) {
        oi_cli_prompt_state_free(&composer->state);
        oi_cli_terminal_restore(&composer->terminal);
        return status;
    }
    return OI_OK;
}

void oi_cli_composer_free(struct oi_cli_composer *composer) {
    if (composer == NULL) {
        return;
    }
    oi_cli_prompt_state_free(&composer->state);
    oi_cli_terminal_restore(&composer->terminal);
}

oi_status oi_cli_composer_wait_submit(struct oi_cli_composer *composer,
                                      int signal_fd, char **out_text,
                                      size_t *out_len, int *out_exit,
                                      int *out_terminate_signal) {
    int done = 0;
    oi_status status;

    if (composer == NULL || out_text == NULL || out_len == NULL ||
        out_exit == NULL || out_terminate_signal == NULL) {
        return OI_ERR_INVAL;
    }
    *out_text = NULL;
    *out_len = 0;
    *out_exit = 0;
    *out_terminate_signal = 0;

    oi_cli_render_set_columns(&composer->render,
                              terminal_columns(composer->output_fd));
    status = oi_cli_render_draw(&composer->render, &composer->state.editor);
    if (status != OI_OK) {
        goto cleanup;
    }
    if (signal_fd >= 0) {
        /* The draw above already used a freshly-read width; discard any
         * resize notification queued before this call started so it
         * doesn't trigger an immediate, redundant second redraw. A
         * terminate signal queued in that same window must not be lost,
         * though -- honor it immediately instead. */
        struct oi_cli_composer_signals pending;

        drain_signals(signal_fd, &pending);
        if (pending.terminate_signal != 0) {
            *out_terminate_signal = pending.terminate_signal;
            goto cleanup;
        }
    }

    while (!done) {
        while (composer->input_len != 0) {
            struct oi_cli_input_event event;
            size_t consumed;

            status = oi_cli_input_decode(&composer->decoder, composer->input,
                                         composer->input_len, &consumed,
                                         &event);
            if (status == OI_ERR_AGAIN) {
                break;
            }
            if (status != OI_OK) {
                goto cleanup;
            }
            consume_input(composer->input, &composer->input_len, consumed);
            status = apply_event(composer, &event, &done, out_text, out_len,
                                 out_exit);
            if (status != OI_OK || done) {
                break;
            }
        }
        /* A lone buffered byte that's a valid but incomplete sequence (a
         * standalone Escape keypress with nothing following yet, or the
         * first byte of one) breaks the loop above with OI_ERR_AGAIN --
         * that's an expected "nothing more to decode without further
         * bytes" signal, not a real error, and must not be mistaken for
         * one here: the whole point of falling through to the poll()
         * below is to wait for either more bytes or the escape timeout to
         * resolve it. Without this, a genuinely standalone Escape press
         * would incorrectly end this call with OI_ERR_AGAIN instead of
         * ever reaching that timeout. */
        if (status == OI_ERR_AGAIN) {
            status = OI_OK;
        }
        if (status != OI_OK || done) {
            break;
        }

        {
            struct pollfd descriptors[2];
            nfds_t ndescriptors = 1;
            int timeout = composer->input_len != 0 &&
                                  composer->input[0] == '\x1b' &&
                                  !composer->decoder.pasting
                              ? OI_CLI_COMPOSER_ESCAPE_TIMEOUT_MS
                              : -1;
            int ready;

            descriptors[0].fd = composer->input_fd;
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
                consume_input(composer->input, &composer->input_len,
                             consumed);
                status = apply_event(composer, &event, &done, out_text,
                                     out_len, out_exit);
                continue;
            }
            if (ndescriptors == 2 &&
                (descriptors[1].revents & POLLIN) != 0) {
                struct oi_cli_composer_signals signals;

                drain_signals(signal_fd, &signals);
                if (signals.terminate_signal != 0) {
                    *out_terminate_signal = signals.terminate_signal;
                    done = 1;
                    continue;
                }
                if (signals.resize) {
                    status = handle_resize(composer);
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

                if (composer->input_len == sizeof composer->input) {
                    status = OI_ERR_PARSE;
                    break;
                }
                do {
                    read_len = read(composer->input_fd,
                                    composer->input + composer->input_len,
                                    sizeof composer->input -
                                        composer->input_len);
                } while (read_len < 0 && errno == EINTR);
                if (read_len > 0) {
                    composer->input_len += (size_t)read_len;
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
    if (oi_cli_render_finish(&composer->render) != OI_OK && status == OI_OK) {
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

/*
 * Applies one already-decoded event to the editor (mutating state exactly
 * like the idle path does), but never draws and never acts on
 * SUBMIT/EXIT/CTRL_C itself -- just classifies which of those (if any)
 * this event produced, for a busy-mode caller to decide what to do with.
 * oi_cli_prompt_state_apply's own SUBMIT/EXIT handling never mutates the
 * editor by itself (SUBMIT is only actually committed by a later, separate
 * oi_cli_prompt_state_commit call; EXIT only fires when the editor was
 * already empty), so it's safe to let it run unconditionally here and
 * classify the result afterward -- CTRL_C is the one exception (it clears
 * the draft immediately as part of applying it), which is fine: a
 * busy-mode Ctrl+C clearing the in-progress draft alongside cancelling the
 * turn matches the idle Ctrl+C behavior of clearing on its own.
 */
static oi_status classify_event(struct oi_cli_composer *composer,
                                const struct oi_cli_input_event *event,
                                enum oi_cli_composer_action *out_action) {
    enum oi_cli_prompt_action prompt_action;
    oi_status status =
        oi_cli_prompt_state_apply(&composer->state, event, &prompt_action);

    if (status != OI_OK) {
        return status;
    }
    if (event->type == OI_CLI_INPUT_CTRL_C) {
        *out_action = OI_CLI_COMPOSER_ACTION_CTRL_C;
    } else if (event->type == OI_CLI_INPUT_ENTER &&
              prompt_action == OI_CLI_PROMPT_ACTION_SUBMIT) {
        *out_action = OI_CLI_COMPOSER_ACTION_SUBMIT;
    } else if (event->type == OI_CLI_INPUT_CTRL_D &&
              prompt_action == OI_CLI_PROMPT_ACTION_EXIT) {
        *out_action = OI_CLI_COMPOSER_ACTION_EXIT;
    } else {
        *out_action = OI_CLI_COMPOSER_ACTION_NONE;
    }
    return OI_OK;
}

oi_status oi_cli_composer_feed(struct oi_cli_composer *composer,
                               enum oi_cli_composer_action *out_action) {
    ssize_t read_len;

    if (composer == NULL || out_action == NULL) {
        return OI_ERR_INVAL;
    }
    *out_action = OI_CLI_COMPOSER_ACTION_NONE;

    if (composer->input_len == sizeof composer->input) {
        return OI_ERR_PARSE;
    }
    do {
        read_len = read(composer->input_fd,
                        composer->input + composer->input_len,
                        sizeof composer->input - composer->input_len);
    } while (read_len < 0 && errno == EINTR);
    if (read_len == 0) {
        return OI_ERR_CLOSED;
    }
    if (read_len < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? OI_OK : OI_ERR_IO;
    }
    composer->input_len += (size_t)read_len;

    while (composer->input_len != 0) {
        struct oi_cli_input_event event;
        size_t consumed;
        oi_status status = oi_cli_input_decode(
            &composer->decoder, composer->input, composer->input_len,
            &consumed, &event);

        if (status == OI_ERR_AGAIN) {
            break;
        }
        if (status != OI_OK) {
            return status;
        }
        consume_input(composer->input, &composer->input_len, consumed);
        status = classify_event(composer, &event, out_action);
        if (status != OI_OK) {
            return status;
        }
        if (*out_action != OI_CLI_COMPOSER_ACTION_NONE) {
            break;
        }
    }
    return OI_OK;
}

int oi_cli_composer_escape_pending(const struct oi_cli_composer *composer) {
    return composer != NULL && composer->input_len != 0 &&
           composer->input[0] == '\x1b' && !composer->decoder.pasting;
}

oi_status oi_cli_composer_resolve_escape(
    struct oi_cli_composer *composer,
    enum oi_cli_composer_action *out_action) {
    struct oi_cli_input_event event;
    size_t consumed;
    oi_status status;

    if (composer == NULL || out_action == NULL) {
        return OI_ERR_INVAL;
    }
    *out_action = OI_CLI_COMPOSER_ACTION_NONE;

    status = oi_cli_input_decode_escape(&consumed, &event);
    if (status != OI_OK) {
        return status;
    }
    consume_input(composer->input, &composer->input_len, consumed);
    return classify_event(composer, &event, out_action);
}

oi_status oi_cli_composer_redraw(struct oi_cli_composer *composer) {
    if (composer == NULL) {
        return OI_ERR_INVAL;
    }
    return handle_resize(composer);
}

oi_status oi_cli_composer_commit_draft(struct oi_cli_composer *composer,
                                       char **out_text, size_t *out_len) {
    if (composer == NULL) {
        return OI_ERR_INVAL;
    }
    return oi_cli_prompt_state_commit(&composer->state, out_text, out_len);
}

oi_status oi_cli_composer_set_draft(struct oi_cli_composer *composer,
                                    const char *text, size_t text_len) {
    if (composer == NULL) {
        return OI_ERR_INVAL;
    }
    return oi_cli_editor_set(&composer->state.editor, text, text_len);
}

oi_status oi_cli_composer_feed_raw(struct oi_cli_composer *composer,
                                   oi_cli_composer_raw_event_cb on_event,
                                   void *user_data) {
    ssize_t read_len;

    if (composer == NULL || on_event == NULL) {
        return OI_ERR_INVAL;
    }
    if (composer->input_len == sizeof composer->input) {
        return OI_ERR_PARSE;
    }
    do {
        read_len = read(composer->input_fd,
                        composer->input + composer->input_len,
                        sizeof composer->input - composer->input_len);
    } while (read_len < 0 && errno == EINTR);
    if (read_len == 0) {
        return OI_ERR_CLOSED;
    }
    if (read_len < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK ? OI_OK : OI_ERR_IO;
    }
    composer->input_len += (size_t)read_len;

    while (composer->input_len != 0) {
        struct oi_cli_input_event event;
        size_t consumed;
        oi_status status = oi_cli_input_decode(
            &composer->decoder, composer->input, composer->input_len,
            &consumed, &event);

        if (status == OI_ERR_AGAIN) {
            break;
        }
        if (status != OI_OK) {
            return status;
        }
        consume_input(composer->input, &composer->input_len, consumed);
        if (on_event(&event, user_data)) {
            break;
        }
    }
    return OI_OK;
}

oi_status oi_cli_composer_resolve_escape_raw(
    struct oi_cli_composer *composer,
    oi_cli_composer_raw_event_cb on_event, void *user_data) {
    struct oi_cli_input_event event;
    size_t consumed;
    oi_status status;

    if (composer == NULL || on_event == NULL) {
        return OI_ERR_INVAL;
    }
    status = oi_cli_input_decode_escape(&consumed, &event);
    if (status != OI_OK) {
        return status;
    }
    consume_input(composer->input, &composer->input_len, consumed);
    (void)on_event(&event, user_data);
    return OI_OK;
}

oi_status oi_cli_composer_redraw_panel(
    struct oi_cli_composer *composer,
    const struct oi_cli_render_line *header_lines, size_t header_count) {
    if (composer == NULL) {
        return OI_ERR_INVAL;
    }
    oi_cli_render_set_columns(&composer->render,
                              terminal_columns(composer->output_fd));
    return oi_cli_render_draw_panel(
        &composer->render, &composer->state.editor, header_lines,
        header_count, composer->state.command_matches,
        composer->state.command_match_count,
        composer->state.command_selection);
}

oi_status oi_cli_composer_draw_selector(
    struct oi_cli_composer *composer,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t selected_option) {
    if (composer == NULL) {
        return OI_ERR_INVAL;
    }
    oi_cli_render_set_columns(&composer->render,
                              terminal_columns(composer->output_fd));
    return oi_cli_render_draw_selector(&composer->render, header_lines,
                                       header_count, option_lines,
                                       option_count, selected_option);
}

oi_status oi_cli_composer_select(
    struct oi_cli_composer *composer, int signal_fd,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t default_selected, size_t *out_selected, int *out_cancelled,
    int *out_terminate_signal) {
    size_t selected;
    int done = 0;
    oi_status status;

    if (composer == NULL || option_lines == NULL || option_count == 0 ||
        default_selected >= option_count || out_selected == NULL ||
        out_cancelled == NULL || out_terminate_signal == NULL) {
        return OI_ERR_INVAL;
    }
    selected = default_selected;
    *out_cancelled = 0;
    *out_terminate_signal = 0;
    status = OI_OK;
    if (signal_fd >= 0) {
        /*
         * Discard a resize queued before this call started, then draw with
         * the current width. This drain must precede the draw: draining
         * afterward can swallow a resize delivered between the draw and the
         * drain, leaving the selector rendered at a stale width until some
         * unrelated input causes another redraw.
         */
        struct oi_cli_composer_signals pending;

        drain_signals(signal_fd, &pending);
        if (pending.terminate_signal != 0) {
            *out_terminate_signal = pending.terminate_signal;
            goto cleanup;
        }
    }
    status = oi_cli_render_draw_selector(&composer->render, header_lines,
                                         header_count, option_lines,
                                         option_count, selected);
    if (status != OI_OK) {
        goto cleanup;
    }

    while (!done) {
        while (composer->input_len != 0) {
            struct oi_cli_input_event event;
            size_t consumed;
            enum oi_cli_selector_action action;

            status = oi_cli_input_decode(&composer->decoder, composer->input,
                                        composer->input_len, &consumed,
                                        &event);
            if (status == OI_ERR_AGAIN) {
                break;
            }
            if (status != OI_OK) {
                goto cleanup;
            }
            consume_input(composer->input, &composer->input_len, consumed);
            status = oi_cli_selector_apply(&event, option_count, &selected,
                                          &action);
            if (status != OI_OK) {
                goto cleanup;
            }
            if (action == OI_CLI_SELECTOR_ACTION_REDRAW) {
                status = oi_cli_render_draw_selector(
                    &composer->render, header_lines, header_count,
                    option_lines, option_count, selected);
            } else if (action == OI_CLI_SELECTOR_ACTION_CONFIRM) {
                done = 1;
            } else if (action == OI_CLI_SELECTOR_ACTION_CANCEL) {
                *out_cancelled = 1;
                done = 1;
            }
            if (status != OI_OK || done) {
                break;
            }
        }
        /* Same normalization as oi_cli_composer_wait_submit's identical
         * inner/outer loop shape: OI_ERR_AGAIN here just means "nothing
         * more to decode without further bytes," not a real error -- a
         * standalone Escape press must fall through to the poll() below
         * to reach its timeout-based resolution, not end this call early. */
        if (status == OI_ERR_AGAIN) {
            status = OI_OK;
        }
        if (status != OI_OK || done) {
            break;
        }

        {
            struct pollfd descriptors[2];
            nfds_t ndescriptors = 1;
            int timeout = composer->input_len != 0 &&
                                  composer->input[0] == '\x1b' &&
                                  !composer->decoder.pasting
                              ? OI_CLI_COMPOSER_ESCAPE_TIMEOUT_MS
                              : -1;
            int ready;

            descriptors[0].fd = composer->input_fd;
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
                enum oi_cli_selector_action action;

                status = oi_cli_input_decode_escape(&consumed, &event);
                if (status != OI_OK) {
                    break;
                }
                consume_input(composer->input, &composer->input_len,
                             consumed);
                status = oi_cli_selector_apply(&event, option_count,
                                              &selected, &action);
                if (status == OI_OK) {
                    if (action == OI_CLI_SELECTOR_ACTION_REDRAW) {
                        status = oi_cli_render_draw_selector(
                            &composer->render, header_lines, header_count,
                            option_lines, option_count, selected);
                    } else if (action == OI_CLI_SELECTOR_ACTION_CONFIRM) {
                        done = 1;
                    } else if (action == OI_CLI_SELECTOR_ACTION_CANCEL) {
                        *out_cancelled = 1;
                        done = 1;
                    }
                }
                continue;
            }
            if (ndescriptors == 2 &&
                (descriptors[1].revents & POLLIN) != 0) {
                struct oi_cli_composer_signals signals;

                drain_signals(signal_fd, &signals);
                if (signals.terminate_signal != 0) {
                    *out_terminate_signal = signals.terminate_signal;
                    done = 1;
                    continue;
                }
                if (signals.resize) {
                    oi_cli_render_set_columns(
                        &composer->render,
                        terminal_columns(composer->output_fd));
                    status = oi_cli_render_draw_selector(
                        &composer->render, header_lines, header_count,
                        option_lines, option_count, selected);
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

                if (composer->input_len == sizeof composer->input) {
                    status = OI_ERR_PARSE;
                    break;
                }
                do {
                    read_len = read(composer->input_fd,
                                    composer->input + composer->input_len,
                                    sizeof composer->input -
                                        composer->input_len);
                } while (read_len < 0 && errno == EINTR);
                if (read_len > 0) {
                    composer->input_len += (size_t)read_len;
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
    *out_selected = selected;
    return status;
}
