// The offline service stubs (#52, maplethief/stubs.h, ADR 0034) on a real guest address space
// (#42's heap and a guest stack): GameKit with its gamecenter.json, StoreKit's "sells
// nothing" store, the CFSocket and SCNetworkReachability imports, deferred deliveries and the
// JSON format. Messages are sent the way the game's GameCenterManager and
// InAppPurchaseManager send them (docs/re/objc-shell.md); completion handlers are blocks whose
// invoke functions are test functions (objc_test.cpp's mt_dispatch). Foundation (#49) is not
// there yet, so the Foundation classes the stubs and the game use (NSString, NSArray,
// NSMutableArray, NSSet) are minimal test doubles.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

#include "hle/hle.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/stubs.h"
#include "objc_test.h"
#include "stubs/json.h"

namespace {

using u32 = std::uint32_t;
namespace fs = std::filesystem;

std::vector<std::string> g_log;
void capture(void *, const char *line) { g_log.emplace_back(line); }

u32 g_next_imp = 0x00E00001u; // fake guest code addresses, apart from objc_services_test's

u32 imp(mt_guest_fn fn) {
    const u32 at = g_next_imp;
    g_next_imp += 0x10u;
    objc_test::add_imp(at, fn);
    return at;
}

mt_mem *g_mem = nullptr; // the current Env's, for the Foundation doubles

// --- Foundation test doubles (#49 brings the real classes) ---------------------------------

struct StringState {
    char text[256];
    u32 utf8; // guest copy for -UTF8String, 0 until asked
};
void string_destroy(mt_cpu *, u32, void *s) {
    const u32 utf8 = static_cast<StringState *>(s)->utf8;
    if (utf8 != 0) {
        mt_heap_free(g_mem, utf8);
    }
}
const mt_objc_state_type k_string = {"test NSString", sizeof(StringState), string_destroy};

// +[NSString stringWithUTF8String:]
void string_with_utf8(mt_cpu *cpu) {
    mt_objc *rt = mt_objc_current();
    const u32 p = cpu->r[2];
    const u32 s = mt_objc_create_instance(rt, cpu->r[0]);
    auto *st = static_cast<StringState *>(mt_objc_state(rt, s, &k_string));
    std::snprintf(st->text, sizeof st->text, "%s", reinterpret_cast<const char *>(cpu->mem + p));
    cpu->r[0] = mt_objc_autorelease(cpu, s);
}
void string_utf8(mt_cpu *cpu) {
    auto *st =
        static_cast<StringState *>(mt_objc_find_state(mt_objc_current(), cpu->r[0], &k_string));
    if (st->utf8 == 0) {
        const u32 n = static_cast<u32>(std::strlen(st->text)) + 1;
        st->utf8 = mt_heap_malloc(g_mem, n);
        std::memcpy(cpu->mem + st->utf8, st->text, n);
    }
    cpu->r[0] = st->utf8;
}

struct ListState {
    u32 items[16];
    u32 count;
};
void list_destroy(mt_cpu *cpu, u32, void *s) {
    auto *st = static_cast<ListState *>(s);
    for (u32 i = 0; i < st->count; i++) {
        mt_objc_release(cpu, st->items[i]);
    }
}
const mt_objc_state_type k_list = {"test collection", sizeof(ListState), list_destroy};
ListState *list_of(u32 obj) {
    return static_cast<ListState *>(mt_objc_state(mt_objc_current(), obj, &k_list));
}
void list_add(mt_cpu *cpu, u32 list, u32 obj) {
    ListState *st = list_of(list);
    REQUIRE(st->count < 16u);
    st->items[st->count++] = mt_objc_retain(cpu, obj);
}
// +arrayWithObject: / +setWithObject:
void list_with_object(mt_cpu *cpu) {
    const u32 obj = cpu->r[2];
    const u32 list = mt_objc_create_instance(mt_objc_current(), cpu->r[0]);
    list_add(cpu, list, obj);
    cpu->r[0] = mt_objc_autorelease(cpu, list);
}
void list_count(mt_cpu *cpu) { cpu->r[0] = list_of(cpu->r[0])->count; }
void list_object_at(mt_cpu *cpu) {
    ListState *st = list_of(cpu->r[0]);
    cpu->r[0] = cpu->r[2] < st->count ? st->items[cpu->r[2]] : 0u;
}
void list_init_with_capacity(mt_cpu *) {} // r0 is self
void list_add_object(mt_cpu *cpu) {
    const u32 self = cpu->r[0], obj = cpu->r[2];
    list_add(cpu, self, obj);
}

const mt_objc_host_method k_string_class_methods[] = {{"stringWithUTF8String:", string_with_utf8}};
const mt_objc_host_method k_string_methods[] = {{"UTF8String", string_utf8}};
const mt_objc_host_class k_test_nsstring = {
    "NSString", "NSObject", 4, k_string_methods, 1, k_string_class_methods, 1};
const mt_objc_host_method k_array_class_methods[] = {{"arrayWithObject:", list_with_object}};
const mt_objc_host_method k_array_methods[] = {{"count", list_count},
                                               {"objectAtIndex:", list_object_at}};
const mt_objc_host_class k_test_nsarray = {
    "NSArray", "NSObject", 4, k_array_methods, 2, k_array_class_methods, 1};
const mt_objc_host_method k_mutable_array_methods[] = {
    {"initWithCapacity:", list_init_with_capacity}, {"addObject:", list_add_object}};
const mt_objc_host_class k_test_nsmutablearray = {
    "NSMutableArray", "NSArray", 4, k_mutable_array_methods, 2, nullptr, 0};
const mt_objc_host_method k_set_class_methods[] = {{"setWithObject:", list_with_object}};
const mt_objc_host_class k_test_nsset = {"NSSet", "NSObject",          4, nullptr,
                                         0,       k_set_class_methods, 1};

// --- the environment ------------------------------------------------------------------------

fs::path test_dir() { return fs::temp_directory_path() / "maplethief-stubs-test"; }

struct Env {
    mt_mem *mem = nullptr;
    mt_cpu cpu{};
    mt_stack stack{};
    mt_objc *rt = nullptr;
    u32 pool = 0;

    explicit Env(bool fresh_dir = true) {
        g_log.clear();
        mem = mt_mem_create(nullptr);
        REQUIRE(mem != nullptr);
        g_mem = mem;
        mt_mem_attach(mem, &cpu);
        REQUIRE(mt_stack_create(mem, 0, &stack) == 0);
        cpu.r[MT_SP] = stack.top;
        cpu.r[MT_LR] = 0x2345u;
        mt_import_config cfg = mt_import_config_default();
        cfg.unimplemented = MT_UNIMPLEMENTED_STOP;
        cfg.log = capture;
        rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
        REQUIRE(rt != nullptr);
        mt_objc_set_free(rt, mt_mem_guest_free);
        mt_objc_set_current(rt);
        REQUIRE(mt_objc_add_host_classes(rt) == 0u);
        for (const mt_objc_host_class *c :
             {&k_test_nsstring, &k_test_nsarray, &k_test_nsmutablearray, &k_test_nsset}) {
            REQUIRE(mt_objc_add_host_class(rt, c, 0, 0) == 0);
        }
        if (fresh_dir) {
            std::error_code ec;
            fs::remove_all(test_dir(), ec);
        }
        REQUIRE(mt_stubs_set_data_dir(test_dir().string().c_str()) == 0);
        pool = mt_objc_pool_push(rt); // what the run loop does around a turn
    }
    ~Env() {
        mt_objc_pool_pop(rt, pool);
        mt_stubs_reset();
        mt_objc_set_current(nullptr);
        mt_objc_destroy(rt);
        mt_mem_destroy(mem);
        mt_stubs_set_data_dir(nullptr);
    }
    Env(const Env &) = delete;
    Env &operator=(const Env &) = delete;

    u32 cls(const char *name) const { return mt_objc_class_named(rt, name); }
    u32 send(u32 obj, const char *name, std::initializer_list<u32> args = {}) {
        return mt_objc_msg(&cpu, obj, mt_objc_sel(rt, name), args.begin(),
                           static_cast<u32>(args.size()));
    }
    u32 string(const char *text) {
        const u32 n = static_cast<u32>(std::strlen(text)) + 1;
        const u32 p = mt_heap_malloc(mem, n);
        std::memcpy(cpu.mem + p, text, n);
        const u32 s = send(cls("NSString"), "stringWithUTF8String:", {p});
        mt_heap_free(mem, p);
        return s;
    }
    std::string text(u32 str) {
        const u32 p = send(str, "UTF8String");
        return p != 0 ? std::string(reinterpret_cast<const char *>(cpu.mem + p)) : "";
    }
    u32 words(std::initializer_list<u32> ws) {
        const u32 p = mt_heap_malloc(mem, 4u * static_cast<u32>(ws.size()));
        u32 i = 0;
        for (u32 w : ws) {
            std::memcpy(cpu.mem + p + 4u * i++, &w, 4);
        }
        return p;
    }
    u32 get32(u32 addr) const {
        u32 v;
        std::memcpy(&v, cpu.mem + addr, 4);
        return v;
    }
    // A completion block on the "stack" (isa __NSStackBlock__), as GameCenterManager builds
    // them; its invoke function is `fn`.
    u32 block(mt_guest_fn fn) {
        const u32 descriptor = words({0u, 20u});
        return words({cls("__NSStackBlock__"), 0u, 0u, imp(fn), descriptor});
    }
    // Calls an HLE import with word arguments (those past r3 on the guest stack).
    u32 call(mt_import_fn fn, std::initializer_list<u32> args) {
        const u32 sp = cpu.r[MT_SP];
        const u32 stacked = args.size() > 4 ? static_cast<u32>(args.size()) - 4u : 0u;
        cpu.r[MT_SP] = sp - 8u * ((stacked + 1u) / 2u);
        u32 i = 0;
        for (u32 a : args) {
            if (i < 4) {
                cpu.r[i] = a;
            } else {
                std::memcpy(cpu.mem + cpu.r[MT_SP] + 4u * (i - 4u), &a, 4);
            }
            i++;
        }
        fn(&cpu, 0);
        cpu.r[MT_SP] = sp;
        return cpu.r[0];
    }
};

u32 lo(std::uint64_t v) { return static_cast<u32>(v); }
u32 hi(std::uint64_t v) { return static_cast<u32>(v >> 32); }
std::uint64_t bits(double d) {
    std::uint64_t b;
    std::memcpy(&b, &d, 8);
    return b;
}
double as_double(u32 l, u32 h) {
    const std::uint64_t b = (static_cast<std::uint64_t>(h) << 32) | l;
    double d;
    std::memcpy(&d, &b, 8);
    return d;
}
// A message that returns a double (r0:r1). The send runs before r1 is read: the two
// operands of as_double(send, r1) would be evaluated in an unspecified order.
double send_f64(Env &e, u32 obj, const char *name) {
    const u32 low = e.send(obj, name);
    return as_double(low, e.cpu.r[1]);
}

// What the completion blocks were called with: (block, first argument, second argument).
struct Call {
    u32 block, a, b;
};
std::vector<Call> g_calls;
void record_block(mt_cpu *cpu) { g_calls.push_back({cpu->r[0], cpu->r[1], cpu->r[2]}); }

std::string slurp(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path gamecenter_file() {
    char path[1100];
    REQUIRE(mt_stubs_gamecenter_path(path, sizeof path) == 0);
    return fs::path(path);
}

constexpr const char *k_ranking = "com.nexonmobile.maplestory.thief.totalranking";

} // namespace

TEST_CASE("GameKit: the local player is authenticated when its request completes, on a pump") {
    Env e;
    g_calls.clear();
    const u32 player = e.send(e.cls("GKLocalPlayer"), "localPlayer");
    REQUIRE(player != 0u);
    CHECK(e.send(e.cls("GKLocalPlayer"), "localPlayer") == player);
    CHECK(mt_objc_is_kind_of(e.rt, player, e.cls("GKPlayer")) == 1);
    CHECK(e.send(player, "isAuthenticated") == 0u);
    // -[GameCenterManager AuthenticationUser]: a stack block, copied by GameKit.
    const u32 block = e.block(record_block);
    e.send(player, "authenticateWithCompletionHandler:", {block});
    CHECK(g_calls.empty()); // never inside the request
    CHECK(mt_stubs_pending() == 1u);
    CHECK(mt_stubs_pump(&e.cpu) == 1u);
    REQUIRE(g_calls.size() == 1u);
    CHECK(g_calls[0].block != block); // the heap copy
    CHECK(g_calls[0].a == 0u);        // error nil
    CHECK(e.send(player, "isAuthenticated") == 1u);
    CHECK(mt_stubs_pending() == 0u);
    CHECK(e.text(e.send(player, "playerID")) == "G:offline");
    CHECK(mt_objc_undeliverable_count(e.rt) == 0u);
}

TEST_CASE("GameKit: reported achievements and scores are persisted and survive a restart") {
    {
        Env e;
        g_calls.clear();
        // -[GameCenterManager ReportScore:forCategory:] (0xBFE41):
        //   [[[GKScore alloc] initWithCategory:c] autorelease], -setValue:(int64_t),
        //   -reportScoreWithCompletionHandler:
        const std::uint64_t value = 0x100000002ull; // a 64-bit value: both words count
        u32 score =
            e.send(e.send(e.cls("GKScore"), "alloc"), "initWithCategory:", {e.string(k_ranking)});
        score = e.send(score, "autorelease");
        e.send(score, "setValue:", {lo(value), hi(value)});
        e.send(score, "reportScoreWithCompletionHandler:", {e.block(record_block)});
        CHECK(fs::exists(gamecenter_file())); // saved at once, not on the pump
        // -[GameCenterManager SubmitAchievement:PercentComplete:] (0xBFB51):
        //   [[[GKAchievement alloc] initWithIdentifier:i] autorelease], -setPercentComplete:,
        //   -reportAchievementWithCompletionHandler:
        u32 ach = e.send(e.send(e.cls("GKAchievement"), "alloc"),
                         "initWithIdentifier:", {e.string("thief.first.boss")});
        ach = e.send(ach, "autorelease");
        e.send(ach, "setPercentComplete:", {lo(bits(100.0)), hi(bits(100.0))});
        CHECK(send_f64(e, ach, "percentComplete") == 100.0);
        CHECK(e.text(e.send(ach, "identifier")) == "thief.first.boss");
        e.send(ach, "reportAchievementWithCompletionHandler:", {e.block(record_block)});
        // A lower percentage later does not lower it; another achievement, half done.
        e.send(ach, "setPercentComplete:", {lo(bits(40.0)), hi(bits(40.0))});
        e.send(ach, "reportAchievementWithCompletionHandler:", {0u});
        u32 half = e.send(e.send(e.cls("GKAchievement"), "alloc"),
                          "initWithIdentifier:", {e.string("thief \"quoted\"\n")});
        e.send(half, "setPercentComplete:", {lo(bits(50.0)), hi(bits(50.0))});
        e.send(half, "reportAchievementWithCompletionHandler:", {0u});
        e.send(half, "release");
        // A lower score later: the best stays, the last changes.
        e.send(score, "setValue:", {7u, 0u});
        e.send(score, "reportScoreWithCompletionHandler:", {0u});

        CHECK(g_calls.empty());
        CHECK(mt_stubs_pump(&e.cpu) == 2u);
        REQUIRE(g_calls.size() == 2u);
        CHECK((g_calls[0].a == 0u && g_calls[1].a == 0u)); // nil errors

        const std::string json = slurp(gamecenter_file());
        INFO(json);
        CHECK(json.find("\"format\": \"maplethief-gamecenter\"") != std::string::npos);
        CHECK(json.find("{\"identifier\": \"thief.first.boss\", \"percentComplete\": 100}") !=
              std::string::npos);
        CHECK(json.find("\"thief \\\"quoted\\\"\\n\", \"percentComplete\": 50") !=
              std::string::npos);
        CHECK(json.find(std::string("{\"category\": \"") + k_ranking +
                        "\", \"best\": 4294967298, \"last\": 7, \"reports\": 2}") !=
              std::string::npos);
        CHECK(mt_objc_undeliverable_count(e.rt) == 0u);
    }
    {
        // A new process: +[GKAchievement loadAchievementsWithCompletionHandler:] answers with
        // what was saved, as GKAchievement objects in an NSArray.
        Env e(false);
        g_calls.clear();
        e.send(e.cls("GKAchievement"),
               "loadAchievementsWithCompletionHandler:", {e.block(record_block)});
        CHECK(mt_stubs_pump(&e.cpu) == 1u);
        REQUIRE(g_calls.size() == 1u);
        CHECK(g_calls[0].b == 0u); // error nil
        const u32 list = g_calls[0].a;
        REQUIRE(list != 0u);
        // The block's array was released after the call: re-load and keep it this time.
        g_calls.clear();
        mt_guest_fn keep = [](mt_cpu *cpu) {
            g_calls.push_back({cpu->r[0], cpu->r[1], cpu->r[2]}); // before a send changes them
            mt_objc_retain(cpu, cpu->r[1]);
        };
        e.send(e.cls("GKAchievement"), "loadAchievementsWithCompletionHandler:", {e.block(keep)});
        mt_stubs_pump(&e.cpu);
        REQUIRE(g_calls.size() == 1u);
        const u32 kept = g_calls[0].a;
        REQUIRE(e.send(kept, "count") == 2u);
        const u32 first = e.send(kept, "objectAtIndex:", {0u});
        CHECK(mt_objc_is_kind_of(e.rt, first, e.cls("GKAchievement")) == 1);
        CHECK(e.text(e.send(first, "identifier")) == "thief.first.boss");
        CHECK(send_f64(e, first, "percentComplete") == 100.0);
        const u32 second = e.send(kept, "objectAtIndex:", {1u});
        CHECK(e.text(e.send(second, "identifier")) == "thief \"quoted\"\n");
        CHECK(send_f64(e, second, "percentComplete") == 50.0);
        e.send(kept, "release");

        // +resetAchievementsWithCompletionHandler: empties them; the scores stay.
        e.send(e.cls("GKAchievement"), "resetAchievementsWithCompletionHandler:", {0u});
        const std::string json = slurp(gamecenter_file());
        CHECK(json.find("\"achievements\": []") != std::string::npos);
        CHECK(json.find(k_ranking) != std::string::npos);
        g_calls.clear();
        e.send(e.cls("GKAchievement"),
               "loadAchievementsWithCompletionHandler:", {e.block(record_block)});
        mt_stubs_pump(&e.cpu);
        REQUIRE(g_calls.size() == 1u);
        CHECK(g_calls[0].a == 0u); // no achievements: nil
        CHECK(mt_objc_undeliverable_count(e.rt) == 0u);
    }
}

TEST_CASE("GameKit: an unreadable gamecenter.json is kept aside, not overwritten silently") {
    Env e;
    fs::create_directories(test_dir());
    {
        std::ofstream out(gamecenter_file(), std::ios::binary);
        out << "{ not json";
    }
    u32 score =
        e.send(e.send(e.cls("GKScore"), "alloc"), "initWithCategory:", {e.string(k_ranking)});
    e.send(score, "setValue:", {5u, 0u});
    e.send(score, "reportScoreWithCompletionHandler:", {0u});
    e.send(score, "release");
    CHECK(slurp(fs::path(gamecenter_file().string() + ".bad")) == "{ not json");
    CHECK(slurp(gamecenter_file()).find("\"best\": 5") != std::string::npos);
    bool logged = false;
    for (const auto &line : g_log) {
        logged |= line.find("is not a version 1 maplethief-gamecenter file") != std::string::npos;
    }
    CHECK(logged);
}

TEST_CASE(
    "GameKit: leaderboards have no online scores and the leaderboard screen finishes at once") {
    Env e;
    g_calls.clear();
    // -[GameCenterManager ReloadHighScoreByCategory:] (0xBFF41)
    const u32 board =
        e.send(e.send(e.send(e.cls("GKLeaderboard"), "alloc"), "init"), "autorelease");
    e.send(board, "setCategory:", {e.string(k_ranking)});
    e.send(board, "setTimeScope:", {2u});
    e.send(board, "setRange:", {1u, 1u}); // NSRange by value
    e.send(board, "loadScoresWithCompletionHandler:", {e.block(record_block)});
    // -[GameCenterManager FindPlayerByid:] (0xBFA09)
    e.send(e.cls("GKPlayer"),
           "loadPlayersForIdentifiers:withCompletionHandler:", {0u, e.block(record_block)});
    CHECK(mt_stubs_pump(&e.cpu) == 2u);
    REQUIRE(g_calls.size() == 2u);
    CHECK((g_calls[0].a == 0u && g_calls[0].b == 0u && g_calls[1].a == 0u && g_calls[1].b == 0u));

    // -[EAGLView initWithCoder:] / -viewGameCenterRank: set up a GKLeaderboardViewController;
    // when UIKit (#54) shows it, it tells its delegate it is done.
    g_calls.clear();
    static const mt_objc_host_method k_finish[] = {
        {"leaderboardViewControllerDidFinish:", record_block}};
    static const mt_objc_host_class k_delegate = {
        "TestLeaderboardDelegate", "NSObject", 4, k_finish, 1, nullptr, 0};
    REQUIRE(mt_objc_add_host_class(e.rt, &k_delegate, 0, 0) == 0);
    const u32 delegate = e.send(e.cls("TestLeaderboardDelegate"), "new");
    const u32 vc = e.send(e.send(e.cls("GKLeaderboardViewController"), "alloc"), "init");
    e.send(vc, "setCategory:", {e.string(k_ranking)});
    e.send(vc, "setTimeScope:", {2u});
    e.send(vc, "setLeaderboardDelegate:", {delegate});
    e.send(vc, "viewDidAppear:", {1u});
    CHECK(g_calls.empty());
    CHECK(mt_stubs_pump(&e.cpu) == 1u);
    REQUIRE(g_calls.size() == 1u);
    CHECK((g_calls[0].block == delegate &&
           g_calls[0].a == mt_objc_sel(e.rt, "leaderboardViewControllerDidFinish:") &&
           g_calls[0].b == vc));
    e.send(vc, "release");
    e.send(delegate, "release");
    CHECK(mt_objc_undeliverable_count(e.rt) == 0u);
}

namespace {

// What InAppPurchaseManager does (0xBE559, 0xBF0D5, 0xBF025), as host methods of a test class.
std::vector<std::string> g_shop;
void shop_products_response(mt_cpu *cpu) {
    const u32 self = cpu->r[0], response = cpu->r[3];
    const u32 products = mt_objc_send0(cpu, response, "products");
    const u32 n = mt_objc_send0(cpu, products, "count");
    g_shop.push_back("products " + std::to_string(n));
    if (n != 0) {
        const u32 product = mt_objc_send1(cpu, products, "objectAtIndex:", 0);
        const u32 payment = mt_objc_send1(cpu, mt_objc_class_named(mt_objc_current(), "SKPayment"),
                                          "paymentWithProduct:", product);
        const u32 queue = mt_objc_send0(
            cpu, mt_objc_class_named(mt_objc_current(), "SKPaymentQueue"), "defaultQueue");
        mt_objc_send1(cpu, queue, "addPayment:", payment);
    }
    const u32 invalid = mt_objc_send0(cpu, response, "invalidProductIdentifiers");
    g_shop.push_back("invalid " + std::to_string(mt_objc_send0(cpu, invalid, "count")));
    (void)self;
}
void shop_updated_transactions(mt_cpu *cpu) {
    const u32 transactions = cpu->r[3];
    const u32 n = mt_objc_send0(cpu, transactions, "count");
    for (u32 i = 0; i < n; i++) {
        const u32 t = mt_objc_send1(cpu, transactions, "objectAtIndex:", i);
        const u32 state = mt_objc_send0(cpu, t, "transactionState");
        g_shop.push_back("transaction state " + std::to_string(state));
        CHECK(mt_objc_send0(cpu, t, "payment") != 0u);
        CHECK(mt_objc_send0(cpu, t, "transactionReceipt") == 0u);
        const u32 queue = mt_objc_send0(
            cpu, mt_objc_class_named(mt_objc_current(), "SKPaymentQueue"), "defaultQueue");
        mt_objc_send1(cpu, queue, "finishTransaction:", t);
    }
}
const mt_objc_host_method k_shop_methods[] = {
    {"productsRequest:didReceiveResponse:", shop_products_response},
    {"paymentQueue:updatedTransactions:", shop_updated_transactions}};
const mt_objc_host_class k_shop = {"TestShop", "NSObject", 4, k_shop_methods, 2, nullptr, 0};

} // namespace

TEST_CASE("StoreKit: products are found and every payment ends cancelled (ADR 0034)") {
    Env e;
    g_shop.clear();
    REQUIRE(mt_objc_add_host_class(e.rt, &k_shop, 0, 0) == 0);
    const u32 queue_cls = e.cls("SKPaymentQueue");
    // -[EAGLView initWithCoder:]: +canMakePayments YES, so InAppPurchaseManager is created;
    // its -init adds itself as the queue's observer.
    CHECK(e.send(queue_cls, "canMakePayments") == 1u);
    const u32 queue = e.send(queue_cls, "defaultQueue");
    CHECK(e.send(queue_cls, "defaultQueue") == queue);
    const u32 shop = e.send(e.cls("TestShop"), "new");
    e.send(queue, "addTransactionObserver:", {shop});
    // -loadProductsInfo: (0xBF695)
    const u32 ids =
        e.send(e.cls("NSSet"),
               "setWithObject:", {e.string("com.nexonmobile.maplestory.thief.powpackage")});
    const u32 request =
        e.send(e.send(e.cls("SKProductsRequest"), "alloc"), "initWithProductIdentifiers:", {ids});
    e.send(request, "setDelegate:", {shop});
    e.send(request, "start");
    CHECK(g_shop.empty());
    CHECK(mt_stubs_pump(&e.cpu) == 1u); // the products answer, which adds the payment
    REQUIRE(g_shop.size() == 2u);
    CHECK(g_shop[0] == "products 1");
    CHECK(g_shop[1] == "invalid 0");
    CHECK(mt_stubs_pending() == 1u);
    const u32 pending = e.send(queue, "transactions");
    CHECK(e.send(pending, "count") == 1u);
    CHECK(e.send(e.send(pending, "objectAtIndex:", {0u}), "transactionState") == 0u); // purchasing
    CHECK(mt_stubs_pump(&e.cpu) == 1u); // the payment fails: SKPaymentTransactionStateFailed
    REQUIRE(g_shop.size() == 3u);
    CHECK(g_shop[2] == "transaction state 2");
    CHECK(e.send(e.send(queue, "transactions"), "count") == 0u); // finished
    bool logged = false;
    for (const auto &line : g_log) {
        logged |= line.find("the payment is cancelled") != std::string::npos;
    }
    CHECK(logged);

    // Without an observer, nothing is delivered (InAppPurchaseManager's -dealloc removes it).
    e.send(queue, "removeTransactionObserver:", {shop});
    const u32 product = e.send(e.cls("SKProduct"), "new");
    e.send(queue, "addPayment:", {e.send(e.cls("SKPayment"), "paymentWithProduct:", {product})});
    CHECK(mt_stubs_pump(&e.cpu) == 1u);
    CHECK(g_shop.size() == 3u);
    e.send(product, "release");
    e.send(request, "release");
    e.send(shop, "release");
    CHECK(mt_objc_undeliverable_count(e.rt) == 0u);
}

TEST_CASE("CFSocket fails fast and SCNetworkReachability says not reachable") {
    Env e;
    // CMSocket::Connect (0x1C419): CFSocketCreate(NULL, PF_INET, SOCK_STREAM, 0, data |
    // connect | write, CMSocket_Callback, &context)
    const u32 sock = e.call(mt_hle_CFSocketCreate, {0u, 2u, 1u, 0u, 2u | 4u | 8u, 0x1C651u, 0u});
    REQUIRE(sock != 0u);
    CHECK(e.call(mt_hle_CFSocketGetNative, {sock}) == 0xFFFFFFFFu);
    const auto start = std::chrono::steady_clock::now();
    // CFSocketConnectToAddress(s, address, 10.0): r2:r3 hold the double.
    CHECK(e.call(mt_hle_CFSocketConnectToAddress,
                 {sock, 0x1000u, lo(bits(10.0)), hi(bits(10.0))}) == 0xFFFFFFFFu);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100));
    REQUIRE(!g_log.empty());
    CHECK(g_log.back().find("CFSocketConnectToAddress") != std::string::npos);
    CHECK(g_log.back().find("timeout 10 s") != std::string::npos);
    CHECK(e.call(mt_hle_CFSocketCreateRunLoopSource, {0u, sock, 0u}) != 0u);
    CHECK(e.call(mt_hle_CFSocketSendData, {sock, 0u, 0x1000u, 0u, 0u}) == 0xFFFFFFFFu);
    e.call(mt_hle_CFSocketInvalidate, {sock});

    const u32 flags = e.words({0xFFFFFFFFu});
    CHECK(e.call(mt_hle_SCNetworkReachabilityCreateWithAddress, {0u, 0x1000u}) == 0u);
    CHECK(e.call(mt_hle_SCNetworkReachabilityCreateWithName, {0u, 0x1000u}) == 0u);
    CHECK(e.call(mt_hle_SCNetworkReachabilityGetFlags, {0u, flags}) == 0u);
    CHECK(e.get32(flags) == 0u);
    CHECK(e.call(mt_hle_SCNetworkReachabilityGetFlags, {0x1234u, flags}) == 1u);
    CHECK(e.call(mt_hle_SCNetworkReachabilitySetCallback, {0x1234u, 0u, 0u}) == 0u);
    CHECK(e.call(mt_hle_SCNetworkReachabilityScheduleWithRunLoop, {0x1234u, 0u, 0u}) == 0u);
    CHECK(e.call(mt_hle_SCNetworkReachabilityUnscheduleFromRunLoop, {0x1234u, 0u, 0u}) == 0u);
}

TEST_CASE("deliveries: posted during a pump wait for the next one and a new runtime drops them") {
    Env e;
    g_calls.clear();
    // A handler that starts another request: its answer comes on the next pump.
    mt_guest_fn again = [](mt_cpu *cpu) {
        g_calls.push_back({cpu->r[0], cpu->r[1], 0});
        mt_objc_send0(cpu, mt_objc_class_named(mt_objc_current(), "GKAchievement"), "class");
    };
    const u32 player = e.send(e.cls("GKLocalPlayer"), "localPlayer");
    e.send(player, "authenticateWithCompletionHandler:", {e.block(again)});
    e.send(e.cls("GKAchievement"),
           "loadAchievementsWithCompletionHandler:", {e.block(record_block)});
    CHECK(mt_stubs_pending() == 2u);
    CHECK(mt_stubs_pump(&e.cpu) == 2u);
    CHECK(g_calls.size() == 2u);
    e.send(e.cls("GKAchievement"),
           "loadAchievementsWithCompletionHandler:", {e.block(record_block)});
    CHECK(mt_stubs_pending() == 1u);
    mt_stubs_reset();
    CHECK(mt_stubs_pending() == 0u);
    CHECK(e.send(player, "isAuthenticated") == 0u); // the reset forgot the player's state
}

TEST_CASE("the data directory: set, environment, default and the gamecenter.json path") {
    char path[1100];
    REQUIRE(mt_stubs_set_data_dir("/tmp/x/") == 0);
    REQUIRE(mt_stubs_gamecenter_path(path, sizeof path) == 0);
    CHECK(std::string(path) == "/tmp/x/gamecenter.json");
    REQUIRE(mt_stubs_set_data_dir("/tmp/y") == 0);
    REQUIRE(mt_stubs_gamecenter_path(path, sizeof path) == 0);
#if defined(_WIN32)
    CHECK(std::string(path) == "/tmp/y\\gamecenter.json");
#else
    CHECK(std::string(path) == "/tmp/y/gamecenter.json");
#endif
    CHECK(mt_stubs_gamecenter_path(path, 8) == -1);
    REQUIRE(mt_stubs_set_data_dir(nullptr) == 0);
    const std::string def = mt_stubs_data_dir();
#if defined(_MSC_VER)
    char *env = nullptr;
    std::size_t env_len = 0;
    const bool from_env = _dupenv_s(&env, &env_len, "MAPLETHIEF_DATA_DIR") == 0 && env != nullptr;
    std::free(env);
#else
    const bool from_env = std::getenv("MAPLETHIEF_DATA_DIR") != nullptr;
#endif
    if (!from_env) {
#if defined(__APPLE__)
        CHECK(def.find("/Library/Application Support/MapleThief") != std::string::npos);
#elif defined(_WIN32)
        CHECK(def.find("\\MapleThief") != std::string::npos);
#else
        CHECK(def.find("maplethief") != std::string::npos);
#endif
    }
    const std::string long_dir(2000, 'a');
    CHECK(mt_stubs_set_data_dir(long_dir.c_str()) == -1);
}

TEST_CASE("gamecenter.json's JSON: parsing, escapes, errors, number forms") {
    using namespace mt_stubs_impl;
    const char *text =
        R"({"a": [1, -2.5, 1e3, true, false, null], "s": "x\"\\\/\b\f\n\r\t\u00e9\ud83d\ude00", "o": {}})";
    JsonValue v{};
    const char *error = nullptr;
    REQUIRE(json_parse(text, std::strlen(text), &v, &error));
    const JsonValue *a = json_get(&v, "a");
    REQUIRE((a != nullptr && a->type == JsonType::array && a->count == 6u));
    CHECK((a->items[0].is_integer && a->items[0].integer == 1));
    CHECK((!a->items[1].is_integer && a->items[1].number == -2.5));
    CHECK((!a->items[2].is_integer && a->items[2].number == 1000.0));
    CHECK((a->items[3].type == JsonType::boolean && a->items[3].boolean));
    CHECK((a->items[4].type == JsonType::boolean && !a->items[4].boolean));
    CHECK(a->items[5].type == JsonType::null);
    const JsonValue *s = json_get(&v, "s");
    REQUIRE(s != nullptr);
    CHECK(std::string(s->string) == "x\"\\/\b\f\n\r\t\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(json_get(&v, "o")->type == JsonType::object);
    CHECK(json_get(&v, "missing") == nullptr);
    json_free(&v);

    for (const char *bad : {"", "{", "[1,]", "{\"a\" 1}", "\"\\x\"", "01x", "[1] 2", "\"a\nb\""}) {
        INFO(bad);
        CHECK_FALSE(json_parse(bad, std::strlen(bad), &v, &error));
        CHECK(error != nullptr);
    }
    std::string deep(100, '[');
    CHECK_FALSE(json_parse(deep.c_str(), deep.size(), &v, &error));

    JsonOut o{};
    json_string(&o, "a\"b\\c\n\x01");
    json_raw(&o, " ");
    json_double(&o, 0.1);
    json_raw(&o, " ");
    json_double(&o, 100.0);
    json_raw(&o, " ");
    json_int(&o, -9007199254740993LL);
    CHECK(std::string(o.data) == "\"a\\\"b\\\\c\\n\\u0001\" 0.1 100 -9007199254740993");
    json_out_free(&o);
}
