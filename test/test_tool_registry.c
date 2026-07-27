#include "oi/tool.h"
#include "test.h"

#include <stdio.h>
#include <string.h>

static oi_status noop_build_argv(const oi_json_value *args, oi_arena *arena,
                                  void *ud, char ***out_argv) {
    (void)args;
    (void)arena;
    (void)ud;
    (void)out_argv;
    return OI_ERR_INVAL; /* never actually invoked in these tests */
}

TEST(create_destroy) {
    oi_tool_registry *reg = oi_tool_registry_create();
    CHECK(reg != NULL);
    oi_tool_registry_destroy(reg);
    oi_tool_registry_destroy(NULL); /* NULL-safe */
}

TEST(add_and_lookup_schema) {
    oi_tool_registry *reg = oi_tool_registry_create();
    CHECK_EQ(oi_tool_registry_add(reg, "echo", "{\"type\":\"object\"}",
                                    noop_build_argv, NULL),
              OI_OK);
    const char *schema = oi_tool_registry_schema(reg, "echo");
    CHECK(schema != NULL);
    CHECK_STREQ(schema, "{\"type\":\"object\"}");
    oi_tool_registry_destroy(reg);
}

TEST(unknown_tool_schema_is_null) {
    oi_tool_registry *reg = oi_tool_registry_create();
    CHECK(oi_tool_registry_schema(reg, "nope") == NULL);
    CHECK(oi_tool_registry_schema(NULL, "nope") == NULL);
    oi_tool_registry_destroy(reg);
}

TEST(duplicate_add_rejected) {
    oi_tool_registry *reg = oi_tool_registry_create();
    CHECK_EQ(oi_tool_registry_add(reg, "x", "{}", noop_build_argv, NULL),
              OI_OK);
    CHECK_EQ(oi_tool_registry_add(reg, "x", "{}", noop_build_argv, NULL),
              OI_ERR_EXISTS);
    oi_tool_registry_destroy(reg);
}

TEST(add_rejects_bad_args) {
    oi_tool_registry *reg = oi_tool_registry_create();
    CHECK_EQ(oi_tool_registry_add(NULL, "x", "{}", noop_build_argv, NULL),
              OI_ERR_INVAL);
    CHECK_EQ(oi_tool_registry_add(reg, NULL, "{}", noop_build_argv, NULL),
              OI_ERR_INVAL);
    CHECK_EQ(oi_tool_registry_add(reg, "x", NULL, noop_build_argv, NULL),
              OI_ERR_INVAL);
    CHECK_EQ(oi_tool_registry_add(reg, "x", "{}", NULL, NULL), OI_ERR_INVAL);
    oi_tool_registry_destroy(reg);
}

TEST(many_tools) {
    oi_tool_registry *reg = oi_tool_registry_create();
    char name[32];
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof name, "tool_%d", i);
        CHECK_EQ(oi_tool_registry_add(reg, name, "{}", noop_build_argv, NULL),
                  OI_OK);
    }
    for (int i = 0; i < 100; i++) {
        snprintf(name, sizeof name, "tool_%d", i);
        CHECK(oi_tool_registry_schema(reg, name) != NULL);
    }
    oi_tool_registry_destroy(reg);
}

int main(void) {
    RUN(create_destroy);
    RUN(add_and_lookup_schema);
    RUN(unknown_tool_schema_is_null);
    RUN(duplicate_add_rejected);
    RUN(add_rejects_bad_args);
    RUN(many_tools);
    return oi_test_report();
}
