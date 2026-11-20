# Comment categories

Read each comment with the code around it. Give it one category.

| `cat` | Category | Signs | Action |
|---|---|---|---|
| `T` | Tautology | The code already says it: a header-guard label, a section divider, an argument-name label, a field label, a test narration next to its `CHECK`. | Delete. |
| `S` | Stale | It names old code, an issue number, or a commit, or it says "now", "no longer", "used to", or "previously". | Delete. |
| `E` | Excuse | It explains a hack: a defensive branch for input that callers cannot make, duplicated code, a sleep or a timing guess, a missing feature, or a text copied next to each repeated step. | Delete. Give it an `issue` key. File one issue for each key. |
| `D` | Decision Record | It records an important decision that the code cannot show. | Move it into an ADR. Delete it. |

## Exceptions

- A `D` comment in `include/oi/` stays in the code. It is the public API
  reference.
- A comment that a compiler or a tool reads (for example, a fallthrough
  marker) stays until the code no longer needs it. Search for these before
  you apply.

## Record fields

`extract.py` writes one record for each comment block:

| Field | Meaning |
|---|---|
| `id` | Sequence number |
| `file`, `l1`, `l2` | Path, first line, last line |
| `trail` | True when code is before the comment on its line |
| `text` | The comment text, with markers |
| `next` | The next code line, or the code before a trailing comment |

Add `cat` to every record, and add `issue` to every `E` record.
