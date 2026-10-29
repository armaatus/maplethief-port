// objc.host_surface (#49): every (class, selector) of docs/re/host-surface.md in the areas that
// are implemented is answered by the runtime's registered host classes. The table is generated
// from #11's analysis (python3 -m mtport.hostsurface generate) and committed, so this runs in CI.
// An area joins kCheckedAreas when its issue lands (AVFoundation: M6, OpenGLES: #117).
#include <doctest/doctest.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"

#ifndef MT_HOST_SURFACE_DOC
#error "MT_HOST_SURFACE_DOC must name docs/re/host-surface.md"
#endif

namespace {

using u32 = std::uint32_t;

// UIKit: #51's drawing classes (ADR 0036, NSString's drawing selectors included) and #54's
// views, controls, application and touches (ADR 0037).
const char *const kCheckedAreas[] = {"Foundation", "GameKit", "StoreKit", "UIKit"};

struct Row {
    std::string area, cls, selector;
    bool is_class_method = false;
};

// The table rows "| `Class` | `±selector` | sends | receiver |" under "## <Area>".
std::vector<Row> read_rows() {
    std::ifstream f(MT_HOST_SURFACE_DOC);
    REQUIRE(f.good());
    std::vector<Row> rows;
    std::string line, area;
    while (std::getline(f, line)) {
        if (line.rfind("## ", 0) == 0) {
            area = line.substr(3);
            continue;
        }
        if (line.rfind("| `", 0) != 0) {
            continue;
        }
        // | `Class` | `-sel` | ...
        const std::size_t c0 = 3, c1 = line.find('`', c0);
        const std::size_t s0 = line.find('`', c1 + 1), s1 = line.find('`', s0 + 1);
        REQUIRE(c1 != std::string::npos);
        REQUIRE(s1 != std::string::npos);
        Row r;
        r.area = area;
        r.cls = line.substr(c0, c1 - c0);
        const std::string sel = line.substr(s0 + 1, s1 - s0 - 1);
        REQUIRE((sel[0] == '+' || sel[0] == '-'));
        r.is_class_method = sel[0] == '+';
        r.selector = sel.substr(1);
        rows.push_back(r);
    }
    return rows;
}

bool checked(const std::string &area) {
    for (const char *a : kCheckedAreas) {
        if (area == a) {
            return true;
        }
    }
    return false;
}

void ignore_log(void *, const char *) {}

} // namespace

TEST_CASE("host_surface") {
    const std::vector<Row> rows = read_rows();
    CHECK(rows.size() > 250u);

    mt_mem *mem = mt_mem_create(nullptr);
    REQUIRE(mem != nullptr);
    mt_cpu cpu{};
    mt_mem_attach(mem, &cpu);
    mt_import_config cfg = mt_import_config_default();
    cfg.log = ignore_log;
    mt_objc *rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
    REQUIRE(rt != nullptr);
    REQUIRE(mt_objc_add_host_classes(rt) == 0u);

    std::vector<std::string> missing;
    u32 seen = 0;
    for (const Row &r : rows) {
        if (!checked(r.area)) {
            continue;
        }
        seen++;
        const std::string name =
            std::string(r.is_class_method ? "+[" : "-[") + r.cls + " " + r.selector + "]";
        const u32 cls = mt_objc_class_named(rt, r.cls.c_str());
        if (cls == 0 || mt_objc_class_is_placeholder(rt, cls) != 0) {
            missing.push_back(name + ": no host class");
            continue;
        }
        const u32 target = r.is_class_method ? mt_objc_class_of(rt, cls) : cls;
        mt_objc_method m{};
        if (mt_objc_lookup(rt, target, mt_objc_sel(rt, r.selector.c_str()), &m) == 0) {
            missing.push_back(name + ": not implemented");
        } else if (m.origin != static_cast<u32>(MT_OBJC_HOST) &&
                   m.origin != static_cast<u32>(MT_OBJC_OVERRIDE)) {
            missing.push_back(name + ": not a host method");
        }
    }
    CHECK(seen > 100u);
    for (const std::string &m : missing) {
        INFO(m);
        CHECK(false);
    }

    mt_objc_destroy(rt);
    mt_mem_destroy(mem);
}
