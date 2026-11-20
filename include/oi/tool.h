#ifndef OI_TOOL_H
#define OI_TOOL_H

#include <stddef.h>

#include "oi/arena.h"
#include "oi/json.h"
#include "oi/reactor.h"
#include "oi/status.h"

typedef struct oi_tool_registry oi_tool_registry;

typedef oi_status (*oi_tool_build_argv)(const oi_json_value *args,
                                         oi_arena *arena, void *user_data,
                                         char ***out_argv);

oi_tool_registry *oi_tool_registry_create(void);
void oi_tool_registry_destroy(oi_tool_registry *reg);

oi_status oi_tool_registry_add(oi_tool_registry *reg, const char *name,
                                const char *json_schema,
                                oi_tool_build_argv build_argv,
                                void *user_data);

const char *oi_tool_registry_schema(const oi_tool_registry *reg,
                                     const char *name);

typedef struct oi_tool_call oi_tool_call;

typedef enum {
    OI_TOOL_ALLOW,
    OI_TOOL_DENY,
    OI_TOOL_ASK
} oi_tool_decision;

typedef oi_tool_decision (*oi_tool_permission_cb)(const char *tool_name,
                                                   const oi_json_value *args,
                                                   void *user_data);

typedef void (*oi_tool_output_cb)(const void *data, size_t len,
                                   void *user_data);

typedef enum {
    OI_TOOL_EXIT_NORMAL,
    OI_TOOL_EXIT_SIGNALED,
    OI_TOOL_EXIT_FAILED,
    OI_TOOL_EXIT_TIMEOUT
} oi_tool_exit_kind;

typedef void (*oi_tool_done_cb)(oi_tool_exit_kind kind, int code,
                                 void *user_data);

oi_status oi_tool_call_start(oi_tool_registry *reg, oi_reactor *r,
                              oi_arena *arena, const char *tool_name,
                              const oi_json_value *args,
                              oi_tool_permission_cb permission_cb,
                              void *permission_ud, oi_tool_output_cb on_output,
                              oi_tool_done_cb on_done, void *user_data,
                              oi_tool_call **out_call);

oi_status oi_tool_call_resolve(oi_tool_call *call, int allow);

oi_status oi_tool_call_write_stdin(oi_tool_call *call, const void *data,
                                    size_t len);

oi_status oi_tool_call_close_stdin(oi_tool_call *call);

oi_status oi_tool_call_set_timeout(oi_tool_call *call, int timeout_ms);

void oi_tool_call_cancel(oi_tool_call *call);

#endif
