# Handoff: GitHub Issue #21 — `/session` lifecycle and legacy import

Date: 2026-07-30

Repository: `zo-ll/oi.c`

Branch: `issue-21-session-lifecycle` (created off `main` at `21c61f6`, pushed to
`origin`). This is a deliberate deviation from how every prior REPL-milestone
issue (#20, #22–#27) was handled — those were all committed directly to
`main`. This one got its own branch because the user asked for the plan to be
staged somewhere safe before going to sleep, not because of any change in
project convention. **Ask the user whether to keep working on this branch and
merge to `main` via a PR at the end, or rebase onto `main` and continue the
direct-to-`main` habit instead** — this wasn't decided, don't assume either
way.

## What's actually done

Nothing implemented yet. This branch contains exactly two new files:

- `docs/PLAN_ISSUE_21.md` — the full implementation plan (research + a
  Plan-agent architecture pass, both already done; read it top to bottom
  before writing any code).
- `docs/HANDOFF_ISSUE_21.md` — this file.

No `src/`, `test/`, or `Makefile` changes exist on this branch yet. `main`
itself is exactly where issue #27 (`/compact`) left it — closed, merged,
verified, CI green.

## Standing instructions from the user (apply across this whole session, not just this issue)

- **Work through every open REPL-milestone issue (#20–#30) autonomously, in
  order, without re-confirming scope per issue.** The user said this
  explicitly early in the session and has reiterated it implicitly by saying
  "continue" whenever asked to resume. Order so far: #20, #22, #23, #24, #25,
  #26, #27 closed (in that order — not strictly numeric, #22/#30 were done
  before #20/#23). #21 is next (this issue). After #21: #28, #29 remain.
  #30 (macOS PTY support) was already folded into the `macos-support` branch
  split, not part of this remaining list.
- **Never attribute a commit to Claude/Anthropic** — no `Co-Authored-By:
  Claude`, no mention in commit messages. This project's own memory has a
  standing note on this; violating it is the single most likely thing to
  upset the user if missed.
- **Never use or suggest paid CI services.** The repo is public specifically
  so GitHub Actions minutes are free and unlimited; don't reintroduce
  anything that would push back toward needing a paid plan (e.g. don't
  suggest self-hosted runners as a "solution" to anything, don't suggest
  macOS CI back on `main`).
- **PTY test race class** (hit twice already, in #25 and #27): when a test
  writes one input, then immediately writes a second input meant to land
  while something is still busy/in-flight, the second write must wait for an
  explicit signal that the first input was actually processed (e.g. the
  echoed `"\r\n"` after a submitted line) — not just be sent immediately
  after the first `write_interactive()` call. Two back-to-back writes can
  arrive in the same `read()` on the child's side and get decoded before the
  first one has actually flipped any "busy" state, so the second one is
  silently treated as ordinary typing into an idle composer instead of being
  queued. See `queued_command_while_busy_resolves_discarded_and_dispatches_live`
  in `test/test_cli.c` for the correct idiom, and
  `compact_typed_while_a_turn_is_active_queues_and_runs_next` (added in #27)
  for a case where getting this wrong caused a real, confusing test failure
  before the fix.
- **Fuzz corpus gotcha** (my own mistake in #27, worth not repeating): when
  manually smoke-testing a fuzz harness binary with `-runs=N <dir>`, **never**
  pass the committed seed-corpus directory (`test/fuzz/corpus/<name>/`)
  directly as libFuzzer's own corpus argument — it writes every newly
  discovered "interesting" input straight into that directory, which then
  shows up as hundreds of untracked files in `git status`. Always point
  manual smoke-test runs at a scratch directory instead (or just use `make
  fuzz-run`, which already does this correctly via `build-fuzz/corpus/`).
- **Established per-issue workflow** (followed for every issue so far, keep
  following it): research via an Explore agent (or direct greps for a small
  issue), an architecture pass via a Plan agent for anything non-trivial,
  `EnterPlanMode` to get the plan approved by the user before writing code,
  then implement as small single-concern commits — each one built and run
  through `make check` (both `CC=gcc` and `CC=clang`) before moving to the
  next, pushed individually, with CI confirmed green via the `Monitor` tool
  (poll `gh run list --repo zo-ll/oi.c --limit 5 --json headSha,status,
  conclusion` matching on the short SHA prefix). Commits with real
  lifetime/concurrency risk or durable-mutation-ordering (this issue has at
  least two: the session-switch struct-move/ownership-transfer commit, and
  the `cli_repl.c`/`cli.c` wiring that destroys/recreates the live
  conversation) get a dedicated `make asan && make ubsan && make valgrind`
  beyond the standard per-commit `make check`. At the very end of the issue:
  a full verification pass (`make check` both compilers, `make asan`, `make
  ubsan`, `make tsan`, `make valgrind`, `make fuzz-run FUZZ_RUNS=200000`),
  then close the GitHub issue with a summary comment describing what shipped,
  any real bugs found along the way, testing added, and verification results
  — see the closing comments on issues #20/#22/#23/#24/#25/#26/#27 for the
  exact tone/format to match (`gh issue view <n> --repo zo-ll/oi.c --json
  title,number,state,body` and scroll its comments to see them, or just look
  at recent `gh issue list --repo zo-ll/oi.c --state closed` history).

## How to pick this up

1. Read `docs/PLAN_ISSUE_21.md` in full.
2. Re-verify its file:line references with fresh `grep`/`Read` calls before
   trusting them — they were spot-checked but not exhaustively re-derived
   (see the note at the top of that file).
3. If anything about the plan's approach seems wrong or the user wants
   changes, that's a normal `EnterPlanMode` revision — this plan hasn't been
   shown to the user for approval yet in a plan-mode session; it was written
   directly to a file because the user asked for exactly that ("develop the
   plan... set up a new branch... with the plan written in an md file").
   Whether to still run it through `EnterPlanMode`/`ExitPlanMode` for a
   formal approval step, or treat this document as already-approved and go
   straight to implementation, is the new agent's/user's call — ask if
   unsure.
4. Follow the plan's commit sequence (section 8 in `PLAN_ISSUE_21.md`),
   applying the standing per-commit verify/push/CI-check discipline above.
5. Full verification + GitHub closeout at the end, per the standing workflow.
6. Continue to #28, then #29, per the standing order — same "don't
   re-confirm scope" instruction applies to those too.

## Open questions the plan itself flags (see its "Open items to verify" section)

- Exact byte-level shape of `cli_session_metadata_codec.c`'s decode strictness
  (needs to relax specifically for the new optional `display_name` field
  without weakening rejection of genuinely unknown keys).
- Whether a helper already exists for rendering a restored message list
  (needed for the switch command's optional "show full replay" step).
- The exact legacy `.oilog` byte fixture already used by
  `test_cli_history_store.c`, reusable verbatim for the import test.
- Whether a cross-device (`EXDEV`) rename failure can be exercised in a real
  test in this environment at all, or should be code-reviewed only.
