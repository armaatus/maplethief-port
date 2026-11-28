// The speedrun timer (#406, run_timer.h, ADR 0156) on synthetic run events and a fake clock: a
// run's start, splits, end and reset, both times, the personal best and its difference; the
// clean check (an Assist set and reset mid-run leaves the run assisted, every Assists row of the
// registry is checked), the run variables; the category parser's refusals by line and the
// shipped categories; the run log's round trip and the files in a data folder. The game's side
// (the last split of Chapter 1 from a save before the trip to Orbis) is play.chapter_1_l's.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "maplethief/events.h"
#include "run_timer.h"
#include "settings.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;

// A clock the test moves by hand.
struct FakeClock {
    fe::RunInstant at;
    fe::RunClock clock() {
        return fe::RunClock{[this] { return at; },
                            [] { return std::string("2026-11-26 21:03:04"); }};
    }
    void advance(double seconds, std::uint64_t ticks) {
        at.seconds += seconds;
        at.ticks += ticks;
    }
};

mt_game_event event(mt_game_event_kind kind, std::int32_t value, std::int32_t more0 = 0,
                    std::int32_t more1 = 0) {
    mt_game_event e{};
    e.kind = static_cast<std::uint32_t>(kind);
    e.value = value;
    e.more[0] = more0;
    e.more[1] = more1;
    return e;
}

const char *const kTestCategory = "title = Test%\n"
                                  "split chapter_end 0 = Tutorial\n"
                                  "split job_advance 2 2 = Bandit\n"
                                  "split map_change 4.1 = Orbis\n";

fe::RunCategory test_category() {
    std::vector<std::string> problems;
    std::optional<fe::RunCategory> c = fe::parse_run_category("test", kTestCategory, &problems);
    REQUIRE(c.has_value());
    return *c;
}

// A whole run of test_category: 10 s and 200 ticks between the events.
std::vector<fe::RunRecord> run_through(fe::RunTimer &t, FakeClock &clock) {
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    clock.advance(10, 200);
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    clock.advance(10, 200);
    t.turn();
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    clock.advance(10, 200);
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1, 0));
    return t.take_records();
}

std::string read_text(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A fresh directory in the test's temporary folder.
fs::path fresh_dir(const char *name) {
    const fs::path dir = fs::temp_directory_path() / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

std::string path_text(const fs::path &p) {
    const std::u8string s = p.u8string();
    return {s.begin(), s.end()};
}

} // namespace

TEST_CASE("run timer: start, splits in order, both times, the end") {
    FakeClock clock;
    clock.at = {100.0, 5000};
    fe::RunTimer t(clock.clock());
    t.set_category(test_category(), std::nullopt);
    CHECK(t.state() == fe::RunTimer::State::kWaiting);

    // Nothing before the start: a split's event waits for a run.
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    CHECK(t.state() == fe::RunTimer::State::kWaiting);
    CHECK(t.next_split() == 0);

    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 1));
    CHECK(t.state() == fe::RunTimer::State::kRunning);
    clock.advance(12.3456, 246);
    CHECK(t.elapsed() == fe::RunTime{12346, 246});

    // A later split's event, another chapter's or another job's do nothing.
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1));
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 1));
    t.on_event(event(MT_GAME_EVENT_LEVEL_UP, 10));
    CHECK(t.next_split() == 0);
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    REQUIRE(t.next_split() == 1);
    CHECK(t.times()[0] == fe::RunTime{12346, 246});

    clock.advance(60, 1200);
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 1)); // tier 1: not the split
    CHECK(t.next_split() == 1);
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    CHECK(t.next_split() == 2);
    clock.advance(3600, 72000);
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 2)); // map 4.2: not the split
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1, 3));
    CHECK(t.state() == fe::RunTimer::State::kFinished);
    const fe::RunTime end{12346 + 60000 + 3600000, 246 + 1200 + 72000};
    CHECK(t.elapsed() == end);
    clock.advance(5, 100); // a finished run's time stands
    CHECK(t.elapsed() == end);

    const std::vector<fe::RunRecord> records = t.take_records();
    REQUIRE(records.size() == 1);
    const fe::RunRecord &r = records[0];
    CHECK(r.finished);
    CHECK(r.category == "test");
    CHECK(r.category_title == "Test%");
    CHECK(r.started == "2026-11-26 21:03:04");
    CHECK(r.times.size() == 3);
    CHECK(r.end == end);
    CHECK(r.clean());
    CHECK(r.in_category);
    CHECK(r.counts());
    CHECK(t.take_records().empty());
    CHECK(fe::format_run_time(r.end.ms) == "1:01:12.3");
}

TEST_CASE("run timer: reset, and a new character restarts a run") {
    FakeClock clock;
    fe::RunTimer t(clock.clock());
    t.set_category(test_category(), std::nullopt);
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    clock.advance(10, 200);
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    clock.advance(5, 100);
    t.reset();
    CHECK(t.state() == fe::RunTimer::State::kWaiting);
    CHECK(t.elapsed() == fe::RunTime{});
    std::vector<fe::RunRecord> records = t.take_records();
    REQUIRE(records.size() == 1);
    CHECK_FALSE(records[0].finished);
    CHECK_FALSE(records[0].counts());
    CHECK(records[0].times.size() == 1);
    CHECK(records[0].end == fe::RunTime{15000, 300});
    t.reset(); // nothing in progress: no record
    CHECK(t.take_records().empty());

    // A new character during a run: that run ends as reset, a new one starts at 0.
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    clock.advance(7, 140);
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 1));
    CHECK(t.state() == fe::RunTimer::State::kRunning);
    CHECK(t.elapsed() == fe::RunTime{});
    records = t.take_records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].end == fe::RunTime{7000, 140});

    // Another category ends it too.
    t.set_category(test_category(), std::nullopt);
    CHECK(t.state() == fe::RunTimer::State::kWaiting);
    CHECK(t.take_records().size() == 1);

    // No category: nothing starts.
    fe::RunTimer none(clock.clock());
    none.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    CHECK(none.state() == fe::RunTimer::State::kWaiting);
}

TEST_CASE("run timer: a category's start line starts a run from a save (#408)") {
    FakeClock clock;
    fe::RunTimer t(clock.clock());
    std::vector<std::string> problems;
    std::optional<fe::RunCategory> c = fe::parse_run_category(
        "orbis", "title = Orbis\nstart = map_change 2.0\nsplit chapter_end 1 = Orbis\n", &problems);
    REQUIRE(c.has_value());
    REQUIRE(c->start.has_value());
    CHECK(fe::run_split_event_text(*c->start) == "map_change 2.0");
    // The canonical text keeps it, and reads back as the same category.
    CHECK(fe::run_category_text(*c).find("\nstart = map_change 2.0\n") != std::string::npos);
    CHECK(fe::parse_run_category("orbis", fe::run_category_text(*c), &problems) == c);
    t.set_category(*c, std::nullopt);
    // A new character does not start it; the start event does.
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    CHECK(t.state() == fe::RunTimer::State::kWaiting);
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 2, 1));
    CHECK(t.state() == fe::RunTimer::State::kWaiting);
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 2, 0));
    CHECK(t.state() == fe::RunTimer::State::kRunning);
    clock.advance(30, 600);
    // The start event again during the run does not restart it.
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 2, 0));
    clock.advance(31.5, 620);
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 1));
    CHECK(t.state() == fe::RunTimer::State::kFinished);
    std::vector<fe::RunRecord> records = t.take_records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].finished);
    CHECK(records[0].end == fe::RunTime{61500, 1220});
    // After a finished run, the start event starts the next.
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 2, 0));
    CHECK(t.state() == fe::RunTimer::State::kRunning);

    // load_character SLOT: a saved character chosen in that slot; a start line's only.
    c = fe::parse_run_category(
        "from_save", "title = From a save\nstart = load_character 0\nsplit chapter_end 1 = Orbis\n",
        &problems);
    REQUIRE(c.has_value());
    CHECK(fe::run_split_event_text(*c->start) == "load_character 0");
    fe::RunTimer l(clock.clock());
    l.set_category(*c, std::nullopt);
    l.on_event(event(MT_GAME_EVENT_LOAD_CHARACTER, 1));
    CHECK(l.state() == fe::RunTimer::State::kWaiting);
    l.on_event(event(MT_GAME_EVENT_LOAD_CHARACTER, 0));
    CHECK(l.state() == fe::RunTimer::State::kRunning);
}

TEST_CASE("run timer: the personal best and the difference to it") {
    FakeClock clock;
    fe::RunTimer t(clock.clock());
    t.set_category(test_category(), std::nullopt);
    std::vector<fe::RunRecord> first = run_through(t, clock);
    REQUIRE(first.size() == 1);
    CHECK(first[0].times[0].ms == 10000);
    CHECK(t.difference() == std::nullopt); // no best

    const fs::path dir = fresh_dir("maplethief_run_timer_best");
    std::string path, why;
    bool best = false;
    REQUIRE(fe::write_run(path_text(dir), first[0], nullptr, &path, &best, &why));
    CHECK(best);
    CHECK(fs::exists(dir / "runs" / "test.best"));
    CHECK(fs::path(path).filename() == "test-20261126-210304.log");
    std::optional<fe::RunRecord> loaded = fe::load_run_best(path_text(dir), "test");
    REQUIRE(loaded.has_value());
    CHECK(*loaded == first[0]);

    // A second run against it: 2 s faster at the first split, 1 s slower at the end.
    t.set_category(test_category(), loaded);
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    clock.advance(8, 160);
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    REQUIRE(t.difference().has_value());
    CHECK(*t.difference() == -2000);
    const std::vector<std::string> lines = t.overlay_lines();
    CHECK(std::find(lines.begin(), lines.end(), "Tutorial -0:02.0 to the best") != lines.end());
    clock.advance(12, 240);
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    clock.advance(11, 220);
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1));
    CHECK(*t.difference() == 1000);
    std::vector<fe::RunRecord> second = t.take_records();
    REQUIRE(second.size() == 1);
    REQUIRE(fe::write_run(path_text(dir), second[0], &*loaded, &path, &best, &why));
    CHECK_FALSE(best); // slower: the best stays
    CHECK(fs::path(path).filename() == "test-20261126-210304-2.log");
    CHECK(fe::load_run_best(path_text(dir), "test")->end.ms == 30000);

    // A faster one replaces it; an assisted or reset one never does.
    fe::RunRecord faster = second[0];
    faster.end.ms = 20000;
    fe::RunRecord assisted = faster;
    assisted.end.ms = 1000;
    assisted.assists = {"game_speed"};
    REQUIRE(fe::write_run(path_text(dir), assisted, &*loaded, &path, &best, &why));
    CHECK_FALSE(best);
    fe::RunRecord reset = faster;
    reset.finished = false;
    reset.end.ms = 500;
    REQUIRE(fe::write_run(path_text(dir), reset, &*loaded, &path, &best, &why));
    CHECK_FALSE(best);
    REQUIRE(fe::write_run(path_text(dir), faster, &*loaded, &path, &best, &why));
    CHECK(best);
    CHECK(fe::load_run_best(path_text(dir), "test")->end.ms == 20000);
    std::size_t logs = 0;
    for (const fs::directory_entry &e : fs::directory_iterator(dir / "runs")) {
        logs += e.path().extension() == ".log" ? 1 : 0;
    }
    CHECK(logs == 5);
    fs::remove_all(dir);
}

TEST_CASE("run timer: an Assist set and reset mid-run leaves the run assisted") {
    FakeClock clock;
    fe::Settings s;
    fe::RunTimer t(clock.clock());
    t.attach(&s);
    t.set_category(test_category(), std::nullopt);
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    t.turn();
    CHECK(t.assists().empty());
    std::vector<std::string> lines = t.overlay_lines();
    CHECK(std::find(lines.begin(), lines.end(), "clean") != lines.end());

    s.game_speed = fe::GameSpeed::k25;
    t.turn();
    s.game_speed = fe::GameSpeed::kGame;
    t.turn();
    CHECK(t.assists() == std::vector<std::string>{"game_speed"});
    lines = t.overlay_lines();
    CHECK(std::find(lines.begin(), lines.end(), "assisted: game_speed") != lines.end());
    clock.advance(1, 20);
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1));
    const std::vector<fe::RunRecord> records = t.take_records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].finished);
    CHECK_FALSE(records[0].clean());
    CHECK_FALSE(records[0].counts());
    CHECK(records[0].assists == std::vector<std::string>{"game_speed"});

    // On at the start: assisted from the start; the next run starts clean again.
    s.save_states = true;
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    CHECK(t.assists() == std::vector<std::string>{"save_states"});
    s.save_states = false;
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    CHECK(t.assists().empty());
}

TEST_CASE("run timer: the clean check names every Assists row in the registry") {
    // Every Assists row, each moved off its neutral value alone, marks a run assisted under its
    // own key: the check reads the registry, so a new Assist is covered with no change.
    std::size_t assists = 0;
    for (const fe::SettingInfo &row : fe::settings_registry()) {
        if (row.group != fe::SettingGroup::kAssists) {
            continue;
        }
        assists++;
        const std::string neutral = fe::preset_column(row, fe::Preset::kOriginal);
        std::vector<std::string> values = fe::setting_choices(row);
        values.erase(std::remove(values.begin(), values.end(), neutral), values.end());
        INFO(row.key);
        REQUIRE_FALSE(values.empty());
        FakeClock clock;
        fe::Settings s;
        fe::RunTimer t(clock.clock());
        t.attach(&s);
        t.set_category(test_category(), std::nullopt);
        t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
        CHECK(t.assists().empty());
        std::string why;
        REQUIRE(fe::apply_setting(&s, row.key, values.front(), &why));
        t.turn();
        CHECK(t.assists() == std::vector<std::string>{row.key});
    }
    CHECK(assists >= 9); // cash_shop ... save_states (ADR 0070) at the time of #406
}

TEST_CASE("run timer: run variables are recorded, and outside the category's values shown") {
    FakeClock clock;
    fe::Settings s;
    s.skip_intros = true;
    fe::RunTimer t(clock.clock());
    t.attach(&s);
    t.set_category(test_category(), std::nullopt); // names no variable: off only
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    CHECK(t.variables() ==
          std::vector<std::pair<std::string, std::string>>{{"fix_original_bugs", "off"},
                                                           {"skip_intros", "on"},
                                                           {"widescreen", "off"},
                                                           {"texture_pack", "off"}});
    CHECK_FALSE(t.in_category());
    CHECK(t.assists().empty()); // a run variable is no Assist
    s.fix_original_bugs = true;
    t.turn();
    s.fix_original_bugs = false;
    t.turn();
    CHECK(t.variables()[0].second == "off,on,off");
    std::vector<std::string> lines = t.overlay_lines();
    REQUIRE(lines.size() >= 2);
    CHECK(lines[lines.size() - 2] == "fix_original_bugs off  skip_intros on");
    CHECK(lines.back() == "widescreen off  texture_pack off");
    CHECK(std::find(lines.begin(), lines.end(), "outside the category's run variables") !=
          lines.end());
    t.reset();
    std::vector<fe::RunRecord> records = t.take_records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].variables[1] == std::pair<std::string, std::string>{"skip_intros", "on"});
    CHECK_FALSE(records[0].in_category);
    CHECK(records[0].settings.find("skip_intros = on") != std::string::npos);

    // widescreen and texture_pack (#408): they reach guest memory, so they are run variables;
    // a pack is recorded as on, never by its folder.
    s = fe::Settings{};
    s.widescreen = fe::Widescreen::kAuto;
    s.texture_pack = "/somewhere/my pack";
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    CHECK(t.variables()[2] == std::pair<std::string, std::string>{"widescreen", "auto"});
    CHECK(t.variables()[3] == std::pair<std::string, std::string>{"texture_pack", "on"});
    CHECK_FALSE(t.in_category());
    CHECK(t.assists().empty());
    t.reset();
    t.take_records();
    s = fe::Settings{};
    s.skip_intros = true;

    // A category that allows it.
    std::vector<std::string> problems;
    std::optional<fe::RunCategory> c = fe::parse_run_category(
        "skips", "title = Skips\nallow skip_intros = off, on\nsplit chapter_end 0 = T\n",
        &problems);
    REQUIRE(c.has_value());
    t.set_category(*c, std::nullopt);
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    records = t.take_records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].in_category);
    CHECK(records[0].counts());
}

TEST_CASE("run categories: the parser refuses malformed files by line") {
    struct Bad {
        const char *text;
        const char *problem;
    };
    const Bad bad[] = {
        {"title = A\nsplit chapter_end 0\n", "line 2: no '='"},
        {"title = A\n= x\nsplit chapter_end 0 = T\n", "line 2: nothing before '='"},
        {"title = A\ntitle = B\nsplit chapter_end 0 = T\n", "line 2: a second title"},
        {"title = A\nstart = map_change 2\nsplit chapter_end 0 = T\n",
         "line 2: start: map_change takes AREA.MAP"},
        {"title = A\nstart x = chapter_end 1\nsplit chapter_end 0 = T\n",
         "line 2: start takes nothing before '='"},
        {"title = A\nstart = chapter_end 1\nstart = chapter_end 2\nsplit chapter_end 0 = T\n",
         "line 3: a second start"},
        {"title = A\nsplit load_character 0 = T\n", "line 2: a split's event is"},
        {"title = A\nstart = level_up 3\nsplit chapter_end 0 = T\n",
         "line 2: start: a start's event is load_character"},
        {"title =\nsplit chapter_end 0 = T\n", "line 1: the title is not"},
        {"title = A\nsplit chapter_end = T\n", "line 2: chapter_end takes one value"},
        {"title = A\nsplit chapter_end 256 = T\n", "line 2: chapter_end takes one value"},
        {"title = A\nsplit chapter_end -1 = T\n", "line 2: chapter_end takes one value"},
        {"title = A\nsplit boss_killed 64 65 = T\n", "line 2: boss_killed takes one value"},
        {"title = A\nsplit job_advance 2 = T\n", "line 2: job_advance takes BRANCH TIER"},
        {"title = A\nsplit map_change 4 1 = T\n", "line 2: map_change takes AREA.MAP"},
        {"title = A\nsplit map_change 4. = T\n", "line 2: map_change takes AREA.MAP"},
        {"title = A\nsplit level_up 10 = T\n", "line 2: a split's event is"},
        {"title = A\nsplit chapter_end 0 =\n", "line 2: a split's title"},
        {"title = A\nsplit chapter_end 0 = \xC3\xA9t\xC3\xA9\n", "line 2: a split's title"},
        {"title = A\nallow game_speed = 25\nsplit chapter_end 0 = T\n",
         "line 2: allow names a run variable"},
        {"title = A\nallow skip_intros = maybe\nsplit chapter_end 0 = T\n",
         "line 2: skip_intros: 'maybe' is not on or off"},
        {"title = A\nallow skip_intros = on\nallow skip_intros = off\nsplit chapter_end 0 = T\n",
         "line 3: a second allow for skip_intros"},
        {"title = A\nallow texture_pack = /my/pack\nsplit chapter_end 0 = T\n",
         "line 2: texture_pack: '/my/pack' is not off or on"},
        {"title = A\nname = B\nsplit chapter_end 0 = T\n", "line 2: unknown line 'name = B'"},
        {"split chapter_end 0 = T\n", "no title line"},
        {"title = A\n# no splits\n", "no split line"},
    };
    for (const Bad &b : bad) {
        INFO(b.text);
        std::vector<std::string> problems;
        CHECK_FALSE(fe::parse_run_category("x", b.text, &problems).has_value());
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].rfind(b.problem, 0) == 0);
    }
    // Every bad line is named, not only the first.
    std::vector<std::string> problems;
    CHECK_FALSE(fe::parse_run_category("x", "title = A\nfoo\nsplit x = y\nbar = 1\n", &problems));
    REQUIRE(problems.size() == 3);
    CHECK(problems[0].rfind("line 2:", 0) == 0);
    CHECK(problems[1].rfind("line 3:", 0) == 0);
    CHECK(problems[2].rfind("line 4:", 0) == 0);
    // The name and the size.
    problems.clear();
    CHECK_FALSE(fe::parse_run_category("Any", "title = A\nsplit chapter_end 0 = T\n", &problems));
    CHECK_FALSE(
        fe::parse_run_category("x", std::string(fe::kMaxRunCategoryFileBytes + 1, '#'), &problems));
    CHECK(problems.size() == 2);
    // 64 splits, not 65.
    std::string many = "title = Many\n";
    for (std::size_t i = 0; i < fe::kMaxRunSplits; i++) {
        many += "split map_change 1." + std::to_string(i) + " = M\n";
    }
    problems.clear();
    CHECK(fe::parse_run_category("x", many, &problems).has_value());
    many += "split chapter_end 0 = T\n";
    CHECK_FALSE(fe::parse_run_category("x", many, &problems).has_value());
    CHECK(problems.back().rfind("line 66: more than 64 splits", 0) == 0);
    // A byte order mark, CR LF, comments and blanks are fine; the text round-trips.
    problems.clear();
    std::optional<fe::RunCategory> c = fe::parse_run_category(
        "x", "\xEF\xBB\xBF# c\r\n\r\n  title  =  A b  \r\nsplit\tmap_change 4.1 = O\r\n",
        &problems);
    REQUIRE(c.has_value());
    CHECK(c->title == "A b");
    CHECK(c->splits[0] == fe::RunSplit{MT_GAME_EVENT_MAP_CHANGE, 4, 1, "O"});
    std::optional<fe::RunCategory> again =
        fe::parse_run_category("x", fe::run_category_text(*c), &problems);
    REQUIRE(again.has_value());
    CHECK(*again == *c);
}

TEST_CASE("run categories: both shipped categories parse") {
    const auto files = fe::shipped_run_category_files();
    REQUIRE(files.size() == 2);
    std::vector<std::string> problems;
    std::optional<fe::RunCategory> any =
        fe::parse_run_category(files[0].first, files[0].second, &problems);
    std::optional<fe::RunCategory> ch1 =
        fe::parse_run_category(files[1].first, files[1].second, &problems);
    CHECK(problems.empty());
    REQUIRE(any.has_value());
    REQUIRE(ch1.has_value());
    CHECK(any->id == "any");
    CHECK(any->title == "Any%");
    // Any%: a new character to mob 69's death, on every chapter end and main-quest boss, in the
    // order the game fires them: 68 is main 8's boss in chapter 2 (#434); 66, side 37's, is not.
    const std::vector<fe::RunSplit> any_splits = {
        {MT_GAME_EVENT_CHAPTER_END, 0, 0, "Tutorial"},
        {MT_GAME_EVENT_BOSS_KILLED, 64, 0, "Chapter 1 boss"},
        {MT_GAME_EVENT_CHAPTER_END, 1, 0, "Chapter 1"},
        {MT_GAME_EVENT_BOSS_KILLED, 68, 0, "Chapter 2 boss"},
        {MT_GAME_EVENT_CHAPTER_END, 2, 0, "Chapter 2"},
        {MT_GAME_EVENT_BOSS_KILLED, 67, 0, "Chapter 3 boss"},
        {MT_GAME_EVENT_BOSS_KILLED, 69, 0, "Last boss"},
    };
    CHECK(any->splits == any_splits);
    // Chapter 1: to the trip to Orbis (chapter 1's clearAll).
    CHECK(ch1->id == "chapter_1");
    CHECK(ch1->splits.back() == fe::RunSplit{MT_GAME_EVENT_CHAPTER_END, 1, 0, "Orbis"});
    // Both allow the run variables off only, the original game's rules.
    for (const fe::RunCategory *c : {&*any, &*ch1}) {
        CHECK(c->allowed("fix_original_bugs") == std::vector<std::string>{"off"});
        CHECK(c->allowed("skip_intros") == std::vector<std::string>{"off"});
    }
    // The repository's files are what the binary embeds.
    for (const auto &[name, text] : files) {
        const fs::path p = fs::path(MT_REPO_DIR) / "platform" / "categories" / (name + ".mtcat");
        CHECK(read_text(p) == text);
    }
}

TEST_CASE("run categories: a data folder's files beside the shipped ones") {
    const fs::path dir = fresh_dir("maplethief_run_timer_categories");
    fs::create_directories(dir / "categories");
    std::ofstream(dir / "categories" / "mine.mtcat") << "title = Mine\nsplit chapter_end 0 = T\n";
    std::ofstream(dir / "categories" / "chapter_1.mtcat")
        << "title = My chapter 1\nsplit chapter_end 1 = O\n";
    std::ofstream(dir / "categories" / "broken.mtcat") << "title = B\nsplit nothing = x\n";
    std::ofstream(dir / "categories" / "Bad Name.mtcat") << "title = B\nsplit chapter_end 0 = T\n";
    std::ofstream(dir / "categories" / "notes.txt") << "not a category";
    std::vector<std::string> problems;
    const std::vector<fe::RunCategory> all = fe::load_run_categories(path_text(dir), &problems);
    std::vector<std::string> ids;
    for (const fe::RunCategory &c : all) {
        ids.push_back(c.id);
    }
    CHECK(ids == std::vector<std::string>{"any", "chapter_1", "mine"});
    CHECK(all[1].title == "My chapter 1"); // the player's replaces the shipped one
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].rfind("categories/Bad Name.mtcat: the name", 0) == 0);
    CHECK(problems[1].rfind("categories/broken.mtcat: line 2:", 0) == 0);
    problems.clear();
    CHECK(fe::load_run_categories("", &problems).size() == 2);
    CHECK(problems.empty());
    fs::remove_all(dir);
}

TEST_CASE("run logs: the round trip, and malformed logs refused by line") {
    FakeClock clock;
    fe::Settings s;
    s.turbo_attack = true;
    fe::RunTimer t(clock.clock());
    t.attach(&s);
    t.set_category(test_category(), std::nullopt);
    const std::vector<fe::RunRecord> records = run_through(t, clock);
    REQUIRE(records.size() == 1);
    const std::string text = fe::run_log_text(records[0]);
    CHECK(text.find("time 3 = 30000 ms 600 ticks\n") != std::string::npos);
    CHECK(text.find("assisted = turbo_attack\n") != std::string::npos);
    std::vector<std::string> problems;
    std::optional<fe::RunRecord> back = fe::parse_run_log(text, &problems);
    CHECK(problems.empty());
    REQUIRE(back.has_value());
    CHECK(*back == records[0]);

    const char *const bad[] = {
        "category = x\nresult = finished\n",                                         // no end
        "category = x\nresult = maybe\nend = 1 ms 1 ticks\n",                        // the result
        "category = x\nresult = reset\nend = 1 ms\n",                                // the end
        "category = x\nresult = reset\nend = 1 ms 1 ticks\ntime 2 = 1 ms 1 ticks\n", // order
        "category = x\nresult = finished\nend = 1 ms 1 ticks\nsplit chapter_end 0 = T\n",
        "category = x\nresult = reset\nend = 1 ms 1 ticks\nwhat = 1\n",
    };
    for (const char *b : bad) {
        INFO(b);
        problems.clear();
        CHECK_FALSE(fe::parse_run_log(b, &problems).has_value());
        CHECK_FALSE(problems.empty());
    }
}

TEST_CASE("run timer: the overlay's lines and the times' text") {
    CHECK(fe::format_run_time(0) == "0:00.0");
    CHECK(fe::format_run_time(61999) == "1:01.9");
    CHECK(fe::format_run_time(3723456) == "1:02:03.4");
    CHECK(fe::format_run_time(-1250, true) == "-0:01.2");
    CHECK(fe::format_run_time(1250, true) == "+0:01.2");

    FakeClock clock;
    fe::RunTimer t(clock.clock());
    t.set_category(test_category(), std::nullopt);
    CHECK(t.overlay_lines() ==
          std::vector<std::string>{"Test%  0:00.0", "a new character starts the run"});
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    clock.advance(75.25, 1505);
    CHECK(t.overlay_lines() == std::vector<std::string>{"Test%  1:15.2", "next: Tutorial (1/3)",
                                                        "no personal best yet", "clean",
                                                        "fix_original_bugs off  skip_intros off",
                                                        "widescreen off  texture_pack off"});
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1));
    CHECK(t.overlay_lines()[1] == "finished (3/3)");
}

// --- Practice and best segments (#409, ADR 0189) -------------------------------------------------

TEST_CASE("best segments: a run's segments, the best of each and the sum of best") {
    FakeClock clock;
    fe::RunTimer t(clock.clock());
    t.set_category(test_category(), std::nullopt);
    CHECK(t.segments().best.size() == 3);
    CHECK_FALSE(t.segments().sum_of_best().has_value());

    // run_through: three segments of 10 s and 200 ticks each.
    run_through(t, clock);
    CHECK(t.take_segments_changed());
    CHECK_FALSE(t.take_segments_changed()); // once
    for (const std::optional<fe::RunTime> &b : t.segments().best) {
        REQUIRE(b.has_value());
        CHECK(*b == fe::RunTime{10000, 200});
    }
    CHECK(t.segments().sum_of_best() == fe::RunTime{30000, 600});

    // A slower run with one faster segment: only that segment improves.
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    clock.advance(12, 240);
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    CHECK(t.take_reached() == std::optional<std::size_t>(1));
    CHECK_FALSE(t.take_reached().has_value());
    CHECK_FALSE(t.take_segments_changed());
    clock.advance(7, 140);
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    CHECK(t.take_segments_changed());
    clock.advance(11, 220);
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1));
    const std::vector<fe::RunRecord> runs = t.take_records();
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].end.ms == 30000);
    CHECK(t.segments().best[1] == fe::RunTime{7000, 140});
    CHECK(t.segments().sum_of_best() == fe::RunTime{27000, 540});
}

TEST_CASE("practice: a load marks the run as practice and times that segment alone") {
    FakeClock clock;
    fe::Settings s;
    s.save_states = true; // a practice needs it: its segments still count
    fe::RunTimer t(clock.clock());
    t.attach(&s);
    t.set_category(test_category(), std::nullopt);

    // A run in progress past its first split: the load ends it as reset.
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    clock.advance(10, 200);
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    clock.advance(5, 100);
    CHECK_FALSE(t.start_practice(0)); // no state before the first split
    CHECK_FALSE(t.start_practice(3)); // nothing follows the last split
    CHECK_FALSE(t.practice());
    REQUIRE(t.start_practice(1));
    CHECK(t.practice());
    CHECK(t.practice_from() == 1);
    CHECK(t.state() == fe::RunTimer::State::kRunning);
    CHECK(t.next_split() == 1);
    std::vector<fe::RunRecord> records = t.take_records();
    REQUIRE(records.size() == 1);
    CHECK_FALSE(records[0].finished); // the run before the load: reset

    // The segment timer restarts at the load.
    CHECK(t.elapsed() == fe::RunTime{0, 0});
    clock.advance(8, 160);
    CHECK(t.elapsed() == fe::RunTime{8000, 160});
    std::vector<std::string> lines = t.overlay_lines();
    CHECK(lines[0] == "Test% practice  0:08.0");
    CHECK(lines[1] == "loaded: Tutorial (1/3)");
    CHECK(lines[2] == "next: Bandit (2/3)");
    CHECK(lines[3] == "no best segment yet");
    CHECK(lines[4] == "sum of best: 1 of 3 segments");
    CHECK(lines[5] == "practice, never a run");

    // An earlier split's event does nothing; the next one ends the segment, the next starts.
    t.on_event(event(MT_GAME_EVENT_CHAPTER_END, 0));
    CHECK(t.next_split() == 1);
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    CHECK(t.next_split() == 2);
    CHECK(t.take_reached() == std::optional<std::size_t>(2));
    CHECK(t.last_segment() == fe::RunTime{8000, 160});
    CHECK(t.segments().best[1] == fe::RunTime{8000, 160});
    CHECK(t.take_segments_changed());
    CHECK(t.elapsed() == fe::RunTime{0, 0});
    clock.advance(9, 180);
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1));
    CHECK(t.state() == fe::RunTimer::State::kFinished);
    CHECK(t.practice());
    CHECK(t.elapsed() == fe::RunTime{9000, 180});
    CHECK(t.segments().sum_of_best() == fe::RunTime{27000, 540});
    CHECK(t.take_segments_changed());
    lines = t.overlay_lines();
    CHECK(lines[2] == "finished (3/3)");
    CHECK(lines[3] == "last segment 0:09.0 (+0:00.0)");
    CHECK(lines[4] == "sum of best 0:27.0");
    // A practice is never a run: no record, no personal best.
    CHECK(t.take_records().empty());
    t.reset();
    CHECK_FALSE(t.practice());
    CHECK(t.take_records().empty());

    // Again from the same split, slower: the best stays; the overlay shows the difference.
    REQUIRE(t.start_practice(1));
    clock.advance(9.5, 190);
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    CHECK_FALSE(t.take_segments_changed());
    CHECK(t.segments().best[1] == fe::RunTime{8000, 160});
    lines = t.overlay_lines();
    CHECK(lines[4] == "last segment 0:09.5 (+0:01.5)");

    // A new character ends the practice and starts a run.
    t.on_event(event(MT_GAME_EVENT_NEW_CHARACTER, 0));
    CHECK_FALSE(t.practice());
    CHECK(t.state() == fe::RunTimer::State::kRunning);
    CHECK(t.next_split() == 0);
}

TEST_CASE(
    "practice: another Assist or a variable outside the category: the segment does not count") {
    FakeClock clock;
    fe::Settings s;
    s.save_states = true;
    fe::RunTimer t(clock.clock());
    t.attach(&s);
    t.set_category(test_category(), std::nullopt);
    REQUIRE(t.start_practice(1));
    s.game_speed = fe::GameSpeed::k25;
    t.turn();
    s.game_speed = fe::GameSpeed::kGame;
    clock.advance(3, 60);
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    CHECK(t.last_segment() == fe::RunTime{3000, 60});
    CHECK_FALSE(t.segments().best[1].has_value());
    CHECK_FALSE(t.take_segments_changed());
    // The next segment starts clean again.
    clock.advance(4, 80);
    t.on_event(event(MT_GAME_EVENT_MAP_CHANGE, 4, 1));
    CHECK(t.segments().best[2] == fe::RunTime{4000, 80});

    // skip_intros on: not allowed by test_category, so outside it.
    s.skip_intros = true;
    REQUIRE(t.start_practice(1));
    clock.advance(1, 20);
    t.on_event(event(MT_GAME_EVENT_JOB_ADVANCE, 2, 2));
    CHECK_FALSE(t.segments().best[1].has_value());
    const std::vector<std::string> lines = t.overlay_lines();
    CHECK(lines.back() == "practice: this segment does not count");
}

TEST_CASE("practice: the picked split on the overlay; another category drops the bests") {
    FakeClock clock;
    fe::RunTimer t(clock.clock());
    t.set_category(test_category(), std::nullopt);
    t.set_picked(2);
    std::vector<std::string> lines = t.overlay_lines();
    CHECK(lines[1] == "practice from: Bandit (2/3)");
    run_through(t, clock);
    CHECK(t.segments().known() == 3);

    // Bests read back are kept for the same splits, dropped for others.
    const fe::RunSegments kept = t.segments();
    t.set_category(test_category(), std::nullopt, kept);
    CHECK(t.segments() == kept);
    CHECK_FALSE(t.picked().has_value());
    fe::RunCategory other = test_category();
    other.splits.pop_back();
    t.set_category(other, std::nullopt, kept);
    CHECK(t.segments().known() == 0);
    CHECK(t.segments().best.size() == 2);
}

TEST_CASE("best segments: the file's round trip, and malformed files refused by line") {
    fe::RunSegments s = fe::run_segments_for(test_category());
    s.best[0] = fe::RunTime{10000, 200};
    s.best[2] = fe::RunTime{4000, 80};
    const std::string text = fe::run_segments_text(s);
    CHECK(text.find("segment 1 = 10000 ms 200 ticks\n") != std::string::npos);
    CHECK(text.find("segment 2") == std::string::npos);
    std::vector<std::string> problems;
    const std::optional<fe::RunSegments> back = fe::parse_run_segments(text, &problems);
    REQUIRE(back.has_value());
    CHECK(problems.empty());
    CHECK(*back == s);

    const struct {
        const char *text;
        const char *problem;
    } bad[] = {
        {"split chapter_end 0 = A\n", "no category or split line"},
        {"category = test\n", "no category or split line"},
        {"category = test\nsplit chapter_end 0 = A\nsegment 0 = 1 ms 1 ticks\n", "line 3: "},
        {"category = test\nsplit chapter_end 0 = A\nsegment 1 = 1 ms\n", "line 3: "},
        {"category = test\nsplit chapter_end 0 = A\nsegment 1 = 1 ms 1 ticks\n"
         "segment 1 = 2 ms 2 ticks\n",
         "line 4: "},
        {"category = test\nsplit chapter_end 0 = A\nsegment 2 = 1 ms 1 ticks\n",
         "segment 2 of a category with 1 splits"},
        {"category = Te st\nsplit chapter_end 0 = A\n", "line 1: "},
        {"category = test\nsplit nothing 0 = A\n", "line 2: a bad split line"},
        {"category = test\nsplit chapter_end 0 = A\nbest = 3\n", "line 3: unknown or bad line"},
    };
    for (const auto &b : bad) {
        problems.clear();
        CHECK_MESSAGE(!fe::parse_run_segments(b.text, &problems).has_value(), b.text);
        REQUIRE(problems.size() == 1);
        CHECK_MESSAGE(problems[0].rfind(b.problem, 0) == 0, problems[0]);
    }

    // The data folder: runs/<category>.segments, written and read back; another category's name
    // reads nothing.
    const fs::path dir = fresh_dir("mt_run_segments_test");
    std::string why;
    REQUIRE(fe::write_run_segments(path_text(dir), s, &why));
    CHECK(fs::is_regular_file(dir / "runs" / "test.segments"));
    CHECK(fe::load_run_segments(path_text(dir), "test") == s);
    CHECK_FALSE(fe::load_run_segments(path_text(dir), "any").has_value());
    CHECK_FALSE(fe::load_run_segments(path_text(dir), "../test").has_value());
    std::error_code ec;
    fs::remove_all(dir, ec);
}
