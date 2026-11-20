# compat: decision records from code comments

Date: 2026-11-20

These records were code comments. The project does not allow code comments, so the records are here. Each heading names the symbol and the file that the record applies to. The record text is the original comment text.

Source files: `src/compat.h`.

## `file scope` (src/compat.h)

Compiler-specific diagnostic suppression, isolated here so the modules that need it don't each carry a compiler conditional.

The reentrancy guard used by the incremental parsers and the LLM/tool callback paths stores the address of a stack flag into its own struct, so a destroy-from-within-a-callback can be detected by the frame that is still running. Every return path that leaves the object alive clears the pointer again; the one path that doesn't is the one where the object has already been freed. GCC 12+ cannot see that lifetime rule and reports the store itself as -Wdangling-pointer.

Clang has no such warning group and, under -Werror, rejects the pragma outright as -Wunknown-warning-option -- so the suppression has to be GCC-only. macOS builds and the fuzz harnesses are both clang.

## `oi_wipe` (src/compat.h)

Zeroes memory that held a secret. The volatile store stops the compiler from dropping the writes to memory that is about to be freed.
