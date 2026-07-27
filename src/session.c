#include "oi/session.h"

#include <stdlib.h>
#include <string.h>

struct oi_session {
    char *id;
    oi_arena *arena; /* NULL once failed */
    oi_sesslog *log; /* NULL once failed */
    oi_session_state state;
};

struct oi_session_registry {
    oi_session **sessions;
    size_t count;
    size_t cap;
};

oi_session_registry *oi_session_registry_create(void) {
    oi_session_registry *reg = malloc(sizeof *reg);
    if (reg == NULL) {
        return NULL;
    }
    reg->sessions = NULL;
    reg->count = 0;
    reg->cap = 0;
    return reg;
}

static void free_session(oi_session *s) {
    oi_arena_destroy(s->arena);
    oi_sesslog_close(s->log);
    free(s->id);
    free(s);
}

void oi_session_registry_destroy(oi_session_registry *reg) {
    if (reg == NULL) {
        return;
    }
    for (size_t i = 0; i < reg->count; i++) {
        free_session(reg->sessions[i]);
    }
    free(reg->sessions);
    free(reg);
}

static oi_session **find_slot(const oi_session_registry *reg,
                               const char *id) {
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->sessions[i]->id, id) == 0) {
            return &reg->sessions[i];
        }
    }
    return NULL;
}

oi_status oi_session_create(oi_session_registry *reg, const char *id,
                             const char *log_path, size_t block_size,
                             oi_session **out_session) {
    if (reg == NULL || id == NULL || log_path == NULL ||
        out_session == NULL) {
        return OI_ERR_INVAL;
    }
    if (find_slot(reg, id) != NULL) {
        return OI_ERR_EXISTS;
    }

    oi_session *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return OI_ERR_NOMEM;
    }

    s->id = strdup(id);
    if (s->id == NULL) {
        free(s);
        return OI_ERR_NOMEM;
    }

    s->arena = oi_arena_create(block_size);
    if (s->arena == NULL) {
        free(s->id);
        free(s);
        return OI_ERR_NOMEM;
    }

    oi_status st = oi_sesslog_open(log_path, &s->log);
    if (st != OI_OK) {
        oi_arena_destroy(s->arena);
        free(s->id);
        free(s);
        return st;
    }

    s->state = OI_SESSION_ACTIVE;

    if (reg->count == reg->cap) {
        size_t new_cap = reg->cap == 0 ? 8 : reg->cap * 2;
        oi_session **ns = realloc(reg->sessions, new_cap * sizeof *ns);
        if (ns == NULL) {
            oi_sesslog_close(s->log);
            oi_arena_destroy(s->arena);
            free(s->id);
            free(s);
            return OI_ERR_NOMEM;
        }
        reg->sessions = ns;
        reg->cap = new_cap;
    }
    reg->sessions[reg->count++] = s;

    *out_session = s;
    return OI_OK;
}

oi_session *oi_session_lookup(const oi_session_registry *reg,
                               const char *id) {
    if (reg == NULL || id == NULL) {
        return NULL;
    }
    oi_session **slot = find_slot(reg, id);
    return slot ? *slot : NULL;
}

void oi_session_destroy(oi_session_registry *reg, oi_session *session) {
    if (reg == NULL || session == NULL) {
        return;
    }
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->sessions[i] == session) {
            reg->sessions[i] = reg->sessions[reg->count - 1];
            reg->count--;
            free_session(session);
            return;
        }
    }
}

const char *oi_session_id(const oi_session *session) {
    return session ? session->id : NULL;
}

oi_session_state oi_session_state_of(const oi_session *session) {
    return session ? session->state : OI_SESSION_FAILED;
}

oi_arena *oi_session_arena(const oi_session *session) {
    return session ? session->arena : NULL;
}

oi_sesslog *oi_session_log(const oi_session *session) {
    return session ? session->log : NULL;
}

void oi_session_fail(oi_session *session) {
    if (session == NULL || session->state == OI_SESSION_FAILED) {
        return;
    }
    oi_arena_destroy(session->arena);
    session->arena = NULL;
    oi_sesslog_close(session->log);
    session->log = NULL;
    session->state = OI_SESSION_FAILED;
}
