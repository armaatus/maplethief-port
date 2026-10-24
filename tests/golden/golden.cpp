// Golden-image helper (golden.h, ADR 0005).
#include "golden.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>

// Private copies of stb_image (PNG decoding only) and stb_image_write, with static linkage so
// that they can't clash with the runtime's own copy when a test links both. All file access
// goes through std::filesystem, which handles non-ASCII paths on Windows.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace maplethief::golden {

namespace {

void set_error(std::string *error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

// The value of an environment variable, as a path (UTF-16 on Windows) or as text.
std::optional<std::filesystem::path> env_path(const char *name) {
#ifdef _WIN32
    std::wstring wide(name, name + std::char_traits<char>::length(name));
    wchar_t *value = nullptr;
    std::size_t length = 0;
    if (_wdupenv_s(&value, &length, wide.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::filesystem::path path(value);
    std::free(value);
    return path;
#else
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::filesystem::path(value);
#endif
}

bool update_requested() {
    auto value = env_path("MAPLETHIEF_UPDATE_GOLDEN");
    return value && *value == "1";
}

// `name` may use '/' for subdirectories. Other characters outside [A-Za-z0-9._-] become '_',
// and so do "." and ".." components, so the result stays inside the output directory.
std::filesystem::path sanitized(std::string_view name) {
    std::filesystem::path out;
    std::string part;
    auto flush = [&] {
        if (part.empty()) {
            return;
        }
        if (part == "." || part == "..") {
            part = "_";
        }
        out /= part;
        part.clear();
    };
    for (char c : name) {
        if (c == '/') {
            flush();
            continue;
        }
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        part += ok ? c : '_';
    }
    flush();
    return out.empty() ? std::filesystem::path("image") : out;
}

std::string display(const std::filesystem::path &path) {
    auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

void remove_quietly(const std::filesystem::path &path) {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

int channel_delta(std::uint8_t a, std::uint8_t b) { return a > b ? a - b : b - a; }

Rgba dimmed_grey(Rgba p) {
    int luma = (299 * p.r + 587 * p.g + 114 * p.b) / 1000;
    auto v = static_cast<std::uint8_t>(64 + luma / 4);
    return {v, v, v, 255};
}

} // namespace

Image::Image(int width, int height, Rgba fill) : width_(width), height_(height) {
    if (width < 0 || height < 0) {
        throw std::invalid_argument("golden::Image: negative size");
    }
    rgba_.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (std::size_t i = 0; i < rgba_.size(); i += 4) {
        rgba_[i] = fill.r;
        rgba_[i + 1] = fill.g;
        rgba_[i + 2] = fill.b;
        rgba_[i + 3] = fill.a;
    }
}

Rgba Image::at(int x, int y) const {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        throw std::out_of_range("golden::Image::at: pixel outside the image");
    }
    const std::uint8_t *p = &rgba_[(static_cast<std::size_t>(y) * width_ + x) * 4];
    return {p[0], p[1], p[2], p[3]};
}

void Image::set(int x, int y, Rgba pixel) {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        throw std::out_of_range("golden::Image::set: pixel outside the image");
    }
    std::uint8_t *p = &rgba_[(static_cast<std::size_t>(y) * width_ + x) * 4];
    p[0] = pixel.r;
    p[1] = pixel.g;
    p[2] = pixel.b;
    p[3] = pixel.a;
}

std::optional<Image> load_png(const std::filesystem::path &path, std::string *error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(error, "cannot open " + display(path));
        return std::nullopt;
    }
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (data.size() > static_cast<std::size_t>(INT_MAX)) {
        set_error(error, display(path) + ": file too large");
        return std::nullopt;
    }
    int width = 0, height = 0, channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(data.data(), static_cast<int>(data.size()), &width,
                                            &height, &channels, 4);
    if (pixels == nullptr) {
        set_error(error, display(path) + ": not a decodable PNG (" + stbi_failure_reason() + ")");
        return std::nullopt;
    }
    Image image(width, height);
    std::copy_n(pixels, image.bytes().size(), image.bytes().begin());
    stbi_image_free(pixels);
    return image;
}

bool write_png(const std::filesystem::path &path, const Image &image, std::string *error) {
    if (image.empty()) {
        set_error(error, "cannot write an empty image to " + display(path));
        return false;
    }
    std::vector<unsigned char> png;
    auto append = [](void *context, void *data, int size) {
        auto *out = static_cast<std::vector<unsigned char> *>(context);
        auto *bytes = static_cast<const unsigned char *>(data);
        out->insert(out->end(), bytes, bytes + size);
    };
    if (stbi_write_png_to_func(append, &png, image.width(), image.height(), 4, image.bytes().data(),
                               image.width() * 4) == 0) {
        set_error(error, "PNG encoding failed for " + display(path));
        return false;
    }
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            set_error(error, "cannot create " + display(path.parent_path()) + ": " + ec.message());
            return false;
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    out.close();
    if (!out) {
        set_error(error, "cannot write " + display(path));
        return false;
    }
    return true;
}

Comparison compare(const Image &actual, const Image &expected, const Tolerance &tolerance) {
    Comparison result;
    result.size_matches =
        actual.width() == expected.width() && actual.height() == expected.height();
    int width = std::max(actual.width(), expected.width());
    int height = std::max(actual.height(), expected.height());
    result.total_pixels = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    result.diff = Image(width, height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            bool in_actual = x < actual.width() && y < actual.height();
            bool in_expected = x < expected.width() && y < expected.height();
            if (!in_actual || !in_expected) {
                ++result.differing_pixels;
                result.diff.set(x, y, kDiffOutside);
                continue;
            }
            Rgba a = actual.at(x, y), e = expected.at(x, y);
            int delta = std::max({channel_delta(a.r, e.r), channel_delta(a.g, e.g),
                                  channel_delta(a.b, e.b), channel_delta(a.a, e.a)});
            result.max_channel_delta = std::max(result.max_channel_delta, delta);
            if (delta > tolerance.channel) {
                ++result.differing_pixels;
                result.diff.set(x, y, kDiffDiffering);
            } else if (delta > 0) {
                result.diff.set(x, y, kDiffWithinTolerance);
            } else {
                result.diff.set(x, y, dimmed_grey(e));
            }
        }
    }

    double allowed = tolerance.max_differing_ratio * static_cast<double>(result.total_pixels);
    result.matches = result.size_matches && static_cast<double>(result.differing_pixels) <= allowed;
    return result;
}

GoldenResult check_golden(const Image &actual, const std::filesystem::path &golden_path,
                          std::string_view name, const Tolerance &tolerance, Update update) {
    GoldenResult result;
    std::filesystem::path relative = sanitized(name);
    std::string stem = display(relative);
    std::filesystem::path base = test_output_dir() / relative;
    result.actual_path = base;
    result.actual_path += ".actual.png";
    result.diff_path = base;
    result.diff_path += ".diff.png";
    // A passing run must not leave the files of an earlier failure behind.
    remove_quietly(result.actual_path);
    remove_quietly(result.diff_path);

    bool may_update = update == Update::FromEnvironment && update_requested();
    auto replace_golden = [&](const char *what) {
        std::string error;
        result.passed = write_png(golden_path, actual, &error);
        result.message =
            result.passed ? stem + ": golden " + what + ": " + display(golden_path) : error;
        result.actual_path.clear();
        result.diff_path.clear();
    };

    std::string error;
    std::optional<Image> golden = load_png(golden_path, &error);
    if (!golden) {
        if (may_update) {
            replace_golden("written");
            return result;
        }
        result.diff_path.clear();
        std::string write_error, written;
        if (write_png(result.actual_path, actual, &write_error)) {
            written = "actual image written to " + display(result.actual_path);
        } else {
            written = "writing the actual image failed: " + write_error;
            result.actual_path.clear();
        }
        result.message = stem + ": cannot load golden (" + error + "); " + written +
                         "; set MAPLETHIEF_UPDATE_GOLDEN=1 to create it";
        return result;
    }

    result.comparison = compare(actual, *golden, tolerance);
    const Comparison &c = result.comparison;
    if (c.matches) {
        result.passed = true;
        result.diff_path.clear();
        result.actual_path.clear();
        std::ostringstream msg;
        msg << stem << ": matches golden (" << c.differing_pixels << " of " << c.total_pixels
            << " pixels differ, max channel delta " << c.max_channel_delta << ")";
        result.message = msg.str();
        return result;
    }
    if (may_update) {
        replace_golden("updated");
        return result;
    }

    std::ostringstream msg;
    msg << stem << ": " << c.differing_pixels << " of " << c.total_pixels
        << " pixels differ by more than " << tolerance.channel << " (allowed ratio "
        << tolerance.max_differing_ratio << "), max channel delta " << c.max_channel_delta;
    if (!c.size_matches) {
        msg << ", size " << actual.width() << "x" << actual.height() << " vs golden "
            << golden->width() << "x" << golden->height();
    }
    bool wrote = write_png(result.actual_path, actual, &error) &&
                 write_png(result.diff_path, c.diff, &error);
    if (wrote) {
        msg << "; diff: " << display(result.diff_path)
            << ", actual: " << display(result.actual_path);
    } else {
        msg << "; writing the diff failed: " << error;
    }
    result.message = msg.str();
    return result;
}

std::filesystem::path test_output_dir() {
    if (auto dir = env_path("MAPLETHIEF_TEST_OUTPUT_DIR"); dir && !dir->empty()) {
        return *dir;
    }
    // u8: the configured path is UTF-8, also on Windows (where narrow means the ANSI page).
    return std::filesystem::path(u8"" MT_TEST_OUTPUT_DIR);
}

Image test_pattern(int width, int height) {
    Image image(width, height);
    int wspan = std::max(1, width - 1);
    int hspan = std::max(1, height - 1);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            Rgba p;
            p.r = static_cast<std::uint8_t>(x * 255 / wspan);
            p.g = static_cast<std::uint8_t>(y * 255 / hspan);
            p.b = static_cast<std::uint8_t>(((x / 8 + y / 8) % 2) != 0 ? 255 : 0);
            p.a = y < height / 2 ? 255 : static_cast<std::uint8_t>(64 + x * 191 / wspan);
            image.set(x, y, p);
        }
    }
    return image;
}

} // namespace maplethief::golden
