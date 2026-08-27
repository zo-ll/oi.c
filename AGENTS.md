# Agent working rules: oi.c

## Comments state invariants, never history

A comment must describe what the code does or why it must be that way —
something a reader cannot learn from the code itself. It must never
narrate process or provenance.

Banned in this repo:

- "added in a later commit", "this commit only ever…", "until a later
  commit wires up…" — commit history lives in `git log`, not source.
- "issue #N", "per issue N's module-boundary rule" — issue archaeology.
  If a rule matters, state the rule; if a boundary matters, say where the
  boundary is and what belongs to whom.
- "no longer", "anymore (since …)", "previously …" — current behavior
  only, with the reason that still holds.

Allowed: cross-references to living docs (`PLAN.md`, `docs/REPL_PLAN.md`),
and the smallest commit/issue reference when the intent is genuinely
untraceable without it.

Density model: `src/arena.c` and `src/reactor_*.c`. If a comment would
survive unchanged after the next feature, keep it; if it would need
editing, it belongs in the commit message.

## Smallest change that works

- Modify existing files before creating new ones. A new file needs a
  reason the existing 100 don't cover.
- No abstractions for a single use. No defensive branches for inputs the
  callers can't produce.
- Match existing naming, error convention, and ownership style. Watch the
  `_GNU_SOURCE` + Linux-only flags; don't add portability shims.

## Verify before finishing

- `make -j` then `make quick`; new or touched code must keep every test
  binary at 0 assertion failures.
- Run `abi-check` when touching public headers in `include/oi/`.
- Sanitizer/fuzz tiers only when behavior changed, not comment edits.