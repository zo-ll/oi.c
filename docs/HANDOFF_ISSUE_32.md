# Handoff: issue #32 test-suite feedback time

Date: 2026-07-30

Repository: `zo-ll/oi.c`

Branch: `issue-32-test-feedback-time`

Current implementation head before this handoff:
`975df6786bc73d011cb86d551681c05a5b85f947`
(`Make the mock control channel actually synchronize`)

Pull request: [#35 — Measure and reduce test-suite feedback
time](https://github.com/zo-ll/oi.c/pull/35), currently a draft targeting
`main`.

Tracking issue: [#32 — Measure and reduce test-suite feedback
time](https://github.com/zo-ll/oi.c/issues/32).

## Read this first: current stopping point

Do not move on to redundant-compilation work yet. The response-hold conversion
in `test/test_cli.c` still has three review findings. Fix these in one focused
commit, run the focused and ordinary checks, push it, and wait for review
before beginning the next issue-#32 phase.

1. `ctrl_d_during_a_turn_has_no_effect` is still racy. It writes Ctrl+D and
   immediately releases the held response (`test/test_cli.c:1886-1891`).
   Writing to the PTY proves only that the byte was delivered to the kernel;
   it does not prove that the CLI processed it while the turn was busy. The
   response can win that race, allowing the test to pass without exercising
   the intended behavior. Keep the response held until there is an observable
   consequence proving the busy input loop continued after Ctrl+D. A small
   deterministic approach is to submit a visible follow-up while still held
   and wait for `oi: queued`, then release and verify normal completion and
   queued-turn behavior. Choose the smallest reliable assertion that proves
   Ctrl+D was processed mid-turn rather than merely written.

2. `slow_mock_server_terminate` accepts an arbitrary signal death. It sends
   `SIGTERM`, waits, and checks only the `WIFEXITED` case
   (`test/test_cli.c:482-489`). A server that already crashed with `SIGSEGV`,
   `SIGABRT`, or another signal therefore passes cleanup. Require either a
   clean exit with `SLOW_MOCK_EXIT_OK`, or `WIFSIGNALED(status)` with
   `WTERMSIG(status) == SIGTERM` when termination by this helper is expected.
   Check the result of `kill` carefully enough not to hide a real failure.

3. The helper contract still permits premature release. The comments explain
   that `slow_mock_wait_accepted` must precede `slow_mock_release`
   (`test/test_cli.c:218-270`), but the API does not enforce it. The new
   `mock_control_release_before_the_request_arrives_holds_nothing` test
   deliberately demonstrates the unsafe behavior
   (`test/test_cli.c:5567-5597`) instead of rejecting or making that ordering
   impossible. Track accepted-but-unreleased credits in
   `slow_mock_control`, or provide a combined/otherwise stateful API, so a
   future call site cannot silently create a no-op hold. Update the direct
   control-channel tests to assert the enforced contract rather than
   memorializing the hazard as allowed behavior.

The intended next commit is a test-harness correction only. Do not alter
production behavior, weaken assertions, add sleeps, or begin the compilation
archive work in the same commit.

## Ready-to-use prompt for the next Claude

> Work in `/home/andrea/personal/oi.c` on branch
> `issue-32-test-feedback-time`. Read `docs/HANDOFF_ISSUE_32.md` completely,
> inspect the current diff from `main`, and fix only the three unresolved
> response-hold review findings documented at the top of the handoff:
>
> 1. Make `ctrl_d_during_a_turn_has_no_effect` prove the CLI processed Ctrl+D
>    while the response was still held; do not release immediately after the
>    PTY write.
> 2. Make `slow_mock_server_terminate` reject every child termination except a
>    clean status-0 exit or the helper's intended `SIGTERM`.
> 3. Enforce accepted-before-release in the helper contract, and replace the
>    current test that treats premature release as permitted with a test of
>    the enforced behavior.
>
> Preserve fail-closed behavior, bounded waits, correct pipe ownership, and
> the runtime improvement. Do not add fixed sleeps. Keep this as one focused
> test-harness commit with no production-code changes. Run
> `make build/test_cli`, run `build/test_cli` repeatedly enough to probe the
> PTY race, run `make check`, and run `git diff --check`. Commit and push the
> result, but do not start the next #32 phase until it has been reviewed.
> Never add Claude/Anthropic attribution or a `Co-Authored-By` trailer.

## Issue #32 goal and measured baseline

Issue #32 exists because the complete suite had become too slow for the normal
edit-test loop. Its constraint is to reduce feedback time without deleting
coverage or weakening the pre-merge gate.

The baseline recorded on merged `main` at `df5058c`, on a 24-core machine,
was:

| Phase | Baseline |
| --- | ---: |
| Clean serial compilation | 19.2 s |
| Clean `make check`, serial | 71.3 s |
| Clean `make check -j24` | 52.7 s |
| `test_cli` alone | 34.4 s |
| Fixed sleeps inside `test_cli` | 28 s |

The first measurement also found that 44 of the test binaries together ran in
under one second, while `test_cli`, `test_tool_exec`, `test_llm`, and two
integration binaries dominated runtime. CLI source files were compiled about
300 times across 53 source files because individual test executables directly
link overlapping private CLI sources. Those measurements drove the current
order: establish tiers first, replace avoidable sleeps second, then address
redundant compilation.

## What has landed on this branch

The branch is five implementation commits ahead of `main`.

### `98b9a95` — Add quick/check/verify test tiers with a coverage audit

This introduced explicit pure and impure test lists
(`Makefile:212-240`). `make quick` builds the production CLI and runs only the
39 deterministic pure test binaries (`Makefile:245-251`). `make check` retains
all ordinary unit and integration coverage and now also runs `tier-audit`
(`Makefile:253-302`, `Makefile:335-339`).

`tier-audit` enforces that every ordinary test binary belongs to exactly one
tier. It detects missing tests, nonexistent names, duplicates inside a tier,
and overlap between tiers (`Makefile:267-302`).

`make verify` is the comprehensive pre-merge/release gate: full checks under
GCC and Clang, compiler-provenance validation, ABI checking, ASan, UBSan,
TSan, Valgrind, and bounded fuzzing (`Makefile:341-426`). `make timings`
performs a clean compile, times each tier and each binary, and propagates
build/test failures instead of producing a successful-looking report
(`Makefile:428-485`).

Initial measurements for the new fast tier were approximately 0.11 s to run
when built and 4.5 s after touching a source, versus about 57.5 s for the
then-current normal loop.

### `f9dcd8f` — Fix four holes in the test tiers

Review found four false-success paths and corrected them:

- `verify` originally reused one build tree for both compiler passes, so Make
  could rerun GCC-built binaries while claiming Clang coverage.
- `quick` originally did not build the production CLI, leaving private
  production-only sources unchecked.
- `tier-audit` originally checked coverage but not duplicate or overlapping
  membership.
- `timings` originally swallowed compilation and test failures.

The current implementations and rationale are documented directly in
`Makefile:245-302` and `Makefile:341-485`.

### `610179f` — Delete each compiler tree before its verify pass

Separate compiler trees were not sufficient: a stale `build-gcc/` populated by
Clang could remain timestamp-current, after which a stamp based on requested
`CC=gcc` falsely certified it. `verify` now deletes each compiler tree
immediately before rebuilding it, then writes the compiler stamp only after
the check completes (`Makefile:345-413`). Do not simplify this back to a
shared tree or parallel goals; the detailed comments record the reproduced
failure modes.

### `9768dd8` — Hold mock responses until released instead of sleeping

The slow mock server in `test/test_cli.c` used fixed per-turn sleeps to keep
requests in flight for cancellation and queued-input tests. This commit
replaced each delay with an accepted/release pipe protocol represented by
`slow_mock_turn.hold` and `slow_mock_control`
(`test/test_cli.c:188-216`). This reduced `test_cli` from 34.4 s to about
14.4 s and `make check` to about 29 s while retaining the then-existing 59
test blocks.

The first version was structurally promising but most call sites released
without first observing request acceptance, its timeout failed open, and the
child retained unused pipe ends.

### `975df67` — Make the mock control channel actually synchronize

This correction made acceptance waits bounded and failure-reporting
(`test/test_cli.c:218-264`), made the server's release wait fail closed, gave
control failures distinct exit codes, fixed pipe-end ownership, checked
expected clean server exits (`test/test_cli.c:464-490`), and changed held call
sites to wait for acceptance.

Cancellation and queueing tests now generally release after observing the
CLI-side effect (`oi: cancelled`, `oi: queued`, or process exit), not merely
after sending the input intended to cause it. Three direct control-channel
tests were added at `test/test_cli.c:5506-5635`. The focused binary now
contains 62 test blocks.

This commit is the current review target and is not approved because of the
three findings at the top of this document.

## Test-harness architecture

### Verification tiers

- `PURE_TESTS` owns tests without PTYs, sockets, forks, signals, or other
  process-global state (`Makefile:212-231`).
- `IMPURE_TESTS` owns the seven tests that do use those facilities
  (`Makefile:233-238`).
- `quick` builds all pure test binaries and the real CLI, then runs only the
  pure tests (`Makefile:240-251`).
- `check` retains the historical meaning of the complete ordinary suite:
  tier audit, all ordinary tests, and integration tests
  (`Makefile:242-243`, `Makefile:304-339`).
- `verify` is intentionally clean, serial across compiler/instrumentation
  passes, and expensive (`Makefile:341-426`).

The explicit tier lists are deliberate. Do not derive purity from filenames:
if a test gains a socket or process-global behavior, a maintainer should have
to notice and reclassify it.

### Held-response protocol

For each held turn, the mock child accepts the connection, drains the HTTP
request, writes one byte to the `accepted` pipe, and waits for one byte from
the `release` pipe before sending a response. The parent reads acceptance
notifications and writes release credits (`test/test_cli.c:188-270`).

The invariant is:

```text
submit request
    -> observe CLI submission if needed
    -> wait for server acceptance
    -> perform the action under test
    -> observe the action's CLI-side effect
    -> release the response
```

Merely writing a PTY byte or sending a signal is not an observation that the
CLI processed it. Likewise, seeing a rendered newline does not prove the HTTP
request reached the server. These distinctions caused real intermittent and
false-positive behavior during this work.

The child and parent close pipe ends they do not own so EOF represents a dead
peer. A timeout, EOF, short read, or read error on the release channel must
withhold the response and surface as a nonzero server exit. Preserve those
properties while enforcing accepted-before-release.

## Verification actually observed

At current head `975df67`:

- `make build/test_cli && build/test_cli` completed with
  `62 test blocks, 0 assertion failures`.
- The direct control-channel EOF test intentionally printed
  `[mock_server] turn 0 release channel hit EOF (read rc=0)` while passing.
- `git diff --check` passed.
- The worktree was clean before this handoff file was added.
- GitHub CI passed GCC, Clang, ASan, UBSan, TSan, and Valgrind for
  `975df67`.
- The fuzz job was still in progress at the last check. Recheck PR #35 rather
  than claiming it passed.
- Claude reported eight consecutive focused runs at 15.90–15.98 s and
  `make check` at 30.6 s. Treat those as the implementation author's reported
  measurements; the reviewing Codex independently observed the single
  62-block focused pass above.

The branch-wide diff before adding this handoff was 765 insertions and 96
deletions across `Makefile` and `test/test_cli.c`.

## Remaining issue #32 sequence

After the three current review findings are fixed and approved:

1. Measure and reduce redundant compilation. The issue suggests a private
   CLI-support test archive or another measured design, but the solution is
   not predetermined. Preserve production linkage behavior wherever it is
   relevant, and compare before/after compile and incremental rebuild times.
2. Diagnose the known `test_cli_composer` selector-resize flake. It has
   previously timed out waiting for the selector clear-to-end escape
   (`\x1b[J`) in `select_resize_redraws_the_selector`. Do not mask it with
   retries.
3. Audit collision risks before parallelizing anything: temporary paths,
   ports, PTYs, signals, environment variables, and process-global state.
   Parallelize only binaries proven independent.
4. Document the developer workflow and record final before/after timing data
   for compilation, every binary, `quick`, `check`, and `verify`.
5. Run final comprehensive verification, update PR #35 out of draft when
   genuinely complete, merge to `main`, and close issue #32 with measured
   results and any remaining limitations.

Do not bundle all of these into one commit. The branch so far uses
single-concern commits followed by review.

## Broader project context and ordering

Issue #21, complete `/session` lifecycle and legacy import, was merged to
`main` as PR #31 at `df5058c` and closed before this branch began. That stable
post-#21 tree is the baseline for issue #32.

Other open work currently includes:

- [#28 — Complete `/status` runtime, queue, and checkpoint
  reporting](https://github.com/zo-ll/oi.c/issues/28)
- [#29 — Document the interactive REPL, sessions, commands, and
  installation](https://github.com/zo-ll/oi.c/issues/29)
- [#30 — Complete the REPL verification matrix and release
  gate](https://github.com/zo-ll/oi.c/issues/30)
- [#33 — Investigate versioned harness memory and context
  reuse](https://github.com/zo-ll/oi.c/issues/33)
- [#34 — Design a suckless-style patch model for
  oi](https://github.com/zo-ll/oi.c/issues/34)

#33 is investigation/design only: compare the same model and tasks across
harnesses, distinguish provider prefix caching from actual context reduction,
measure cold and warm runs, and design provenance/invalidation rules for
compact retrievable memory. Do not implement a cache under that issue.

#34 is also design only and explicitly comes after #32. Its aim is a
suckless-style source-patch workflow without a runtime plugin ABI, pervasive
feature `#ifdef`s, or a package manager.

The exact ordering among #28–#30, #33, and #34 after #32 has not been firmly
settled in this handoff. Do not infer one; ask the user when #32 is finished.

The user wants `oi` to remain substantially smaller and more understandable
than large agent harnesses, but expects to add capabilities over time.
Minimality means a small coherent core and deliberate extension mechanisms,
not permanently freezing the feature set. The user also considered whether
the CLI should remain C; no rewrite was authorized, and the C core is expected
to remain regardless. Do not begin a language migration without an explicit
future decision.

## Standing collaboration constraints

- The user has Claude implement individual commits and asks Codex to review
  them. Treat review findings as blockers until the reviewer explicitly
  approves the result.
- Never add `Co-Authored-By: Claude`, Anthropic attribution, or mention Claude
  in commit messages.
- Do not weaken tests or reduce coverage to improve timings.
- Do not replace deterministic synchronization with sleeps or retries.
- Do not start implementation for #33 or #34; both are currently
  investigation/design issues.
- Avoid polluting committed fuzz seed corpora during manual runs. Use the
  existing `make fuzz-run` flow or a scratch corpus directory.
- Before every commit, inspect `git status` and preserve unrelated user
  changes. Push focused commits individually to the current branch.

## Resume checklist

1. Fetch and check out `issue-32-test-feedback-time`.
2. Read this document completely.
3. Run `git status --short`, `git log --oneline -10`, and
   `git diff main...HEAD`.
4. Confirm PR #35 and its CI state with `gh pr checks 35`.
5. Apply only the three current response-hold corrections.
6. Run the focused binary repeatedly, `make check`, and `git diff --check`.
7. Commit and push the focused correction without attribution trailers.
8. Stop for Codex review before beginning redundant-compilation work.
