# cli_tools: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_tools.c`, `src/cli_tools.h`.

## `oi_cli_tool_permission.OI_TOOL_ASK` (src/cli_tools.c)

Deferred to the embedder: cli_tools has no terminal I/O. An interactive caller (cli_repl.c) resolves this asynchronously via oi_cli_conversation_resolve_permission once a decision arrives through its own UI.

## `oi_cli_tool_permission_noninteractive.OI_TOOL_DENY` (src/cli_tools.c)

Never defers: a caller with no way to ever resolve a deferred ASK (the non-interactive one-shot loop, cli_loop.c) would stage a call and then hang forever waiting for a resolution nothing will ever supply -- deny outright instead, matching "ask without a usable controlling terminal denies safely rather than hanging."

## `oi_cli_tool_permission` (src/cli_tools.h)

Permission callback used by the interactive REPL: OI_CLI_TOOLS_ASK maps to OI_TOOL_ASK, deferring the actual decision to the embedder (resolved later via oi_cli_conversation_resolve_permission). cli_tools defines policy only and has no terminal I/O; requesting a decision is the REPL's job.

## `oi_cli_tool_permission_noninteractive` (src/cli_tools.h)

Permission callback used by the non-interactive one-shot loop (cli_loop.c): OI_CLI_TOOLS_ASK maps to OI_TOOL_DENY outright, since that caller has no way to ever resolve a deferred decision -- using oi_cli_tool_permission there would hang the process waiting for a resolution that will never come.
