// Game Center on this computer (#154, ADR 0084); see local_game_center.h.
#include "local_game_center.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "maplethief/offline.h"
#include "price_line.h"

namespace maplethief::frontend {

namespace {

constexpr std::uint32_t kMaxEntries = 32;
// The rows that fit the screen under the heading and above the hint.
constexpr std::size_t kMaxRows = 20;

void hook(void *user, const char *category) {
    static_cast<LocalGameCenter *>(user)->open(category);
}

std::string padded(std::string s, std::size_t width, bool right) {
    if (s.size() >= width) {
        return s.substr(0, width);
    }
    const std::string pad(width - s.size(), ' ');
    return right ? pad + s : s + pad;
}

const char *category_title(const char *category) {
    return category != nullptr && std::strcmp(category, kTotalRanking) == 0 ? "Total ranking"
                                                                            : category;
}

// Saves the renderer's state and puts it back.
class RenderState {
  public:
    explicit RenderState(SDL_Renderer *r) : r_(r) {
        SDL_GetRenderScale(r_, &sx_, &sy_);
        SDL_GetRenderDrawColor(r_, &cr_, &cg_, &cb_, &ca_);
        SDL_GetRenderDrawBlendMode(r_, &blend_);
    }
    ~RenderState() {
        SDL_SetRenderScale(r_, sx_, sy_);
        SDL_SetRenderDrawBlendMode(r_, blend_);
        SDL_SetRenderDrawColor(r_, cr_, cg_, cb_, ca_);
    }
    RenderState(const RenderState &) = delete;
    RenderState &operator=(const RenderState &) = delete;

  private:
    SDL_Renderer *r_;
    float sx_ = 1, sy_ = 1;
    Uint8 cr_ = 0, cg_ = 0, cb_ = 0, ca_ = 0;
    SDL_BlendMode blend_ = SDL_BLENDMODE_NONE;
};

void gold(SDL_Renderer *r) { SDL_SetRenderDrawColor(r, 0xF2, 0xC4, 0x4C, SDL_ALPHA_OPAQUE); }
void white(SDL_Renderer *r) { SDL_SetRenderDrawColor(r, 0xF0, 0xF2, 0xF5, SDL_ALPHA_OPAQUE); }
void panel(SDL_Renderer *r, const SDL_FRect &box) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(r, 0x0A, 0x0D, 0x12, 0xE8);
    SDL_RenderFillRect(r, &box);
    gold(r);
    SDL_RenderRect(r, &box);
}

} // namespace

std::vector<std::string> leaderboard_lines(const std::vector<mt_gamekit_entry> &entries,
                                           const char *category) {
    std::vector<std::string> lines;
    lines.push_back(std::string("Leaderboard: ") + category_title(category) + " (this computer)");
    lines.emplace_back("");
    if (entries.empty()) {
        lines.emplace_back("No score yet: the game reports one when your");
        lines.emplace_back("character changes map or levels up.");
    } else {
        lines.push_back(padded("#", 4, false) + padded("Player", 12, false) +
                        padded("Best", 12, true) + padded("Last", 12, true) +
                        padded("Reports", 10, true));
        for (std::size_t i = 0; i < entries.size() && i < kMaxRows; i++) {
            const mt_gamekit_entry &e = entries[i];
            lines.push_back(padded(std::to_string(i + 1) + ".", 4, false) +
                            padded(e.player, 12, false) + padded(meso_text(e.best), 12, true) +
                            padded(meso_text(e.last), 12, true) +
                            padded(std::to_string(e.reports), 10, true));
        }
    }
    lines.emplace_back("");
    lines.emplace_back("Esc, Enter, A or a click: back to the menu");
    return lines;
}

std::vector<mt_gamekit_entry> leaderboard_entries(const char *category) {
    std::vector<mt_gamekit_entry> rows(kMaxEntries);
    const std::uint32_t n = mt_gamekit_entries(category, rows.data(), kMaxEntries);
    rows.resize(std::min(n, kMaxEntries));
    return rows;
}

std::vector<AchievementRow> achievement_rows() {
    std::vector<AchievementRow> rows;
    for (std::uint32_t i = 0; i < mt_achievements_count(); i++) {
        const mt_achievement *a = mt_achievement_at(i);
        rows.push_back(AchievementRow{a->title, a->description, mt_achievement_unlocked(i) != 0});
    }
    return rows;
}

std::string achievement_summary(const std::vector<AchievementRow> &rows) {
    const auto n =
        std::count_if(rows.begin(), rows.end(), [](const AchievementRow &r) { return r.unlocked; });
    return std::to_string(n) + " of " + std::to_string(rows.size()) + " unlocked";
}

std::string achievement_toast_text(std::uint32_t index) {
    const mt_achievement *a = mt_achievement_at(index);
    return a != nullptr ? std::string("Achievement unlocked: ") + a->title : std::string();
}

void LocalGameCenter::install(bool enabled) {
    enabled_ = enabled;
    mt_gamekit_set_leaderboard_hook(enabled ? hook : nullptr, enabled ? this : nullptr);
    mt_achievements_install(enabled ? 1 : 0);
    if (!enabled) {
        mt_achievement_clear_notices();
    }
}

void LocalGameCenter::open(const char *category) {
    category_ = category != nullptr ? category : "";
    const std::vector<mt_gamekit_entry> entries = leaderboard_entries(category_.c_str());
    lines_ = leaderboard_lines(entries, category_.c_str());
    open_ = true;
    SDL_Log("game: the local leaderboard of %s is open: %zu entries", category_.c_str(),
            entries.size());
}

void LocalGameCenter::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    mt_gamekit_leaderboard_close();
    SDL_Log("game: the local leaderboard is closed");
}

bool LocalGameCenter::handle(const SDL_Event &e) {
    if (!open_) {
        return false;
    }
    switch (e.type) {
    // Closed on the release, so that the press never reaches the game either.
    case SDL_EVENT_KEY_UP:
        switch (e.key.key) {
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            close();
            break;
        default:
            break;
        }
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
            e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST ||
            e.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
            close();
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_FINGER_UP:
        close();
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
        return true;
    default:
        return false; // window, device and lifecycle events go on
    }
}

void LocalGameCenter::tick(std::uint64_t now_ns) {
    // A new runtime drops the controller it waited with: nothing to close any more.
    if (open_ && mt_gamekit_leaderboard_is_open() == 0) {
        open_ = false;
    }
    if (!toast_.empty() && now_ns >= toast_until_) {
        toast_.clear();
    }
    std::uint32_t index = 0;
    if (toast_.empty() && mt_achievement_next_notice(&index) != 0) {
        toast_ = achievement_toast_text(index);
        toast_until_ = now_ns + kToastNs;
        SDL_Log("game: %s", toast_.c_str());
    }
}

void LocalGameCenter::draw(SDL_Renderer *renderer, int scale) const {
    if (renderer == nullptr || (!open_ && toast_.empty())) {
        return;
    }
    const RenderState keep(renderer);
    const float glyph = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    const float line_h = glyph + 3;
    const float pad = 6;
    int out_w = 0, out_h = 0;
    SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h);
    const float k = static_cast<float>(std::max(1, scale));
    SDL_SetRenderScale(renderer, k, k);
    const float w = static_cast<float>(out_w) / k;
    const float h = static_cast<float>(out_h) / k;
    if (open_) {
        std::size_t longest = 0;
        for (const std::string &l : lines_) {
            longest = std::max(longest, l.size());
        }
        const float bw = glyph * static_cast<float>(longest) + 2 * pad;
        const float bh = line_h * static_cast<float>(lines_.size()) - 3 + 2 * pad;
        const SDL_FRect box = {std::max(0.0f, (w - bw) / 2), std::max(0.0f, (h - bh) / 2), bw, bh};
        panel(renderer, box);
        float y = box.y + pad;
        for (std::size_t i = 0; i < lines_.size(); i++) {
            if (i == 0 || i + 1 == lines_.size()) {
                gold(renderer);
            } else {
                white(renderer);
            }
            SDL_RenderDebugText(renderer, box.x + pad, y, lines_[i].c_str());
            y += line_h;
        }
    }
    if (!toast_.empty()) {
        const float bw = glyph * static_cast<float>(toast_.size()) + 2 * pad;
        const float bh = glyph + 2 * pad;
        const SDL_FRect box = {std::max(0.0f, w - bw - 4), 4, bw, bh};
        panel(renderer, box);
        gold(renderer);
        SDL_RenderDebugText(renderer, box.x + pad, box.y + pad, toast_.c_str());
    }
}

} // namespace maplethief::frontend
