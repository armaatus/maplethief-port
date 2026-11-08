// Synthetic save files for the platform tests (save_tools_test.cpp, data_copy_test.cpp): a new
// character and its quests as the game builds them (docs/formats/save.md), the config, and a
// valid set of two characters with the other files.
#ifndef MAPLETHIEF_TESTS_SAVE_FIXTURES_H
#define MAPLETHIEF_TESTS_SAVE_FIXTURES_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace save_fixtures {

using Bytes = std::vector<std::uint8_t>;
using Tree = std::map<std::string, Bytes>; // a folder's files, by path below it

inline void poke16(Bytes &b, std::size_t at, int v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

// A new character as CDataMgr::LoadSaveData 0x39989 builds it (docs/formats/save.md), with
// `seed` in the bytes the game never writes.
inline Bytes new_character(std::uint8_t seed = 0) {
    Bytes b(0x464, 0);
    b[2] = 1;                                   // level
    poke16(b, 0x009, 10), poke16(b, 0x00B, 10); // max HP / MP of a new character
    poke16(b, 0x00D, 10), poke16(b, 0x00F, 10); // hp, mp
    b[0x011] = 100;                             // meso
    for (std::size_t i = 0; i < 48; i++) {
        b[0x015 + 16 * i] = 0xFF; // equipment inventory: empty
        b[0x325 + 3 * i] = 0xFF;  // items: empty
    }
    for (std::size_t i = 0; i < 16; i++) {
        b[0x315 + i] = static_cast<std::uint8_t>(seed + i); // never written: heap bytes
    }
    b[0x325] = 3, b[0x326] = 5; // 5 Red Potions
    b[0x328] = 6, b[0x329] = 5; // 5 Blue Potions
    for (std::size_t i = 0; i < 5; i++) {
        b[0x3C2 + 16 * i] = 0xFF;
    }
    b[0x3C2 + 16] = 0x48, b[0x3C2 + 16 + 2] = 5;            // the Wooden Club, 5 slots
    b[0x414] = 1, b[0x415] = 3, b[0x416] = 1, b[0x417] = 6; // shortcuts: the potions
    b[0x41C] = 2, b[0x41D] = 8;                             // 28.map
    poke16(b, 0x41E, -1), poke16(b, 0x420, -1);
    b[0x423] = 2;
    const char caption[] = "How it All Began";
    std::memcpy(&b[0x424], caption, sizeof caption - 1);
    return b;
}

// The quest state of a new slot (CDataMgr::LoadQuestData 0x37D7D): every record empty.
inline Bytes new_quests(std::uint8_t chapter = 0) {
    Bytes b(0x12AD, 0);
    b[0] = chapter;
    for (std::size_t r = 0; r < 10; r++) {
        const std::size_t base = 0xC9 + 0x1C9 * r;
        for (std::size_t k = 0; k < 5; k++) {
            b[base + 0x143 + 14 * k] = 0xFF;
            b[base + 0x189 + 13 * k] = 0xFF;
        }
    }
    for (std::size_t r = 0; r < 10; r++) {
        b[0xC9 + 0x1C9 * r] = 0xFF; // after the rewards: each record's key_a wins the overlap
    }
    return b;
}

inline const Bytes kConfig = {3, 1, 0, 1, 1, 1};

// Two characters and the other files.
inline Tree valid_set() {
    return {{"theif0", new_character(1)},
            {"quest0", new_quests(1)},
            {"theif2", new_character(7)},
            {"quest2", new_quests(3)},
            {"config", kConfig},
            {"CTerms", Bytes(2, 0)},
            {"crestore", {1, 0, 0, 3, 0, 'a', 'b', 'c'}}};
}

} // namespace save_fixtures

#endif // MAPLETHIEF_TESTS_SAVE_FIXTURES_H
