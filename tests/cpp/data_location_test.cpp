// The data folder's resolver, layout and migration (#261, ADR 0127, data_location.h): every
// rule of the precedence on injected inputs (the environment, --data-location, user-data.txt,
// the Applications folder, the game folder's probe and the fallbacks with their reasons), the
// base folder of an app bundle and of the Linux game folder, iOS, the log and --print-data-dir
// lines, the save location it configures, and the in-place migration of an old layout: byte
// for byte, a second run that does nothing and an interrupted run that is finished.
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "data_location.h"
#include "game.h"
#include "maplethief/saves.h"
#include "maplethief/stubs.h"
#include "save_tools.h"
#include "settings.h"

namespace {

namespace fs = std::filesystem;
namespace fe = maplethief::frontend;
using Place = fe::DataPlace;
using Reason = fe::DataReason;
using Tree = std::map<std::string, std::string>; // a folder's files, by path below it

struct TempDir {
    fs::path path;
    explicit TempDir(const char *name) {
        path = fs::temp_directory_path() / (std::string("maplethief-data-location-") + name);
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path &p, const std::string &text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
std::string read_text(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
Tree tree(const fs::path &dir) {
    Tree t;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return t;
    }
    for (const auto &e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) {
            t[fs::relative(e.path(), dir).generic_string()] = read_text(e.path());
        }
    }
    return t;
}
std::string str(const fs::path &p) { return p.string(); }

// Inputs for a desktop run from `exe_dir` with the per-user folder `user`; counts the calls.
struct Fake {
    int user_calls = 0;
    int probe_calls = 0;
    fe::DataInputs in;
    Fake(const fs::path &exe_dir, const fs::path &user, fe::DataInputs::Os os) {
        in.base_path = exe_dir.empty() ? std::string() : str(exe_dir) + "/";
        in.os = os;
        in.home = "/Users/player";
        in.user_dir = [this, user] {
            user_calls++;
            return str(user);
        };
    }
    void probe_says(bool ok, const std::string &why = "") {
        in.probe = [this, ok, why](const std::string &dir, std::string *w) {
            probe_calls++;
            if (ok) {
                return fe::probe_data_dir(dir, w);
            }
            *w = why;
            return false;
        };
    }
};

constexpr auto kMac = fe::DataInputs::Os::kMacos;
constexpr auto kWin = fe::DataInputs::Os::kWindows;
constexpr auto kLinux = fe::DataInputs::Os::kLinux;

} // namespace

TEST_CASE("data folder: MAPLETHIEF_DATA_DIR beats everything, with its own game/") {
    const TempDir tmp("env");
    const fs::path kit = tmp.path / "kit";
    write_text(kit / fe::kUserDataMarkerName, "x");
    Fake f(kit, tmp.path / "user", kMac);
    f.probe_says(true);
    f.in.env = str(tmp.path / "test-data");
    f.in.flag = Place::kGame;
    const fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kEnv);
    CHECK(loc.reason == Reason::kEnv);
    CHECK(loc.dir == str(tmp.path / "test-data"));
    CHECK(loc.saves == str(tmp.path / "test-data" / "saves"));
    CHECK(loc.bundle_dir == str(tmp.path / "test-data" / "game"));
    CHECK(loc.bundle_parent() == loc.dir);
    CHECK(loc.backups == str(tmp.path / "test-data" / "backups"));
    CHECK(loc.states == str(tmp.path / "test-data" / "states"));
    CHECK(loc.screenshots == str(tmp.path / "test-data" / "screenshots"));
    CHECK(loc.exports == str(tmp.path / "test-data" / "exports"));
    CHECK(loc.import == str(tmp.path / "test-data" / "import"));
    CHECK(loc.logs == str(tmp.path / "test-data" / "logs"));
    CHECK(loc.crashes == str(tmp.path / "test-data" / "crashes"));
    CHECK_FALSE(loc.fallback());
    CHECK(f.user_calls == 0); // never SDL_GetPrefPath
    CHECK(f.probe_calls == 0);
    CHECK_FALSE(fs::exists(kit / "data"));
}

TEST_CASE("data folder: the default is <game folder>/data when the probe can write there") {
    const TempDir tmp("default");
    const fs::path kit = tmp.path / "MapleThief-1.0-windows-x64";
    fs::create_directories(kit);
    Fake f(kit, tmp.path / "user", kWin);
    const fe::DataLocation loc = fe::resolve_data_location(f.in); // the real probe
    CHECK(loc.place == Place::kGame);
    CHECK(loc.reason == Reason::kDefault);
    CHECK(loc.base == str(kit));
    CHECK(loc.dir == str(kit / "data"));
    CHECK(loc.saves == str(kit / "data" / "saves"));
    CHECK(loc.bundle_dir == str(kit / "game")); // beside data/, not in it
    CHECK(loc.bundle_parent() == str(kit));
    CHECK(loc.placed_bundle == str(kit / "game" / "MapleThief.app"));
    CHECK(loc.why.empty());
    CHECK(f.user_calls == 0);
    // The probe made data/ and left nothing in it.
    CHECK(fs::is_directory(kit / "data"));
    CHECK(fs::is_empty(kit / "data"));
    CHECK(fe::data_location_log(loc) ==
          std::vector<std::string>{"data: " + str(kit / "data") + " (default)"});
}

TEST_CASE("data folder: --data-location and user-data.txt choose the per-user folder") {
    const TempDir tmp("optout");
    const fs::path kit = tmp.path / "kit", user = tmp.path / "user";
    fs::create_directories(kit);
    Fake f(kit, user, kLinux);
    f.probe_says(true);
    // The flag: this run only.
    f.in.flag = Place::kUser;
    fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kUser);
    CHECK(loc.reason == Reason::kFlag);
    CHECK(loc.dir == str(user));
    CHECK(loc.saves == str(user / "saves"));
    CHECK(loc.bundle_dir == str(user / "game")); // the user folder's own game/
    CHECK(loc.base == str(kit));
    CHECK(f.probe_calls == 0);
    CHECK_FALSE(fs::exists(kit / "data"));
    // The marker: the user folder from now on; nothing but its presence counts.
    f.in.flag.reset();
    write_text(kit / fe::kUserDataMarkerName, "");
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kUser);
    CHECK(loc.reason == Reason::kMarker);
    CHECK(loc.dir == str(user));
    CHECK_FALSE(loc.fallback());
    // --data-location game beats the marker.
    f.in.flag = Place::kGame;
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kGame);
    CHECK(loc.reason == Reason::kFlag);
    CHECK(loc.dir == str(kit / "data"));
    CHECK(f.probe_calls == 1);
    // The flag's values.
    CHECK(fe::parse_data_place("game") == Place::kGame);
    CHECK(fe::parse_data_place("user") == Place::kUser);
    CHECK_FALSE(fe::parse_data_place("env"));
    CHECK_FALSE(fe::parse_data_place(""));
}

TEST_CASE("data folder: an app bundle's base is the folder around MapleThief.app") {
    const TempDir tmp("bundle");
    const fs::path games = tmp.path / "Games";
    const fs::path exe = games / "MapleThief.app" / "Contents" / "MacOS";
    fs::create_directories(exe);
    Fake f(exe, tmp.path / "user", kMac);
    f.probe_says(true);
    const fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kGame);
    CHECK(loc.base == str(games));
    CHECK(loc.dir == str(games / "data")); // never inside the bundle
    CHECK(loc.bundle_dir == str(games / "game"));
    CHECK_FALSE(fs::exists(exe / "data"));
    CHECK(fe::data_base_dir(str(games) + "/MapleThief.app/Contents/Resources/") == str(games));
}

TEST_CASE("data folder: the Linux game folder is the parent of bin/ when it holds MapleThief.sh") {
    const TempDir tmp("linux");
    const fs::path kit = tmp.path / "MapleThief-1.0-linux-x86_64";
    fs::create_directories(kit / "bin");
    // Without the launcher, bin/ is the base.
    CHECK(fe::data_base_dir(str(kit / "bin") + "/") == str(kit / "bin"));
    write_text(kit / "MapleThief.sh", "#!/bin/sh\n");
    CHECK(fe::data_base_dir(str(kit / "bin") + "/") == str(kit));
    Fake f(kit / "bin", tmp.path / "user", kLinux);
    const fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kGame);
    CHECK(loc.base == str(kit));
    CHECK(loc.dir == str(kit / "data"));
    CHECK(loc.bundle_dir == str(kit / "game"));
    CHECK(loc.placed_bundle == str(kit / "game" / "MapleThief.app"));
    CHECK_FALSE(fs::exists(kit / "bin" / "data"));
}

TEST_CASE("data folder: macOS, the app alone in an Applications folder uses the user folder") {
    Fake f(fs::path("/Applications/MapleThief.app/Contents/Resources"), "/u/MapleThief", kMac);
    f.probe_says(true);
    fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kUser);
    CHECK(loc.reason == Reason::kApplicationsFolder);
    CHECK(loc.dir == "/u/MapleThief");
    CHECK(loc.base == "/Applications");
    CHECK_FALSE(loc.fallback()); // a normal state
    CHECK(f.probe_calls == 0);
    f.in.base_path = "/Users/player/Applications/MapleThief.app/Contents/MacOS/";
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.reason == Reason::kApplicationsFolder);
    // A folder below Applications is a game folder like any other.
    f.in.base_path = "/Applications/Games/MapleThief.app/Contents/MacOS/";
    f.probe_says(false, "denied");
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.reason == Reason::kFallbackNotWritable);
    // Not on Windows or Linux: the probe decides there.
    f.in.os = kWin;
    f.in.base_path = "/Applications/";
    f.in.probe = [](const std::string &, std::string *) { return true; };
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kGame);
    CHECK(loc.dir == str(fs::path("/Applications") / "data"));
}

TEST_CASE("data folder: a game folder that can't be written falls back to the user folder") {
    const TempDir tmp("readonly");
    const fs::path kit = tmp.path / "kit", user = tmp.path / "user";
    fs::create_directories(kit);
    Fake f(kit, user, kWin);
    f.probe_says(false, "Permission denied");
    fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kUser);
    CHECK(loc.reason == Reason::kFallbackNotWritable);
    CHECK(loc.why == "Permission denied");
    CHECK(loc.dir == str(user));
    CHECK(loc.saves == str(user / "saves"));
    CHECK(loc.bundle_dir == str(user / "game"));
    CHECK(loc.fallback());
    CHECK(f.user_calls == 1);
    const std::vector<std::string> log = fe::data_location_log(loc);
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "data: " + str(user) + " (fallback-not-writable)");
    CHECK(log[1] == "data: " + str(kit / "data") + " can't be written (Permission denied); " +
                        "using the user folder " + str(user));
    CHECK(fe::describe_data_location(loc) ==
          "data: " + str(user) + "\nlocation: user\nreason: fallback-not-writable\n" +
              "why: Permission denied\nsaves: " + str(user / "saves") +
              "\ngame: " + str(user / "game") + "\n");
#if !defined(_WIN32) && !defined(__SWITCH__)
    // The real probe on a read-only folder (not as root, who may write anyway). Not on the
    // Switch: the SD card's FAT has no permissions, and newlib no geteuid.
    if (geteuid() != 0) {
        fs::permissions(kit, fs::perms::owner_read | fs::perms::owner_exec);
        f.in.probe = nullptr;
        loc = fe::resolve_data_location(f.in);
        fs::permissions(kit, fs::perms::owner_all);
        CHECK(loc.reason == Reason::kFallbackNotWritable);
        CHECK(loc.why.find(str(kit / "data")) != std::string::npos);
        CHECK(loc.why.find("ermission denied") != std::string::npos);
        CHECK_FALSE(fs::exists(kit / "data"));
    }
#endif
    // An existing data/ that can't be written isn't read either.
    fs::create_directories(kit / "data" / "saves");
    f.probe_says(false, "Read-only file system");
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.dir == str(user));
}

TEST_CASE("data folder: the probe creates, writes and removes one file") {
    const TempDir tmp("probe");
    std::string why;
    CHECK(fe::probe_data_dir(str(tmp.path / "a" / "b" / "data"), &why));
    CHECK(why.empty());
    CHECK(fs::is_empty(tmp.path / "a" / "b" / "data"));
    // A file where the folder should be.
    write_text(tmp.path / "file", "x");
    CHECK_FALSE(fe::probe_data_dir(str(tmp.path / "file"), &why));
    CHECK_FALSE(why.empty());
    CHECK_FALSE(fe::probe_data_dir("", &why));
}

TEST_CASE("data folder: App Translocation and no base folder are fallbacks with their reason") {
    Fake f(fs::path("/private/var/folders/x/T/AppTranslocation/1234-ABCD/d/MapleThief.app/"
                    "Contents/Resources"),
           "/u/MapleThief", kMac);
    f.probe_says(true);
    fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kUser);
    CHECK(loc.reason == Reason::kFallbackTranslocated);
    CHECK_FALSE(loc.why.empty());
    CHECK(loc.dir == "/u/MapleThief");
    CHECK(f.probe_calls == 0); // decided by the path
    CHECK(fe::data_location_log(loc).size() == 2);
    // No SDL_GetBasePath answer.
    f.in.base_path.clear();
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kUser);
    CHECK(loc.reason == Reason::kFallbackNoBase);
    CHECK(loc.base.empty());
    CHECK(loc.placed_bundle.empty());
    CHECK(loc.fallback());
    // Nor a per-user folder: none at all.
    f.in.user_dir = [] { return std::string(); };
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kNone);
    CHECK(loc.dir.empty());
    CHECK(loc.saves.empty());
    CHECK(fe::data_location_log(loc)[0] == "data: none (none)");
}

TEST_CASE("data folder: iOS keeps the app's folder and its own Documents (ADR 0074)") {
    Fake f(fs::path("/var/containers/Bundle/Application/X/MapleThief.app"),
           "/var/mobile/Containers/Data/Application/Y/Library/Application Support/MapleThief",
           fe::DataInputs::Os::kIos);
    f.in.ios_documents = "/var/mobile/Containers/Data/Application/Y/Documents/";
    f.probe_says(true);
    fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kIos);
    CHECK(loc.reason == Reason::kPlatform);
    CHECK(loc.saves == "/var/mobile/Containers/Data/Application/Y/Documents");
    CHECK(loc.base.empty());
    CHECK(f.probe_calls == 0);
    // MAPLETHIEF_DATA_DIR keeps everything in one place there too (the tests' data directory).
    f.in.env = "/tmp/somewhere";
    loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kEnv);
    CHECK(loc.saves == str(fs::path("/tmp/somewhere") / "saves"));
}

TEST_CASE("data folder: portable.txt is accepted, does nothing and is logged") {
    const TempDir tmp("portable");
    const fs::path kit = tmp.path / "kit";
    write_text(kit / fe::kPortableFileName, "");
    Fake f(kit, tmp.path / "user", kWin);
    const fe::DataLocation loc = fe::resolve_data_location(f.in);
    CHECK(loc.place == Place::kGame);
    CHECK(loc.reason == Reason::kDefault);
    CHECK(loc.portable_marker);
    const std::vector<std::string> log = fe::data_location_log(loc);
    REQUIRE(log.size() == 2);
    CHECK(log[1] == "data: portable.txt is no longer needed: the data folder is next to the game "
                    "by default");
    CHECK(fs::exists(kit / fe::kPortableFileName)); // nothing removes it
}

TEST_CASE("data folder: data_location() answers once; the test seam replaces the answer") {
    const TempDir tmp("seam");
    fe::DataLocation fake =
        fe::data_location_at(str(tmp.path / "x"), Place::kUser, Reason::kFallbackNotWritable);
    fake.why = "a test";
    fe::set_data_location_for_test(fake);
    CHECK(fe::data_location() == fake);
    CHECK(&fe::data_location() == &fe::data_location());
    CHECK(fe::data_dir() == str(tmp.path / "x"));
    fe::set_data_location_for_test(std::nullopt);
}

TEST_CASE("data folder: configure_save_location puts the saves and gamecenter.json in saves/") {
    const TempDir tmp("configure");
    const fs::path data = tmp.path / "data";
    write_text(data / "gamecenter.json", "{\"old\": 1}");
    fe::Settings settings;
    fe::SaveLocation loc = fe::configure_save_location(settings, fe::data_location_at(str(data)));
    CHECK(loc.data_dir == str(data));
    CHECK(loc.documents_dir == str(data / "saves"));
    CHECK(std::string(mt_saves_documents_dir()) == str(data / "saves"));
    char path[1024];
    REQUIRE(mt_stubs_gamecenter_path(path, sizeof path) == 0);
    CHECK(fs::path(path) == data / "saves" / "gamecenter.json");
    // Migration step 2: the old gamecenter.json moved into saves/.
    CHECK(read_text(data / "saves" / "gamecenter.json") == "{\"old\": 1}");
    CHECK_FALSE(fs::exists(data / "gamecenter.json"));
    // save_dir moves saves/, gamecenter.json with it (ADR 0127); an old <data>/gamecenter.json
    // is moved there.
    write_text(data / "gamecenter.json", "{\"older\": 2}");
    settings.save_dir = str(tmp.path / "cloud");
    loc = fe::configure_save_location(settings, fe::data_location_at(str(data)));
    CHECK(loc.documents_dir == str(tmp.path / "cloud"));
    REQUIRE(mt_stubs_gamecenter_path(path, sizeof path) == 0);
    CHECK(fs::path(path) == tmp.path / "cloud" / "gamecenter.json");
    CHECK(read_text(tmp.path / "cloud" / "gamecenter.json") == "{\"older\": 2}");
    // iOS: gamecenter.json stays in the data folder.
    fe::DataLocation ios = fe::data_location_at(str(data), Place::kIos, Reason::kPlatform);
    ios.saves = str(tmp.path / "Documents");
    settings.save_dir.clear();
    loc = fe::configure_save_location(settings, ios);
    CHECK(loc.documents_dir == str(tmp.path / "Documents"));
    REQUIRE(mt_stubs_gamecenter_path(path, sizeof path) == 0);
    CHECK(fs::path(path) == data / "gamecenter.json");
    mt_saves_set_documents_dir(nullptr);
    mt_stubs_set_gamecenter_path(nullptr);
    mt_stubs_set_data_dir(nullptr);
}

TEST_CASE("data folder: an old layout is migrated in place, byte for byte, once") {
    const TempDir tmp("migrate");
    const fs::path kit = tmp.path / "kit", data = kit / "data";
    std::string theif(1124, '\0'), quest(4781, '\x7f');
    for (std::size_t i = 0; i < theif.size(); i++) {
        theif[i] = static_cast<char>(i * 7);
    }
    write_text(data / "Documents" / "theif0", theif);
    write_text(data / "Documents" / "quest0", quest);
    write_text(data / "Documents" / "config", std::string("\x03\x01\x00\x01\x01\x01", 6));
    write_text(data / "gamecenter.json", "{\"achievements\": []}");
    write_text(data / "settings.ini", "music_volume = 50\n");
    write_text(data / "game" / "MapleThief", "exe bytes");
    write_text(data / "game" / "Info.plist", "plist");
    const Tree saves_before = tree(data / "Documents");
    const Tree game_before = tree(data / "game");
    const fe::DataLocation loc =
        fe::data_location_at(str(data), Place::kGame, Reason::kDefault, str(kit));

    const std::vector<std::string> lines = fe::migrate_data_layout(loc);
    CHECK(lines.size() == 2);
    CHECK_FALSE(fs::exists(data / "Documents"));
    CHECK(tree(data / "saves") == saves_before);
    CHECK_FALSE(fs::exists(data / "game"));
    CHECK(tree(kit / "game") == game_before);
    CHECK(read_text(data / "settings.ini") == "music_volume = 50\n"); // untouched
    CHECK_FALSE(fe::migrate_gamecenter(str(data), str(data / "saves")).empty());
    CHECK(read_text(data / "saves" / "gamecenter.json") == "{\"achievements\": []}");
    CHECK_FALSE(fs::exists(data / "gamecenter.json"));

    // A second start does nothing.
    const Tree all = tree(kit);
    CHECK(fe::migrate_data_layout(loc).empty());
    CHECK(fe::migrate_gamecenter(str(data), str(data / "saves")).empty());
    CHECK(tree(kit) == all);
}

TEST_CASE("data folder: an interrupted migration is finished on the next start") {
    const TempDir tmp("interrupted");
    const fs::path kit = tmp.path / "kit", data = kit / "data";
    // Step 1 done (saves/), the bundle and gamecenter.json not yet moved.
    write_text(data / "saves" / "theif1", "character");
    write_text(data / "gamecenter.json", "gc");
    write_text(data / "game" / "MapleThief", "exe");
    const fe::DataLocation loc =
        fe::data_location_at(str(data), Place::kGame, Reason::kDefault, str(kit));
    CHECK(fe::migrate_data_layout(loc).size() == 1);
    CHECK(fe::migrate_gamecenter(loc.dir, loc.saves).size() > 0);
    CHECK(tree(data) == Tree{{"saves/gamecenter.json", "gc"}, {"saves/theif1", "character"}});
    CHECK(tree(kit / "game") == Tree{{"MapleThief", "exe"}});

    // Both Documents/ and saves/: saves/ is used, Documents/ left alone with a warning.
    write_text(data / "Documents" / "theif0", "stale");
    const std::vector<std::string> lines = fe::migrate_data_layout(loc);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("saves/ is used") != std::string::npos);
    CHECK(read_text(data / "Documents" / "theif0") == "stale");
    // A bundle already in <base>/game: data/game/ is left as it is.
    write_text(data / "game" / "MapleThief", "older exe");
    CHECK(fe::migrate_data_layout(loc).size() == 1); // only the warning again
    CHECK(read_text(kit / "game" / "MapleThief") == "exe");

    // A gamecenter.json copy interrupted before its source was removed: removed now. A
    // different one beside it stays (saves/ is the one in use).
    write_text(data / "gamecenter.json", "gc");
    CHECK(fe::migrate_gamecenter(loc.dir, loc.saves).find("removed") != std::string::npos);
    CHECK_FALSE(fs::exists(data / "gamecenter.json"));
    write_text(data / "gamecenter.json", "other");
    CHECK(fe::migrate_gamecenter(loc.dir, loc.saves).empty());
    CHECK(read_text(data / "gamecenter.json") == "other");
    CHECK(read_text(data / "saves" / "gamecenter.json") == "gc");

    // Outside the game folder the bundle folder is the data folder's own game/: not moved.
    const fs::path user = tmp.path / "user";
    write_text(user / "Documents" / "quest0", "q");
    write_text(user / "game" / "MapleThief", "exe");
    const fe::DataLocation u =
        fe::data_location_at(str(user), Place::kUser, Reason::kMarker, str(kit));
    CHECK(fe::migrate_data_layout(u).size() == 1);
    CHECK(tree(user) == Tree{{"game/MapleThief", "exe"}, {"saves/quest0", "q"}});
}
