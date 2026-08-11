#ifndef OI_TEST_H
#define OI_TEST_H

/*
 * Minimal header-only test harness. No external framework: a test file
 * defines TEST(name) blocks, each executed by RUN(name) from main(), with
 * CHECK/CHECK_EQ recording failures without aborting the run.
 */

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int oi_test_failures = 0;
static int oi_test_count = 0;
static const char *oi_test_current = "";

/*
 * Optional per-process watchdog for tests that fork children, own PTYs, or
 * wait on sockets. A child that regresses into a silent hang must fail the
 * run with the name of the test that was executing rather than stall CI
 * until the platform's own job timeout. Opt-in: call
 * oi_test_set_deadline(seconds) from main() before any RUN(). Pure tests
 * never arm it; the deadline is deliberately generous so slow
 * instrumented runs (valgrind, tsan) do not false-positive.
 */
static unsigned oi_test_deadline_seconds = 0;

/* Unused by pure tests, which never arm the watchdog; the attribute keeps
 * -Werror happy under both supported compilers without forcing a fake use. */
static void oi_test_deadline_handler(int signum) __attribute__((unused));
static void oi_test_set_deadline(unsigned seconds) __attribute__((unused));

static void oi_test_deadline_handler(int signum) {
    (void)signum;
    fprintf(stderr,
            "TIMEOUT: suite exceeded %u s while running test \"%s\"\n",
            oi_test_deadline_seconds, oi_test_current);
    _exit(124);
}

static void oi_test_set_deadline(unsigned seconds) {
    if (seconds == 0) {
        return;
    }
    oi_test_deadline_seconds = seconds;
    signal(SIGALRM, oi_test_deadline_handler);
    alarm(seconds);
}

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
