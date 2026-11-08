// INTERIM (#263): replaced by #262's data_notice; see data_notice.h.
#include "data_notice.h"

#include <SDL3/SDL.h>

#include <utility>

namespace maplethief::frontend {

namespace {
std::vector<std::string> &queue() {
    static std::vector<std::string> q;
    return q;
}
} // namespace

void queue_data_notice(std::string text) {
    SDL_Log("data: notice: %s", text.c_str());
    queue().push_back(std::move(text));
}

std::vector<std::string> take_data_notices() {
    std::vector<std::string> out;
    out.swap(queue());
    return out;
}

} // namespace maplethief::frontend
