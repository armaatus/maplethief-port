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
// The fake receipt server (#156, ADR 0072, docs/re/cash-shop.md). A connect to
// 50.17.236.149:20001 succeeds when one of the game's receipt lists (crestore, riprestore1,
// riprestore2, totalrestore in Documents/) holds a synthetic receipt of the port's store
// (maplethief/stubs.h): a purchase of the offline cash shop, or one it left behind for a
// restore. Every other connect fails at once as before (the Pigmy hatch, popup 0x16, a real
// App Store receipt). Then, like a server on the other end:
//   - the next pump calls the socket's callout (CMSocket_Callback 0x1C651) with type 4
//     (connect) and type 8 (write), which make CMSocket's status 1 (CMSocket::OnSocketEvent);
//   - CFSocketSendData reads the client's frames ({u32 length incl. this header, u16 0xFFFF,
//     payload}, payload {u16 op, ...}) and the next pump answers each through the callout,
//     type 3 with a CFData frame {u32 length, u16 0, u16 op, u8 status, u8 len, char id[len]}
//     that CMSocket::OnData hands to CDataMgr::OnServerPacket:
//       op 0 (hello)                 -> op 1, status 0
//       op 2 (receipt), 4 (restore)  -> op 3 / op 5, status 0 and the product id the synthetic
//         {u16 len, receipt}            receipt names; any other receipt: status 1, which the
//                                       game ends with Disconnect(true) and popup 0x20, as the
//                                       dead server did
//       op 0xB4 (keep-alive)         -> nothing;
//   - CFSocketInvalidate (CMSocket::Disconnect after the grant) ends it.
//
// The SCNetworkReachability* functions are Chartboost's only (CBReachability), which never
// runs while +[Chartboost sharedChartboost] returns nil; they answer "not reachable" in case
// it ever does (the creation functions return NULL, which CBReachability treats as no
// reachability object).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cf/cf_objects.h"
#include "hle/hle.h"
#include "maplethief/hle/abi.h"
#include "maplethief/saves.h"
#include "stubs/stubs_internal.h"

namespace mt_stubs_impl {
namespace {

constexpr std::int32_t k_cf_socket_error = -1; // kCFSocketError

struct SocketState {
    u32 callout; // the CFSocketCallBack: called by the fake server only
    std::int32_t family, type;
    bool invalid;
    bool connected; // to the fake server
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

// --- the fake receipt server -------------------------------------------------------------------

mt_stubs_server_stats g_server{};

// CFSocketCallBackType
constexpr u32 k_cb_data = 3, k_cb_connect = 4, k_cb_write = 8;
// The server: 50.17.236.149 port 20001 (CDataMgr::ConnectServer 0x37B55).
constexpr std::uint8_t k_server_addr[4] = {50, 17, 236, 149};
constexpr std::uint16_t k_server_port = 20001;

std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}
std::uint16_t le16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }

// Whether `address` (CFData of a struct sockaddr_in) is the receipt server's.
bool is_server(mt_cpu *cpu, u32 address) {
    u32 bytes = 0, n = 0;
    if (address == 0 || !mt_cf::data_bytes(cpu, address, &bytes, &n) || n < 8) {
        return false;
    }
    const std::uint8_t *a = cpu->mem + bytes;
    const std::uint16_t port = static_cast<std::uint16_t>(a[2] << 8 | a[3]); // network order
    return a[1] == 2 /* AF_INET */ && port == k_server_port &&
           std::memcmp(a + 4, k_server_addr, 4) == 0;
}

// Whether the receipt list file `name` in Documents/ (docs/formats/save.md: i16 count, then
// {u8 flag, i16 len, bytes}) holds a synthetic receipt.
bool list_has_synthetic(const char *name) {
    const char *docs = mt_saves_documents_dir();
    char path[1100];
    if (docs == nullptr || docs[0] == '\0' ||
        std::snprintf(path, sizeof path, "%s/%s", docs, name) >= static_cast<int>(sizeof path)) {
        return false;
    }
    std::size_t size = 0;
    char *text = read_file(path, &size);
    if (text == nullptr) {
        return false;
    }
    const auto *b = reinterpret_cast<const std::uint8_t *>(text);
    bool found = false;
    if (size >= 2) {
        const int count = static_cast<std::int16_t>(le16(b));
        std::size_t at = 2;
        for (int i = 0; i < count && at + 3 <= size && !found; i++) {
            const std::size_t len = le16(b + at + 1);
            at += 3;
            if (at + len > size) {
                break;
            }
            char id[128];
            found = mt_stubs_receipt_product(b + at, static_cast<u32>(len), id, sizeof id) != 0;
            at += len;
        }
    }
    std::free(text);
    return found;
}

bool synthetic_receipt_pending() {
    for (const char *name : {"crestore", "riprestore1", "riprestore2", "totalrestore"}) {
        if (list_has_synthetic(name)) {
            return true;
        }
    }
    return false;
}

// The socket's callout(s, type, NULL, data) while it is connected to the fake server.
void callout(mt_cpu *cpu, u32 s, u32 type, u32 data) {
    const SocketState *st = socket_of(s);
    if (st == nullptr || st->invalid || !st->connected || st->callout == 0) {
        return;
    }
    const u32 args[4] = {s, type, 0u, data};
    mt_call_guest(cpu, st->callout, args, 4, st->callout & ~1u);
}

// The connect's events: words = {socket (retained)}.
void deliver_connected(mt_cpu *cpu, const u32 *w) {
    callout(cpu, w[0], k_cb_connect, 0);
    callout(cpu, w[0], k_cb_write, 0);
    mt_objc_release(cpu, w[0]);
}

// A server frame: words = {socket (retained), CFData (owned)}.
void deliver_answer(mt_cpu *cpu, const u32 *w) {
    const SocketState *st = socket_of(w[0]);
    if (st != nullptr && !st->invalid && st->connected) {
        g_server.answers++;
    }
    callout(cpu, w[0], k_cb_data, w[1]);
    mt_objc_release(cpu, w[1]);
    mt_objc_release(cpu, w[0]);
}

// Queues the server frame with `payload` for the next pump.
void answer(mt_cpu *cpu, u32 s, const std::uint8_t *payload, u32 n) {
    std::uint8_t frame[6 + 4 + 256];
    if (n > sizeof frame - 6) {
        return;
    }
    const u32 total = 6 + n;
    for (u32 i = 0; i < 4; i++) {
        frame[i] = static_cast<std::uint8_t>(total >> (8 * i));
    }
    frame[4] = frame[5] = 0;
    std::memcpy(frame + 6, payload, n);
    const u32 data = mt_cf::data_create(cpu, frame, total, false);
    if (data == 0) {
        log("fake server: out of memory: an answer is dropped");
        return;
    }
    const u32 words[2] = {mt_objc_retain(cpu, s), data};
    if (!post(deliver_answer, words, 2)) {
        mt_objc_release(cpu, data);
        mt_objc_release(cpu, s);
    }
}

// One client payload (after the frame's 6-byte header).
void serve(mt_cpu *cpu, u32 s, const std::uint8_t *p, u32 n) {
    if (n < 2) {
        return;
    }
    const std::uint16_t op = le16(p);
    std::uint8_t out[4 + 128] = {};
    if (op == 0) {
        out[0] = 1; // op 1, status 0: hello acknowledged
        log("fake server: hello (op 0) -> op 1, status 0");
        answer(cpu, s, out, 4);
    } else if (op == 2 || op == 4) {
        const u32 len = n >= 4 ? le16(p + 2) : 0u;
        const u32 have = n >= 4 ? (len <= n - 4 ? len : n - 4) : 0u;
        char id[128];
        const bool ours = mt_stubs_receipt_product(p + 4, have, id, sizeof id) != 0;
        out[0] = static_cast<std::uint8_t>(op + 1);
        out[2] = ours ? 0 : 1;
        u32 size = 4;
        if (ours) {
            const std::size_t idn = std::strlen(id);
            out[3] = static_cast<std::uint8_t>(idn);
            std::memcpy(out + 4, id, idn);
            size += static_cast<u32>(idn);
            g_server.confirmed++;
            log("fake server: receipt (op %u) for %s -> op %u, status 0", op, id, op + 1);
        } else {
            g_server.rejected++;
            log("fake server: a receipt that is not the port's (op %u, %u bytes) -> op %u, "
                "status 1 (the dead server's popup 0x20)",
                op, len, op + 1);
        }
        answer(cpu, s, out, size);
    } else if (op != 0xB4) {
        log("fake server: client op %u: no answer", op);
    }
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
// fails at once, offline, unless a synthetic receipt waits for the receipt server (the fake
// server above): then 0, and the connect's events on the next pump.
void mt_hle_CFSocketConnectToAddress(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 s = mt_arg_u32(&a);
    const u32 address = mt_arg_u32(&a);
    const double timeout = mt_arg_f64(&a);
    g_server.connects++;
    SocketState *st = socket_of(s);
    if (st != nullptr && !st->invalid && is_server(cpu, address) && synthetic_receipt_pending()) {
        st->connected = true;
        const u32 words[1] = {mt_objc_retain(cpu, s)};
        if (post(deliver_connected, words, 1)) {
            g_server.accepted++;
            log("CFSocketConnectToAddress from 0x%08X: a synthetic receipt waits: connected to "
                "the fake receipt server (ADR 0072)",
                static_cast<unsigned>(cpu->r[MT_LR]));
            mt_ret_u32(cpu, 0u);
            return;
        }
        mt_objc_release(cpu, s);
        st->connected = false;
    }
    log("CFSocketConnectToAddress from 0x%08X (timeout %g s): offline, it fails at once",
        static_cast<unsigned>(cpu->r[MT_LR]), timeout);
    mt_ret_u32(cpu, static_cast<u32>(k_cf_socket_error));
}

// CFRunLoopSourceRef CFSocketCreateRunLoopSource(CFAllocatorRef, CFSocketRef, CFIndex order):
// a source that never fires (the fake server calls the socket's callout from the stubs' pump).
// CFRunLoopAddSource (#48) accepts it like any other.
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
//     CFTimeInterval timeout): to the fake server while connected to it (its answers come on
// the next pump), else kCFSocketError.
void mt_hle_CFSocketSendData(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 s = mt_arg_u32(&a);
    mt_arg_u32(&a); // address
    const u32 data = mt_arg_u32(&a);
    const SocketState *st = socket_of(s);
    u32 bytes = 0, n = 0;
    if (st == nullptr || st->invalid || !st->connected ||
        !mt_cf::data_bytes(cpu, data, &bytes, &n)) {
        mt_ret_u32(cpu, static_cast<u32>(k_cf_socket_error));
        return;
    }
    const std::uint8_t *b = cpu->mem + bytes;
    for (u32 at = 0; at + 6 <= n;) {
        const u32 len = le32(b + at);
        if (len < 8 || len > n - at) {
            log("fake server: a malformed frame (length %u at %u of %u): ignored",
                static_cast<unsigned>(len), static_cast<unsigned>(at), static_cast<unsigned>(n));
            break;
        }
        g_server.frames++;
        serve(cpu, s, b + at + 6, len - 6);
        at += len;
    }
    mt_ret_u32(cpu, 0u);
}

// void CFSocketInvalidate(CFSocketRef)
void mt_hle_CFSocketInvalidate(mt_cpu *cpu, std::uint32_t) noexcept {
    if (SocketState *st = socket_of(cpu->r[0])) {
        st->invalid = true;
        st->connected = false;
    }
}

mt_stubs_server_stats mt_stubs_get_server_stats(void) noexcept { return g_server; }

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
