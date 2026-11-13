// Fuzz target: the state file (runtime/src/snapshot/state_file.cpp) and every section's load
// function (ADR 0105), against a synthetic guest built once (#323, ADR 0154).
//
// The guest is the one snapshot_test.cpp restores, with a host state of most serializers alive.
// Its state file (the baseline) is saved once. The first byte of an input picks what the rest
// is (modulo the number of choices):
//
//   0          a whole state file: read with and without its payloads, its header, then
//              restored
//   1 .. S     the payload of baseline section k (core sections, then every module's): the
//              baseline with that section replaced, restored. Every other section is valid, so
//              the payload reaches its section's reader
//   S+1 .. +O  the payload of one host state serializer's load (StateOps), run on a zeroed
//              block as a restore runs it, then dropped
//
// Oracles besides the sanitizers: a refusal has a reason; a refused restore changes nothing (the
// guest saved again is the baseline, section by section); an accepted one is undone by
// restoring the baseline, which must succeed.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "cg/cg_objects.h"
#include "foundation/foundation.h"
#include "fuzz2.h"
#include "maplethief/cf.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/snapshot.h"
#include "objc/objc_internal.h"
#include "snapshot/snapshot_internal.h"
#include "snapshot/state_file.h"

namespace {

using mt_fuzz2::Bytes;
using u32 = std::uint32_t;

constexpr u32 k_segment = 0x00100000u; // a synthetic writable image segment
constexpr u32 k_segment_size = 0x8000u;

struct Guest {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    Bytes baseline;                         // the state file, deflated
    std::vector<mt_snap::Section> sections; // its sections, inflated
    mt_snapshot_header header{};
};

Guest *g_guest = nullptr;

[[noreturn]] void setup_failed(const char *what) { mt_fuzz2::broken(what); }

u32 send(Guest &g, u32 self, const char *sel, std::initializer_list<u32> args = {}) {
    const std::vector<u32> v(args);
    return mt_objc_msg(&g.cpu, self, mt_objc_sel(g.rt, sel), v.data(), static_cast<u32>(v.size()));
}

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

void call(Guest &g, mt_import_fn fn, std::initializer_list<u32> args) {
    u32 i = 0;
    for (const u32 a : args) {
        g.cpu.r[i++] = a;
    }
    fn(&g.cpu, 0);
}

bool save(Guest &g, Bytes *out, bool store) {
    mt_snapshot_meta meta = mt_snapshot_meta_default();
    std::memset(meta.binary_sha256, 0xAB, 32);
    meta.flags = store ? MT_SNAPSHOT_STORE : 0u;
    std::uint8_t *bytes = nullptr;
    std::size_t n = 0;
    char why[256] = {};
    if (mt_snapshot_save(&g.cpu, &meta, &bytes, &n, why, sizeof why) != 0) {
        std::fprintf(stderr, "save: %s\n", why);
        return false;
    }
    out->assign(bytes, bytes + n);
    std::free(bytes);
    return true;
}

std::vector<mt_snap::Section> sections_of(const Bytes &file) {
    mt_snapshot_header h{};
    std::vector<mt_snap::Section> s;
    std::string why;
    if (!mt_snap::read_state_file(file.data(), file.size(), nullptr, &h, &s, &why)) {
        setup_failed("a state file the port saved does not read back");
    }
    return s;
}

// Host states of every kind this guest can make without the game: the value states of #202,
// CG's, and UIKit, GameKit and StoreKit objects by alloc/init. An object whose class has no
// serializer would make the save fail; each is kept only if the save still works.
void populate(Guest &g) {
    mt_cpu *cpu = &g.cpu;
    const u32 str = mt_cf::string_create(cpu, u"Maple", false);
    const u32 mstr = mt_cf::string_create(cpu, u"Thief", true);
    mt_cf::string_guest_cstring(cpu, str, mt_cf::k_enc_utf8);
    const u32 num = mt_cf::number_create(cpu, mt_cf::Number{4u, false, 42, 42.0});
    const std::uint8_t raw[5] = {1, 2, 3, 4, 5};
    const u32 data = mt_cf::data_create(cpu, raw, 5, true);
    const u32 items[2] = {str, num};
    mt_ns::array_create(cpu, items, 2, true);
    mt_ns::set_create(cpu, &data, 1);
    const u32 dict = mt_ns::dictionary_create(cpu, true);
    mt_ns::dictionary_set(cpu, dict, str, num);
    mt_cf::url_create(cpu, "Data/Map.img");
    call(g, mt_hle_CFBundleGetMainBundle, {});
    const u32 pool = mt_objc_pool_push(g.rt);
    mt_objc_retain(cpu,
                   mt_objc_send1(cpu, mt_objc_class_named(g.rt, "NSURL"), "URLWithString:", str));
    const u32 error_args[3] = {str, 7u, dict};
    mt_objc_retain(cpu,
                   mt_objc_msg(cpu, mt_objc_class_named(g.rt, "NSError"),
                               mt_objc_sel(g.rt, "errorWithDomain:code:userInfo:"), error_args, 3));
    mt_objc_pool_pop(g.rt, pool);
    mt_objc_impl::map_put(g.rt->syncs, mstr, 2);
    mt_cg::color_create(cpu, mt_cg::Color{0.25f, 0.5f, 0.75f, 1.0f});
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(
        std::vector<std::uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 10, 20, 30, 255});
    const u32 image = mt_cg::image_create_host(cpu, 2, 2, 32, 1u, pixels);
    mt_cg::Path p;
    p.move_to({1, 2});
    p.line_to({3, 4});
    p.close();
    mt_cg::path_create(cpu, p);
    const u32 image_pool = mt_objc_pool_push(g.rt);
    mt_objc_retain(cpu, mt_cg::uiimage_create(cpu, image));
    mt_objc_pool_pop(g.rt, image_pool);
    Bytes check;
    if (!save(g, &check, true)) {
        setup_failed("the value host states do not save");
    }
    // The rest by their own messages, each object kept (retained) only if the guest still
    // saves with it: a class method's result, or alloc and an initializer.
    const u32 frame[4] = {fbits(0), fbits(0), fbits(320), fbits(480)};
    const u32 rgba[4] = {fbits(0.1f), fbits(0.2f), fbits(0.3f), fbits(1.0f)};
    const u32 ids = mt_ns::set_create(cpu, &str, 1);
    struct Make {
        const char *cls, *sel;
        bool alloc;
        std::vector<u32> args;
        const char *then = nullptr; // then sent with `str`
    };
    const Make makes[] = {
        {"UIView", "initWithFrame:", true, {frame, frame + 4}},
        {"UIWindow", "initWithFrame:", true, {frame, frame + 4}},
        {"UIImageView", "initWithFrame:", true, {frame, frame + 4}},
        {"UIViewController", "init", true, {}},
        {"UIColor", "colorWithRed:green:blue:alpha:", false, {rgba, rgba + 4}},
        {"UIFont", "boldSystemFontOfSize:", false, {fbits(12)}},
        {"UIBezierPath", "bezierPath", false, {}},
        {"GKScore", "initWithCategory:", true, {str}},
        {"GKAchievement", "initWithIdentifier:", true, {str}},
        {"GKLeaderboard", "init", true, {}, "setCategory:"},
        {"GKLeaderboardViewController", "init", true, {}, "setCategory:"},
        {"SKProductsRequest", "initWithProductIdentifiers:", true, {ids}},
        {"AVAudioSession", "sharedInstance", false, {}},
    };
    for (const Make &m : makes) {
        const u32 cls = mt_objc_class_named(g.rt, m.cls);
        if (cls == 0) {
            continue;
        }
        const u32 made_pool = mt_objc_pool_push(g.rt);
        const u32 self = m.alloc ? send(g, cls, "alloc") : cls;
        const u32 made =
            mt_objc_retain(cpu, mt_objc_msg(cpu, self, mt_objc_sel(g.rt, m.sel), m.args.data(),
                                            static_cast<u32>(m.args.size())));
        if (m.alloc && made != 0) {
            mt_objc_release(cpu, made); // alloc's reference
        }
        if (made != 0 && m.then != nullptr) {
            send(g, made, m.then, {str});
        }
        mt_objc_pool_pop(g.rt, made_pool);
        if (made != 0 && !save(g, &check, true)) {
            std::fprintf(stderr, "fuzz_state: a %s left out (no serializer)\n", m.cls);
            mt_objc_release(cpu, made);
        }
    }
    // A view controller's view and a view's layer come on first use.
    const u32 vc = send(g, mt_objc_class_named(g.rt, "UIViewController"), "alloc");
    if (vc != 0) {
        const u32 view = send(g, send(g, vc, "init"), "view");
        if (view != 0) {
            send(g, view, "layer");
        }
    }
    if (!save(g, &check, true)) {
        setup_failed("the guest does not save with its UIKit objects");
    }
    std::memset(cpu->mem + k_segment + 0x100, 0x5E, 0x200);
    std::memset(cpu->mem + g.stack.top - 0x400, 0x77, 0x100);
    const u32 big = mt_heap_malloc(g.mem, 64u << 10);
    std::memset(cpu->mem + big, 0x42, 64u << 10);
    cpu->r[4] = 0x44444444u;
    cpu->d[3] = 0x3333333333333333ull;
}

std::size_t choices() {
    std::size_t ops = 0;
    mt_snap::all_ops(&ops);
    return 1 + g_guest->sections.size() + ops;
}

// The payloads of objc.states by serializer name (the first state of each).
std::vector<std::pair<std::string, Bytes>> state_payloads(const Bytes &section) {
    std::vector<std::pair<std::string, Bytes>> out;
    mt_snap::Reader r(section.data(), section.size());
    u32 n = 0;
    if (!r.u32(&n)) {
        return out;
    }
    for (u32 i = 0; i < n; i++) {
        std::string name;
        u32 obj = 0, len = 0;
        if (!r.str(&name) || !r.u32(&obj) || !r.u32(&len)) {
            break;
        }
        const std::uint8_t *p = r.raw(len);
        if (p == nullptr) {
            break;
        }
        out.emplace_back(name, Bytes(p, p + len));
    }
    return out;
}

bool same_as_baseline(Guest &g) {
    Bytes now;
    if (!save(g, &now, true)) {
        return false;
    }
    const std::vector<mt_snap::Section> s = sections_of(now);
    if (s.size() != g.sections.size()) {
        return false;
    }
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i].name != g.sections[i].name || s[i].bytes != g.sections[i].bytes) {
            std::fprintf(stderr, "section %s changed\n", s[i].name.c_str());
            return false;
        }
    }
    return true;
}

// Restores `file`; on success, restores the baseline again. On a refusal, checks nothing
// changed.
void restore(Guest &g, const std::uint8_t *file, std::size_t size) {
    char why[256] = {};
    mt_snapshot_header h{};
    if (mt_snapshot_restore(&g.cpu, file, size, nullptr, &h, why, sizeof why) == 0) {
        if (mt_snapshot_restore(&g.cpu, g.baseline.data(), g.baseline.size(), nullptr, nullptr, why,
                                sizeof why) != 0) {
            std::fprintf(stderr, "baseline: %s\n", why);
            mt_fuzz2::broken("the baseline does not restore after an accepted state");
        }
        return;
    }
    if (why[0] == '\0') {
        mt_fuzz2::broken("a state refused without a reason");
    }
    if (!same_as_baseline(g)) {
        std::fprintf(stderr, "refused with: %s\n", why);
        mt_fuzz2::broken("a refused state changed the guest");
    }
}

} // namespace

void mt_fuzz2::init() {
    if (g_guest != nullptr) {
        return;
    }
    // Lives for the process: the fuzzer never returns to tear it down.
    auto *g = new Guest;
    g->mem = mt_mem_create(nullptr);
    if (g->mem == nullptr) {
        setup_failed("no guest memory");
    }
    mt_mem_attach(g->mem, &g->cpu);
    mt_segment seg{};
    std::memcpy(seg.name, "__DATA", 7);
    seg.vmaddr = k_segment;
    seg.vmsize = k_segment_size;
    seg.initprot = MT_PROT_READ | MT_PROT_WRITE;
    const std::uint8_t file[16] = {0xDA, 0x7A};
    seg.filesize = sizeof file;
    if (mt_mem_map_segment(g->mem, &seg, file, sizeof file) != 0 ||
        mt_stack_create(g->mem, 0, &g->stack) != 0) {
        setup_failed("no guest segment or stack");
    }
    g->cpu.r[MT_SP] = g->stack.top - 64u;
    g->cpu.r[MT_LR] = 0x2345u;
    mt_import_config cfg = mt_import_config_default();
    g->rt = mt_objc_create(&g->cpu, mt_mem_guest_alloc, g->mem, &cfg);
    if (g->rt == nullptr) {
        setup_failed("no ObjC runtime");
    }
    mt_objc_set_free(g->rt, mt_mem_guest_free);
    mt_objc_set_current(g->rt);
    if (mt_objc_add_host_classes(g->rt) != 0u) {
        setup_failed("no host classes");
    }
    mt_cf_start(&g->cpu, g->rt);
    const mt_snapshot_config sc{0, 0, 0};
    mt_snapshot_configure(&sc);
    populate(*g);
    if (!save(*g, &g->baseline, false)) {
        setup_failed("the guest does not save");
    }
    g->sections = sections_of(g->baseline);
    std::string why;
    std::vector<mt_snap::Section> unused;
    mt_snap::read_state_file(g->baseline.data(), g->baseline.size(), nullptr, &g->header, &unused,
                             &why);
    g_guest = g;
    // Restored once, so that the guest is exactly what the baseline restores to.
    restore(*g, g->baseline.data(), g->baseline.size());
}

std::vector<mt_fuzz2::Seed> mt_fuzz2::seeds() {
    init();
    Guest &g = *g_guest;
    std::vector<Seed> out;
    const auto add = [&out](std::string name, std::size_t choice, const Bytes &payload) {
        for (char &c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') {
                c = '_';
            }
        }
        Bytes b{static_cast<std::uint8_t>(choice)};
        b.insert(b.end(), payload.begin(), payload.end());
        out.push_back(Seed{name, b});
    };
    add("file-deflated", 0, g.baseline);
    Bytes stored;
    if (save(g, &stored, true)) {
        add("file-stored", 0, stored);
    }
    for (std::size_t i = 0; i < g.sections.size(); i++) {
        add("section-" + g.sections[i].name, 1 + i, g.sections[i].bytes);
    }
    const mt_snap::Section *states = mt_snap::find_section(g.sections, "objc.states");
    const auto payloads = states != nullptr ? state_payloads(states->bytes)
                                            : std::vector<std::pair<std::string, Bytes>>{};
    std::size_t nops = 0;
    const mt_snap::StateOps *const *ops = mt_snap::all_ops(&nops);
    for (std::size_t i = 0; i < nops; i++) {
        Bytes payload;
        for (const auto &p : payloads) {
            if (p.first == ops[i]->name) {
                payload = p.second;
                break;
            }
        }
        add(std::string("state-") + ops[i]->name, 1 + g.sections.size() + i, payload);
    }
    return out;
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    Guest &g = *g_guest;
    const std::size_t choice = data[0] % choices();
    const std::uint8_t *payload = data + 1;
    const std::size_t n = size - 1;
    if (choice == 0) {
        // The file in its own allocation, so that ASan sees a read past its end.
        const Bytes file(payload, payload + n);
        mt_snapshot_header h{};
        std::vector<mt_snap::Section> sections;
        std::string why;
        if (!mt_snap::read_state_file(file.data(), file.size(), nullptr, &h, nullptr, &why) &&
            why.empty()) {
            mt_fuzz2::broken("a state file refused without a reason");
        }
        mt_snap::read_state_file(file.data(), file.size(), nullptr, &h, &sections, &why);
        char text[160] = {};
        mt_snapshot_read_header(file.data(), file.size(), nullptr, &h, text, sizeof text);
        restore(g, file.data(), file.size());
        return 0;
    }
    if (choice <= g.sections.size()) {
        std::vector<mt_snap::Section> sections = g.sections;
        sections[choice - 1].bytes.assign(payload, payload + n);
        Bytes file;
        std::string why;
        if (!mt_snap::write_state_file(g.header, sections, true, &file, &why)) {
            mt_fuzz2::broken("the state file writer refused a section");
        }
        restore(g, file.data(), file.size());
        return 0;
    }
    std::size_t nops = 0;
    const mt_snap::StateOps *ops = mt_snap::all_ops(&nops)[choice - 1 - g.sections.size()];
    const Bytes bytes(payload, payload + n);
    void *block = std::calloc(1, ops->type->size != 0 ? ops->type->size : 1);
    if (block == nullptr) {
        return 0;
    }
    mt_snap::Reader r(bytes.data(), bytes.size(), ops->name);
    // A failed load may hold host memory, as in read_states: drop frees what it set.
    (void)(ops->load(&g.cpu, 0x1000u, block, r) && r.done());
    if (ops->drop != nullptr) {
        ops->drop(block);
    }
    std::free(block);
    return 0;
}
