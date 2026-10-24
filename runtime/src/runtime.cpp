// Placeholder for the HLE runtime (M4).
#include "maplethief/runtime.h"

extern "C" const char *mt_runtime_version(void) noexcept {
    return "maplethief-runtime " MT_VERSION;
}
