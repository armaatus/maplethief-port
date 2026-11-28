// Practice states (#409): practice_state.h.
#include "practice_state.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "actions.h"
#include "settings.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

bool good_category(const std::string &s) {
    return !s.empty() && s.size() <= kMaxRunCategoryBytes &&
           std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
           });
}

} // namespace

std::string practice_state_path(const std::string &data_dir, const std::string &category, int slot,
                                std::size_t split) {
    if (slot < 0 || slot > 2 || split == 0 || !good_category(category)) {
        return std::string();
    }
    const std::string name = "states/" + category + "/slot" + std::to_string(slot) + "-split" +
                             std::to_string(split) + ".state";
    if (data_dir.empty()) {
        return name;
    }
    const char last = data_dir.back();
    return data_dir + (last == '/' || last == '\\' ? "" : "/") + name;
}

bool practice_state_exists(const std::string &path) {
    std::error_code ec;
    return !path.empty() &&
           fs::is_regular_file(fs::path(std::u8string(path.begin(), path.end())), ec);
}

PracticeRequest PracticeStates::take(const Actions &actions, bool on) {
    const auto both = [&actions](std::uint32_t a) {
        return actions.presses(a, Context::kField) + actions.presses(a, Context::kMenu);
    };
    return take(both(MT_ACTION_PRACTICE_PICK), both(MT_ACTION_PRACTICE_LOAD), on);
}

PracticeRequest PracticeStates::take(std::uint64_t picks, std::uint64_t loads, bool on) {
    const bool pick = picks != seen_picks_;
    const bool load = loads != seen_loads_;
    seen_picks_ = picks;
    seen_loads_ = loads;
    if ((pick || load) && !on) {
        SDL_Log("practice: %s pressed with save_states off: nothing done",
                pick ? "practice_pick" : "practice_load");
        return PracticeRequest::kNone;
    }
    if (pick && load) {
        SDL_Log("practice: practice_pick and practice_load pressed together: neither");
        return PracticeRequest::kNone;
    }
    return pick ? PracticeRequest::kPick : load ? PracticeRequest::kLoad : PracticeRequest::kNone;
}

void PracticeStates::reached(std::size_t split, std::size_t splits) {
    if (split >= 1 && split < splits) {
        armed_ = split;
    }
}

QuickResult PracticeStates::take_armed(const QuickOps &ops, const std::string &data_dir,
                                       const std::string &category) {
    QuickResult out;
    if (!armed_) {
        return out;
    }
    std::string why;
    if (!ops.safe(&why)) {
        return out; // not yet: the next turn tries again
    }
    const std::size_t split = *armed_;
    armed_.reset();
    out = save_state_to(
        ops, [&](int slot) { return practice_state_path(data_dir, category, slot, split); },
        "practice state after split " + std::to_string(split));
    if (out.done) {
        stats_.saves++;
    } else {
        stats_.refused++;
    }
    SDL_Log("practice: %s", out.log.c_str());
    return out;
}

std::optional<std::size_t>
PracticeStates::pick(std::optional<std::size_t> picked, std::size_t splits, const QuickOps &ops,
                     const std::string &data_dir, const std::string &category,
                     const std::function<bool(const std::string &)> &exists) const {
    if (splits < 2) {
        return std::nullopt;
    }
    const int slot = ops.slot();
    const std::size_t last = splits - 1; // the last split's state is never taken
    const std::size_t from = picked && *picked >= 1 && *picked <= last ? *picked : last;
    for (std::size_t k = 1; k <= last; k++) {
        const std::size_t split = (from + k - 1) % last + 1;
        if (exists(practice_state_path(data_dir, category, slot, split))) {
            return split;
        }
    }
    return std::nullopt;
}

QuickResult PracticeStates::load(const QuickOps &ops, const std::string &data_dir,
                                 const std::string &category, std::size_t split) {
    QuickResult out = load_state_from(
        ops, [&](int slot) { return practice_state_path(data_dir, category, slot, split); },
        "practice_load of split " + std::to_string(split), Txt::practice_why_none);
    if (out.done) {
        stats_.loads++;
    } else {
        stats_.refused++;
    }
    SDL_Log("practice: %s", out.log.c_str());
    return out;
}

} // namespace maplethief::frontend
