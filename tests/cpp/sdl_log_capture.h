// The SDL log lines written while a SdlLogCapture lives, for the platform tests that check what
// a loader said (settings, bindings, border art). The previous output function is put back.
#ifndef MAPLETHIEF_TESTS_SDL_LOG_CAPTURE_H
#define MAPLETHIEF_TESTS_SDL_LOG_CAPTURE_H

#include <SDL3/SDL.h>

#include <string>
#include <string_view>
#include <vector>

class SdlLogCapture {
  public:
    SdlLogCapture() {
        SDL_GetLogOutputFunction(&old_fn_, &old_data_);
        SDL_SetLogOutputFunction(
            [](void *self, int, SDL_LogPriority, const char *message) {
                static_cast<SdlLogCapture *>(self)->lines.emplace_back(message);
            },
            this);
    }
    ~SdlLogCapture() { SDL_SetLogOutputFunction(old_fn_, old_data_); }
    SdlLogCapture(const SdlLogCapture &) = delete;
    SdlLogCapture &operator=(const SdlLogCapture &) = delete;

    // Whether a line holds `text`.
    bool has(std::string_view text) const {
        for (const std::string &line : lines) {
            if (line.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> lines;

  private:
    SDL_LogOutputFunction old_fn_ = nullptr;
    void *old_data_ = nullptr;
};

#endif // MAPLETHIEF_TESTS_SDL_LOG_CAPTURE_H
