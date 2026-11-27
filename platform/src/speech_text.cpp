// The game's dialogue, notify lines and popups read aloud (#391, ADR 0187; #392, ADR 0191); see
// speech_text.h.
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

// A popup's buttons, by its kind (Popup_PlaceShellButtons 0x78E91); kind 0 has none.
std::string popup_buttons(std::int32_t kind) {
    switch (kind) {
    case 1:
    case 2:
    case 5:
        return tr(Txt::speech_popup_yes_no);
    case 3:
    case 6:
        return tr(Txt::speech_popup_ok);
    case 4:
        return tr(Txt::speech_popup_ok_cancel);
    default:
        return std::string();
    }
}

bool sentence_end(char c) { return c == '.' || c == '!' || c == '?'; }

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
    if (e.kind == MT_TEXT_EVENT_POPUP) {
        if (e.index == MT_TEXT_POPUP_DEATH && text.empty()) {
            return tr(Txt::speech_death_popup);
        }
        const std::string buttons = popup_buttons(e.value);
        if (text.empty() || buttons.empty()) {
            return text.empty() ? buttons : std::string(text);
        }
        return std::string(text) + " " + buttons;
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

std::string merged_notices(const std::vector<std::string> &lines) {
    std::string out;
    for (const std::string &l : lines) {
        if (l.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += sentence_end(out.back()) ? " " : ". ";
        }
        out += l;
    }
    if (!out.empty() && !sentence_end(out.back())) {
        out += '.';
    }
    return out;
}

void SpeechText::on_event(const mt_text_event &e) {
    if (e.kind == MT_TEXT_EVENT_NOTICE) {
        stats_.notices++;
        notices_.emplace_back(text_of(e.text));
        return;
    }
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
    case MT_TEXT_EVENT_POPUP:
        stats_.popups++;
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
        notices_.clear();
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
    // The turn's notify lines as one notice, after its dialogue (which would cut it off), queued
    // behind what is being said (#392).
    if (!notices_.empty()) {
        std::string text = settings.speech_notices ? merged_notices(notices_) : std::string();
        if (!settings.speech_notices) {
            stats_.notices_off += notices_.size();
        } else if (!text.empty()) {
            stats_.said++;
            stats_.notice_utterances++;
            speech.say(SpeechKind::kNotice, std::move(text), now);
        }
        notices_.clear();
    }
}

void SpeechText::restart() {
    pending_.clear();
    notices_.clear();
    mt_text_events_reset();
}

} // namespace maplethief::frontend
