// Crash recovery points (#335, ADR 0151): recovery.h.
#include "recovery.h"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

#include "autosave.h"
#include "maplethief/boot.h"
#include "maplethief/hooks.h"
#include "maplethief/snapshot.h"
#include "port_text.h"
#include "quick_state.h"

#ifndef MT_VERSION
#define MT_VERSION "unknown"
#endif

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

double megabytes(std::size_t n) { return static_cast<double>(n) / 1e6; }

// The game's loading modes (switch_applet.cpp's rule): a map load is a new map outside them.
bool loading(std::int32_t mode) { return mode >= 6 && mode <= 8; }

std::optional<fs::file_time_type> mtime(const fs::path &p) {
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time(p, ec);
    if (ec) {
        return std::nullopt;
    }
    return t;
}

void remove_file(const fs::path &p) {
    std::error_code ec;
    fs::remove(p, ec);
}

std::atomic<std::uint64_t> g_game_saves{0};

// A wrapper of CDataMgr::SaveQuestData: the pair is written once the call returns.
int count_game_save(mt_cpu *cpu, const mt_hook_call *call) {
    call->original(cpu);
    g_game_saves.fetch_add(1, std::memory_order_relaxed);
    return MT_HOOK_HANDLED;
}

} // namespace

std::string recovery_path(const std::string &data_dir, int n) {
    const std::string name = "recovery-" + std::to_string(n) + ".state";
    if (data_dir.empty()) {
        return "states/" + name;
    }
    const char last = data_dir.back();
    return data_dir + (last == '/' || last == '\\' ? "" : "/") + "states/" + name;
}

const char *recovery_trigger_name(RecoveryTrigger t) {
    switch (t) {
    case RecoveryTrigger::kMapLoad:
        return "a map load";
    case RecoveryTrigger::kGameSave:
        return "a game save";
    case RecoveryTrigger::kTimer:
        return "the timer";
    case RecoveryTrigger::kNone:
        break;
    }
    return "nothing";
}

std::string recovery_header_check(const std::vector<std::uint8_t> &bytes, std::int32_t *slot) {
    mt_snapshot_header h{};
    char why[256] = {};
    if (mt_snapshot_read_header(bytes.data(), bytes.size(), nullptr, &h, why, sizeof why) != 0) {
        return why;
    }
    if (std::strcmp(h.port_version, MT_VERSION) != 0) {
        return std::string("a state of port version ") + h.port_version + ", not " + MT_VERSION;
    }
    if (h.slot > 2) {
        return "no character slot";
    }
    *slot = static_cast<std::int32_t>(h.slot);
    return std::string();
}

void delete_recovery_points(const std::string &data_dir) {
    remove_file(utf8_path(recovery_path(data_dir, 1)));
    remove_file(utf8_path(recovery_path(data_dir, 2)));
}

RecoveryOffer find_recovery_point(const std::string &data_dir, const std::string &saves_dir,
                                  bool crashed, const std::string &crash_file,
                                  const RecoveryHeaderCheck &check) {
    RecoveryOffer offer;
    const fs::path files[2] = {utf8_path(recovery_path(data_dir, 1)),
                               utf8_path(recovery_path(data_dir, 2))};
    if (!crashed) {
        delete_recovery_points(data_dir);
        offer.why = "the last run ended cleanly";
        return offer;
    }
    for (const fs::path &file : files) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            continue;
        }
        const auto u8 = file.u8string();
        const std::string path(u8.begin(), u8.end());
        const std::optional<std::vector<std::uint8_t>> bytes = read_quick_state(path);
        std::int32_t slot = -1;
        std::string why = !bytes ? "it cannot be read" : check(*bytes, &slot);
        const std::optional<fs::file_time_type> taken = mtime(file);
        if (why.empty() && taken) {
            const fs::path saves = utf8_path(saves_dir);
            for (const char *stem : {"theif", "quest"}) {
                const std::optional<fs::file_time_type> saved =
                    mtime(saves / (stem + std::to_string(slot)));
                if (saved && *saved > *taken) {
                    why = "older than the character's last save";
                }
            }
        }
        if (!why.empty()) {
            SDL_Log("recovery: not offered (%s: %s)", file.filename().string().c_str(),
                    why.c_str());
            remove_file(file);
            offer.why = why;
            continue;
        }
        offer.offered = true;
        offer.path = path;
        offer.slot = slot;
        offer.why.clear();
        const std::optional<fs::file_time_type> crash =
            crash_file.empty() ? std::nullopt : mtime(utf8_path(crash_file));
        if (crash && taken) {
            offer.minutes_before = std::chrono::duration<double>(*crash - *taken).count() / 60.0;
            if (offer.minutes_before < 0) {
                offer.minutes_before = 0;
            }
        }
        SDL_Log("recovery: offered (%s, character %d)", file.filename().string().c_str(),
                static_cast<int>(slot + 1));
        return offer;
    }
    if (offer.why.empty()) {
        offer.why = "no recovery point";
        SDL_Log("recovery: not offered (no recovery point)");
    }
    return offer;
}

RecoveryPoints::~RecoveryPoints() { wait(); }

void RecoveryPoints::arm(const RecoveryOffer &offer) {
    if (!offer.offered) {
        return;
    }
    armed_slot_ = offer.slot;
    armed_path_ = offer.path;
    SDL_Log("recovery: armed for character %d (%s)", static_cast<int>(offer.slot + 1),
            offer.path.c_str());
}

void RecoveryPoints::wait() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void RecoveryPoints::clean_exit() {
    wait();
    if (!data_dir_.empty()) {
        delete_recovery_points(data_dir_);
    }
}

bool RecoveryPoints::writing() const { return busy_.load(std::memory_order_acquire); }

RecoveryPoints::Stats RecoveryPoints::stats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

void RecoveryPoints::skip(const std::string &why) {
    SDL_Log("recovery: point skipped: %s", why.c_str());
    std::lock_guard<std::mutex> lock(mu_);
    stats_.skipped++;
}

RecoveryTurn RecoveryPoints::resume(const RecoveryOps &ops, const RecoveryWhere &w) {
    RecoveryTurn out;
    const auto drop = [&](const std::string &why, const std::string &log) {
        SDL_Log("recovery: dropped (%s)", log.c_str());
        remove_file(utf8_path(armed_path_));
        armed_slot_ = -1;
        armed_path_.clear();
        out.dropped = true;
        out.notice = tr(Txt::recovery_dropped, {why});
        return out;
    };
    if (w.slot != armed_slot_) {
        return drop(tr(Txt::recovery_why_other_slot), "character " + std::to_string(w.slot + 1) +
                                                          " was loaded, not " +
                                                          std::to_string(armed_slot_ + 1));
    }
    // The safe point, but for held input: a restore lets go of it (quick_load's rule).
    std::string why;
    if (!ops.safe(&why) && !held_input_reason(why)) {
        return out; // a later turn
    }
    const std::optional<std::vector<std::uint8_t>> bytes = ops.read(armed_path_);
    if (!bytes) {
        return drop(tr(Txt::state_why_none), armed_path_ + " cannot be read");
    }
    const auto slot = static_cast<std::uint32_t>(armed_slot_);
    if (const std::string bad = ops.check(*bytes, slot); !bad.empty()) {
        return drop(bad, bad);
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!ops.restore(*bytes, slot, &why)) {
        if (held_input_reason(why) || why.rfind("not in the field", 0) == 0 ||
            why.rfind("a popup is open", 0) == 0 || why.rfind("an animation", 0) == 0 ||
            why.rfind("during a scene", 0) == 0 || why.rfind("background work", 0) == 0 ||
            why.rfind("a file is open", 0) == 0 || why.rfind("autoreleased", 0) == 0) {
            return out; // not a safe turn after all: a later one
        }
        return drop(quick_state_reason(why), why);
    }
    SDL_Log("recovery: resumed (character %d, %zu bytes in %.0f ms from %s)",
            static_cast<int>(armed_slot_ + 1), bytes->size(), ms_since(t0), armed_path_.c_str());
    // The restored point is now the live game: its file goes, and the next trigger writes a new
    // one. The older point, if any, is the newest now.
    remove_file(utf8_path(armed_path_));
    if (armed_path_ == recovery_path(data_dir_, 1)) {
        std::error_code ec;
        if (fs::is_regular_file(utf8_path(recovery_path(data_dir_, 2)), ec)) {
            fs::rename(utf8_path(recovery_path(data_dir_, 2)),
                       utf8_path(recovery_path(data_dir_, 1)), ec);
        }
    }
    armed_slot_ = -1;
    armed_path_.clear();
    // The triggers start over from the restored game: its map is the one now, its clock the
    // next turn's.
    const RecoveryWhere now = ops.where();
    area_ = now.area;
    map_ = now.map;
    since_.reset();
    pending_ = RecoveryTrigger::kNone;
    out.resumed = true;
    out.notice = tr(Txt::recovery_resumed);
    return out;
}

void RecoveryPoints::write_async(const RecoveryOps &ops, std::vector<std::uint8_t> stored) {
    wait(); // the last worker is done (busy_ said so): join it
    busy_.store(true, std::memory_order_release);
    worker_ = std::thread(
        [this, deflate = ops.deflate, write = ops.write, stored = std::move(stored)]() mutable {
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<std::uint8_t> file;
            std::string why;
            bool ok = deflate(stored, &file, &why);
            stored = std::vector<std::uint8_t>(); // the raw state's memory back before the write
            const std::string newest = recovery_path(data_dir_, 1);
            if (ok) {
                std::error_code ec;
                if (fs::is_regular_file(utf8_path(newest), ec)) {
                    fs::rename(utf8_path(newest), utf8_path(recovery_path(data_dir_, 2)), ec);
                }
                ok = write(newest, file);
                if (!ok) {
                    why = newest + " could not be written";
                }
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (ok) {
                    stats_.written++;
                    stats_.last_written = file.size();
                } else {
                    stats_.failed++;
                }
            }
            if (ok) {
                SDL_Log("recovery: point written: %.2f MB in %.0f ms", megabytes(file.size()),
                        ms_since(t0));
            } else {
                SDL_Log("recovery: point not written: %s", why.c_str());
            }
            busy_.store(false, std::memory_order_release);
        });
}

RecoveryTurn RecoveryPoints::turn(const RecoveryOps &ops, bool on, double t,
                                  std::uint64_t game_saves, bool busy) {
    RecoveryTurn out;
    const RecoveryWhere w = ops.where();
    if (armed() && w.mode == 0 && w.slot >= 0 && w.slot <= 2) {
        return resume(ops, w);
    }
    // The triggers, seen every turn whatever the setting, so that turning it on does not count
    // what happened before.
    RecoveryTrigger trigger = RecoveryTrigger::kNone;
    if (!loading(w.mode) && w.area >= 0 && w.map >= 0 && (w.area != area_ || w.map != map_)) {
        area_ = w.area;
        map_ = w.map;
        trigger = RecoveryTrigger::kMapLoad;
    }
    if (saves_seen_ && game_saves != saves_) {
        trigger = RecoveryTrigger::kGameSave;
    }
    saves_ = game_saves;
    saves_seen_ = true;
    if (!since_) {
        since_ = t;
    }
    if (trigger == RecoveryTrigger::kNone && t - *since_ >= interval_) {
        trigger = RecoveryTrigger::kTimer;
    }
    if (!on) {
        pending_ = RecoveryTrigger::kNone;
        since_ = t;
        return out;
    }
    if (trigger != RecoveryTrigger::kNone && pending_ == RecoveryTrigger::kNone) {
        pending_ = trigger;
    }
    if (pending_ == RecoveryTrigger::kNone || busy || w.slot < 0 || w.slot > 2) {
        return out;
    }
    if (writing()) {
        skip(std::string("the last point is still being written (") +
             recovery_trigger_name(pending_) + ")");
        pending_ = RecoveryTrigger::kNone;
        since_ = t;
        return out;
    }
    std::string why;
    if (!ops.safe(&why)) {
        return out; // the first safe turn takes it
    }
    if (ops.heap_free) {
        if (const std::optional<std::uint64_t> free = ops.heap_free()) {
            std::size_t last_raw = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                last_raw = stats_.last_raw;
            }
            const std::uint64_t need = ops.heap_margin + last_raw + last_raw / 4;
            if (*free < need) {
                skip("applet memory, " + std::to_string(*free >> 20) + " MiB free");
                pending_ = RecoveryTrigger::kNone;
                since_ = t;
                return out;
            }
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> stored;
    if (!ops.save_stored(static_cast<std::uint32_t>(w.slot), &stored, &why)) {
        skip(why);
        pending_ = RecoveryTrigger::kNone;
        since_ = t;
        return out;
    }
    points_++;
    SDL_Log("recovery: point %llu taken at %s (map %d.%d, slot %d): %.1f MB raw in %.0f ms",
            static_cast<unsigned long long>(points_), recovery_trigger_name(pending_),
            static_cast<int>(w.area), static_cast<int>(w.map), static_cast<int>(w.slot),
            megabytes(stored.size()), ms_since(t0));
    {
        std::lock_guard<std::mutex> lock(mu_);
        stats_.taken++;
        stats_.last_raw = stored.size();
    }
    out.taken = true;
    out.trigger = pending_;
    pending_ = RecoveryTrigger::kNone;
    since_ = t;
    write_async(ops, std::move(stored));
    return out;
}

void install_game_save_counter() {
    mt_hook_wrap(MT_HOOK_FN_SAVE_QUEST_DATA, &count_game_save, nullptr, 1);
}

std::uint64_t game_saves_seen() { return g_game_saves.load(std::memory_order_relaxed); }

RecoveryOps game_recovery_ops(mt_cpu *cpu, std::function<void()> release,
                              void (*rebase)(void *user, std::uint64_t guest_unix_us), void *user) {
    const QuickOps quick = game_quick_ops(cpu, std::move(release), rebase, user);
    auto sha = std::make_shared<std::array<std::uint8_t, 32>>();
    mt_process_image_sha256(mt_process_current(), sha->data());
    RecoveryOps ops;
    ops.safe = quick.safe;
    ops.where = [cpu] {
        const AutosaveCheck c = check_autosave(cpu);
        return RecoveryWhere{c.mode, c.area, c.map, c.slot};
    };
    ops.save_stored = [cpu, sha](std::uint32_t slot, std::vector<std::uint8_t> *out,
                                 std::string *why) {
        mt_snapshot_meta meta = mt_snapshot_meta_default();
        std::memcpy(meta.binary_sha256, sha->data(), 32);
        meta.slot = slot;
        meta.flags = MT_SNAPSHOT_STORE;
        std::uint8_t *bytes = nullptr;
        std::size_t size = 0;
        char buf[256] = {};
        if (mt_snapshot_save(cpu, &meta, &bytes, &size, buf, sizeof buf) != 0) {
            *why = buf;
            return false;
        }
        try {
            out->assign(bytes, bytes + size);
        } catch (const std::bad_alloc &) {
            std::free(bytes);
            *why = "out of memory";
            return false;
        }
        std::free(bytes);
        return true;
    };
    ops.deflate = [](const std::vector<std::uint8_t> &stored, std::vector<std::uint8_t> *out,
                     std::string *why) {
        std::uint8_t *bytes = nullptr;
        std::size_t size = 0;
        char buf[256] = {};
        if (mt_snapshot_deflate(stored.data(), stored.size(), &bytes, &size, buf, sizeof buf) !=
            0) {
            *why = buf;
            return false;
        }
        try {
            out->assign(bytes, bytes + size);
        } catch (const std::bad_alloc &) {
            std::free(bytes);
            *why = "out of memory";
            return false;
        }
        std::free(bytes);
        return true;
    };
    ops.write = write_quick_state;
    ops.read = read_quick_state;
    ops.check = quick.check;
    ops.restore = quick.restore;
    return ops;
}

} // namespace maplethief::frontend
