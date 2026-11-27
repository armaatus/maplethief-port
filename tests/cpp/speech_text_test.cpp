// The dialogue read aloud (#391, ADR 0187, speech_text.h) on the log backend: what is said for
// a page, its answer and a scene box; the answer said with its page; skip_intros = on says no
// scene box; speech off says nothing. The events here are
// made by hand; the runtime's own are text_events_test.cpp's, the real game's
// play.speech_dialogue.
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
