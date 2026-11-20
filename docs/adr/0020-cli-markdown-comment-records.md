# cli_markdown: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/cli_markdown.h`, `src/cli_markdown_block.h`, `src/cli_markdown_inline.c`, `src/cli_markdown_inline.h`, `test/test_cli_markdown_inline.c`.

## `oi_cli_md_style_bits` (src/cli_markdown.h)

Flat inline style spans shared by the Markdown inline matcher, the block state machine, and the SGR renderer. BOLD/ITALIC may combine; CODE is mutually exclusive with both (a code span's interior is never further interpreted for emphasis).

## `oi_cli_md_block_style` (src/cli_markdown.h)

Which block construct produced a line handed to the style renderer. Shared between cli_markdown_block (the producer) and cli_render_style (the consumer) so neither depends on the other's header.

## `oi_cli_md_span_list_covers` (src/cli_markdown.h)

Debug/test invariant check: true iff runs are sorted by start, mutually non-overlapping, and together cover exactly [0, text_len) with no gaps.

## `OI_CLI_MARKDOWN_LINE_CAP` (src/cli_markdown_block.h)

Incremental block-level Markdown state machine: recognizes ATX headings, single-level list items, and fenced code blocks at line boundaries; ordinary lines are treated as one-line paragraphs. Consumes already UTF-8-fixed-up, control-sanitized bytes (never raw/untrusted at this layer) and drives cli_markdown_inline + cli_render_style to produce the final styled bytes.

Deliberate, documented simplification: a "paragraph" is exactly one line, resolved as soon as its terminating '\n' arrives (or at cap/ end-of-turn for the last unterminated line) -- not accumulated across multiple lines. This means an emphasis span that opens on one line and closes on a later line (a real but rare CommonMark construct) is not recognized; each line's inline content is matched independently. This keeps chunk-boundary handling simple: no cross-line lookahead is ever needed to decide whether a line continues a paragraph or starts a new block.

A line (of any kind, including fenced code content) is bounded at

OI_CLI_MARKDOWN_LINE_CAP bytes. Exceeding it flushes what's buffered as literal, unstyled text and continues accumulating the rest of that same line in a plain passthrough mode until its terminating '\n' -- bytes are never dropped, only structured interpretation is skipped for the overlong line.

## `oi_cli_markdown_block_finish` (src/cli_markdown_block.h)

End of turn: resolves and emits any still-buffered, un-terminated final line (paragraph, list item, heading, or fence content). Does not itself force a trailing style reset -- that is cli_render_stream's job.

## `classify_byte` (src/cli_markdown_inline.c)

ASCII whitespace and ASCII punctuation classification. Bytes >= 0x80 (any multi-byte UTF-8 sequence) classify as OTHER -- a documented simplification of CommonMark's full Unicode whitespace/punctuation tables, adequate for the agreed minimal subset.

## `classify_before` (src/cli_markdown_inline.c)

Start/end of the text counts as whitespace, per CommonMark.

## `resolve_code_spans` (src/cli_markdown_inline.c)

Resolves backtick code spans: a run of N backticks is closed by the next run of exactly N backticks later in the text. An unmatched run is literal and scanning continues after it. Marks both delimiter runs and everything between them (inclusive) as `guard` -- excluded from emphasis tokenizing -- and the interior alone as CODE-styled and stripped-delimiter.

## `match_emphasis` (src/cli_markdown_inline.c)

Stack-based emphasis matcher (CommonMark's "process emphasis"): walks filtered delimiter runs left to right, matching each closer against the nearest compatible still-open opener, honoring the multiple-of-3 rule. ORs BOLD/ITALIC onto `style` for the fixed interior byte range between a matched pair (the range never changes across repeated partial matches of the same pair, since run positions are fixed -- only how many of a run's own characters are stripped changes), and marks each run's final consumed characters in `stripped`.

## `oi_cli_md_delim_kind` (src/cli_markdown_inline.h)

Delimiter-run tokenizer and CommonMark-accurate emphasis/code-span matcher for one complete, bounded chunk of inline text (a paragraph, a heading's text, or a list item's text -- never a fenced code block, which has no inline interpretation at all).

Deliberate, documented simplification: a delimiter run longer than 3 characters can never open or close emphasis (real-world model output essentially never intends 4+ repeated '*'/'_' as nested emphasis; this avoids implementing arbitrary partial-run consumption beyond the 1/2/3-length cases CommonMark's own examples focus on).

## `oi_cli_md_delim_run.len` (src/cli_markdown_inline.h)

original run length, including any inert (>3) tail

## `oi_cli_markdown_tokenize` (src/cli_markdown_inline.h)

Tokenizes every maximal run of '*' or '_' in text[0,len) into a left-to-right ordered oi_cli_md_delim_list, classifying each run's can_open/can_close per CommonMark's left-/right-flanking rules and the intraword-underscore exception. Does not know about code spans -- the caller (oi_cli_markdown_inline_parse) is responsible for excluding runs that fall inside a resolved code span before matching.

## `oi_cli_markdown_inline_parse` (src/cli_markdown_inline.h)

Resolves code spans and CommonMark-accurate emphasis over one complete bounded chunk of inline text, producing plain output text (all `` ` ``/ `*`/`_` delimiters that were actually matched are stripped) plus a flat, non-overlapping span list over that output text (oi_cli_md_span_list_ covers holds for the result).

Known, documented simplification: a delimiter run can act as an opener for one closer and later (if leftover characters remain) as an opener again for a further closer, or close then reopen; when the exact same run is consumed from both its "closer" and "opener" role, this implementation strips consumed characters from each end independently (closer-consumed from the run's start, opener-consumed from its end) rather than tracking the precise chronological sub-position of each partial consumption. This only affects which literal punctuation character(s) survive in the rare case of such a hybrid partial match -- never emphasis correctness or memory safety.

## `parse` (test/test_cli_markdown_inline.c)

"foo*bar**baz*qux**e": the single '*' after "foo" and the '**' after "bar" are both "can open and close" (intraword-adjacent stars have no intraword restriction) with lengths 1 and 2 -- their sum (3) is a multiple of 3 but the lengths individually are not, so CommonMark's rule forbids that pairing. The single '*' instead matches the later single '*' after "baz", leaving the first '**' permanently enclosed (unmatched, literal) and the final '**' unmatched too.
