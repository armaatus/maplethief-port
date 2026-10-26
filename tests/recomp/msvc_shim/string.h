/* Stand-in for the Windows SDK's <string.h> in the clang --target=x86_64-pc-windows-msvc
 * syntax check of generated code (tests/recomp_harness.py), on hosts without the SDK. Only
 * what the recompiler's support headers use. Never used for a real build. */
#ifndef MT_MSVC_SHIM_STRING_H
#define MT_MSVC_SHIM_STRING_H
#include <stddef.h>
void *memcpy(void *dest, const void *src, size_t n);
#endif
