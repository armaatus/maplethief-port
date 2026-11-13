// The opt-in update notice (#199, ADR 0137); see update_check.h.
#include "update_check.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#ifndef MT_VERSION
#define MT_VERSION "unknown"
#endif

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ident_char(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
}
bool all_digits(std::string_view s) { return std::all_of(s.begin(), s.end(), is_digit); }

// A numeric identifier of the version core: digits, no leading zero, fits 64 bits.
bool parse_core(std::string_view s, std::uint64_t *out) {
    if (s.empty() || !all_digits(s) || (s.size() > 1 && s[0] == '0')) {
        return false;
    }
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), *out);
    return ec == std::errc() && end == s.data() + s.size();
}

std::vector<std::string_view> split_dots(std::string_view s) {
    std::vector<std::string_view> parts;
    for (;;) {
        const std::size_t dot = s.find('.');
        parts.push_back(s.substr(0, dot));
        if (dot == std::string_view::npos) {
            return parts;
        }
        s.remove_prefix(dot + 1);
    }
}

bool valid_identifiers(const std::vector<std::string_view> &parts) {
    return std::all_of(parts.begin(), parts.end(), [](std::string_view p) {
        return !p.empty() && std::all_of(p.begin(), p.end(), is_ident_char);
    });
}

int sign(int c) { return c < 0 ? -1 : c > 0 ? 1 : 0; }
int compare_number(std::uint64_t a, std::uint64_t b) { return a < b ? -1 : a > b ? 1 : 0; }

// A pre-release identifier. Numeric ones compare by value: having no leading zeros, by length
// and then by digits, so one of any length needs no integer type.
int compare_identifier(std::string_view a, std::string_view b) {
    const bool na = all_digits(a);
    const bool nb = all_digits(b);
    if (na != nb) {
        return na ? -1 : 1; // numeric identifiers are older than alphanumeric ones
    }
    if (na && a.size() != b.size()) {
        return a.size() < b.size() ? -1 : 1;
    }
    return sign(a.compare(b));
}

// A small JSON reader over the response: just enough to step over any value and read the top
// object's strings and booleans. The input is untrusted: every step is bounded by the end of the
// text and nesting by kMaxDepth.
class JsonScan {
  public:
    explicit JsonScan(std::string_view text) : text_(text) {}

    void ws() {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' ||
                                      text_[at_] == '\n' || text_[at_] == '\r')) {
            at_++;
        }
    }
    bool done() const { return at_ == text_.size(); }
    bool eat(char c) {
        if (at_ < text_.size() && text_[at_] == c) {
            at_++;
            return true;
        }
        return false;
    }
    char peek() const { return at_ < text_.size() ? text_[at_] : '\0'; }

    // A string into *out (may be null). A \u escape outside ASCII becomes '?', which no key or
    // valid tag holds.
    bool read_string(std::string *out) {
        if (!eat('"')) {
            return false;
        }
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            char put = c;
            if (c == '\\' && !read_escape(&put)) {
                return false;
            }
            if (out != nullptr) {
                out->push_back(put);
            }
        }
        return false;
    }

    bool literal(std::string_view word) {
        if (text_.substr(at_, word.size()) != word) {
            return false;
        }
        at_ += word.size();
        return true;
    }

    // Any value, stepped over.
    bool value(int depth) {
        if (depth > kMaxDepth) {
            return false;
        }
        ws();
        switch (peek()) {
        case '{':
            at_++;
            return members(depth, [](const std::string &) { return false; });
        case '[':
            at_++;
            return elements(depth);
        case '"':
            return read_string(nullptr);
        case 't':
            return literal("true");
        case 'f':
            return literal("false");
        case 'n':
            return literal("null");
        default:
            return number();
        }
    }

    // The members of an object whose '{' was eaten. `read(key)` returns true when it consumed
    // the member's value itself; otherwise the value is stepped over.
    template <typename Read> bool members(int depth, Read read) {
        ws();
        if (eat('}')) {
            return true;
        }
        for (;;) {
            ws();
            std::string key;
            if (!read_string(&key)) {
                return false;
            }
            ws();
            if (!eat(':')) {
                return false;
            }
            ws();
            if (!read(key) && !value(depth + 1)) {
                return false;
            }
            ws();
            if (eat('}')) {
                return true;
            }
            if (!eat(',')) {
                return false;
            }
        }
    }

  private:
    static constexpr int kMaxDepth = 32;

    bool read_escape(char *put) {
        if (at_ >= text_.size()) {
            return false;
        }
        const char e = text_[at_++];
        switch (e) {
        case '"':
        case '\\':
        case '/':
            *put = e;
            return true;
        case 'b':
            *put = '\b';
            return true;
        case 'f':
            *put = '\f';
            return true;
        case 'n':
            *put = '\n';
            return true;
        case 'r':
            *put = '\r';
            return true;
        case 't':
            *put = '\t';
            return true;
        case 'u': {
            if (text_.size() - at_ < 4) {
                return false;
            }
            unsigned code = 0;
            const char *first = text_.data() + at_;
            const auto [end, ec] = std::from_chars(first, first + 4, code, 16);
            if (ec != std::errc() || end != first + 4) {
                return false;
            }
            at_ += 4;
            *put = code < 0x80 ? static_cast<char>(code) : '?';
            return true;
        }
        default:
            return false;
        }
    }

    bool elements(int depth) {
        ws();
        if (eat(']')) {
            return true;
        }
        for (;;) {
            if (!value(depth + 1)) {
                return false;
            }
            ws();
            if (eat(']')) {
                return true;
            }
            if (!eat(',')) {
                return false;
            }
        }
    }

    bool number() {
        eat('-');
        const std::size_t start = at_;
        while (at_ < text_.size() &&
               (is_digit(text_[at_]) || text_[at_] == '.' || text_[at_] == 'e' ||
                text_[at_] == 'E' || text_[at_] == '+' || text_[at_] == '-')) {
            at_++;
        }
        return at_ != start;
    }

    std::string_view text_;
    std::size_t at_ = 0;
};

// A boolean member's value; std::nullopt if it is neither (the member is then malformed).
std::optional<bool> json_bool(JsonScan &scan) {
    if (scan.peek() == 't' && scan.literal("true")) {
        return true;
    }
    if (scan.peek() == 'f' && scan.literal("false")) {
        return false;
    }
    return std::nullopt;
}

std::string read_small_file(const std::string &path) {
    std::ifstream in(utf8_path(path), std::ios::binary);
    if (!in) {
        return "";
    }
    std::string text(4096, '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    return text;
}

// Beside the file, then renamed over it, so a crash leaves the old state or the new.
bool write_state_file(const std::string &path, const std::string &text) {
    std::error_code ec;
    const fs::path file = utf8_path(path);
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace

std::optional<SemVer> parse_semver(std::string_view text) {
    if (!text.empty() && text[0] == 'v') {
        text.remove_prefix(1);
    }
    SemVer v;
    if (const std::size_t plus = text.find('+'); plus != std::string_view::npos) {
        const std::string_view build = text.substr(plus + 1);
        if (!valid_identifiers(split_dots(build))) {
            return std::nullopt;
        }
        v.build = std::string(build);
        text = text.substr(0, plus);
    }
    if (const std::size_t dash = text.find('-'); dash != std::string_view::npos) {
        const std::vector<std::string_view> pre = split_dots(text.substr(dash + 1));
        if (!valid_identifiers(pre)) {
            return std::nullopt;
        }
        for (const std::string_view p : pre) {
            if (p.size() > 1 && p[0] == '0' && all_digits(p)) {
                return std::nullopt; // no leading zero in a numeric identifier
            }
            v.pre.emplace_back(p);
        }
        text = text.substr(0, dash);
    }
    const std::vector<std::string_view> core = split_dots(text);
    if (core.size() != 3 || !parse_core(core[0], &v.major) || !parse_core(core[1], &v.minor) ||
        !parse_core(core[2], &v.patch)) {
        return std::nullopt;
    }
    return v;
}

int compare_semver(const SemVer &a, const SemVer &b) {
    if (const int c = compare_number(a.major, b.major); c != 0) {
        return c;
    }
    if (const int c = compare_number(a.minor, b.minor); c != 0) {
        return c;
    }
    if (const int c = compare_number(a.patch, b.patch); c != 0) {
        return c;
    }
    if (a.pre.empty() != b.pre.empty()) {
        return a.pre.empty() ? 1 : -1; // a pre-release is older than its release
    }
    const std::size_t n = std::min(a.pre.size(), b.pre.size());
    for (std::size_t i = 0; i < n; i++) {
        if (const int c = compare_identifier(a.pre[i], b.pre[i]); c != 0) {
            return c;
        }
    }
    return compare_number(a.pre.size(), b.pre.size());
}

std::optional<ReleaseInfo> parse_release_json(std::string_view body) {
    JsonScan scan(body);
    scan.ws();
    if (!scan.eat('{')) {
        return std::nullopt;
    }
    ReleaseInfo info;
    bool malformed = false;
    const bool ok = scan.members(0, [&](const std::string &key) {
        if (key == "tag_name") {
            info.tag.clear();
            malformed = malformed || !scan.read_string(&info.tag);
            return true;
        }
        if (key == "prerelease" || key == "draft") {
            const std::optional<bool> b = json_bool(scan);
            malformed = malformed || !b;
            (key == "draft" ? info.draft : info.prerelease) = b.value_or(false);
            return true;
        }
        return false;
    });
    scan.ws();
    if (!ok || malformed || !scan.done() || info.tag.empty()) {
        return std::nullopt;
    }
    return info;
}

std::optional<std::string> newer_release(const ReleaseInfo &release, std::string_view running) {
    const std::optional<SemVer> tag = parse_semver(release.tag);
    const std::optional<SemVer> current = parse_semver(running);
    if (release.draft || release.prerelease || !tag || !current || !tag->pre.empty() ||
        compare_semver(*tag, *current) <= 0) {
        return std::nullopt;
    }
    return release.tag;
}

bool update_check_due(std::int64_t now, std::optional<std::int64_t> last) {
    if (!last || *last > now) {
        return true;
    }
    return now - *last >= kUpdateIntervalSeconds;
}

bool update_check_wanted(const Settings &settings, bool interactive) {
    return settings.update_check && settings.preset != Preset::kOriginal && interactive;
}

UpdateState parse_update_state(std::string_view text) {
    UpdateState state;
    std::istringstream in{std::string(text)};
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t eq = line.find(" = ");
        if (eq == std::string::npos) {
            continue;
        }
        const std::string_view key = std::string_view(line).substr(0, eq);
        const std::string_view value = std::string_view(line).substr(eq + 3);
        if (key == "last_check") {
            std::int64_t t = 0;
            const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), t);
            if (ec == std::errc() && end == value.data() + value.size() && t >= 0) {
                state.last_check = t;
            }
        } else if (key == "latest" && parse_semver(value)) {
            state.latest_tag = std::string(value);
        }
    }
    return state;
}

std::string serialise_update_state(const UpdateState &state) {
    std::string out = "# MapleThief: when the port last looked for a new release (update_check)\n";
    if (state.last_check) {
        out += "last_check = " + std::to_string(*state.last_check) + "\n";
    }
    if (!state.latest_tag.empty()) {
        out += "latest = " + state.latest_tag + "\n";
    }
    return out;
}

std::string update_user_agent() { return std::string("MapleThief/") + MT_VERSION; }

bool update_redirect_allowed(std::string_view url) {
    constexpr std::string_view scheme = "https://";
    if (url.size() <= scheme.size()) {
        return false;
    }
    for (std::size_t i = 0; i < scheme.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(url[i])) != scheme[i]) {
            return false;
        }
    }
    const char host = url[scheme.size()];
    return host != '/' && host != '?' && host != '#' && host != ':' && host != '@';
}

bool UpdateBody::stated_too_long(std::int64_t stated) {
    return stated > static_cast<std::int64_t>(kUpdateMaxResponseBytes);
}

bool UpdateBody::append(const void *data, std::size_t n) {
    if (too_long_ || n > kUpdateMaxResponseBytes - body_.size()) {
        too_long_ = true;
        return false;
    }
    body_.append(static_cast<const char *>(data), n);
    return true;
}

struct UpdateCheck::Shared {
    mutable std::mutex mutex;
    mutable std::condition_variable finished;
    std::string newer;    // guarded by mutex
    bool running = false; // guarded by mutex
    std::atomic<bool> has_notice{false};
    std::string state_path;
    std::string version;
    UpdateState state;
    std::unique_ptr<UpdateFetcher> fetcher;
    std::function<void(const std::string &)> log;

    void write_log(const std::string &line) const {
        if (log) {
            log(line);
        } else {
            SDL_Log("%s", line.c_str());
        }
    }
    void set_newer(const std::string &tag) {
        const std::lock_guard<std::mutex> lock(mutex);
        newer = tag;
        has_notice.store(!tag.empty());
    }
    void finish() {
        const std::lock_guard<std::mutex> lock(mutex);
        running = false;
        finished.notify_all();
    }
    // The check's thread. One log line, whatever happens.
    void check() {
        std::string why;
        const std::optional<std::string> body = fetcher->get(kUpdateApiUrl, &why);
        const std::optional<ReleaseInfo> release = body ? parse_release_json(*body) : std::nullopt;
        if (!body) {
            write_log("update: check failed: " + (why.empty() ? std::string("no response") : why));
        } else if (!release) {
            write_log("update: check failed: the response is not a release");
        } else if (release->draft || release->prerelease || !parse_semver(release->tag)) {
            write_log("update: check skipped " + release->tag + ": not a finished release");
        } else {
            const std::optional<std::string> tag = newer_release(*release, version);
            state.latest_tag = release->tag;
            const bool saved = write_state_file(state_path, serialise_update_state(state));
            set_newer(tag.value_or(""));
            write_log((tag ? "update: port " + *tag + " is out"
                           : "update: " + release->tag + " is the latest; this is " + version) +
                      (saved ? "" : " (the state file could not be written)"));
        }
        finish();
    }
};

void UpdateCheck::start(UpdateCheckConfig config) {
    if (!config.wanted) {
        return;
    }
    auto shared = std::make_shared<Shared>();
    shared->state_path = std::move(config.state_path);
    shared->version = std::move(config.running_version);
    shared->log = std::move(config.log);
    if (!parse_semver(shared->version)) {
        shared->write_log("update: no check: this build's version " + shared->version +
                          " is not a release version");
        return;
    }
    shared->state = parse_update_state(read_small_file(shared->state_path));
    if (!shared->state.latest_tag.empty()) {
        ReleaseInfo kept;
        kept.tag = shared->state.latest_tag;
        shared->set_newer(newer_release(kept, shared->version).value_or(""));
    }
    shared_ = shared;
    if (!update_check_due(config.now, shared->state.last_check)) {
        return;
    }
    shared->fetcher = config.make_fetcher ? config.make_fetcher() : nullptr;
    if (shared->fetcher == nullptr) {
        shared->write_log("update: no check: no HTTP on this system");
        return;
    }
    // Before the request: a failed check counts too, so the port asks at most once a day.
    shared->state.last_check = config.now;
    if (!write_state_file(shared->state_path, serialise_update_state(shared->state))) {
        shared->write_log("update: no check: the state file could not be written");
        return;
    }
    shared->running = true;
    try {
        std::thread([shared] { shared->check(); }).detach();
    } catch (const std::exception &e) {
        shared->finish();
        shared->write_log(std::string("update: check failed: no thread: ") + e.what());
    }
}

bool UpdateCheck::has_notice() const { return shared_ != nullptr && shared_->has_notice.load(); }

std::string UpdateCheck::newer_tag() const {
    if (shared_ == nullptr) {
        return "";
    }
    const std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->newer;
}

std::string UpdateCheck::release_url() const {
    const std::string tag = newer_tag();
    return tag.empty() ? std::string() : std::string(kUpdateReleasePage) + tag;
}

bool UpdateCheck::wait(double seconds) const {
    if (shared_ == nullptr) {
        return true;
    }
    std::unique_lock<std::mutex> lock(shared_->mutex);
    return shared_->finished.wait_for(lock, std::chrono::duration<double>(seconds),
                                      [this] { return !shared_->running; });
}

} // namespace maplethief::frontend
