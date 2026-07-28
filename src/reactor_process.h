#ifndef OI_REACTOR_PROCESS_H
#define OI_REACTOR_PROCESS_H

#include <sys/types.h>

#include "oi/reactor.h"
#include "oi/status.h"

typedef struct oi_reactor_process oi_reactor_process;
typedef void (*oi_reactor_process_cb)(oi_reactor *reactor, pid_t pid,
                                       void *user_data);

/* Internal process watcher used by tool execution. */
oi_status oi_reactor_process_start(oi_reactor *reactor, pid_t pid,
                                    oi_reactor_process_cb cb, void *user_data,
                                    oi_reactor_process **out_process);
void oi_reactor_process_cancel(oi_reactor_process *process);

#endif
