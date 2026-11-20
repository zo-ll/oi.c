#ifndef OI_COMPAT_H
#define OI_COMPAT_H

#include <stddef.h>

#if defined(__GNUC__) && !defined(__clang__)
#define OI_DIAG_PUSH_IGNORE_DANGLING                                         \
    _Pragma("GCC diagnostic push")                                           \
        _Pragma("GCC diagnostic ignored \"-Wdangling-pointer\"")
#define OI_DIAG_POP _Pragma("GCC diagnostic pop")
#else
#define OI_DIAG_PUSH_IGNORE_DANGLING
#define OI_DIAG_POP
#endif

static inline void oi_wipe(void *data, size_t len) {
    for (volatile unsigned char *p = data; len > 0; len--) {
        *p++ = 0;
    }
}

#endif
