#ifndef OI_COMPAT_H
#define OI_COMPAT_H

/*
 * Compiler-specific diagnostic suppression, isolated here so the modules
 * that need it don't each carry a compiler conditional.
 *
 * The reentrancy guard used by the incremental parsers and the LLM/tool
 * callback paths stores the address of a stack flag into its own struct,
 * so a destroy-from-within-a-callback can be detected by the frame that
 * is still running. Every return path that leaves the object alive
 * clears the pointer again; the one path that doesn't is the one where
 * the object has already been freed. GCC 12+ cannot see that lifetime
 * rule and reports the store itself as -Wdangling-pointer.
 *
 * Clang has no such warning group and, under -Werror, rejects the pragma
 * outright as -Wunknown-warning-option -- so the suppression has to be
 * GCC-only. macOS builds and the fuzz harnesses are both clang.
 */

#if defined(__GNUC__) && !defined(__clang__)
#define OI_DIAG_PUSH_IGNORE_DANGLING                                         \
    _Pragma("GCC diagnostic push")                                           \
        _Pragma("GCC diagnostic ignored \"-Wdangling-pointer\"")
#define OI_DIAG_POP _Pragma("GCC diagnostic pop")
#else
#define OI_DIAG_PUSH_IGNORE_DANGLING
#define OI_DIAG_POP
#endif

#endif /* OI_COMPAT_H */
