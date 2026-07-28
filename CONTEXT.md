# oi.c

Date: 2026-07-28

The shared language for the agent harness, its sessions, and its command-line
interaction modes.

## Language

**Interactive mode**:
A CLI interaction that accepts multiple user turns within one conversation.
_Avoid_: REPL, interactive session

**One-shot mode**:
A CLI interaction that processes one user input and then exits.
_Avoid_: Batch mode

**Session**:
A named, durable conversation whose prior completed turns are restored as
model-visible context when it is resumed.
_Avoid_: Transcript, interactive session

**Session ID**:
The bounded, portable ASCII identifier that names a session and its durable
storage; it never contains path separators or traversal components.
_Avoid_: Session name, log path

**Session history**:
The complete durable message sequence in a session, including user and assistant
messages, assistant tool calls, tool results, and compacted material.
_Avoid_: Chat transcript

**Active context**:
The context checkpoint and subsequent messages supplied to the model for the
next turn.
_Avoid_: Session history, context window

**Context checkpoint**:
A durable model-generated summary that replaces an older prefix of session
history in active context without deleting the original records.
_Avoid_: Truncation, transcript summary

**Input history**:
The prior user messages from the current session that are available for recall
while editing a new message.
_Avoid_: Command history, global history

**Turn**:
A user message and the resulting assistant and tool interactions, ending when
the assistant produces its final response.
_Avoid_: Request, prompt

**Interrupted turn**:
A turn that ended before a final assistant response. Its completed messages
remain in session history, and unresolved interactions are made explicit when
the session is resumed.
_Avoid_: Failed turn, discarded turn

**Partial response**:
Assistant text displayed before its message was interrupted. It remains in
session history for audit and replay but is excluded from active context.
_Avoid_: Assistant message, completed response

**Turn cancellation**:
An interruption requested by the user that stops the active turn while keeping
the interactive conversation open for further input.
_Avoid_: Exit, session cancellation

**Queued message**:
The single user message submitted while another turn is active and waiting to
become the next turn.
_Avoid_: Pending prompt, message queue

**Steering**:
Advancing a queued message at a safe operation boundary. A running tool may
finish, but no further model step or newly requested tool begins before the
queued message becomes the next turn.
_Avoid_: Immediate cancellation, concurrent turn

**Slash command**:
A local interactive-mode instruction beginning with `/`; it controls the CLI
and is not sent to the model as a user message.
_Avoid_: Tool, prompt

**Command menu**:
The filterable selector of available slash commands shown when `/` begins the
current input.
_Avoid_: Dropdown, tool menu

**Session model**:
The model selected for a session’s future turns and restored when that session
is resumed.
_Avoid_: Default model, global model

**Session working directory**:
The durable directory associated with a session and restored before that
session may run tools.
_Avoid_: Launch directory, session directory

**Permission policy**:
The process-wide rule—`ask`, `allow`, or `deny`—governing whether
model-requested tools may execute.
_Avoid_: Tool policy, session permissions
