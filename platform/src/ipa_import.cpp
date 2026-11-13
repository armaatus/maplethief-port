// The first-launch IPA import (#182, ADR 0100); see ipa_import.h.
#include "ipa_import.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <set>
#include <system_error>

#include "game.h" // find_armv7_slice
#include "maplethief/snapshot.h"
#include "save_zip.h"

namespace maplethief::frontend {

namespace fs = std::filesystem;

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr std::string_view kAppName = "MapleThief.app";
constexpr std::string_view kExecutableName = "MapleThief";
constexpr std::string_view kTmpPrefix = "game.tmp-";
constexpr std::string_view kOldPrefix = "game.old-";

// A path from a UTF-8 string, and back (on Windows a narrow string is not UTF-8).
fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

IpaResult refuse(IpaProblem problem, std::string why) {
    IpaResult r;
    r.problem = problem;
    r.why = std::move(why);
    return r;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
std::string lowered(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), lower);
    return out;
}
bool ends_with_ci(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && lowered(s.substr(s.size() - suffix.size())) == suffix;
}

u32 le32(const u8 *p) {
    return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
}
u64 be_uint(const u8 *p, std::size_t n) {
    u64 v = 0;
    for (std::size_t i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

// --- binary property lists (bplist00) -------------------------------------------------------

class BinaryPlist {
  public:
    explicit BinaryPlist(std::span<const u8> b) : b_(b) {}

    bool open() {
        const std::size_t n = b_.size();
        if (n < 8 + 32 || std::memcmp(b_.data(), "bplist00", 8) != 0) {
            return false;
        }
        const u8 *t = b_.data() + n - 32;
        offset_size_ = t[6];
        ref_size_ = t[7];
        count_ = be_uint(t + 8, 8);
        top_ = be_uint(t + 16, 8);
        table_ = be_uint(t + 24, 8);
        if (offset_size_ < 1 || offset_size_ > 8 || ref_size_ < 1 || ref_size_ > 8 || count_ == 0 ||
            count_ > n || top_ >= count_ || table_ < 8 || table_ > n - 32 ||
            count_ > (n - 32 - table_) / offset_size_) {
            return false;
        }
        return true;
    }
    u64 top() const { return top_; }

    // Object `ref`'s offset, or 0 if it lies outside the objects.
    std::size_t offset(u64 ref) const {
        if (ref >= count_) {
            return 0;
        }
        const u64 at = be_uint(b_.data() + table_ + ref * offset_size_, offset_size_);
        return at >= 8 && at < table_ ? static_cast<std::size_t>(at) : 0;
    }

    // The marker's count (the low nibble, or the int that follows a 0xF) and where the
    // object's contents start; false if that runs outside the objects.
    bool header(std::size_t at, u8 *type, u64 *count, std::size_t *body) const {
        if (at == 0 || at >= table_) {
            return false;
        }
        const u8 marker = b_[at];
        *type = static_cast<u8>(marker >> 4);
        *count = marker & 0x0F;
        *body = at + 1;
        if (*count == 0x0F) {
            if (*body >= table_ || (b_[*body] >> 4) != 0x1) {
                return false;
            }
            const std::size_t width = std::size_t{1} << (b_[*body] & 0x0F);
            if (width > 8 || *body + 1 + width > table_) {
                return false;
            }
            *count = be_uint(b_.data() + *body + 1, width);
            *body += 1 + width;
        }
        return true;
    }

    // The string object `ref` (ASCII, or UTF-16 to UTF-8).
    bool string(u64 ref, std::string *out) const {
        u8 type = 0;
        u64 count = 0;
        std::size_t body = 0;
        if (!header(offset(ref), &type, &count, &body)) {
            return false;
        }
        if (type == 0x5) {
            if (count > table_ - body) {
                return false;
            }
            out->assign(reinterpret_cast<const char *>(b_.data() + body),
                        static_cast<std::size_t>(count));
            return true;
        }
        if (type != 0x6 || count > (table_ - body) / 2) {
            return false;
        }
        out->clear();
        for (u64 i = 0; i < count; i++) {
            u32 c = static_cast<u32>(be_uint(b_.data() + body + 2 * i, 2));
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < count) {
                const u32 lo = static_cast<u32>(be_uint(b_.data() + body + 2 * (i + 1), 2));
                if (lo >= 0xDC00 && lo < 0xE000) {
                    c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                }
            }
            if (c < 0x80) {
                out->push_back(static_cast<char>(c));
            } else if (c < 0x800) {
                out->push_back(static_cast<char>(0xC0 | (c >> 6)));
                out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else if (c < 0x10000) {
                out->push_back(static_cast<char>(0xE0 | (c >> 12)));
                out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
            } else {
                out->push_back(static_cast<char>(0xF0 | (c >> 18)));
                out->push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
            }
        }
        return true;
    }

    // The value of `key` in the dictionary object `ref`, as a string.
    bool dict_string(u64 ref, std::string_view key, std::string *value) const {
        u8 type = 0;
        u64 count = 0;
        std::size_t body = 0;
        if (!header(offset(ref), &type, &count, &body) || type != 0xD ||
            count > (table_ - body) / (2 * ref_size_)) {
            return false;
        }
        for (u64 i = 0; i < count; i++) {
            const u64 k = be_uint(b_.data() + body + i * ref_size_, ref_size_);
            std::string name;
            if (!string(k, &name) || name != key) {
                continue;
            }
            const u64 v = be_uint(b_.data() + body + (count + i) * ref_size_, ref_size_);
            return string(v, value);
        }
        return false;
    }

  private:
    std::span<const u8> b_;
    std::size_t offset_size_ = 0, ref_size_ = 0;
    u64 count_ = 0, top_ = 0, table_ = 0;
};

// An XML plist's <key>KEY</key> followed by <string>VALUE</string> (whitespace between).
bool xml_plist_string(std::string_view xml, std::string_view key, std::string *value) {
    const std::string open = "<key>" + std::string(key) + "</key>";
    const std::size_t at = xml.find(open);
    if (at == std::string_view::npos) {
        return false;
    }
    std::size_t p = at + open.size();
    while (p < xml.size() &&
           (xml[p] == ' ' || xml[p] == '\t' || xml[p] == '\r' || xml[p] == '\n')) {
        p++;
    }
    constexpr std::string_view kOpen = "<string>", kClose = "</string>";
    if (xml.substr(p, kOpen.size()) != kOpen) {
        return false;
    }
    p += kOpen.size();
    const std::size_t end = xml.find(kClose, p);
    if (end == std::string_view::npos) {
        return false;
    }
    *value = std::string(xml.substr(p, end - p));
    return true;
}

// --- the archive -----------------------------------------------------------------------------

// The entry named `name` (exactly, as stored), or nullptr.
const ZipItem *find_item(const std::vector<ZipItem> &items, std::string_view name) {
    for (const ZipItem &item : items) {
        if (item.name == name) {
            return &item;
        }
    }
    return nullptr;
}

// The <name>.app folders right under Payload/.
std::set<std::string> app_folders(const std::vector<ZipItem> &items) {
    std::set<std::string> apps;
    for (const ZipItem &item : items) {
        const std::string_view name = item.name;
        if (name.substr(0, 8) != "Payload/") {
            continue;
        }
        const std::size_t slash = name.find('/', 8);
        const std::string_view first =
            name.substr(8, slash == std::string_view::npos ? std::string_view::npos : slash - 8);
        if (ends_with_ci(first, ".app") && first.size() > 4) {
            apps.insert(std::string(first));
        }
    }
    return apps;
}

std::string short_hex(const Sha256 &d) { return sha256_hex(d).substr(0, 16) + "..."; }

// Why a file that is not the expected IPA is not: see ipa_import.h.
IpaResult diagnose(std::span<const u8> ipa, const IpaExpected &expected) {
    std::vector<ZipItem> items;
    std::string why;
    if (!zip_list(ipa, &items, &why)) {
        return refuse(IpaProblem::kNotZip, "This file is not an IPA: " + why + ".");
    }
    const std::set<std::string> apps = app_folders(items);
    if (apps.empty()) {
        return refuse(IpaProblem::kNotIpa,
                      "This zip file is not an iPhone app (IPA): it has no Payload/<name>.app "
                      "folder.");
    }
    if (apps.count(std::string(kAppName)) == 0) {
        return refuse(IpaProblem::kOtherApp,
                      "This is the IPA of another app (" + *apps.begin() + "), not MapleThief.");
    }
    for (const ZipItem &item : items) {
        if (item.encrypted()) {
            return refuse(IpaProblem::kEncryptedZip,
                          "This IPA's files are encrypted with a zip password (" + item.name +
                              "), so they cannot be read.");
        }
    }
    const std::string app = "Payload/" + std::string(kAppName) + "/";
    std::string version;
    std::string executable(kExecutableName);
    if (const ZipItem *plist = find_item(items, app + "Info.plist")) {
        std::vector<u8> bytes;
        if (zip_extract(ipa, *plist, 1u << 20, &bytes, nullptr)) {
            if (!plist_string(bytes, "CFBundleVersion", &version)) {
                plist_string(bytes, "CFBundleShortVersionString", &version);
            }
            std::string name;
            if (plist_string(bytes, "CFBundleExecutable", &name) && !name.empty() &&
                name.find('/') == std::string::npos) {
                executable = name;
            }
        }
    }
    if (const ZipItem *exe = find_item(items, app + executable)) {
        std::vector<u8> bytes;
        std::size_t off = 0, len = 0;
        u32 cryptid = 0;
        if (zip_extract(ipa, *exe, kMaxIpaEntryBytes, &bytes, nullptr) &&
            find_armv7_slice(bytes.data(), bytes.size(), &off, &len, nullptr) &&
            macho_cryptid(std::span<const u8>(bytes).subspan(off, len), &cryptid) && cryptid != 0) {
            IpaResult r = refuse(IpaProblem::kEncryptedApp,
                                 "This IPA's game executable is encrypted (cryptid " +
                                     std::to_string(cryptid) +
                                     "): the port cannot run an encrypted executable.");
            r.version = version;
            return r;
        }
    }
    if (version.empty()) {
        return refuse(IpaProblem::kOtherVersion,
                      "This MapleThief IPA is not version 1.5: its Info.plist gives no version "
                      "the port can read.");
    }
    if (version != kIpaVersion) {
        IpaResult r = refuse(IpaProblem::kOtherVersion, "This is MapleThief " + version +
                                                            "; the port needs version " +
                                                            std::string(kIpaVersion) + ".");
        r.version = version;
        return r;
    }
    IpaResult r = refuse(IpaProblem::kModified,
                         "This is MapleThief " + version + ", but not the known " +
                             std::string(kIpaFileName) + " (" + std::to_string(ipa.size()) +
                             " bytes, SHA-256 " + short_hex(sha256_of(ipa)) + "; expected " +
                             std::to_string(expected.size) + " bytes, " +
                             short_hex(expected.ipa_sha256) + "): it is damaged or changed.");
    r.version = version;
    return r;
}

// One entry to extract: its place below the app folder.
struct Member {
    const ZipItem *item = nullptr;
    std::vector<std::string> rel; // the components below Payload/MapleThief.app/
};

// bootstrap.py's safe_members over every entry; the members below Payload/MapleThief.app/.
IpaResult safe_members(const std::vector<ZipItem> &items, std::vector<Member> *members) {
    std::map<std::string, std::string> seen; // lower-cased path -> entry name
    std::set<std::string> files, parents;
    for (const ZipItem &item : items) {
        std::vector<std::string> parts;
        std::string why;
        const auto unsafe = [&item](const std::string &what) {
            return refuse(IpaProblem::kUnsafeEntry, "This IPA's entry \"" + item.name +
                                                        "\" is not safe to extract (" + what +
                                                        "), so nothing was imported.");
        };
        if (!ipa_entry_parts(item.name, &parts, &why)) {
            return unsafe(why);
        }
        // A Unix host's file type (0 for MS-DOS entries).
        const u32 mode = (item.external >> 16) & 0170000u;
        if (mode == 0120000u) {
            return unsafe("a symbolic link");
        }
        if (mode != 0 && mode != 0100000u && mode != 0040000u) {
            return unsafe("not a regular file");
        }
        if (item.encrypted()) {
            return refuse(IpaProblem::kEncryptedZip,
                          "This IPA's entry \"" + item.name + "\" is encrypted.");
        }
        std::string key;
        for (const std::string &p : parts) {
            key += (key.empty() ? "" : "/") + lowered(p);
        }
        if (const auto [at, fresh] = seen.emplace(key, item.name); !fresh) {
            return unsafe("it names the same file as \"" + at->second +
                          "\" on a case-insensitive file system");
        }
        if (!item.is_dir()) {
            files.insert(key);
        }
        std::string parent;
        for (std::size_t i = 0; i + 1 < parts.size(); i++) {
            parent += (i == 0 ? "" : "/") + lowered(parts[i]);
            parents.insert(parent);
        }
        if (parts.size() > 2 && parts[0] == "Payload" && parts[1] == kAppName) {
            members->push_back(
                Member{&item, std::vector<std::string>(parts.begin() + 2, parts.end())});
        }
    }
    for (const std::string &f : files) {
        if (parents.count(f) != 0) {
            return refuse(IpaProblem::kUnsafeEntry, "This IPA's entry \"" + f +
                                                        "\" is both a file and a folder, so "
                                                        "nothing was imported.");
        }
    }
    return IpaResult{};
}

void remove_quietly(const fs::path &p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// Removes the game.tmp-* and game.old-* folders an interrupted import left in `dir`.
void remove_leftovers(const fs::path &dir) {
    std::error_code ec;
    std::vector<fs::path> stale;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = path_utf8(it->path().filename());
        if (name.rfind(kTmpPrefix, 0) == 0 || name.rfind(kOldPrefix, 0) == 0) {
            stale.push_back(it->path());
        }
    }
    for (const fs::path &p : stale) {
        remove_quietly(p);
    }
}

// A suffix that no other import of this process or a concurrent one uses.
std::string unique_suffix() {
    static std::atomic<unsigned> counter{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    char buf[40];
    std::snprintf(buf, sizeof buf, "%llx-%u", static_cast<unsigned long long>(now),
                  counter.fetch_add(1));
    return buf;
}

// The whole file, if it is at most `max` bytes.
bool read_whole_file(const fs::path &path, std::vector<u8> *bytes, u64 max = kMaxIpaBytes) {
    bytes->clear();
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if (ec || size > max) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    bytes->resize(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char *>(bytes->data()), static_cast<std::streamsize>(size));
    return static_cast<u64>(in.gcount()) == size;
}

IpaResult read_ipa(const std::string &path, std::vector<u8> *bytes) {
    const fs::path p = utf8_path(path);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        return refuse(IpaProblem::kUnreadable, path + " is not a file.");
    }
    const auto size = fs::file_size(p, ec);
    if (!ec && size > kMaxIpaBytes) {
        return refuse(IpaProblem::kUnreadable, path + " is too large (" + std::to_string(size) +
                                                   " bytes) to be MapleThief's IPA.");
    }
    try {
        if (!read_whole_file(p, bytes)) {
            return refuse(IpaProblem::kUnreadable, path + " cannot be read.");
        }
    } catch (const std::bad_alloc &) {
        return refuse(IpaProblem::kUnreadable, "There is not enough memory to read " + path + ".");
    }
    return IpaResult{};
}

bool write_file(const fs::path &path, const std::vector<u8> &bytes) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
        return false; // never through anything that is already there
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return static_cast<bool>(out);
}

IpaResult extract(std::span<const u8> ipa, const std::string &data_dir, const IpaExpected &expected,
                  const IpaProgress &progress) {
    std::vector<ZipItem> items;
    std::string why;
    if (!zip_list(ipa, &items, &why)) {
        return refuse(IpaProblem::kNotZip, "This file is not an IPA: " + why + ".");
    }
    std::vector<Member> members;
    if (IpaResult r = safe_members(items, &members); !r.ok()) {
        return r;
    }
    if (data_dir.empty()) {
        return refuse(IpaProblem::kWriteFailed, "There is no data directory to import into.");
    }
    const fs::path base = utf8_path(data_dir);
    std::error_code ec;
    fs::create_directories(base, ec);
    if (!fs::is_directory(base, ec)) {
        return refuse(IpaProblem::kWriteFailed,
                      "The data directory " + data_dir + " cannot be created.");
    }
    remove_leftovers(base);
    const fs::path game = base / std::string(kGameDirName);
    const fs::path tmp = base / (std::string(kTmpPrefix) + unique_suffix());
    const auto failed = [&](IpaProblem problem, const std::string &what) {
        remove_quietly(tmp);
        return refuse(problem, what);
    };
    if (!fs::create_directory(tmp, ec)) {
        return failed(IpaProblem::kWriteFailed,
                      "Cannot create " + path_utf8(tmp) + ": " + ec.message() + ".");
    }
    u64 total = 0;
    for (const Member &m : members) {
        total += m.item->is_dir() ? 0 : m.item->size;
    }
    u64 done = 0;
    std::vector<u8> bytes;
    for (const Member &m : members) {
        fs::path target = tmp;
        for (const std::string &p : m.rel) {
            target /= utf8_path(p);
        }
        if (m.item->is_dir()) {
            fs::create_directories(target, ec);
            if (ec) {
                return failed(IpaProblem::kWriteFailed,
                              "Cannot create " + path_utf8(target) + ": " + ec.message() + ".");
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            return failed(IpaProblem::kWriteFailed, "Cannot create " +
                                                        path_utf8(target.parent_path()) + ": " +
                                                        ec.message() + ".");
        }
        if (!zip_extract(ipa, *m.item, kMaxIpaEntryBytes, &bytes, &why)) {
            return failed(IpaProblem::kModified, "This IPA cannot be read: " + why + ".");
        }
        if (!write_file(target, bytes)) {
            return failed(IpaProblem::kWriteFailed, "Cannot write " + path_utf8(target) + ".");
        }
        done += m.item->size;
        if (progress && !progress(done, total)) {
            return failed(IpaProblem::kInterrupted, "The import was stopped; nothing was kept.");
        }
    }
    std::vector<u8> exe;
    const fs::path exe_path = tmp / std::string(kExecutableName);
    if (!read_whole_file(exe_path, &exe) || sha256_of(exe) != expected.executable_sha256) {
        return failed(IpaProblem::kWrongExecutable,
                      "This IPA's MapleThief executable is not version 1.5's.");
    }
    // Into place: an old game/ aside first, back if the rename fails, removed after.
    fs::path old;
    if (fs::exists(game, ec)) {
        old = base / (std::string(kOldPrefix) + unique_suffix());
        fs::rename(game, old, ec);
        if (ec) {
            return failed(IpaProblem::kWriteFailed, "Cannot move the old " + path_utf8(game) +
                                                        " aside: " + ec.message() + ".");
        }
    }
    fs::rename(tmp, game, ec);
    if (ec) {
        const std::string what = ec.message();
        if (!old.empty()) {
            fs::rename(old, game, ec);
        }
        return failed(IpaProblem::kWriteFailed,
                      "Cannot rename the import to " + path_utf8(game) + ": " + what + ".");
    }
    if (!old.empty()) {
        remove_quietly(old);
    }
    IpaResult r;
    r.version = std::string(kIpaVersion);
    r.game_dir = path_utf8(game);
    return r;
}

} // namespace

IpaExpected ipa_expected() {
    IpaExpected e;
    e.size = kIpaSize;
    parse_sha256(kIpaSha256, &e.ipa_sha256);
    parse_sha256(kExecutableSha256, &e.executable_sha256);
    return e;
}

const char *ipa_problem_name(IpaProblem problem) {
    switch (problem) {
    case IpaProblem::kNone:
        return "ok";
    case IpaProblem::kUnreadable:
        return "unreadable";
    case IpaProblem::kNotZip:
        return "not_zip";
    case IpaProblem::kNotIpa:
        return "not_ipa";
    case IpaProblem::kOtherApp:
        return "other_app";
    case IpaProblem::kEncryptedZip:
        return "encrypted_zip";
    case IpaProblem::kEncryptedApp:
        return "encrypted_app";
    case IpaProblem::kOtherVersion:
        return "other_version";
    case IpaProblem::kModified:
        return "modified";
    case IpaProblem::kUnsafeEntry:
        return "unsafe_entry";
    case IpaProblem::kWrongExecutable:
        return "wrong_executable";
    case IpaProblem::kWriteFailed:
        return "write_failed";
    case IpaProblem::kInterrupted:
        return "interrupted";
    }
    return "?";
}

IpaResult check_ipa_bytes(std::span<const std::uint8_t> ipa, const IpaExpected &expected) {
    try {
        if (ipa.size() == expected.size && sha256_of(ipa) == expected.ipa_sha256) {
            return IpaResult{};
        }
        return diagnose(ipa, expected);
    } catch (const std::bad_alloc &) {
        return refuse(IpaProblem::kUnreadable, "There is not enough memory to read this file.");
    }
}

IpaResult check_ipa(const std::string &path, const IpaExpected &expected,
                    std::vector<std::uint8_t> *bytes) {
    std::vector<u8> local;
    std::vector<u8> &data = bytes != nullptr ? *bytes : local;
    if (IpaResult r = read_ipa(path, &data); !r.ok()) {
        return r;
    }
    return check_ipa_bytes(data, expected);
}

IpaResult import_ipa_bytes(std::span<const std::uint8_t> ipa, const std::string &data_dir,
                           const IpaExpected &expected, const IpaProgress &progress) {
    if (IpaResult r = check_ipa_bytes(ipa, expected); !r.ok()) {
        return r;
    }
    try {
        return extract(ipa, data_dir, expected, progress);
    } catch (const std::bad_alloc &) {
        return refuse(IpaProblem::kWriteFailed, "There is not enough memory to import.");
    } catch (const fs::filesystem_error &e) {
        return refuse(IpaProblem::kWriteFailed, std::string("The import failed: ") + e.what());
    }
}

IpaResult import_ipa(const std::string &path, const std::string &data_dir,
                     const IpaExpected &expected, const IpaProgress &progress) {
    std::vector<u8> data;
    if (IpaResult r = read_ipa(path, &data); !r.ok()) {
        return r;
    }
    return import_ipa_bytes(data, data_dir, expected, progress);
}

std::vector<std::string> find_ipas(const std::string &folder) {
    std::vector<std::string> out;
    if (folder.empty()) {
        return out;
    }
    const fs::path dir = utf8_path(folder);
    std::vector<std::string> others;
    bool named = false;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_regular_file(type_ec)) {
            continue; // folders, links to folders, devices
        }
        const std::string name = path_utf8(it->path().filename());
        if (name.empty() || name[0] == '.' || !ends_with_ci(name, ".ipa")) {
            continue; // hidden, macOS's "._" files, not an IPA
        }
        if (name == kIpaFileName) {
            named = true;
        } else {
            others.push_back(name);
        }
    }
    std::sort(others.begin(), others.end());
    if (named) {
        out.push_back(path_utf8(dir / std::string(kIpaFileName)));
    }
    for (const std::string &name : others) {
        out.push_back(path_utf8(dir / utf8_path(name)));
    }
    return out;
}

std::string imported_game_dir(const std::string &data_dir) {
    return data_dir.empty() ? std::string()
                            : path_utf8(utf8_path(data_dir) / std::string(kGameDirName));
}

bool has_imported_game(const std::string &data_dir) {
    if (data_dir.empty()) {
        return false;
    }
    std::error_code ec;
    const fs::path game = utf8_path(data_dir) / std::string(kGameDirName);
    return fs::is_directory(game, ec) &&
           fs::is_regular_file(game / std::string(kExecutableName), ec);
}

Sha256 sha256_of(std::span<const std::uint8_t> bytes) {
    Sha256 d{};
    mt_sha256(bytes.data(), bytes.size(), d.data());
    return d;
}

std::string sha256_hex(const Sha256 &digest) {
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    for (const u8 b : digest) {
        s.push_back(kHex[b >> 4]);
        s.push_back(kHex[b & 15]);
    }
    return s;
}

bool parse_sha256(std::string_view hex, Sha256 *digest) {
    if (hex.size() != 64) {
        return false;
    }
    const auto nibble = [](char c) {
        return c >= '0' && c <= '9'   ? c - '0'
               : c >= 'a' && c <= 'f' ? c - 'a' + 10
               : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                      : -1;
    };
    Sha256 d{};
    for (std::size_t i = 0; i < 32; i++) {
        const int hi = nibble(hex[2 * i]), lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        d[i] = static_cast<u8>(hi << 4 | lo);
    }
    *digest = d;
    return true;
}

bool ipa_entry_parts(std::string_view name, std::vector<std::string> *parts, std::string *why) {
    parts->clear();
    const auto unsafe = [why](const char *what) {
        if (why != nullptr) {
            *why = what;
        }
        return false;
    };
    if (!name.empty() && name.front() == '/') {
        return unsafe("an absolute path");
    }
    if (!name.empty() && name.back() == '/') {
        name.remove_suffix(1); // a folder
    }
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = name.find('/', start);
        const std::string_view part = name.substr(
            start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (part.empty() || part == "." || part == "..") {
            parts->clear();
            return unsafe("an empty, '.' or '..' path component");
        }
        for (const char c : part) {
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x20 || std::strchr("<>:\"|?*\\", c) != nullptr) {
                parts->clear();
                return unsafe("a backslash, drive letter, control or reserved character");
            }
        }
        // Windows' device names, with or without an extension.
        const std::string stem = lowered(part.substr(0, part.find('.')));
        const bool device =
            stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
            (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) &&
             stem[3] >= '0' && stem[3] <= '9');
        if (part.back() == '.' || part.back() == ' ' || device) {
            parts->clear();
            return unsafe("a name Windows cannot store");
        }
        parts->emplace_back(part);
        if (slash == std::string_view::npos) {
            return true;
        }
        start = slash + 1;
    }
}

bool plist_string(std::span<const std::uint8_t> plist, std::string_view key, std::string *value) {
    BinaryPlist binary(plist);
    if (binary.open()) {
        return binary.dict_string(binary.top(), key, value);
    }
    const std::string_view text(reinterpret_cast<const char *>(plist.data()), plist.size());
    if (text.find("<plist") == std::string_view::npos) {
        return false;
    }
    return xml_plist_string(text, key, value);
}

bool macho_cryptid(std::span<const std::uint8_t> macho, std::uint32_t *cryptid) {
    constexpr u32 kMachMagic = 0xFEEDFACEu;
    constexpr u32 kEncryptionInfo = 0x21;   // LC_ENCRYPTION_INFO
    constexpr u32 kEncryptionInfo64 = 0x2C; // LC_ENCRYPTION_INFO_64
    constexpr std::size_t kHeader = 28;
    *cryptid = 0;
    const std::size_t n = macho.size();
    if (n < kHeader || le32(macho.data()) != kMachMagic) {
        return false;
    }
    const u32 ncmds = le32(macho.data() + 16);
    const u32 sizeofcmds = le32(macho.data() + 20);
    if (sizeofcmds > n - kHeader) {
        return false;
    }
    const std::size_t end = kHeader + sizeofcmds;
    std::size_t at = kHeader;
    for (u32 i = 0; i < ncmds; i++) {
        if (at + 8 > end) {
            return false;
        }
        const u32 cmd = le32(macho.data() + at);
        const u32 size = le32(macho.data() + at + 4);
        if (size < 8 || size > end - at) {
            return false;
        }
        if ((cmd == kEncryptionInfo || cmd == kEncryptionInfo64) && size >= 20) {
            *cryptid = le32(macho.data() + at + 16);
        }
        at += size;
    }
    return true;
}

} // namespace maplethief::frontend
