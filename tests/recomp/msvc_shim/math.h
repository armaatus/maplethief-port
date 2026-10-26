/* Stand-in for the Windows SDK's <math.h> in the clang --target=x86_64-pc-windows-msvc
 * syntax check of generated code (tests/recomp_harness.py). Never used for a real build. */
#ifndef MT_MSVC_SHIM_MATH_H
#define MT_MSVC_SHIM_MATH_H
double sqrt(double x);
float sqrtf(float x);
#endif
