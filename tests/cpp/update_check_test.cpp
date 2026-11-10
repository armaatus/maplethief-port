// The opt-in update notice (#199, ADR 0137, update_check.h) with a counting fetcher in place of
// the system's HTTP (CI never goes online): SemVer's precedence (pre-release, build metadata),
// the once-a-day gate, the release JSON, the state file, the rule that off, the Original preset
// and a scripted run make no call at all, the check on its own thread, and one log line per
// failure. The picker's question and the settings row are here too.

#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "canvas.h"
#include "perf_overlay.h"
#include "port_text.h"
#include "preset_picker.h"
#include "settings.h"
#include "settings_menu.h"
#include "update_check.h"

namespace {

namespace fe = maplethief::frontend;
namespace fs = std::filesystem;

constexpr std::int64_t kNow = 1'800'000'000;

std::optional<fe::SemVer> ver(const char *text) { return fe::parse_semver(text); }

int cmp(const char *a, const char *b) {
    const std::optional<fe::SemVer> va = ver(a);
    const std::optional<fe::SemVer> vb = ver(b);
    REQUIRE(va);
    REQUIRE(vb);
    return fe::compare_semver(*va, *vb);
}

// What a test's fetcher did, shared with the check's thread.
struct Counts {
    std::atomic<int> made{0};
    std::atomic<int> calls{0};
    std::mutex mutex;
    std::vector<std::string> urls;
    std::vector<std::string> logs;
};

class FakeFetcher final : public fe::UpdateFetcher {
  public:
    FakeFetcher(std::shared_ptr<Counts> counts, std::optional<std::string> body,
                std::shared_ptr<std::atomic<bool>> gate)
        : counts_(std::move(counts)), body_(std::move(body)), gate_(std::move(gate)) {}

    std::optional<std::string> get(const std::string &url, std::string *why) override {
        counts_->calls++;
        {
            const std::lock_guard<std::mutex> lock(counts_->mutex);
            counts_->urls.push_back(url);
        }
        while (gate_ != nullptr && !gate_->load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!body_) {
            *why = "the network is down";
        }
        return body_;
    }

  private:
    std::shared_ptr<Counts> counts_;
    std::optional<std::string> body_;
    std::shared_ptr<std::atomic<bool>> gate_;
};

std::string release_json(const std::string &tag, bool prerelease = false, bool draft = false) {
    return R"({"url":"https://evil.example/x","tag_name":")" + tag + R"(","draft":)" +
           (draft ? "true" : "false") + R"(,"prerelease":)" + (prerelease ? "true" : "false") +
           R"(,"author":{"tag_name":"v99.0.0"},"body":"tag_name v98.0.0"})";
}

struct Run {
    fs::path dir;
    fs::path state;
    std::shared_ptr<Counts> counts = std::make_shared<Counts>();
    fe::UpdateCheck check;

    explicit Run(const char *name) {
        dir = fs::temp_directory_path() / "maplethief_update_check_test" / name;
        fs::remove_all(dir);
        fs::create_directories(dir);
        state = dir / fe::kUpdateStateFile;
    }
    ~Run() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    fe::UpdateCheckConfig config(bool wanted, std::optional<std::string> body,
                                 std::shared_ptr<std::atomic<bool>> gate = nullptr,
                                 std::int64_t now = kNow) {
        fe::UpdateCheckConfig c;
        c.wanted = wanted;
        c.state_path = state.string();
        c.running_version = "1.0.0";
        c.now = now;
        c.make_fetcher = [counts = counts, body = std::move(body), gate = std::move(gate)]() {
            counts->made++;
            return std::unique_ptr<fe::UpdateFetcher>(new FakeFetcher(counts, body, gate));
        };
        c.log = [counts = counts](const std::string &line) {
            const std::lock_guard<std::mutex> lock(counts->mutex);
            counts->logs.push_back(line);
        };
        return c;
    }
    std::string read_state() const {
        std::ifstream in(state, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<std::string> logs() const {
        const std::lock_guard<std::mutex> lock(counts->mutex);
        return counts->logs;
    }
};

SDL_Event key(SDL_Keycode k) {
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = k;
    e.key.down = true;
    return e;
}

} // namespace

TEST_CASE("update check: SemVer parsing") {
    REQUIRE(ver("1.2.3"));
    CHECK(ver("1.2.3")->major == 1);
    CHECK(ver("1.2.3")->minor == 2);
    CHECK(ver("1.2.3")->patch == 3);
    CHECK(ver("v1.2.3"));
    REQUIRE(ver("1.0.0-rc.1"));
    CHECK(ver("1.0.0-rc.1")->pre == std::vector<std::string>{"rc", "1"});
    REQUIRE(ver("1.0.0-rc.1+abc.5"));
    CHECK(ver("1.0.0-rc.1+abc.5")->build == "abc.5");
    CHECK(ver("1.0.0+20260101-x")->build == "20260101-x");
    CHECK(ver("1.0.0+20260101-x")->pre.empty());
    CHECK(ver("1.0.0-alpha-1.x")->pre == std::vector<std::string>{"alpha-1", "x"});

    for (const char *bad : {"",          "v",         "1",
                            "1.2",       "1.2.3.4",   "01.2.3",
                            "1.02.3",    "1.2.03",    "1.2.x",
                            "1.2.3-",    "1.2.3-.a",  "1.2.3-a..b",
                            "1.2.3-01",  "1.2.3+",    "1.2.3+a..b",
                            "1.2.3 ",    " 1.2.3",    "V1.2.3",
                            "1.2.3-a b", "1.2.3-a/b", "99999999999999999999.0.0"}) {
        CAPTURE(bad);
        CHECK_FALSE(ver(bad));
    }
    CHECK(ver("1.2.3-0a")); // alphanumeric: not numeric, a leading zero is fine
    CHECK(ver("1.2.3-0"));  // a lone zero is fine
    CHECK(ver("1.2.3+01")); // build identifiers may lead with zero
}

TEST_CASE("update check: SemVer precedence") {
    CHECK(cmp("1.0.0", "1.0.0") == 0);
    CHECK(cmp("v1.0.0", "1.0.0") == 0);
    CHECK(cmp("1.0.1", "1.0.0") > 0);
    CHECK(cmp("1.1.0", "1.0.9") > 0);
    CHECK(cmp("2.0.0", "1.99.99") > 0);
    CHECK(cmp("1.10.0", "1.9.0") > 0); // numbers, not text
    // SemVer's own order of pre-releases.
    const char *order[] = {"1.0.0-alpha",  "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
                           "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1",       "1.0.0"};
    for (std::size_t i = 0; i + 1 < std::size(order); i++) {
        CAPTURE(order[i]);
        CHECK(cmp(order[i], order[i + 1]) < 0);
        CHECK(cmp(order[i + 1], order[i]) > 0);
    }
    CHECK(cmp("1.0.0-rc.2", "1.0.0-rc.10") < 0);
    CHECK(cmp("1.0.0-rc.99999999999999999999", "1.0.0-rc.100000000000000000000") < 0);
    CHECK(cmp("1.0.0-1", "1.0.0-a") < 0); // numeric before alphanumeric
    // Build metadata does not count.
    CHECK(cmp("1.0.0+a", "1.0.0+b") == 0);
    CHECK(cmp("1.0.0-rc.1+a", "1.0.0-rc.1") == 0);
    CHECK(cmp("1.0.1+a", "1.0.0+z") > 0);
}

TEST_CASE("update check: which release is newer") {
    const auto newer = [](const char *tag, const char *running, bool pre = false,
                          bool draft = false) {
        fe::ReleaseInfo r;
        r.tag = tag;
        r.prerelease = pre;
        r.draft = draft;
        return fe::newer_release(r, running);
    };
    CHECK(newer("v1.2.0", "1.0.0") == std::optional<std::string>("v1.2.0"));
    CHECK(newer("v1.0.0", "1.0.0-rc.1").has_value());
    CHECK_FALSE(newer("v1.0.0", "1.0.0"));
    CHECK_FALSE(newer("v0.9.0", "1.0.0"));
    CHECK_FALSE(newer("v1.2.0-rc.1", "1.0.0"));         // a pre-release by its tag
    CHECK_FALSE(newer("v1.2.0", "1.0.0", true));        // ... by GitHub's flag
    CHECK_FALSE(newer("v1.2.0", "1.0.0", false, true)); // a draft
    CHECK_FALSE(newer("not-a-version", "1.0.0"));       // a tag that isn't SemVer
    CHECK_FALSE(newer("v1.2.0", "not-a-version"));      // nor a build that isn't
    CHECK(newer("v1.0.1+build", "1.0.0").has_value());
}

TEST_CASE("update check: the once-a-day gate") {
    CHECK(fe::update_check_due(kNow, std::nullopt));
    CHECK_FALSE(fe::update_check_due(kNow, kNow));
    CHECK_FALSE(fe::update_check_due(kNow, kNow - fe::kUpdateIntervalSeconds + 1));
    CHECK(fe::update_check_due(kNow, kNow - fe::kUpdateIntervalSeconds));
    CHECK(fe::update_check_due(kNow, kNow - 10 * fe::kUpdateIntervalSeconds));
    CHECK(fe::update_check_due(kNow, kNow + 3600)); // a clock set back
}

TEST_CASE("update check: JSON of a canned response") {
    const auto r = fe::parse_release_json(release_json("v1.2.0"));
    REQUIRE(r);
    CHECK(r->tag == "v1.2.0");
    CHECK_FALSE(r->prerelease);
    CHECK_FALSE(r->draft);

    const auto pre = fe::parse_release_json(release_json("v1.2.0-rc.1", true));
    REQUIRE(pre);
    CHECK(pre->prerelease);
    const auto draft = fe::parse_release_json(release_json("v1.2.0", false, true));
    REQUIRE(draft);
    CHECK(draft->draft);

    // Only the top object's keys count; nesting, arrays, escapes and numbers are stepped over.
    const std::string shaped =
        R"(  { "assets": [ { "tag_name": "v50.0.0", "n": [1, -2.5e3, null] } ],
        "name": "a \"quoted\" name é \\ end",
        "id": 12345, "ok": true, "none": null,
        "tag_name": "v2.0.0"
      }
    )";
    const auto s = fe::parse_release_json(shaped);
    REQUIRE(s);
    CHECK(s->tag == "v2.0.0");

    // The tag may come with \u escapes of ASCII characters.
    const auto esc = fe::parse_release_json(R"({"tag_name":"v3.0.0"})");
    REQUIRE(esc);
    CHECK(esc->tag == "v3.0.0");

    for (const char *bad :
         {"", "null", "[]", "{", R"({"draft":false})", R"({"tag_name":1})",
          R"({"tag_name":"v1.0.0")", R"({"tag_name":"v1.0.0",})", R"({"tag_name":"v1.0.0"} x)",
          R"({"tag_name":"v1.0.0","draft":"no"})", R"({"tag_name":"v1.0.0","prerelease":1})",
          R"({"tag_name":"\x"})", "{\"tag_name\":\"v1\n\"}"}) {
        CAPTURE(bad);
        CHECK_FALSE(fe::parse_release_json(bad));
    }
    // Deep nesting is refused, not recursed into.
    std::string deep = "{\"a\":";
    for (int i = 0; i < 100000; i++) {
        deep += '[';
    }
    CHECK_FALSE(fe::parse_release_json(deep));
}

TEST_CASE("update check: the state file") {
    fe::UpdateState state;
    state.last_check = kNow;
    state.latest_tag = "v1.2.0";
    const fe::UpdateState back = fe::parse_update_state(fe::serialise_update_state(state));
    CHECK(back.last_check == kNow);
    CHECK(back.latest_tag == "v1.2.0");

    CHECK_FALSE(fe::parse_update_state("").last_check);
    CHECK_FALSE(fe::parse_update_state("garbage\nlast_check = soon\n").last_check);
    CHECK_FALSE(fe::parse_update_state("last_check = -5\n").last_check);
    CHECK(fe::parse_update_state("last_check = 12\r\nlatest = v1.0.0\r\n").last_check == 12);
    // A tag that isn't SemVer is not kept.
    CHECK(fe::parse_update_state("latest = ../../etc\n").latest_tag.empty());
}

TEST_CASE("update check: off, Original or a scripted run make no call") {
    fe::Settings s;
    CHECK_FALSE(s.update_check); // the default
    CHECK_FALSE(fe::update_check_wanted(s, true));
    s.update_check = true;
    CHECK(fe::update_check_wanted(s, true));
    CHECK_FALSE(fe::update_check_wanted(s, false)); // a scripted run
    s.preset = fe::Preset::kOriginal;
    CHECK_FALSE(fe::update_check_wanted(s, true));
    s.preset = fe::Preset::kEnhanced;
    CHECK(fe::update_check_wanted(s, true));
    s.preset = fe::Preset::kCustom;
    CHECK(fe::update_check_wanted(s, true));

    // With update_check = off the fetcher is never made and nothing touches the disk.
    Run run("off");
    fe::Settings off;
    std::string why;
    REQUIRE(fe::apply_setting(&off, "update_check", "off", &why));
    run.check.start(run.config(fe::update_check_wanted(off, true), release_json("v9.0.0")));
    CHECK(run.check.wait(1));
    CHECK(run.counts->made == 0);
    CHECK(run.counts->calls == 0);
    CHECK_FALSE(run.check.has_notice());
    CHECK(run.check.newer_tag().empty());
    CHECK(run.check.release_url().empty());
    CHECK_FALSE(fs::exists(run.state));
    CHECK(run.logs().empty());

    // The same with Original chosen and the setting on, and an old state naming a newer tag:
    // a preset that never checks shows nothing either.
    Run original("original");
    std::ofstream(original.state, std::ios::binary) << "latest = v9.0.0\n";
    fe::Settings orig;
    orig.update_check = true;
    orig.preset = fe::Preset::kOriginal;
    original.check.start(original.config(fe::update_check_wanted(orig, true), release_json("v9")));
    CHECK(original.counts->calls == 0);
    CHECK_FALSE(original.check.has_notice());
}

TEST_CASE("update check: the check runs on its own thread and is due once a day") {
    Run run("thread");
    auto gate = std::make_shared<std::atomic<bool>>(false);
    run.check.start(run.config(true, release_json("v1.2.0"), gate));
    // start() returned with the request held open: it did not wait for the network.
    CHECK_FALSE(run.check.wait(0.01));
    CHECK_FALSE(run.check.has_notice());
    gate->store(true);
    REQUIRE(run.check.wait(10));
    CHECK(run.counts->calls == 1);
    {
        const std::lock_guard<std::mutex> lock(run.counts->mutex);
        CHECK(run.counts->urls == std::vector<std::string>{fe::kUpdateApiUrl});
    }
    CHECK(run.check.has_notice());
    CHECK(run.check.newer_tag() == "v1.2.0");
    CHECK(run.check.release_url() ==
          "https://github.com/armaatus/maplethief-port/releases/tag/v1.2.0");
    // The address comes from the tag, not from the response's own (evil.example) one.
    REQUIRE(run.logs().size() == 1);
    const fe::UpdateState saved = fe::parse_update_state(run.read_state());
    CHECK(saved.last_check == kNow);
    CHECK(saved.latest_tag == "v1.2.0");

    // A later start the same day: no request, and the notice from the file.
    Run again("again");
    fs::copy_file(run.state, again.state);
    again.check.start(again.config(true, release_json("v1.5.0"), nullptr, kNow + 3600));
    CHECK(again.check.wait(1));
    CHECK(again.counts->made == 0);
    CHECK(again.counts->calls == 0);
    CHECK(again.check.newer_tag() == "v1.2.0");

    // A day on: due again.
    Run tomorrow("tomorrow");
    fs::copy_file(run.state, tomorrow.state);
    tomorrow.check.start(
        tomorrow.config(true, release_json("v1.5.0"), nullptr, kNow + fe::kUpdateIntervalSeconds));
    REQUIRE(tomorrow.check.wait(10));
    CHECK(tomorrow.counts->calls == 1);
    CHECK(tomorrow.check.newer_tag() == "v1.5.0");

    // The player has updated: the kept tag is not newer any more.
    Run updated("updated");
    fs::copy_file(run.state, updated.state);
    fe::UpdateCheckConfig c = updated.config(true, release_json("v1.2.0"), nullptr, kNow + 60);
    c.running_version = "1.2.0";
    updated.check.start(std::move(c));
    CHECK_FALSE(updated.check.has_notice());
}

TEST_CASE("update check: a failure is one log line and counts as the day's check") {
    const struct {
        const char *name;
        std::optional<std::string> body;
    } cases[] = {
        {"down", std::nullopt},
        {"garbage", std::string("<html>rate limited</html>")},
        {"prerelease", release_json("v2.0.0", true)},
        {"bad_tag", release_json("nightly")},
    };
    for (const auto &k : cases) {
        CAPTURE(k.name);
        Run run(k.name);
        run.check.start(run.config(true, k.body));
        REQUIRE(run.check.wait(10));
        CHECK(run.counts->calls == 1);
        CHECK_FALSE(run.check.has_notice());
        CHECK(run.logs().size() == 1);
        // The stamp was written before the request, so a failure does not ask again today.
        CHECK(fe::parse_update_state(run.read_state()).last_check == kNow);
    }
    // No HTTP on this system: one line, no stamp, no thread.
    Run none("no_http");
    fe::UpdateCheckConfig c = none.config(true, std::nullopt);
    c.make_fetcher = [] { return std::unique_ptr<fe::UpdateFetcher>(); };
    none.check.start(std::move(c));
    CHECK(none.logs().size() == 1);
    CHECK_FALSE(fs::exists(none.state));
    // A build whose version is not SemVer: one line, nothing else.
    Run dev("dev_version");
    fe::UpdateCheckConfig d = dev.config(true, release_json("v9.0.0"));
    d.running_version = "unknown";
    dev.check.start(std::move(d));
    CHECK(dev.counts->made == 0);
    CHECK(dev.logs().size() == 1);
}

TEST_CASE("update check: the platform fetcher can be made without going online") {
    // Only construction: this test never calls get(). macOS and Windows have one always; Linux
    // has one only with the system's libcurl.
    const std::unique_ptr<fe::UpdateFetcher> fetcher = fe::make_platform_update_fetcher();
#if defined(__APPLE__) || defined(_WIN32)
    CHECK(fetcher != nullptr);
#endif
    CHECK(fe::update_user_agent().rfind("MapleThief/", 0) == 0);
}

TEST_CASE("update check: the title corner's line is one box at the frame's bottom left") {
    const std::string line = "Port v1.2.0 is out: github.com/armaatus/maplethief-port";
    fe::TextFrame frame;
    frame.scale = 2;
    frame.area = fe::Rect{0, 0, 1280, 800};
    const fe::Rect r = fe::PerfOverlay::notice_rect(line, frame);
    // 8 pixels a character, 4 of padding each side, 2 margin, at scale 2.
    CHECK(r.w == static_cast<float>((line.size() * 8 + 8) * 2));
    CHECK(r.h == 32.0f);
    CHECK(r.x == 4.0f);
    CHECK(r.y + r.h == 796.0f);
}

TEST_CASE("update check: the setting's row, and the picker asks once") {
    const fe::SettingInfo *row = fe::find_setting("update_check");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kOther);
    CHECK_FALSE(row->presets.in_presets());
    fe::Settings s;
    for (const fe::Preset p : fe::kPresets) {
        if (p != fe::Preset::kCustom) {
            fe::apply_preset(&s, p);
            CHECK_FALSE(s.update_check); // no preset turns it on
        }
    }
    std::string why;
    REQUIRE(fe::apply_setting(&s, "update_check", "on", &why));
    CHECK(s.update_check);
    REQUIRE(fe::find_menu_action("update_notice") != nullptr);
    CHECK(fe::find_menu_action("update_notice")->group == fe::SettingGroup::kOther);

    // A preset other than Original: the question comes next, "No" selected first.
    {
        fe::PresetPicker picker(false, true, true);
        picker.handle(key(SDLK_3)); // Enhanced
        REQUIRE(picker.chosen() == fe::Preset::kEnhanced);
        CHECK(picker.asking_updates());
        CHECK_FALSE(picker.offering());
        CHECK_FALSE(picker.done());
        CHECK(picker.count() == 2);
        CHECK(picker.selected() == 0);
        picker.handle(key(SDLK_RETURN)); // the default answer: no
        CHECK(picker.update_check() == false);
        CHECK(picker.offering()); // then the controls step (#193)
        picker.handle(key(SDLK_2));
        CHECK(picker.done());
        CHECK(picker.show_controls() == false);
    }
    // Yes.
    {
        fe::PresetPicker picker(false, false, true);
        picker.handle(key(SDLK_2));
        CHECK(picker.asking_updates());
        picker.handle(key(SDLK_2)); // "Yes, once a day"
        CHECK(picker.update_check() == true);
        CHECK(picker.done());
    }
    // Original never checks, so the question is not asked.
    {
        fe::PresetPicker picker(false, true, true);
        picker.handle(key(SDLK_1)); // Original
        CHECK_FALSE(picker.asking_updates());
        CHECK_FALSE(picker.update_check());
        CHECK(picker.offering());
    }
    // The question fits the picker (480 points wide, canvas font).
    CHECK(fe::Canvas::text_width(fe::tr(fe::Txt::picker_updates_help)) <= fe::PresetPicker::kListW);
    // Without the step (older callers) nothing changes.
    {
        fe::PresetPicker picker(false, true);
        picker.handle(key(SDLK_3));
        CHECK_FALSE(picker.asking_updates());
        CHECK(picker.offering());
    }
}
