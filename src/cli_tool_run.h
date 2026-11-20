#ifndef OI_CLI_TOOL_RUN_H
#define OI_CLI_TOOL_RUN_H

#include <stddef.h>
#include <stdio.h>

int oi_cli_tool_run(const char *name, const char *arguments,
                    size_t arguments_len, FILE *out);

int oi_cli_tool_main(const char *name);

#endif
