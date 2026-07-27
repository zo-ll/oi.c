#include "oi/arena.h"

#include <stdalign.h>
#include <stdlib.h>

#define OI_ARENA_DEFAULT_BLOCK_SIZE (64 * 1024)
#define OI_ARENA_ALIGN (_Alignof(max_align_t))

struct oi_block {
    struct oi_block *next;
    size_t capacity;
    size_t used;
    _Alignas(max_align_t) unsigned char data[];
};

struct oi_arena {
    struct oi_block *head;
    struct oi_block *current;
    size_t block_size;
};

static struct oi_block *block_alloc(size_t capacity) {
    struct oi_block *b = malloc(sizeof *b + capacity);
    if (b == NULL) {
        return NULL;
    }
    b->next = NULL;
    b->capacity = capacity;
    b->used = 0;
    return b;
}

static size_t round_up(size_t n, size_t align) {
    return (n + (align - 1)) & ~(align - 1);
}

oi_arena *oi_arena_create(size_t block_size) {
    if (block_size == 0) {
        block_size = OI_ARENA_DEFAULT_BLOCK_SIZE;
    }

    oi_arena *a = malloc(sizeof *a);
    if (a == NULL) {
        return NULL;
    }

    struct oi_block *first = block_alloc(block_size);
    if (first == NULL) {
        free(a);
        return NULL;
    }

    a->head = first;
    a->current = first;
    a->block_size = block_size;
    return a;
}

void oi_arena_destroy(oi_arena *a) {
    if (a == NULL) {
        return;
    }
    struct oi_block *b = a->head;
    while (b != NULL) {
        struct oi_block *next = b->next;
        free(b);
        b = next;
    }
    free(a);
}

/* Advances a->current past a full block, reusing an already-allocated
 * next block left over from before the last reset if one exists, and
 * only calling malloc when the chain needs to grow. */
static struct oi_block *advance_block(oi_arena *a) {
    if (a->current->next != NULL) {
        a->current = a->current->next;
        return a->current;
    }

    struct oi_block *nb = block_alloc(a->block_size);
    if (nb == NULL) {
        return NULL;
    }
    a->current->next = nb;
    a->current = nb;
    return nb;
}

void *oi_arena_alloc(oi_arena *a, size_t size) {
    if (a == NULL || size == 0 || size > a->block_size) {
        return NULL;
    }

    struct oi_block *b = a->current;
    size_t aligned = round_up(b->used, OI_ARENA_ALIGN);

    if (aligned > b->capacity || size > b->capacity - aligned) {
        b = advance_block(a);
        if (b == NULL) {
            return NULL;
        }
        aligned = 0; /* fresh or just-reset block */
    }

    void *ptr = b->data + aligned;
    b->used = aligned + size;
    return ptr;
}

void oi_arena_reset(oi_arena *a) {
    if (a == NULL) {
        return;
    }
    for (struct oi_block *b = a->head; b != NULL; b = b->next) {
        b->used = 0;
    }
    a->current = a->head;
}

size_t oi_arena_used(const oi_arena *a) {
    if (a == NULL) {
        return 0;
    }
    size_t total = 0;
    for (struct oi_block *b = a->head; b != NULL; b = b->next) {
        total += b->used;
    }
    return total;
}
