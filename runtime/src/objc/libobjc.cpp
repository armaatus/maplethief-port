// libobjc's helpers for compiled code (ADR 0029): synthesized property accessors
// (objc_getProperty / objc_setProperty, 42 + 99 call sites), @synchronized
// (objc_sync_enter / objc_sync_exit, only +[EAGLView_Instance allocWithZone:] and +Instance)
// and the fast-enumeration mutation check. One guest thread (ADR 0014): an atomic property
// needs no lock, and @synchronized is a recursive count per object.
#include <cstdint>
#include <cstdio>

#include "objc/objc_internal.h"

using namespace mt_objc_impl;

namespace {

u32 sel1(mt_objc *rt, const char *name) {
    const long id = sel_id_of(rt, name);
    return id >= 0 ? static_cast<u32>(id) + 1 : 0;
}

constexpr int k_sync_success = 0;
constexpr int k_sync_not_owning = -1; // OBJC_SYNC_NOT_OWNING_THREAD_ERROR

} // namespace

extern "C" void mt_hle_objc_getProperty(mt_cpu *cpu, u32) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 self = mt_arg_u32(&a);
    (void)mt_arg_u32(&a); // _cmd
    const u32 offset = mt_arg_u32(&a);
    const u32 atomic = mt_arg_u32(&a) & 0xFFu;
    const u32 lr = cpu->r[MT_LR];
    const u32 value = mt_ld32(cpu, self + offset, lr);
    mt_objc *rt = mt_objc_current();
    if (atomic == 0 || value == 0 || rt == nullptr) {
        cpu->r[0] = value;
        return;
    }
    // [[value retain] autorelease]: the caller's value survives a concurrent set.
    const u32 kept = host_send(cpu, rt, value, -1, sel1(rt, "retain"), nullptr, 0);
    cpu->r[0] = host_send(cpu, rt, kept, -1, sel1(rt, "autorelease"), nullptr, 0);
}

extern "C" void mt_hle_objc_setProperty(mt_cpu *cpu, u32) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 self = mt_arg_u32(&a);
    (void)mt_arg_u32(&a); // _cmd
    const u32 offset = mt_arg_u32(&a);
    u32 value = mt_arg_u32(&a);
    (void)mt_arg_u32(&a); // atomic: no lock needed
    const u32 copy = mt_arg_u32(&a) & 0xFFu;
    const u32 lr = cpu->r[MT_LR];
    mt_objc *rt = mt_objc_current();
    if (rt == nullptr) {
        mt_st32(cpu, self + offset, value, lr);
        return;
    }
    const u32 zone = 0;
    if (copy == 1) {
        value = host_send(cpu, rt, value, -1, sel1(rt, "copyWithZone:"), &zone, 1);
    } else if (copy != 0) {
        value = host_send(cpu, rt, value, -1, sel1(rt, "mutableCopyWithZone:"), &zone, 1);
    } else {
        value = host_send(cpu, rt, value, -1, sel1(rt, "retain"), nullptr, 0);
    }
    const u32 old = mt_ld32(cpu, self + offset, lr);
    mt_st32(cpu, self + offset, value, lr);
    host_send(cpu, rt, old, -1, sel1(rt, "release"), nullptr, 0);
}

extern "C" void mt_hle_objc_sync_enter(mt_cpu *cpu, u32) noexcept {
    const u32 obj = cpu->r[0];
    mt_objc *rt = mt_objc_current();
    if (obj != 0 && rt != nullptr) {
        u32 depth = 0;
        map_get(rt->syncs, obj, &depth);
        map_put(rt->syncs, obj, depth + 1);
    }
    cpu->r[0] = static_cast<u32>(k_sync_success);
}

extern "C" void mt_hle_objc_sync_exit(mt_cpu *cpu, u32) noexcept {
    const u32 obj = cpu->r[0];
    mt_objc *rt = mt_objc_current();
    int result = k_sync_success;
    if (obj != 0 && rt != nullptr) {
        u32 depth = 0;
        if (!map_get(rt->syncs, obj, &depth)) {
            char line[160];
            std::snprintf(line, sizeof line,
                          "objc: objc_sync_exit(0x%08X) without objc_sync_enter (called from "
                          "0x%08X)",
                          obj, cpu->r[MT_LR]);
            log_line(rt, line);
            result = k_sync_not_owning;
        } else if (depth == 1) {
            map_erase(rt->syncs, obj);
        } else {
            map_put(rt->syncs, obj, depth - 1);
        }
    }
    cpu->r[0] = static_cast<u32>(result);
}

extern "C" void mt_hle_objc_enumerationMutation(mt_cpu *cpu, u32) noexcept {
    const u32 obj = cpu->r[0];
    const u32 lr = cpu->r[MT_LR];
    mt_objc *rt = mt_objc_current();
    char line[200];
    std::snprintf(line, sizeof line,
                  "objc: collection 0x%08X was mutated while being enumerated (called from "
                  "0x%08X): stopping",
                  obj, lr);
    if (rt != nullptr) {
        log_line(rt, line);
    } else {
        std::fputs(line, stderr);
        std::fputc('\n', stderr);
    }
    mt_trap(cpu, MT_TRAP_OBJC, lr, obj);
}
