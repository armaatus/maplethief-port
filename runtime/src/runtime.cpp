// Placeholder for the HLE runtime (M4).
#include "maplethief/runtime.h"

// #113 demo, never merged: MSVC-only warning C4459 (a local hides a global); clang and GCC
// warn only with -Wshadow, which isn't on.
int mt_demo_msvc_only = 0;
int mt_demo_msvc_only_hide() {
    int mt_demo_msvc_only = 1;
    return mt_demo_msvc_only;
}

extern "C" const char *mt_runtime_version(void) noexcept {
    return "maplethief-runtime " MT_VERSION;
}
