// Quick-save and quick-load (#204): quick_state.h.
#include "quick_state.h"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>

#include "actions.h"
#include "autosave.h"
#include "maplethief/boot.h"
#include "maplethief/saves.h"
#include "maplethief/snapshot.h"
#include "port_text.h"

#ifndef MT_VERSION
#define MT_VERSION "unknown"
#endif

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

bool starts_with(std::string_view s, std::string_view head) {
    return s.size() >= head.size() && s.substr(0, head.size()) == head;
}

// A state file larger than this is not read (the runtime's own limit is 768 MiB a section).
constexpr std::streamoff kMaxStateBytes = std::streamoff(1) << 30;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

// The safe point's, the save's and the restore's reasons (runtime/src/snapshot/) by their start.
struct Reason {
    const char *start;
    Txt text;
};
constexpr Reason kReasons[] = {
    {"no game is running", Txt::state_why_no_game},
    {"the game mode cannot be read", Txt::state_why_no_game},
    {"the popup cannot be read", Txt::state_why_no_game},
    {"inside a run-loop turn", Txt::state_why_busy},
    {"during a scene", Txt::state_why_scene},
    {"not in the field", Txt::state_why_menu},
    {"a popup is open", Txt::state_why_popup},
    {"a touch is down", Txt::state_why_held},
    {"a key is held", Txt::state_why_held},
    {"input is waiting", Txt::state_why_held},
    {"an animation is running", Txt::state_why_animation},
    {"background work is running", Txt::state_why_busy},
    {"a file is open", Txt::state_why_busy},
    {"autoreleased objects are waiting", Txt::state_why_busy},
};

std::string elapsed_ms(std::chrono::steady_clock::time_point t0) {
    char buf[32];
    std::snprintf(
        buf, sizeof buf, "%.0f ms",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return buf;
}

} // namespace

std::string quick_state_path(const std::string &data_dir, int slot) {
    if (slot < 0 || slot > 2) {
        return std::string();
    }
    const std::string name = "slot" + std::to_string(slot) + ".state";
    if (data_dir.empty()) {
        return "states/" + name;
    }
    const char last = data_dir.back();
    return data_dir + (last == '/' || last == '\\' ? "" : "/") + "states/" + name;
}

std::string quick_state_reason(const std::string &why) {
    for (const Reason &r : kReasons) {
        if (starts_with(why, r.start)) {
            return tr(r.text);
        }
    }
    return why;
}

bool held_input_reason(const std::string &why) {
    return starts_with(why, "a touch is down") || starts_with(why, "a key is held") ||
           starts_with(why, "input is waiting");
}

std::string check_quick_state(const std::vector<std::uint8_t> &bytes, const std::uint8_t sha[32],
                              std::uint32_t slot, const char *port_version) {
    mt_snapshot_expect ex = mt_snapshot_expect_default();
    ex.binary_sha256 = sha;
    ex.slot = slot;
    mt_snapshot_header h{};
    char why[256] = {};
    if (mt_snapshot_read_header(bytes.data(), bytes.size(), &ex, &h, why, sizeof why) != 0) {
        const std::string_view w = why;
        if (starts_with(w, "a state of another game binary")) {
            return tr(Txt::state_why_other_binary);
        }
        if (starts_with(w, "a state of character slot")) {
            return tr(Txt::state_why_other_slot);
        }
        if (starts_with(w, "format version ")) {
            // "format version N, this port reads M"
            const std::string_view n = w.substr(15, w.find(',') - 15);
            return tr(Txt::state_why_other_version, {"format " + std::string(n)});
        }
        return tr(Txt::state_why_damaged);
    }
    if (port_version != nullptr && std::strcmp(h.port_version, port_version) != 0) {
        return tr(Txt::state_why_other_version, {h.port_version});
    }
    return std::string();
}

bool write_quick_state(const std::string &path, const std::vector<std::uint8_t> &bytes) {
    return mt_saves_replace_file(path.c_str(), bytes.data(), bytes.size()) == 0;
}

std::optional<std::vector<std::uint8_t>> read_quick_state(const std::string &path) {
    std::ifstream f(utf8_path(path), std::ios::binary | std::ios::ate);
    if (!f) {
        return std::nullopt;
    }
    const std::streamoff size = f.tellg();
    if (size < 0 || size > kMaxStateBytes) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    f.seekg(0);
    if (size > 0 &&
        !f.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return bytes;
}

const char *quick_request_name(QuickRequest r) {
    switch (r) {
    case QuickRequest::kSave:
        return "quick_save";
    case QuickRequest::kLoad:
        return "quick_load";
    case QuickRequest::kNone:
        break;
    }
    return "none";
}

QuickRequest QuickStates::take(const Actions &actions) {
    const auto both = [&actions](std::uint32_t a) {
        return actions.presses(a, Context::kField) + actions.presses(a, Context::kMenu);
    };
    return take(both(MT_ACTION_QUICK_SAVE), both(MT_ACTION_QUICK_LOAD));
}

QuickRequest QuickStates::take(std::uint64_t saves, std::uint64_t loads) {
    const bool save = saves != seen_saves_;
    const bool load = loads != seen_loads_;
    seen_saves_ = saves;
    seen_loads_ = loads;
    if (save && load) {
        SDL_Log("save state: quick_save and quick_load pressed together: neither");
        return QuickRequest::kNone;
    }
    return save ? QuickRequest::kSave : load ? QuickRequest::kLoad : QuickRequest::kNone;
}

QuickResult QuickStates::run(QuickRequest r, const QuickOps &ops, const std::string &data_dir) {
    QuickResult out;
    if (r == QuickRequest::kSave) {
        out = save(ops, data_dir);
    } else if (r == QuickRequest::kLoad) {
        out = load(ops, data_dir);
    } else {
        return out;
    }
    if (!out.done) {
        stats_.refused++;
    }
    SDL_Log("save state: %s", out.log.c_str());
    return out;
}

QuickResult QuickStates::save(const QuickOps &ops, const std::string &data_dir) {
    QuickResult out;
    out.request = QuickRequest::kSave;
    const auto refuse = [&out](const std::string &why, const std::string &log) {
        out.notice = tr(Txt::state_save_refused, {why});
        out.log = "quick_save refused: " + log;
        return out;
    };
    std::string why;
    if (!ops.safe(&why)) {
        return refuse(quick_state_reason(why), why);
    }
    const int slot = ops.slot();
    const std::string path = quick_state_path(data_dir, slot);
    if (path.empty()) {
        return refuse(tr(Txt::state_why_no_game), "no character slot in play");
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> bytes;
    if (!ops.save(static_cast<std::uint32_t>(slot), &bytes, &why)) {
        return refuse(quick_state_reason(why), why);
    }
    if (!ops.write(path, bytes)) {
        return refuse(tr(Txt::state_why_not_written), path + " could not be written");
    }
    stats_.saves++;
    stats_.last_bytes = bytes.size();
    out.done = true;
    out.notice = tr(Txt::state_saved);
    out.log = "quick_save: slot " + std::to_string(slot) + ", " + std::to_string(bytes.size()) +
              " bytes in " + elapsed_ms(t0) + " to " + path;
    return out;
}

QuickResult QuickStates::load(const QuickOps &ops, const std::string &data_dir) {
    QuickResult out;
    out.request = QuickRequest::kLoad;
    const auto refuse = [&out](const std::string &why, const std::string &log) {
        out.notice = tr(Txt::state_load_refused, {why});
        out.log = "quick_load refused: " + log;
        return out;
    };
    // The safe point, but for held input: a load lets go of it (the state's own replaces it).
    // mt_snapshot_restore checks the rest of the point again.
    std::string why;
    if (!ops.safe(&why) && !held_input_reason(why)) {
        return refuse(quick_state_reason(why), why);
    }
    const int slot = ops.slot();
    const std::string path = quick_state_path(data_dir, slot);
    if (path.empty()) {
        return refuse(tr(Txt::state_why_no_game), "no character slot in play");
    }
    const std::optional<std::vector<std::uint8_t>> bytes = ops.read(path);
    if (!bytes) {
        return refuse(tr(Txt::state_why_none), "no state at " + path);
    }
    if (const std::string bad = ops.check(*bytes, static_cast<std::uint32_t>(slot));
        !bad.empty()) {
        return refuse(bad, path + ": " + bad);
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!ops.restore(*bytes, static_cast<std::uint32_t>(slot), &why)) {
        return refuse(quick_state_reason(why), why);
    }
    stats_.loads++;
    stats_.last_bytes = bytes->size();
    out.done = true;
    out.notice = tr(Txt::state_loaded);
    out.log = "quick_load: slot " + std::to_string(slot) + ", " + std::to_string(bytes->size()) +
              " bytes in " + elapsed_ms(t0) + " from " + path;
    return out;
}

QuickOps game_quick_ops(mt_cpu *cpu, std::function<void()> release,
                        void (*rebase)(void *user, std::uint64_t guest_unix_us), void *user) {
    // The binary's hash, once (mt_process_image_sha256 hashes the slice).
    auto sha = std::make_shared<std::array<std::uint8_t, 32>>();
    mt_process_image_sha256(mt_process_current(), sha->data());
    QuickOps ops;
    ops.safe = [cpu](std::string *why) {
        char buf[256] = {};
        if (mt_snapshot_safe(cpu, buf, sizeof buf) == 1) {
            return true;
        }
        *why = buf;
        return false;
    };
    ops.slot = [cpu] {
        const AutosaveCheck c = check_autosave(cpu);
        return c.slot >= 0 && c.slot <= 2 ? static_cast<int>(c.slot) : -1;
    };
    ops.save = [cpu, sha](std::uint32_t slot, std::vector<std::uint8_t> *out, std::string *why) {
        mt_snapshot_meta meta = mt_snapshot_meta_default();
        std::memcpy(meta.binary_sha256, sha->data(), 32);
        meta.slot = slot;
        std::uint8_t *bytes = nullptr;
        std::size_t size = 0;
        char buf[256] = {};
        if (mt_snapshot_save(cpu, &meta, &bytes, &size, buf, sizeof buf) != 0) {
            *why = buf;
            return false;
        }
        out->assign(bytes, bytes + size);
        std::free(bytes);
        return true;
    };
    ops.check = [sha](const std::vector<std::uint8_t> &bytes, std::uint32_t slot) {
        return check_quick_state(bytes, sha->data(), slot, MT_VERSION);
    };
    ops.restore = [cpu, sha, release = std::move(release), rebase,
                   user](const std::vector<std::uint8_t> &bytes, std::uint32_t slot,
                         std::string *why) {
        if (release) {
            release();
        }
        mt_snapshot_expect ex = mt_snapshot_expect_default();
        ex.binary_sha256 = sha->data();
        ex.slot = slot;
        ex.rebase_clock = rebase;
        ex.user = user;
        char buf[256] = {};
        if (mt_snapshot_restore(cpu, bytes.data(), bytes.size(), &ex, nullptr, buf, sizeof buf) !=
            0) {
            *why = buf;
            return false;
        }
        return true;
    };
    ops.write = write_quick_state;
    ops.read = read_quick_state;
    return ops;
}

} // namespace maplethief::frontend
