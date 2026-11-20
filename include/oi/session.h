#ifndef OI_SESSION_H
#define OI_SESSION_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/sesslog.h"
#include "oi/status.h"

typedef struct oi_session oi_session;
typedef struct oi_session_registry oi_session_registry;

typedef enum { OI_SESSION_ACTIVE, OI_SESSION_FAILED } oi_session_state;

oi_session_registry *oi_session_registry_create(void);
void oi_session_registry_destroy(oi_session_registry *reg);

oi_status oi_session_create(oi_session_registry *reg, const char *id,
                             const char *log_path, size_t block_size,
                             oi_session **out_session);

oi_session *oi_session_lookup(const oi_session_registry *reg,
                               const char *id);

void oi_session_destroy(oi_session_registry *reg, oi_session *session);

const char *oi_session_id(const oi_session *session);

oi_session_state oi_session_state_of(const oi_session *session);

oi_arena *oi_session_arena(const oi_session *session);
oi_sesslog *oi_session_log(const oi_session *session);

void oi_session_fail(oi_session *session);

#endif
