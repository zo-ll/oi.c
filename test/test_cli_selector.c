#include "cli_selector.h"
#include "test.h"

#include <string.h>

static struct oi_cli_input_event make_event(enum oi_cli_input_event_type type) {
    struct oi_cli_input_event event;

    memset(&event, 0, sizeof event);
    event.type = type;
    return event;
}

static struct oi_cli_input_event make_text(char c) {
    struct oi_cli_input_event event = make_event(OI_CLI_INPUT_TEXT);

    event.text[0] = (unsigned char)c;
    event.text_len = 1;
    return event;
}

TEST(down_moves_selection_forward_and_wraps) {
    struct oi_cli_input_event down = make_event(OI_CLI_INPUT_DOWN);
    enum oi_cli_selector_action action;
    size_t selected = 0;

    CHECK_EQ(oi_cli_selector_apply(&down, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 1);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_REDRAW);

    CHECK_EQ(oi_cli_selector_apply(&down, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 2);

    CHECK_EQ(oi_cli_selector_apply(&down, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 0);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_REDRAW);
}

TEST(up_moves_selection_backward_and_wraps) {
    struct oi_cli_input_event up = make_event(OI_CLI_INPUT_UP);
    enum oi_cli_selector_action action;
    size_t selected = 0;

    CHECK_EQ(oi_cli_selector_apply(&up, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 2);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_REDRAW);

    CHECK_EQ(oi_cli_selector_apply(&up, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 1);
}

TEST(a_single_option_never_moves_or_redraws) {
    struct oi_cli_input_event up = make_event(OI_CLI_INPUT_UP);
    struct oi_cli_input_event down = make_event(OI_CLI_INPUT_DOWN);
    enum oi_cli_selector_action action;
    size_t selected = 0;

    CHECK_EQ(oi_cli_selector_apply(&up, 1, &selected, &action), OI_OK);
    CHECK_EQ(selected, 0);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_NONE);

    CHECK_EQ(oi_cli_selector_apply(&down, 1, &selected, &action), OI_OK);
    CHECK_EQ(selected, 0);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_NONE);
}

TEST(enter_confirms_the_current_selection_without_changing_it) {
    struct oi_cli_input_event enter = make_event(OI_CLI_INPUT_ENTER);
    enum oi_cli_selector_action action;
    size_t selected = 1;

    CHECK_EQ(oi_cli_selector_apply(&enter, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 1);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_CONFIRM);
}

TEST(escape_and_ctrl_c_both_cancel) {
    struct oi_cli_input_event escape = make_event(OI_CLI_INPUT_ESCAPE);
    struct oi_cli_input_event ctrl_c = make_event(OI_CLI_INPUT_CTRL_C);
    enum oi_cli_selector_action action;
    size_t selected = 1;

    CHECK_EQ(oi_cli_selector_apply(&escape, 3, &selected, &action), OI_OK);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_CANCEL);
    CHECK_EQ(selected, 1);

    CHECK_EQ(oi_cli_selector_apply(&ctrl_c, 3, &selected, &action), OI_OK);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_CANCEL);
    CHECK_EQ(selected, 1);
}

TEST(a_digit_key_jumps_to_that_option_and_confirms) {
    struct oi_cli_input_event two = make_text('2');
    enum oi_cli_selector_action action;
    size_t selected = 0;

    CHECK_EQ(oi_cli_selector_apply(&two, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 1);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_CONFIRM);
}

TEST(a_digit_beyond_option_count_is_ignored) {
    struct oi_cli_input_event nine = make_text('9');
    enum oi_cli_selector_action action;
    size_t selected = 0;

    CHECK_EQ(oi_cli_selector_apply(&nine, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 0);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_NONE);
}

TEST(multi_byte_text_is_never_treated_as_a_digit_shortcut) {
    struct oi_cli_input_event event = make_event(OI_CLI_INPUT_TEXT);
    enum oi_cli_selector_action action;
    size_t selected = 0;

    event.text[0] = '2';
    event.text[1] = 'x';
    event.text_len = 2;
    CHECK_EQ(oi_cli_selector_apply(&event, 3, &selected, &action), OI_OK);
    CHECK_EQ(selected, 0);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_NONE);
}

TEST(unrelated_keys_have_no_effect) {
    struct oi_cli_input_event left = make_event(OI_CLI_INPUT_LEFT);
    struct oi_cli_input_event backspace = make_event(OI_CLI_INPUT_BACKSPACE);
    enum oi_cli_selector_action action;
    size_t selected = 0;

    CHECK_EQ(oi_cli_selector_apply(&left, 3, &selected, &action), OI_OK);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_NONE);
    CHECK_EQ(selected, 0);

    CHECK_EQ(oi_cli_selector_apply(&backspace, 3, &selected, &action), OI_OK);
    CHECK_EQ(action, OI_CLI_SELECTOR_ACTION_NONE);
    CHECK_EQ(selected, 0);
}

TEST(bad_arguments_are_rejected) {
    struct oi_cli_input_event enter = make_event(OI_CLI_INPUT_ENTER);
    enum oi_cli_selector_action action;
    size_t selected = 0;

    CHECK_EQ(oi_cli_selector_apply(NULL, 3, &selected, &action), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_selector_apply(&enter, 3, NULL, &action), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_selector_apply(&enter, 3, &selected, NULL), OI_ERR_INVAL);
    CHECK_EQ(oi_cli_selector_apply(&enter, 0, &selected, &action),
             OI_ERR_INVAL);
    selected = 3;
    CHECK_EQ(oi_cli_selector_apply(&enter, 3, &selected, &action),
             OI_ERR_INVAL);
}

int main(void) {
    RUN(down_moves_selection_forward_and_wraps);
    RUN(up_moves_selection_backward_and_wraps);
    RUN(a_single_option_never_moves_or_redraws);
    RUN(enter_confirms_the_current_selection_without_changing_it);
    RUN(escape_and_ctrl_c_both_cancel);
    RUN(a_digit_key_jumps_to_that_option_and_confirms);
    RUN(a_digit_beyond_option_count_is_ignored);
    RUN(multi_byte_text_is_never_treated_as_a_digit_shortcut);
    RUN(unrelated_keys_have_no_effect);
    RUN(bad_arguments_are_rejected);
    return oi_test_report();
}
