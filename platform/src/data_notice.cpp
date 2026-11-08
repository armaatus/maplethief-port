// The data folder's one-time notices (#262, ADR 0127); see data_notice.h.
#include "data_notice.h"

#include <SDL3/SDL.h>

#include <mutex>
#include <utility>

namespace maplethief::frontend {

namespace {

std::mutex &queue_mutex() {
    static std::mutex m;
    return m;
}
std::vector<std::string> &queue() {
    static std::vector<std::string> q;
    return q;
}

} // namespace

void queue_data_notice(std::string text) {
    if (text.empty()) {
        return;
    }
    SDL_Log("data: %s", text.c_str());
    const std::lock_guard<std::mutex> lock(queue_mutex());
    queue().push_back(std::move(text));
}

std::vector<std::string> take_data_notices() {
    const std::lock_guard<std::mutex> lock(queue_mutex());
    std::vector<std::string> out;
    out.swap(queue());
    return out;
}

std::vector<std::string> wrap_notice(const std::string &text, std::size_t columns) {
    std::vector<std::string> lines;
    if (columns == 0) {
        columns = 1;
    }
    std::string line;
    std::size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') {
            i++;
            continue;
        }
        std::size_t end = text.find(' ', i);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string word = text.substr(i, end - i);
        i = end;
        // A word that fits on the line, or on a line of its own.
        if (!line.empty() && line.size() + 1 + word.size() <= columns) {
            line += ' ' + word;
            continue;
        }
        if (!line.empty()) {
            lines.push_back(line);
            line.clear();
        }
        // Longer than a line: broken at the edge, after its last separator before it if any.
        while (word.size() > columns) {
            std::size_t cut = columns;
            const std::size_t sep = word.find_last_of("/\\", columns - 1);
            if (sep != std::string::npos && sep > 0) {
                cut = sep + 1;
            }
            lines.push_back(word.substr(0, cut));
            word.erase(0, cut);
        }
        line = word;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    return lines;
}

} // namespace maplethief::frontend
