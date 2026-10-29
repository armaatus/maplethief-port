// CFData (#48, ADR 0032): __NSCFData keeps its bytes in one guest heap block, so the pointer
// CFDataGetBytePtr returns is a guest address; the block is freed when the object is
// deallocated. The game reads every resource file through it (CFDataGetLength, then
// CFDataGetBytes into its own buffer).
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "cf/cf_hle.h"
#include "cf/cf_objects.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"

namespace mt_cf {

namespace {

struct DataState {
    u32 bytes;    // guest heap block, 0 for none (length 0)
    u32 length;   // bytes in use
    u32 capacity; // bytes of the block
    bool is_mutable;
};

void destroy_data(mt_cpu *cpu, u32, void *p) {
    const auto *s = static_cast<const DataState *>(p);
    if (s->bytes != 0) {
        mt_heap_free(heap(cpu), s->bytes);
    }
}

const mt_objc_state_type k_data_state = {"CFData", sizeof(DataState), destroy_data};

DataState *state_of(mt_cpu *cpu, u32 obj) {
    mt_objc *rt = runtime(cpu);
    if (rt == nullptr || obj == 0) {
        return nullptr;
    }
    return static_cast<DataState *>(mt_objc_find_state(rt, obj, &k_data_state));
}

// Makes room for `length` bytes (zeroing new ones); false if out of guest memory.
bool reserve(mt_cpu *cpu, DataState *s, u32 length) {
    if (length <= s->capacity) {
        return true;
    }
    const u32 capacity = std::max(length, s->capacity < 0x40000000u ? s->capacity * 2u : length);
    const u32 block = mt_heap_calloc(heap(cpu), 1, capacity);
    if (block == 0) {
        return false;
    }
    if (s->bytes != 0) {
        std::memcpy(cpu->mem + block, cpu->mem + s->bytes, s->length);
        mt_heap_free(heap(cpu), s->bytes);
    }
    s->bytes = block;
    s->capacity = capacity;
    return true;
}

} // namespace

u32 data_create(mt_cpu *cpu, const std::uint8_t *bytes, u32 n, bool is_mutable) {
    const u32 obj = create(cpu, classes(cpu).data);
    auto *s = obj != 0 ? static_cast<DataState *>(mt_objc_state(runtime(cpu), obj, &k_data_state))
                       : nullptr;
    if (s == nullptr) {
        return 0;
    }
    s->is_mutable = is_mutable;
    // At least one byte, so that CFDataGetBytePtr of empty data is a valid pointer too.
    if (!reserve(cpu, s, n != 0 ? n : 1u)) {
        log(cpu, "no guest memory for %u bytes of CFData", static_cast<unsigned>(n));
        mt_objc_release(cpu, obj);
        return 0;
    }
    if (n != 0) {
        std::memcpy(cpu->mem + s->bytes, bytes, n);
    }
    s->length = n;
    return obj;
}

bool data_bytes(mt_cpu *cpu, u32 obj, u32 *guest_bytes, u32 *length) {
    const DataState *s = state_of(cpu, obj);
    if (s == nullptr) {
        return false;
    }
    *guest_bytes = s->bytes;
    *length = s->length;
    return true;
}

bool data_assign(mt_cpu *cpu, u32 obj, const std::uint8_t *bytes, u32 n) {
    DataState *s = state_of(cpu, obj);
    if (s == nullptr || !reserve(cpu, s, n != 0 ? n : 1u)) {
        return false;
    }
    if (n != 0) {
        std::memcpy(cpu->mem + s->bytes, bytes, n);
    }
    s->length = n;
    return true;
}

bool data_set_length(mt_cpu *cpu, u32 obj, u32 length) {
    DataState *s = state_of(cpu, obj);
    if (s == nullptr || !s->is_mutable || !reserve(cpu, s, length)) {
        return false;
    }
    if (length > s->length) {
        std::memset(cpu->mem + s->bytes + s->length, 0, length - s->length);
    }
    s->length = length;
    return true;
}

bool data_append(mt_cpu *cpu, u32 obj, const std::uint8_t *bytes, u32 n) {
    DataState *s = state_of(cpu, obj);
    if (s == nullptr || !s->is_mutable || s->length > 0xFFFFFFFFu - n ||
        !reserve(cpu, s, s->length + n)) {
        return false;
    }
    std::memcpy(cpu->mem + s->bytes + s->length, bytes, n);
    s->length += n;
    return true;
}

} // namespace mt_cf

using namespace mt_cf;

namespace {

// Stops the guest unless `obj` is a CFData (CF would crash on it).
void checked(mt_cpu *cpu, const char *function, u32 obj) {
    u32 bytes, length;
    if (obj == 0) {
        misuse(cpu, function, "NULL", 0);
    }
    if (!data_bytes(cpu, obj, &bytes, &length)) {
        misuse(cpu, function, "not a CFData", obj);
    }
}

} // namespace

// CFDataRef CFDataCreate(CFAllocatorRef, const UInt8 *bytes, CFIndex length)
extern "C" void mt_hle_CFDataCreate(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    (void)mt_arg_u32(&a);
    const u32 bytes = mt_arg_u32(&a);
    const std::int32_t length = mt_arg_i32(&a);
    if (length < 0) {
        misuse(cpu, "CFDataCreate", "negative length", static_cast<u32>(length));
    }
    const u32 n = static_cast<u32>(length);
    const std::uint8_t *p = n != 0 ? mt_g2h(cpu, bytes, n) : nullptr;
    mt_ret_u32(cpu, data_create(cpu, p, n, false));
}

// CFIndex CFDataGetLength(CFDataRef)
extern "C" void mt_hle_CFDataGetLength(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 obj = cpu->r[0];
    checked(cpu, "CFDataGetLength", obj);
    u32 bytes = 0, length = 0;
    data_bytes(cpu, obj, &bytes, &length);
    mt_ret_u32(cpu, length);
}

// const UInt8 *CFDataGetBytePtr(CFDataRef)
extern "C" void mt_hle_CFDataGetBytePtr(mt_cpu *cpu, std::uint32_t) noexcept {
    const u32 obj = cpu->r[0];
    checked(cpu, "CFDataGetBytePtr", obj);
    u32 bytes = 0, length = 0;
    data_bytes(cpu, obj, &bytes, &length);
    mt_ret_u32(cpu, bytes);
}

// void CFDataGetBytes(CFDataRef, CFRange range, UInt8 *buffer): range {location, length} by
// value in r1, r2; buffer in r3.
extern "C" void mt_hle_CFDataGetBytes(mt_cpu *cpu, std::uint32_t) noexcept {
    mt_args a = mt_args_begin(cpu);
    const u32 obj = mt_arg_u32(&a);
    const std::int32_t location = mt_arg_i32(&a);
    const std::int32_t length = mt_arg_i32(&a);
    const u32 buffer = mt_arg_u32(&a);
    checked(cpu, "CFDataGetBytes", obj);
    u32 bytes = 0, size = 0;
    data_bytes(cpu, obj, &bytes, &size);
    if (location < 0 || length < 0 ||
        static_cast<std::uint64_t>(location) + static_cast<u32>(length) > size) {
        // CF's range check is an assertion, compiled out on the device: it copies anyway.
        // Copy what exists, and say so.
        log(cpu,
            "CFDataGetBytes: range {%d, %d} outside the %u bytes of 0x%08X (from 0x%08X); "
            "copying the bytes inside it",
            static_cast<int>(location), static_cast<int>(length), static_cast<unsigned>(size),
            static_cast<unsigned>(obj), static_cast<unsigned>(cpu->r[MT_LR]));
    }
    if (location < 0 || length <= 0 || static_cast<u32>(location) >= size) {
        return;
    }
    const u32 n = std::min(static_cast<u32>(length), size - static_cast<u32>(location));
    std::uint8_t *dst = mt_g2h(cpu, buffer, n);
    std::memmove(dst, cpu->mem + bytes + static_cast<u32>(location), n);
}
