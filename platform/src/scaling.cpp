// Scaling (#59, ADR 0043); see scaling.h.
#include "scaling.h"

#include <algorithm>
#include <cmath>

namespace maplethief::frontend {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool equal_nocase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = lower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

constexpr ScaleMode kModes[] = {ScaleMode::kInteger, ScaleMode::kFit, ScaleMode::kSmooth,
                                ScaleMode::kSharp};

} // namespace

const char *scale_mode_name(ScaleMode mode) {
    switch (mode) {
    case ScaleMode::kInteger:
        return "integer";
    case ScaleMode::kFit:
        return "fit";
    case ScaleMode::kSmooth:
        return "smooth";
    case ScaleMode::kSharp:
        return "sharp";
    }
    return "?";
}

bool parse_scale_mode(std::string_view name, ScaleMode *mode) {
    for (const ScaleMode m : kModes) {
        if (equal_nocase(name, scale_mode_name(m))) {
            *mode = m;
            return true;
        }
    }
    return false;
}

bool parse_color(std::string_view text, Color *color) {
    if (text.size() != 7 || text[0] != '#') {
        return false;
    }
    int v[6];
    for (int i = 0; i < 6; i++) {
        v[i] = hex_digit(text[1 + i]);
        if (v[i] < 0) {
            return false;
        }
    }
    color->r = static_cast<std::uint8_t>(v[0] * 16 + v[1]);
    color->g = static_cast<std::uint8_t>(v[2] * 16 + v[3]);
    color->b = static_cast<std::uint8_t>(v[4] * 16 + v[5]);
    return true;
}

Rect fit_rect(int output_w, int output_h, int image_w, int image_h) {
    if (output_w <= 0 || output_h <= 0 || image_w <= 0 || image_h <= 0) {
        return {};
    }
    // Compare output_w / output_h with image_w / image_h in integers: the image fills the width
    // if the output is relatively taller, else the height.
    const long long ow = output_w, oh = output_h, iw = image_w, ih = image_h;
    Rect r;
    if (ow * ih <= oh * iw) {
        r.w = static_cast<float>(ow);
        r.h = static_cast<float>(static_cast<double>(ow) * static_cast<double>(ih) /
                                 static_cast<double>(iw));
    } else {
        r.h = static_cast<float>(oh);
        r.w = static_cast<float>(static_cast<double>(oh) * static_cast<double>(iw) /
                                 static_cast<double>(ih));
    }
    // Whole pixels for the bars, so the image's edges do not straddle a pixel.
    r.w = std::round(r.w);
    r.h = std::round(r.h);
    r.x = std::floor((static_cast<float>(ow) - r.w) / 2);
    r.y = std::floor((static_cast<float>(oh) - r.h) / 2);
    return r;
}

Placement place(ScaleMode mode, int output_w, int output_h, int image_w, int image_h) {
    Placement p;
    p.mode = mode;
    p.filter = mode == ScaleMode::kSmooth  ? Filter::kLinear
               : mode == ScaleMode::kSharp ? Filter::kPixelArt
                                           : Filter::kNearest;
    if (output_w <= 0 || output_h <= 0 || image_w <= 0 || image_h <= 0) {
        return p; // nothing to draw into, or nothing to draw
    }
    if (mode == ScaleMode::kInteger) {
        const int k = std::min(output_w / kScreenWidth, output_h / kScreenHeight);
        if (k >= 1) {
            p.integer_scale = k;
            const int w = kScreenWidth * k;
            const int h = kScreenHeight * k;
            p.rect.w = static_cast<float>(w);
            p.rect.h = static_cast<float>(h);
            p.rect.x = static_cast<float>((output_w - w) / 2);
            p.rect.y = static_cast<float>((output_h - h) / 2);
            return p;
        }
        // Smaller than 1x: scale down to fit (nearest).
    }
    p.rect = fit_rect(output_w, output_h, image_w, image_h);
    return p;
}

ScreenPoint drawable_to_screen(const Placement &p, float px, float py) {
    ScreenPoint s;
    const Rect &r = p.rect;
    if (r.w <= 0 || r.h <= 0) {
        return s;
    }
    // In double, so that the forward map and this inverse round-trip to within a float's ulp.
    s.x = static_cast<float>((static_cast<double>(px) - r.x) * kScreenWidth / r.w);
    s.y = static_cast<float>((static_cast<double>(py) - r.y) * kScreenHeight / r.h);
    s.inside = s.x >= 0 && s.x < static_cast<float>(kScreenWidth) && s.y >= 0 &&
               s.y < static_cast<float>(kScreenHeight);
    return s;
}

void screen_to_drawable(const Placement &p, float sx, float sy, float *px, float *py) {
    const Rect &r = p.rect;
    *px = static_cast<float>(r.x + static_cast<double>(sx) * r.w / kScreenWidth);
    *py = static_cast<float>(r.y + static_cast<double>(sy) * r.h / kScreenHeight);
}

ScreenPoint window_to_screen(const Placement &p, float wx, float wy, float density_x,
                             float density_y) {
    return drawable_to_screen(p, wx * (density_x > 0 ? density_x : 1.0f),
                              wy * (density_y > 0 ? density_y : 1.0f));
}

void screen_to_window(const Placement &p, float sx, float sy, float density_x, float density_y,
                      float *wx, float *wy) {
    float px = 0, py = 0;
    screen_to_drawable(p, sx, sy, &px, &py);
    *wx = density_x > 0 ? px / density_x : px;
    *wy = density_y > 0 ? py / density_y : py;
}

} // namespace maplethief::frontend
