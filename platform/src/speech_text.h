// The game's dialogue read aloud (#391, ADR 0187, docs/re/speech.md): the runtime's text events
// (maplethief/text_events.h) as utterances for the speech (speech.h, #390).
//
//   a page         "speaker: text" (port_text.def's speech_speaker; the text alone when the game
//                  names no speaker), SpeechKind::kDialogue: a new page interrupts the last
//   its answer     Accept / Refuse, or the rows of a list, said after the page that asks for it
//                  in the same utterance (an answer of its own would cut the page off)
//   a scene box    "speaker: two lines", kDialogue, when a box opens and each time OK shows its
//                  next two lines
//
// skip_intros = on (scene_skip.h) taps every scene box away, so its boxes are not said: nothing
// is said of a scene the player does not watch. The runtime's wrappers run only while speech is
// on (mt_text_events_install), so with it off the game runs as without them.
//
// The events arrive on the guest thread, inside the guest's call (which is the run loop's):
// they are kept and said at the next turn. Single thread.
#ifndef MAPLETHIEF_PLATFORM_SPEECH_TEXT_H
#define MAPLETHIEF_PLATFORM_SPEECH_TEXT_H

#include <cstdint>
#include <string>
#include <vector>

#include "maplethief/text_events.h"
#include "settings.h"
#include "speech.h"

namespace maplethief::frontend {

// What the speech says for one event: "speaker: text" for a page or a scene box, the answer's
// words for an answer.
std::string speech_line(const mt_text_event &e);

class SpeechText {
  public:
    // Listens to the text events (the runtime keeps up to MT_TEXT_EVENTS_MAX_LISTENERS).
    SpeechText();
    ~SpeechText();
    SpeechText(const SpeechText &) = delete;
    SpeechText &operator=(const SpeechText &) = delete;

    // One run-loop turn: the wrappers on while `speech` is on, and what arrived since the last
    // turn said (scene boxes not with skip_intros on), at `now` (speech_clock()).
    void turn(Speech &speech, const Settings &settings, double now);
    // A save state is restored or the guest starts over: nothing kept is said.
    void restart();

    // What the listener heard (tests).
    void on_event(const mt_text_event &e);

    struct Stats {
        std::uint64_t pages = 0;       // page events heard
        std::uint64_t answers = 0;     // answer events heard (said with their page)
        std::uint64_t scene_boxes = 0; // scene box events heard
        std::uint64_t skipped = 0;     // scene boxes not said: skip_intros
        std::uint64_t said = 0;        // utterances handed to the speech
    };
    const Stats &stats() const { return stats_; }

  private:
    struct Pending {
        std::uint32_t kind = 0; // mt_text_event_kind of the first event in it
        std::string text;
    };
    std::vector<Pending> pending_;
    int installed_ = -1; // the wrappers' state last asked for (-1: not yet)
    Stats stats_;
};

} // namespace maplethief::frontend

#endif // MAPLETHIEF_PLATFORM_SPEECH_TEXT_H
