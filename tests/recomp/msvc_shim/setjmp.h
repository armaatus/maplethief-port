/* Stand-in for the Windows SDK's <setjmp.h> in the clang --target=x86_64-pc-windows-msvc
 * syntax check of generated code (tests/recomp_harness.py). Never used for a real build. */
#ifndef MT_MSVC_SHIM_SETJMP_H
#define MT_MSVC_SHIM_SETJMP_H
typedef struct {
    unsigned long long part[2];
} mt_shim_jbtype;
typedef mt_shim_jbtype jmp_buf[16];
int setjmp(jmp_buf env);
__declspec(noreturn) void longjmp(jmp_buf env, int value);
#endif
