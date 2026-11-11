// The missing-bundle screen (#238); see missing_bundle.h.
#include "missing_bundle.h"

#include <utility>
#include <vector>

#include "canvas.h"
#include "import_screen.h"
#include "port_text.h"
#include "present.h"
#include "scaling.h"

namespace maplethief::frontend {

namespace {

// The import screen's colours (import_screen.cpp).
constexpr Rgba kBackground = {0x0B, 0x0E, 0x14, 255};
constexpr Rgba kFrame = {0x2A, 0x33, 0x42, 255};
constexpr Rgba kHeader = {0xF2, 0xB8, 0x4B, 255};
constexpr Rgba kText = {0xE8, 0xEC, 0xF2, 255};
constexpr Rgba kDim = {0x8C, 0x96, 0xA8, 255};

constexpr int kScreenScale = 2;
constexpr int kLeft = 24;
constexpr std::size_t kLineChars = 72; // (480 - 2 * 24) / 6

} // namespace

MissingBundleScreen::MissingBundleScreen(std::string expected) : expected_(std::move(expected)) {}

bool MissingBundleScreen::ends(const SDL_Event &e) {
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_FINGER_UP:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return e.button.button == SDL_BUTTON_LEFT;
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = e.key.key;
        return !e.key.repeat &&
               (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE || k == SDLK_ESCAPE);
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        return e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
               e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST ||
               e.gbutton.button == SDL_GAMEPAD_BUTTON_START ||
               e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK;
    default:
        return false;
    }
}

void MissingBundleScreen::track(const SDL_Event &e) {
    const unsigned before = glyphs_.changes();
    glyphs_.handle(e, 0);
    dirty_ = dirty_ || glyphs_.changes() != before;
}

void MissingBundleScreen::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height,
                               int scale) {
    dirty_ = false;
    Canvas c(rgba, width, height, scale);
    c.fill(0, 0, c.width(), c.height(), kBackground);
    c.text(kLeft, 16, tr(Txt::missing_title), kHeader, 2);
    int y = 42;
    for (const std::string &line : wrap_text(tr(Txt::missing_needs), kLineChars)) {
        c.text(kLeft, y, line, kText);
        y += 11;
    }
    y += 6;
    c.frame(kLeft, y, 432, 16, 1, kFrame);
    c.text(kLeft + 4, y + 4, shorten_path(expected_, kLineChars - 2), kDim);
    y += 26;
    for (const std::string &line : wrap_text(tr(Txt::missing_how), kLineChars)) {
        c.text(kLeft, y, line, kText);
        y += 11;
    }
    y += 6;
    for (const std::string &line : wrap_text(tr(Txt::missing_later), kLineChars)) {
        c.text(kLeft, y, line, kDim);
        y += 11;
    }

    // The hint, with the pad's own glyphs.
    const SDL_GamepadType type = glyphs_.type();
    const FaceButtons face = FaceButtons::kPosition;
    int x = kLeft;
    const int hy = 290;
    x += draw_glyph(c, x, hy, Control{ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_START}, type,
                    face) +
         2;
    x += draw_glyph(c, x, hy, Control{ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_EAST}, type,
                    face) +
         4;
    c.text(x, hy + 3, tr(Txt::missing_hint), kDim);
}

bool run_missing_bundle_screen(SDL_Renderer *renderer, const Settings &settings,
                               const std::string &expected) {
    constexpr std::uint32_t w = kScreenWidth * kScreenScale, h = kScreenHeight * kScreenScale;
    Presenter presenter;
    if (!presenter.start(renderer, w, h)) {
        SDL_Log("game: the missing-bundle screen cannot be shown");
        return false;
    }
    presenter.set_scaling(settings.scale_mode, settings.border_color);
    SDL_Log("game: no bundle at %s: the screen asks for the extracted MapleThief.app there "
            "(the IPA import on this platform comes with #243)",
            expected.c_str());
    MissingBundleScreen screen(expected);
    std::vector<SDL_Gamepad *> pads;
    const auto open_pad = [&pads](SDL_JoystickID id) {
        if (SDL_Gamepad *pad = SDL_OpenGamepad(id); pad != nullptr) {
            pads.push_back(pad);
        }
    };
    int count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count); ids != nullptr) {
        for (int i = 0; i < count; i++) {
            open_pad(ids[i]);
        }
        SDL_free(ids);
    }
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    bool done = false;
    while (!done) {
        if (screen.dirty()) {
            screen.draw(px.data(), w, h, kScreenScale);
            presenter.upload(px.data(), w * 4);
        }
        presenter.draw();
        SDL_RenderPresent(renderer);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 100)) {
            continue;
        }
        do {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                open_pad(e.gdevice.which);
            }
            screen.track(e);
            done = MissingBundleScreen::ends(e);
        } while (!done && SDL_PollEvent(&e));
    }
    for (SDL_Gamepad *pad : pads) {
        SDL_CloseGamepad(pad);
    }
    presenter.stop();
    SDL_Log("game: the missing-bundle screen closed");
    return true;
}

} // namespace maplethief::frontend
