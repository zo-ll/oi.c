#ifndef OI_ARENA_H
#define OI_ARENA_H

#include <stddef.h>

#include "oi/status.h"

typedef struct oi_arena oi_arena;

oi_arena *oi_arena_create(size_t block_size);

void oi_arena_destroy(oi_arena *a);

void *oi_arena_alloc(oi_arena *a, size_t size);

void oi_arena_reset(oi_arena *a);

size_t oi_arena_used(const oi_arena *a);

#endif
