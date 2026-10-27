// Network imports, offline (#52, docs/re/network.md): CFSocket fails fast and
// SystemConfiguration reachability says "not reachable".
//
// CFSocket is used only by CMSocket, the client of Nexon's receipt server 50.17.236.149:20001
// (purchases, restores, the Pigmy hatch dialog). Offline, CFSocketConnectToAddress returns
// kCFSocketError at once instead of blocking the main thread for its 10 s timeout, and
// CMSocket::Connect runs its own failure branch: the "Server connection has failed" popup
// 0x20, then control returns to the player. No callback ever fires. The socket and its
// run-loop source are plain NSObject instances, so that a toll-free CFRelease (#48) of them
// works like any object's.
//
// The SCNetworkReachability* functions are Chartboost's only (CBReachability), which never
// runs while +[Chartboost sharedChartboost] returns nil; they answer "not reachable" in case
// it ever does (the creation functions return NULL, which CBReachability treats as no
// reachability object).
#include <cstdint>

#include "hle/hle.h"
#include "maplethief/hle/abi.h"
#include "stubs/stubs_internal.h"

namespace mt_stubs_impl {
namespace {

constexpr std::int32_t k_cf_socket_error = -1; // kCFSocketError

struct SocketState {
    u32 callout; // the CFSocketCallBack, never called offline
    std::int32_t family, type;
    bool invalid;
};
struct SourceState {
    u32 socket;
};

const mt_objc_state_type k_socket_state = {"CFSocket (offline)", sizeof(SocketState), nullptr};
const mt_objc_state_type k_source_state = {"CFRunLoopSource of a CFSocket (offline)",
                                           sizeof(SourceState), nullptr};

u32 new_object() {
    mt_objc *rt = runtime();
    return mt_objc_create_instance(rt, mt_objc_class_named(rt, "NSObject"));
}

SocketState *socket_of(u32 s) {
    return s != 0 ? static_cast<SocketState *>(mt_objc_find_state(runtime(), s, &k_socket_state))
                  : nullptr;
}

} // namespace
} // namespace mt_stubs_impl

using namespace mt_stubs_impl;

extern "C" {

// CFSocketRef CFSocketCreate(CFAllocatorRef, SInt32 protocolFamily, SInt32 socketType,
//     SInt32 protocol, CFOptionFlags callBackTypes, CFSocketCallBack callout,
//     const CFSocketContext *context)
void mt_hle_CFSocketCreate(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_arg_u32(&a); // allocator
    const std::int32_t family = mt_arg_i32(&a), type = mt_arg_i32(&a);
    mt_arg_u32(&a); // protocol
    mt_arg_u32(&a); // callBackTypes
    const u32 callout = mt_arg_u32(&a);
    const u32 s = new_object();
    auto *st =
        s != 0 ? static_cast<SocketState *>(mt_objc_state(runtime(), s, &k_socket_state)) : nullptr;
    if (st != nullptr) {
        st->callout = callout;
        st->family = family;
        st->type = type;
    }
    mt_ret_u32(cpu, s);
}

// CFSocketNativeHandle CFSocketGetNative(CFSocketRef): there is no native socket.
void mt_hle_CFSocketGetNative(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, static_cast<u32>(-1));
}

// CFSocketError CFSocketConnectToAddress(CFSocketRef, CFDataRef address, CFTimeInterval):
// fails at once, offline.
void mt_hle_CFSocketConnectToAddress(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 s = mt_arg_u32(&a);
    mt_arg_u32(&a); // address
    const double timeout = mt_arg_f64(&a);
    log("CFSocketConnectToAddress from 0x%08X (timeout %g s): offline, it fails at once",
        static_cast<unsigned>(cpu->r[MT_LR]), timeout);
    (void)socket_of(s);
    mt_ret_u32(cpu, static_cast<u32>(k_cf_socket_error));
}

// CFRunLoopSourceRef CFSocketCreateRunLoopSource(CFAllocatorRef, CFSocketRef, CFIndex order):
// a source that never fires. CFRunLoopAddSource (#48) accepts it like any other.
void mt_hle_CFSocketCreateRunLoopSource(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    mt_arg_u32(&a); // allocator
    const u32 s = mt_arg_u32(&a);
    const u32 source = new_object();
    if (auto *st = source != 0 ? static_cast<SourceState *>(
                                     mt_objc_state(runtime(), source, &k_source_state))
                               : nullptr) {
        st->socket = s;
    }
    mt_ret_u32(cpu, source);
}

// CFSocketError CFSocketSendData(CFSocketRef, CFDataRef address, CFDataRef data,
//     CFTimeInterval timeout): never connected, so nothing is sent.
void mt_hle_CFSocketSendData(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, static_cast<u32>(k_cf_socket_error));
}

// void CFSocketInvalidate(CFSocketRef)
void mt_hle_CFSocketInvalidate(mt_cpu *cpu, std::uint32_t) noexcept {
    if (SocketState *st = socket_of(cpu->r[0])) {
        st->invalid = true;
    }
}

// SCNetworkReachabilityRef SCNetworkReachabilityCreateWithAddress(CFAllocatorRef,
//     const struct sockaddr *) and ...CreateWithName(CFAllocatorRef, const char *): NULL.
void mt_hle_SCNetworkReachabilityCreateWithAddress(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, 0u);
}
void mt_hle_SCNetworkReachabilityCreateWithName(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, 0u);
}

// Boolean SCNetworkReachabilityGetFlags(SCNetworkReachabilityRef, SCNetworkReachabilityFlags *):
// flags 0, "not reachable"; FALSE for a NULL target.
void mt_hle_SCNetworkReachabilityGetFlags(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 target = mt_arg_u32(&a), flags = mt_arg_u32(&a);
    if (flags != 0) {
        mt_st32(cpu, flags, 0u, cpu->r[MT_LR]);
    }
    mt_ret_u32(cpu, target != 0 ? 1u : 0u);
}

// SetCallback, ScheduleWithRunLoop, UnscheduleFromRunLoop: FALSE (no notifications offline).
void mt_hle_SCNetworkReachabilitySetCallback(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, 0u);
}
void mt_hle_SCNetworkReachabilityScheduleWithRunLoop(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, 0u);
}
void mt_hle_SCNetworkReachabilityUnscheduleFromRunLoop(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_ret_u32(cpu, 0u);
}

} // extern "C"
