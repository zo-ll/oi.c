# Agent working rules: oi.c

## No code comments

Do not write code comments in C files, headers, the Makefile, or CI
YAML. `make comment-lint` (part of `make check`) fails when a file has
more comments than `tools/comment-baseline.txt` allows.

- Put the reason for a change in the commit message.
- Put a non-obvious decision in an ADR in `docs/adr/`. The
  `NNNN-<module>-comment-records.md` files hold the decisions that were
  comments.
- Make the code say what it does: names, types, small functions, tests.
- The baseline only goes down. After you delete comments, run
  `make comment-ratchet` and commit the new baseline.
- Use the `/delete-comment` skill to classify and remove the comments
  that remain.

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