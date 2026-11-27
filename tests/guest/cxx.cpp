// The guest program's C++ (#385, ADR 0213): a static constructor (__mod_init_func, which the
// boot runs before main as dyld did) and a throw caught by type through the SjLj unwinder
// (__gxx_personality_sj0, the LSDA, the runtime's __cxa_throw and catch helpers).
#include "guest.h"

namespace {

struct Registered {
    int value;
    Registered() : value(0) {
        volatile int v = 385; // not a constant the compiler could fold into a static initializer
        value = v;
    }
};

struct GuestError {
    virtual ~GuestError() {}
    int code;
};

struct GuestFault : GuestError {}; // caught as its base: the si_class_type_info path

Registered g_registered;

__attribute__((noinline)) void raise(int code) {
    GuestFault fault;
    fault.code = code;
    throw fault;
}

} // namespace

extern "C" int guest_constructed(void) { return g_registered.value; }

extern "C" int guest_throw_and_catch(int code) {
    try {
        raise(code);
    } catch (const GuestError &e) {
        return e.code;
    }
    return -1;
}
