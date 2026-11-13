// The first-launch IPA import (platform/src/ipa_import.cpp, #182): a file the player picks.
// check_ipa_bytes against the 1.5 IPA (the input is never it, so the check looks into the zip
// for the reason), then import_ipa_bytes with the input's own hashes expected, so that the
// safe-extraction rules and the extraction into a scratch folder run too. Whatever happens,
// the folder holds game/ after a good import and nothing at all otherwise.
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "ipa_import.h"
#include "save_zip.h"

namespace {

namespace fs = std::filesystem;
namespace mf = maplethief::frontend;

// The executable's SHA-256 if the input has one where the import looks for it, so that a good
// import can happen; zeros otherwise.
mf::Sha256 executable_sha(std::span<const std::uint8_t> zip) {
    std::vector<mf::ZipItem> items;
    std::string why;
    if (!mf::zip_list(zip, &items, &why)) {
        return {};
    }
    for (const mf::ZipItem &item : items) {
        const std::string &n = item.name;
        const std::string tail = ".app/MapleThief";
        if (n.rfind("Payload/", 0) == 0 && n.size() > tail.size() &&
            n.compare(n.size() - tail.size(), tail.size(), tail) == 0) {
            std::vector<std::uint8_t> bytes;
            if (mf::zip_extract(zip, item, 1u << 20, &bytes, &why)) {
                return mf::sha256_of(bytes);
            }
        }
    }
    return {};
}

const fs::path &scratch() {
    static const fs::path dir =
        fs::temp_directory_path() / ("maplethief-fuzz-ipa-" + std::to_string(::getpid()));
    return dir;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    const std::span<const std::uint8_t> ipa(data, size);
    (void)mf::check_ipa_bytes(ipa, mf::ipa_expected());

    mf::IpaExpected expected;
    expected.size = size;
    expected.ipa_sha256 = mf::sha256_of(ipa);
    expected.executable_sha256 = executable_sha(ipa);
    // Some inputs stop the import half-way: it must leave nothing behind either.
    const bool stop = size % 5 == 0;
    const mf::IpaProgress progress = [stop](std::uint64_t done, std::uint64_t total) {
        return !(stop && done * 2 >= total);
    };
    std::error_code ec;
    fs::remove_all(scratch(), ec);
    const mf::IpaResult r = mf::import_ipa_bytes(ipa, scratch().string(), expected, progress);
    for (fs::directory_iterator it(scratch(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!r.ok() || it->path().filename() != "game") {
            __builtin_trap(); // a leftover staging folder, or a file outside game/
        }
    }
    fs::remove_all(scratch(), ec);
    return 0;
}
