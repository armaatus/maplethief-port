// Text-to-speech (#390, ADR 0174, speech.h) on the log backend: the queue rules (a dialogue page
// interrupts, notices queue behind it, the oldest notices dropped past the limit, off never
// speaks), the voice's language, the length estimate, the backend choice (MAPLETHIEF_SPEECH),
// Speech Dispatcher missing, the settings rows and their live change. The system voice itself
// speaks in "speech: the system voice speaks a test string", which runs only with
// MAPLETHIEF_SPEECH_SYSTEM_TEST=1 (a voice and an audio output: a local run, not CI).
#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "live_settings.h"
#include "settings.h"
#include "speech.h"

namespace {

namespace fe = maplethief::frontend;
using fe::SpeechKind;

// A Speech on a log backend whose lines go to `lines`; `log` is the backend, which the Speech
// owns.
struct LogSpeech {
    std::vector<std::string> lines;
    fe::LogSpeechBackend *log = nullptr;
    fe::Speech speech;

    explicit LogSpeech(bool on = true) {
        auto backend = std::make_unique<fe::LogSpeechBackend>(
            [this](const std::string &l) { lines.push_back(l); });
        log = backend.get();
        speech.set_backend(std::move(backend));
        speech.set_enabled(on);
    }
    std::vector<std::string> said() const {
        std::vector<std::string> out;
        for (const fe::Utterance &u : log->said()) {
            out.push_back(u.text);
        }
        return out;
    }
};

// Long enough to be speaking for seconds at 100%.
const std::string kPage = "A page of dialogue that takes the voice a few seconds to say aloud.";

} // namespace

TEST_CASE("speech: a new dialogue page interrupts the one being spoken") {
    LogSpeech s;
    s.speech.say(SpeechKind::kDialogue, kPage, 0.0);
    CHECK(s.speech.speaking(0.1));
    s.speech.say(SpeechKind::kDialogue, "the next page", 0.5);
    CHECK(s.said() == std::vector<std::string>{kPage, "the next page"});
    CHECK(s.log->stops() == 1);
    CHECK(s.speech.stats().interrupted == 1);
    CHECK(s.speech.stats().spoken == 2);
    // A menu row interrupts as well; once a page is done, nothing is cut off.
    s.speech.say(SpeechKind::kMenu, "a row", 0.6);
    CHECK(s.log->stops() == 2);
    const double done = 0.6 + fe::speech_seconds("a row", 100) + 0.01;
    CHECK_FALSE(s.speech.speaking(done));
    s.speech.say(SpeechKind::kDialogue, "after a pause", done);
    CHECK(s.log->stops() == 2);
    CHECK(s.speech.stats().interrupted == 2);
    CHECK(s.log->said().back().kind == SpeechKind::kDialogue);
    // The log's line names the kind and the voice.
    CHECK(s.lines.back() == "speech: dialogue [en-US 100% 100%] after a pause");
}

TEST_CASE("speech: notices queue behind what is being spoken") {
    LogSpeech s;
    // Idle: a notice is spoken at once.
    s.speech.say(SpeechKind::kNotice, "first notice", 0.0);
    CHECK(s.said() == std::vector<std::string>{"first notice"});
    double t = fe::speech_seconds("first notice", 100) + 0.01;
    s.speech.turn(t);
    s.speech.say(SpeechKind::kDialogue, kPage, t);
    s.speech.say(SpeechKind::kNotice, "pick-up", t + 0.1);
    s.speech.say(SpeechKind::kNotice, "meso", t + 0.2);
    CHECK(s.speech.queued() == 2);
    CHECK(s.log->stops() == 0); // nothing was cut off
    s.speech.turn(t + 0.3);     // the page still speaks
    CHECK(s.said().size() == 2);
    // The page ends: one notice per turn, each after the last is done.
    t += fe::speech_seconds(kPage, 100) + 0.01;
    s.speech.turn(t);
    CHECK(s.said().back() == "pick-up");
    s.speech.turn(t + 0.01);
    CHECK(s.said().back() == "pick-up");
    s.speech.turn(t + fe::speech_seconds("pick-up", 100) + 0.01);
    CHECK(s.said() == std::vector<std::string>{"first notice", kPage, "pick-up", "meso"});
    CHECK(s.speech.queued() == 0);
    // A page that comes while notices wait cuts off the one speaking; the waiting ones follow.
    LogSpeech q;
    q.speech.say(SpeechKind::kDialogue, kPage, 0.0);
    q.speech.say(SpeechKind::kNotice, "waiting", 0.1);
    q.speech.say(SpeechKind::kDialogue, "the next page", 0.2);
    CHECK(q.speech.queued() == 1);
    q.speech.turn(0.2 + fe::speech_seconds("the next page", 100) + 0.01);
    CHECK(q.said() == std::vector<std::string>{kPage, "the next page", "waiting"});
}

TEST_CASE("speech: past the limit the oldest queued notices are dropped") {
    LogSpeech s;
    s.speech.say(SpeechKind::kDialogue, kPage, 0.0);
    const std::size_t n = fe::Speech::kQueueLimit + 2;
    for (std::size_t i = 0; i < n; i++) {
        s.speech.say(SpeechKind::kNotice, "notice " + std::to_string(i), 0.1);
    }
    CHECK(s.speech.queued() == fe::Speech::kQueueLimit);
    CHECK(s.speech.stats().dropped == 2);
    double t = fe::speech_seconds(kPage, 100);
    for (int turn = 0; turn < 20; t += 1.0, turn++) {
        s.speech.turn(t);
    }
    std::vector<std::string> expect{kPage};
    for (std::size_t i = 2; i < n; i++) {
        expect.push_back("notice " + std::to_string(i));
    }
    CHECK(s.said() == expect);
    CHECK(s.speech.stats().said == n + 1);
    CHECK(s.speech.stats().spoken == n - 1);
}

TEST_CASE("speech: off never speaks, and turning it off stops the voice") {
    LogSpeech off(false);
    for (const SpeechKind k : {SpeechKind::kDialogue, SpeechKind::kNotice, SpeechKind::kMenu}) {
        off.speech.say(k, "nothing", 0.0);
        off.speech.turn(10.0);
    }
    CHECK(off.log->said().empty());
    CHECK(off.lines.empty());
    CHECK(off.speech.stats().ignored == 3);
    CHECK(off.speech.stats().said == 0);

    LogSpeech s;
    s.speech.say(SpeechKind::kDialogue, kPage, 0.0);
    s.speech.say(SpeechKind::kNotice, "queued", 0.1);
    s.speech.set_enabled(false);
    CHECK(s.log->stops() == 1);
    CHECK(s.speech.queued() == 0);
    CHECK_FALSE(s.speech.speaking(0.2));
    s.speech.turn(100.0);
    s.speech.say(SpeechKind::kNotice, "while off", 100.0);
    CHECK(s.said() == std::vector<std::string>{kPage});
    // Empty text and no backend are ignored too.
    s.speech.set_enabled(true);
    s.speech.say(SpeechKind::kDialogue, "", 101.0);
    fe::Speech none;
    none.set_enabled(true);
    none.say(SpeechKind::kDialogue, "no voice", 0.0);
    CHECK_FALSE(none.available());
    CHECK(none.stats().ignored == 1);
    CHECK(s.said().size() == 1);
}

TEST_CASE("speech: the voice's language follows the game's, or the text pack's") {
    CHECK(fe::speech_language("en", nullptr) == "en-US");
    CHECK(fe::speech_language("ko", nullptr) == "ko-KR");
    CHECK(fe::speech_language("ko", "fr") == "fr");
    CHECK(fe::speech_language("en", "pt_BR") == "pt-BR");
    CHECK(fe::speech_language("en", "zh-Hant-TW") == "zh-Hant-TW");
    // Not a tag: the game's.
    CHECK(fe::speech_language("ko", "") == "ko-KR");
    CHECK(fe::speech_language("en", "../x") == "en-US");
    CHECK(fe::speech_language("en", "1de") == "en-US");
    CHECK(fe::speech_language("en", "a-very-long-tag-that-is-no-language-at-all") == "en-US");

    fe::Settings set;
    set.speech_rate = 150;
    set.speech_volume = 40;
    const fe::SpeechVoice v = fe::speech_voice(set, "ko-KR");
    CHECK(v.language == "ko-KR");
    CHECK(v.rate == 150u);
    CHECK(v.volume == 40u);
    // The log backend says which voice.
    LogSpeech s;
    s.speech.set_voice(v);
    s.speech.say(SpeechKind::kNotice, "\xEC\x95\x88\xEB\x85\x95", 0.0); // two Hangul syllables
    CHECK(s.lines.back() == "speech: notice [ko-KR 150% 40%] \xEC\x95\x88\xEB\x85\x95");
}

TEST_CASE("speech: the length estimate") {
    // Hangul is slower per character than Latin letters; twice the rate, half the time.
    const std::string hangul = "\xEC\x95\x88\xEB\x85\x95\xED\x95\x98\xEC\x84\xB8\xEC\x9A\x94";
    CHECK(fe::speech_seconds(hangul, 100) > fe::speech_seconds("hello", 100));
    CHECK(fe::speech_seconds(kPage, 200) == doctest::Approx(fe::speech_seconds(kPage, 100) / 2));
    CHECK(fe::speech_seconds("", 100) == doctest::Approx(0.25));
    // A bad UTF-8 byte is one character; out-of-range rates are clamped.
    CHECK(fe::speech_seconds("\xFF\xC3", 100) == doctest::Approx(0.25 + 2.0 / 14.0));
    CHECK(fe::speech_seconds(kPage, 1000) == doctest::Approx(fe::speech_seconds(kPage, 200)));
}

TEST_CASE("speech: MAPLETHIEF_SPEECH picks the backend; no libspeechd means no voice") {
    CHECK(fe::parse_speech_backend(nullptr) == fe::SpeechBackendChoice::kSystem);
    CHECK(fe::parse_speech_backend("") == fe::SpeechBackendChoice::kSystem);
    CHECK(fe::parse_speech_backend("system") == fe::SpeechBackendChoice::kSystem);
    CHECK(fe::parse_speech_backend("log") == fe::SpeechBackendChoice::kLog);
    CHECK(fe::parse_speech_backend("none") == fe::SpeechBackendChoice::kNone);
    CHECK_FALSE(fe::parse_speech_backend("LOG").has_value());
    CHECK_FALSE(fe::parse_speech_backend("espeak").has_value());

    const std::unique_ptr<fe::SpeechBackend> log =
        fe::open_speech_backend(fe::SpeechBackendChoice::kLog);
    REQUIRE(log != nullptr);
    CHECK(std::string(log->name()) == "log");
    CHECK(fe::open_speech_backend(fe::SpeechBackendChoice::kNone) == nullptr);

    // Linux without Speech Dispatcher: the library does not load, so there is no backend and the
    // game runs on with speech unavailable (the rows say so: settings_menu_test).
    CHECK(fe::open_speechd_backend("libspeechd-not-installed.so.0") == nullptr);
    fe::Speech speech(fe::open_speechd_backend("libspeechd-not-installed.so.0"));
    CHECK_FALSE(speech.available());
    CHECK(std::string(speech.backend_name()) == "none");
    speech.set_enabled(true);
    speech.say(SpeechKind::kDialogue, "nothing", 0.0);
    speech.turn(1.0);
    CHECK(speech.stats().spoken == 0);
}

TEST_CASE("settings: speech, speech_rate and speech_volume (#390)") {
    fe::Settings s;
    CHECK_FALSE(s.speech);
    CHECK(s.speech_rate == 100u);
    CHECK(s.speech_volume == 100u);
    CHECK(fe::parse_settings("speech = on\nspeech_rate = 150\nspeech_volume = 30\n", &s).empty());
    CHECK(s.speech);
    CHECK(s.speech_rate == 150u);
    CHECK(s.speech_volume == 30u);
    // Out of range keeps the old value.
    CHECK(fe::parse_settings("speech_rate = 40\nspeech_rate = 210\nspeech_volume = 101\n", &s)
              .size() == 3);
    CHECK(s.speech_rate == 150u);
    CHECK(s.speech_volume == 30u);
    const std::string text = fe::serialise_settings(s);
    CHECK(text.find("speech = on\n") != std::string::npos);
    CHECK(text.find("speech_rate = 150\n") != std::string::npos);
    fe::Settings r;
    CHECK(fe::parse_settings(text, &r).empty());
    CHECK(r == s);
    // Accessibility, live, in no preset, a flag each, only where the platform has a voice.
    for (const char *key : {"speech", "speech_rate", "speech_volume"}) {
        const fe::SettingInfo *row = fe::find_setting(key);
        REQUIRE(row != nullptr);
        CHECK(row->group == fe::SettingGroup::kAccessibility);
        CHECK(row->applies == fe::Applies::kAtOnce);
        CHECK(row->presets.original == nullptr);
        CHECK(row->presets.original_controller == nullptr);
        CHECK(row->presets.enhanced == nullptr);
        CHECK(row->needs == fe::kCapSpeech);
        CHECK(fe::setting_shown(*row, fe::SettingsPlatform::kDesktop));
        CHECK(fe::setting_shown(*row, fe::SettingsPlatform::kIos));
        CHECK_FALSE(fe::setting_shown(*row, fe::SettingsPlatform::kSwitch));
    }
    CHECK(fe::find_setting_flag("--speech") == fe::find_setting("speech"));
    CHECK(fe::find_setting_flag("--speech-rate") == fe::find_setting("speech_rate"));
    CHECK(fe::find_setting_flag("--speech-volume") == fe::find_setting("speech_volume"));
    for (const fe::Preset p :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        fe::Settings mine = s;
        fe::apply_preset(&mine, p);
        CHECK(mine.speech);
        CHECK(mine.speech_rate == 150u);
        CHECK(mine.speech_volume == 30u);
    }
}

TEST_CASE("speech rows are live through apply_live_setting, with a sample of the voice") {
    LogSpeech s(false);
    s.speech.set_voice(fe::speech_voice(fe::Settings{}, "ko-KR"));
    fe::LiveTargets t;
    t.speech = &s.speech;
    fe::Settings set;
    set.speech = true;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("speech"), set));
    CHECK(s.speech.enabled());
    REQUIRE(s.log->said().size() == 1);
    CHECK(s.log->said()[0].kind == SpeechKind::kMenu);
    set.speech_rate = 180;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("speech_rate"), set));
    CHECK(s.speech.voice().rate == 180u);
    CHECK(s.speech.voice().language == "ko-KR"); // the language stays
    set.speech_volume = 20;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("speech_volume"), set));
    CHECK(s.speech.voice().volume == 20u);
    CHECK(s.log->said().size() == 3); // each change speaks the sample
    set.speech = false;
    CHECK(fe::apply_live_setting(t, *fe::find_setting("speech"), set));
    CHECK_FALSE(s.speech.enabled());
    CHECK(s.log->said().size() == 3); // off: no sample
    // Without a Speech it waits for the next start.
    CHECK_FALSE(fe::apply_live_setting(fe::LiveTargets{}, *fe::find_setting("speech"), set));
}

TEST_CASE("speech: the system voice speaks a test string") {
    const char *want = SDL_getenv("MAPLETHIEF_SPEECH_SYSTEM_TEST");
    if (want == nullptr || std::string(want) != "1") {
        MESSAGE("skipped: set MAPLETHIEF_SPEECH_SYSTEM_TEST=1 to hear the system voice");
        return;
    }
    fe::Speech speech(fe::open_system_speech_backend());
    REQUIRE_MESSAGE(speech.available(), "this platform's system voice did not open");
    MESSAGE("system voice: " << speech.backend_name());
    speech.set_enabled(true);
    const double start = fe::speech_clock();
    speech.say(SpeechKind::kDialogue, "MapleThief speech test: one, two, three.", start);
    CHECK(speech.stats().spoken == 1);
    CHECK(speech.stats().failed == 0);
    bool was_speaking = false;
    double end = start;
    while (fe::speech_clock() - start < 20.0) {
        const double now = fe::speech_clock();
        speech.turn(now);
        if (speech.speaking(now)) {
            was_speaking = true;
            end = now;
        } else if (was_speaking) {
            break;
        }
        SDL_Delay(20);
    }
    CHECK(was_speaking);
    CHECK_FALSE(speech.speaking(fe::speech_clock()));
    MESSAGE("spoke for " << (end - start) << " s");
    CHECK(end - start > 0.5); // it said something, not nothing
}
