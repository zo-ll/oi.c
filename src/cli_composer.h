#ifndef OI_CLI_COMPOSER_H
#define OI_CLI_COMPOSER_H

#include <stddef.h>

#include "cli_input.h"
#include "cli_input_history.h"
#include "cli_prompt_state.h"
#include "cli_render.h"
#include "cli_selector.h"
#include "cli_terminal.h"
#include "oi/reactor.h"
#include "oi/status.h"

#define OI_CLI_COMPOSER_INPUT_CAP 4096U
#define OI_CLI_COMPOSER_ESCAPE_TIMEOUT_MS 40

struct oi_cli_composer {
    struct oi_cli_terminal terminal;
    struct oi_cli_input_decoder decoder;
    struct oi_cli_prompt_state state;
    struct oi_cli_render render;
    unsigned char input[OI_CLI_COMPOSER_INPUT_CAP];
    size_t input_len;
    int input_fd;
    int output_fd;
    oi_reactor *reactor;
};

oi_status oi_cli_composer_init(struct oi_cli_composer *composer, int input_fd,
                               int output_fd,
                               struct oi_cli_input_history *history);
void oi_cli_composer_free(struct oi_cli_composer *composer);

oi_status oi_cli_composer_wait_submit(struct oi_cli_composer *composer,
                                      int signal_fd, char **out_text,
                                      size_t *out_len, int *out_exit,
                                      int *out_terminate_signal);

enum oi_cli_composer_action {
    OI_CLI_COMPOSER_ACTION_NONE = 0,
    OI_CLI_COMPOSER_ACTION_SUBMIT,
    OI_CLI_COMPOSER_ACTION_EXIT,
    OI_CLI_COMPOSER_ACTION_CTRL_C
};

oi_status oi_cli_composer_feed(struct oi_cli_composer *composer,
                               enum oi_cli_composer_action *out_action);

int oi_cli_composer_escape_pending(const struct oi_cli_composer *composer);

oi_status oi_cli_composer_resolve_escape(
    struct oi_cli_composer *composer,
    enum oi_cli_composer_action *out_action);

oi_status oi_cli_composer_redraw(struct oi_cli_composer *composer);

oi_status oi_cli_composer_commit_draft(struct oi_cli_composer *composer,
                                       char **out_text, size_t *out_len);

oi_status oi_cli_composer_set_draft(struct oi_cli_composer *composer,
                                    const char *text, size_t text_len);

typedef int (*oi_cli_composer_raw_event_cb)(
    const struct oi_cli_input_event *event, void *user_data);

oi_status oi_cli_composer_feed_raw(struct oi_cli_composer *composer,
                                   oi_cli_composer_raw_event_cb on_event,
                                   void *user_data);
oi_status oi_cli_composer_resolve_escape_raw(
    struct oi_cli_composer *composer,
    oi_cli_composer_raw_event_cb on_event, void *user_data);

oi_status oi_cli_composer_redraw_panel(
    struct oi_cli_composer *composer,
    const struct oi_cli_render_line *header_lines, size_t header_count);
oi_status oi_cli_composer_draw_selector(
    struct oi_cli_composer *composer,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t selected_option);

oi_status oi_cli_composer_select(
    struct oi_cli_composer *composer, int signal_fd,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const struct oi_cli_render_line *option_lines, size_t option_count,
    size_t default_selected, size_t *out_selected, int *out_cancelled,
    int *out_terminate_signal);

oi_status oi_cli_composer_read_line(
    struct oi_cli_composer *composer, int signal_fd,
    const struct oi_cli_render_line *header_lines, size_t header_count,
    const char *prompt, int masked, char **out_text, size_t *out_len,
    int *out_cancelled, int *out_terminate_signal);

oi_status oi_cli_composer_wait_fd(
    struct oi_cli_composer *composer, int signal_fd,
    const struct oi_cli_render_line *status_line, int fd, int timeout_ms,
    int *out_ready, int *out_cancelled, int *out_terminate_signal);

#endif
