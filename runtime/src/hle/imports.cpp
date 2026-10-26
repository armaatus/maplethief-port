// The import table at run time (ADR 0019): linking HLE implementations by name, the default
// handler for unimplemented imports, data objects and binds. See maplethief/hle/imports.h.
//
// Plain C++ without exceptions, allocation through new or function-local statics, so that the
// recompiler's C test harness can also compile it with -fno-exceptions and link it into a C
// program (tests/test_recomp_imports.py).
#include "maplethief/hle/imports.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct State {
    const mt_import_desc *descs = nullptr;
    std::uint32_t count = 0;
    mt_import_config cfg = {MT_UNIMPLEMENTED_STOP, nullptr, nullptr};
    std::uint32_t *calls = nullptr; // per id: calls of the default handler
};

// Set once by mt_imports_link at start-up; the guest is single-threaded (ADR 0014).
State g_state;

void log_line(const mt_import_config &cfg, const char *line) {
    if (cfg.log != nullptr) {
        cfg.log(cfg.log_user, line);
        return;
    }
    std::fputs(line, stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

const mt_hle_entry *find(const mt_hle_entry *hle, std::uint32_t n, const char *name) {
    for (std::uint32_t i = 0; i < n; i++) {
        if (hle[i].name != nullptr && std::strcmp(hle[i].name, name) == 0) {
            return &hle[i];
        }
    }
    return nullptr;
}

// "/System/Library/Frameworks/UIKit.framework/UIKit" -> "UIKit"
const char *base_name(const char *path) {
    if (path == nullptr) {
        return "?";
    }
    const char *slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

bool in_guest(const mt_cpu *cpu, std::uint32_t addr, std::uint32_t size) {
    return static_cast<std::uint64_t>(addr) + size <= cpu->mem_size;
}

} // namespace

extern "C" mt_import_config mt_import_config_default(void) noexcept {
    return mt_import_config{MT_UNIMPLEMENTED_STOP, nullptr, nullptr};
}

extern "C" int mt_unimplemented_policy_parse(const char *text, std::uint32_t *policy) noexcept {
    static constexpr struct {
        const char *name;
        std::uint32_t policy;
    } k_names[] = {
        {"stop", MT_UNIMPLEMENTED_STOP},
        {"continue", MT_UNIMPLEMENTED_CONTINUE},
        {"trace", MT_UNIMPLEMENTED_TRACE},
    };
    if (text == nullptr) {
        return -1;
    }
    for (const auto &n : k_names) {
        if (std::strcmp(text, n.name) == 0) {
            *policy = n.policy;
            return 0;
        }
    }
    return -1;
}

extern "C" int mt_import_config_from_env(mt_import_config *cfg) noexcept {
    char value[32] = {};
#if defined(_MSC_VER)
    char *dup = nullptr;
    std::size_t len = 0;
    if (_dupenv_s(&dup, &len, MT_UNIMPLEMENTED_IMPORT_ENV) != 0 || dup == nullptr) {
        return 0;
    }
    std::snprintf(value, sizeof value, "%s", dup);
    std::free(dup);
#else
    const char *env = std::getenv(MT_UNIMPLEMENTED_IMPORT_ENV);
    if (env == nullptr) {
        return 0;
    }
    std::snprintf(value, sizeof value, "%s", env);
#endif
    if (mt_unimplemented_policy_parse(value, &cfg->unimplemented) != 0) {
        char line[160];
        std::snprintf(line, sizeof line,
                      "%s=%s is not a policy (stop, continue or trace); keeping the default",
                      MT_UNIMPLEMENTED_IMPORT_ENV, value);
        log_line(*cfg, line);
        return -1;
    }
    return 0;
}

extern "C" std::uint32_t mt_imports_link(const mt_import_desc *descs, std::uint32_t count,
                                         mt_import_fn *table, const mt_hle_entry *hle,
                                         std::uint32_t hle_count,
                                         const mt_import_config *cfg) noexcept {
    std::free(g_state.calls);
    g_state.descs = descs;
    g_state.count = count;
    g_state.cfg = cfg != nullptr ? *cfg : mt_import_config_default();
    g_state.calls = static_cast<std::uint32_t *>(std::calloc(count + 1u, sizeof(std::uint32_t)));
    std::uint32_t unimplemented = 0;
    for (std::uint32_t id = 0; id < count; id++) {
        const mt_import_desc &d = descs[id];
        const mt_hle_entry *e =
            (d.flags & MT_IMPORT_FUNCTION) != 0u ? find(hle, hle_count, d.name) : nullptr;
        if (e != nullptr && e->fn != nullptr) {
            table[id] = e->fn;
        } else {
            table[id] = mt_import_unimplemented;
            unimplemented += (d.flags & MT_IMPORT_FUNCTION) != 0u ? 1u : 0u;
        }
    }
    return unimplemented;
}

extern "C" void mt_import_unimplemented(mt_cpu *cpu, std::uint32_t id) noexcept {
    const State &s = g_state;
    const std::uint32_t lr = cpu->r[MT_LR];
    const mt_import_desc *d = s.descs != nullptr && id < s.count ? &s.descs[id] : nullptr;
    std::uint32_t calls = 1;
    if (s.calls != nullptr && id < s.count) {
        calls = ++s.calls[id];
    }
    // A data import's slot is never called by generated code: stop whatever the policy.
    const bool returns = s.cfg.unimplemented == MT_UNIMPLEMENTED_CONTINUE ||
                         s.cfg.unimplemented == MT_UNIMPLEMENTED_TRACE;
    const bool stop = !returns || d == nullptr || (d->flags & MT_IMPORT_FUNCTION) == 0u;
    if (stop || calls == 1u || s.cfg.unimplemented == MT_UNIMPLEMENTED_TRACE) {
        char line[512];
        std::snprintf(
            line, sizeof line, "unimplemented import %s (id %u, %s%s) called from 0x%08X: %s%s",
            d != nullptr ? d->name : "?", static_cast<unsigned>(id),
            d != nullptr ? base_name(d->library) : "unknown id",
            d != nullptr && (d->flags & MT_IMPORT_DATA) != 0u ? ", data" : "",
            static_cast<unsigned>(lr), stop ? "stopping" : "returning 0",
            !stop && (d->flags & MT_IMPORT_STRET) != 0u ? " (struct result left unwritten)" : "");
        log_line(s.cfg, line);
    }
    if (stop) {
        mt_trap(cpu, MT_TRAP_IMPORT, lr, id);
    }
    cpu->r[0] = 0;
    cpu->r[1] = 0;
}

extern "C" std::uint32_t mt_import_unimplemented_calls(std::uint32_t id) noexcept {
    return g_state.calls != nullptr && id < g_state.count ? g_state.calls[id] : 0u;
}

extern "C" int mt_imports_place(mt_cpu *cpu, const mt_import_desc *descs, std::uint32_t count,
                                const mt_hle_entry *hle, std::uint32_t hle_count,
                                mt_guest_alloc_fn alloc, void *alloc_user,
                                std::uint32_t *addrs) noexcept {
    for (std::uint32_t id = 0; id < count; id++) {
        const mt_import_desc &d = descs[id];
        addrs[id] = 0;
        if ((d.flags & MT_IMPORT_DATA) == 0u) {
            addrs[id] = d.addr;
            continue;
        }
        const std::uint32_t size = d.size != 0u ? d.size : 1u;
        const std::uint32_t align = d.align != 0u ? d.align : 1u;
        const std::uint32_t at = alloc != nullptr ? alloc(alloc_user, size, align) : 0u;
        if (at == 0u || (at & (align - 1u)) != 0u || !in_guest(cpu, at, size)) {
            char line[256];
            std::snprintf(line, sizeof line,
                          "cannot place data import %s (%u bytes, align %u): got 0x%08X", d.name,
                          static_cast<unsigned>(size), static_cast<unsigned>(align),
                          static_cast<unsigned>(at));
            log_line(g_state.cfg, line);
            return -1;
        }
        std::memset(cpu->mem + at, 0, size);
        addrs[id] = at;
    }
    for (std::uint32_t id = 0; id < count; id++) {
        if ((descs[id].flags & MT_IMPORT_DATA) == 0u) {
            continue;
        }
        const mt_hle_entry *e = find(hle, hle_count, descs[id].name);
        if (e != nullptr && e->init != nullptr) {
            e->init(cpu, id, addrs[id]);
        }
    }
    return 0;
}

extern "C" int mt_imports_apply_binds(mt_cpu *cpu, const mt_import_bind *binds,
                                      std::uint32_t bind_count, const std::uint32_t *addrs,
                                      std::uint32_t count) noexcept {
    int status = 0;
    for (std::uint32_t i = 0; i < bind_count; i++) {
        const mt_import_bind &b = binds[i];
        if (b.id >= count || !in_guest(cpu, b.slot, 4u)) {
            status = -1;
            continue;
        }
        const std::uint32_t value = addrs[b.id] + static_cast<std::uint32_t>(b.addend);
        std::memcpy(cpu->mem + b.slot, &value, 4);
    }
    return status;
}
