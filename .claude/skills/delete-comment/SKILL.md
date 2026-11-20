---
name: delete-comment
description: >-
  Classify the code comments that remain in this repository as Tautology,
  Stale, Excuse, or Decision Record, then delete them, file an issue for each
  Excuse, move each Decision Record into an ADR, and lower the comment-lint
  baseline. Use on "/delete-comment", "delete comments", or "lower the comment
  baseline".
license: MIT
---

# Delete comment

Remove the comments that `tools/comment-baseline.txt` still allows. Keep each
decision in an ADR and each hack in an issue. Then lower the baseline.

**Why:** a comment can hide a hack, and the next change copies the hack. A
decision in an ADR and a hack in an issue stay visible after the code changes.

Read [references/categories.md](references/categories.md) before you classify.

## 1. Pick the scope

1. Make sure that the working tree is clean. Make a branch from `main`.
2. Read `tools/comment-baseline.txt`. Each line is `<count> <path>`.
3. Use the files or modules that the user names. Otherwise, use every file in
   the baseline.
4. The scripts read C only. Classify `#` comments in the Makefile and in
   `.github/workflows/*.yml` by hand.

## 2. Extract

```sh
python3 .claude/skills/delete-comment/scripts/extract.py /tmp/comments.json
```

Remove the records of files that are out of scope.

## 3. Classify

Read every comment with its code. Give each record a `cat`, and give each
Excuse an `issue` key. Do not classify from the first line only. If a rule
sorts a batch, say so in the report.

## 4. Apply

```sh
python3 .claude/skills/delete-comment/scripts/apply.py /tmp/comments.json docs/adr <first free ADR number> <YYYY-MM-DD>
```

The script deletes the comments and writes one
`NNNN-<module>-comment-records.md` ADR for each module. If an ADR for the module
exists, merge the new sections into it, delete the new file, and keep the
existing number. A Decision Record in `include/oi/` goes to `docs/API.md`
instead, under the section of its header. Read each new `docs/API.md` heading:
the symbol name comes from a script.

## 5. Prove it, then file the issues

1. Run `make -j && make quick`. Every test binary must report
   `0 assertion failures`.
2. Run `make abi-check` if a file in `include/oi/` changed.
3. Run `make comment-ratchet && make comment-lint`.
4. File one issue for each `issue` key with `gh issue create`. List each
   `file:line` on `main`, the comment text, and the correct fix.
5. Commit the code, the ADRs, and `tools/comment-baseline.txt` together.

## Invariants

- Do not add a comment to explain a deletion. Put the reason in the commit
  message.
- Do not raise a baseline count. `make comment-ratchet` refuses an increase.
- If a deletion makes code hard to read, make the code clearer: a better name,
  a named constant, or a small function.

## Related

- Use `small-units` to apply one module at a time.
- Use `prove-it` before you report the result.
- Use `why` when you cannot tell if a comment records a decision.
