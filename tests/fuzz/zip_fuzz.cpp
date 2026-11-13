// The save import's zip reader (platform/src/save_zip.cpp, #148, #324): a zip a player picks.
// zip_read in both forms, zip_list + zip_extract as the IPA import calls them, and the round
// trip: what zip_read gives back, stored by zip_store and read again, is the same.
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "save_zip.h"

namespace {

namespace mf = maplethief::frontend;

// Small limits, so that an input claiming large entries is refused rather than inflated.
constexpr std::size_t kMaxEntry = 1u << 20;
constexpr std::size_t kMaxTotal = 2u << 20;

void round_trip(const std::vector<mf::ZipEntry> &entries) {
    std::set<std::string> names;
    for (const mf::ZipEntry &e : entries) {
        if (!names.insert(e.name).second) {
            return; // zip_store wants unique names
        }
    }
    const std::vector<std::uint8_t> again = mf::zip_store(entries);
    std::vector<mf::ZipEntry> back;
    std::string why;
    if (!mf::zip_read(again, kMaxEntry, &back, &why) || back.size() != entries.size()) {
        __builtin_trap();
    }
    for (std::size_t i = 0; i < back.size(); ++i) {
        if (back[i].name != entries[i].name || back[i].bytes != entries[i].bytes) {
            __builtin_trap();
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> zip(data, size);
    std::vector<mf::ZipEntry> entries;
    std::string why;
    if (mf::zip_read(zip, kMaxEntry, &entries, &why)) {
        round_trip(entries);
    }

    // The save import's form: only some names kept, a total limit.
    entries.clear();
    const auto keep = [](const std::string &name) { return name.size() % 2 == 0; };
    (void)mf::zip_read(zip, kMaxEntry, kMaxTotal, keep, &entries, &why);

    // The IPA import's: the central directory, then each entry.
    std::vector<mf::ZipItem> items;
    if (mf::zip_list(zip, &items, &why)) {
        std::vector<std::uint8_t> bytes;
        for (const mf::ZipItem &item : items) {
            if (mf::zip_extract(zip, item, kMaxEntry, &bytes, &why) && bytes.size() != item.size) {
                __builtin_trap();
            }
        }
    }
    return 0;
}
