// Crash recovery points (#335, ADR 0151, recovery.h): the crash_recovery row, the triggers (a
// map load outside the loading modes, a game save, five minutes of the guest's time), the first
// safe turn taking a point, nothing taken with the setting off, the files' rotation through the
// worker, a point skipped while the last is written and for the Switch's applet memory, the
// start's offer (a damaged, foreign-version or older-than-its-save point refused and deleted;
// none kept after a clean run), the armed resume and its drop, and #198's four-button decision.
// The game's side is a fake (RecoveryOps); the real save and restore are snapshot_test's
// (synthetic) and play.recovery_crash / play.recovery_resume's (real assets). Every state file
// here is synthetic, in a temporary directory.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "crash_report.h"
#include "maplethief/snapshot.h"
#include "quick_state.h"
#include "recovery.h"
#include "settings.h"
#include "snapshot/state_file.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using Bytes = std::vector<u8>;
using fe::RecoveryTrigger;

fs::path fresh_dir(const char *name) {
    const fs::path dir = fs::temp_directory_path() / "maplethief_recovery_test" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "states");
    fs::create_directories(dir / "saves");
    return dir;
}

Bytes slurp(const fs::path &p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void put(const fs::path &p, const Bytes &b) {
    std::ofstream(p, std::ios::binary)
        .write(reinterpret_cast<const char *>(b.data()), static_cast<std::streamsize>(b.size()));
}

// A synthetic state file of `slot` written by port `version` (no game: one small section).
Bytes state_file(u32 slot, const char *version, u8 fill = 0x11) {
    mt_snapshot_header h{};
    h.format = MT_SNAPSHOT_FORMAT;
    std::memset(h.binary_sha256, 0xAB, 32);
    std::snprintf(h.port_version, sizeof h.port_version, "%s", version);
    h.slot = slot;
    h.guest_unix_us = 1735689600000000ull;
    std::vector<mt_snap::Section> sections;
    sections.push_back(mt_snap::Section{"alpha", Bytes(300, fill)});
    Bytes out;
    std::string why;
    REQUIRE(mt_snap::write_state_file(h, sections, false, &out, &why));
    return out;
}

// The fake game: where it is, whether the turn is safe, what a stored save gives; the files are
// real (write_quick_state into a temporary directory).
struct Fake {
    fe::RecoveryWhere where{0, 2, 8, 0};
    std::string unsafe; // "" safe; else mt_snapshot_safe's reason
    std::string bad;    // "" the armed point checks; else check's reason
    std::string no_restore;
    u8 next = 1; // the stored save's content: a counter
    std::vector<std::string> calls;
    std::optional<std::uint64_t> free_bytes;
    // The worker's deflate waits while `hold` is set.
    std::mutex mu;
    std::condition_variable cv;
    bool hold = false;

    fe::RecoveryOps ops() {
        fe::RecoveryOps o;
        o.safe = [this](std::string *why) {
            *why = unsafe;
            return unsafe.empty();
        };
        o.where = [this] { return where; };
        o.save_stored = [this](u32 slot, Bytes *out, std::string *) {
            calls.push_back("save " + std::to_string(slot));
            *out = Bytes{0x53, next++, static_cast<u8>(slot)};
            return true;
        };
        o.deflate = [this](const Bytes &stored, Bytes *out, std::string *) {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [this] { return !hold; });
            *out = stored;
            out->push_back(0xDF); // "deflated"
            return true;
        };
        o.write = fe::write_quick_state;
        o.read = fe::read_quick_state;
        o.check = [this](const Bytes &, u32 slot) {
            calls.push_back("check " + std::to_string(slot));
            return bad;
        };
        o.restore = [this](const Bytes &b, u32 slot, std::string *why) {
            calls.push_back("restore " + std::to_string(slot) + " " +
                            std::to_string(b.empty() ? -1 : b[0]));
            *why = no_restore;
            return no_restore.empty();
        };
        if (free_bytes) {
            o.heap_free = [this] { return free_bytes; };
            o.heap_margin = 64;
        }
        return o;
    }
    void release() {
        {
            std::lock_guard<std::mutex> lock(mu);
            hold = false;
        }
        cv.notify_all();
    }
};

} // namespace

TEST_CASE("recovery: crash_recovery is a Gameplay row: off, off, on, a flag, read at once") {
    const fe::SettingInfo *info = fe::find_setting("crash_recovery");
    REQUIRE(info != nullptr);
    CHECK(info->group == fe::SettingGroup::kGameplay);
    CHECK(std::string(info->flag) == "--crash-recovery");
    CHECK(info->game_only);
    CHECK(info->applies == fe::Applies::kAtOnce);
    CHECK(std::string(info->presets[fe::Preset::kOriginal]) == "off");
    CHECK(std::string(info->presets[fe::Preset::kOriginalController]) == "off");
    CHECK(std::string(info->presets[fe::Preset::kEnhanced]) == "on");
    fe::Settings s;
    CHECK_FALSE(s.crash_recovery);
    std::string why;
    REQUIRE(fe::apply_setting(&s, "crash_recovery", "on", &why));
    CHECK(s.crash_recovery);
    fe::apply_preset(&s, fe::Preset::kOriginal);
    CHECK_FALSE(s.crash_recovery);
    fe::apply_preset(&s, fe::Preset::kEnhanced);
    CHECK(s.crash_recovery == (fe::kSettingsPlatform != fe::SettingsPlatform::kSwitch));
}

TEST_CASE("recovery: the files are states/recovery-1.state and recovery-2.state") {
    CHECK(fe::recovery_path("/d", 1) == "/d/states/recovery-1.state");
    CHECK(fe::recovery_path("/d/", 2) == "/d/states/recovery-2.state");
    CHECK(fe::recovery_path("", 1) == "states/recovery-1.state");
    CHECK(std::string(fe::recovery_trigger_name(RecoveryTrigger::kMapLoad)) == "a map load");
    CHECK(std::string(fe::recovery_trigger_name(RecoveryTrigger::kGameSave)) == "a game save");
    CHECK(std::string(fe::recovery_trigger_name(RecoveryTrigger::kTimer)) == "the timer");
}

TEST_CASE("recovery: a point at the first safe turn after a map load, a game save or five "
          "minutes; none outside them") {
    const fs::path dir = fresh_dir("triggers");
    Fake fake;
    fe::RecoveryPoints rp;
    rp.set_data_dir(dir.string());
    // The title: no slot, nothing; then the field's first map, while a key is held: the point
    // waits for the first safe turn.
    fake.where = {3, -1, -1, -1};
    CHECK_FALSE(rp.turn(fake.ops(), true, 0.0, 0).taken);
    fake.where = {0, 2, 8, 0};
    fake.unsafe = "a key is held";
    CHECK_FALSE(rp.turn(fake.ops(), true, 1.0, 0).taken);
    fake.unsafe.clear();
    // Nor in the turn of a quick-save or quick-load.
    CHECK_FALSE(rp.turn(fake.ops(), true, 1.05, 0, true).taken);
    fe::RecoveryTurn t = rp.turn(fake.ops(), true, 1.1, 0);
    CHECK(t.taken);
    CHECK(t.trigger == RecoveryTrigger::kMapLoad);
    rp.wait();
    // Nothing more on the same map.
    CHECK_FALSE(rp.turn(fake.ops(), true, 2.0, 0).taken);
    // A loading mode is not a map load; the map after it is.
    fake.where = {7, 2, 9, 0};
    CHECK_FALSE(rp.turn(fake.ops(), true, 3.0, 0).taken);
    fake.where = {0, 2, 9, 0};
    t = rp.turn(fake.ops(), true, 4.0, 0);
    CHECK((t.taken && t.trigger == RecoveryTrigger::kMapLoad));
    rp.wait();
    // A game save (SaveQuestData called).
    t = rp.turn(fake.ops(), true, 5.0, 1);
    CHECK((t.taken && t.trigger == RecoveryTrigger::kGameSave));
    rp.wait();
    // Five minutes of the guest's time since the last point.
    CHECK_FALSE(rp.turn(fake.ops(), true, 5.0 + fe::kRecoveryIntervalSeconds - 0.1, 1).taken);
    t = rp.turn(fake.ops(), true, 5.0 + fe::kRecoveryIntervalSeconds, 1);
    CHECK((t.taken && t.trigger == RecoveryTrigger::kTimer));
    rp.wait();
    CHECK(rp.stats().taken == 4);
    CHECK(rp.stats().written == 4);
    CHECK(fake.calls == std::vector<std::string>{"save 0", "save 0", "save 0", "save 0"});
}

TEST_CASE("recovery: with crash_recovery off no point is taken and no file is written, and what "
          "happened while off does not fire later") {
    const fs::path dir = fresh_dir("off");
    Fake fake;
    fe::RecoveryPoints rp;
    rp.set_data_dir(dir.string());
    CHECK_FALSE(rp.turn(fake.ops(), false, 0.0, 0).taken); // the map load
    CHECK_FALSE(rp.turn(fake.ops(), false, 1.0, 3).taken); // a game save
    CHECK_FALSE(rp.turn(fake.ops(), false, 1000.0, 3).taken);
    rp.wait();
    CHECK(fake.calls.empty());
    CHECK(fs::is_empty(dir / "states"));
    // On now: none of those counts; the next trigger does.
    CHECK_FALSE(rp.turn(fake.ops(), true, 1001.0, 3).taken);
    fake.where.map = 9;
    CHECK(rp.turn(fake.ops(), true, 1002.0, 3).taken);
    rp.wait();
}

TEST_CASE("recovery: the worker writes recovery-1, the one before goes to recovery-2, the "
          "newest two kept; a clean exit deletes both") {
    const fs::path dir = fresh_dir("rotation");
    Fake fake;
    fe::RecoveryPoints rp;
    rp.set_data_dir(dir.string());
    const fs::path one = dir / "states" / "recovery-1.state";
    const fs::path two = dir / "states" / "recovery-2.state";
    for (int i = 0; i < 3; i++) {
        fake.where.map = 10 + i;
        REQUIRE(rp.turn(fake.ops(), true, i, 0).taken);
        rp.wait();
    }
    CHECK(slurp(one) == Bytes{0x53, 3, 0, 0xDF}); // the third point, "deflated"
    CHECK(slurp(two) == Bytes{0x53, 2, 0, 0xDF});
    CHECK(rp.stats().last_raw == 3);
    CHECK(rp.stats().last_written == 4);
    std::size_t files = 0;
    for ([[maybe_unused]] const auto &e : fs::directory_iterator(dir / "states")) {
        files++;
    }
    CHECK(files == 2); // no temporary file left (mt_saves_replace_file renamed it)
    rp.clean_exit();
    CHECK_FALSE(fs::exists(one));
    CHECK_FALSE(fs::exists(two));
}

TEST_CASE("recovery: a point due while the last is being written is skipped; the next trigger "
          "takes one") {
    const fs::path dir = fresh_dir("skip");
    Fake fake;
    fake.hold = true;
    fe::RecoveryPoints rp;
    rp.set_data_dir(dir.string());
    REQUIRE(rp.turn(fake.ops(), true, 0.0, 0).taken);
    CHECK(rp.writing());
    CHECK_FALSE(rp.turn(fake.ops(), true, 1.0, 1).taken); // a game save, while it writes
    CHECK(rp.stats().skipped == 1);
    fake.release();
    rp.wait();
    CHECK_FALSE(rp.writing());
    CHECK_FALSE(rp.turn(fake.ops(), true, 2.0, 1).taken); // the skipped one is gone
    CHECK(rp.turn(fake.ops(), true, 3.0, 2).taken);
    rp.wait();
    CHECK(rp.stats().written == 2);
}

TEST_CASE("recovery: in the Switch's applet mode a point needs the margin plus 1.25 times the "
          "last one free") {
    const fs::path dir = fresh_dir("applet");
    Fake fake;
    fake.free_bytes = 64; // the margin, no room for anything
    fe::RecoveryPoints rp;
    rp.set_data_dir(dir.string());
    // The first point: nothing measured yet, the margin is enough.
    REQUIRE(rp.turn(fake.ops(), true, 0.0, 0).taken);
    rp.wait();
    // Now 3 bytes raw: 64 + 3 + 0 (3 / 4) wanted.
    fake.free_bytes = 66;
    CHECK_FALSE(rp.turn(fake.ops(), true, 1.0, 1).taken);
    CHECK(rp.stats().skipped == 1);
    fake.free_bytes = 67;
    CHECK(rp.turn(fake.ops(), true, 2.0, 2).taken);
    rp.wait();
}

TEST_CASE("recovery: the start offers the newest point that reads; a damaged, other-version or "
          "older-than-its-save one is refused and deleted") {
    const fs::path dir = fresh_dir("offer");
    const fs::path one = dir / "states" / "recovery-1.state";
    const fs::path two = dir / "states" / "recovery-2.state";
    const fs::path crash = dir / "crash.txt";
    put(crash, Bytes{'x'});
    const auto ok_check = [](const Bytes &b, std::int32_t *slot) {
        *slot = b.size() > 1 ? b[1] : 0;
        return std::string();
    };
    // A clean run before: nothing offered, both deleted.
    put(one, Bytes{0, 1});
    put(two, Bytes{0, 1});
    fe::RecoveryOffer o =
        fe::find_recovery_point(dir.string(), (dir / "saves").string(), false, "", ok_check);
    CHECK_FALSE(o.offered);
    CHECK_FALSE(fs::exists(one));
    CHECK_FALSE(fs::exists(two));
    // After a crash: the newest, its slot, the minutes before the crash report.
    put(one, Bytes{0, 2});
    put(two, Bytes{0, 2});
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(one, now - std::chrono::minutes(4));
    fs::last_write_time(crash, now);
    o = fe::find_recovery_point(dir.string(), (dir / "saves").string(), true, crash.string(),
                                ok_check);
    CHECK(o.offered);
    CHECK(fs::path(o.path).lexically_normal() ==
          one.lexically_normal()); // "/" joins on Windows too
    CHECK(o.slot == 2);
    CHECK(o.minutes_before == doctest::Approx(4.0).epsilon(0.01));
    CHECK(fe::recovery_offer_text(o) ==
          "A recovery point of character 3, taken 4 minutes before the crash, is kept.");
    // The real header check: a damaged newest file is deleted, the older one offered.
    put(one, state_file(1, MT_VERSION));
    std::fstream(one, std::ios::in | std::ios::out | std::ios::binary).seekp(120).put('\x7F');
    put(two, state_file(0, "0.0.0-other"));
    o = fe::find_recovery_point(dir.string(), (dir / "saves").string(), true, "");
    CHECK_FALSE(o.offered);
    CHECK(o.why == "a state of port version 0.0.0-other, not " MT_VERSION);
    CHECK_FALSE(fs::exists(one));
    CHECK_FALSE(fs::exists(two));
    // A point of this version is offered without a crash report's minutes.
    put(two, state_file(1, MT_VERSION));
    o = fe::find_recovery_point(dir.string(), (dir / "saves").string(), true, "");
    CHECK(o.offered);
    CHECK(fs::path(o.path).lexically_normal() == two.lexically_normal());
    CHECK(o.slot == 1);
    CHECK(o.minutes_before < 0);
    CHECK(fe::recovery_offer_text(o) == "A recovery point of character 2, is kept.");
    // The character saved after the point: it would go back before progress already on disk.
    put(dir / "saves" / "quest1", Bytes{1});
    fs::last_write_time(two, fs::file_time_type::clock::now() - std::chrono::minutes(1));
    o = fe::find_recovery_point(dir.string(), (dir / "saves").string(), true, "");
    CHECK_FALSE(o.offered);
    CHECK(o.why == "older than the character's last save");
    CHECK_FALSE(fs::exists(two));
}

TEST_CASE("recovery: an armed point is restored at the first safe turn in the field with its "
          "character, then its file goes and the older one is the newest") {
    const fs::path dir = fresh_dir("resume");
    const fs::path one = dir / "states" / "recovery-1.state";
    const fs::path two = dir / "states" / "recovery-2.state";
    put(one, Bytes{0x51});
    put(two, Bytes{0x52});
    Fake fake;
    fe::RecoveryPoints rp;
    rp.set_data_dir(dir.string());
    fe::RecoveryOffer offer;
    offer.offered = true;
    offer.path = fe::recovery_path(dir.string(), 1);
    offer.slot = 1;
    rp.arm(offer);
    CHECK(rp.armed());
    // The title and the character select: nothing.
    fake.where = {3, -1, -1, 1};
    CHECK_FALSE(rp.turn(fake.ops(), true, 0.0, 0).resumed);
    // The field, a popup open: a later turn. Held keys are let go of by the restore.
    fake.where = {0, 2, 0, 1};
    fake.unsafe = "a popup is open";
    CHECK_FALSE(rp.turn(fake.ops(), true, 1.0, 0).resumed);
    fake.unsafe = "a key is held";
    const fe::RecoveryTurn t = rp.turn(fake.ops(), true, 2.0, 0);
    CHECK(t.resumed);
    CHECK(t.notice == "Resumed from the recovery point");
    CHECK_FALSE(t.taken); // the restored game is the point: no new one in the same turn
    CHECK_FALSE(rp.armed());
    CHECK(fake.calls == std::vector<std::string>{"check 1", "restore 1 81"});
    CHECK(slurp(one) == Bytes{0x52}); // the older point is the newest now
    CHECK_FALSE(fs::exists(two));
    // The restored map is the current one: no map-load point for it.
    fake.unsafe.clear();
    CHECK_FALSE(rp.turn(fake.ops(), true, 3.0, 0).taken);
}

TEST_CASE("recovery: an armed point is dropped for another character, or a refused check") {
    const fs::path dir = fresh_dir("drop");
    const fs::path one = dir / "states" / "recovery-1.state";
    fe::RecoveryOffer offer;
    offer.offered = true;
    offer.path = fe::recovery_path(dir.string(), 1);
    offer.slot = 0;
    {
        put(one, Bytes{1});
        Fake fake;
        fake.where = {0, 2, 0, 2};
        fe::RecoveryPoints rp;
        rp.set_data_dir(dir.string());
        rp.arm(offer);
        const fe::RecoveryTurn t = rp.turn(fake.ops(), true, 0.0, 0);
        CHECK(t.dropped);
        CHECK(t.notice == "The recovery point was not used: another character was loaded");
        CHECK_FALSE(rp.armed());
        CHECK_FALSE(fs::exists(one));
        CHECK(fake.calls.empty());
    }
    {
        put(one, Bytes{1});
        Fake fake;
        fake.bad = "the state is of another copy of the game";
        fake.where = {0, 2, 0, 0};
        fe::RecoveryPoints rp;
        rp.set_data_dir(dir.string());
        rp.arm(offer);
        const fe::RecoveryTurn t = rp.turn(fake.ops(), true, 0.0, 0);
        CHECK(t.dropped);
        CHECK(t.notice ==
              "The recovery point was not used: the state is of another copy of the game");
        CHECK_FALSE(fs::exists(one));
    }
}

TEST_CASE("recovery: #198's box with a point: Resume, Resume with default settings, Start with "
          "default settings, Start normally") {
    fe::LastRun last;
    last.crashed = true;
    last.crash_file = "crashes/x.txt";
    fe::RecoveryOffer offer;
    offer.offered = true;
    offer.slot = 0;
    struct Case {
        fe::RecoveryChoice choice;
        bool safe, resume;
    };
    const Case cases[] = {
        {fe::RecoveryChoice::kResume, false, true},
        {fe::RecoveryChoice::kResumeDefaults, true, true},
        {fe::RecoveryChoice::kStartDefaults, true, false},
        {fe::RecoveryChoice::kStartNormal, false, false},
    };
    for (const Case &c : cases) {
        int asked = 0;
        fe::set_recovery_prompt_for_test([&](const std::string &file, const fe::RecoveryOffer &o) {
            asked++;
            CHECK(file == "crashes/x.txt");
            CHECK(o.offered);
            return c.choice;
        });
        const fe::StartDecision d = fe::decide_start(false, last, true, offer);
        CHECK(asked == 1);
        CHECK(d.safe_start == c.safe);
        CHECK(d.resume == c.resume);
    }
    fe::set_recovery_prompt_for_test(nullptr);
    // --safe-start: the safe start without asking, no resume. A scripted run: neither.
    fe::StartDecision d = fe::decide_start(true, last, true, offer);
    CHECK((d.safe_start && !d.resume));
    d = fe::decide_start(false, last, false, offer);
    CHECK((!d.safe_start && !d.resume));
    // Without a point: #198's own question.
    fe::set_safe_start_prompt_for_test(
        [](const std::string &) { return fe::SafeStartChoice::kDefaults; });
    d = fe::decide_start(false, last, true, fe::RecoveryOffer{});
    CHECK((d.safe_start && !d.resume));
    fe::set_safe_start_prompt_for_test(nullptr);
}
