// The bindings file (#70, ADR 0054); see bindings.h.
#include "bindings.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

#include "maplethief/input.h"
#include "settings.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

std::string_view trim(std::string_view s) {
    const auto blank = [](char c) { return c == ' ' || c == '\t'; };
    while (!s.empty() && blank(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && blank(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

const char *action_name(std::uint32_t action) {
    const mt_input_action_info *info = mt_input_action_get_info(action);
    return info != nullptr ? info->name : "?";
}

const char *in_context(Context c) { return c == Context::kField ? "in the field" : "in menus"; }

// A path from a UTF-8 string, and back (on Windows a narrow string is not UTF-8).
fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::uint32_t &slot_of(Binding &b, Context c) { return c == Context::kField ? b.field : b.menu; }
std::uint32_t slot_of(const Binding &b, Context c) {
    return c == Context::kField ? b.field : b.menu;
}

void drop_empty_rows(std::vector<Binding> *table) {
    table->erase(std::remove_if(table->begin(), table->end(),
                                [](const Binding &b) {
                                    return b.field == MT_ACTION_NONE && b.menu == MT_ACTION_NONE;
                                }),
                 table->end());
}

} // namespace

bool reserved_control(const Control &c) {
    return (c.kind == ControlKind::kKey && c.code == static_cast<int>(SDLK_F1)) ||
           (c.kind == ControlKind::kGamepadButton && c.code == SDL_GAMEPAD_BUTTON_GUIDE);
}

bool parse_control(std::string_view text, Control *out, std::string *why) {
    const auto fail = [&](std::string message) {
        if (why != nullptr) {
            *why = std::move(message);
        }
        return false;
    };
    text = trim(text);
    const std::size_t space = text.find_first_of(" \t");
    const std::string kind = lower(text.substr(0, space));
    const std::string_view name =
        space == std::string_view::npos ? std::string_view() : trim(text.substr(space));
    const std::string lname = lower(name);
    if (name.empty() &&
        (kind == "key" || kind == "button" || kind == "trigger" || kind == "direction")) {
        return fail("'" + printable(text) + "' needs a name after '" + kind + "'");
    }
    if (kind == "key") {
        SDL_Keycode k = SDLK_UNKNOWN;
        if (lname == "comma") {
            k = SDLK_COMMA;
        } else {
            k = SDL_GetKeyFromName(std::string(name).c_str());
        }
        // A letter's name is its capital ("Z"); its keycode is the small letter.
        if (k >= 'A' && k <= 'Z') {
            k = k - 'A' + 'a';
        }
        if (k == SDLK_UNKNOWN) {
            return fail("unknown key '" + printable(name) + "'");
        }
        *out = Control{ControlKind::kKey, static_cast<int>(k)};
        return true;
    }
    if (kind == "button") {
        SDL_GamepadButton b = SDL_GAMEPAD_BUTTON_INVALID;
        if (lname == "south") {
            b = SDL_GAMEPAD_BUTTON_SOUTH;
        } else if (lname == "east") {
            b = SDL_GAMEPAD_BUTTON_EAST;
        } else if (lname == "west") {
            b = SDL_GAMEPAD_BUTTON_WEST;
        } else if (lname == "north") {
            b = SDL_GAMEPAD_BUTTON_NORTH;
        } else {
            b = SDL_GetGamepadButtonFromString(lname.c_str());
        }
        if (b == SDL_GAMEPAD_BUTTON_INVALID) {
            return fail("unknown gamepad button '" + printable(name) + "'");
        }
        if (b == SDL_GAMEPAD_BUTTON_DPAD_UP || b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ||
            b == SDL_GAMEPAD_BUTTON_DPAD_LEFT || b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) {
            return fail("the d-pad is 'direction up' (down, left, right), not '" + printable(text) +
                        "'");
        }
        *out = Control{ControlKind::kGamepadButton, static_cast<int>(b)};
        return true;
    }
    if (kind == "trigger") {
        SDL_GamepadAxis a = SDL_GAMEPAD_AXIS_INVALID;
        if (lname == "left") {
            a = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        } else if (lname == "right") {
            a = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        } else {
            a = SDL_GetGamepadAxisFromString(lname.c_str());
        }
        if (a != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && a != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            return fail("unknown trigger '" + printable(name) + "' (lefttrigger or righttrigger)");
        }
        *out = Control{ControlKind::kGamepadTrigger, static_cast<int>(a)};
        return true;
    }
    if (kind == "direction") {
        for (int d = 0; d < 4; d++) {
            if (lname == direction_name(static_cast<Direction>(d))) {
                *out = Control{ControlKind::kDirection, d};
                return true;
            }
        }
        return fail("unknown direction '" + printable(name) + "' (up, down, left or right)");
    }
    return fail("'" + printable(text) +
                "' is not a control (key NAME, button NAME, trigger NAME or direction NAME)");
}

std::string control_text(const Control &c) {
    if (c.kind == ControlKind::kKey) {
        if (c.code == static_cast<int>(SDLK_COMMA)) {
            return "key Comma";
        }
        const char *n = SDL_GetKeyName(static_cast<SDL_Keycode>(c.code));
        return std::string("key ") + (n != nullptr ? n : "");
    }
    if (c.kind == ControlKind::kGamepadButton) {
        const char *n = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(c.code));
        return std::string("button ") + (n != nullptr ? n : "?");
    }
    if (c.kind == ControlKind::kGamepadTrigger) {
        const char *n = SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(c.code));
        return std::string("trigger ") + (n != nullptr ? n : "?");
    }
    return std::string("direction ") + direction_name(static_cast<Direction>(c.code));
}

Device device_of(const Control &c) {
    return c.kind == ControlKind::kKey ? Device::kKeyboard : Device::kGamepad;
}

std::vector<Control> controls_for(const std::vector<Binding> &table, Context context,
                                  std::uint32_t action) {
    std::vector<Control> out;
    for (const Binding &b : table) {
        if (slot_of(b, context) == action &&
            std::find(out.begin(), out.end(), b.control) == out.end()) {
            out.push_back(b.control);
        }
    }
    return out;
}

void set_controls(std::vector<Binding> *table, Context context, std::uint32_t action,
                  const std::vector<Control> &controls) {
    for (Binding &b : *table) {
        if (slot_of(b, context) == action &&
            std::find(controls.begin(), controls.end(), b.control) == controls.end()) {
            slot_of(b, context) = MT_ACTION_NONE;
        }
    }
    for (const Control &c : controls) {
        // Actions::action_for takes the first row of a control: give it to that row, and clear
        // the context in any later row of the same control.
        bool first = true;
        for (Binding &b : *table) {
            if (b.control == c) {
                slot_of(b, context) = first ? action : static_cast<std::uint32_t>(MT_ACTION_NONE);
                first = false;
            }
        }
        if (first) {
            Binding row;
            row.control = c;
            slot_of(row, context) = action;
            table->push_back(row);
        }
    }
    drop_empty_rows(table);
}

std::uint32_t bind_control(std::vector<Binding> *table, Context context, std::uint32_t action,
                           const Control &control) {
    std::uint32_t before = MT_ACTION_NONE;
    for (const Binding &b : *table) {
        if (b.control == control) {
            before = slot_of(b, context);
            break;
        }
    }
    std::vector<Control> keep;
    for (const Control &c : controls_for(*table, context, action)) {
        if (device_of(c) != device_of(control)) {
            keep.push_back(c);
        }
    }
    keep.push_back(control);
    set_controls(table, context, action, keep);
    return before == action ? static_cast<std::uint32_t>(MT_ACTION_NONE) : before;
}

std::vector<ActionSlot> action_slots(const std::vector<Binding> &table) {
    static const std::vector<Binding> defaults = default_bindings();
    std::vector<ActionSlot> out;
    for (const Context context : {Context::kField, Context::kMenu}) {
        for (std::uint32_t a = MT_ACTION_NONE + 1; a < MT_ACTION_COUNT; a++) {
            if (!controls_for(defaults, context, a).empty() ||
                !controls_for(table, context, a).empty()) {
                out.push_back(ActionSlot{context, a});
            }
        }
    }
    return out;
}

std::vector<std::string> unbound_warnings(const std::vector<Binding> &table) {
    static const std::vector<Binding> defaults = default_bindings();
    std::vector<std::string> out;
    for (const ActionSlot &s : action_slots(table)) {
        const std::vector<Control> had = controls_for(defaults, s.context, s.action);
        const std::vector<Control> has = controls_for(table, s.context, s.action);
        // The port's own actions (fast_forward) are optional: no warning without a control.
        if (had.empty() || port_action(s.action)) {
            continue;
        }
        const std::string what = std::string(action_name(s.action));
        if (has.empty()) {
            out.push_back(what + " has no control " + in_context(s.context) +
                          ": it cannot be used there");
            continue;
        }
        for (const Device d : {Device::kKeyboard, Device::kGamepad}) {
            const auto of = [d](const Control &c) { return device_of(c) == d; };
            if (std::any_of(had.begin(), had.end(), of) &&
                std::none_of(has.begin(), has.end(), of)) {
                out.push_back(what + " has no " +
                              (d == Device::kKeyboard ? "key " : "gamepad control ") +
                              in_context(s.context));
            }
        }
    }
    return out;
}

std::vector<std::string> parse_bindings(std::string_view text, const std::string &file,
                                        BindingSet *set) {
    std::vector<std::string> problems;
    const auto report = [&](std::size_t line, const std::string &what) {
        problems.push_back(file + ":" + std::to_string(line) + ": " + what);
    };
    if (text.size() > kMaxBindingsBytes) {
        problems.push_back(file + ": larger than " + std::to_string(kMaxBindingsBytes) +
                           " bytes: ignored, the defaults stay");
        return problems;
    }
    if (text.substr(0, 3) == "\xEF\xBB\xBF") {
        text.remove_prefix(3);
    }
    enum class Section { kNone, kField, kMenu, kUnknown };
    Section section = Section::kNone;
    // Per context: the line that set each action, and the action and line that took each control.
    std::map<std::pair<int, std::uint32_t>, std::size_t> action_line;
    struct Claim {
        Control control;
        std::uint32_t action;
        std::size_t line;
    };
    std::vector<Claim> claims[2];
    std::size_t line_no = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t nl = text.find('\n', start);
        std::string_view line = text.substr(start, nl == std::string_view::npos ? nl : nl - start);
        start = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
        line_no++;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        line = trim(line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                report(line_no, "malformed section '" + printable(line) + "'");
                section = Section::kUnknown;
                continue;
            }
            const std::string name = lower(trim(line.substr(1, line.size() - 2)));
            if (name == "field") {
                section = Section::kField;
            } else if (name == "menu" || name == "menus") {
                section = Section::kMenu;
            } else {
                report(line_no, "unknown section '" + printable(line) +
                                    "' ([field] or [menu]): its lines are skipped");
                section = Section::kUnknown;
            }
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            report(line_no, "not 'action = controls': '" + printable(line) + "'");
            continue;
        }
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key == "nintendo_buttons") {
            FaceButtons face{};
            if (!parse_face_buttons(value, &face)) {
                report(line_no, "nintendo_buttons must be position or label, not '" +
                                    printable(value) + "'");
            } else {
                set->face = face;
            }
            continue;
        }
        if (section == Section::kUnknown) {
            continue; // reported at its header
        }
        if (section == Section::kNone) {
            report(line_no, "'" + printable(key) + "' before [field] or [menu]: skipped");
            continue;
        }
        const Context context = section == Section::kField ? Context::kField : Context::kMenu;
        const int ci = section == Section::kField ? 0 : 1;
        const std::uint32_t action = mt_input_action_named(key.c_str());
        if (action == MT_ACTION_NONE) {
            report(line_no, "unknown action '" + printable(key) + "'");
            continue;
        }
        const auto seen = action_line.find({ci, action});
        if (seen != action_line.end()) {
            report(line_no, key + " " + in_context(context) + " is already set on line " +
                                std::to_string(seen->second) + ": this line is skipped");
            continue;
        }
        std::vector<Control> controls;
        const bool none = lower(value) == "none";
        if (!none) {
            std::size_t from = 0;
            while (from <= value.size()) {
                const std::size_t comma = value.find(',', from);
                const std::string_view item = trim(
                    value.substr(from, comma == std::string_view::npos ? comma : comma - from));
                from = comma == std::string_view::npos ? value.size() + 1 : comma + 1;
                if (item.empty()) {
                    report(line_no, "an empty control (two commas, or one at the end)");
                    continue;
                }
                Control c;
                std::string why;
                if (!parse_control(item, &c, &why)) {
                    report(line_no, why + ": skipped");
                    continue;
                }
                if (reserved_control(c)) {
                    report(line_no, control_text(c) +
                                        " opens the remap overlay and cannot be bound: skipped");
                    continue;
                }
                if (std::find(controls.begin(), controls.end(), c) != controls.end()) {
                    report(line_no, control_text(c) + " is listed twice");
                    continue;
                }
                const auto claim = std::find_if(claims[ci].begin(), claims[ci].end(),
                                                [&](const Claim &k) { return k.control == c; });
                if (claim != claims[ci].end()) {
                    report(line_no, control_text(c) + " is already bound to " +
                                        action_name(claim->action) + " " + in_context(context) +
                                        " on line " + std::to_string(claim->line) +
                                        ": skipped here");
                    continue;
                }
                controls.push_back(c);
            }
            if (controls.empty()) {
                report(line_no, key + ": no control left on the line (write '" + key +
                                    " = none' to unbind it): the line is skipped");
                continue;
            }
        }
        action_line[{ci, action}] = line_no;
        for (const Control &c : controls) {
            claims[ci].push_back(Claim{c, action, line_no});
        }
        set_controls(&set->table, context, action, controls);
    }
    for (const std::string &w : unbound_warnings(set->table)) {
        problems.push_back(file + ": " + w);
    }
    return problems;
}

std::string serialise_bindings(const BindingSet &set) {
    std::string out =
        "# MapleThief bindings (docs/controls/remapping.md). One line per action and context:\n"
        "#   action = control, control, ...   (or: action = none)\n"
        "# Controls by SDL's names: key NAME (Z, Return, Left Ctrl, F5, Comma), button NAME (a b x "
        "y\n"
        "# by the Xbox layout's positions, back start leftshoulder rightshoulder leftstick\n"
        "# rightstick ...), trigger lefttrigger | righttrigger, direction up | down | left | "
        "right\n"
        "# (the d-pad and the left stick). F1 and the gamepad's Guide button open the remap\n"
        "# overlay and cannot be bound.\n"
        "\n"
        "# A Nintendo pad's face buttons by position (SDL's way: the bottom one is 'a') or by\n"
        "# label (its A is 'a'): position | label\n";
    out += std::string("nintendo_buttons = ") + face_buttons_name(set.face) + "\n";
    Context current = Context::kMenu;
    bool any = false;
    for (const ActionSlot &s : action_slots(set.table)) {
        if (!any || s.context != current) {
            out += s.context == Context::kField
                       ? "\n# In the field (the game's key pad)\n[field]\n"
                       : "\n# In menus, popups and every other screen\n[menu]\n";
            current = s.context;
            any = true;
        }
        out += action_name(s.action);
        out += " = ";
        const std::vector<Control> controls = controls_for(set.table, s.context, s.action);
        if (controls.empty()) {
            out += "none";
        }
        for (std::size_t i = 0; i < controls.size(); i++) {
            out += (i == 0 ? "" : ", ") + control_text(controls[i]);
        }
        out += "\n";
    }
    return out;
}

bool same_bindings(const std::vector<Binding> &a, const std::vector<Binding> &b) {
    // What Actions::action_for sees: a control's first row.
    const auto first = [](const std::vector<Binding> &t, const Control &c, Context context) {
        for (const Binding &x : t) {
            if (x.control == c) {
                return slot_of(x, context);
            }
        }
        return static_cast<std::uint32_t>(MT_ACTION_NONE);
    };
    for (const std::vector<Binding> *t : {&a, &b}) {
        for (const Binding &r : *t) {
            for (const Context context : {Context::kField, Context::kMenu}) {
                if (first(a, r.control, context) != first(b, r.control, context)) {
                    return false;
                }
            }
        }
    }
    return true;
}

std::string bindings_path(const std::string &data_dir, const std::string &flag) {
    if (!flag.empty()) {
        return flag;
    }
    if (data_dir.empty()) {
        return "";
    }
    const std::u8string s = (utf8_path(data_dir) / kBindingsFileName).u8string();
    return std::string(s.begin(), s.end());
}

bool load_bindings(const std::string &path, BindingSet *set) {
    if (path.empty()) {
        SDL_Log("bindings: no data directory, so no %s: the defaults", kBindingsFileName);
        return true;
    }
    const fs::path file = utf8_path(path);
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        SDL_Log("bindings: no %s: the defaults", path.c_str());
        return true;
    }
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec || size > kMaxBindingsBytes) {
        SDL_Log("bindings: %s: %s, ignored: the defaults", path.c_str(),
                ec ? "cannot get its size" : "too large");
        return !ec;
    }
    std::ifstream in(file, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && !in.eof()) {
        SDL_Log("bindings: cannot read %s: the defaults", path.c_str());
        return false;
    }
    BindingSet read = *set;
    for (const std::string &problem : parse_bindings(text, path, &read)) {
        SDL_Log("bindings: %s", problem.c_str());
    }
    *set = std::move(read);
    SDL_Log("bindings: read %s", path.c_str());
    return true;
}

bool save_bindings(const std::string &path, const BindingSet &set) {
    if (path.empty()) {
        SDL_Log("bindings: no data directory: %s not written", kBindingsFileName);
        return false;
    }
    const fs::path file = utf8_path(path);
    const std::string text = serialise_bindings(set);
    std::error_code ec;
    if (file.has_parent_path()) {
        fs::create_directories(file.parent_path(), ec);
    }
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            SDL_Log("bindings: cannot write %s.tmp", path.c_str());
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        SDL_Log("bindings: cannot replace %s: %s", path.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
        return false;
    }
    SDL_Log("bindings: wrote %s", path.c_str());
    return true;
}

} // namespace maplethief::frontend
