#include "cli_prompt_state.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

static struct oi_cli_input_event simple_event(
    enum oi_cli_input_event_type type) {
    struct oi_cli_input_event event;
    memset(&event, 0, sizeof event);
    event.type = type;
    return event;
}

static struct oi_cli_input_event text_event(const char *text, size_t len,
                                            int pasted) {
    struct oi_cli_input_event event = simple_event(OI_CLI_INPUT_TEXT);
    memcpy(event.text, text, len);
    event.text_len = len;
    event.pasted = pasted;
    return event;
}

static oi_status apply(struct oi_cli_prompt_state *state,
                       struct oi_cli_input_event event,
                       enum oi_cli_prompt_action *action) {
    return oi_cli_prompt_state_apply(state, &event, action);
}

TEST(editing_events_update_the_buffer) {
    static const char cent[] = {(char)0xc2, (char)0xa2};
    struct oi_cli_input_history history;
    struct oi_cli_prompt_state state;
    enum oi_cli_prompt_action action;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_prompt_state_init(&state, &history), OI_OK);
    CHECK_EQ(apply(&state, text_event("A", 1, 0), &action), OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_REDRAW);
    CHECK_EQ(apply(&state, text_event(cent, sizeof cent, 0), &action), OI_OK);
    CHECK_EQ(apply(&state, text_event("B", 1, 0), &action), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_LEFT), &action), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_BACKSPACE), &action),
             OI_OK);
    CHECK_STREQ(oi_cli_editor_data(&state.editor), "AB");
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_DELETE), &action), OI_OK);
    CHECK_STREQ(oi_cli_editor_data(&state.editor), "A");

    oi_cli_prompt_state_free(&state);
    oi_cli_input_history_free(&history);
}

TEST(ctrl_j_inserts_newline_and_enter_submits) {
    struct oi_cli_input_history history;
    struct oi_cli_prompt_state state;
    enum oi_cli_prompt_action action;
    char *submitted = NULL;
    size_t submitted_len = 0;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_prompt_state_init(&state, &history), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_ENTER), &action), OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_NONE);
    CHECK_EQ(apply(&state, text_event("one", 3, 0), &action), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_NEWLINE), &action),
             OI_OK);
    CHECK_EQ(apply(&state, text_event("two", 3, 0), &action), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_ENTER), &action), OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_SUBMIT);
    CHECK_EQ(oi_cli_prompt_state_commit(&state, &submitted, &submitted_len),
             OI_OK);
    CHECK_EQ(submitted_len, 7);
    CHECK_STREQ(submitted, "one\ntwo");
    CHECK_EQ(oi_cli_editor_length(&state.editor), 0);
    CHECK_EQ(history.len, 1);

    free(submitted);
    oi_cli_prompt_state_free(&state);
    oi_cli_input_history_free(&history);
}

TEST(history_navigation_restores_the_draft) {
    struct oi_cli_input_history history;
    struct oi_cli_prompt_state state;
    enum oi_cli_prompt_action action;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_input_history_append(&history, "old", 3), OI_OK);
    CHECK_EQ(oi_cli_input_history_append(&history, "new", 3), OI_OK);
    CHECK_EQ(oi_cli_prompt_state_init(&state, &history), OI_OK);
    CHECK_EQ(oi_cli_editor_set(&state.editor, "draft", 5), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_UP), &action), OI_OK);
    CHECK_STREQ(oi_cli_editor_data(&state.editor), "new");
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_UP), &action), OI_OK);
    CHECK_STREQ(oi_cli_editor_data(&state.editor), "old");
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_DOWN), &action), OI_OK);
    CHECK_STREQ(oi_cli_editor_data(&state.editor), "new");
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_DOWN), &action), OI_OK);
    CHECK_STREQ(oi_cli_editor_data(&state.editor), "draft");

    oi_cli_prompt_state_free(&state);
    oi_cli_input_history_free(&history);
}

TEST(paste_batches_redraw_and_keeps_control_bytes_as_text) {
    struct oi_cli_input_history history;
    struct oi_cli_prompt_state state;
    enum oi_cli_prompt_action action;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_prompt_state_init(&state, &history), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_PASTE_BEGIN), &action),
             OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_NONE);
    CHECK_EQ(apply(&state, text_event("a", 1, 1), &action), OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_NONE);
    CHECK_EQ(apply(&state, text_event("\n", 1, 1), &action), OI_OK);
    CHECK_EQ(apply(&state, text_event("b", 1, 1), &action), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_PASTE_END), &action),
             OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_REDRAW);
    CHECK_STREQ(oi_cli_editor_data(&state.editor), "a\nb");

    oi_cli_prompt_state_free(&state);
    oi_cli_input_history_free(&history);
}

TEST(ctrl_c_clears_and_ctrl_d_exits_only_when_empty) {
    struct oi_cli_input_history history;
    struct oi_cli_prompt_state state;
    enum oi_cli_prompt_action action;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_prompt_state_init(&state, &history), OI_OK);
    CHECK_EQ(apply(&state, text_event("x", 1, 0), &action), OI_OK);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_CTRL_D), &action), OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_REDRAW);
    CHECK_EQ(oi_cli_editor_length(&state.editor), 1);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_CTRL_C), &action), OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_REDRAW);
    CHECK_EQ(oi_cli_editor_length(&state.editor), 0);
    CHECK_EQ(apply(&state, simple_event(OI_CLI_INPUT_CTRL_D), &action), OI_OK);
    CHECK_EQ(action, OI_CLI_PROMPT_ACTION_EXIT);

    oi_cli_prompt_state_free(&state);
    oi_cli_input_history_free(&history);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_input_history history;
    struct oi_cli_prompt_state state;
    struct oi_cli_input_event event = simple_event(OI_CLI_INPUT_NONE);
    enum oi_cli_prompt_action action;
    char *text;
    size_t len;

    oi_cli_input_history_init(&history);
    CHECK_EQ(oi_cli_prompt_state_init(NULL, &history), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_prompt_state_init(&state, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_prompt_state_init(&state, &history), OI_OK);
    CHECK_EQ(oi_cli_prompt_state_apply(NULL, &event, &action), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_prompt_state_apply(&state, NULL, &action), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_prompt_state_commit(&state, &text, &len), OI_ERR_INVAL);
    event.type = OI_CLI_INPUT_TEXT;
    event.text_len = sizeof event.text + 1;
    CHECK_EQ(oi_cli_prompt_state_apply(&state, &event, &action),
             OI_ERR_INVAL);

    oi_cli_prompt_state_free(&state);
    oi_cli_input_history_free(&history);
}

int main(void) {
    RUN(editing_events_update_the_buffer);
    RUN(ctrl_j_inserts_newline_and_enter_submits);
    RUN(history_navigation_restores_the_draft);
    RUN(paste_batches_redraw_and_keeps_control_bytes_as_text);
    RUN(ctrl_c_clears_and_ctrl_d_exits_only_when_empty);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
