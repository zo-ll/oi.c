#ifndef OI_TOOL_INTERNAL_H
#define OI_TOOL_INTERNAL_H

#include "oi/tool.h"

oi_status oi_tool_registry_lookup(const oi_tool_registry *reg,
                                   const char *name,
                                   oi_tool_build_argv *out_build_argv,
                                   void **out_user_data);

#endif
