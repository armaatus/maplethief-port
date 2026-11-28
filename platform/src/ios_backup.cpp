// The game's saves out of an iPhone or iPod backup (#374); see ios_backup.h.
#include "ios_backup.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include "ipa_import.h"
#include "maplethief/saves.h"
#include "port_text.h"
#include "sqlite_read.h"

namespace maplethief::frontend {

namespace {

namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr std::string_view kDocuments = "Documents/";
// Manifest.db Files.flags; mbdb modes' type bits.
constexpr std::int64_t kFlagFile = 1, kFlagDirectory = 2, kFlagLink = 4;
constexpr u32 kTypeMask = 0xF000, kTypeFile = 0x8000, kTypeDirectory = 0x4000, kTypeLink = 0xA000;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

bool fail(std::string *why, const std::string &what) {
    if (why != nullptr) {
        *why = what;
    }
    return false;
}

std::string permission_text(const fs::path &p) {
    std::string out = "cannot read " + path_utf8(p) + ": permission denied";
#if defined(__APPLE__)
    if (path_utf8(p).find("MobileSync") != std::string::npos) {
        out += ". macOS protects the backup folder: give MapleThief Full Disk Access (System "
               "Settings > Privacy & Security), or copy the backup's folder into the data "
               "folder's import/";
    }
#endif
    return out;
}

// A regular file's bytes, refusing links, other kinds of file and files over `cap`.
bool read_capped(const fs::path &p, std::size_t cap, const std::string &what, std::vector<u8> *out,
                 std::string *why) {
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(p, ec);
    if (ec || st.type() == fs::file_type::not_found) {
        return fail(why, what + " is missing (" + path_utf8(p) + ")");
    }
    if (st.type() == fs::file_type::symlink) {
        return fail(why, what + " is a link (" + path_utf8(p) + "); a backup never holds one");
    }
    if (st.type() != fs::file_type::regular) {
        return fail(why, what + " is not a file (" + path_utf8(p) + ")");
    }
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec) {
        return fail(why, "cannot read " + path_utf8(p) + ": " + ec.message());
    }
    if (size > cap) {
        return fail(why, what + " has " + std::to_string(size) + " bytes, over the limit of " +
                             std::to_string(cap));
    }
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return fail(why, permission_text(p));
    }
    out->resize(static_cast<std::size_t>(size));
    if (size > 0 &&
        !f.read(reinterpret_cast<char *>(out->data()), static_cast<std::streamsize>(size))) {
        return fail(why, "cannot read " + path_utf8(p));
    }
    return true;
}

bool exists_file(const fs::path &p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

// A plist's bytes, or none when absent; `problem` when it is there and cannot be read.
std::optional<std::vector<u8>> read_plist(const fs::path &p, const std::string &what,
                                          std::string *problem) {
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(p, ec))) {
        return std::nullopt;
    }
    std::vector<u8> bytes;
    if (!read_capped(p, kMaxIosPlistBytes, what, &bytes, problem)) {
        return std::nullopt;
    }
    const bool binary = bytes.size() >= 8 && std::memcmp(bytes.data(), "bplist00", 8) == 0;
    const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    if (!binary && text.find("<plist") == std::string_view::npos) {
        *problem = what + " is not a property list";
        return std::nullopt;
    }
    return bytes;
}

bool is_old_format(const fs::path &dir) {
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(dir, ec)) {
        const fs::path ext = e.path().extension();
        if (ext == ".mdinfo" || ext == ".mddata") {
            return true;
        }
    }
    return false;
}

bool is_file_id(std::string_view id) {
    return id.size() == 40 && std::all_of(id.begin(), id.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

// One file of the game's domain, as the manifest lists it.
struct Entry {
    std::string relative_path;
    std::string file_id;
    enum class Kind { kFile, kDirectory, kLink, kOther } kind = Kind::kOther;
    std::optional<std::uint64_t> size; // mbdb's
    bool encrypted = false;            // mbdb: the record carries a key
};

const char *kind_text(Entry::Kind k) {
    switch (k) {
    case Entry::Kind::kFile:
        return "file";
    case Entry::Kind::kDirectory:
        return "folder";
    case Entry::Kind::kLink:
        return "link";
    case Entry::Kind::kOther:
        break;
    }
    return "special file";
}

bool db_entries(const fs::path &dir, const std::string &domain, std::vector<Entry> *out,
                std::string *why) {
    const fs::path db = dir / "Manifest.db";
    std::vector<u8> head(16);
    {
        std::ifstream f(db, std::ios::binary);
        if (!f) {
            return fail(why, permission_text(db));
        }
        f.read(reinterpret_cast<char *>(head.data()), 16);
        head.resize(static_cast<std::size_t>(f.gcount()));
    }
    if (head.size() < 16 || std::memcmp(head.data(), "SQLite format 3", 16) != 0) {
        // Since iOS 10.2 an encrypted backup's Manifest.db is encrypted too.
        return fail(why, "Manifest.db is not an SQLite database, so the backup is encrypted or "
                         "broken. " +
                             ios_encrypted_text());
    }
    SqliteReader reader;
    std::string error;
    if (!reader.open_file(path_utf8(db), &error)) {
        return fail(why, "Manifest.db cannot be opened: " + error);
    }
    std::uint32_t root = 0;
    std::vector<std::string> columns;
    if (!reader.table("Files", &root, &columns, &error)) {
        return fail(why, "Manifest.db cannot be read: " + error);
    }
    const auto column = [&columns](std::string_view name) {
        for (std::size_t i = 0; i < columns.size(); i++) {
            if (columns[i] == name) {
                return static_cast<int>(i);
            }
        }
        return -1;
    };
    const int c_id = column("fileID"), c_domain = column("domain"), c_path = column("relativePath"),
              c_flags = column("flags");
    if (c_id < 0 || c_domain < 0 || c_path < 0 || c_flags < 0) {
        return fail(why, "Manifest.db's table Files lacks fileID, domain, relativePath or flags");
    }
    bool too_many = false;
    const bool ok = reader.scan(
        root,
        [&](std::int64_t, const SqliteRow &r) {
            const auto at = [&r](int c) -> const SqliteValue * {
                return static_cast<std::size_t>(c) < r.size() ? &r[static_cast<std::size_t>(c)]
                                                              : nullptr;
            };
            const SqliteValue *d = at(c_domain);
            if (d == nullptr || !d->is_text() || d->bytes != domain) {
                return true;
            }
            const SqliteValue *id = at(c_id), *rel = at(c_path), *flags = at(c_flags);
            if (id == nullptr || rel == nullptr || !id->is_text() || !rel->is_text()) {
                return true; // not a row iOS writes; nothing to read for it
            }
            if (out->size() >= kMaxIosAppRows) {
                too_many = true;
                return false;
            }
            Entry e;
            e.relative_path = rel->bytes;
            e.file_id = id->bytes;
            if (flags != nullptr && flags->is_integer()) {
                e.kind = flags->integer == kFlagFile        ? Entry::Kind::kFile
                         : flags->integer == kFlagDirectory ? Entry::Kind::kDirectory
                         : flags->integer == kFlagLink      ? Entry::Kind::kLink
                                                            : Entry::Kind::kOther;
            }
            out->push_back(std::move(e));
            return true;
        },
        &error);
    if (!ok) {
        return fail(why, "Manifest.db cannot be read: " + error);
    }
    if (too_many) {
        return fail(why, "Manifest.db lists over " + std::to_string(kMaxIosAppRows) +
                             " files for " + domain);
    }
    return true;
}

// Manifest.mbdb's records (docs/formats/ios-backup.md), every read bounds-checked.
class Mbdb {
  public:
    explicit Mbdb(const std::vector<u8> &b) : b_(b) {}

    bool take(std::size_t n, const std::string &what, const u8 **out) {
        if (n > b_.size() - pos_) {
            return fail(why_, "Manifest.mbdb is cut short: " + what + " at byte " +
                                  std::to_string(pos_) + " needs " + std::to_string(n) +
                                  " bytes, " + std::to_string(b_.size() - pos_) + " are left");
        }
        *out = b_.data() + pos_;
        pos_ += n;
        return true;
    }
    bool uint(std::size_t n, const std::string &what, std::uint64_t *v) {
        const u8 *p = nullptr;
        if (!take(n, what, &p)) {
            return false;
        }
        *v = 0;
        for (std::size_t i = 0; i < n; i++) {
            *v = (*v << 8) | p[i];
        }
        return true;
    }
    // A string (u16 length, 0xFFFF: none, read as empty).
    bool blob(const std::string &what, std::string *out) {
        std::uint64_t n = 0;
        if (!uint(2, "the length of " + what, &n)) {
            return false;
        }
        out->clear();
        if (n == 0xFFFF) {
            return true;
        }
        const u8 *p = nullptr;
        if (!take(static_cast<std::size_t>(n), what, &p)) {
            return false;
        }
        out->assign(reinterpret_cast<const char *>(p), static_cast<std::size_t>(n));
        return true;
    }

    bool entries(const std::string &domain, std::vector<Entry> *out, std::string *why) {
        why_ = why;
        const u8 *p = nullptr;
        if (!take(4, "the magic", &p) || std::memcmp(p, "mbdb", 4) != 0) {
            return fail(why, "Manifest.mbdb does not start with 'mbdb'");
        }
        if (!take(2, "the version", &p)) {
            return false;
        }
        if (!((p[0] == 5 || p[0] == 4) && p[1] == 0)) {
            return fail(why, "Manifest.mbdb has version " + std::to_string(p[0]) + "." +
                                 std::to_string(p[1]) + ", not 5.0 or 4.0");
        }
        std::size_t n = 0;
        while (pos_ < b_.size()) {
            const std::string rec = "record " + std::to_string(n++);
            std::string dom, path, link, hash, key, name, value;
            std::uint64_t mode = 0, skip = 0, size = 0, count = 0;
            if (!blob(rec + "'s domain", &dom) || !blob(rec + "'s path", &path) ||
                !blob(rec + "'s link target", &link) || !blob(rec + "'s data hash", &hash) ||
                !blob(rec + "'s encryption key", &key) || !uint(2, "mode", &mode) ||
                !uint(8, "inode", &skip) || !uint(4, "uid", &skip) || !uint(4, "gid", &skip) ||
                !uint(4, "a time", &skip) || !uint(4, "a time", &skip) ||
                !uint(4, "a time", &skip) || !uint(8, "size", &size) ||
                !uint(1, "protection class", &skip) || !uint(1, "property count", &count)) {
                return false;
            }
            for (std::uint64_t i = 0; i < count; i++) {
                if (!blob(rec + "'s property name", &name) ||
                    !blob(rec + "'s property '" + name + "'", &value)) {
                    return false;
                }
            }
            if (dom != domain) {
                continue;
            }
            if (out->size() >= kMaxIosAppRows) {
                return fail(why, "Manifest.mbdb lists over " + std::to_string(kMaxIosAppRows) +
                                     " files for " + domain);
            }
            Entry e;
            e.relative_path = std::move(path);
            e.file_id = ios_file_id(dom, e.relative_path);
            const u32 type = static_cast<u32>(mode) & kTypeMask;
            e.kind = type == kTypeFile        ? Entry::Kind::kFile
                     : type == kTypeDirectory ? Entry::Kind::kDirectory
                     : type == kTypeLink      ? Entry::Kind::kLink
                                              : Entry::Kind::kOther;
            e.size = size;
            e.encrypted = !key.empty();
            out->push_back(std::move(e));
        }
        return true;
    }

  private:
    const std::vector<u8> &b_;
    std::size_t pos_ = 0;
    std::string *why_ = nullptr;
};

u32 rotl(u32 v, int n) { return (v << n) | (v >> (32 - n)); }

} // namespace

std::array<std::uint8_t, 20> sha1_of(std::span<const std::uint8_t> bytes) {
    u32 h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::vector<u8> m(bytes.begin(), bytes.end());
    const std::uint64_t bits = static_cast<std::uint64_t>(bytes.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) {
        m.push_back(0);
    }
    for (int i = 7; i >= 0; i--) {
        m.push_back(static_cast<u8>(bits >> (8 * i)));
    }
    for (std::size_t off = 0; off < m.size(); off += 64) {
        u32 w[80];
        for (int i = 0; i < 16; i++) {
            const u8 *p = m.data() + off + 4 * i;
            w[i] = (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
        }
        for (int i = 16; i < 80; i++) {
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            u32 f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const u32 t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    std::array<u8, 20> out{};
    for (int i = 0; i < 20; i++) {
        out[static_cast<std::size_t>(i)] = static_cast<u8>(h[i / 4] >> (24 - 8 * (i % 4)));
    }
    return out;
}

std::string ios_file_id(std::string_view domain, std::string_view relative_path) {
    std::string text;
    text.reserve(domain.size() + 1 + relative_path.size());
    text.append(domain).append("-").append(relative_path);
    const auto digest =
        sha1_of(std::span<const u8>(reinterpret_cast<const u8 *>(text.data()), text.size()));
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (const u8 b : digest) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 15]);
    }
    return out;
}

std::string ios_backup_label(const IosBackupInfo &b) {
    std::string out = b.device.empty() ? std::string(tr(Txt::saves_ios_unknown_device)) : b.device;
    if (!b.ios.empty()) {
        out += ", iOS " + b.ios;
    }
    return out;
}

std::string ios_encrypted_text() {
    return "This backup is encrypted, and an encrypted backup's files cannot be read without "
           "its password. Make an unencrypted one: in Finder (or Apple Devices / iTunes) select "
           "the device, untick \"Encrypt local backup\", then click \"Back Up Now\" "
           "(docs/play/iphone-saves.md)";
}

bool looks_like_ios_backup(const std::string &dir) {
    const fs::path d = utf8_path(dir);
    return exists_file(d / "Manifest.db") || exists_file(d / "Manifest.mbdb") ||
           exists_file(d / "Manifest.plist");
}

IosBackupInfo ios_backup_info(const std::string &dir) {
    IosBackupInfo info;
    info.path = dir;
    const fs::path d = utf8_path(dir);
    std::string problem;
    if (auto manifest = read_plist(d / "Manifest.plist", "Manifest.plist", &problem)) {
        bool encrypted = false;
        info.encrypted = plist_bool(*manifest, "IsEncrypted", &encrypted) && encrypted;
    }
    if (auto plist = read_plist(d / "Info.plist", "Info.plist", &problem)) {
        plist_string(*plist, "Device Name", &info.device);
        plist_string(*plist, "Product Version", &info.ios);
    }
    if (auto status = read_plist(d / "Status.plist", "Status.plist", &problem)) {
        std::string state;
        info.unfinished = plist_string(*status, "SnapshotState", &state) && state != "finished";
    }
    fs::path manifest;
    if (exists_file(d / "Manifest.db")) {
        info.manifest = "db";
        manifest = d / "Manifest.db";
    } else if (exists_file(d / "Manifest.mbdb")) {
        info.manifest = "mbdb";
        manifest = d / "Manifest.mbdb";
    }
    if (!manifest.empty()) {
        std::error_code ec;
        const auto when = fs::last_write_time(manifest, ec);
        if (!ec) {
            // Not clock_cast: the file clock's epoch differs per library.
            const auto sys = std::chrono::system_clock::now() +
                             std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                 when - fs::file_time_type::clock::now());
            info.time =
                std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
        }
    }
    if (!problem.empty()) {
        info.problem = problem;
    } else if (info.manifest.empty()) {
        info.problem = is_old_format(d) ? "an iOS 3 backup (.mdinfo files): not supported"
                                        : "no Manifest.db or Manifest.mbdb: not an iOS backup";
    }
    return info;
}

std::vector<IosBackupInfo> find_ios_backups(const std::vector<std::string> &roots) {
    std::vector<IosBackupInfo> out;
    for (const std::string &root : roots) {
        if (root.empty()) {
            continue;
        }
        if (looks_like_ios_backup(root)) {
            out.push_back(ios_backup_info(root));
            continue;
        }
        std::vector<std::string> dirs;
        std::error_code ec;
        for (const fs::directory_entry &e : fs::directory_iterator(utf8_path(root), ec)) {
            std::error_code fec;
            if (e.is_symlink(fec) || !e.is_directory(fec)) {
                continue;
            }
            dirs.push_back(path_utf8(e.path()));
        }
        std::sort(dirs.begin(), dirs.end());
        for (const std::string &d : dirs) {
            if (looks_like_ios_backup(d)) {
                out.push_back(ios_backup_info(d));
            }
        }
    }
    return out;
}

std::vector<std::string> default_ios_backup_roots(const std::string &home,
                                                  const std::string &appdata) {
    std::vector<std::string> out;
#if defined(_WIN32)
    const char *h = home.empty() ? SDL_getenv("USERPROFILE") : home.c_str();
    const char *a = appdata.empty() ? SDL_getenv("APPDATA") : appdata.c_str();
    if (h != nullptr && *h != '\0') {
        out.push_back(path_utf8(utf8_path(h) / "Apple" / "MobileSync" / "Backup"));
    }
    if (a != nullptr && *a != '\0') {
        out.push_back(path_utf8(utf8_path(a) / "Apple Computer" / "MobileSync" / "Backup"));
    }
#elif defined(__APPLE__) && !defined(SDL_PLATFORM_IOS)
    (void)appdata;
    const char *h = home.empty() ? SDL_getenv("HOME") : home.c_str();
    if (h != nullptr && *h != '\0') {
        out.push_back(
            path_utf8(utf8_path(h) / "Library" / "Application Support" / "MobileSync" / "Backup"));
    }
#else
    (void)home;
    (void)appdata;
#endif
    return out;
}

bool read_ios_backup(const std::string &dir, IosBackupFiles *out, std::string *why) {
    *out = IosBackupFiles{};
    const fs::path d = utf8_path(dir);
    std::error_code ec;
    if (!fs::is_directory(d, ec)) {
        return fail(why, dir + " is not a folder");
    }
    if (!looks_like_ios_backup(dir)) {
        const std::vector<IosBackupInfo> found = find_ios_backups({dir});
        if (!found.empty()) {
            return fail(why, dir + " is a folder of backups, not a backup; choose one of its " +
                                 std::to_string(found.size()) + " folders");
        }
    }
    IosBackupInfo info = ios_backup_info(dir);
    if (info.encrypted) {
        return fail(why, ios_encrypted_text());
    }
    if (!info.problem.empty()) {
        return fail(why, info.problem);
    }
    const std::string domain = std::string("AppDomain-") + kIosAppId;
    std::vector<Entry> entries;
    if (info.manifest == "db") {
        if (!db_entries(d, domain, &entries, why)) {
            return false;
        }
    } else {
        std::vector<u8> mbdb;
        if (!read_capped(d / "Manifest.mbdb", kMaxIosMbdbBytes, "Manifest.mbdb", &mbdb, why) ||
            !Mbdb(mbdb).entries(domain, &entries, why)) {
            return false;
        }
    }
    out->info = info;
    if (info.unfinished) {
        out->warnings.emplace_back("Status.plist says this backup did not finish");
    }
    std::set<std::string> seen;
    for (const Entry &e : entries) {
        if (e.relative_path.rfind(kDocuments, 0) != 0 || e.kind == Entry::Kind::kDirectory) {
            continue;
        }
        const std::string name = e.relative_path.substr(kDocuments.size());
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\") != std::string::npos) {
            out->ignored.push_back(e.relative_path); // the game keeps no subfolders
            continue;
        }
        if (mt_saves_is_game_file(name.c_str()) == 0) {
            if (name.size() < 7 || name.substr(name.size() - 7) != MT_SAVES_TMP_SUFFIX) {
                out->ignored.push_back(name);
            }
            continue;
        }
        if (e.kind != Entry::Kind::kFile) {
            return fail(why, "Documents/" + name + " is a " + kind_text(e.kind) +
                                 " in the backup, not a file");
        }
        if (e.encrypted) {
            return fail(why, ios_encrypted_text());
        }
        if (!seen.insert(name).second) {
            return fail(why, "the backup lists Documents/" + name + " twice");
        }
        if (!is_file_id(e.file_id)) {
            return fail(why, "Documents/" + name + " has a malformed file ID");
        }
        const fs::path where = info.manifest == "db"
                                   ? d / e.file_id.substr(0, 2) / utf8_path(e.file_id)
                                   : d / utf8_path(e.file_id);
        if (info.manifest == "db" && fs::is_symlink(where.parent_path(), ec)) {
            return fail(why, path_utf8(where.parent_path()) + " is a link; a backup never "
                                                              "holds one");
        }
        SaveFile f;
        f.name = name;
        if (!read_capped(where, kMaxSaveSourceFileBytes,
                         "Documents/" + name + "'s data (file " + e.file_id + ")", &f.bytes, why)) {
            return false;
        }
        if (e.size && *e.size != f.bytes.size()) {
            return fail(why, "Documents/" + name + " has " + std::to_string(f.bytes.size()) +
                                 " bytes, the manifest says " + std::to_string(*e.size));
        }
        out->files.push_back(std::move(f));
    }
    if (out->files.empty()) {
        if (entries.empty()) {
            return fail(why, "this backup has no files of MapleStory Thief Edition (" + domain +
                                 "); the game must still be installed on the device when it "
                                 "is backed up");
        }
        return fail(why, "this backup has no saves of the game in " + domain + "'s Documents/");
    }
    std::sort(out->files.begin(), out->files.end(),
              [](const SaveFile &a, const SaveFile &b) { return a.name < b.name; });
    std::sort(out->ignored.begin(), out->ignored.end());
    return true;
}

} // namespace maplethief::frontend
