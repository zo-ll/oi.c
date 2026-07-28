# Compact active context with manual checkpoints

Date: 2026-07-28

The CLI will compact context only when `/compact [N]` is invoked, preserving the
latest eight completed turns by default and replacing the older active-context
prefix with a persisted synthetic assistant summary. Original session-history
records remain authoritative and addressable by stable IDs, while each
checkpoint records its source range; this avoids silent context loss, does not
elevate summarized content to system authority, and leaves a future vector index
rebuildable without making semantic retrieval part of the REPL milestone.
