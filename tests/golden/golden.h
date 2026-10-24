// Golden-image helper for C++ tests (ADR 0005).
//
// A test renders an image, then checks it against a golden PNG:
//
//     auto result = maplethief::golden::check_golden(frame, golden_path, "title_screen",
//                                                    {.channel = 2, .max_differing_ratio = 0.001});
//     CHECK_MESSAGE(result.passed, result.message);
//
// On failure, <name>.actual.png and <name>.diff.png are written to test_output_dir(). With
// MAPLETHIEF_UPDATE_GOLDEN=1 in the environment, a missing or different golden is replaced
// by the actual image instead (review the change before committing it).
//
// Images are 8-bit RGBA, row-major, top row first, without row padding.
#ifndef MAPLETHIEF_TESTS_GOLDEN_H
#define MAPLETHIEF_TESTS_GOLDEN_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace maplethief::golden {

struct Rgba {
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
    friend bool operator==(const Rgba &, const Rgba &) = default;
};

class Image {
  public:
    Image() = default;
    Image(int width, int height, Rgba fill = {});

    int width() const { return width_; }
    int height() const { return height_; }
    bool empty() const { return width_ == 0 || height_ == 0; }

    Rgba at(int x, int y) const;
    void set(int x, int y, Rgba pixel);

    // width() * height() * 4 bytes.
    const std::vector<std::uint8_t> &bytes() const { return rgba_; }
    std::vector<std::uint8_t> &bytes() { return rgba_; }

    friend bool operator==(const Image &, const Image &) = default;

  private:
    int width_ = 0, height_ = 0;
    std::vector<std::uint8_t> rgba_;
};

// Decodes a PNG file to RGBA. On failure returns nullopt and, if given, sets *error.
std::optional<Image> load_png(const std::filesystem::path &path, std::string *error = nullptr);

// Encodes an image as PNG, creating parent directories. The output is deterministic.
bool write_png(const std::filesystem::path &path, const Image &image, std::string *error = nullptr);

struct Tolerance {
    // A pixel differs when any channel (R, G, B or A) is off by more than this.
    int channel = 0;
    // The images match when at most this fraction of their pixels differ (0 = none).
    double max_differing_ratio = 0.0;
};

// Diff image colours: matching pixels are the expected pixel's luma, dimmed to 64..127 grey;
// pixels that differ only within the channel tolerance are yellow; differing pixels are red;
// pixels that exist in only one of the two images (size mismatch) are magenta.
inline constexpr Rgba kDiffWithinTolerance{255, 255, 0, 255};
inline constexpr Rgba kDiffDiffering{255, 0, 0, 255};
inline constexpr Rgba kDiffOutside{255, 0, 255, 255};

struct Comparison {
    bool matches = false;
    bool size_matches = false;
    std::size_t differing_pixels = 0; // beyond the channel tolerance, plus outside pixels
    std::size_t total_pixels = 0;     // of the union of both images
    int max_channel_delta = 0;        // over the overlapping area
    Image diff;                       // max(width) x max(height), colours as above
};

Comparison compare(const Image &actual, const Image &expected, const Tolerance &tolerance = {});

enum class Update {
    FromEnvironment, // replace the golden when MAPLETHIEF_UPDATE_GOLDEN=1
    Never,
};

struct GoldenResult {
    bool passed = false;
    std::string message;               // a one-line summary, for CHECK_MESSAGE
    std::filesystem::path actual_path; // written on failure
    std::filesystem::path diff_path;   // written on failure, unless the golden is missing
    Comparison comparison;
};

// Compares `actual` against the PNG at `golden_path`. `name` becomes the path of the output
// files below test_output_dir(), "<name>.actual.png" and "<name>.diff.png": '/' separates
// subdirectories, other characters outside [A-Za-z0-9._-] become '_'.
GoldenResult check_golden(const Image &actual, const std::filesystem::path &golden_path,
                          std::string_view name, const Tolerance &tolerance = {},
                          Update update = Update::FromEnvironment);

// The MAPLETHIEF_TEST_OUTPUT_DIR environment variable if set, else the directory configured
// in CMake (build/test-output/<preset> with the presets).
std::filesystem::path test_output_dir();

// A deterministic synthetic pattern: a red/green gradient over x/y, a blue 8x8 checkerboard,
// and an alpha ramp in the bottom half. Nothing in it comes from the game.
Image test_pattern(int width, int height);

} // namespace maplethief::golden

#endif // MAPLETHIEF_TESTS_GOLDEN_H
