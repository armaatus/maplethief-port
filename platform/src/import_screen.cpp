// The first-launch import screen (#182, ADR 0100); see import_screen.h.
#include "import_screen.h"

#include <filesystem>
#include <utility>

#include "canvas.h"
#include "file_dialog.h"
#include "gamepad.h"
#include "port_text.h"
#include "present.h"

namespace maplethief::frontend {

namespace {

namespace fs = std::filesystem;

constexpr Rgba kBackground = {0x0B, 0x0E, 0x14, 255};
constexpr Rgba kFrame = {0x2A, 0x33, 0x42, 255};
constexpr Rgba kBorder = {0x5A, 0x6B, 0x85, 255};
constexpr Rgba kButton = {0x2E, 0x4A, 0x7A, 255};
constexpr Rgba kButtonDown = {0x3D, 0x62, 0xA0, 255};
constexpr Rgba kHeader = {0xF2, 0xB8, 0x4B, 255};
constexpr Rgba kText = {0xE8, 0xEC, 0xF2, 255};
constexpr Rgba kDim = {0x8C, 0x96, 0xA8, 255};
constexpr Rgba kBad = {0xF0, 0x6A, 0x5A, 255};
constexpr Rgba kGood = {0x7C, 0xD3, 0x8B, 255};

// 480 x 320 points at 2 pixels each, as the preset picker.
constexpr int kScreenScale = 2;
constexpr int kLeft = 24;
constexpr std::size_t kLineChars = 72; // (480 - 2 * 24) / 6

std::string file_name(const std::string &path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 17872861 -> "17,872,861".
std::string grouped(std::uint64_t n) {
    std::string s = std::to_string(n);
    for (int at = static_cast<int>(s.size()) - 3; at > 0; at -= 3) {
        s.insert(static_cast<std::size_t>(at), ",");
    }
    return s;
}

bool is_trackpad(SDL_TouchID id) {
    return SDL_GetTouchDeviceType(id) == SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE ||
           SDL_GetTouchDeviceType(id) == SDL_TOUCH_DEVICE_INDIRECT_RELATIVE;
}

} // namespace

std::vector<std::string> wrap_text(const std::string &text, std::size_t width) {
    std::vector<std::string> lines;
    if (width == 0) {
        return lines;
    }
    std::string line;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && text[at] == ' ') {
            at++;
        }
        std::size_t end = text.find(' ', at);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string word = text.substr(at, end - at);
        at = end;
        while (word.size() > width) { // a long path: cut it
            if (!line.empty()) {
                lines.push_back(line);
                line.clear();
            }
            lines.push_back(word.substr(0, width));
            word.erase(0, width);
        }
        if (word.empty()) {
            continue;
        }
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            lines.push_back(line);
            line.clear();
        }
        line += (line.empty() ? "" : " ") + word;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

std::string shorten_path(const std::string &path, std::size_t width) {
    if (path.size() <= width) {
        return path;
    }
    if (width <= 3) {
        return path.substr(path.size() - width);
    }
    return "..." + path.substr(path.size() - (width - 3));
}

ImportScreen::ImportScreen(std::string data_dir, Importer importer, bool folder_route)
    : data_dir_(std::move(data_dir)), importer_(std::move(importer)), folder_route_(folder_route) {}

ImportScreen::~ImportScreen() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::string ImportScreen::folder_file() const {
    const fs::path dir(std::u8string(data_dir_.begin(), data_dir_.end()));
    const std::u8string s = (dir / std::string(kIpaFileName)).u8string();
    return std::string(s.begin(), s.end());
}

bool ImportScreen::start(const std::string &path) {
    if (state_ == State::kImporting || state_ == State::kImported) {
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    file_ = path;
    state_ = State::kImporting;
    result_ = IpaResult{};
    shown_percent_ = -1;
    dirty_ = true;
    auto job = std::make_shared<Job>();
    job_ = job;
    SDL_Log("import: importing %s", path.c_str());
    worker_ = std::thread([job, path, importer = importer_] {
        const IpaProgress progress = [job](std::uint64_t done, std::uint64_t total) {
            job->total.store(total);
            job->done.store(done);
            return !job->cancel.load();
        };
        job->result = importer(path, progress);
        job->finished.store(true, std::memory_order_release);
    });
    return true;
}

bool ImportScreen::poll() {
    if (state_ != State::kImporting || !job_) {
        return false;
    }
    if (!job_->finished.load(std::memory_order_acquire)) {
        const int percent = static_cast<int>(progress() * 100.0);
        if (percent != shown_percent_) {
            dirty_ = true;
        }
        return false;
    }
    worker_.join();
    result_ = job_->result;
    job_.reset();
    state_ = result_.ok() ? State::kImported : State::kRefused;
    dirty_ = true;
    if (result_.ok()) {
        SDL_Log("import: imported into %s", result_.game_dir.c_str());
    } else {
        SDL_Log("import: refused (%s): %s", ipa_problem_name(result_.problem), result_.why.c_str());
    }
    return true;
}

void ImportScreen::wait() {
    if (worker_.joinable() && job_) {
        while (!job_->finished.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    poll();
}

void ImportScreen::cancel() {
    if (job_) {
        job_->cancel.store(true);
    }
}

double ImportScreen::progress() const {
    if (state_ == State::kImported) {
        return 1.0;
    }
    if (!job_) {
        return 0.0;
    }
    const std::uint64_t total = job_->total.load();
    return total == 0 ? 0.0 : static_cast<double>(job_->done.load()) / static_cast<double>(total);
}

bool ImportScreen::on_button(float x, float y) const {
    return x >= kButtonX && x < kButtonX + kButtonW && y >= kButtonY && y < kButtonY + kButtonH;
}

bool ImportScreen::handle(const SDL_Event &e, Action *action) {
    *action = Action::kNone;
    const bool can_choose = state_ == State::kWaiting || state_ == State::kRefused;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN: {
        const SDL_Keycode k = e.key.key;
        if (!e.key.repeat && can_choose &&
            (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE)) {
            *action = Action::kChoose;
        }
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (can_choose && (screen_button_control(e.gbutton.which,
                                                 static_cast<SDL_GamepadButton>(e.gbutton.button))
                                   .code == SDL_GAMEPAD_BUTTON_SOUTH ||
                           e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
            *action = Action::kChoose;
        }
        return true;
    case SDL_EVENT_DROP_FILE:
        if (e.drop.data != nullptr && can_choose) {
            start(e.drop.data);
        }
        return true;
    default:
        return false;
    }
}

ImportScreen::Action ImportScreen::pointer(const ScreenPoint &p, bool down, bool up) {
    const bool over = p.inside && on_button(p.x, p.y);
    if (down) {
        pressed_ = over;
        dirty_ = true;
        return Action::kNone;
    }
    if (up) {
        const bool chosen = pressed_ && over;
        pressed_ = false;
        dirty_ = true;
        if (chosen && (state_ == State::kWaiting || state_ == State::kRefused)) {
            return Action::kChoose;
        }
    }
    return Action::kNone;
}

void ImportScreen::track(const SDL_Event &e) {
    const unsigned before = glyphs_.changes();
    glyphs_.handle(e, 0);
    dirty_ = dirty_ || glyphs_.changes() != before;
}

bool ImportScreen::dirty() const {
    if (dirty_) {
        return true;
    }
    return state_ == State::kImporting && static_cast<int>(progress() * 100.0) != shown_percent_;
}

void ImportScreen::draw(std::uint8_t *rgba, std::uint32_t width, std::uint32_t height, int scale) {
    dirty_ = false;
    Canvas c(rgba, width, height, scale);
    c.fill(0, 0, c.width(), c.height(), kBackground);
    c.text(kLeft, 16, tr(Txt::import_title), kHeader, 2);
    c.text(kLeft, 40, tr(Txt::import_needs), kText);
    c.text(kLeft, 52, tr(Txt::import_file, {kIpaFileName, grouped(kIpaSize)}), kText);
    c.frame(kLeft, 66, 432, 16, 1, kFrame);
    c.text(kLeft + 4, 70, shorten_path(data_dir_, kLineChars - 2), kDim);
    if (folder_route_) {
        c.text(kLeft, 90, tr(Txt::import_folder, {kIpaFileName}), kText);
    }

    const bool can_choose = state_ == State::kWaiting || state_ == State::kRefused;
    c.fill(kButtonX, kButtonY, kButtonW, kButtonH, pressed_ ? kButtonDown : kButton);
    c.frame(kButtonX, kButtonY, kButtonW, kButtonH, 1, can_choose ? kBorder : kFrame);
    const char *label = tr(Txt::import_choose);
    c.text(kButtonX + (kButtonW - Canvas::text_width(label, 2)) / 2, kButtonY + 8, label,
           can_choose ? kText : kDim, 2);
    if (!folder_route_) {
        c.text(kButtonX + kButtonW + 12, kButtonY + 12, tr(Txt::import_or_drop), kDim);
    }

    int y = 160;
    switch (state_) {
    case State::kWaiting:
        break;
    case State::kImporting: {
        const double p = progress();
        shown_percent_ = static_cast<int>(p * 100.0);
        c.text(kLeft, y, shorten_path(tr(Txt::import_importing, {file_name(file_)}), kLineChars),
               kText);
        c.frame(kLeft, y + 14, 432, 12, 1, kBorder);
        c.fill(kLeft + 2, y + 16, static_cast<int>(428 * p), 8, kButtonDown);
        c.text(kLeft, y + 32, std::to_string(shown_percent_) + "%", kDim);
        break;
    }
    case State::kRefused: {
        c.text(kLeft, y, tr(Txt::import_refused), kBad, 2);
        y += 22;
        c.text(kLeft, y, shorten_path(file_name(file_), kLineChars), kDim);
        y += 12;
        int lines = 0;
        for (const std::string &line : wrap_text(result_.why, kLineChars)) {
            if (++lines > 6) {
                break;
            }
            c.text(kLeft, y, line, kText);
            y += 11;
        }
        c.text(kLeft, y + 4, tr(Txt::import_retry), kDim);
        break;
    }
    case State::kImported:
        c.text(kLeft, y, tr(Txt::import_done), kGood, 2);
        break;
    }

    // The hints, with the pad's own glyphs.
    const SDL_GamepadType type = glyphs_.type();
    const FaceButtons face = kFaceButtonsDefault;
    int x = kLeft;
    const int hy = 290;
    x += draw_glyph(c, x, hy, Control{ControlKind::kKey, static_cast<int>(SDLK_RETURN)}, type,
                    face) +
         2;
    x += draw_glyph(c, x, hy, Control{ControlKind::kGamepadButton, SDL_GAMEPAD_BUTTON_SOUTH}, type,
                    face) +
         2;
    x += c.text(x, hy + 3, tr(Txt::import_hint_choose), kDim) + 8;
    c.text(x, hy + 3, tr(Txt::import_hint_quit), kDim);
}

std::optional<std::string> run_import_screen(SDL_Window *window, SDL_Renderer *renderer,
                                             const Settings &settings, const std::string &data_dir,
                                             bool *quit, ImportScreen::Importer importer) {
    *quit = false;
    if (!importer) {
        importer = [data_dir](const std::string &path, const IpaProgress &progress) {
            return import_ipa(path, data_dir, ipa_expected(), progress);
        };
    }
    constexpr std::uint32_t w = kScreenWidth * kScreenScale, h = kScreenHeight * kScreenScale;
    Presenter presenter;
    if (!presenter.start(renderer, w, h)) {
        SDL_Log("import: the import screen cannot be shown");
        return std::nullopt;
    }
    presenter.set_scaling(settings.scale_mode, settings.border_color);
    const bool folder = dialog_route(dialog_place(window)) == DialogRoute::kFolder;
    ImportScreen screen(data_dir, std::move(importer), folder);
    FileDialogs dialogs;
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
    SDL_Log("import: no game bundle yet: the import screen asks for %s (%s), to go into %s",
            std::string(kIpaFileName).c_str(), folder ? "from that folder" : "a dialog or a drop",
            data_dir.c_str());
    const auto choose = [&] {
        if (screen.folder_route()) {
            screen.start(screen.folder_file());
            return;
        }
        if (dialogs.busy()) {
            return;
        }
        FileDialogRequest request;
        request.kind = FileDialogRequest::Kind::kOpenFile;
        request.title = "import the IPA";
        request.filters = {{"MapleThief IPA (ipa)", "ipa"}, {"All files", "*"}};
        std::string why;
        const bool shown = dialogs.show(
            window, request,
            [&screen](const FileDialogResult &r) {
                if (r.outcome == FileDialogResult::Outcome::kChosen) {
                    screen.start(r.path);
                } else if (r.outcome == FileDialogResult::Outcome::kFailed) {
                    SDL_Log("import: the dialog failed: %s", r.why.c_str());
                }
            },
            &why);
        if (!shown) {
            SDL_Log("import: no dialog (%s): drop the file on the window", why.c_str());
        }
    };
    std::vector<std::uint8_t> px(std::size_t{w} * h * 4);
    const auto to_screen = [&](float wx, float wy) {
        return window_point_to_screen(window, presenter.placement(), wx, wy);
    };
    while (screen.state() != ImportScreen::State::kImported && !*quit) {
        dialogs.poll();
        screen.poll();
        if (screen.dirty()) {
            screen.draw(px.data(), w, h, kScreenScale);
            presenter.upload(px.data(), w * 4);
        }
        presenter.draw();
        SDL_RenderPresent(renderer);
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e,
                                  screen.state() == ImportScreen::State::kImporting ? 30 : 100)) {
            continue;
        }
        do {
            ImportScreen::Action action = ImportScreen::Action::kNone;
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_TERMINATING:
                *quit = true;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                open_pad(e.gdevice.which);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT && e.button.which != SDL_TOUCH_MOUSEID &&
                    e.button.which != SDL_PEN_MOUSEID) {
                    action = screen.pointer(to_screen(e.button.x, e.button.y),
                                            e.type == SDL_EVENT_MOUSE_BUTTON_DOWN,
                                            e.type == SDL_EVENT_MOUSE_BUTTON_UP);
                }
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
                if (e.tfinger.touchID != SDL_MOUSE_TOUCHID && !is_trackpad(e.tfinger.touchID)) {
                    int ww = 0, wh = 0;
                    SDL_GetWindowSize(window, &ww, &wh);
                    action = screen.pointer(to_screen(e.tfinger.x * static_cast<float>(ww),
                                                      e.tfinger.y * static_cast<float>(wh)),
                                            e.type == SDL_EVENT_FINGER_DOWN,
                                            e.type == SDL_EVENT_FINGER_UP);
                }
                break;
            case SDL_EVENT_WINDOW_EXPOSED:
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                break; // drawn again at the top of the loop
            default:
                screen.handle(e, &action);
                break;
            }
            screen.track(e);
            if (action == ImportScreen::Action::kChoose) {
                choose();
            }
        } while (!*quit && SDL_PollEvent(&e));
    }
    if (*quit) {
        screen.cancel(); // an import that runs stops and leaves nothing behind
    }
    screen.wait();
    for (SDL_Gamepad *pad : pads) {
        SDL_CloseGamepad(pad);
    }
    presenter.stop();
    if (*quit) {
        SDL_Log("import: the window closed in the import screen");
        return std::nullopt;
    }
    return screen.result().game_dir;
}

} // namespace maplethief::frontend
