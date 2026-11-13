// The logged performance session (#244, ADR 0145): see perf_session.h.
#include "perf_session.h"

#include <algorithm>
#include <cstdio>

namespace maplethief::frontend {

std::string perf_scene(std::int32_t mode, std::int32_t area, std::int32_t map) {
    if (mode < 0) {
        return "none";
    }
    if ((mode == 0 || mode == 1) && area >= 0 && map >= 0) {
        return "map:" + std::to_string(area) + "." + std::to_string(map);
    }
    if (mode >= 6 && mode <= 8) {
        return "load";
    }
    if (mode == 9 || mode == 10) {
        return "scene";
    }
    return "menu";
}

namespace {

std::string ms(std::uint64_t ns) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(ns) / 1e6);
    return buf;
}

} // namespace

std::string PerfSession::line(const Window &w) const {
    char head[96];
    std::snprintf(head, sizeof head, "perf: t=%.3f s=%.3f", static_cast<double>(w.end_ns) / 1e9,
                  static_cast<double>(w.span_ns) / 1e9);
    std::string out = head;
    out += " display=" + w.ctx.display + " scene=" + w.ctx.scene +
           " ff=" + std::to_string(w.ctx.ff) + " scale=" + std::to_string(w.ctx.scale) +
           " backend=" + w.ctx.backend + " frames=" + std::to_string(w.frames) +
           " ticks=" + std::to_string(w.ticks) + " held=" + std::to_string(w.held) +
           " held3=" + std::to_string(w.held3) + " over=" + std::to_string(w.over) +
           " tick_max=" + ms(w.tick_max_ns) + " guest=" + ms(w.guest) + " gfx=" + ms(w.gfx) +
           " take=" + ms(w.take) + " compose=" + ms(w.compose) + " draw=" + ms(w.draw) +
           " present=" + ms(w.present) + " wait=" + ms(w.wait) + " other=" + ms(w.other);
    return out;
}

std::optional<std::string> PerfSession::add(const FrameRecord &r, const PerfContext &ctx) {
    std::optional<std::string> closed;
    // A pause (no interval) or a new context ends the window; a pause is not counted.
    if (open_ && (r.interval_ns == 0 || !(open_->ctx == ctx))) {
        closed = flush();
    }
    if (r.interval_ns == 0) {
        return closed;
    }
    if (!open_) {
        open_ = Window{};
        open_->ctx = ctx;
    }
    Window &w = *open_;
    w.end_ns = r.at_ns;
    w.span_ns += r.interval_ns;
    w.frames++;
    if (r.tick) {
        w.ticks++;
        w.tick_max_ns = std::max(w.tick_max_ns, r.tick_ns);
        if (r.tick_ns > ctx.overrun_ns) {
            w.over++;
        }
    }
    if (r.vblanks >= 0) {
        w.held++;
        if (r.vblanks == 3) {
            w.held3++;
        }
    }
    w.guest += r.guest_ns;
    w.gfx += r.gfx_ns;
    w.take += r.take_ns;
    w.compose += r.compose_ns;
    w.draw += r.draw_ns;
    w.present += r.present_ns;
    w.wait += r.wait_ns;
    w.other += r.other_ns;
    if (!closed && w.span_ns >= kWindowNs) {
        closed = flush();
    }
    return closed;
}

std::optional<std::string> PerfSession::flush() {
    if (!open_) {
        return std::nullopt;
    }
    std::string out = line(*open_);
    open_.reset();
    lines_++;
    return out;
}

} // namespace maplethief::frontend
