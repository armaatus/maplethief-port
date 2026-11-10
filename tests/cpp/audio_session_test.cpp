// The iOS audio session (#197, ADR 0045's addendum, audio_session.h): the category the frontend
// asks for, per setting, through the seam; the interruption's order on a recording host; the
// stream really paused and resumed on SDL's dummy driver. AVAudioSession itself is iOS only and
// not reachable here: an iPhone checks it (#197's hand-off).
#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "audio_out.h"
#include "audio_session.h"
#include "maplethief/audio.h"
#include "settings.h"

namespace fe = maplethief::frontend;
using fe::IosAudio;

namespace {

void record_category(void *user, const char *category) {
    static_cast<std::vector<std::string> *>(user)->push_back(category);
}

using Calls = std::vector<std::string>;

class RecordingHost final : public fe::AudioInterruptionHost {
  public:
    bool activate_session() override {
        calls.emplace_back("activate");
        return activates;
    }
    void pause_output() override { calls.emplace_back("pause"); }
    void resume_output() override { calls.emplace_back("resume"); }

    Calls calls;
    bool activates = true;
};

std::uint64_t frames_pulled() { return mt_audio_get_output_stats().frames; }

bool wait_for_frames_above(std::uint64_t frames) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (frames_pulled() <= frames && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return frames_pulled() > frames;
}

} // namespace

TEST_CASE("audio session: the category asked for, per setting, through the seam") {
    Calls asked;
    CHECK(std::string(fe::request_ios_audio_category(IosAudio::kOriginal, true, &record_category,
                                                     &asked)) == "ambient");
    CHECK(std::string(fe::request_ios_audio_category(IosAudio::kOwn, true, &record_category,
                                                     &asked)) == "playback");
    CHECK(asked == Calls{"ambient", "playback"});
    // A desktop asks for nothing, whatever the setting.
    asked.clear();
    CHECK(fe::request_ios_audio_category(IosAudio::kOriginal, false, &record_category, &asked) ==
          nullptr);
    CHECK(fe::request_ios_audio_category(IosAudio::kOwn, false, &record_category, &asked) ==
          nullptr);
    CHECK(asked.empty());
    // The settings' default is the original's.
    CHECK(fe::Settings{}.ios_audio == IosAudio::kOriginal);
}

TEST_CASE("audio session: the setting: original | own, iOS only, outside the presets") {
    const fe::SettingInfo *row = fe::find_setting("ios_audio");
    REQUIRE(row != nullptr);
    CHECK(row->group == fe::SettingGroup::kAudio);
    CHECK(row->ios_only);
    CHECK(row->applies == fe::Applies::kNextStart); // SDL reads the hint when the device opens
    CHECK_FALSE(row->presets.in_presets());
    CHECK(fe::find_setting_flag("--ios-audio") == row);
    CHECK(fe::setting_choices(*row) == Calls{"original", "own"});
    fe::Settings s;
    CHECK(fe::setting_text(s, *row) == "original");
    CHECK(fe::parse_settings("ios_audio = OWN\n", &s).empty());
    CHECK(s.ios_audio == IosAudio::kOwn);
    CHECK_FALSE(fe::parse_settings("ios_audio = loud\n", &s).empty());
    CHECK(s.ios_audio == IosAudio::kOwn);
    for (const fe::Preset p :
         {fe::Preset::kOriginal, fe::Preset::kOriginalController, fe::Preset::kEnhanced}) {
        fe::Settings q = s;
        REQUIRE(fe::apply_setting(&q, fe::kPresetKey, fe::preset_name(p), nullptr));
        CHECK(q.ios_audio == IosAudio::kOwn);
    }
    // Never in a desktop's menu.
    CHECK(fe::setting_shown(*row, true));
    CHECK_FALSE(fe::setting_shown(*row, false));
}

TEST_CASE("audio session: an interruption pauses, its end activates the session, then resumes") {
    RecordingHost host;
    fe::AudioInterruption i(&host);
    // An end with no interruption, as every return to the app is: nothing.
    CHECK_FALSE(i.ended("the app is active again"));
    CHECK(host.calls.empty());

    i.began();
    i.began(); // only one pause
    CHECK(i.interrupted());
    CHECK(host.calls == Calls{"pause"});

    host.calls.clear();
    CHECK(i.ended("the system says it ended"));
    CHECK_FALSE(i.interrupted());
    CHECK(host.calls == Calls{"activate", "resume"}); // in this order
    // The system's end and the app becoming active both come: the second does nothing.
    host.calls.clear();
    CHECK_FALSE(i.ended("the app is active again"));
    CHECK(host.calls.empty());
    CHECK(i.stats().began == 1);
    CHECK(i.stats().ended == 1);
}

TEST_CASE("audio session: a session that refuses to activate keeps the output paused") {
    RecordingHost host;
    fe::AudioInterruption i(&host);
    i.began();
    host.calls.clear();
    host.activates = false;
    CHECK_FALSE(i.ended("the app is active again")); // the call goes on
    CHECK(i.interrupted());
    CHECK(host.calls == Calls{"activate"}); // no resume
    CHECK(i.stats().refused == 1);

    host.calls.clear();
    host.activates = true;
    CHECK(i.ended("the system says it ended"));
    CHECK(host.calls == Calls{"activate", "resume"});
}

TEST_CASE("audio session: the device's stream stops pulling the mixer while interrupted") {
    mt_audio_reset_output_stats();
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    SDL_ResetHint(SDL_HINT_AUDIO_CATEGORY);
    {
        fe::AudioDevice device;
        REQUIRE(device.open("", IosAudio::kOwn));
        // Only a build for iOS asks for a category.
        const char *hint = SDL_GetHint(SDL_HINT_AUDIO_CATEGORY);
        if (fe::kSettingsForIos) {
            REQUIRE(hint != nullptr);
            CHECK(std::string(hint) == "playback");
        } else {
            CHECK(hint == nullptr);
        }
        device.resume();
        REQUIRE(wait_for_frames_above(0));

        device.interruption().began();
        const std::uint64_t paused_at = frames_pulled();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(frames_pulled() == paused_at);

        // The menu's device change reopens the stream: an interruption still holds it, and so
        // does the frontend's resume.
        CHECK(device.set_device(""));
        device.resume();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(frames_pulled() == paused_at);

        CHECK(device.interruption().ended("the system says it ended")); // no watch: active
        CHECK(wait_for_frames_above(paused_at));
        device.close();
        CHECK_FALSE(device.interruption().interrupted());
    }
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
    fe::set_audio_device_choices({});
}
