// Text events (#391, ADR 0187, maplethief/text_events.h) on a synthetic guest: a CStateGame, its
// CChatBox with pages, rows, an NPC table and a mob list on the guest heap, and the game's
// globals in a __DATA segment. CChatBox::FirstPage, NextPage and CStateGame::Paint are written
// as the recompiler writes a hooked function (MT_FN_HOOK) around stand-ins that set what the
// game's would. Each event kind, its speaker, the joined text, the answer of the last page, the
// scene box once per change, a bad pointer (no event), and no write to guest memory: a guest
// run with the wrappers on ends byte for byte (and register for register) as one without them.
// The real game's events are play.speech_dialogue (play_real_test.cpp).
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "maplethief/hooks.h"
#include "maplethief/memory.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/text_events.h"

namespace {

using u32 = std::uint32_t;
using i32 = std::int32_t;

constexpr u32 k_state_vptr = 0x119B78u; // CStateGame's vtable pointer
constexpr u32 k_canvas_global = 0x11A950u;

struct Seen {
    mt_text_event e;
    std::string speaker, text;
};
std::vector<Seen> g_seen;
void record(void *, const mt_text_event *e) { g_seen.push_back(Seen{*e, e->speaker, e->text}); }

// One page the stand-in FirstPage / NextPage sets: its CMsg kind and its wrapped lines.
struct Page {
    i32 kind = 1;
    std::vector<std::string> lines;
};

struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    u32 state = 0, box = 0, canvas = 0, pages = 0;
    std::vector<Page> script; // the box's pages
    std::vector<u32> page_lines;

    Guest() {
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        mt_mem_attach(mem, &cpu);
        mt_segment seg{};
        std::strcpy(seg.name, "__DATA");
        seg.vmaddr = 0x11A000u;
        seg.vmsize = 0x1000u;
        seg.initprot = MT_PROT_READ | MT_PROT_WRITE;
        const std::uint8_t none[1] = {0};
        REQUIRE(mt_mem_map_segment(mem, &seg, none, 0) == 0);
        state = alloc(0x500);
        box = alloc(0x60);
        canvas = alloc(0x20);
        put(state, k_state_vptr);
        put(state + 0x5C, box);
        put(box + 0x48, state);
        put(canvas + 0x0C, state);
        put(k_canvas_global, canvas);
        pages = list({});
        put(box + 0x14, pages);
        // The NPC table: id 7 "Mai", id 9 a Korean CP949 name (0xB0 0xA1, U+AC00), stride 33.
        const u32 table = alloc(6 + 33 * 2);
        put16(table + 2, 2);
        put16(table + 4, 33);
        cpu.mem[table + 6] = 7;
        std::memcpy(cpu.mem + table + 7, "Mai", 3);
        cpu.mem[table + 6 + 33] = 9;
        cpu.mem[table + 7 + 33] = 0xB0;
        cpu.mem[table + 8 + 33] = 0xA1;
        put(0x11A988u, table);
        // The mobs: a snail (id 3), then a boss (id 0x42).
        const u32 snail = alloc(0x100);
        put(snail + 0xA0, 3);
        const u32 boss = alloc(0x100);
        put(boss + 0xA0, 0x42);
        put(boss + 0x9C, str("Werewolf"));
        put(0x11A9ECu, list({snail, boss}));
    }
    ~Guest() { mt_mem_destroy(mem); }
    Guest(const Guest &) = delete;
    Guest &operator=(const Guest &) = delete;

    u32 alloc(u32 n) {
        const u32 a = mt_heap_calloc(mem, 1, n);
        REQUIRE(a != 0);
        return a;
    }
    void put(u32 addr, u32 v) { std::memcpy(cpu.mem + addr, &v, 4); }
    void put16(u32 addr, u32 v) {
        cpu.mem[addr] = static_cast<std::uint8_t>(v);
        cpu.mem[addr + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    u32 get(u32 addr) const {
        u32 v = 0;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    u32 str(const std::string &s) {
        const u32 a = alloc(static_cast<u32>(s.size()) + 1);
        std::memcpy(cpu.mem + a, s.data(), s.size());
        return a;
    }
    // The game's list: count +4, items +8.
    u32 list(const std::vector<u32> &items) {
        const u32 l = alloc(12);
        const u32 a = alloc(static_cast<u32>(items.size()) * 4 + 4);
        for (std::size_t i = 0; i < items.size(); i++) {
            put(a + 4 * static_cast<u32>(i), items[i]);
        }
        put(l + 4, static_cast<u32>(items.size()));
        put(l + 8, a);
        return l;
    }
    u32 lines(const std::vector<std::string> &ls) {
        std::vector<u32> ptrs;
        for (const std::string &l : ls) {
            ptrs.push_back(str(l));
        }
        const u32 a = alloc(static_cast<u32>(ls.size()) * 4 + 4);
        for (std::size_t i = 0; i < ptrs.size(); i++) {
            put(a + 4 * static_cast<u32>(i), ptrs[i]);
        }
        return a;
    }
    // The script's pages (the CMsg list's count is what the box reads) and their lines, made
    // before the run, as Text_Wrap's would be.
    void set_script(std::vector<Page> s, i32 mode, i32 npc) {
        script = std::move(s);
        put(box + 0x08, static_cast<u32>(mode));
        put(state + 0xAC, static_cast<u32>(npc));
        put(pages + 4, static_cast<u32>(script.size()));
        page_lines.clear();
        for (const Page &p : script) {
            page_lines.push_back(lines(p.lines));
        }
    }
    // Heap and __DATA bytes: everything the guest has.
    std::vector<std::uint8_t> bytes() const {
        mt_heap_stats h{};
        mt_heap_get_stats(mem, &h);
        std::vector<std::uint8_t> b(cpu.mem + 0x11A000u, cpu.mem + 0x11B000u);
        b.insert(b.end(), cpu.mem + MT_GUEST_HEAP_BASE, cpu.mem + h.committed);
        return b;
    }
};

Guest *g_guest = nullptr;

// The stand-ins: FirstPage (page 0) and NextPage (page + 1) set the page, its kind, its lines
// and their count, or return 0 past the last page; they clobber r1-r3 and r12.
void set_page(mt_cpu *cpu, i32 page) {
    Guest &g = *g_guest;
    const u32 box = cpu->r[0];
    g.put(box + 0x10, static_cast<u32>(page));
    cpu->r[1] = 0x1111u;
    cpu->r[2] = 0x2222u;
    cpu->r[3] = 0x3333u;
    cpu->r[12] = 0xCCCCu;
    if (page >= static_cast<i32>(g.script.size())) {
        g.put(box + 0x18, 0);
        cpu->r[0] = 0xABCD00u; // false (the low byte), whatever the rest
        return;
    }
    const Page &p = g.script[static_cast<std::size_t>(page)];
    g.put(box + 0x0C, static_cast<u32>(p.kind));
    g.put(box + 0x18, g.page_lines[static_cast<std::size_t>(page)]);
    g.put(box + 0x1C, static_cast<u32>(p.lines.size()));
    cpu->r[0] = 1u;
}
void first_page_original(mt_cpu *MT_RESTRICT cpu) { set_page(cpu, 0); }
void next_page_original(mt_cpu *MT_RESTRICT cpu) {
    set_page(cpu, static_cast<i32>(g_guest->get(cpu->r[0] + 0x10)) + 1);
}
void first_page(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_CHAT_BOX_FIRST_PAGE, first_page_original);
}
void next_page(mt_cpu *MT_RESTRICT cpu) {
    MT_FN_HOOK(MT_HOOK_FN_CHAT_BOX_NEXT_PAGE, next_page_original);
}

// Paint's stand-in runs `g_paint_step` (the scene's runner, or a key between frames).
std::function<void(Guest &)> g_paint_step;
void paint_original(mt_cpu *MT_RESTRICT cpu) {
    if (g_paint_step) {
        g_paint_step(*g_guest);
    }
    cpu->r[0] = 0;
    cpu->r[1] = 0x77u;
}
void paint(mt_cpu *MT_RESTRICT cpu) { MT_FN_HOOK(MT_HOOK_FN_STATE_GAME_PAINT, paint_original); }

const u32 k_regs[] = {0,    0x11, 0x22, 0x33, 0x44, 0x55,   0x66,  0x77,
                      0x88, 0x99, 0xAA, 0xBB, 0xCC, 0x8000, 0x1235};

// Calls a hooked function with r0 = `self` and checks r4-r11, sp and lr are kept.
std::vector<u32> call(Guest &g, void (*fn)(mt_cpu *), u32 self) {
    for (u32 i = 0; i < 15; i++) {
        g.cpu.r[i] = k_regs[i];
    }
    g.cpu.r[0] = self;
    g_guest = &g;
    fn(&g.cpu);
    for (u32 i = 4; i < 15; i++) {
        if (i != 12) { // r12 is the callee's to clobber
            CHECK(g.cpu.r[i] == k_regs[i]);
        }
    }
    return {g.cpu.r, g.cpu.r + 4};
}

struct Fresh {
    Fresh() {
        mt_hook_reset();
        mt_text_events_reset();
        mt_text_events_reset_stats();
        g_seen.clear();
        g_paint_step = nullptr;
        REQUIRE(mt_text_events_add_listener(record, nullptr) == 0);
    }
    ~Fresh() {
        mt_text_events_remove_listener(record, nullptr);
        mt_hook_reset();
        g_paint_step = nullptr;
        g_guest = nullptr;
    }
};

bool valid_utf8(const std::string &s) {
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80                 ? 1
                              : c >= 0xC2 && c <= 0xDF ? 2
                              : c >= 0xE0 && c <= 0xEF ? 3
                              : c >= 0xF0 && c <= 0xF4 ? 4
                                                       : 0;
        if (n == 0 || i + n > s.size()) {
            return false;
        }
        for (std::size_t k = 1; k < n; k++) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += n;
    }
    return true;
}

// A dialogue: page 0 (two lines cut at a space), page 1 (cut at a newline, which Text_Wrap
// drops), past the end.
void dialogue(Guest &g, i32 mode, i32 npc) {
    g.set_script({Page{1, {"Hello there, ", "traveller."}}, Page{1, {"Line one", "line  two "}}},
                 mode, npc);
    if (mode == 2) {
        const u32 a = g.alloc(12), b = g.alloc(12);
        g.put(a + 4, g.str("Quest.row.1"));
        g.put(b + 4, g.str("Quest.row.2"));
        g.put(g.box + 0x20, g.list({a, b}));
    }
}

} // namespace

TEST_CASE("text events: FirstPage and NextPage are a page each, joined, with the NPC (#391)") {
    Fresh fresh;
    Guest g;
    dialogue(g, 0, 7);
    mt_text_events_install(1);
    call(g, first_page, g.box);
    REQUIRE(g_seen.size() == 1);
    CHECK(g_seen[0].e.kind == MT_TEXT_EVENT_PAGE);
    CHECK(g_seen[0].e.speaker_kind == MT_TEXT_SPEAKER_NPC);
    CHECK(g_seen[0].e.speaker_id == 7);
    CHECK(g_seen[0].speaker == "Mai");
    CHECK(g_seen[0].text == "Hello there, traveller.");
    CHECK(g_seen[0].e.index == 0);
    CHECK(g_seen[0].e.count == 2);
    call(g, next_page, g.box);
    REQUIRE(g_seen.size() == 2);
    CHECK(g_seen[1].text == "Line one line two"); // a space where the newline was, runs as one
    CHECK(g_seen[1].e.index == 1);
    // The last page of an OK box asks for no answer; past it, NextPage sets nothing.
    call(g, next_page, g.box);
    CHECK(g_seen.size() == 2);
    const mt_text_events_stats st = mt_text_events_get_stats();
    CHECK(st.entries == 3);
    CHECK(st.pages == 2);
    CHECK(st.answers == 0);
    CHECK(st.ignored == 0);
}

TEST_CASE("text events: the last page's answer, Accept / Refuse or the rows (#391)") {
    Fresh fresh;
    mt_text_events_install(1);
    for (const i32 mode : {1, 3}) {
        Guest g;
        g_seen.clear();
        dialogue(g, mode, 7);
        call(g, first_page, g.box);
        call(g, next_page, g.box);
        REQUIRE(g_seen.size() == 3);
        CHECK(g_seen[1].e.kind == MT_TEXT_EVENT_PAGE);
        CHECK(g_seen[2].e.kind == MT_TEXT_EVENT_ANSWER);
        CHECK(g_seen[2].e.value == mode);
        CHECK(g_seen[2].text.empty());
        CHECK(g_seen[2].speaker == "Mai");
    }
    Guest g;
    g_seen.clear();
    dialogue(g, 2, 7);
    call(g, first_page, g.box);
    CHECK(g_seen.size() == 1); // page 0 is not the last
    call(g, next_page, g.box);
    REQUIRE(g_seen.size() == 3);
    CHECK(g_seen[2].e.kind == MT_TEXT_EVENT_ANSWER);
    CHECK(g_seen[2].e.value == 2);
    // No bundle here: a row's Scripts key stands for itself, as -localizedStringForKey: answers.
    CHECK(g_seen[2].text == "Quest.row.1\nQuest.row.2");
}

TEST_CASE("text events: the speaker: the hero, a Korean NPC name, the boss (#391)") {
    Fresh fresh;
    mt_text_events_install(1);
    Guest g;
    g.set_script({Page{0, {"I am the hero."}}, Page{1, {"An NPC again."}}}, 0, 9);
    call(g, first_page, g.box);
    call(g, next_page, g.box);
    REQUIRE(g_seen.size() == 2);
    CHECK(g_seen[0].e.speaker_kind == MT_TEXT_SPEAKER_HERO);
    CHECK(g_seen[0].e.speaker_id == -1);
    CHECK(g_seen[0].speaker.empty()); // ScriptExtra's caption: no bundle in this guest
    CHECK(g_seen[1].e.speaker_kind == MT_TEXT_SPEAKER_NPC);
    CHECK(g_seen[1].speaker == "\xEA\xB0\x80"); // CP949 0xB0A1 as UTF-8 (U+AC00)
    // The box's NPC -1: the first boss in the mob list.
    Guest b;
    g_seen.clear();
    b.set_script({Page{1, {"Grr."}}}, 0, -1);
    call(b, first_page, b.box);
    REQUIRE(g_seen.size() == 1);
    CHECK(g_seen[0].e.speaker_kind == MT_TEXT_SPEAKER_MOB);
    CHECK(g_seen[0].e.speaker_id == 0x42);
    CHECK(g_seen[0].speaker == "Werewolf");
    // An NPC id the table has not: the speaker is known by id, not by name.
    Guest n;
    g_seen.clear();
    n.set_script({Page{1, {"Who am I?"}}}, 0, 200);
    call(n, first_page, n.box);
    REQUIRE(g_seen.size() == 1);
    CHECK(g_seen[0].e.speaker_id == 200);
    CHECK(g_seen[0].speaker.empty());
}

TEST_CASE("text events: the scene box when it opens and when it moves on two lines (#391)") {
    Fresh fresh;
    mt_text_events_install(1);
    Guest g;
    const u32 box_lines = g.lines({"First line.", "Second", "line.", "Last."});
    const auto box = [&](bool on, i32 first, i32 speaker) {
        g.cpu.mem[g.state + 0x1E9] = on ? 1 : 0;
        g.put(g.state + 0x4C8, static_cast<u32>(first));
        g.put(g.state + 0x4D0, box_lines);
        g.put(g.state + 0x4D4, 4);
        g.put(g.state + 0x4B4, static_cast<u32>(speaker));
    };
    // The runner opens it (command 50) inside a Paint: said after that frame.
    g_paint_step = [&](Guest &) { box(true, 0, -1); };
    call(g, paint, g.state);
    REQUIRE(g_seen.size() == 1);
    CHECK(g_seen[0].e.kind == MT_TEXT_EVENT_SCENE_BOX);
    CHECK(g_seen[0].text == "First line. Second");
    CHECK(g_seen[0].e.index == 0);
    CHECK(g_seen[0].e.count == 4);
    CHECK(g_seen[0].e.speaker_kind == MT_TEXT_SPEAKER_HERO);
    // More frames of the same box: nothing more.
    g_paint_step = nullptr;
    call(g, paint, g.state);
    call(g, paint, g.state);
    CHECK(g_seen.size() == 1);
    // OK between frames shows the next two lines.
    box(true, 2, 7);
    call(g, paint, g.state);
    REQUIRE(g_seen.size() == 2);
    CHECK(g_seen[1].text == "line. Last.");
    CHECK(g_seen[1].e.index == 2);
    CHECK(g_seen[1].speaker == "Mai");
    // OK on the last: the box closes; then a boss's box (-0x42: the bosses' ScriptExtra name).
    box(false, 2, 7);
    call(g, paint, g.state);
    CHECK(g_seen.size() == 2);
    box(true, 0, -0x42);
    call(g, paint, g.state);
    REQUIRE(g_seen.size() == 3);
    CHECK(g_seen[2].e.speaker_kind == MT_TEXT_SPEAKER_MOB);
    CHECK(g_seen[2].e.speaker_id == 0x42);
    // -2: the boss in the mob list.
    box(false, 0, -2);
    call(g, paint, g.state);
    box(true, 0, -2);
    call(g, paint, g.state);
    REQUIRE(g_seen.size() == 4);
    CHECK(g_seen[3].speaker == "Werewolf");
    // A Paint that ends the state (the engine's state is another now): nothing read.
    box(false, 0, -1);
    call(g, paint, g.state);
    g_paint_step = [&](Guest &gg) {
        box(true, 0, -1);
        gg.put(gg.canvas + 0x0C, gg.box);
    };
    call(g, paint, g.state);
    CHECK(g_seen.size() == 4);
    CHECK(mt_text_events_get_stats().scene_boxes == 4);
}

TEST_CASE("text events: a bad pointer is no event, invalid UTF-8 is replaced (#391)") {
    Fresh fresh;
    mt_text_events_install(1);
    Guest g;
    g.set_script({Page{1, {"Fine."}}, Page{1, {"Also fine."}}}, 0, 7);
    // A line pointer that is not guest memory.
    g.put(g.page_lines[0], 0xDEAD0000u);
    call(g, first_page, g.box);
    CHECK(g_seen.empty());
    // The lines array itself outside the heap.
    g.page_lines[1] = 0x00F00000u;
    call(g, next_page, g.box);
    CHECK(g_seen.empty());
    CHECK(mt_text_events_get_stats().ignored == 2);
    // A box whose state pointer is bad: an NPC page cannot name its speaker; no event.
    Guest s;
    s.set_script({Page{1, {"Hm."}}}, 0, 7);
    s.put(s.box + 0x48, 0x10u);
    call(s, first_page, s.box);
    CHECK(g_seen.empty());
    // A scene box whose lines are gone.
    Guest p;
    p.cpu.mem[p.state + 0x1E9] = 1;
    p.put(p.state + 0x4D0, 0xDEAD0000u);
    p.put(p.state + 0x4D4, 2);
    call(p, paint, p.state);
    CHECK(g_seen.empty());
    CHECK(mt_text_events_get_stats().ignored == 4);
    // Bytes that are not UTF-8 (a stray continuation byte, a lone lead byte): U+FFFD each.
    Guest u;
    u.set_script({Page{1,
                       {"a\x80"
                        "b\xC3"}}},
                 0, 7);
    call(u, first_page, u.box);
    REQUIRE(g_seen.size() == 1);
    CHECK(valid_utf8(g_seen[0].text));
    CHECK(g_seen[0].text == "a\xEF\xBF\xBD"
                            "b\xEF\xBF\xBD");
}

TEST_CASE("text events: the wrappers write no guest memory and leave the registers (#391)") {
    Fresh fresh;
    // Two guests built alike; one runs with the wrappers off, one with them on.
    const auto run = [](Guest &g) {
        dialogue(g, 2, -1);
        g_paint_step = [](Guest &gg) {
            gg.cpu.mem[gg.state + 0x1E9] = 1;
            gg.put(gg.state + 0x4D0, gg.page_lines[0]);
            gg.put(gg.state + 0x4D4, 2);
        };
        std::vector<u32> regs;
        for (void (*fn)(mt_cpu *) : {first_page, next_page, next_page}) {
            const std::vector<u32> r = call(g, fn, g.box);
            regs.insert(regs.end(), r.begin(), r.end());
        }
        const std::vector<u32> r = call(g, paint, g.state);
        regs.insert(regs.end(), r.begin(), r.end());
        return regs;
    };
    Guest off;
    mt_text_events_install(0);
    const std::vector<u32> regs_off = run(off);
    CHECK(g_seen.empty()); // off: the original alone
    Guest on;
    mt_text_events_install(1);
    const std::vector<u32> regs_on = run(on);
    CHECK(g_seen.size() == 4); // two pages, the answer, the scene box
    CHECK(regs_on == regs_off);
    CHECK(on.bytes() == off.bytes());
}

TEST_CASE("text events: a listener added twice hears once; removed, it hears nothing") {
    Fresh fresh;
    CHECK(mt_text_events_add_listener(record, nullptr) == 0);
    CHECK(mt_text_events_add_listener(nullptr, nullptr) == -1);
    mt_text_events_install(1);
    Guest g;
    g.set_script({Page{1, {"Once."}}}, 0, 7);
    call(g, first_page, g.box);
    CHECK(g_seen.size() == 1);
    mt_text_events_remove_listener(record, nullptr);
    call(g, first_page, g.box);
    CHECK(g_seen.size() == 1);
}
