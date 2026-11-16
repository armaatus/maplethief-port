// A folder's state for the pack tests (texpack, audio.pack, textpack): every entry below it with
// its type, size and write time, so a test can check a refused file left the folder as it was
// (#413). Sizes and times, not contents: a refused file may be a sparse one of 80 MiB.
#ifndef MAPLETHIEF_TESTS_DIR_STATE_H
#define MAPLETHIEF_TESTS_DIR_STATE_H

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

inline std::vector<std::string> dir_state(const std::filesystem::path &dir) {
    namespace fs = std::filesystem;
    std::vector<std::string> entries;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::file_status st = it->symlink_status();
        std::string line = it->path().lexically_relative(dir).generic_string();
        line += ' ' + std::to_string(static_cast<int>(st.type()));
        if (fs::is_regular_file(st)) {
            std::error_code ec2;
            line += ' ' + std::to_string(fs::file_size(it->path(), ec2));
            line += ' ' + std::to_string(static_cast<long long>(
                              fs::last_write_time(it->path(), ec2).time_since_epoch().count()));
        }
        entries.push_back(line);
    }
    std::sort(entries.begin(), entries.end());
    return entries;
}

#endif
