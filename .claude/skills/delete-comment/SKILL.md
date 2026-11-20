---
name: delete-comment
description: Classify the code comments that remain in this repository (Tautology, Excuse, Stale, Decision Record), delete them, file issues for Excuses, move Decision Records into docs/adr/, and lower the comment-lint baseline. Use on "/delete-comment", "delete comments", or "lower the comment baseline".
---

# Delete comments

The repository bans code comments. `make comment-lint` compares each file with
`tools/comment-baseline.txt`. This skill removes the comments that the baseline
still allows, and then lowers the baseline.

## Scope

1. Read `tools/comment-baseline.txt`. Each line is `<count> <path>`.
2. If the user names files or modules, use only those. Otherwise, use every
   C file and header in the baseline.
3. The Makefile and `.github/workflows/*.yml` use `#` comments. The scripts
   below read C only. Classify those files by hand.

## Steps

1. Make sure that the working tree is clean. Make a branch from `main`.
2. Extract the comments:

   ```sh
   python3 .claude/skills/delete-comment/scripts/extract.py /tmp/comments.json
   ```

   Each record has `id`, `file`, `l1`, `l2`, `trail`, `text`, and `next`
   (the next code line).
3. Read every comment with its code. Add `cat` to each record, and add
   `issue` to each Excuse:

   | `cat` | Meaning | Action |
   |---|---|---|
   | `T` | Tautology: the code already says it. Header-guard labels, section dividers, argument-name labels, field labels. | Delete. |
   | `S` | Stale: it names old code, an issue number, "now", "no longer", or "used to". | Delete. |
   | `E` | Excuse: it explains a hack, a defensive branch for input that callers cannot make, duplicated code, a sleep or timing guess, or a missing feature. | Delete. File one issue for each `issue` key. |
   | `D` | Decision Record: an important decision that the code cannot show. | Move into an ADR. Delete. |

   Remove records for files that stay out of scope. A `D` record in
   `include/oi/` stays in the code, because it is the public API reference.
4. Apply:

   ```sh
   python3 .claude/skills/delete-comment/scripts/apply.py /tmp/comments.json docs/adr <first free ADR number> <YYYY-MM-DD>
   ```

   The script writes one `NNNN-<module>-comment-records.md` ADR for each
   module. If an ADR for that module exists, merge the new sections into it
   and delete the new file. Keep the existing number.
5. Verify. All results must pass:

   ```sh
   make -j && make quick
   make comment-ratchet && make comment-lint
   ```

   Every test binary must report `0 assertion failures`. Run `make abi-check`
   if a file in `include/oi/` changed.
6. File the Excuse issues with `gh issue create`. Use one issue for each
   `issue` key. List each `file:line` from the comment records and the comment
   text, and say what a correct fix is.
7. Commit the code, the ADRs, and `tools/comment-baseline.txt` together.

## Rules

- Do not add a comment to explain a deletion. Put the reason in the commit
  message.
- Do not raise a baseline count. `make comment-ratchet` refuses an increase.
- If a deletion makes code hard to read, make the code clearer: a better
  name, a named constant, or a small function.
