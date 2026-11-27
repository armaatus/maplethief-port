// Text_Wrap's runaway on a word wider than the box (#419, ADR 0228, docs/re/original-bugs.md 15)
// with a synthetic guest: Text_Wrap 0x22EC5's loop written here as the game runs it (its frame's
// character and last space in guest memory, where the guard reads and writes them), calling
// Text_SubChars through MT_FN_HOOK with the game's return addresses, and a stand-in for
// Text_SubChars that steps as the original (1 byte, 3 for a byte >= 0x80, no look at the NUL)
// and counts every read outside the string. Characters are 6 points wide. The real game's wrap
// is play.korean_missing_key_original_synthetic (real assets).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "maplethief/hle/abi.h"
#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/recomp/calls.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 k_piece_return = 0x22F7Fu;    // SubChars(start, i): the piece measured
constexpr u32 k_too_wide_return = 0x22FFBu; // SubChars(start, lastSpace + 1): a line
constexpr u32 k_char_return = 0x23083u;     // SubChars(i, i + 1): the character
constexpr u32 k_line_return = 0x230C7u;     // SubChars(start, i): a line at a newline
constexpr int k_point_per_char = 6;
constexpr std::size_t k_max_lines = 128; // the line array on Text_Wrap's stack

struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    u32 frame = 0;
    u32 text = 0, text_end = 0; // the string and its NUL
    int outside = 0;            // reads outside [text, text_end]

    Guest() {
        mt_hook_reset();
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        frame = mt_heap_calloc(mem, 1, 64);
        REQUIRE(frame != 0);
        cpu.r[7] = frame + 64;
    }
    ~Guest() {
        mt_hooks_install_text_wrap_guard(0);
        mt_hook_reset();
        mt_mem_destroy(mem);
    }
    i32 get(u32 off) {
        i32 v = 0;
        std::memcpy(&v, mt_g2h(&cpu, cpu.r[7] - off, 4), 4);
        return v;
    }
    void set(u32 off, i32 v) { std::memcpy(mt_g2h(&cpu, cpu.r[7] - off, 4), &v, 4); }
};

Guest *g_guest = nullptr;

std::uint8_t read_byte(u32 addr) {
    Guest &g = *g_guest;
    if (addr < g.text || addr > g.text_end) {
        g.outside++;
    }
    return *mt_g2h(&g.cpu, addr, 1);
}

// Text_SubChars 0x22A1D as the game has it: skip `from` characters, copy `to - from`.
void sub_chars_original(mt_cpu *MT_RESTRICT cpu) {
    u32 p = cpu->r[1];
    const auto from = static_cast<i32>(cpu->r[2]);
    const auto to = static_cast<i32>(cpu->r[3]);
    for (i32 k = 0; k < from; k++) {
        p += read_byte(p) >= 0x80u ? 3u : 1u;
    }
    std::string out;
    for (i32 k = 0; k < to - from && out.size() < 255; k++) {
        const std::uint8_t c = read_byte(p);
        const u32 n = c >= 0x80u ? 3u : 1u;
        for (u32 b = 0; b < n; b++) {
            out.push_back(static_cast<char>(read_byte(p + b)));
        }
        p += n;
    }
    const u32 block = mt_heap_malloc(g_guest->mem, static_cast<u32>(out.size()) + 1u);
    std::memcpy(mt_g2h(cpu, block, static_cast<u32>(out.size()) + 1u), out.c_str(), out.size() + 1);
    cpu->r[0] = block;
}

void hooked_sub_chars(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_TEXT_SUB_CHARS, sub_chars_original);
}

std::string sub(Guest &g, i32 from, i32 to, u32 lr) {
    g.cpu.r[0] = 0x1000;
    g.cpu.r[1] = g.text;
    g.cpu.r[2] = static_cast<u32>(from);
    g.cpu.r[3] = static_cast<u32>(to);
    g.cpu.r[MT_LR] = lr;
    hooked_sub_chars(&g.cpu);
    return std::string(mt_g2h_str(&g.cpu, g.cpu.r[0]));
}

struct Wrapped {
    std::vector<std::string> lines;
    bool runaway = false; // more lines than Text_Wrap's array holds
};

// Text_Wrap 0x22EC5's loop: i at [r7-36], lastSpace at [r7-48].
Wrapped wrap(Guest &g, const std::string &s, int width) {
    g_guest = &g;
    g.text = mt_heap_calloc(g.mem, 1, static_cast<u32>(s.size()) + 1u);
    std::memcpy(mt_g2h(&g.cpu, g.text, static_cast<u32>(s.size())), s.data(), s.size());
    g.text_end = g.text + static_cast<u32>(s.size());
    i32 n = 0;
    for (std::size_t b = 0; b < s.size(); b += static_cast<std::uint8_t>(s[b]) >= 0x80u ? 3 : 1) {
        n++;
    }
    Wrapped w;
    i32 start = 0;
    g.set(48, 0);
    for (g.set(36, 0); g.get(36) < n; g.set(36, g.get(36) + 1)) {
        const i32 i = g.get(36);
        const std::string piece = sub(g, start, i, k_piece_return);
        if (static_cast<int>(piece.size()) * k_point_per_char > width) {
            w.lines.push_back(sub(g, start, g.get(48) + 1, k_too_wide_return));
            if (w.lines.size() >= k_max_lines) {
                w.runaway = true;
                return w;
            }
            start = g.get(48) + 1;
            g.set(36, i - 1);
            continue;
        }
        const std::string c = sub(g, i, i + 1, k_char_return);
        if (c == "\n" || c == "$") {
            w.lines.push_back(sub(g, start, i, k_line_return));
            start = i + 1;
            continue;
        }
        if (c == " ") {
            g.set(48, i);
        }
        if (i + 1 == n) {
            g.set(36, i + 1);
            w.lines.push_back(sub(g, start, i + 1, k_line_return));
            start = i + 1;
        }
    }
    return w;
}

std::string joined(const Wrapped &w) {
    std::string s;
    for (const std::string &l : w.lines) {
        s += l;
    }
    return s;
}

const std::string k_key = "Chapter3_Quest_Zenumist_Chief.dialog.1"; // 228 points

} // namespace

TEST_CASE("text wrap: without the guard an unbroken word wider than the box never ends") {
    Guest g;
    const Wrapped w = wrap(g, k_key, 168);
    CHECK(w.runaway);
    REQUIRE(w.lines.size() >= 2);
    CHECK(w.lines[0] == "C");  // lastSpace 0: cut after the first character
    CHECK(w.lines[1].empty()); // then empty lines: start never moves
}

TEST_CASE("text wrap: the guard cuts an unbroken word at the box's width, inside the string") {
    Guest g;
    mt_hooks_install_text_wrap_guard(1);
    const std::uint64_t before = mt_hooks_text_wrap_guarded();
    const Wrapped w = wrap(g, k_key, 168);
    CHECK_FALSE(w.runaway);
    CHECK(g.outside == 0);
    CHECK(joined(w) == k_key);
    REQUIRE(w.lines.size() == 3);
    CHECK(w.lines[0] == "C");                            // the device's own first line
    CHECK(w.lines[1] == "hapter3_Quest_Zenumist_Chief"); // 28 characters, 168 points
    CHECK(w.lines[2] == ".dialog.1");
    for (const std::string &l : w.lines) {
        CHECK(static_cast<int>(l.size()) * k_point_per_char <= 168);
    }
    CHECK(mt_hooks_text_wrap_guarded() - before == 1u);
}

TEST_CASE("text wrap: a long word after a space, and Hangul's 3-byte step") {
    Guest g;
    mt_hooks_install_text_wrap_guard(1);
    const std::string s = "Hi " + k_key + " ok";
    const Wrapped w = wrap(g, s, 168);
    CHECK_FALSE(w.runaway);
    CHECK(g.outside == 0);
    CHECK(joined(w) == s);
    for (const std::string &l : w.lines) {
        CHECK(static_cast<int>(l.size()) * k_point_per_char <= 168);
    }
    // 30 Hangul syllables (3 bytes each, 18 points here): one word, 540 points.
    std::string hangul;
    for (int k = 0; k < 30; k++) {
        hangul += "\xEA\xB0\x80"; // U+AC00
    }
    const Wrapped h = wrap(g, hangul, 168);
    CHECK_FALSE(h.runaway);
    CHECK(g.outside == 0);
    CHECK(joined(h) == hangul);
    for (const std::string &l : h.lines) {
        CHECK(l.size() % 3 == 0); // never inside a syllable
        CHECK(static_cast<int>(l.size()) * k_point_per_char <= 168);
    }
}

TEST_CASE("text wrap: text that breaks at spaces wraps the same with the guard and without") {
    const std::string s = "the quick brown fox jumps over the lazy dog, again and again$and a "
                          "second line\nand a third";
    std::vector<std::string> off, on;
    {
        Guest g;
        const Wrapped w = wrap(g, s, 168);
        CHECK_FALSE(w.runaway);
        CHECK(g.outside == 0);
        off = w.lines;
    }
    {
        Guest g;
        mt_hooks_install_text_wrap_guard(1);
        const std::uint64_t before = mt_hooks_text_wrap_guarded();
        const Wrapped w = wrap(g, s, 168);
        CHECK(mt_hooks_text_wrap_guarded() == before);
        on = w.lines;
    }
    CHECK(on == off);
    CHECK(off.size() >= 4);
}

TEST_CASE("text wrap: the guard wraps Text_SubChars while on, around any handler") {
    Guest g;
    mt_hooks_install_text_wrap_guard(1);
    CHECK(mt_hook_enabled(MT_HOOK_FN_TEXT_SUB_CHARS) == 0); // no handler: a pack's place
    // A handler inside it (as a text pack's) gets the guard's arguments.
    static i32 seen_to = -1;
    mt_hook_register(
        MT_HOOK_FN_TEXT_SUB_CHARS,
        [](mt_cpu *cpu, const mt_hook_call *call) -> int {
            seen_to = static_cast<i32>(cpu->r[3]);
            call->original(cpu);
            return MT_HOOK_HANDLED;
        },
        nullptr, 1);
    const Wrapped w = wrap(g, k_key, 168);
    CHECK_FALSE(w.runaway);
    CHECK(joined(w) == k_key);
    CHECK(seen_to >= 0);
    mt_hooks_install_text_wrap_guard(0);
    mt_hook_unregister(MT_HOOK_FN_TEXT_SUB_CHARS);
    const Wrapped off = wrap(g, k_key, 168);
    CHECK(off.runaway);
}
