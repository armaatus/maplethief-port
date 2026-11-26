// Text-to-speech on Windows (#390, ADR 0174, speech.h): SAPI 5's ISpVoice, part of every Windows
// and of the Windows SDK. Compiled only for Windows (platform/CMakeLists.txt).
//
// Speak with SPF_ASYNC returns at once and SPF_PURGEBEFORESPEAK cuts off what it was saying;
// SPF_IS_NOT_XML keeps the game's text from being read as SAPI's markup. The voice plays through
// its own audio output, not the game's mixer.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <sapi.h>

#include <SDL3/SDL_log.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

#include "speech.h"

namespace maplethief::frontend {

namespace {

std::wstring wide(const std::string &utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                      static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), n);
    return out;
}

// SAPI's rate runs from -10 to 10, 0 the voice's normal, 10 about three times as fast: a
// logarithmic scale, so 50% -> -6, 200% -> 6.
long sapi_rate(std::uint32_t rate) {
    const double r = static_cast<double>(std::clamp(rate, kSpeechRateMin, kSpeechRateMax)) / 100.0;
    return std::clamp(std::lround(10.0 * std::log(r) / std::log(3.0)), -10L, 10L);
}

class SapiSpeech final : public SpeechBackend {
  public:
    static std::unique_ptr<SapiSpeech> open() {
        // SDL may have made this thread an apartment already; either kind serves ISpVoice.
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const bool owns_com = SUCCEEDED(init);
        ISpVoice *voice = nullptr;
        const HRESULT hr = CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice,
                                            reinterpret_cast<void **>(&voice));
        if (FAILED(hr) || voice == nullptr) {
            SDL_Log("speech: SAPI's voice does not open (0x%08lx): no system voice",
                    static_cast<unsigned long>(hr));
            if (owns_com) {
                CoUninitialize();
            }
            return nullptr;
        }
        return std::unique_ptr<SapiSpeech>(new SapiSpeech(voice, owns_com));
    }

    ~SapiSpeech() override {
        stop();
        voice_->Release();
        if (owns_com_) {
            CoUninitialize();
        }
    }

    const char *name() const override { return "sapi"; }

    bool speak(const Utterance &u, const SpeechVoice &voice) override {
        const std::wstring text = wide(u.text);
        if (text.empty()) {
            return false; // not UTF-8
        }
        if (voice.language != language_) {
            language_ = voice.language;
            choose_voice(language_);
        }
        voice_->SetRate(sapi_rate(voice.rate));
        voice_->SetVolume(static_cast<USHORT>(std::min(voice.volume, kSpeechVolumeMax)));
        const auto flags = static_cast<DWORD>(SPF_ASYNC | SPF_PURGEBEFORESPEAK | SPF_IS_NOT_XML);
        const HRESULT hr = voice_->Speak(text.c_str(), flags, nullptr);
        return SUCCEEDED(hr);
    }

    void stop() override {
        voice_->Speak(nullptr, static_cast<DWORD>(SPF_PURGEBEFORESPEAK), nullptr);
    }

    std::optional<bool> speaking() const override {
        SPVOICESTATUS status{};
        if (FAILED(voice_->GetStatus(&status, nullptr))) {
            return false;
        }
        return status.dwRunningState == SPRS_IS_SPEAKING;
    }

  private:
    SapiSpeech(ISpVoice *voice, bool owns_com) : voice_(voice), owns_com_(owns_com) {}

    // The first installed voice whose Language attribute is the tag's language id (hex, as
    // SAPI writes it: "409" for en-US, "412" for ko-KR), else the user's default voice.
    void choose_voice(const std::string &tag) {
        const std::wstring wtag = wide(tag);
        const LCID lcid = wtag.empty() ? 0 : LocaleNameToLCID(wtag.c_str(), 0);
        ISpObjectToken *token = nullptr;
        if (lcid != 0) {
            wchar_t attrs[32];
            std::swprintf(attrs, 32, L"Language=%x", static_cast<unsigned>(LANGIDFROMLCID(lcid)));
            ISpObjectTokenCategory *category = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_SpObjectTokenCategory, nullptr, CLSCTX_ALL,
                                           IID_ISpObjectTokenCategory,
                                           reinterpret_cast<void **>(&category)))) {
                IEnumSpObjectTokens *tokens = nullptr;
                if (SUCCEEDED(category->SetId(SPCAT_VOICES, FALSE)) &&
                    SUCCEEDED(category->EnumTokens(attrs, nullptr, &tokens)) && tokens != nullptr) {
                    tokens->Next(1, &token, nullptr);
                    tokens->Release();
                }
                category->Release();
            }
        }
        voice_->SetVoice(token); // null: the default voice
        if (token != nullptr) {
            token->Release();
        } else {
            SDL_Log("speech: no SAPI voice for %s: the default voice", tag.c_str());
        }
    }

    ISpVoice *voice_;
    bool owns_com_;
    std::string language_;
};

} // namespace

std::unique_ptr<SpeechBackend> open_sapi_speech_backend() { return SapiSpeech::open(); }

} // namespace maplethief::frontend
