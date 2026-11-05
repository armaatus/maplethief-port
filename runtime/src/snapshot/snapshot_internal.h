// Save states' internals (#202, ADR 0105): the byte writer and the bounds-checked reader every
// section is written and read with, the registry of host state serializers and the module
// sections. The public side is maplethief/snapshot.h.
//
// Adding a host state type's serializer (#203 and every new kind of host state): next to the
// type, in its own file,
//
//     bool save_foo(mt_cpu *, u32 obj, const void *state, mt_snap::Writer &w) {
//         const auto *s = static_cast<const FooState *>(state);
//         w.u32(s->count);
//         w.str(*s->name);
//         return true;
//     }
//     bool load_foo(mt_cpu *, u32 obj, void *state, mt_snap::Reader &r) {  // state is zeroed
//         auto *s = static_cast<FooState *>(state);
//         std::string name;
//         if (!r.u32(&s->count) || !r.str(&name)) return false;  // the reader says why
//         s->name = new std::string(std::move(name));
//         return true;
//     }
//     void drop_foo(void *state) { delete static_cast<FooState *>(state)->name; }
//     const mt_snap::StateOps k_snap_foo = {&k_foo_state, "Foo", save_foo, load_foo, drop_foo,
//                                           nullptr};
//
// and one line in runtime/src/snapshot/registry.cpp. The rules:
//   - save writes only host values; guest objects are written as their guest addresses (a
//     restore brings guest memory back, so the addresses stay valid).
//   - load runs before guest memory is restored, on a detached zeroed block: it reads the
//     payload into the block and must not touch guest memory, send messages or create states.
//     It returns false on a malformed payload (the whole restore is then refused).
//   - drop frees the host memory a state owns without calling into the guest (the guest memory
//     it refers to is replaced wholesale); NULL when the block owns nothing.
//   - link, if not NULL, runs after every state is attached and guest memory restored: for
//     host pointers between states (UIView's boxes, #203).
//   - name is stable: it is written into the file and looked up at restore; never rename one.
#ifndef MAPLETHIEF_RUNTIME_SNAPSHOT_SNAPSHOT_INTERNAL_H
#define MAPLETHIEF_RUNTIME_SNAPSHOT_SNAPSHOT_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "maplethief/objc/host.h"
#include "maplethief/snapshot.h"

namespace mt_snap {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// Little-endian values appended to a byte vector.
class Writer {
  public:
    void u8v(std::uint8_t v) { bytes_.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; i++) {
            bytes_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void f64(double v) {
        std::uint64_t b;
        std::memcpy(&b, &v, 8);
        u64(b);
    }
    void boolean(bool v) { u8v(v ? 1u : 0u); }
    void raw(const void *p, std::size_t n) {
        const auto *b = static_cast<const std::uint8_t *>(p);
        bytes_.insert(bytes_.end(), b, b + n);
    }
    // A length (u32) and the bytes.
    void str(const std::string &s) {
        u32(static_cast<std::uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void u16str(const std::u16string &s) {
        u32(static_cast<std::uint32_t>(s.size()));
        for (const char16_t c : s) {
            u8v(static_cast<std::uint8_t>(c));
            u8v(static_cast<std::uint8_t>(c >> 8));
        }
    }
    void words(const std::vector<std::uint32_t> &v) {
        u32(static_cast<std::uint32_t>(v.size()));
        for (const std::uint32_t w : v) {
            u32(w);
        }
    }

    std::vector<std::uint8_t> &bytes() { return bytes_; }
    const std::vector<std::uint8_t> &bytes() const { return bytes_; }

  private:
    std::vector<std::uint8_t> bytes_;
};

// Reads what Writer wrote, never past the end: a read that does not fit fails, sets the error
// and leaves the output alone. Counts are checked against the bytes left before anything is
// allocated for them.
class Reader {
  public:
    Reader(const std::uint8_t *p, std::size_t n, std::string what = {})
        : p_(p), n_(n), what_(std::move(what)) {}

    bool u8v(std::uint8_t *v) {
        if (!need(1)) {
            return false;
        }
        *v = p_[at_++];
        return true;
    }
    bool u32(std::uint32_t *v) {
        if (!need(4)) {
            return false;
        }
        *v = static_cast<std::uint32_t>(p_[at_]) | static_cast<std::uint32_t>(p_[at_ + 1]) << 8 |
             static_cast<std::uint32_t>(p_[at_ + 2]) << 16 |
             static_cast<std::uint32_t>(p_[at_ + 3]) << 24;
        at_ += 4;
        return true;
    }
    bool i32(std::int32_t *v) {
        std::uint32_t u;
        if (!u32(&u)) {
            return false;
        }
        *v = static_cast<std::int32_t>(u);
        return true;
    }
    bool u64(std::uint64_t *v) {
        std::uint32_t lo, hi;
        if (!need(8)) {
            return false;
        }
        u32(&lo);
        u32(&hi);
        *v = static_cast<std::uint64_t>(hi) << 32 | lo;
        return true;
    }
    bool i64(std::int64_t *v) {
        std::uint64_t u;
        if (!u64(&u)) {
            return false;
        }
        *v = static_cast<std::int64_t>(u);
        return true;
    }
    bool f64(double *v) {
        std::uint64_t b;
        if (!u64(&b)) {
            return false;
        }
        std::memcpy(v, &b, 8);
        return true;
    }
    bool boolean(bool *v) {
        std::uint8_t b;
        if (!u8v(&b)) {
            return false;
        }
        if (b > 1u) {
            return fail("a flag is neither 0 nor 1");
        }
        *v = b != 0;
        return true;
    }
    // A pointer to the next `n` bytes (valid while the input is), or nullptr.
    const std::uint8_t *raw(std::size_t n) {
        if (!need(n)) {
            return nullptr;
        }
        const std::uint8_t *p = p_ + at_;
        at_ += n;
        return p;
    }
    bool raw(void *out, std::size_t n) {
        const std::uint8_t *p = raw(n);
        if (p == nullptr) {
            return false;
        }
        std::memcpy(out, p, n);
        return true;
    }
    bool str(std::string *s) {
        std::uint32_t n;
        if (!u32(&n)) {
            return false;
        }
        const std::uint8_t *p = raw(n);
        if (p == nullptr) {
            return false;
        }
        s->assign(reinterpret_cast<const char *>(p), n);
        return true;
    }
    bool u16str(std::u16string *s) {
        std::uint32_t n;
        if (!u32(&n) || !count_fits(n, 2)) {
            return false;
        }
        const std::uint8_t *p = raw(static_cast<std::size_t>(n) * 2u);
        s->resize(n);
        for (std::uint32_t i = 0; i < n; i++) {
            (*s)[i] = static_cast<char16_t>(p[2 * i] | p[2 * i + 1] << 8);
        }
        return true;
    }
    bool words(std::vector<std::uint32_t> *v) {
        std::uint32_t n;
        if (!u32(&n) || !count_fits(n, 4)) {
            return false;
        }
        v->resize(n);
        for (std::uint32_t i = 0; i < n; i++) {
            u32(&(*v)[i]);
        }
        return true;
    }
    // Whether `count` items of at least `item_bytes` bytes each can still be in the input.
    bool count_fits(std::uint64_t count, std::uint64_t item_bytes) {
        if (count * item_bytes > n_ - at_) {
            return fail("a count of " + std::to_string(count) + " runs past the section");
        }
        return true;
    }
    // Every byte read: a section with bytes left over is malformed.
    bool done() {
        if (at_ != n_) {
            return fail(std::to_string(n_ - at_) + " bytes left over");
        }
        return true;
    }

    bool fail(const std::string &why) {
        if (error_.empty()) {
            error_ = (what_.empty() ? std::string() : what_ + ": ") + why;
        }
        return false;
    }
    const std::string &error() const { return error_; }
    std::size_t left() const { return n_ - at_; }

  private:
    bool need(std::size_t n) {
        if (n > n_ - at_) {
            return fail("truncated (" + std::to_string(n) + " bytes wanted, " +
                        std::to_string(n_ - at_) + " left)");
        }
        return true;
    }

    const std::uint8_t *p_;
    std::size_t n_;
    std::size_t at_ = 0;
    std::string what_;
    std::string error_;
};

// --- host state serializers -----------------------------------------------------------------

struct StateOps {
    const mt_objc_state_type *type;
    const char *name; // stable: written into the file
    bool (*save)(mt_cpu *cpu, u32 obj, const void *state, Writer &w);
    bool (*load)(mt_cpu *cpu, u32 obj, void *state, Reader &r);
    void (*drop)(void *state);
    void (*link)(mt_cpu *cpu, u32 obj, void *state);
};

// The serializer of `type`, or nullptr.
const StateOps *ops_for(const mt_objc_state_type *type);
// The serializer named `name`, or nullptr.
const StateOps *ops_named(const char *name);
// Every registered serializer.
const StateOps *const *all_ops(std::size_t *count);

// --- module sections ----------------------------------------------------------------------
// A runtime module's own tables that hold guest objects (CF's immortal objects and run loop,
// Foundation's singletons, ...). load parses the payload and, if it is well formed, returns the
// change in *apply, run only once the whole state has been accepted (after guest memory and the
// host states are back).
struct Module {
    const char *section; // stable section name
    bool (*save)(mt_cpu *cpu, Writer &w, std::string *why);
    bool (*load)(mt_cpu *cpu, Reader &r, std::function<void(mt_cpu *)> *apply);
};

const Module *const *all_modules(std::size_t *count);

} // namespace mt_snap

#endif // MAPLETHIEF_RUNTIME_SNAPSHOT_SNAPSHOT_INTERNAL_H
