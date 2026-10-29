// libSystem.B.dylib, the system part (ADR 0028): sysctl and sysctlbyname for the device the
// guest sees, if_nametoindex, and the BSD socket calls of CMSocket, stubbed offline as
// docs/re/network.md (#23) recommends.
//
// The device: hw.machine/hw.model/hw.ncpu/hw.physmem/hw.memsize from mt_libc_config, and one
// network interface, en0 (index 2; lo0 is 1), whose MAC address sysctl(NET_RT_IFLIST)
// reports. -[UIDevice(IdentifierAddition) macaddress] (0xC1025) and Chartboost's
// +[CBIdentity cbma] read the MAC that way: an if_msghdr (112 bytes on armv7) followed by a
// sockaddr_dl, whose link-level address follows the interface name (the game reads
// buf + 0x70 + 8 + sdl_nlen). Without it the device id is nil and the CStateTitle
// constructor (0x98865) crashes in strcpy.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hle/hle.h"
#include "hle/libc_internal.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"

namespace {

// <sys/sysctl.h>, <sys/socket.h>, <net/route.h>, <net/if_types.h>
constexpr std::uint32_t k_ctl_net = 4u, k_ctl_hw = 6u;
constexpr std::uint32_t k_hw_machine = 1u, k_hw_model = 2u, k_hw_ncpu = 3u, k_hw_physmem = 5u,
                        k_hw_memsize = 24u;
constexpr std::uint32_t k_af_route = 17u, k_af_link = 18u, k_net_rt_iflist = 3u;
constexpr std::uint32_t k_lo0_index = 1u, k_en0_index = 2u;

// Per device: CPUs and memory (bytes) for hw.ncpu, hw.physmem and hw.memsize.
struct Device {
    const char *machine;
    std::uint32_t ncpu;
    std::uint32_t memory;
};
constexpr Device k_devices[] = {
    {"iPhone2,1", 1u, 256u << 20}, {"iPhone3,1", 1u, 512u << 20}, {"iPhone3,3", 1u, 512u << 20},
    {"iPhone4,1", 2u, 512u << 20}, {"iPod3,1", 1u, 256u << 20},   {"iPod4,1", 1u, 256u << 20},
    {"iPod5,1", 2u, 512u << 20},   {"iPad1,1", 1u, 256u << 20},   {"iPad2,1", 2u, 512u << 20},
};

Device device() {
    const char *machine = mt_libc_cfg().hw_machine;
    for (const Device &d : k_devices) {
        if (std::strcmp(machine, d.machine) == 0) {
            return d;
        }
    }
    return Device{machine, 1u, 512u << 20};
}

void put16(std::string &b, std::size_t at, std::uint32_t v) {
    b[at] = static_cast<char>(v & 0xFFu);
    b[at + 1u] = static_cast<char>((v >> 8) & 0xFFu);
}
void put32(std::string &b, std::size_t at, std::uint32_t v) {
    put16(b, at, v & 0xFFFFu);
    put16(b, at + 2u, v >> 16);
}

std::string word(std::uint32_t v) {
    std::string b(4u, '\0');
    put32(b, 0u, v);
    return b;
}

std::string c_string(const char *s) { return std::string(s, std::strlen(s) + 1u); }

// The RTM_IFINFO message of en0: struct if_msghdr (armv7: 14 bytes of header, padded to 16,
// then the 96-byte if_data) and its sockaddr_dl (20 bytes: name "en0", the 6-byte MAC).
std::string en0_ifinfo() {
    constexpr std::size_t k_ifm = 112u, k_sdl = 20u;
    std::string b(k_ifm + k_sdl, '\0');
    put16(b, 0u, static_cast<std::uint32_t>(b.size())); // ifm_msglen
    b[2] = 5;                                           // ifm_version: RTM_VERSION
    b[3] = 0x0E;                                        // ifm_type: RTM_IFINFO
    put32(b, 4u, 0x10u);                                // ifm_addrs: RTA_IFP
    put32(b, 8u, 0x8863u);      // ifm_flags: UP BROADCAST NOTRAILERS RUNNING SIMPLEX MULTICAST
    put16(b, 12u, k_en0_index); // ifm_index
    b[16] = 6;                  // ifi_type: IFT_ETHER
    b[19] = 6;                  // ifi_addrlen
    b[20] = 14;                 // ifi_hdrlen
    put32(b, 24u, 1500u);       // ifi_mtu
    const std::size_t sdl = k_ifm;
    b[sdl + 0u] = static_cast<char>(k_sdl); // sdl_len
    b[sdl + 1u] = static_cast<char>(k_af_link);
    put16(b, sdl + 2u, k_en0_index);
    b[sdl + 4u] = 6; // sdl_type: IFT_ETHER
    b[sdl + 5u] = 3; // sdl_nlen: "en0"
    b[sdl + 6u] = 6; // sdl_alen
    b[sdl + 7u] = 0; // sdl_slen
    std::memcpy(&b[sdl + 8u], "en0", 3u);
    std::memcpy(&b[sdl + 11u], mt_libc_cfg().en0_mac, 6u);
    return b;
}

// The value of a sysctl node, or false if the guest asks for one we do not have.
bool node_by_mib(const std::uint32_t *mib, std::uint32_t n, std::string &out) {
    const Device d = device();
    if (n == 2u && mib[0] == k_ctl_hw) {
        switch (mib[1]) {
        case k_hw_machine:
            out = c_string(mt_libc_cfg().hw_machine);
            return true;
        case k_hw_model:
            out = c_string(mt_libc_hw_model(mt_libc_cfg().hw_machine));
            return true;
        case k_hw_ncpu:
            out = word(d.ncpu);
            return true;
        case k_hw_physmem:
            out = word(d.memory);
            return true;
        case k_hw_memsize:
            out = word(d.memory) + word(0u);
            return true;
        default:
            return false;
        }
    }
    if (n == 6u && mib[0] == k_ctl_net && mib[1] == k_af_route && mib[2] == 0u &&
        mib[4] == k_net_rt_iflist && (mib[3] == 0u || mib[3] == k_af_link)) {
        // mib[5]: an interface index, 0 for all. lo0 has no link-level address to report.
        out = mib[5] == 0u || mib[5] == k_en0_index ? en0_ifinfo() : std::string();
        return true;
    }
    return false;
}

bool node_by_name(const char *name, std::string &out) {
    static constexpr struct {
        const char *name;
        std::uint32_t mib1;
    } k_names[] = {
        {"hw.machine", k_hw_machine}, {"hw.model", k_hw_model},     {"hw.ncpu", k_hw_ncpu},
        {"hw.physmem", k_hw_physmem}, {"hw.memsize", k_hw_memsize},
    };
    for (const auto &n : k_names) {
        if (std::strcmp(name, n.name) == 0) {
            const std::uint32_t mib[2] = {k_ctl_hw, n.mib1};
            return node_by_mib(mib, 2u, out);
        }
    }
    return false;
}

// xnu's copy-out: oldp NULL asks for the size; a short buffer gets what fits and ENOMEM.
std::uint32_t copy_out(mt_cpu *cpu, const std::string &value, std::uint32_t oldp,
                       std::uint32_t oldlenp) {
    const auto size = static_cast<std::uint32_t>(value.size());
    if (oldlenp == 0u) {
        return oldp == 0u ? 0u : 0xFFFFFFFFu;
    }
    if (oldp == 0u) {
        std::memcpy(mt_g2h(cpu, oldlenp, 4u), &size, 4u);
        return 0u;
    }
    std::uint32_t room;
    std::memcpy(&room, mt_g2h(cpu, oldlenp, 4u), 4u);
    const std::uint32_t n = room < size ? room : size;
    if (n != 0u) {
        std::memcpy(mt_g2h(cpu, oldp, n), value.data(), n);
    }
    if (room < size) {
        return 0xFFFFFFFFu; // ENOMEM; *oldlenp keeps the buffer size
    }
    std::memcpy(cpu->mem + oldlenp, &size, 4u);
    return 0u;
}

// The guest's static inet_ntoa buffer (18 bytes in the guest heap), made on first use.
struct NtoaBuffer {
    const std::uint8_t *base = nullptr;
    std::uint32_t addr = 0;
};
NtoaBuffer g_ntoa;

// inet_aton's parser: one to four parts separated by dots, each decimal, octal (leading 0)
// or hex (0x); the last part fills the remaining bytes. Returns false if malformed.
bool parse_inet(const char *s, std::uint32_t *out) {
    std::uint64_t parts[4];
    int n = 0;
    for (;;) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        std::uint64_t v = 0;
        unsigned base = 10u;
        if (*s == '0') {
            base = 8u;
            s++;
            if (*s == 'x' || *s == 'X') {
                base = 16u;
                s++;
            }
        }
        for (;; s++) {
            unsigned digit;
            if (*s >= '0' && *s <= '9' && static_cast<unsigned>(*s - '0') < base) {
                digit = static_cast<unsigned>(*s - '0');
            } else if (base == 16u && *s >= 'a' && *s <= 'f') {
                digit = static_cast<unsigned>(*s - 'a' + 10);
            } else if (base == 16u && *s >= 'A' && *s <= 'F') {
                digit = static_cast<unsigned>(*s - 'A' + 10);
            } else {
                break;
            }
            v = v * base + digit;
            if (v > 0xFFFFFFFFull) {
                return false;
            }
        }
        if (n == 4) {
            return false;
        }
        parts[n++] = v;
        if (*s != '.') {
            break;
        }
        s++;
    }
    if (*s != 0 && *s != ' ' && !(*s >= '\t' && *s <= '\r')) {
        return false;
    }
    std::uint64_t host = parts[n - 1];
    const std::uint64_t limit[4] = {0xFFFFFFFFull, 0xFFFFFFull, 0xFFFFull, 0xFFull};
    if (host > limit[n - 1]) {
        return false;
    }
    for (int i = 0; i < n - 1; i++) {
        if (parts[i] > 0xFFu) {
            return false;
        }
        host |= parts[i] << (24 - 8 * i);
    }
    // Network byte order in a little-endian guest word: the first octet is the low byte.
    const auto h = static_cast<std::uint32_t>(host);
    *out = (h >> 24) | ((h >> 8) & 0xFF00u) | ((h << 8) & 0xFF0000u) | (h << 24);
    return true;
}

} // namespace

extern "C" {

// int sysctl(int *name, u_int namelen, void *oldp, size_t *oldlenp, void *newp,
//            size_t newlen)
void mt_hle_sysctl(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t name = mt_arg_u32(&a), namelen = mt_arg_u32(&a);
    const std::uint32_t oldp = mt_arg_u32(&a), oldlenp = mt_arg_u32(&a);
    const std::uint32_t newp = mt_arg_u32(&a);
    std::uint32_t mib[12] = {};
    const std::uint32_t n = namelen < 12u ? namelen : 12u;
    if (n != 0u) {
        std::memcpy(mib, mt_g2h(cpu, name, 4u * n), 4u * n);
    }
    std::string value;
    if (newp != 0u || namelen > 12u || !node_by_mib(mib, n, value)) {
        mt_libc_log("sysctl {%u, %u, %u, %u, %u, %u}%s (%u names) from 0x%08X: not available",
                    mib[0], mib[1], mib[2], mib[3], mib[4], mib[5],
                    newp != 0u ? " with a new value" : "", static_cast<unsigned>(namelen),
                    static_cast<unsigned>(cpu->r[MT_LR]));
        mt_ret_u32(cpu, 0xFFFFFFFFu);
        return;
    }
    mt_ret_u32(cpu, copy_out(cpu, value, oldp, oldlenp));
}

// int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen)
void mt_hle_sysctlbyname(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const char *name = mt_g2h_str(cpu, mt_arg_u32(&a));
    const std::uint32_t oldp = mt_arg_u32(&a), oldlenp = mt_arg_u32(&a);
    const std::uint32_t newp = mt_arg_u32(&a);
    std::string value;
    if (newp != 0u || !node_by_name(name, value)) {
        mt_libc_log("sysctlbyname \"%s\"%s from 0x%08X: not available", name,
                    newp != 0u ? " with a new value" : "", static_cast<unsigned>(cpu->r[MT_LR]));
        mt_ret_u32(cpu, 0xFFFFFFFFu);
        return;
    }
    mt_ret_u32(cpu, copy_out(cpu, value, oldp, oldlenp));
}

// unsigned if_nametoindex(const char *name): lo0 1, en0 2, anything else 0.
void mt_hle_if_nametoindex(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const char *name = mt_g2h_str(cpu, mt_arg_u32(&a));
    mt_ret_u32(cpu, std::strcmp(name, "en0") == 0   ? k_en0_index
                    : std::strcmp(name, "lo0") == 0 ? k_lo0_index
                                                    : 0u);
}

// struct hostent *gethostbyname(const char *name): offline, no host resolves (the game's
// only call is dead code, docs/re/network.md).
void mt_hle_gethostbyname(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const char *name = mt_g2h_str(cpu, mt_arg_u32(&a));
    mt_libc_log("gethostbyname(\"%s\") from 0x%08X: offline, no such host", name,
                static_cast<unsigned>(cpu->r[MT_LR]));
    mt_ret_u32(cpu, 0u);
}

// in_addr_t inet_addr(const char *cp): INADDR_NONE (0xFFFFFFFF) if malformed.
void mt_hle_inet_addr(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    std::uint32_t v = 0;
    mt_ret_u32(cpu, parse_inet(mt_g2h_str(cpu, mt_arg_u32(&a)), &v) ? v : 0xFFFFFFFFu);
}

// char *inet_ntoa(struct in_addr in): the struct is one word, passed by value; the result
// is a static buffer, overwritten by the next call.
void mt_hle_inet_ntoa(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const std::uint32_t in = mt_arg_u32(&a);
    if (g_ntoa.base != cpu->mem) {
        g_ntoa = NtoaBuffer{cpu->mem, 0u};
    }
    if (g_ntoa.addr == 0u) {
        mt_mem *mem = mt_mem_from_cpu(cpu);
        g_ntoa.addr = mem != nullptr ? mt_heap_calloc(mem, 1u, 18u) : 0u;
        if (g_ntoa.addr == 0u) {
            mt_libc_log("inet_ntoa: no guest memory for its buffer");
            mt_ret_u32(cpu, 0u);
            return;
        }
    }
    char text[18];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u", in & 0xFFu, (in >> 8) & 0xFFu,
                  (in >> 16) & 0xFFu, in >> 24);
    std::memcpy(mt_g2h(cpu, g_ntoa.addr, 18u), text, sizeof text);
    mt_ret_u32(cpu, g_ntoa.addr);
}

// int setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen):
// CMSocket's SO_NOSIGPIPE and SO_LINGER; accepted and ignored.
void mt_hle_setsockopt(mt_cpu *cpu, std::uint32_t) noexcept { mt_ret_u32(cpu, 0u); }

// int close(int fd): only CMSocket's socket, which never connects offline.
void mt_hle_close(mt_cpu *cpu, std::uint32_t) noexcept { mt_ret_u32(cpu, 0u); }

} // extern "C"
