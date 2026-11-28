// Text-to-speech on macOS and iOS (#390, ADR 0174, speech.h): AVSpeechSynthesizer. Compiled
// only for Apple targets (platform/CMakeLists.txt), with ARC.
//
// On iOS the synthesizer speaks in the app's own audio session (usesApplicationAudioSession,
// the default, set here to say so): the voice mixes with the game's sound in the category SDL
// set (audio_session.h, ADR 0045's addendum) rather than taking a session of its own, which
// would interrupt the game's.
#import <AVFoundation/AVFoundation.h>

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>

#include "speech.h"

#if !__has_feature(objc_arc)
#error "speech_apple.mm needs ARC (-fobjc-arc)"
#endif

namespace maplethief::frontend {

namespace {

// The rate: AVSpeechUtteranceDefaultSpeechRate at 100%, scaled, within the API's range.
float av_rate(std::uint32_t rate) {
    const float r = AVSpeechUtteranceDefaultSpeechRate *
                    static_cast<float>(std::clamp(rate, kSpeechRateMin, kSpeechRateMax)) / 100.0f;
    return std::clamp(r, AVSpeechUtteranceMinimumSpeechRate, AVSpeechUtteranceMaximumSpeechRate);
}

class AppleSpeech final : public SpeechBackend {
  public:
    AppleSpeech() : synth_([AVSpeechSynthesizer new]) {
#if TARGET_OS_IPHONE
        synth_.usesApplicationAudioSession = YES;
#endif
    }

    const char *name() const override { return "avspeech"; }

    bool speak(const Utterance &u, const SpeechVoice &voice) override {
        @autoreleasepool {
            NSString *text = [[NSString alloc] initWithBytes:u.text.data()
                                                      length:u.text.size()
                                                    encoding:NSUTF8StringEncoding];
            if (text == nil) {
                return false; // not UTF-8
            }
            if (voice.language != language_) {
                language_ = voice.language;
                NSString *tag = [NSString stringWithUTF8String:language_.c_str()];
                voice_ = tag != nil ? [AVSpeechSynthesisVoice voiceWithLanguage:tag] : nil;
                if (voice_ == nil) {
                    SDL_Log("speech: no system voice for %s: the default voice", language_.c_str());
                }
            }
            AVSpeechUtterance *utt = [AVSpeechUtterance speechUtteranceWithString:text];
            utt.voice = voice_; // nil: the system's default voice
            utt.rate = av_rate(voice.rate);
            utt.volume = static_cast<float>(std::min(voice.volume, kSpeechVolumeMax)) / 100.0f;
            if (synth_.speaking) {
                [synth_ stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
            }
            [synth_ speakUtterance:utt];
            started_ns_ = SDL_GetTicksNS();
            seen_ = false;
            return true;
        }
    }

    void stop() override {
        [synth_ stopSpeakingAtBoundary:AVSpeechBoundaryImmediate];
        started_ns_ = 0;
    }

    // The synthesizer starts on a thread of its own, so `speaking` can still be NO just after
    // speakUtterance: until it has been seen YES, the utterance counts as speaking for up to
    // kStartGraceNs (then it never started, or was done before a turn looked).
    std::optional<bool> speaking() const override {
        if (synth_.speaking == YES) {
            seen_ = true;
            return true;
        }
        return !seen_ && started_ns_ != 0 && SDL_GetTicksNS() - started_ns_ < kStartGraceNs;
    }

  private:
    AVSpeechSynthesizer *synth_;
    AVSpeechSynthesisVoice *voice_ = nil;
    std::string language_;
    static constexpr Uint64 kStartGraceNs = 1'000'000'000;
    Uint64 started_ns_ = 0;
    mutable bool seen_ = false;
};

} // namespace

std::unique_ptr<SpeechBackend> open_apple_speech_backend() {
    return std::make_unique<AppleSpeech>();
}

} // namespace maplethief::frontend
