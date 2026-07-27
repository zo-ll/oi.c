#include "oi/session.h"
#include "test.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static char g_path[256];

static const char *fresh_log_path(void) {
    static int counter = 0;
    snprintf(g_path, sizeof g_path, "/tmp/oi_session_test_%d_%d.log",
             (int)getpid(), counter++);
    unlink(g_path);
    return g_path;
}

TEST(create_destroy_registry) {
    oi_session_registry *reg = oi_session_registry_create();
    CHECK(reg != NULL);
    oi_session_registry_destroy(reg);
    oi_session_registry_destroy(NULL); /* NULL-safe */
}

TEST(create_lookup_session) {
    oi_session_registry *reg = oi_session_registry_create();
    const char *path = fresh_log_path();

    oi_session *s;
    CHECK_EQ(oi_session_create(reg, "sess-1", path, 0, &s), OI_OK);
    CHECK(s != NULL);
    CHECK_STREQ(oi_session_id(s), "sess-1");
    CHECK_EQ(oi_session_state_of(s), OI_SESSION_ACTIVE);
    CHECK(oi_session_arena(s) != NULL);
    CHECK(oi_session_log(s) != NULL);

    CHECK(oi_session_lookup(reg, "sess-1") == s);
    CHECK(oi_session_lookup(reg, "nope") == NULL);

    oi_session_registry_destroy(reg);
    unlink(path);
}

TEST(duplicate_id_rejected) {
    oi_session_registry *reg = oi_session_registry_create();
    const char *path = fresh_log_path();
    oi_session *s1, *s2;
    CHECK_EQ(oi_session_create(reg, "dup", path, 0, &s1), OI_OK);
    CHECK_EQ(oi_session_create(reg, "dup", path, 0, &s2), OI_ERR_EXISTS);
    oi_session_registry_destroy(reg);
    unlink(path);
}

TEST(create_rejects_bad_args) {
    oi_session_registry *reg = oi_session_registry_create();
    const char *path = fresh_log_path();
    oi_session *s;
    CHECK_EQ(oi_session_create(NULL, "x", path, 0, &s), OI_ERR_INVAL);
    CHECK_EQ(oi_session_create(reg, NULL, path, 0, &s), OI_ERR_INVAL);
    CHECK_EQ(oi_session_create(reg, "x", NULL, 0, &s), OI_ERR_INVAL);
    CHECK_EQ(oi_session_create(reg, "x", path, 0, NULL), OI_ERR_INVAL);
    oi_session_registry_destroy(reg);
}

TEST(lookup_rejects_null_args) {
    oi_session_registry *reg = oi_session_registry_create();
    CHECK(oi_session_lookup(NULL, "x") == NULL);
    CHECK(oi_session_lookup(reg, NULL) == NULL);
    oi_session_registry_destroy(reg);
}

TEST(destroy_removes_from_registry) {
    oi_session_registry *reg = oi_session_registry_create();
    const char *path = fresh_log_path();
    oi_session *s;
    CHECK_EQ(oi_session_create(reg, "gone", path, 0, &s), OI_OK);
    CHECK(oi_session_lookup(reg, "gone") != NULL);

    oi_session_destroy(reg, s);
    CHECK(oi_session_lookup(reg, "gone") == NULL);

    /* The id is now free to reuse. */
    oi_session *s2;
    CHECK_EQ(oi_session_create(reg, "gone", path, 0, &s2), OI_OK);

    oi_session_registry_destroy(reg);
    unlink(path);
}

TEST(destroy_null_safe) {
    oi_session_registry *reg = oi_session_registry_create();
    oi_session_destroy(reg, NULL);
    oi_session_destroy(NULL, NULL);
    oi_session_registry_destroy(reg);
}

TEST(multiple_independent_sessions) {
    oi_session_registry *reg = oi_session_registry_create();
    char p1[256], p2[256], p3[256];
    snprintf(p1, sizeof p1, "/tmp/oi_session_multi_1_%d.log", (int)getpid());
    snprintf(p2, sizeof p2, "/tmp/oi_session_multi_2_%d.log", (int)getpid());
    snprintf(p3, sizeof p3, "/tmp/oi_session_multi_3_%d.log", (int)getpid());
    unlink(p1);
    unlink(p2);
    unlink(p3);

    oi_session *a, *b, *c;
    CHECK_EQ(oi_session_create(reg, "a", p1, 0, &a), OI_OK);
    CHECK_EQ(oi_session_create(reg, "b", p2, 0, &b), OI_OK);
    CHECK_EQ(oi_session_create(reg, "c", p3, 0, &c), OI_OK);

    oi_session_destroy(reg, b);

    CHECK(oi_session_lookup(reg, "a") == a);
    CHECK(oi_session_lookup(reg, "b") == NULL);
    CHECK(oi_session_lookup(reg, "c") == c);
    CHECK_EQ(oi_session_state_of(a), OI_SESSION_ACTIVE);
    CHECK_EQ(oi_session_state_of(c), OI_SESSION_ACTIVE);

    oi_session_registry_destroy(reg);
    unlink(p1);
    unlink(p2);
    unlink(p3);
}

/* --- fault containment --- */

TEST(fail_marks_failed_and_releases_resources) {
    oi_session_registry *reg = oi_session_registry_create();
    const char *path = fresh_log_path();
    oi_session *s;
    CHECK_EQ(oi_session_create(reg, "faulty", path, 0, &s), OI_OK);

    oi_session_fail(s);

    CHECK_EQ(oi_session_state_of(s), OI_SESSION_FAILED);
    CHECK(oi_session_arena(s) == NULL);
    CHECK(oi_session_log(s) == NULL);
    /* still in the registry -- fault containment doesn't remove it */
    CHECK(oi_session_lookup(reg, "faulty") == s);

    oi_session_registry_destroy(reg);
    unlink(path);
}

TEST(fail_is_idempotent) {
    oi_session_registry *reg = oi_session_registry_create();
    const char *path = fresh_log_path();
    oi_session *s;
    CHECK_EQ(oi_session_create(reg, "x", path, 0, &s), OI_OK);
    oi_session_fail(s);
    oi_session_fail(s); /* must not double-free */
    oi_session_fail(s);
    CHECK_EQ(oi_session_state_of(s), OI_SESSION_FAILED);
    oi_session_registry_destroy(reg);
    unlink(path);
}

TEST(fail_null_safe) { oi_session_fail(NULL); }

TEST(destroy_after_fail_is_clean) {
    oi_session_registry *reg = oi_session_registry_create();
    const char *path = fresh_log_path();
    oi_session *s;
    CHECK_EQ(oi_session_create(reg, "x", path, 0, &s), OI_OK);
    oi_session_fail(s);
    oi_session_destroy(reg, s); /* arena/log already NULL; must not crash */
    CHECK(oi_session_lookup(reg, "x") == NULL);
    oi_session_registry_destroy(reg);
    unlink(path);
}

TEST(failed_session_does_not_affect_siblings) {
    oi_session_registry *reg = oi_session_registry_create();
    char p1[256], p2[256];
    snprintf(p1, sizeof p1, "/tmp/oi_session_fault_1_%d.log", (int)getpid());
    snprintf(p2, sizeof p2, "/tmp/oi_session_fault_2_%d.log", (int)getpid());
    unlink(p1);
    unlink(p2);

    oi_session *a, *b;
    CHECK_EQ(oi_session_create(reg, "a", p1, 0, &a), OI_OK);
    CHECK_EQ(oi_session_create(reg, "b", p2, 0, &b), OI_OK);

    oi_session_fail(a);

    CHECK_EQ(oi_session_state_of(a), OI_SESSION_FAILED);
    CHECK_EQ(oi_session_state_of(b), OI_SESSION_ACTIVE);
    CHECK(oi_session_arena(b) != NULL);
    CHECK_EQ(oi_sesslog_append(oi_session_log(b), "still-alive", 11), OI_OK);

    oi_session_registry_destroy(reg);
    unlink(p1);
    unlink(p2);
}

/* --- resume from an existing log across a simulated restart --- */

struct collected {
    char buf[256];
    size_t len;
    int count;
};

static void collect_cb(const void *data, size_t len, void *ud) {
    struct collected *c = ud;
    CHECK(c->len + len <= sizeof c->buf);
    memcpy(c->buf + c->len, data, len);
    c->len += len;
    c->count++;
}

TEST(resume_replays_prior_records) {
    const char *path = fresh_log_path();

    /* "before restart" */
    oi_session_registry *reg1 = oi_session_registry_create();
    oi_session *s1;
    CHECK_EQ(oi_session_create(reg1, "resumable", path, 0, &s1), OI_OK);
    CHECK_EQ(oi_sesslog_append(oi_session_log(s1), "turn-1", 6), OI_OK);
    CHECK_EQ(oi_sesslog_append(oi_session_log(s1), "turn-2", 6), OI_OK);
    oi_session_registry_destroy(reg1); /* closes the log's fd/lock */

    /* "after restart": a fresh registry, same id and log_path */
    oi_session_registry *reg2 = oi_session_registry_create();
    oi_session *s2;
    CHECK_EQ(oi_session_create(reg2, "resumable", path, 0, &s2), OI_OK);

    struct collected c = {0};
    CHECK_EQ(oi_sesslog_replay(oi_session_log(s2), collect_cb, &c), OI_OK);
    CHECK_EQ(c.count, 2);
    CHECK(memcmp(c.buf, "turn-1turn-2", 12) == 0);

    /* Resuming must not stop the session from being used further. */
    CHECK_EQ(oi_sesslog_append(oi_session_log(s2), "turn-3", 6), OI_OK);

    oi_session_registry_destroy(reg2);
    unlink(path);
}

TEST(create_propagates_locked_log_error) {
    const char *path = fresh_log_path();
    oi_sesslog *raw_log;
    CHECK_EQ(oi_sesslog_open(path, &raw_log), OI_OK);

    oi_session_registry *reg = oi_session_registry_create();
    oi_session *s;
    /* The log is already locked by raw_log, so opening it as a session
     * must fail with the same error oi_sesslog_open would give. */
    CHECK_EQ(oi_session_create(reg, "locked-out", path, 0, &s),
              OI_ERR_EXISTS);

    oi_sesslog_close(raw_log);
    oi_session_registry_destroy(reg);
    unlink(path);
}

int main(void) {
    RUN(create_destroy_registry);
    RUN(create_lookup_session);
    RUN(duplicate_id_rejected);
    RUN(create_rejects_bad_args);
    RUN(lookup_rejects_null_args);
    RUN(destroy_removes_from_registry);
    RUN(destroy_null_safe);
    RUN(multiple_independent_sessions);
    RUN(fail_marks_failed_and_releases_resources);
    RUN(fail_is_idempotent);
    RUN(fail_null_safe);
    RUN(destroy_after_fail_is_clean);
    RUN(failed_session_does_not_affect_siblings);
    RUN(resume_replays_prior_records);
    RUN(create_propagates_locked_log_error);
    return oi_test_report();
}
