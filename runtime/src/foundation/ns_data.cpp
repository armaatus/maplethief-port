// NSData and NSNumber (#49, ADR 0035): Foundation's selectors on CoreFoundation's __NSCFData,
// __NSCFNumber and __NSCFBoolean (#48, ADR 0032), as host method sets. NSData's bytes are one
// guest heap block, so -bytes is a guest pointer the game reads directly (the save files'
// readDataOfLength: + getBytes:, the purchase receipts).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "cf/cf_objects.h"
#include "foundation/foundation.h"
#include "maplethief/hle/abi.h"
#include "maplethief/memory.h"

namespace mt_ns {

namespace {

#define MT_COUNT(a) static_cast<u32>(sizeof(a) / sizeof((a)[0]))

bool data_of(mt_cpu *cpu, u32 obj, const char *method, u32 *bytes, u32 *length) {
    if (!mt_cf::data_bytes(cpu, obj, bytes, length)) {
        raise(cpu, method, "not an NSData", obj);
    }
    return true;
}

// --- NSData --------------------------------------------------------------------------------

// +allocWithZone: (and so +alloc): empty __NSCFData for the -init... methods.
void data_alloc(mt_cpu *cpu) {
    const bool is_mutable = inherits(cpu, cpu->r[0], class_named(cpu, "NSMutableData"));
    cpu->r[0] = mt_cf::data_create(cpu, nullptr, 0, is_mutable);
}

// -initWithBytes:length: (a copy)
void data_init_bytes(mt_cpu *cpu) {
    const u32 self = cpu->r[0], p = cpu->r[2], n = cpu->r[3];
    if (p == 0 && n != 0) {
        raise(cpu, "-[NSData initWithBytes:length:]", "NULL bytes", 0);
    }
    const std::uint8_t *bytes = n != 0 ? mt_g2h(cpu, p, n) : nullptr;
    if (!mt_cf::data_assign(cpu, self, bytes, n)) {
        log(cpu, "-[NSData initWithBytes:length:]: no guest memory for %u bytes: nil",
            static_cast<unsigned>(n));
        mt_objc_release(cpu, self);
        cpu->r[0] = 0;
    }
}

// -initWithData: (a copy; nil gives empty data)
void data_init_data(mt_cpu *cpu) {
    const u32 self = cpu->r[0], other = cpu->r[2];
    u32 bytes = 0, length = 0;
    if (other != 0) {
        data_of(cpu, other, "-[NSData initWithData:]", &bytes, &length);
    }
    const std::vector<std::uint8_t> copy(cpu->mem + bytes, cpu->mem + bytes + length);
    if (!mt_cf::data_assign(cpu, self, copy.data(), length)) {
        mt_objc_release(cpu, self);
        cpu->r[0] = 0;
    }
}

// -bytes: the guest address of the bytes.
void data_bytes(mt_cpu *cpu) {
    u32 bytes, length;
    data_of(cpu, cpu->r[0], "-[NSData bytes]", &bytes, &length);
    cpu->r[0] = bytes;
}

// -length
void data_length(mt_cpu *cpu) {
    u32 bytes, length;
    data_of(cpu, cpu->r[0], "-[NSData length]", &bytes, &length);
    cpu->r[0] = length;
}

// -getBytes: copies all of the bytes to the buffer.
void data_get_bytes(mt_cpu *cpu) {
    const u32 self = cpu->r[0], buffer = cpu->r[2];
    u32 bytes, length;
    data_of(cpu, self, "-[NSData getBytes:]", &bytes, &length);
    if (length != 0) {
        std::memmove(mt_g2h(cpu, buffer, length), cpu->mem + bytes, length);
    }
}

// -copyWithZone: an immutable copy.
void data_copy(mt_cpu *cpu) {
    u32 bytes, length;
    data_of(cpu, cpu->r[0], "-[NSData copyWithZone:]", &bytes, &length);
    const std::vector<std::uint8_t> copy(cpu->mem + bytes, cpu->mem + bytes + length);
    cpu->r[0] = mt_cf::data_create(cpu, copy.data(), length, false);
}

const mt_objc_host_method k_data_instance[] = {
    {"initWithBytes:length:", data_init_bytes},
    {"initWithData:", data_init_data},
    {"bytes", data_bytes},
    {"length", data_length},
    {"getBytes:", data_get_bytes},
    {"copyWithZone:", data_copy},
};
const mt_objc_host_method k_data_class[] = {{"allocWithZone:", data_alloc}};

// --- NSNumber ------------------------------------------------------------------------------

// +numberWithBool: kCFBooleanTrue / kCFBooleanFalse (immortal, as on iOS). The game passes
// one in the drawable properties of -[EAGLView createGLes].
void number_with_bool(mt_cpu *cpu) { cpu->r[0] = mt_cf::boolean(cpu, (cpu->r[2] & 0xFFu) != 0); }

bool number_of(mt_cpu *cpu, u32 obj, mt_cf::Number *n) {
    if (!mt_cf::number_value(cpu, obj, n)) {
        raise(cpu, "NSNumber", "not an NSNumber", obj);
    }
    return true;
}

// -description: the value as NSNumber prints it (%d / %0.16g; a boolean prints 1 or 0).
void number_description(mt_cpu *cpu) {
    mt_cf::Number n;
    number_of(cpu, cpu->r[0], &n);
    char text[64];
    if (n.is_float) {
        std::snprintf(text, sizeof text, "%0.16g", n.f);
    } else {
        std::snprintf(text, sizeof text, "%lld", static_cast<long long>(n.i));
    }
    const u32 s = string_from_utf8(cpu, text);
    cpu->r[0] = s != 0 ? mt_objc_autorelease(cpu, s) : 0u;
}

// -isEqual: and -hash (dictionary keys)
void number_is_equal(mt_cpu *cpu) {
    const u32 self = cpu->r[0], other = cpu->r[2];
    mt_cf::Number n;
    cpu->r[0] = other != 0 && mt_cf::number_value(cpu, other, &n) && objects_equal(cpu, self, other)
                    ? 1u
                    : 0u;
}
void number_hash(mt_cpu *cpu) {
    mt_cf::Number n;
    number_of(cpu, cpu->r[0], &n);
    const double v = n.is_float ? n.f : static_cast<double>(n.i);
    cpu->r[0] = std::isfinite(v) && std::fabs(v) < 9.0e18
                    ? static_cast<u32>(static_cast<std::int64_t>(v))
                    : 0u;
}

const mt_objc_host_method k_number_instance[] = {
    {"description", number_description},
    {"isEqual:", number_is_equal},
    {"hash", number_hash},
};
const mt_objc_host_method k_number_class[] = {{"numberWithBool:", number_with_bool}};

} // namespace

const mt_objc_host_methods k_nsdata_methods = {"NSData",        "Foundation (#49)",
                                               k_data_instance, MT_COUNT(k_data_instance),
                                               k_data_class,    MT_COUNT(k_data_class)};
const mt_objc_host_methods k_nsnumber_methods = {"NSNumber",        "Foundation (#49)",
                                                 k_number_instance, MT_COUNT(k_number_instance),
                                                 k_number_class,    MT_COUNT(k_number_class)};

} // namespace mt_ns
