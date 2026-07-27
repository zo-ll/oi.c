#ifndef OI_ARENA_H
#define OI_ARENA_H

#include <stddef.h>

#include "oi/status.h"

/*
 * Bump allocator over growable blocks, one arena per session. Allocations
 * within a turn are never individually freed; oi_arena_reset bulk-frees
 * everything allocated since the last reset (or creation) in one step,
 * amortizing away malloc/free overhead on the hot path.
 *
 * Not thread-safe: an arena must be used from one thread only, matching
 * the reactor's single-threaded model.
 */

typedef struct oi_arena oi_arena;

/*
 * `block_size` is the size of each underlying block the arena grows by
 * (and the largest single allocation it can ever satisfy); 0 selects a
 * reasonable default. Returns NULL on allocation failure or invalid args.
 */
oi_arena *oi_arena_create(size_t block_size);

/* NULL-safe. Frees every block owned by the arena. */
void oi_arena_destroy(oi_arena *a);

/*
 * Returns a pointer to `size` bytes, aligned to at least
 * `alignof(max_align_t)`, valid until the next oi_arena_reset or
 * oi_arena_destroy. Returns NULL if `size` is 0, exceeds the arena's
 * block_size, or a new block is needed and allocation fails.
 */
void *oi_arena_alloc(oi_arena *a, size_t size);

/*
 * Invalidates every pointer previously returned by oi_arena_alloc on `a`
 * and makes that memory available for reuse. Retains (does not free) the
 * arena's blocks, so a reset-then-reuse cycle does not re-hit malloc.
 */
void oi_arena_reset(oi_arena *a);

/* Total bytes currently handed out via oi_arena_alloc since the last
 * reset. Useful for tests/diagnostics, not required for correct use. */
size_t oi_arena_used(const oi_arena *a);

#endif /* OI_ARENA_H */
