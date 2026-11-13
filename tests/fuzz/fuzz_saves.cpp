// Fuzz target: the save files' checks and sanitiser (runtime/src/foundation/saves.cpp,
// docs/formats/save.md, #323, ADR 0154) and the cash shop's counters file
// (mt_cash_counters_parse). The first byte picks the file name (its low 5 bits) and the size of
// the reason's buffer (its top 3 bits); the rest is the file. Oracles besides the sanitizers:
// the reason is NUL-terminated inside its buffer and a refusal has one; the verdict has a name;
// the verdict does not depend on that buffer; the sanitiser changes only config, at most two
// bytes, never one the checks pass, and a second pass changes nothing.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "fuzz2.h"
#include "maplethief/cashshop.h"
#include "maplethief/saves.h"

namespace {

using mt_fuzz2::Bytes;

const char *const k_names[] = {
    "theif0",
    "theif1",
    "theif2",
    "quest0",
    "quest1",
    "quest2",
    "config",
    "cupon",
    "CTerms",
    "crestore",
    "riprestore1",
    "riprestore2",
    "totalrestore",
    "maplethief-cashshop0",
    "maplethief-cashshop1",
    "maplethief-cashshop2",
    "thief0",
    "theif3",
    "",
};
constexpr unsigned k_name_count = sizeof k_names / sizeof k_names[0];
const std::size_t k_why_sizes[8] = {0, 1, 2, 8, 24, 64, 160, 512};

std::uint8_t selector(const char *name, unsigned why) {
    for (unsigned i = 0; i < k_name_count; i++) {
        if (std::strcmp(k_names[i], name) == 0) {
            return static_cast<std::uint8_t>(i | why << 5);
        }
    }
    return 0;
}

mt_fuzz2::Seed seed(const char *name, const std::string &bytes, unsigned why = 6) {
    Bytes b{selector(name, why)};
    b.insert(b.end(), bytes.begin(), bytes.end());
    return mt_fuzz2::Seed{
        std::string(name[0] != '\0' ? name : "noname") + "-" + std::to_string(why), b};
}

// docs/formats/save.md, "A new character" (the bytes saves_test.cpp checks).
std::string new_character() {
    std::string b(0x464, '\0');
    b[2] = 1;
    for (std::size_t i = 0; i < 48; i++) {
        b[0x015 + 16 * i] = '\xFF';
        b[0x325 + 3 * i] = '\xFF';
    }
    b[0x325] = 3, b[0x326] = 5, b[0x328] = 6, b[0x329] = 5;
    for (std::size_t i = 0; i < 5; i++) {
        b[0x3C2 + 16 * i] = '\xFF';
    }
    b[0x3C2 + 16] = 0x48;
    b[0x41C] = 2, b[0x41D] = 8;
    return b;
}

// Ten quest records, the first one active with empty keys and lists.
std::string quests() {
    std::string q(0x12AD, '\0');
    for (std::size_t r = 0; r < 10; r++) {
        q[0xC9 + 0x1C9 * r] = '\xFF';
    }
    const std::size_t rec = 0xC9;
    for (std::size_t k = 0; k < 5; k++) {
        q[rec + 0x143 + 14 * k] = '\xFF';
        q[rec + 0x189 + 13 * k] = '\xFF';
    }
    q[rec] = 0;
    return q;
}

} // namespace

void mt_fuzz2::init() {}

std::vector<mt_fuzz2::Seed> mt_fuzz2::seeds() {
    mt_cash_counters c{};
    mt_cash_counters_reset(&c, 12);
    mt_cash_counters_death(&c);
    mt_cash_counters_bought(&c, 0);
    char text[1024];
    const std::size_t n = mt_cash_counters_format(&c, text, sizeof text);
    return {
        seed("theif0", new_character()),
        seed("theif2", new_character(), 3),
        seed("quest1", quests()),
        seed("config", std::string("\x03\x01\x00\x01\x01\x01", 6)),
        seed("config", std::string("\xFF\x07\x09\x09\x09\x05", 6), 1),
        seed("cupon", std::string(0x174A, '\0')),
        seed("CTerms", std::string(2, '\0')),
        seed("crestore", std::string("\x02\x00\x01\x03\x00"
                                     "abc"
                                     "\x02\x01\x00"
                                     "z",
                                     13)),
        seed("totalrestore", std::string("\x00\x00", 2)),
        seed("maplethief-cashshop1", std::string(text, n)),
        seed("maplethief-cashshop0", "# counters\r\nlevel = 7\n\tdeaths=4294967295  \n"),
        seed("theif3", "x", 0),
        seed("", "", 2),
    };
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const char *name = k_names[(data[0] & 0x1Fu) % k_name_count];
    const std::size_t why_size = k_why_sizes[data[0] >> 5];
    // The file in its own allocation, so that ASan sees a read past its end.
    const Bytes file(data + 1, data + size);
    std::vector<char> why(why_size + 16, '\x7F');
    const mt_saves_verdict v = mt_saves_check(name, file.data(), file.size(),
                                              why_size > 0 ? why.data() : nullptr, why_size);
    if (std::strcmp(mt_saves_verdict_name(v), "?") == 0) {
        mt_fuzz2::broken("a verdict without a name");
    }
    if (why_size > 0) {
        const void *nul = std::memchr(why.data(), '\0', why_size);
        if (nul == nullptr) {
            mt_fuzz2::broken("the reason is not NUL-terminated inside its buffer");
        }
        if (why[why_size] != '\x7F') {
            mt_fuzz2::broken("the reason was written past its buffer");
        }
        if (v != MT_SAVES_OK && why_size > 1 && why[0] == '\0') {
            mt_fuzz2::broken("a refused save without a reason");
        }
    }
    if ((v == MT_SAVES_OK) !=
        (mt_saves_check(name, file.data(), file.size(), nullptr, 0) == MT_SAVES_OK)) {
        mt_fuzz2::broken("the verdict depends on the reason's buffer");
    }

    Bytes copy = file;
    const std::uint32_t changed = mt_saves_sanitise(name, copy.data(), copy.size());
    if (changed > 2 || (changed > 0 && std::strcmp(name, "config") != 0)) {
        mt_fuzz2::broken("the sanitiser changed more than config's two bytes");
    }
    if (mt_saves_sanitise(name, copy.data(), copy.size()) != 0) {
        mt_fuzz2::broken("a sanitised file changes again");
    }
    if (std::strcmp(name, "config") == 0 && v == MT_SAVES_OK && changed != 0) {
        mt_fuzz2::broken("the sanitiser changed a config the checks pass");
    }

    if (std::strncmp(name, MT_SAVES_COUNTERS_PREFIX, std::strlen(MT_SAVES_COUNTERS_PREFIX)) == 0) {
        const std::string text(file.begin(), file.end());
        mt_cash_counters c{};
        const int rc = mt_cash_counters_parse(text.data(), text.size(), &c);
        (void)rc;
    }
    return 0;
}
