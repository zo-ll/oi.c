# arena: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/arena.c`, `test/test_arena.c`.

## `advance_block` (src/arena.c)

Advances a->current past a full block, reusing an already-allocated next block left over from before the last reset if one exists, and only calling malloc when the chain needs to grow.

## `alloc_oversize` (src/arena.c)

A request larger than one block gets a block of its own, linked at the head: blocks before a->current are never bumped again, so the current block keeps its free space. Only these blocks have a capacity above block_size, which is how oi_arena_reset finds them.

## `oi_arena_reset` (src/arena.c)

Oversize blocks go back to the system: one large allocation must not pin its memory for the life of the arena.

## `allocations_are_aligned.p` (test/test_arena.c)

Odd sizes deliberately misalign the bump pointer unless the allocator rounds up correctly.

## `alloc_spans_multiple_blocks.a` (test/test_arena.c)

Small block_size forces many block-growth transitions.

## `alloc_exactly_block_size_fits.q` (test/test_arena.c)

A second allocation of any size must roll to a new block, since the first exactly filled the current one (post-alignment room may be 0).

## `alloc_larger_than_a_block_gets_its_own_block.next` (test/test_arena.c)

The current block keeps its free space after the large request.

## `reset_reclaims_space` (test/test_arena.c)

Reusing the same (now-empty) first block should hand back the same address as before the reset.

## `reset_reuse_cycle_reuses_all_blocks` (test/test_arena.c)

Second cycle with the same allocation pattern must reuse the same chain of blocks (same addresses) rather than mallocing anew.
