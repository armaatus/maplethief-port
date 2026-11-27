// Adding a pack from anywhere on a desktop (#422, ADR 0149, 0154); see pack_add.h.
#include "pack_add.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#include "ipa_import.h"
#include "maplethief/audio.h"
#include "maplethief/pack_manifest.h"
#include "maplethief/texpack.h"
#include "maplethief/textpack.h"
#include "port_text.h"
#include "save_zip.h"

namespace maplethief::frontend {

namespace {

namespace fs = std::filesystem;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string path_utf8(const fs::path &p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string lowered(std::string s) {
    for (char &c : s) {
        c = c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return s;
}

bool ends_with(const std::string &s, std::string_view tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// Two paths name one folder: the same text without trailing separators.
std::string trimmed(std::string_view p) {
    while (p.size() > 1 && (p.back() == '/' || p.back() == '\\')) {
        p.remove_suffix(1);
    }
    return std::string(p);
}

std::optional<PackKind> kind_named(const char *name) {
    for (const PackKind k : kPackKinds) {
        if (std::string_view(name) == pack_kind_name(k)) {
            return k;
        }
    }
    return std::nullopt;
}

// A pack's kind from the files at its top: pack.strings's bytes (`manifest`, nullptr: none),
// whether it has a textpack.strings, and the lower-cased names of its files anywhere below.
std::optional<PackKind> kind_of(const std::vector<std::uint8_t> *manifest, bool textpack,
                                const std::vector<std::string> &files, std::string *why) {
    if (manifest != nullptr) {
        char kind[8] = {};
        if (mt_pack_manifest_kind(manifest->data(), manifest->size(), kind) == 0) {
            *why = "its pack.strings names no kind (texture, audio or text)";
            return std::nullopt;
        }
        return kind_named(kind);
    }
    if (textpack) {
        return PackKind::kText;
    }
    bool png = false, sound = false;
    for (const std::string &f : files) {
        png = png || ends_with(f, ".png");
        sound = sound || ends_with(f, ".wav") || ends_with(f, ".mp3") || ends_with(f, ".flac");
    }
    if (png != sound) {
        return png ? PackKind::kTexture : PackKind::kAudio;
    }
    *why = "its kind is not known: give it a pack.strings with its kind";
    return std::nullopt;
}

// A kind's files at most, and bytes per file at most, by PackKind.
constexpr std::uint64_t kMaxFiles[] = {MT_TEXPACK_MAX_FILES, MT_AUDIO_PACK_MAX_FILES,
                                       kMaxZipEntries};
constexpr std::uint64_t kMaxFileBytes[] = {MT_TEXPACK_MAX_FILE, MT_AUDIO_PACK_MAX_FILE,
                                           MT_TEXTPACK_MAX_FILE};
std::uint64_t max_files(PackKind kind) { return kMaxFiles[static_cast<std::size_t>(kind)]; }
std::uint64_t max_file_bytes(PackKind kind) {
    return kMaxFileBytes[static_cast<std::size_t>(kind)];
}

// "1 GiB", "4 MiB", "64 bytes".
std::string mib(std::uint64_t bytes) {
    if (bytes != 0 && bytes % (std::uint64_t{1} << 30) == 0) {
        return std::to_string(bytes >> 30) + " GiB";
    }
    if (bytes != 0 && bytes % (std::uint64_t{1} << 20) == 0) {
        return std::to_string(bytes >> 20) + " MiB";
    }
    return std::to_string(bytes) + " bytes";
}

PackAdd refuse(std::string why) {
    PackAdd a;
    a.why = std::move(why);
    return a;
}

// A Finder zip's extras, left out.
bool finder_extra(const std::vector<std::string> &parts) {
    return parts.front() == "__MACOSX" || parts.back() == ".DS_Store";
}

// One file of the zip to write: its entry and its place below the pack's folder.
struct ZipFile {
    const ZipItem *item;
    std::string rel; // '/' separated
};

// A name for a staging folder no other add of this process or a concurrent one uses.
std::string staging_name(const std::string &name) {
    static std::mt19937_64 rng{
        std::random_device{}() ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    return ".adding-" + name + "-" + std::to_string(rng() % 1000000000u);
}

} // namespace

std::optional<PackKind> pack_folder_kind(const std::string &dir, std::string *why) {
    std::error_code ec;
    const fs::path root = utf8_path(dir);
    if (!fs::is_directory(root, ec)) {
        *why = "it is not a folder";
        return std::nullopt;
    }
    std::optional<std::vector<std::uint8_t>> manifest;
    bool textpack = false;
    for (const fs::directory_entry &e : fs::directory_iterator(root, ec)) {
        std::error_code fe;
        if (!fs::is_regular_file(e.symlink_status(fe)) || fe) {
            continue;
        }
        const std::string name = lowered(path_utf8(e.path().filename()));
        if (name == "pack.strings") {
            const std::uintmax_t size = e.file_size(fe);
            if (fe || size > MT_TEXTPACK_MAX_FILE) {
                *why = "its pack.strings cannot be read";
                return std::nullopt;
            }
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            std::ifstream in(e.path(), std::ios::binary);
            if (!in || (size > 0 && !in.read(reinterpret_cast<char *>(bytes.data()),
                                             static_cast<std::streamsize>(size)))) {
                *why = "its pack.strings cannot be read";
                return std::nullopt;
            }
            manifest = std::move(bytes);
        } else if (name == "textpack.strings") {
            textpack = true;
        }
    }
    // Without a manifest, the files below tell (a texture pack's ui/ folder too); a bounded walk.
    std::vector<std::string> files;
    if (!manifest && !textpack) {
        for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (files.size() >= 4096) {
                break;
            }
            std::error_code fe;
            if (fs::is_regular_file(it->symlink_status(fe))) {
                files.push_back(lowered(path_utf8(it->path().filename())));
            }
        }
    }
    return kind_of(manifest ? &*manifest : nullptr, textpack, files, why);
}

PackAdd check_pack_folder(const std::string &dir, const std::string &port_version) {
    std::string why;
    const std::optional<PackKind> kind = pack_folder_kind(dir, &why);
    if (!kind) {
        return refuse(why);
    }
    const PackInfo info = read_pack(trimmed(dir), *kind, port_version);
    PackAdd a;
    a.kind = *kind;
    a.path = info.path;
    a.title = pack_title(info);
    a.why = info.refused;
    a.ok = info.refused.empty();
    return a;
}

PackAdd unpack_pack_zip(std::span<const std::uint8_t> zip, const std::string &zip_name,
                        const std::string &base, const std::string &port_version,
                        std::uint64_t max_total) {
    if (base.empty()) {
        return refuse("there is no folder for packs on this device");
    }
    if (zip.size() > max_total) {
        return refuse("the zip is larger than " + mib(max_total));
    }
    std::vector<ZipItem> items;
    std::string why;
    if (!zip_list(zip, &items, &why)) {
        return refuse(why);
    }
    // Every entry's name and type first (ADR 0149): one bad entry refuses the whole zip.
    std::map<std::string, std::string> seen; // lower-cased path -> entry name
    std::set<std::string> folders;           // lower-cased folders every entry implies
    std::vector<std::pair<const ZipItem *, std::vector<std::string>>> kept;
    for (const ZipItem &item : items) {
        std::vector<std::string> parts;
        if (!ipa_entry_parts(item.name, &parts, &why)) {
            return refuse("its entry \"" + item.name + "\" is not safe (" + why + ")");
        }
        const std::uint32_t mode = (item.external >> 16) & 0170000u;
        if (mode == 0120000u) {
            return refuse("its entry \"" + item.name + "\" is a link");
        }
        if (mode != 0 && mode != 0100000u && mode != 0040000u) {
            return refuse("its entry \"" + item.name + "\" is not a file or folder");
        }
        if (item.encrypted()) {
            return refuse("its entry \"" + item.name + "\" is encrypted");
        }
        if (parts.empty() || finder_extra(parts)) {
            continue;
        }
        std::string key;
        for (std::size_t i = 0; i < parts.size(); i++) {
            key += (i == 0 ? "" : "/") + lowered(parts[i]);
            if (i + 1 < parts.size()) {
                folders.insert(key);
            }
        }
        if (const auto [at, fresh] = seen.emplace(key, item.name); !fresh) {
            return refuse("its entries \"" + at->second + "\" and \"" + item.name +
                          "\" name one file");
        }
        if (item.is_dir()) {
            folders.insert(key);
        } else {
            kept.emplace_back(&item, std::move(parts));
        }
    }
    for (const auto &[item, parts] : kept) {
        std::string key;
        for (std::size_t i = 0; i < parts.size(); i++) {
            key += (i == 0 ? "" : "/") + lowered(parts[i]);
        }
        if (folders.count(key) != 0) {
            return refuse("its entry \"" + item->name + "\" is both a file and a folder");
        }
    }
    if (kept.empty()) {
        return refuse("the zip holds no files");
    }
    // The pack's folder: the zip's one top folder, or the zip itself.
    std::string name;
    std::size_t strip = 0;
    const std::string &top = kept.front().second.front();
    const bool one_top = std::all_of(kept.begin(), kept.end(), [&top](const auto &k) {
        return k.second.size() > 1 && k.second.front() == top;
    });
    if (one_top) {
        name = top;
        strip = 1;
    } else {
        name = path_utf8(utf8_path(zip_name).filename());
        if (ends_with(lowered(name), ".zip")) {
            name.resize(name.size() - 4);
        }
    }
    std::vector<std::string> name_parts;
    if (name.empty() || !ipa_entry_parts(name, &name_parts, &why) || name_parts.size() != 1 ||
        name.find('|') != std::string::npos || name.front() == '.') {
        return refuse("\"" + name + "\" cannot name a pack folder");
    }
    std::vector<ZipFile> files;
    std::vector<std::string> names;
    const std::vector<std::uint8_t> *manifest_at = nullptr;
    std::vector<std::uint8_t> manifest;
    const ZipItem *manifest_item = nullptr;
    bool textpack = false;
    for (const auto &[item, parts] : kept) {
        std::string rel;
        for (std::size_t i = strip; i < parts.size(); i++) {
            rel += (i == strip ? "" : "/") + parts[i];
        }
        names.push_back(lowered(parts.back()));
        if (parts.size() == strip + 1) {
            const std::string low = lowered(parts.back());
            manifest_item = low == "pack.strings" ? item : manifest_item;
            textpack = textpack || low == "textpack.strings";
        }
        files.push_back(ZipFile{item, rel});
    }
    if (manifest_item != nullptr) {
        if (!zip_extract(zip, *manifest_item, MT_TEXTPACK_MAX_FILE, &manifest, &why)) {
            return refuse("its pack.strings: " + why);
        }
        manifest_at = &manifest;
    }
    const std::optional<PackKind> kind = kind_of(manifest_at, textpack, names, &why);
    if (!kind) {
        return refuse(why);
    }
    // The kind's limits and 1 GiB in all, from the central directory, before anything is written.
    if (files.size() > max_files(*kind)) {
        return refuse("it has " + std::to_string(files.size()) + " files; a " +
                      pack_kind_name(*kind) + " pack has at most " +
                      std::to_string(max_files(*kind)));
    }
    std::uint64_t total = 0;
    for (const ZipFile &f : files) {
        if (f.item->size > max_file_bytes(*kind)) {
            return refuse("its file \"" + f.rel + "\" is larger than " +
                          mib(max_file_bytes(*kind)));
        }
        total += f.item->size;
        if (total > max_total) {
            return refuse("its files hold more than " + mib(max_total));
        }
    }
    const fs::path folder = utf8_path(base) / pack_kind_folder(*kind);
    const fs::path dest = folder / utf8_path(name);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(dest, ec))) {
        return refuse("a pack folder named \"" + name + "\" is there already");
    }
    // Unpacked into a staging folder, checked as a pack, then renamed into place.
    const fs::path staging = folder / utf8_path(staging_name(name));
    const auto fail = [&staging](std::string reason) {
        std::error_code rc;
        fs::remove_all(staging, rc);
        return refuse(std::move(reason));
    };
    fs::create_directories(staging, ec);
    if (ec) {
        return fail("its folder cannot be made (" + ec.message() + ")");
    }
    for (const ZipFile &f : files) {
        std::vector<std::uint8_t> bytes;
        try {
            if (!zip_extract(zip, *f.item, max_file_bytes(*kind), &bytes, &why)) {
                return fail("its file \"" + f.rel + "\": " + why);
            }
        } catch (const std::bad_alloc &) {
            return fail("its file \"" + f.rel + "\" does not fit in memory");
        }
        const fs::path out = staging / utf8_path(f.rel);
        fs::create_directories(out.parent_path(), ec);
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        if (ec || !o ||
            !o.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
            return fail("its file \"" + f.rel + "\" cannot be written");
        }
    }
    const PackInfo staged = read_pack(path_utf8(staging), *kind, port_version);
    if (!staged.refused.empty()) {
        return fail(staged.refused);
    }
    fs::rename(staging, dest, ec);
    if (ec) {
        return fail("it cannot be moved into place (" + ec.message() + ")");
    }
    // Its path as the packs screen lists it (PacksScreen::scan, list_pack_folder).
    const std::string listed =
        base + (base.back() == '/' ? "" : "/") + pack_kind_folder(*kind) + "/" + name;
    const PackInfo info = read_pack(listed, *kind, port_version);
    PackAdd a;
    a.ok = true;
    a.kind = *kind;
    a.path = info.path;
    a.title = pack_title(info);
    return a;
}

PackAdd unpack_pack_zip_file(const std::string &path, const std::string &base,
                             const std::string &port_version, std::uint64_t max_total) {
    std::error_code ec;
    const fs::path p = utf8_path(path);
    if (!fs::is_regular_file(fs::symlink_status(p, ec))) {
        return refuse("it is not a file");
    }
    const std::uintmax_t size = fs::file_size(p, ec);
    if (ec) {
        return refuse("it cannot be read");
    }
    if (size > max_total) {
        return refuse("the zip is larger than " + mib(max_total));
    }
    std::vector<std::uint8_t> bytes;
    try {
        bytes.resize(static_cast<std::size_t>(size));
    } catch (const std::bad_alloc &) {
        return refuse("it does not fit in memory");
    }
    std::ifstream in(p, std::ios::binary);
    if (!in || (size > 0 && !in.read(reinterpret_cast<char *>(bytes.data()),
                                     static_cast<std::streamsize>(size)))) {
        return refuse("it cannot be read");
    }
    return unpack_pack_zip(bytes, path_utf8(p.filename()), base, port_version, max_total);
}

PackAddChange add_pack_path(const std::string &path, const Settings &s, const std::string &base,
                            const std::string &port_version) {
    PackAddChange c;
    const std::string shown = path_utf8(utf8_path(trimmed(path)).filename());
    std::error_code ec;
    const fs::path p = utf8_path(path);
    if (!fs::is_directory(p, ec) && ends_with(lowered(path), ".zip")) {
        c.added = unpack_pack_zip_file(path, base, port_version);
        if (!c.added.ok) {
            c.status = tr(Txt::packs_add_refused, {shown, c.added.why});
            return c;
        }
        c.status = tr(Txt::packs_unpacked,
                      {c.added.title, std::string(pack_kind_folder(c.added.kind)) + "/"});
        return c;
    }
    c.added = check_pack_folder(path, port_version);
    if (!c.added.ok) {
        c.status = tr(Txt::packs_add_refused,
                      {c.added.title.empty() ? shown : c.added.title, c.added.why});
        return c;
    }
    std::vector<std::string> on = split_pack_list(pack_kind_value(s, c.added.kind));
    const bool listed = std::any_of(on.begin(), on.end(), [&c](const std::string &o) {
        return trimmed(o) == trimmed(c.added.path);
    });
    if (listed) {
        c.status = tr(Txt::packs_add_listed, {c.added.title});
        return c;
    }
    // Its list's own rules (a second id, another language, the 17th) as the start applies them.
    on.push_back(c.added.path);
    const std::vector<PackInfo> checked = check_pack_list(on, c.added.kind, port_version);
    if (!checked.back().refused.empty()) {
        c.added.ok = false;
        c.added.why = checked.back().refused;
        c.status = tr(Txt::packs_add_refused, {c.added.title, c.added.why});
        return c;
    }
    c.key = pack_kind_key(c.added.kind);
    c.value = join_pack_list(on);
    c.status = tr(Txt::packs_added_on, {c.added.title, std::to_string(on.size())});
    return c;
}

} // namespace maplethief::frontend
