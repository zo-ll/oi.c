#include "oi/arena.h"
#include "test.h"

#include <stdalign.h>
#include <stdint.h>
#include <string.h>

TEST(create_destroy) {
    oi_arena *a = oi_arena_create(0);
    CHECK(a != NULL);
    oi_arena_destroy(a);
    oi_arena_destroy(NULL); /* NULL-safe */
}

TEST(alloc_rejects_bad_args) {
    oi_arena *a = oi_arena_create(64);
    CHECK(oi_arena_alloc(NULL, 8) == NULL);
    CHECK(oi_arena_alloc(a, 0) == NULL);
    CHECK(oi_arena_alloc(a, 65) == NULL); /* exceeds block_size */
    oi_arena_destroy(a);
}

TEST(alloc_returns_usable_memory) {
    oi_arena *a = oi_arena_create(0);
    char *p = oi_arena_alloc(a, 32);
    CHECK(p != NULL);
    memset(p, 0xAB, 32);
    for (int i = 0; i < 32; i++) {
        CHECK_EQ((unsigned char)p[i], 0xABu);
    }
    oi_arena_destroy(a);
}

TEST(allocations_are_aligned) {
    oi_arena *a = oi_arena_create(0);
    size_t align = alignof(max_align_t);
    for (int i = 0; i < 50; i++) {
        /* Odd sizes deliberately misalign the bump pointer unless the
         * allocator rounds up correctly. */
        void *p = oi_arena_alloc(a, (size_t)(i % 7) + 1);
        CHECK(p != NULL);
        CHECK_EQ((uintptr_t)p % align, 0u);
    }
    oi_arena_destroy(a);
}

TEST(allocations_do_not_overlap) {
    oi_arena *a = oi_arena_create(0);
    enum { N = 200 };
    unsigned char *ptrs[N];
    size_t sizes[N];

    for (int i = 0; i < N; i++) {
        sizes[i] = (size_t)(i % 37) + 1;
        ptrs[i] = oi_arena_alloc(a, sizes[i]);
        CHECK(ptrs[i] != NULL);
        memset(ptrs[i], (i % 251) + 1, sizes[i]);
    }
    for (int i = 0; i < N; i++) {
        for (size_t j = 0; j < sizes[i]; j++) {
            CHECK_EQ(ptrs[i][j], (unsigned char)((i % 251) + 1));
        }
    }
    oi_arena_destroy(a);
}

TEST(alloc_spans_multiple_blocks) {
    /* Small block_size forces many block-growth transitions. */
    oi_arena *a = oi_arena_create(64);
    int count = 0;
    for (int i = 0; i < 100; i++) {
        void *p = oi_arena_alloc(a, 16);
        CHECK(p != NULL);
        count++;
    }
    CHECK_EQ(count, 100);
    oi_arena_destroy(a);
}

TEST(alloc_exactly_block_size_fits) {
    oi_arena *a = oi_arena_create(64);
    void *p = oi_arena_alloc(a, 64);
    CHECK(p != NULL);
    /* A second allocation of any size must roll to a new block, since
     * the first exactly filled the current one (post-alignment room may
     * be 0). */
    void *q = oi_arena_alloc(a, 1);
    CHECK(q != NULL);
    CHECK(q != p);
    oi_arena_destroy(a);
}

TEST(reset_reclaims_space) {
    oi_arena *a = oi_arena_create(0);
    void *p1 = oi_arena_alloc(a, 100);
    CHECK(p1 != NULL);
    CHECK(oi_arena_used(a) >= 100u);

    oi_arena_reset(a);
    CHECK_EQ(oi_arena_used(a), 0u);

    void *p2 = oi_arena_alloc(a, 100);
    CHECK(p2 != NULL);
    /* Reusing the same (now-empty) first block should hand back the same
     * address as before the reset. */
    CHECK(p2 == p1);
    oi_arena_destroy(a);
}

TEST(reset_reuse_cycle_reuses_all_blocks) {
    oi_arena *a = oi_arena_create(64);

    /* First cycle: force growth to several blocks. */
    void *first_addrs[10];
    for (int i = 0; i < 10; i++) {
        first_addrs[i] = oi_arena_alloc(a, 64);
        CHECK(first_addrs[i] != NULL);
    }

    oi_arena_reset(a);
    CHECK_EQ(oi_arena_used(a), 0u);

    /* Second cycle with the same allocation pattern must reuse the same
     * chain of blocks (same addresses) rather than mallocing anew. */
    for (int i = 0; i < 10; i++) {
        void *p = oi_arena_alloc(a, 64);
        CHECK(p != NULL);
        CHECK(p == first_addrs[i]);
    }

    oi_arena_destroy(a);
}

TEST(many_reset_cycles) {
    oi_arena *a = oi_arena_create(256);
    for (int cycle = 0; cycle < 1000; cycle++) {
        for (int i = 0; i < 20; i++) {
            void *p = oi_arena_alloc(a, 8);
            CHECK(p != NULL);
        }
        oi_arena_reset(a);
    }
    oi_arena_destroy(a);
}

TEST(used_tracks_across_growth) {
    oi_arena *a = oi_arena_create(64);
    CHECK_EQ(oi_arena_used(a), 0u);
    oi_arena_alloc(a, 32);
    oi_arena_alloc(a, 64); /* forces a new block */
    CHECK(oi_arena_used(a) >= 96u);
    oi_arena_destroy(a);
}

TEST(used_null_safe) { CHECK_EQ(oi_arena_used(NULL), 0u); }

int main(void) {
    RUN(create_destroy);
    RUN(alloc_rejects_bad_args);
    RUN(alloc_returns_usable_memory);
    RUN(allocations_are_aligned);
    RUN(allocations_do_not_overlap);
    RUN(alloc_spans_multiple_blocks);
    RUN(alloc_exactly_block_size_fits);
    RUN(reset_reclaims_space);
    RUN(reset_reuse_cycle_reuses_all_blocks);
    RUN(many_reset_cycles);
    RUN(used_tracks_across_growth);
    RUN(used_null_safe);
    return oi_test_report();
}
