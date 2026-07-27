#ifndef OI_TOOL_INTERNAL_H
#define OI_TOOL_INTERNAL_H

#include "oi/tool.h"

/*
 * Seam between tool_registry.c (pure bookkeeping) and tool_exec.c (the
 * fork/exec/reactor machinery), so tool_exec.c never touches the
 * registry's internal struct layout directly.
 */
oi_status oi_tool_registry_lookup(const oi_tool_registry *reg,
                                   const char *name,
                                   oi_tool_build_argv *out_build_argv,
                                   void **out_user_data);

#endif /* OI_TOOL_INTERNAL_H */
