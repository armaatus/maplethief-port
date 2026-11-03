// Unit tests for the HLE runtime's C interface (runtime/include/maplethief/).
#include <doctest/doctest.h>

#include <string_view>

#include "maplethief/runtime.h"

TEST_CASE("version string names the runtime and the project version") {
    const char *version = mt_runtime_version();
    REQUIRE(version != nullptr);
    CHECK(std::string_view(version) == std::string_view("maplethief-runtime " MT_VERSION));
}

// A name with every character CTest discovery used to cut or mangle (#171). Its CTest fails if
// the filter selects anything but this case (MT_DOCTEST_EXPECT_CASES), so it runs or it fails.
TEST_CASE(
    "doctest discovery (#171): ; [ ] ]==] , \\ \" $ # * ? é in a name; it runs as one CTest") {
    CHECK(doctest::getContextOptions() != nullptr);
}
