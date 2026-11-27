// The game's dialogue read aloud (#391, ADR 0187); see speech_text.h.
#include "speech_text.h"

#include <string_view>

#include "port_text.h"

namespace maplethief::frontend {

namespace {

void listener(void *user, const mt_text_event *e) {
    if (e != nullptr) {
        static_cast<SpeechText *>(user)->on_event(*e);
    }
}

std::string_view text_of(const char *s) { return s != nullptr ? s : ""; }

} // namespace

std::string speech_line(const mt_text_event &e) {
    const std::string_view text = text_of(e.text);
    if (e.kind == MT_TEXT_EVENT_ANSWER) {
        if (e.value != 2) {
            return tr(Txt::speech_accept_refuse);
        }
        std::string rows;
        for (std::size_t a = 0; a <= text.size();) {
            std::size_t b = text.find('\n', a);
            b = b == std::string_view::npos ? text.size() : b;
            if (b > a) {
                rows += (rows.empty() ? "" : "; ") + std::string(text.substr(a, b - a));
            }
            a = b + 1;
        }
        return rows.empty() ? std::string() : tr(Txt::speech_rows, {rows});
    }
    const std::string_view speaker = text_of(e.speaker);
    if (speaker.empty() || text.empty()) {
        return std::string(text);
    }
    return tr(Txt::speech_speaker, {speaker, text});
}

SpeechText::SpeechText() { mt_text_events_add_listener(listener, this); }

SpeechText::~SpeechText() {
    mt_text_events_remove_listener(listener, this);
    if (installed_ == 1) {
        mt_text_events_install(0);
    }
}

void SpeechText::on_event(const mt_text_event &e) {
    switch (e.kind) {
    case MT_TEXT_EVENT_PAGE:
        stats_.pages++;
        break;
    case MT_TEXT_EVENT_ANSWER:
        stats_.answers++;
        break;
    case MT_TEXT_EVENT_SCENE_BOX:
        stats_.scene_boxes++;
        break;
    default:
        return;
    }
    const std::string line = speech_line(e);
    // The answer goes with the page that asks for it (they come from one FirstPage or NextPage).
    if (e.kind == MT_TEXT_EVENT_ANSWER && !pending_.empty() &&
        pending_.back().kind == MT_TEXT_EVENT_PAGE) {
        std::string &text = pending_.back().text;
        text += text.empty() || line.empty() ? line : " " + line;
        return;
    }
    pending_.push_back(Pending{e.kind, line});
}

void SpeechText::turn(Speech &speech, const Settings &settings, double now) {
    const int on = speech.enabled() && speech.available() ? 1 : 0;
    if (on != installed_) {
        mt_text_events_install(on);
        installed_ = on;
    }
    if (on == 0) {
        pending_.clear();
        return;
    }
    for (Pending &p : pending_) {
        if (p.kind == MT_TEXT_EVENT_SCENE_BOX && settings.skip_intros) {
            stats_.skipped++;
            continue;
        }
        if (!p.text.empty()) {
            stats_.said++;
            speech.say(SpeechKind::kDialogue, std::move(p.text), now);
        }
    }
    pending_.clear();
}

void SpeechText::restart() {
    pending_.clear();
    mt_text_events_reset();
}

} // namespace maplethief::frontend
