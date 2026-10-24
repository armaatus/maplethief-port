// Unit tests for the HLE runtime's C interface (runtime/include/maplethief/).
#include <doctest/doctest.h>

#include <string_view>

#include "maplethief/runtime.h"

TEST_CASE("version string names the runtime and the project version") {
    const char *version = mt_runtime_version();
    REQUIRE(version != nullptr);
    CHECK(std::string_view(version) == std::string_view("maplethief-runtime " MT_VERSION));
}
