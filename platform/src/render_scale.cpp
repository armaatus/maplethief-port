// render_scale = auto and the frame-budget guard (#152, ADR 0080); see render_scale.h.
#include "render_scale.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace maplethief::frontend {

namespace {

// The measured costs scaled from scale `from` to scale `to`: pixel work grows with N squared.
double scaled(double ms, std::uint32_t from, std::uint32_t to) {
    const double k = static_cast<double>(to) / static_cast<double>(from);
    return ms * k * k;
}

bool fits(double frontend_ms, double render_ms) {
    return frontend_ms <= kFrontendBudgetMs && render_ms <= kRenderBudgetMs;
}

std::string costs(double frontend_ms, double render_ms) {
    char b[160];
    std::snprintf(b, sizeof b, "frontend %.2f ms, with mt_gfx %.2f ms a frame (budget %g / %g ms)",
                  frontend_ms, render_ms, kFrontendBudgetMs, kRenderBudgetMs);
    return b;
}

} // namespace

std::uint32_t render_scale_cap(double image_w, double image_h) {
    if (!(image_w > 0) || !(image_h > 0)) {
        return 1;
    }
    // A hair under a whole multiple counts as it (a fractional fit of 1439.99 pixels is 3x).
    const double n = std::max(image_w / 480.0, image_h / 320.0);
    const double up = std::ceil(n - 1e-6);
    return static_cast<std::uint32_t>(std::clamp(up, 1.0, static_cast<double>(kMaxRenderScale)));
}

RenderScaleGovernor::RenderScaleGovernor(bool automatic, std::uint32_t scale)
    : automatic_(automatic), scale_(std::clamp<std::uint32_t>(scale, 1, kMaxRenderScale)) {}

std::uint32_t RenderScaleGovernor::set_cap(std::uint32_t cap, double now) {
    cap = std::clamp<std::uint32_t>(cap, 1, kMaxRenderScale);
    if (cap == cap_) {
        return 0;
    }
    if (cap > cap_) {
        retry_at_ = -1e300; // a bigger window: look again
    }
    cap_ = cap;
    if (!automatic_ || scale_ <= cap_) {
        return 0;
    }
    char b[96];
    std::snprintf(b, sizeof b, "the window's image needs at most %ux", static_cast<unsigned>(cap_));
    reason_ = b;
    scale_ = cap_;
    last_change_ = now;
    over_ = 0;
    return scale_;
}

std::uint32_t RenderScaleGovernor::update(const RecentStats &recent, double now) {
    if (now - last_eval_ < kEvalSeconds || now - last_change_ < kSettleSeconds) {
        return 0;
    }
    last_eval_ = now;
    if (recent.frames < kMinFrames || recent.seconds < 0.5) {
        return 0; // too little to judge (a pause, a load)
    }
    const double fe = recent.frontend_ms;
    const double total = recent.frontend_ms + recent.gfx_ms;
    const bool over = !fits(fe, total);
    over_ = over ? over_ + 1 : 0;
    if (!automatic_) {
        if (over && scale_ > 1 && over_ >= kWarnAfter && warning_.empty()) {
            char b[160];
            std::snprintf(b, sizeof b,
                          "Render scale %ux is too slow here (%.0f ms a frame): try auto or less",
                          static_cast<unsigned>(scale_), std::max(fe, total));
            warning_ = b;
            reason_ = costs(fe, total);
        }
        return 0;
    }
    if (over && scale_ > 1 && over_ >= kDropAfter) {
        std::uint32_t n = scale_ - 1;
        while (n > 1 && !fits(scaled(fe, scale_, n), scaled(total, scale_, n))) {
            n--;
        }
        char b[64];
        std::snprintf(b, sizeof b, " at %ux: down to %ux", static_cast<unsigned>(scale_),
                      static_cast<unsigned>(n));
        reason_ = costs(fe, total) + b;
        scale_ = n;
        last_change_ = now;
        retry_at_ = now + kRetrySeconds;
        over_ = 0;
        return scale_;
    }
    if (!over && scale_ < cap_ && now >= retry_at_) {
        const std::uint32_t n = scale_ + 1;
        if (fits(scaled(fe, scale_, n), scaled(total, scale_, n))) {
            char b[64];
            std::snprintf(b, sizeof b, " at %ux: %ux should fit", static_cast<unsigned>(scale_),
                          static_cast<unsigned>(n));
            reason_ = costs(fe, total) + b;
            scale_ = n;
            last_change_ = now;
            over_ = 0;
            return scale_;
        }
    }
    return 0;
}

void RenderScaleGovernor::failed(std::uint32_t scale) {
    scale_ = std::clamp<std::uint32_t>(scale, 1, kMaxRenderScale);
    retry_at_ = 1e300; // until the window grows
}

bool RenderScaleGovernor::take_warning(std::string *text) {
    if (warning_.empty() || warning_taken_) {
        return false;
    }
    warning_taken_ = true;
    if (text != nullptr) {
        *text = warning_;
    }
    return true;
}

std::string RenderScaleGovernor::status() const {
    char b[96];
    if (automatic_) {
        std::snprintf(b, sizeof b, "render %ux auto (window %ux)", static_cast<unsigned>(scale_),
                      static_cast<unsigned>(cap_));
    } else if (!warning_.empty()) {
        std::snprintf(b, sizeof b, "render %ux: OVER BUDGET", static_cast<unsigned>(scale_));
    } else {
        std::snprintf(b, sizeof b, "render %ux", static_cast<unsigned>(scale_));
    }
    return b;
}

} // namespace maplethief::frontend
