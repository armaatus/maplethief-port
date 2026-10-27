// Internals shared by the offline service stubs (#52, maplethief/stubs.h): stubs.cpp (deferred
// deliveries, per-runtime state, guest strings, files), json.cpp (the gamecenter.json
// format), gamekit.cpp, storekit.cpp and network.cpp.
//
// The host classes are listed in runtime/src/objc/classes.cpp, which the recompiler's C test
// harness links into a C program (tests/recomp_harness.py, OBJC_RUNTIME). These files are
// therefore plain C++ like the ObjC runtime (ADR 0019, ADR 0029): no exceptions, no `new`,
// no standard containers and no function-local statics; memory comes from malloc.
#ifndef MAPLETHIEF_RUNTIME_STUBS_STUBS_INTERNAL_H
#define MAPLETHIEF_RUNTIME_STUBS_STUBS_INTERNAL_H

#include <cstddef>
#include <cstdint>

#include "maplethief/objc/host.h"
#include "maplethief/stubs.h"

#if defined(__GNUC__) || defined(__clang__)
#define MT_STUBS_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define MT_STUBS_PRINTF(f, a)
#endif

namespace mt_stubs_impl {

using u32 = std::uint32_t;

// --- deferred deliveries (stubs.cpp) --------------------------------------------------------

// A delivery runs on the guest's main thread from mt_stubs_pump with the words it was posted
// with. It owns what the poster retained or copied for it (objects, blocks) and releases it.
typedef void (*Deliver)(mt_cpu *cpu, const u32 *words);
constexpr u32 k_delivery_words = 4;

// Queues `fn` with up to k_delivery_words words. Returns false (and logs) if out of memory;
// the caller then releases what it retained for the delivery.
bool post(Deliver fn, const u32 *words, u32 count);

// --- per-runtime state ---------------------------------------------------------------------

// Every entry point calls this first. When the current ObjC runtime is not the one the stubs
// last saw (a new process, or a test's new runtime), it drops the waiting deliveries and calls
// the modules' reset functions below, whose guest objects belonged to the old runtime.
mt_objc *runtime();

void gamekit_reset();  // gamekit.cpp: the local player
void storekit_reset(); // storekit.cpp: the payment queue, its observers and transactions

// --- logging -------------------------------------------------------------------------------

// One line to the ObjC runtime's log sink (its mt_import_config), prefixed "stubs: ".
void log(const char *fmt, ...) MT_STUBS_PRINTF(1, 2);

// --- guest strings --------------------------------------------------------------------------

// The text of an NSString: [str UTF8String], copied to out[0..size) and NUL-terminated
// (truncated if longer). False, with "" in `out`, for nil or no answer.
bool string_text(mt_cpu *cpu, u32 str, char *out, u32 size);

// [NSString stringWithUTF8String:text]: an autoreleased NSString (Foundation, #49); 0 if
// there is no NSString class or the text does not fit the scratch space below the guest sp.
u32 new_string(mt_cpu *cpu, const char *text);

// --- growing host arrays of guest words ------------------------------------------------------

struct Words {
    u32 *items = nullptr;
    u32 count = 0, cap = 0;
};
bool words_push(Words &w, u32 value);
bool words_remove(Words &w, u32 value); // the first occurrence; keeps the order
bool words_contains(const Words &w, u32 value);
void words_free(Words &w);

// --- files ----------------------------------------------------------------------------------

// The whole file, malloc'd and NUL-terminated (*size without the NUL); NULL if it can't be read.
char *read_file(const char *path, std::size_t *size);
// Creates `path`'s directory (and its parents), writes `path`.tmp and renames it over `path`,
// so a crash never leaves a half-written file. False (and logs) on failure.
bool write_file(const char *path, const char *data, std::size_t size);

// --- the host classes (runtime/src/objc/classes.cpp lists them) -----------------------------

// gamekit.cpp
extern const mt_objc_host_class k_gk_player;
extern const mt_objc_host_class k_gk_local_player; // after GKPlayer, its superclass
extern const mt_objc_host_class k_gk_score;
extern const mt_objc_host_class k_gk_achievement;
extern const mt_objc_host_class k_gk_leaderboard;
extern const mt_objc_host_class k_gk_leaderboard_view_controller;
// storekit.cpp
extern const mt_objc_host_class k_sk_payment_queue;
extern const mt_objc_host_class k_sk_products_request;
extern const mt_objc_host_class k_sk_products_response;
extern const mt_objc_host_class k_sk_product;
extern const mt_objc_host_class k_sk_payment;
extern const mt_objc_host_class k_sk_payment_transaction;

} // namespace mt_stubs_impl

#endif // MAPLETHIEF_RUNTIME_STUBS_STUBS_INTERNAL_H
