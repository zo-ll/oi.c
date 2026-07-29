#include "cli_tool_panel.h"

#include <stdio.h>
#include <string.h>

static void trim_tail_to_bound(struct oi_cli_bytebuf *tail, size_t max_bytes) {
    size_t cut;
    size_t i;
    int found = 0;

    if (tail->len <= max_bytes) {
        return;
    }
    cut = tail->len - max_bytes;
    for (i = cut; i < tail->len; i++) {
        if (tail->data[i] == '\n') {
            cut = i + 1;
            found = 1;
            break;
        }
    }
    if (!found) {
        /* No newline anywhere in the excess -- one pathologically long
         * line; drop it all rather than leave something still over the
         * bound. */
        tail->len = 0;
        return;
    }
    memmove(tail->data, tail->data + cut, tail->len - cut);
    tail->len -= cut;
}

static const char *status_label(enum oi_cli_tool_panel_status status) {
    switch (status) {
    case OI_CLI_TOOL_PANEL_RUNNING:
        return "running";
    case OI_CLI_TOOL_PANEL_COMPLETED:
        return "completed";
    case OI_CLI_TOOL_PANEL_FAILED:
        return "failed";
    case OI_CLI_TOOL_PANEL_DENIED:
        return "denied";
    case OI_CLI_TOOL_PANEL_CANCELLED:
        return "cancelled";
    }
    return "unknown";
}

void oi_cli_tool_panel_init(struct oi_cli_tool_panel *panel) {
    memset(panel, 0, sizeof *panel);
}

void oi_cli_tool_panel_free(struct oi_cli_tool_panel *panel) {
    oi_cli_string_free(&panel->tool_call_id);
    oi_cli_bytebuf_free(&panel->tail);
}

oi_status oi_cli_tool_panel_start(struct oi_cli_tool_panel *panel,
                                  const char *tool_call_id,
                                  size_t tool_call_id_len, const char *name,
                                  size_t name_len) {
    struct oi_cli_sanitize_state name_sanitize;
    struct oi_cli_bytebuf sanitized_name = {0};
    oi_status status;
    size_t copy_len;

    if (panel == NULL || (tool_call_id == NULL && tool_call_id_len != 0) ||
        (name == NULL && name_len != 0)) {
        return OI_ERR_INVAL;
    }
    status = oi_cli_string_set(&panel->tool_call_id, tool_call_id,
                               tool_call_id_len);
    if (status != OI_OK) {
        return status;
    }
    oi_cli_utf8_stream_init(&panel->utf8);
    oi_cli_sanitize_init(&panel->sanitize);
    oi_cli_bytebuf_reset(&panel->tail);

    oi_cli_sanitize_init(&name_sanitize);
    status = oi_cli_sanitize_feed(&name_sanitize,
                                  (const unsigned char *)name, name_len,
                                  &sanitized_name);
    if (status == OI_OK) {
        status = oi_cli_sanitize_finish(&name_sanitize);
    }
    if (status != OI_OK) {
        oi_cli_bytebuf_free(&sanitized_name);
        return status;
    }
    copy_len = sanitized_name.len > sizeof panel->name - 1
                   ? sizeof panel->name - 1
                   : sanitized_name.len;
    if (copy_len > 0) {
        memcpy(panel->name, sanitized_name.data, copy_len);
    }
    panel->name[copy_len] = '\0';
    panel->name_len = copy_len;
    oi_cli_bytebuf_free(&sanitized_name);

    panel->status = OI_CLI_TOOL_PANEL_RUNNING;
    panel->active = 1;
    return OI_OK;
}

int oi_cli_tool_panel_matches(const struct oi_cli_tool_panel *panel,
                              const char *tool_call_id,
                              size_t tool_call_id_len) {
    return panel != NULL && panel->active &&
           panel->tool_call_id.len == tool_call_id_len &&
           (tool_call_id_len == 0 ||
            (tool_call_id != NULL &&
             memcmp(panel->tool_call_id.data, tool_call_id,
                    tool_call_id_len) == 0));
}

oi_status oi_cli_tool_panel_feed(struct oi_cli_tool_panel *panel,
                                 const void *data, size_t len) {
    struct oi_cli_bytebuf fixed = {0};
    oi_status status;

    status = oi_cli_utf8_stream_feed(&panel->utf8, data, len, &fixed);
    if (status == OI_OK) {
        status = oi_cli_sanitize_feed(
            &panel->sanitize, (const unsigned char *)fixed.data, fixed.len,
            &panel->tail);
    }
    oi_cli_bytebuf_free(&fixed);
    if (status == OI_OK) {
        trim_tail_to_bound(&panel->tail, OI_CLI_TOOL_PANEL_MAX_BYTES);
    }
    return status;
}

void oi_cli_tool_panel_finish(struct oi_cli_tool_panel *panel,
                              enum oi_cli_tool_panel_status status) {
    (void)oi_cli_utf8_stream_finish(&panel->utf8, &panel->tail);
    (void)oi_cli_sanitize_finish(&panel->sanitize);
    trim_tail_to_bound(&panel->tail, OI_CLI_TOOL_PANEL_MAX_BYTES);
    panel->status = status;
}

void oi_cli_tool_panel_clear(struct oi_cli_tool_panel *panel) {
    panel->active = 0;
    panel->status = OI_CLI_TOOL_PANEL_RUNNING;
    panel->name_len = 0;
    oi_cli_string_free(&panel->tool_call_id);
    oi_cli_bytebuf_reset(&panel->tail);
}

size_t oi_cli_tool_panel_lines(struct oi_cli_tool_panel *panel,
                               struct oi_cli_render_line *out_lines,
                               size_t max_lines) {
    size_t content_lines_wanted;
    size_t window_start = 0;
    size_t boundaries_found = 0;
    size_t line_count;
    size_t i;

    if (panel == NULL || out_lines == NULL || max_lines == 0) {
        return 0;
    }

    {
        int written =
            snprintf(panel->header, sizeof panel->header, "%.*s: %s",
                    (int)panel->name_len, panel->name,
                    status_label(panel->status));
        panel->header_len =
            written < 0 ? 0
            : (size_t)written >= sizeof panel->header
                ? sizeof panel->header - 1
                : (size_t)written;
    }
    out_lines[0].text = panel->header;
    out_lines[0].len = panel->header_len;
    line_count = 1;
    if (max_lines == 1) {
        return line_count;
    }
    content_lines_wanted = max_lines - 1;

    /* Find the start of the last content_lines_wanted lines by scanning
     * backward for that many newlines -- excluding a bare trailing '\n'
     * from consideration first (the common case, since every fed chunk
     * typically ends with one): it terminates the last line rather than
     * separating it from a line after it, so counting it here would
     * wrongly stop one line short of the one actually wanted. */
    if (panel->tail.len > 0) {
        size_t effective_len = panel->tail.len;

        if (panel->tail.data[effective_len - 1] == '\n') {
            effective_len--;
        }
        i = effective_len;
        while (i > 0) {
            i--;
            if (panel->tail.data[i] == '\n') {
                boundaries_found++;
                if (boundaries_found == content_lines_wanted) {
                    window_start = i + 1;
                    break;
                }
            }
        }
    }

    {
        size_t start = window_start;

        for (i = window_start;
             i < panel->tail.len && line_count < max_lines; i++) {
            if (panel->tail.data[i] == '\n') {
                out_lines[line_count].text = panel->tail.data + start;
                out_lines[line_count].len = i - start;
                line_count++;
                start = i + 1;
            }
        }
        if (line_count < max_lines && start < panel->tail.len) {
            out_lines[line_count].text = panel->tail.data + start;
            out_lines[line_count].len = panel->tail.len - start;
            line_count++;
        }
    }
    return line_count;
}
