# tool: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/tool_exec.c`, `src/tool_internal.h`, `src/tool_registry.c`, `test/test_tool_exec.c`.

## `oi_tool_call.build_argv` (src/tool_exec.c)

staged for a call awaiting an ASK decision; consumed by spawn()

## `oi_tool_call.destroyed_flag` (src/tool_exec.c)

See the identical mechanism in oi_llm_conn/oi_llm_http_parser/ oi_llm_sse_parser: set before invoking on_output, so a reentrant oi_tool_call_cancel() from within it can signal back that `call` was freed out from under the still-running dispatch loop.

## `OI_TOOL_DRAIN_MS` (src/tool_exec.c)

How long stdout may stay open after the child exits. Only a background process that inherited the pipe keeps it open then, and the call must not wait for that process.

## `OI_TOOL_DRAIN_MAX_READS` (src/tool_exec.c)

Bounds the last read: a writer outside the process group survives the kill and can fill the pipe without end.

## `on_process_event` (src/tool_exec.c)

The embedder may use SIG_IGN/SA_NOCLDWAIT for SIGCHLD, causing the kernel to reap children automatically. The native process watcher still fires, but no exit status remains to collect.

On a failed start the call waits for end-of-file, bounded by its timeout.

## `terminate_process_group` (src/tool_exec.c)

After the collect, the PID is free and can name an unrelated process: only the group, which still holds any grandchildren, gets the signal.

The child creates a fresh process group before exec, so a tool cannot leave grandchildren running after its call is cancelled.

## `on_err_event` (src/tool_exec.c)

hard error reading the pipe: no exec-failure info available, fall through and close it like any other terminal case

## `oi_reactor_remove` (src/tool_exec.c)

n == 0 (EOF: exec succeeded, the CLOEXEC write end closed itself), or we just consumed the one failure notice, or a hard error: nothing more will ever arrive on this fd.

n == 0 (EOF) or a hard read error: treat both as "no more output is coming."

## `on_stdin_event` (src/tool_exec.c)

hard write error (e.g. EPIPE, the child closed stdin/exited): give up on further stdin writes and drop whatever's queued.

## `sigemptyset` (src/tool_exec.c)

cli_repl.c blocks SIGWINCH/SIGINT/SIGTERM/SIGHUP process-wide for its own signalfd; that block survives exec() and would otherwise be inherited permanently by this tool subprocess, silently breaking its own signal-awareness (e.g. Ctrl+C not working as expected for a pager or another interactive program it invokes). Unblock all four here, mirroring the setpgid reset already done for process-group membership.

## `oi_tool_call_close_stdin` (src/tool_exec.c)

still draining a queued write: defer the actual close until on_stdin_event finishes flushing it, so the child sees everything written before it sees EOF.

## `oi_tool_call_cancel` (src/tool_exec.c)

SIGKILL has already made termination inevitable.

## `oi_tool_registry_lookup` (src/tool_internal.h)

Seam between tool_registry.c (pure bookkeeping) and tool_exec.c (the fork/exec/reactor machinery), so tool_exec.c never touches the registry's internal struct layout directly.

OI_TOOL_INTERNAL_H

## `oi_tool_registry_lookup` (src/tool_registry.c)

Internal accessor used by tool_exec.c; not part of the public header since callers only ever need the schema string.

## `tool_execution_does_not_reap_unrelated_children.unrelated` (test/test_tool_exec.c)

Forked before any allocation: under valgrind the child runs its own leak check at _exit, and at -O2 the parent's heap pointers can live only in registers that the child's check cannot see.
