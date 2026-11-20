#ifndef OI_CLI_TOOL_PANEL_H
#define OI_CLI_TOOL_PANEL_H

#include <stddef.h>

#include "cli_bytebuf.h"
#include "cli_message.h"
#include "cli_render.h"
#include "cli_render_sanitize.h"
#include "cli_utf8_stream.h"
#include "oi/status.h"

#define OI_CLI_TOOL_PANEL_MAX_BYTES 4096u
#define OI_CLI_TOOL_PANEL_MAX_LINES 6u
#define OI_CLI_TOOL_PANEL_NAME_MAX 128u
#define OI_CLI_TOOL_PANEL_HEADER_MAX 160u

enum oi_cli_tool_panel_status {
    OI_CLI_TOOL_PANEL_RUNNING = 0,
    OI_CLI_TOOL_PANEL_COMPLETED,
    OI_CLI_TOOL_PANEL_FAILED,
    OI_CLI_TOOL_PANEL_DENIED,
    OI_CLI_TOOL_PANEL_CANCELLED
};

struct oi_cli_tool_panel {
    struct oi_cli_string tool_call_id;
    char name[OI_CLI_TOOL_PANEL_NAME_MAX];
    size_t name_len;
    enum oi_cli_tool_panel_status status;
    struct oi_cli_utf8_stream utf8;
    struct oi_cli_sanitize_state sanitize;
    struct oi_cli_bytebuf tail;
    char header[OI_CLI_TOOL_PANEL_HEADER_MAX];
    size_t header_len;
    int active;
};

void oi_cli_tool_panel_init(struct oi_cli_tool_panel *panel);
void oi_cli_tool_panel_free(struct oi_cli_tool_panel *panel);

oi_status oi_cli_tool_panel_start(struct oi_cli_tool_panel *panel,
                                  const char *tool_call_id,
                                  size_t tool_call_id_len, const char *name,
                                  size_t name_len);
int oi_cli_tool_panel_matches(const struct oi_cli_tool_panel *panel,
                              const char *tool_call_id,
                              size_t tool_call_id_len);
oi_status oi_cli_tool_panel_feed(struct oi_cli_tool_panel *panel,
                                 const void *data, size_t len);
void oi_cli_tool_panel_finish(struct oi_cli_tool_panel *panel,
                              enum oi_cli_tool_panel_status status);
void oi_cli_tool_panel_clear(struct oi_cli_tool_panel *panel);

size_t oi_cli_tool_panel_lines(struct oi_cli_tool_panel *panel,
                               struct oi_cli_render_line *out_lines,
                               size_t max_lines);

#endif
