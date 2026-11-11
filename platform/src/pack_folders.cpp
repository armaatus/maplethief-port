// The pack folders (#241, ADR 0142); see pack_folders.h.
#include "pack_folders.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <system_error>

#include "settings.h"

namespace maplethief::frontend {

namespace {

namespace fs = std::filesystem;

fs::path utf8_path(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string path_utf8(const fs::path &p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool ends_with_ci(std::string_view s, std::string_view end) {
    if (s.size() < end.size()) {
        return false;
    }
    for (std::size_t i = 0; i < end.size(); i++) {
        const auto a = static_cast<unsigned char>(s[s.size() - end.size() + i]);
        const auto b = static_cast<unsigned char>(end[i]);
        if (std::tolower(a) != std::tolower(b)) {
            return false;
        }
    }
    return true;
}

} // namespace

const PackFolder *find_pack_folder(std::string_view key) {
    for (const PackFolder &f : kPackFolders) {
        if (key == f.key) {
            return &f;
        }
    }
    return nullptr;
}

std::vector<std::string> list_pack_folder(const std::string &dir, const PackFolder &f) {
    std::vector<std::string> names;
    if (dir.empty()) {
        return names;
    }
    std::error_code ec;
    for (const fs::directory_entry &e : fs::directory_iterator(utf8_path(dir), ec)) {
        const std::string name = path_utf8(e.path().filename());
        if (name.empty() || name.front() == '.') {
            continue;
        }
        std::error_code fec;
        const bool ok = f.ext == nullptr ? e.is_directory(fec)
                                         : e.is_regular_file(fec) && ends_with_ci(name, f.ext);
        if (ok) {
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    std::string prefix = dir;
    if (prefix.back() != '/') {
        prefix += '/';
    }
    for (std::string &n : names) {
        n = prefix + n;
    }
    return names;
}

void offer_pack_folders(const std::string &base) {
    for (const PackFolder &f : kPackFolders) {
        std::vector<std::string> values;
        if (!base.empty()) {
            const std::string dir = base + (base.back() == '/' ? "" : "/") + f.folder;
            std::error_code ec;
            fs::create_directories(utf8_path(dir), ec);
            values = list_pack_folder(dir, f);
        }
        set_pack_choices(f.key, std::move(values));
    }
}

std::string pack_value_text(std::string_view value, const char *none) {
    if (value.empty()) {
        return none;
    }
    std::string_view v = value;
    while (v.size() > 1 && (v.back() == '/' || v.back() == '\\')) {
        v.remove_suffix(1);
    }
    const std::size_t cut = v.find_last_of("/\\");
    return std::string(cut == std::string_view::npos ? v : v.substr(cut + 1));
}

} // namespace maplethief::frontend
