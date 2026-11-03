// The desktop conveniences (#145, ADR 0077); see desktop.h.
#include "desktop.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "frame_stats.h"
#include "game.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// The modifiers that never change what a key is.
constexpr SDL_Keymod kNeutralMods = SDL_KMOD_SHIFT | SDL_KMOD_NUM | SDL_KMOD_CAPS;

// A stick or trigger pushed past half counts as using the pad.
constexpr int kAxisActive = 16384;

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// A decimal integer with an optional '-', within +-1,000,000.
bool parse_int(std::string_view s, int *out) {
    int v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size() || v < -1000000 || v > 1000000) {
        return false;
    }
    *out = v;
    return true;
}

} // namespace

bool is_interactive_run(const GameOptions &options, const char *video_driver) {
    // game.h's one list of scripted options (the settings menu uses it too, #89).
    if (scripted_run(options)) {
        return false;
    }
    return video_driver == nullptr ||
           (std::strcmp(video_driver, "dummy") != 0 && std::strcmp(video_driver, "offscreen") != 0);
}

// --- Keys and buttons -------------------------------------------------------------------------

bool is_fullscreen_hotkey(SDL_Keycode key, SDL_Keymod mod, bool macos) {
    const SDL_Keymod m = mod & ~kNeutralMods;
    if (key == SDLK_F11) {
        return m == 0;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        // Alt (either) and nothing else.
        return m != 0 && (m & ~SDL_KMOD_ALT) == 0;
    }
    if (key == SDLK_F && !macos) {
        // Cmd+Ctrl+F (GUI+Ctrl+F). On macOS it is the Window menu's (SDL's "Toggle Full Screen"):
        // the port toggling as well toggled twice (#179).
        return (m & SDL_KMOD_GUI) != 0 && (m & SDL_KMOD_CTRL) != 0 &&
               (m & ~(SDL_KMOD_GUI | SDL_KMOD_CTRL)) == 0;
    }
    return false;
}

bool is_screenshot_key(SDL_Keycode key, SDL_Keymod mod) {
    return key == SDLK_F12 && (mod & ~kNeutralMods) == 0;
}

bool is_screenshot_button(int button, bool right_stick_held) {
    return button == SDL_GAMEPAD_BUTTON_MISC1 ||
           (button == SDL_GAMEPAD_BUTTON_BACK && right_stick_held);
}

// --- The cursor -------------------------------------------------------------------------------

CursorAutoHide::Change CursorAutoHide::on_event(const SDL_Event &e) {
    bool hide = false;
    bool show = false;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        hide = !e.key.repeat;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        hide = true;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        hide = std::abs(static_cast<int>(e.gaxis.value)) >= kAxisActive;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        show = e.motion.which != SDL_TOUCH_MOUSEID && e.motion.which != SDL_PEN_MOUSEID;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        show = e.button.which != SDL_TOUCH_MOUSEID && e.button.which != SDL_PEN_MOUSEID;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        show = e.wheel.which != SDL_TOUCH_MOUSEID && e.wheel.which != SDL_PEN_MOUSEID;
        break;
    default:
        break;
    }
    if (hide && !hidden_) {
        hidden_ = true;
        return Change::kHide;
    }
    if (show && hidden_) {
        hidden_ = false;
        return Change::kShow;
    }
    return Change::kNone;
}

// --- The mouse wheel --------------------------------------------------------------------------

int WheelSteps::add(float y) {
    if (!std::isfinite(y) || y == 0) {
        return 0;
    }
    if ((y > 0) != (rest_ > 0) && rest_ != 0) {
        rest_ = 0; // the other way: start again
    }
    rest_ += y;
    const float whole = std::trunc(rest_);
    rest_ -= whole;
    return static_cast<int>(std::clamp(whole, -64.0f, 64.0f));
}

// --- window.ini -------------------------------------------------------------------------------

std::optional<WindowState> parse_window_state(std::string_view text, int min_side, int max_side,
                                              std::vector<std::string> *problems) {
    auto problem = [&](const std::string &what) {
        if (problems != nullptr) {
            problems->push_back(what);
        }
    };
    if (text.size() > 4096) {
        problem("the file has more than 4096 bytes: ignored");
        return std::nullopt;
    }
    WindowState s;
    bool have_x = false, have_y = false, have_w = false, have_h = false;
    std::size_t number = 0;
    while (!text.empty()) {
        const std::size_t nl = text.find('\n');
        const std::string_view line = trim(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        number++;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::string at = "line " + std::to_string(number) + ": ";
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            problem(at + "not key = value");
            continue;
        }
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        int v = 0;
        if (key == "maximized") {
            if (value == "on" || value == "off") {
                s.maximized = value == "on";
            } else {
                problem(at + "maximized is not on or off");
            }
            continue;
        }
        if (key != "x" && key != "y" && key != "width" && key != "height") {
            problem(at + "unknown key");
            continue;
        }
        if (!parse_int(value, &v)) {
            problem(at + std::string(key) + " is not an integer");
            continue;
        }
        if (key == "x") {
            s.x = v;
            have_x = true;
        } else if (key == "y") {
            s.y = v;
            have_y = true;
        } else if (key == "width") {
            s.width = v;
            have_w = true;
        } else {
            s.height = v;
            have_h = true;
        }
    }
    if (!have_x || !have_y || !have_w || !have_h) {
        problem("x, y, width and height are not all there: ignored");
        return std::nullopt;
    }
    if (s.width < min_side || s.width > max_side || s.height < min_side || s.height > max_side) {
        problem("the size " + std::to_string(s.width) + "x" + std::to_string(s.height) +
                " is not from " + std::to_string(min_side) + " to " + std::to_string(max_side) +
                ": ignored");
        return std::nullopt;
    }
    return s;
}

std::string serialise_window_state(const WindowState &s) {
    return "# MapleThief's window (window.ini), written when the game ends: its position and "
           "size in\n# points when not fullscreen. Delete the file to start centred again.\n"
           "x = " +
           std::to_string(s.x) + "\ny = " + std::to_string(s.y) +
           "\nwidth = " + std::to_string(s.width) + "\nheight = " + std::to_string(s.height) +
           "\nmaximized = " + (s.maximized ? "on" : "off") + "\n";
}

bool window_state_on_display(const WindowState &s, std::span<const SDL_Rect> displays) {
    // The middle of the window's top edge, a little inside it.
    const long long px = static_cast<long long>(s.x) + s.width / 2;
    const long long py = static_cast<long long>(s.y) + std::min(16, s.height / 2);
    for (const SDL_Rect &d : displays) {
        if (px >= d.x && px < static_cast<long long>(d.x) + d.w && py >= d.y &&
            py < static_cast<long long>(d.y) + d.h) {
            return true;
        }
    }
    return false;
}

std::optional<WindowState> load_window_state(const std::string &dir, int min_side, int max_side) {
    if (dir.empty()) {
        return std::nullopt;
    }
    const fs::path file = utf8_path(dir) / kWindowStateFileName;
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        return std::nullopt;
    }
    const std::string path = path_utf8(file);
    if (fs::file_size(file, ec) > 4096 || ec) {
        SDL_Log("desktop: %s is too large or unreadable: ignored", path.c_str());
        return std::nullopt;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<std::string> problems;
    const std::optional<WindowState> s = parse_window_state(text, min_side, max_side, &problems);
    for (const std::string &p : problems) {
        SDL_Log("desktop: %s: %s", path.c_str(), p.c_str());
    }
    return s;
}

bool save_window_state(const std::string &dir, const WindowState &state) {
    if (dir.empty()) {
        return false;
    }
    const fs::path file = utf8_path(dir) / kWindowStateFileName;
    fs::path tmp = file;
    tmp += ".tmp";
    const std::string text = serialise_window_state(state);
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            SDL_Log("desktop: cannot write %s", path_utf8(tmp).c_str());
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        SDL_Log("desktop: cannot replace %s: %s", path_utf8(file).c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// --- Screenshots ------------------------------------------------------------------------------

std::string screenshot_file_name(const SDL_DateTime &t) {
    char name[64];
    std::snprintf(name, sizeof name, "MapleThief_%04d-%02d-%02d_%02d-%02d-%02d-%03d.png", t.year,
                  t.month, t.day, t.hour, t.minute, t.second, t.nanosecond / 1000000);
    return name;
}

std::string write_screenshot(const std::string &data_dir, const std::uint8_t *rgba,
                             std::uint32_t width, std::uint32_t height) {
    if (data_dir.empty() || rgba == nullptr || width == 0 || height == 0) {
        SDL_Log("desktop: screenshot: no data directory or no screen yet");
        return "";
    }
    const fs::path dir = utf8_path(data_dir) / kScreenshotDirName;
    std::error_code ec;
    fs::create_directories(dir, ec);
    SDL_Time now = 0;
    SDL_DateTime t{};
    if (!SDL_GetCurrentTime(&now) || !SDL_TimeToDateTime(now, &t, true)) {
        t = SDL_DateTime{};
        t.year = 1970;
        t.month = 1;
        t.day = 1;
    }
    std::string name = screenshot_file_name(t);
    fs::path file = dir / name;
    for (int n = 2; fs::exists(file, ec) && n < 1000; n++) {
        file = dir / (name.substr(0, name.size() - 4) + "_" + std::to_string(n) + ".png");
    }
    const std::string path = path_utf8(file);
    SDL_Surface *surface = SDL_CreateSurfaceFrom(
        static_cast<int>(width), static_cast<int>(height), SDL_PIXELFORMAT_ABGR8888,
        const_cast<std::uint8_t *>(rgba), static_cast<int>(width * 4));
    const bool ok = surface != nullptr && SDL_SavePNG(surface, path.c_str());
    SDL_DestroySurface(surface);
    if (!ok) {
        SDL_Log("desktop: screenshot: cannot write %s: %s", path.c_str(), SDL_GetError());
        return "";
    }
    return path;
}

// --- The window -------------------------------------------------------------------------------

void Desktop::attach(SDL_Window *window, SDL_Renderer *renderer,
                     std::optional<WindowState> windowed) {
    window_ = window;
    renderer_ = renderer;
    want_vsync_.reset();
    if (window_ == nullptr) {
        return;
    }
    want_fullscreen_ = fullscreen();
    windowed_ = windowed;
    track();
}

bool Desktop::fullscreen() const {
    return window_ != nullptr && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
}

void Desktop::apply(const Settings &settings) {
    if (window_ == nullptr) {
        return;
    }
#if !defined(SDL_PLATFORM_IOS)
    // Only a change of the request asks the window (#179): while macOS's transition into the
    // Space runs, the window is not fullscreen yet, and a second request (run_game's apply after
    // main.cpp's) pumped AppKit's queued transition inside it and crashed in SkyLight. The
    // system's own changes (the green button) come back through handle() as the request.
    const bool want = settings.fullscreen;
    if (want != want_fullscreen_) {
        track(); // the windowed size and position, before it goes
        want_fullscreen_ = want;
        fullscreen_requests_++;
        if (want) {
            SDL_SetWindowFullscreenMode(window_, nullptr); // desktop fullscreen: no mode switch
        }
        if (SDL_SetWindowFullscreen(window_, want)) {
            SDL_Log("desktop: fullscreen %s", want ? "on (the whole display)" : "off");
        } else {
            SDL_Log("desktop: fullscreen %s: %s", want ? "on" : "off", SDL_GetError());
        }
    }
#endif
    if (renderer_ != nullptr && want_vsync_ != settings.vsync) {
        const bool first = !want_vsync_.has_value();
        want_vsync_ = settings.vsync;
        const bool ok = SDL_SetRenderVSync(renderer_, settings.vsync ? 1 : 0);
        vsync_ = settings.vsync && ok;
        if (!first || !ok) {
            SDL_Log("desktop: vsync %s%s%s", settings.vsync ? "on" : "off",
                    ok ? "" : ": the renderer refused: ", ok ? "" : SDL_GetError());
        }
    }
}

bool follow_vsync(const Desktop &desktop, bool *vsync, FrameStats *stats) {
    if (desktop.vsync() == *vsync) {
        return false;
    }
    *vsync = desktop.vsync();
    stats->set_vsync(*vsync);
    return true;
}

void Desktop::track() {
    if (window_ == nullptr) {
        return;
    }
    const SDL_WindowFlags flags = SDL_GetWindowFlags(window_);
    if ((flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED)) != 0 ||
        want_fullscreen_) {
        return;
    }
    WindowState s;
    if (!SDL_GetWindowPosition(window_, &s.x, &s.y) ||
        !SDL_GetWindowSize(window_, &s.width, &s.height) || s.width <= 0 || s.height <= 0) {
        return;
    }
    // A size that is the whole display's is fullscreen on its way (macOS's green button can
    // resize before SDL sees the window enter fullscreen): not the windowed size.
    SDL_Rect display{};
    const SDL_DisplayID id = SDL_GetDisplayForWindow(window_);
    if (id != 0 && SDL_GetDisplayBounds(id, &display) && s.width == display.w &&
        s.height == display.h) {
        return;
    }
    windowed_ = s;
}

std::optional<WindowState> Desktop::state() const {
    if (window_ == nullptr || !windowed_) {
        return std::nullopt;
    }
    WindowState s = *windowed_;
    s.maximized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
    return s;
}

void Desktop::set_cursor(CursorAutoHide::Change change) {
    if (change == CursorAutoHide::Change::kHide) {
        SDL_HideCursor();
        cursor_hides_++;
    } else if (change == CursorAutoHide::Change::kShow) {
        SDL_ShowCursor();
    }
}

Desktop::Request Desktop::handle(const SDL_Event &e) {
    Request r;
    if (window_ == nullptr) {
        return r;
    }
    set_cursor(cursor_.on_event(e));
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = e.key.key;
        if (std::find(held_keys_.begin(), held_keys_.end(), k) != held_keys_.end()) {
            r.consumed = true; // a repeat of the port's key
            break;
        }
        const bool full = is_fullscreen_hotkey(k, e.key.mod);
        const bool shot = !full && is_screenshot_key(k, e.key.mod);
        if (full || shot) {
            held_keys_.push_back(k);
            r.consumed = true;
            r.toggle_fullscreen = full && !e.key.repeat;
            r.screenshot = shot && !e.key.repeat;
            if (r.toggle_fullscreen) {
                toggles_++;
            }
        }
        break;
    }
    case SDL_EVENT_KEY_UP: {
        const auto it = std::find(held_keys_.begin(), held_keys_.end(), e.key.key);
        if (it != held_keys_.end()) {
            held_keys_.erase(it);
            r.consumed = true;
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(e.gbutton.which);
        const bool r3 = pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        if (is_screenshot_button(e.gbutton.button, r3)) {
            held_buttons_.emplace_back(e.gbutton.which, e.gbutton.button);
            r.consumed = true;
            r.screenshot = true;
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const auto it =
            std::find(held_buttons_.begin(), held_buttons_.end(),
                      std::pair<SDL_JoystickID, int>(e.gbutton.which, e.gbutton.button));
        if (it != held_buttons_.end()) {
            held_buttons_.erase(it);
            r.consumed = true;
        }
        break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED:
        std::erase_if(held_buttons_, [&](const auto &h) { return h.first == e.gdevice.which; });
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        held_keys_.clear(); // their releases go elsewhere
        break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
        want_fullscreen_ = e.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN;
        r.fullscreen_changed = want_fullscreen_;
        track();
        break;
    case SDL_EVENT_WINDOW_MOVED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        track();
        break;
    default:
        break;
    }
    return r;
}

} // namespace maplethief::frontend
