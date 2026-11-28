// The dialogue read aloud (#391, ADR 0187, speech_text.h) on the log backend: what is said for
// a page, its answer and a scene box; the answer said with its page; skip_intros = on says no
// scene box; speech off says nothing. The events here are
// made by hand; the runtime's own are text_events_test.cpp's, the real game's
// play.speech_dialogue. #392 adds the popups and the notify lines (below; the real game's
// play.speech_notices).
#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "maplethief/hooks.h"
#include "maplethief/text_events.h"
#include "settings.h"
#include "speech.h"
#include "speech_text.h"

namespace {

namespace fe = maplethief::frontend;

mt_text_event event(std::uint32_t kind, const char *speaker, const char *text, int value = 0) {
    mt_text_event e{};
    e.kind = kind;
    e.speaker_kind = speaker[0] != 0 ? MT_TEXT_SPEAKER_NPC : MT_TEXT_SPEAKER_NONE;
    e.speaker = speaker;
    e.text = text;
    e.value = value;
    return e;
}

struct Fixture {
    fe::LogSpeechBackend *log = nullptr;
    fe::Speech speech;
    fe::Settings settings;
    Fixture() {
        mt_hook_reset();
        auto backend = std::make_unique<fe::LogSpeechBackend>([](const std::string &) {});
        log = backend.get();
        speech.set_backend(std::move(backend));
        speech.set_enabled(true);
    }
    ~Fixture() { mt_hook_reset(); }
    std::vector<std::string> said() const {
        std::vector<std::string> out;
        for (const fe::Utterance &u : log->said()) {
            CHECK(u.kind == fe::SpeechKind::kDialogue);
            out.push_back(u.text);
        }
        return out;
    }
};

} // namespace

TEST_CASE("speech text: what a page, an answer and a scene box say (#391)") {
    CHECK(fe::speech_line(event(MT_TEXT_EVENT_PAGE, "Mai", "Hello there.")) == "Mai: Hello there.");
    CHECK(fe::speech_line(event(MT_TEXT_EVENT_PAGE, "", "Nobody speaks.")) == "Nobody speaks.");
    CHECK(fe::speech_line(event(MT_TEXT_EVENT_SCENE_BOX, "Robin", "Where am I?")) ==
          "Robin: Where am I?");
    CHECK(fe::speech_line(event(MT_TEXT_EVENT_ANSWER, "Mai", "", 1)) == "Accept or refuse.");
    CHECK(fe::speech_line(event(MT_TEXT_EVENT_ANSWER, "Mai", "", 3)) == "Accept or refuse.");
    CHECK(fe::speech_line(event(MT_TEXT_EVENT_ANSWER, "Mai", "Go left\nGo right\n", 2)) ==
          "Choose: Go left; Go right.");
    CHECK(fe::speech_line(event(MT_TEXT_EVENT_ANSWER, "Mai", "", 2)).empty());
}

TEST_CASE("speech text: each page is said at the turn, its answer with it (#391)") {
    Fixture f;
    fe::SpeechText text;
    text.turn(f.speech, f.settings, 0.0); // speech on: the wrappers on
    text.on_event(event(MT_TEXT_EVENT_PAGE, "Mai", "Page one."));
    CHECK(f.said().empty()); // said at the turn, not inside the guest's call
    text.turn(f.speech, f.settings, 1.0);
    text.on_event(event(MT_TEXT_EVENT_PAGE, "Mai", "Will you help?"));
    text.on_event(event(MT_TEXT_EVENT_ANSWER, "Mai", "", 1));
    text.turn(f.speech, f.settings, 2.0);
    const std::vector<std::string> want = {"Mai: Page one.",
                                           "Mai: Will you help? Accept or refuse."};
    CHECK(f.said() == want);
    CHECK(text.stats().pages == 2);
    CHECK(text.stats().answers == 1);
    CHECK(text.stats().said == 2);
}

TEST_CASE("speech text: skip_intros = on says no scene box, and still the dialogue (#391)") {
    Fixture f;
    fe::SpeechText text;
    f.settings.skip_intros = true;
    text.turn(f.speech, f.settings, 0.0);
    text.on_event(event(MT_TEXT_EVENT_SCENE_BOX, "Robin", "Tapped away."));
    text.on_event(event(MT_TEXT_EVENT_SCENE_BOX, "Robin", "This one too."));
    text.turn(f.speech, f.settings, 1.0);
    CHECK(f.said().empty());
    CHECK(text.stats().skipped == 2);
    text.on_event(event(MT_TEXT_EVENT_PAGE, "Mai", "A page."));
    text.turn(f.speech, f.settings, 2.0);
    CHECK(f.said() == std::vector<std::string>{"Mai: A page."});
    // skip_intros off: the boxes are said.
    f.settings.skip_intros = false;
    text.on_event(event(MT_TEXT_EVENT_SCENE_BOX, "Robin", "Watched."));
    text.turn(f.speech, f.settings, 3.0);
    CHECK(f.said().back() == "Robin: Watched.");
}

TEST_CASE("speech text: speech off says nothing, not even later (#391)") {
    Fixture f;
    f.speech.set_enabled(false);
    fe::SpeechText text;
    text.turn(f.speech, f.settings, 0.0);
    text.on_event(event(MT_TEXT_EVENT_PAGE, "Mai", "Unheard."));
    text.turn(f.speech, f.settings, 1.0);
    CHECK(f.said().empty());
    // On again: what arrived while off is not said later.
    f.speech.set_enabled(true);
    text.turn(f.speech, f.settings, 2.0);
    CHECK(f.said().empty());
    text.on_event(event(MT_TEXT_EVENT_PAGE, "Mai", "Heard."));
    text.turn(f.speech, f.settings, 3.0);
    CHECK(f.said() == std::vector<std::string>{"Mai: Heard."});
}

// --- Popups and notify lines (#392, ADR 0191) ----------------------------------------------

namespace {

mt_text_event popup(int id, int kind, const char *text) {
    mt_text_event e = event(MT_TEXT_EVENT_POPUP, "", text, kind);
    e.speaker_kind = MT_TEXT_SPEAKER_NONE;
    e.index = id;
    return e;
}

// Everything said, kind and text, in order.
std::vector<std::pair<fe::SpeechKind, std::string>> all_said(const Fixture &f) {
    std::vector<std::pair<fe::SpeechKind, std::string>> out;
    for (const fe::Utterance &u : f.log->said()) {
        out.emplace_back(u.kind, u.text);
    }
    return out;
}

} // namespace

TEST_CASE("speech text: what a popup says, the death popup's own words (#392)") {
    CHECK(fe::speech_line(popup(0x69, 1, "Move to the nearest safe village.")) ==
          "Move to the nearest safe village. Yes or No.");
    CHECK(fe::speech_line(popup(600, 3, "You can't gain EXP anymore.")) ==
          "You can't gain EXP anymore. OK.");
    CHECK(fe::speech_line(popup(8, 0, "Quest complete.")) == "Quest complete.");
    CHECK(fe::speech_line(popup(MT_TEXT_POPUP_DEATH, 0, "")) ==
          "You died. Grade revival, Revival, Return to town, or Go to main menu.");
    CHECK(fe::speech_line(popup(0x20, 3, "")) == "OK.");
    CHECK(fe::speech_line(popup(0x21, 0, "")).empty());
    CHECK(fe::speech_line(popup(5, 4, "How many?")) == "How many? OK or Cancel.");
    CHECK(fe::merged_notices({"Meso obtained.(+70)", "", "EXP obtained.(+7)", "Done!"}) ==
          "Meso obtained.(+70). EXP obtained.(+7). Done!");
    CHECK(fe::merged_notices({}).empty());
}

TEST_CASE(
    "speech text: a popup interrupts like a page; a turn's notify lines are one notice (#392)") {
    Fixture f;
    fe::SpeechText text;
    text.turn(f.speech, f.settings, 0.0);
    // A fight's burst in one turn: one notice.
    text.on_event(event(MT_TEXT_EVENT_NOTICE, "", "EXP obtained.(+7)"));
    text.on_event(event(MT_TEXT_EVENT_NOTICE, "", "Meso obtained.(+70)"));
    text.on_event(event(MT_TEXT_EVENT_NOTICE, "", "Red Potion Obtained!"));
    text.turn(f.speech, f.settings, 1.0);
    f.speech.turn(1.0);
    using K = fe::SpeechKind;
    using Said = std::vector<std::pair<K, std::string>>;
    CHECK(all_said(f) ==
          Said{{K::kNotice, "EXP obtained.(+7). Meso obtained.(+70). Red Potion Obtained!"}});
    CHECK(text.stats().notices == 3);
    CHECK(text.stats().notice_utterances == 1);
    // A turn with a page and a line: the page first (a line said first would be cut off by it),
    // the line queued behind it, said once the page is over.
    text.on_event(event(MT_TEXT_EVENT_NOTICE, "", "EXP obtained.(+8)"));
    text.on_event(event(MT_TEXT_EVENT_PAGE, "Mai", "Hello."));
    text.turn(f.speech, f.settings, 10.0);
    f.speech.turn(10.0);
    CHECK(all_said(f).back() == std::pair<K, std::string>{K::kDialogue, "Mai: Hello."});
    f.speech.turn(20.0);
    CHECK(all_said(f).back() == std::pair<K, std::string>{K::kNotice, "EXP obtained.(+8)."});
    // The death popup, then its confirmation: dialogue, each interrupting the last.
    text.on_event(popup(MT_TEXT_POPUP_DEATH, 0, ""));
    text.turn(f.speech, f.settings, 30.0);
    text.on_event(popup(0x69, 1, "Move to the nearest safe village. EXP: 5 Decrease"));
    text.turn(f.speech, f.settings, 30.5);
    const Said s = all_said(f);
    REQUIRE(s.size() == 5);
    CHECK(s[3] == std::pair<K, std::string>{
                      K::kDialogue,
                      "You died. Grade revival, Revival, Return to town, or Go to main menu."});
    CHECK(s[4] ==
          std::pair<K, std::string>{
              K::kDialogue, "Move to the nearest safe village. EXP: 5 Decrease Yes or No."});
    CHECK(f.log->stops() >= 1); // the confirmation cut the death popup off
    CHECK(text.stats().popups == 2);
}

TEST_CASE("speech text: a long fight's notices never pile up, and never cut off a page (#392)") {
    Fixture f;
    fe::SpeechText text;
    text.turn(f.speech, f.settings, 0.0);
    text.on_event(event(MT_TEXT_EVENT_PAGE, "Mai", "A long page that is still being read."));
    text.turn(f.speech, f.settings, 0.0);
    // Twenty turns of lines while the page is read: none interrupts it, at most the queue's
    // limit wait, the newest kept.
    for (int i = 0; i < 20; i++) {
        const std::string line = "EXP obtained.(+" + std::to_string(i) + ")";
        text.on_event(event(MT_TEXT_EVENT_NOTICE, "", line.c_str()));
        text.turn(f.speech, f.settings, 0.01 * i);
        f.speech.turn(0.01 * i);
    }
    CHECK(f.log->said().size() == 1);
    CHECK(f.log->stops() == 0);
    CHECK(f.speech.stats().dropped == 20 - fe::Speech::kQueueLimit);
    for (double t = 10.0; t < 200.0; t += 5.0) {
        f.speech.turn(t);
    }
    const auto s = all_said(f);
    REQUIRE(s.size() == 1 + fe::Speech::kQueueLimit);
    CHECK(s.back().second == "EXP obtained.(+19).");
}

TEST_CASE("speech text: speech_notices = off says no notify line, still the popups (#392)") {
    Fixture f;
    fe::SpeechText text;
    f.settings.speech_notices = false;
    text.turn(f.speech, f.settings, 0.0);
    text.on_event(event(MT_TEXT_EVENT_NOTICE, "", "Meso obtained.(+70)"));
    text.on_event(popup(600, 3, "You can't gain EXP anymore."));
    text.turn(f.speech, f.settings, 1.0);
    f.speech.turn(50.0);
    CHECK(f.said() == std::vector<std::string>{"You can't gain EXP anymore. OK."});
    CHECK(text.stats().notices_off == 1);
}

TEST_CASE("settings: speech_notices (#392)") {
    fe::Settings s;
    CHECK(s.speech_notices);
    CHECK(fe::parse_settings("speech_notices = off\n", &s).empty());
    CHECK_FALSE(s.speech_notices);
    const fe::SettingInfo *row = fe::find_setting("speech_notices");
    REQUIRE(row != nullptr);
    CHECK(row->applies == fe::Applies::kAtOnce);
    CHECK(row->group == fe::SettingGroup::kAccessibility);
}
