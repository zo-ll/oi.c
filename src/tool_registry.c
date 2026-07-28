#include "tool_internal.h"

#include <stdlib.h>
#include <string.h>

struct oi_tool_entry {
    char *name;
    char *json_schema;
    oi_tool_build_argv build_argv;
    void *user_data;
};

struct oi_tool_registry {
    struct oi_tool_entry *entries;
    size_t count;
    size_t cap;
};

oi_tool_registry *oi_tool_registry_create(void) {
    oi_tool_registry *reg = malloc(sizeof *reg);
    if (reg == NULL) {
        return NULL;
    }
    reg->entries = NULL;
    reg->count = 0;
    reg->cap = 0;
    return reg;
}

void oi_tool_registry_destroy(oi_tool_registry *reg) {
    if (reg == NULL) {
        return;
    }
    for (size_t i = 0; i < reg->count; i++) {
        free(reg->entries[i].name);
        free(reg->entries[i].json_schema);
    }
    free(reg->entries);
    free(reg);
}

static struct oi_tool_entry *find_entry(const oi_tool_registry *reg,
                                         const char *name) {
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            return &reg->entries[i];
        }
    }
    return NULL;
}

oi_status oi_tool_registry_add(oi_tool_registry *reg, const char *name,
                                const char *json_schema,
                                oi_tool_build_argv build_argv,
                                void *user_data) {
    if (reg == NULL || name == NULL || json_schema == NULL ||
        build_argv == NULL) {
        return OI_ERR_INVAL;
    }
    if (find_entry(reg, name) != NULL) {
        return OI_ERR_EXISTS;
    }

    if (reg->count == reg->cap) {
        if (reg->cap > (size_t)-1 / 2) {
            return OI_ERR_NOMEM;
        }
        size_t new_cap = reg->cap == 0 ? 8 : reg->cap * 2;
        if (new_cap > (size_t)-1 / sizeof *reg->entries) {
            return OI_ERR_NOMEM;
        }
        struct oi_tool_entry *ne =
            realloc(reg->entries, new_cap * sizeof *ne);
        if (ne == NULL) {
            return OI_ERR_NOMEM;
        }
        reg->entries = ne;
        reg->cap = new_cap;
    }

    char *name_copy = strdup(name);
    char *schema_copy = strdup(json_schema);
    if (name_copy == NULL || schema_copy == NULL) {
        free(name_copy);
        free(schema_copy);
        return OI_ERR_NOMEM;
    }

    struct oi_tool_entry *e = &reg->entries[reg->count++];
    e->name = name_copy;
    e->json_schema = schema_copy;
    e->build_argv = build_argv;
    e->user_data = user_data;
    return OI_OK;
}

const char *oi_tool_registry_schema(const oi_tool_registry *reg,
                                     const char *name) {
    if (reg == NULL || name == NULL) {
        return NULL;
    }
    struct oi_tool_entry *e = find_entry(reg, name);
    return e ? e->json_schema : NULL;
}

/* Internal accessor used by tool_exec.c; not part of the public header
 * since callers only ever need the schema string. */
oi_status oi_tool_registry_lookup(const oi_tool_registry *reg,
                                   const char *name,
                                   oi_tool_build_argv *out_build_argv,
                                   void **out_user_data) {
    if (reg == NULL || name == NULL) {
        return OI_ERR_INVAL;
    }
    struct oi_tool_entry *e = find_entry(reg, name);
    if (e == NULL) {
        return OI_ERR_NOTFOUND;
    }
    *out_build_argv = e->build_argv;
    *out_user_data = e->user_data;
    return OI_OK;
}
