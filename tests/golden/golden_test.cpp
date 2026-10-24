// Tests for the golden-image helper (golden.h) against a committed synthetic golden.
//
// data/test_pattern_64x64.png is golden::test_pattern(64, 64), written once with
// MAPLETHIEF_UPDATE_GOLDEN=1 (README: "Golden images").
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "golden.h"

namespace fs = std::filesystem;
using namespace maplethief::golden;

namespace {

const fs::path kData = fs::path(u8"" MT_GOLDEN_DATA_DIR);
const fs::path kPatternGolden = kData / "test_pattern_64x64.png";

// Moves a channel by exactly `by` (<= 127) towards mid-grey, so it never clamps.
std::uint8_t nudge(std::uint8_t v, int by) {
    return static_cast<std::uint8_t>(v < 128 ? v + by : v - by);
}

// A golden written to the test output directory, so that tests expecting a failure never
// touch the committed one (they also pass Update::Never).
fs::path scratch_golden(const std::string &name, const Image &image) {
    fs::path path = test_output_dir() / "golden_test" / (name + ".golden.png");
    std::string error;
    REQUIRE_MESSAGE(write_png(path, image, &error), error);
    return path;
}

} // namespace

TEST_CASE("test pattern matches its committed golden") {
    GoldenResult r = check_golden(test_pattern(64, 64), kPatternGolden, "test_pattern_64x64");
    CHECK_MESSAGE(r.passed, r.message);
    CHECK(r.comparison.differing_pixels == 0);
    CHECK(r.comparison.max_channel_delta == 0);
}

TEST_CASE("test pattern is deterministic and uses every channel") {
    Image p = test_pattern(64, 64);
    CHECK(p == test_pattern(64, 64));
    CHECK(p.at(0, 0) == Rgba{0, 0, 0, 255});
    CHECK(p.at(63, 0) == Rgba{255, 0, 255, 255});
    CHECK(p.at(8, 0).b == 255);
    CHECK(p.at(63, 63) == Rgba{255, 255, 0, 255});
    CHECK(p.at(0, 63).a == 64);
}

TEST_CASE("identical images match") {
    Image p = test_pattern(64, 64);
    Comparison c = compare(p, p);
    CHECK(c.matches);
    CHECK(c.size_matches);
    CHECK(c.differing_pixels == 0);
    CHECK(c.total_pixels == 64 * 64);
    CHECK(c.max_channel_delta == 0);
    CHECK(c.diff.at(10, 10).r == c.diff.at(10, 10).g); // dimmed grey

    SUBCASE("and survive a PNG round trip") {
        fs::path path = scratch_golden("roundtrip", p);
        std::string error;
        auto loaded = load_png(path, &error);
        REQUIRE_MESSAGE(loaded, error);
        CHECK(*loaded == p);
    }
}

TEST_CASE("differences within the channel tolerance match") {
    Image expected = test_pattern(64, 64);
    Image actual = expected;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            Rgba p = actual.at(x, y);
            int by = (x + y) % 2 != 0 ? 3 : 1;
            actual.set(x, y, {nudge(p.r, by), nudge(p.g, 3), nudge(p.b, by), p.a});
        }
    }

    Comparison within = compare(actual, expected, {.channel = 3});
    CHECK(within.matches);
    CHECK(within.differing_pixels == 0);
    CHECK(within.max_channel_delta == 3);
    CHECK(within.diff.at(1, 0) == kDiffWithinTolerance);
    CHECK(within.diff.at(0, 0) == kDiffWithinTolerance);

    Comparison beyond = compare(actual, expected, {.channel = 2});
    CHECK_FALSE(beyond.matches);
    CHECK(beyond.differing_pixels == 64 * 64);
}

TEST_CASE("a differing-pixel ratio within the limit matches") {
    Image expected = test_pattern(64, 64);
    Image actual = expected;
    for (int i = 0; i < 10; ++i) {
        actual.set(i * 6, i * 6, {12, 34, 56, 78});
    }
    CHECK(compare(actual, expected, {.max_differing_ratio = 10.0 / 4096}).matches);
    CHECK_FALSE(compare(actual, expected, {.max_differing_ratio = 9.0 / 4096}).matches);
    CHECK(compare(actual, expected).differing_pixels == 10);
}

TEST_CASE("a failing comparison writes the actual image and a diff PNG") {
    Image expected = test_pattern(64, 64);
    fs::path golden = scratch_golden("failing", expected);
    Image actual = expected;
    for (int y = 20; y < 28; ++y) {
        for (int x = 40; x < 48; ++x) {
            actual.set(x, y, {0, 0, 0, 0});
        }
    }

    GoldenResult r =
        check_golden(actual, golden, "golden_test/failing", {.channel = 2}, Update::Never);
    CHECK_FALSE(r.passed);
    CHECK(r.comparison.differing_pixels == 64);
    CHECK(r.message.find("64 of 4096 pixels differ") != std::string::npos);
    REQUIRE(fs::is_regular_file(r.diff_path));
    REQUIRE(fs::is_regular_file(r.actual_path));
    CHECK(r.diff_path == test_output_dir() / "golden_test" / "failing.diff.png");
    CHECK(r.actual_path == test_output_dir() / "golden_test" / "failing.actual.png");
    CHECK(r.message.find(r.diff_path.filename().string()) != std::string::npos);

    std::string error;
    auto diff = load_png(r.diff_path, &error);
    REQUIRE_MESSAGE(diff, error);
    REQUIRE(diff->width() == 64);
    REQUIRE(diff->height() == 64);
    int red = 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            Rgba p = diff->at(x, y);
            bool in_block = x >= 40 && x < 48 && y >= 20 && y < 28;
            if (in_block) {
                CHECK(p == kDiffDiffering);
                ++red;
            } else {
                CHECK((p.r == p.g && p.g == p.b && p.a == 255 && p.r >= 64 && p.r <= 127));
            }
        }
    }
    CHECK(red == 64);

    auto written = load_png(r.actual_path, &error);
    REQUIRE_MESSAGE(written, error);
    CHECK(*written == actual);
}

TEST_CASE("a passing run removes the files of an earlier failure") {
    Image expected = test_pattern(16, 16);
    fs::path golden = scratch_golden("stale", expected);
    GoldenResult fail =
        check_golden(test_pattern(16, 8), golden, "golden_test/stale", {}, Update::Never);
    REQUIRE_FALSE(fail.passed);
    REQUIRE(fs::is_regular_file(fail.diff_path));

    GoldenResult pass = check_golden(expected, golden, "golden_test/stale", {}, Update::Never);
    CHECK_MESSAGE(pass.passed, pass.message);
    CHECK(pass.diff_path.empty());
    CHECK_FALSE(fs::exists(fail.diff_path));
    CHECK_FALSE(fs::exists(fail.actual_path));
}

TEST_CASE("a size mismatch fails and marks the missing area") {
    Image expected = test_pattern(64, 64);
    Image actual = test_pattern(32, 64);
    Comparison c = compare(actual, expected, {.channel = 255, .max_differing_ratio = 1.0});
    CHECK_FALSE(c.matches);
    CHECK_FALSE(c.size_matches);
    CHECK(c.diff.width() == 64);
    CHECK(c.diff.height() == 64);
    CHECK(c.diff.at(40, 3) == kDiffOutside);
    CHECK(c.diff.at(3, 3) != kDiffOutside);

    fs::path golden = scratch_golden("size", expected);
    GoldenResult r = check_golden(actual, golden, "golden_test/size", {}, Update::Never);
    CHECK_FALSE(r.passed);
    CHECK(r.message.find("size 32x64 vs golden 64x64") != std::string::npos);
    CHECK(fs::is_regular_file(r.diff_path));
}

TEST_CASE("a missing golden fails and writes the actual image") {
    fs::path missing = test_output_dir() / "golden_test" / "does-not-exist.png";
    fs::remove(missing);
    GoldenResult r =
        check_golden(test_pattern(8, 8), missing, "golden_test/missing", {}, Update::Never);
    CHECK_FALSE(r.passed);
    CHECK(r.message.find("MAPLETHIEF_UPDATE_GOLDEN=1") != std::string::npos);
    CHECK(fs::is_regular_file(r.actual_path));
    CHECK_FALSE(fs::exists(missing));
}

TEST_CASE("a file that is not a PNG reports an error") {
    fs::path path = test_output_dir() / "golden_test" / "not-a-png.png";
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "not a png";
    std::string error;
    CHECK_FALSE(load_png(path, &error));
    CHECK(error.find("not a decodable PNG") != std::string::npos);
}

TEST_CASE("output names stay inside the test output directory") {
    fs::path golden = scratch_golden("names", test_pattern(4, 4));
    GoldenResult r = check_golden(test_pattern(4, 2), golden, "golden_test/../names/./we ird:name",
                                  {}, Update::Never);
    REQUIRE_FALSE(r.passed);
    CHECK(r.diff_path ==
          test_output_dir() / "golden_test" / "_" / "names" / "_" / "we_ird_name.diff.png");
    CHECK(fs::is_regular_file(r.diff_path));
}
