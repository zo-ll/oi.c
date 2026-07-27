#ifndef OI_TEST_H
#define OI_TEST_H

/*
 * Minimal header-only test harness. No external framework: a test file
 * defines TEST(name) blocks, each executed by RUN(name) from main(), with
 * CHECK/CHECK_EQ recording failures without aborting the run.
 */

#include <stdio.h>
#include <string.h>

static int oi_test_failures = 0;
static int oi_test_count = 0;
static const char *oi_test_current = "";

#define TEST(name) static void test_##name(void)

#define RUN(name)                                                            \
    do {                                                                     \
        oi_test_current = #name;                                            \
        oi_test_count++;                                                     \
        test_##name();                                                       \
    } while (0)

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s: %s:%d: %s\n", oi_test_current,        \
                    __FILE__, __LINE__, #cond);                              \
            oi_test_failures++;                                              \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
#define CHECK_STREQ(a, b) CHECK(strcmp((a), (b)) == 0)

/* Call from the end of main() after all RUN()s; returns process exit code. */
static int oi_test_report(void) {
    fprintf(stderr, "%d test blocks, %d assertion failure%s\n", oi_test_count,
            oi_test_failures, oi_test_failures == 1 ? "" : "s");
    return oi_test_failures != 0;
}

#endif /* OI_TEST_H */
