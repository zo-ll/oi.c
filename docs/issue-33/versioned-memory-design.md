# Versioned harness memory and context reuse — design

Issue #33 investigation. Research only: no implementation. Companion fact
file: `provider-caching-facts.md` (primary-source facts on OpenAI/Anthropic
prompt caching, fetched 2026-08-11).

Terminology follows `CONTEXT.md`: **session history** is the durable record;
**active context** is what is actually sent to the model for a turn; a
**context checkpoint** is a durable summary replacing an older prefix of
history in active context.

## 1. What oi already does (measured)

Measured on this repository at `fa9d5ef` with the real CLI against the
integration mock server (which captures the exact request bytes). Three
one-shot runs against a hand-built, codec-valid six-turn session log with
`shell` tool calls (generator: `measure_context.py`):

| Scenario | Request bytes | Messages | Notes |
| --- | ---: | ---: | --- |
| Fresh session, no history | 349 | 1 | 232B is the `shell` tool schema |
| Resume after 6 turns (25 context messages) | 4,334 | 26 | history is 93% of the request |
| Resume after `/compact` checkpoint (10 messages) | 2,104 | 11 | checkpoint cut the request to 48.5% |

Findings:

- **oi's fixed per-request overhead is small**: no system prompt is sent; the
  only constant cost is the tool schema (~232B for `shell`), which is 66% of a
  fresh one-shot request.
- **Context growth is linear in replayed history.** Each turn adds a user
  message, an assistant message, and per tool call a `tool_started` + tool
  result. Tool outputs are the largest messages (the largest here was 249B;
  they can reach `OI_CLI_HISTORY_MAX_CONTENT` in real sessions).
- **The existing `/compact` checkpoint is an effective reducer**: at six turns
  it halved the request. The gap widens as the conversation grows, because the
  checkpoint collapses a prefix into one summary message.
- `--dry-run` does **not** load the session, so it cannot be used as a context
  preview; the measurements above used the real request path.

Token estimates (rough, 4 bytes/token): fresh ≈ 87 tokens, six-turn resume ≈
1,083 tokens, compacted ≈ 526 tokens. These are byte-derived estimates; the
design below treats byte counts as the stable unit.

## 2. Taxonomy of memory and caching mechanisms

Three distinct mechanisms, kept separate because they have different costs,
different invalidation rules, and different trust:

| Mechanism | What it reduces | Who does it | Lifetime |
| --- | --- | --- | --- |
| Provider prompt/prefix caching | Monetary + latency on *re-sending identical tokens* | OpenAI/Anthropic | Seconds to hours (vendor TTL) |
| Context compaction (`/compact`) | *Logical context size* — what the model sees | Harness (already in oi) | Until the next checkpoint |
| Harness-side durable memory (this issue) | *Re-discovery* — rereading files, re-running commands, replaying old output | Proposed | Persistent, keyed by source/version |

Provider caching and compaction are **not** substitutes for durable memory:
caching does not give the model durable knowledge (facts must still be
re-sent to be seen), and compaction only reshapes what the model sees this
turn. Durable memory is the only one that changes what the model *needs* to
see. (See `provider-caching-facts.md` O2/A2: exact-prefix match; the cached
prefix must still be sent every request.)

## 3. Where oi wastes tokens today (measured + reasoned)

From the measurements and the code:

1. **Replayed tool output** — every resumed turn re-sends all prior tool
   outputs verbatim. In a long debugging session the same `ls`/`grep`/test
   output is replayed turn after turn, and again on every resume, even though
   the model already answered from it.
2. **Replayed user/assistant exchange** — prior turns are re-sent for
   continuity. Some is necessary (the current task), but old turns that have
   already been summarized or acted upon are replayed at full size.
3. **No system prompt / minimal tool schemas** — oi is already cheap here;
   the tool schema is the only fixed cost (232B).
4. **Command outputs that the model did not fully use** — the harness records
   the full tool result in history whether or not the model used it.

Quantified on the six-turn trace: history replay (4,102B) is 93% of the
request; tool outputs are 1,204B of that (29%). Across a whole day of
session use, resume-after-resume multiplies this.

## 4. Design target and constraints

`oi` is deliberately small, single-threaded, local-first, no new
dependencies, and "an arena per session, bump allocation, no indexing
platform". Any memory design must:

- add no database, index server, or new on-disk format beyond what exists
  (append-only session logs + metadata caches);
- keep the model's instructions and schemas stable so provider prefix
  caching keeps working (O4/A2: hash of the first ~256 tokens / fixed
  tools→system→messages order);
- never let a cached observation outrank current evidence or exact source
  (`REPL_PLAN.md` already encodes "replayed history outranks metadata");
- be understandable by one developer and testable with the existing
  pure/integration/fuzz tiers.

## 5. Candidate architecture A — minimal: provenance-tagged checkpoints

Extend the **existing** context-checkpoint mechanism with a source range and
reuse it as the only durable memory. No new storage, no new request shape.

- Every checkpoint (already durable in the session log) records its source
  range `[first_record_id, last_record_id]` — this already exists today.
- Add one field: the checkpoint records *which statements came from tool
  output vs. model conclusion* (a per-record provenance bit on the source
  range, or simply "the checkpoint summarizes records N..M", which is
  already true).
- On resume, active context = latest checkpoint + trailing exact records,
  exactly as today. The model can ask for exact evidence via a new built-in
  tool `recall` that reads the recorded source range from the durable log
  and returns the original tool outputs — exact, bounded, on demand.

Cost: ~one new tool + checkpoint provenance field. Benefit: the compacted
context stays small *and* exact evidence is recoverable without replaying
everything every turn.

## 6. Candidate architecture B — richer: version-keyed observation cache

A second tier: a per-session (or per-repository) cache of *observations*
keyed by version hashes, offered to the model as a small retrieval tool.

- **Keys** (provenance): repository commit ID (via `git rev-parse HEAD`),
  Git blob ID or content hash of each file read, command line, environment
  fingerprint (compiler, flags), and the producing tool.
- **Payloads**: prior tool outputs, test results keyed by `(commit, command,
  env)`, file listings.
- **Invalidation**: an observation is stale if its key's hash changes —
  file content hash for reads; `(commit, env, command)` for test results.
  Generated files and build flags are part of the key, not special-cased.
- **Discovery**: one tool `memory` with a tiny catalog (e.g., most recent 5
  observations per category), *not* a large index. The model requests
  entries by key; the harness returns exact stored bytes with their
  provenance, never a summary.
- **Trust**: stored observations are exact source evidence (tool stdout),
  never model-generated conclusions. A cached conclusion is explicitly out
  of scope for tier B.

## 7. Invalidation and provenance rules

For every cached object, both architectures follow the same rules:

1. **Exact evidence beats cached anything.** If a key's hash matches the
   current state, the stored bytes are the same bytes; if it does not match,
   the entry is dropped, never "updated by guesswork".
2. **Summaries are labeled summaries.** Anything model-generated (checkpoints)
   is visibly a summary with its source range; anything stored (tier B) is
   visibly raw tool output with its producing command.
3. **Freshness is derived from the key, not a clock.** No TTL heuristics;
   staleness is *deterministic*: hash mismatch.
4. **Cross-session/cross-repo boundary**: tier B is scoped per session by
   default (matching oi's per-session storage), never shared across repos;
   paths are stored relative to the session's working directory.
5. **Secrets**: tool output already passes through the session log's existing
   permission boundary; cached payloads inherit the log's `0600` files and
   `0700` directories (already enforced). No new exfiltration surface.

## 8. When exact source must replace a summary

The checkpoint's summary must be replaced by exact evidence when the model
is about to act on a claim the summary contains: a file path it will edit, a
test failure it will fix, a symbol it will rename. Rule of thumb: **the
summary is for orientation; the exact record is for action.** The `recall`
tool (A) or `memory` tool (B) returns exact bytes on demand, and the harness
never rewrites history based on a summary — matching the existing
`/compact` invariant that "the durable checkpoint append must fully succeed
before the live conversation is spliced" (`REPL_PLAN.md`).

## 9. Privacy and security boundaries

- Both tiers store only what already exists in session history; no new data
  is collected.
- Tier B's hash keys are derived from content; they reveal nothing beyond
  "this content changed".
- Cross-repository retrieval is prohibited by scoping keys to the session's
  working directory root.
- Command output may contain secrets (env, tokens in test output); tier B
  stores raw tool output with the same file permissions as the session log
  and never exposes it beyond the model request that asks for it.

## 10. Benchmark plan (quality-normalized)

The acceptance bar is "token savings evaluated alongside correctness", so:

1. **Task set** (from the issue): focused bug fix, cross-module feature,
   code review, failing-test diagnosis, continuation after compaction.
2. **Instrumentation**: count request bytes per turn and per tool result
   (already measurable via the mock capture path used above); count
   model-call turns; record task success and post-review defects.
3. **Cold vs warm**: cold = first request after a 5+ minute gap (no provider
   cache, no memory); warm = immediate continuation (provider cache hit on
   the stable prefix) — reported separately, as the issue requires.
4. **Comparison**: same model and reasoning config through (a) oi as-is,
   (b) oi + architecture A, (c) oi + architecture B. Report input bytes,
   cache-read vs cache-write tokens where exposed, wall time, task success,
   and follow-up review quality.
5. **Honesty guard**: a cached test result must not change the *outcome*
   reported by the suite; the benchmark runs the real suite for any
   correctness claim.

## 11. Recommendation

**Adopt architecture A (provenance-tagged checkpoints + a `recall` tool) as
a follow-up; do not build architecture B now.**

- A is a small delta over code that exists and is already tested
  (`cli_compact`, `cli_history_replay`'s checkpoint splice); B adds a new
  storage layer and a retrieval-tool contract that oi's "small, local-first"
  charter does not yet justify.
- The measurements show the biggest lever is replayed tool output and old
  turns — which is exactly what compaction + on-demand recall addresses
  (29% of replay bytes here, more in real sessions).
- Provider caching (facts file) already discounts the stable prefix to 0.1×
  (O/A pricing), so a second tier that merely *re-sends less* competes with
  a mechanism that is already automatic; A wins by changing *logical*
  context, which caching cannot do.
- Tier B's value would need the benchmark to prove rediscovery is still
  costly after A + provider caching; until then it is speculative weight.

Non-goals (explicit):

- No cache, index, database, or new persistence format now (per issue).
- No model-generated conclusion cache (staleness/false-confidence risk).
- No cross-repository memory.
- No change to the session-log format unless a follow-up issue approves the
  version bump.

## 12. Proposed follow-up issues (if approved)

1. Add source-range provenance to the checkpoint record and expose it via
   `/status` (small, testable; `cli_history_codec` + `cli_status`).
2. Implement the `recall` tool: given a record range, stream the original
   tool outputs from the durable log into the next request (new tool in
   `cli_tools.c`, permission policy unchanged).
3. Benchmark task set above against oi as-is vs. A; decide on B from data.

Each is a separate implementation issue per the issue's constraint; this
issue closes with the design only.
