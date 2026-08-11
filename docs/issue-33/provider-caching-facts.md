# Provider-side prompt/prefix caching — primary-source facts

Research date: 2026-08-11. All facts below were fetched directly from official vendor
documentation pages (HTML or the docs' `.md` export) via curl. Prices for models no longer
listed on the live OpenAI pricing page (`gpt-4o`, `gpt-4o-mini`) were taken from an
Internet Archive snapshot of the official OpenAI pricing page and are explicitly marked.

Primary sources consulted:

| Vendor | Page | URL |
|---|---|---|
| OpenAI | Prompt caching guide | https://platform.openai.com/docs/guides/prompt-caching |
| OpenAI | Pricing (live) | https://platform.openai.com/docs/pricing |
| OpenAI | Pricing (archived snapshot 2025-09-03, has gpt-4o rows) | https://web.archive.org/web/20250903020216/https://platform.openai.com/docs/pricing |
| OpenAI | Responses API reference | https://developers.openai.com/api/reference/resources/responses |
| Anthropic | Prompt caching guide | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| Anthropic | Pricing | https://docs.anthropic.com/en/docs/about-claude/pricing |
| Anthropic | Context windows | https://docs.anthropic.com/en/docs/build-with-claude/context-windows |
| Anthropic | Rate limits | https://docs.anthropic.com/en/api/rate-limits |

Checked and *not* useful: OpenAI "Counting tokens" guide and "Streaming" guide contain no
statement about caching (no explicit streaming–caching claim found in OpenAI docs at all).

---

## 1. How prompt caching works

| # | Fact | Source URL |
|---|---|---|
| O1 | OpenAI caching is **automatic** for eligible requests ("no code changes required") and is enabled for all recent models, `gpt-4o` and newer. | https://platform.openai.com/docs/guides/prompt-caching |
| O2 | Cache hits are only possible for **exact prefix matches** within a prompt; static content (instructions, examples, images, tools) must be at the beginning and identical between requests. | https://platform.openai.com/docs/guides/prompt-caching |
| O3 | Minimum cacheable prefix: automatic caching for prompts **≥ 1024 tokens**. For GPT-5.6 and later models 1,024 is a **strict minimum**; for GPT-5.5 and earlier the minimum "varies by model and can range from 1,024 to 2,048 tokens" and prompts just above 1,024 may not cache consistently. Requests under the minimum report `cached_tokens = 0`. | https://platform.openai.com/docs/guides/prompt-caching |
| O4 | Routing: requests are routed to a machine by a hash of the initial prefix, "typically the first 256 tokens, though the exact length varies depending on the model". Optional `prompt_cache_key` combines with the prefix hash to influence routing. | https://platform.openai.com/docs/guides/prompt-caching |
| O5 | Retention (GPT-5.6+): `prompt_cache_options.ttl` sets a **minimum cache lifetime**; the only supported value is **`30m`** (also the default). A cached prefix stays eligible for reuse at least 30 minutes; OpenAI may retain longer. | https://platform.openai.com/docs/guides/prompt-caching |
| O6 | Retention (pre-GPT-5.6): `prompt_cache_retention` — `in_memory`: generally active **5–10 minutes of inactivity, up to one hour**; `24h` (extended, KV tensors offloaded to GPU-local storage) for a fixed list: gpt-5.5, gpt-5.5-pro, gpt-5.4, gpt-5.2, gpt-5.1-codex-max, gpt-5.1, gpt-5.1-codex, gpt-5.1-codex-mini, gpt-5.1-chat-latest, gpt-5, gpt-5-codex, gpt-4.1. Non-ZDR orgs default to `24h`; ZDR orgs default to `in_memory`. | https://platform.openai.com/docs/guides/prompt-caching |
| O7 | GPT-5.6+ explicit breakpoints: `prompt_cache_breakpoint: {mode: "explicit"}` on a content block marks the exact end of the cached prefix; content after it may change without invalidating the earlier prefix. Modes: `implicit` (default — service places a breakpoint at the latest user/tool message *and* uses explicit breakpoints) vs `explicit` (only explicit breakpoints; disables the implicit one). | https://platform.openai.com/docs/guides/prompt-caching |
| O8 | GPT-5.6+ gotcha: with the implicit breakpoint at the latest user/tool message, if that block changes per request, `cached_tokens` can be 0 even when requests share thousands of identical tokens, and the changing prefix gets rewritten each time. Fix: explicit breakpoint at end of the stable prefix + shared `prompt_cache_key`. | https://platform.openai.com/docs/guides/prompt-caching |
| A1 | Anthropic caching is **opt-in**: you must add `cache_control` — either top-level (`{type: "ephemeral"}`) for *automatic caching* (breakpoint auto-placed on the last cacheable block, moved forward as conversations grow), or per-block `cache_control` for *explicit breakpoints*. It is not on by default. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A2 | Prefix order is fixed: **tools → system → messages**, "up to and including the block designated with cache_control". The cache hash is cumulative over the whole prefix. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A3 | Cache writes happen **only at breakpoints**; reads look *backward* (max **20 blocks** per breakpoint, counting the breakpoint itself) for entries prior requests wrote. The lookback finds prior writes, not stable content — it will not discover an unmarked static prefix behind a changing block. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A4 | TTL: **5 minutes by default** (ephemeral); optional **1-hour** TTL (`cache_control: {type: "ephemeral", ttl: "1h"}`) at higher write cost. Lifetime is measured from the **start** of the request that writes or reads the entry, not from the end of its response (e.g. a 4-min streamed response leaves ~1 min of the 5-min TTL for a follow-up). | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A5 | Minimum cacheable prefix per model (shorter prompts silently skip caching, no error): **512** tokens for Claude Opus 5 / Fable 5 / Mythos 5; **2,048** for Mythos Preview and Opus 4.7; **4,096** for Opus 4.6, Opus 4.5, Haiku 4.5; **1,024** for Opus 4.8, Sonnet 5, Sonnet 4.6, Sonnet 4.5, Opus 4.1, Opus 4, Sonnet 4; **2,048** for Haiku 3.5. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A6 | Max **4 cache breakpoints** per request. Adding breakpoints costs nothing extra; billing is per token actually written/read. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A7 | A cache entry becomes available only **after the first response begins** — for concurrent/parallel requests that need hits, wait for the first response before sending subsequent ones. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A8 | Storage/retention: KV representations + hashes are held **in memory only, not at rest**; minimum lifetime 5 min (or 1 h); cache isolated per organization and (Claude API, AWS, Foundry) per workspace; Bedrock/Google Cloud isolate per organization only. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |

## 2. Pricing (cache read vs cache write vs uncached input)

Reported multipliers:

| # | Fact | Source URL |
|---|---|---|
| O9 | OpenAI current (GPT-5.6 family): **cache reads = 0.1×** uncached input (90% off); **cache writes = 1.25×** uncached input. Cache writes have **no additional fee on models before the GPT-5.6 family**. | https://platform.openai.com/docs/guides/prompt-caching |
| A9 | Anthropic: **cache reads = 0.1×** base input; **5-minute cache write = 1.25×**; **1-hour cache write = 2×**. Multipliers stack with Batch API discount and data residency. "Caching pays off after one cache read for the 5-minute duration (1.25× write), or after two cache reads for the 1-hour duration (2× write)." | https://docs.anthropic.com/en/docs/about-claude/pricing |

OpenAI current list prices (live pricing page, standard tier, short context, per 1M tokens):

| Model | Input | Cached input | Cache writes | Output |
|---|---|---|---|---|
| gpt-5.6-sol | $5.00 | $0.50 | $6.25 | $30.00 |
| gpt-5.6-terra | $2.00 | $0.20 | $2.50 | $12.00 |
| gpt-5.6-luna | $0.20 | $0.02 | $0.25 | $1.20 |

Source: https://platform.openai.com/docs/pricing

OpenAI legacy models — **from archived official pricing page (snapshot 2025-09-03)**; no longer
listed on the live page, so treat as historical:

| Model | Input | Cached input | Cache writes | Output |
|---|---|---|---|---|
| gpt-4o | $2.50 | $1.25 (0.5×) | n/a (no write fee then) | $10.00 |
| gpt-4o-mini | $0.15 | $0.075 (0.5×) | n/a | $0.60 |
| gpt-4.1 | $2.00 | $0.50 (0.25×) | n/a | $8.00 |
| gpt-5 | $1.25 | $0.125 (0.1×) | n/a | $10.00 |

Source: https://web.archive.org/web/20250903020216/https://platform.openai.com/docs/pricing
(⚠ archived snapshot, not the live page)

Anthropic list prices (prompt-caching guide table; pricing page agrees):

| Model | Base input | 5m write | 1h write | Cache hit/read | Output |
|---|---|---|---|---|---|
| Claude Opus 5 / 4.8 / 4.7 / 4.6 / 4.5 | $5.00 | $6.25 | $10.00 | $0.50 | $25.00 |
| Claude Sonnet 5 | $2.00 | $2.50 | $4.00 | $0.20 | $10.00 |
| Claude Sonnet 4.6 / 4.5 | $3.00 | $3.75 | $6.00 | $0.30 | $15.00 |
| Claude Haiku 4.5 | $1.00 | $1.25 | $2.00 | $0.10 | $5.00 |

Source: https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching

## 3. Does caching reduce the *logical* context size, or only cost/latency?

| # | Fact | Source URL |
|---|---|---|
| A10 | **Anthropic, explicit:** "Cached prompt prefixes still occupy the context window: prompt caching changes what you pay for those tokens, not whether they count." Also: everything in the request counts toward the context window, and with caching the input count is split across `input_tokens`, `cache_read_input_tokens`, and `cache_creation_input_tokens` — "all three count toward the window". | https://docs.anthropic.com/en/docs/build-with-claude/context-windows |
| A11 | **Anthropic rate limits:** cache reads do **NOT** count toward the ITPM rate limit for most models (only `input_tokens` + `cache_creation_input_tokens` do). Exception: Claude Haiku 3.5 counts `cache_read_input_tokens` toward ITPM. Example given: with a 2,000,000 ITPM limit and 80% hit rate you can effectively process 10M input tokens/min. So caching does not reduce the context window but *does* raise effective throughput. | https://docs.anthropic.com/en/api/rate-limits |
| O10 | **OpenAI rate limits:** "Do cached prompts contribute to TPM rate limits? **Yes, as caching does not affect rate limits.**" Usage example shows cached tokens inside `prompt_tokens` (1,920 cached of 2,006 total) — i.e. cached tokens remain part of the request's token count. | https://platform.openai.com/docs/guides/prompt-caching |
| O11 | ⚠ **Gap:** I found no OpenAI statement (in the prompt-caching guide, pricing, responses reference, token-counting guide, or streaming guide) that explicitly says whether cached input reduces the logical context window. The FAQ's "caching does not affect rate limits" plus cached tokens being included in `prompt_tokens` implies caching does **not** reduce logical context consumption, but I could not verify an explicit sentence. Treat OpenAI's position as "caching = monetary/latency only" with that caveat. | — |

Net answer: caching is cost/latency optimization, **not** context-window expansion. The full
system prompt + tools + history must still fit the model's context window even when a prefix
is cached (explicit for Anthropic; strongly implied for OpenAI).

## 4. Compatibility: streaming, tools, concurrency, limits

| # | Fact | Source URL |
|---|---|---|
| O12 | OpenAI cacheable content: complete `messages` array (system/user/assistant), images in user messages (the `detail` parameter must be identical — it affects image tokenization), the `tools` list plus the messages array ("contributing to the model's minimum cacheable prefix length"), and the structured-output schema (serves as a prefix to the system message). | https://platform.openai.com/docs/guides/prompt-caching |
| O13 | OpenAI usage reporting: `cached_tokens` (reads) and `cache_write_tokens` (writes, GPT-5.6+) in Responses API `usage.input_tokens_details` / Chat Completions `usage.prompt_tokens_details`. | https://platform.openai.com/docs/guides/prompt-caching |
| O14 | OpenAI breakpoint limits: each request can create **up to 4 new cache writes**; implicit mode's latest-message breakpoint uses one slot (→ up to 3 explicit write slots); **up to the latest 50 breakpoints** are considered for reads. | https://platform.openai.com/docs/guides/prompt-caching |
| O15 | OpenAI concurrency: no explicit statement about concurrent requests. Relevant guidance: keep traffic per `prompt_cache_key` to **~15 requests/minute** (higher rates may miss); use more keys for volume and keep a stable key→prefix mapping. Maintain a steady stream of identical prefixes to minimize evictions. | https://platform.openai.com/docs/guides/prompt-caching |
| O16 | ⚠ OpenAI streaming: no explicit statement in current docs that caching composes with streaming. The guide says caching is automatic per request and that all requests report `cached_tokens`; I could not verify an explicit "works with streaming" sentence. (Anthropic does state it — see A12.) | https://platform.openai.com/docs/guides/prompt-caching |
| A12 | Anthropic streaming: cache usage fields are reported "within `usage` in the response (or `message_start` event **if streaming**)" — caching works with streaming. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A13 | Anthropic cacheable: tools array, system content blocks, text blocks in `messages.content` (user + assistant turns), images & documents (user turns), tool use and tool result blocks. Not directly cacheable: thinking blocks (they *are* cached as part of previous assistant turns and count as input tokens when read from cache), sub-content blocks like citations (cache the top-level document block instead), empty text blocks. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A14 | Anthropic invalidation matrix: changing **tool definitions** invalidates the *entire* cache (tools + system + messages); web-search toggle, citations toggle, and `speed` setting change the system prompt (invalidates system + messages); `tool_choice` changes invalidate message blocks only; adding/removing images anywhere affects message blocks; thinking configuration and `output_config.effort` invalidate message blocks always (and tool/system on some models). Stable key ordering in `tool_use` blocks is required (Swift/Go JSON key randomization can break caches). | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A15 | Anthropic pre-warming: `max_tokens: 0` requests write the cache and return immediately (empty content, `stop_reason: "max_tokens"`). Rejected with `stream: true`, extended thinking, structured outputs, or forced `tool_choice`. Pre-warm uses the same thinking/effort config as real traffic or the entry never matches. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A16 | Anthropic cache diagnostics (beta): API compares consecutive requests and reports where the prefix diverged — useful for debugging cache misses. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |

## 5. When cache hits are NOT possible

| # | Fact | Source URL |
|---|---|---|
| O17 | Exact byte/token-prefix match required: "Cache hits are only possible for exact prefix matches within a prompt"; images and tools "must be identical between requests". Any change in the prefix invalidates it. | https://platform.openai.com/docs/guides/prompt-caching |
| O18 | Below the minimum prefix length (1,024 tokens; 1,024–2,048 on pre-GPT-5.6 models) nothing is cached. | https://platform.openai.com/docs/guides/prompt-caching |
| O19 | GPT-5.6+ implicit breakpoint at the latest user/tool message: if the changing content (timestamps, tool-call history, user input) is inside the cached prefix, hits are impossible and the changing prefix is rewritten every request — even when thousands of earlier tokens are identical. Requires explicit breakpoint + `prompt_cache_key` for reliable matching. | https://platform.openai.com/docs/guides/prompt-caching |
| O20 | No `prompt_cache_key` on GPT-5.6+ ⇒ only the weaker automatic matching (hits still possible, "do not use the improved matching"). >~15 req/min per key ⇒ some requests miss. Caches are not shared between organizations. | https://platform.openai.com/docs/guides/prompt-caching |
| A17 | "Cache hits require **100% identical prompt segments**, including all text and images up to and including the block marked with cache control." Because the hash is cumulative, changing any block at or before a breakpoint produces a different hash. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A18 | Read-side lookback only finds entries that earlier requests *wrote at breakpoints*; if a growing conversation pushes the breakpoint ≥20 blocks past the last write, the lookback misses it (add a second breakpoint). A breakpoint on content that changes per request (e.g. a timestamp) produces a fresh write every request and never a read. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |
| A19 | Requests outside the TTL (5 min default / 1 h) miss. Minimum token thresholds per model (A5). Tool/system/message changes per invalidation matrix (A14). Cache not shared across organizations/workspaces. | https://docs.anthropic.com/en/docs/build-with-claude/prompt-caching |

---

## Implications for harness design (oi.c)

oi sends (a) a stable system prompt, (b) tool schemas, and (c) replayable session history on
every request — exactly the shape both vendors designed caching for. Facts that matter:

1. **Caching does not expand the context window.** oi's compaction / context-management logic
   must treat the full prompt (system + tools + history) as occupying the window regardless of
   cache hits — explicit per Anthropic (A10), implied per OpenAI (O10/O11). Caching only changes
   the bill and (Anthropic only) ITPM rate-limit consumption (A11).

2. **Prefix ordering is free performance.** Both vendors require the stable content to lead
   (O2, A2). oi already sends system prompt → tools → history; keep it that way and keep the
   system prompt and tool-schema text **byte-identical across requests** (O17, A17). Any
   schema drift (tool reordering, description edits, trailing whitespace) invalidates the whole
   prefix. Consider a schema-versioning/immutability rule.

3. **Minimum prefix thresholds matter for oi's typical sizes.** OpenAI: ≥1,024 tokens (O3);
   Anthropic: 512–4,096 by model (A5). A short system prompt + few tools may sit below the
   threshold, meaning a harness that replays history every turn gets *writes without reads*.
   Anthropic explicitly notes that padding cached content to reach the threshold "is often
   worthwhile" for frequently reused prompts — a relevant cost consideration for oi's
   system-prompt design (but note: padding also consumes context window, and on GPT-5.6+
   writes are billed at 1.25×, so padding only pays if it produces hits).

4. **Turn-level economics.** Each new turn rewrites only the delta after the breakpoint and
   reads the whole prefix:
   - Anthropic: read = 0.1×, 5m write = 1.25× (A9). oi requests within a turn must be kept
     within the 5-minute TTL (or use the 1h TTL at 2× write cost, A4) — long tool-execution
     pauses between oi's API calls can silently cost write-rate tokens on every call.
     Measuring from request start (A4) makes multi-minute tool calls even more TTL-hostile.
   - OpenAI pre-GPT-5.6: writes free, reads 0.5× (gpt-4o) or 0.1× (gpt-5) (O9). GPT-5.6+:
     writes 1.25×, reads 0.1×, implicit breakpoint at latest message (O7) — matches oi's
     "history + new user message" shape, but the per-key ~15 req/min ceiling (O15) is a real
     constraint for a multi-request-per-second agent loop; plan `prompt_cache_key` per session
     or per tenant.

5. **The Anthropic lookback-window trap is oi-shaped.** oi's growing history adds blocks per
   turn; if a turn adds ≥20 blocks (long tool results, big file dumps), the 20-block lookback
   misses the prior write (A3, A18). oi should place an explicit breakpoint at the end of the
   stable prefix (system + tools) so the history tail is always covered by the 5-minute
   write/refresh cadence, and/or add a second breakpoint on a stable early-history block.

6. **Never put volatile content before the breakpoint.** Timestamps, per-request state, or the
   incoming user message must live after the breakpoint (O8, A18) — otherwise every request is
   a full cache write and hits collapse to 0.

7. **Concurrency:** Anthropic cache entries exist only after the first response begins (A7) —
   if oi ever fires parallel requests sharing a cold prefix, only the first pays write; the
   others will miss unless serialized. OpenAI has no explicit statement; assume the same
   physical behavior (write completes as the first response starts).

8. **Observability:** both vendors expose read/write token splits in usage (O13, A12). oi
   should log `cached_tokens`/`cache_write_tokens` (OpenAI) and
   `cache_read_input_tokens`/`cache_creation_input_tokens` (Anthropic) and fail-loud or
   warn on a sustained 0% hit rate, since that indicates prefix drift or threshold misses.
   Anthropic's cache diagnostics beta (A16) is the debugging tool for this.

9. **Anthropic-specific: automatic caching is the low-effort default for growing
   conversations** (A1) — the breakpoint moves forward automatically; but for oi's static
   system/tools plus replayable history, an explicit breakpoint at the end of the stable
   prefix is more robust than automatic mode, and it composes with automatic caching (both use
   the 4-slot budget, A6).

10. **Verified-from-primary-source caveats:** OpenAI streaming–caching compatibility (O16) and
    OpenAI context-window accounting (O11) could not be confirmed from a primary source;
    gpt-4o/gpt-4o-mini prices are from an archived 2025-09-03 snapshot of the official pricing
    page, since the live page no longer lists those models. Model lists, prices, and TTL values
    here are as documented on 2026-08-11 and will drift — re-verify before relying on any
    specific number in the design.
