// The offline service stubs (#52, maplethief/stubs.h, ADR 0034) on a real guest address space
// (#42's heap and a guest stack): GameKit with its gamecenter.json, StoreKit's "sells
// nothing" store, the CFSocket and SCNetworkReachability imports, deferred deliveries and the
// JSON format. Messages are sent the way the game's GameCenterManager and
// InAppPurchaseManager send them (docs/re/objc-shell.md); completion handlers are blocks whose
// invoke functions are test functions (objc_test.cpp's mt_dispatch). The Foundation classes the
// stubs and the game use (NSString, NSArray, NSMutableArray, NSSet) are the runtime's own (#49).
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
#include <random>
#include <string>
#include <vector>

#include "cf/cf_objects.h"
#include "hle/hle.h"
#include "maplethief/cashshop.h"
#include "maplethief/events.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/offline.h"
#include "maplethief/recomp/calls.h"
#include "maplethief/saves.h"
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

// --- the environment ------------------------------------------------------------------------

// One data directory per test process (ctest runs these cases in parallel processes), removed
// when the process ends.
struct TestDir {
    fs::path path = fs::temp_directory_path() /
                    ("maplethief-stubs-test-" + std::to_string(std::random_device{}()) + "-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TestDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
fs::path test_dir() {
    static const TestDir dir;
    return dir.path;
}

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

// --- the offline cash shop's store and receipt server (#156, ADR 0072) -----------------------

namespace {

constexpr const char *k_pow = "com.nexonmobile.maplestory.thief.powpackage";
constexpr const char *k_real_receipt = "ewoJInNpZ25hdHVyZSIgPSAiQXBNVUJDOD";

// The payment policy: records the product ids it is asked about, answers g_accept.
std::vector<std::string> g_asked;
int g_accept = 0;
int test_policy(void *user, mt_cpu *, const char *product_id) {
    CHECK(user == &g_accept);
    g_asked.emplace_back(product_id);
    return g_accept;
}

// -[InAppPurchaseManager paymentQueue:updatedTransactions:] for a paid purchase: records the
// state and the receipt's bytes ("state N receipt <bytes>|<length>").
void paid_updated_transactions(mt_cpu *cpu) {
    const u32 transactions = cpu->r[3];
    const u32 n = mt_objc_send0(cpu, transactions, "count");
    for (u32 i = 0; i < n; i++) {
        const u32 t = mt_objc_send1(cpu, transactions, "objectAtIndex:", i);
        std::string line = "state " + std::to_string(mt_objc_send0(cpu, t, "transactionState"));
        const u32 receipt = mt_objc_send0(cpu, t, "transactionReceipt");
        u32 bytes = 0, length = 0;
        if (receipt != 0 && mt_cf::data_bytes(cpu, receipt, &bytes, &length)) {
            line += " receipt " +
                    std::string(reinterpret_cast<const char *>(cpu->mem + bytes), length) + "|" +
                    std::to_string(length);
        }
        g_shop.push_back(line);
        const u32 queue = mt_objc_send0(
            cpu, mt_objc_class_named(mt_objc_current(), "SKPaymentQueue"), "defaultQueue");
        mt_objc_send1(cpu, queue, "finishTransaction:", t);
    }
}
const mt_objc_host_method k_paid_shop_methods[] = {
    {"productsRequest:didReceiveResponse:", shop_products_response},
    {"paymentQueue:updatedTransactions:", paid_updated_transactions}};
const mt_objc_host_class k_paid_shop = {"TestPaidShop", "NSObject", 4, k_paid_shop_methods, 2,
                                        nullptr,        0};

// -loadProductsInfo: for `id` through the shop object; the purchase's outcome on the next pumps.
void buy(Env &e, u32 shop, const char *id) {
    const u32 ids = e.send(e.cls("NSSet"), "setWithObject:", {e.string(id)});
    const u32 request =
        e.send(e.send(e.cls("SKProductsRequest"), "alloc"), "initWithProductIdentifiers:", {ids});
    e.send(request, "setDelegate:", {shop});
    e.send(request, "start");
    CHECK(mt_stubs_pump(&e.cpu) == 1u); // the products answer, which adds the payment
    CHECK(mt_stubs_pump(&e.cpu) == 1u); // the payment's outcome
    e.send(request, "release");
}

// CMSocket_Callback (0x1C651) as a test function: (type, the data frame's bytes).
struct SocketEvent {
    u32 type;
    std::vector<std::uint8_t> data;
};
std::vector<SocketEvent> g_events;
void record_socket(mt_cpu *cpu) {
    SocketEvent ev{cpu->r[1], {}};
    u32 bytes = 0, length = 0;
    if (cpu->r[3] != 0 && mt_cf::data_bytes(cpu, cpu->r[3], &bytes, &length)) {
        ev.data.assign(cpu->mem + bytes, cpu->mem + bytes + length);
    }
    g_events.push_back(std::move(ev));
}

// A CFData of struct sockaddr_in for a.b.c.d:port.
u32 sockaddr(Env &e, std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d,
             std::uint16_t port) {
    const std::uint8_t sa[16] = {
        16, 2, static_cast<std::uint8_t>(port >> 8), static_cast<std::uint8_t>(port), a, b, c, d};
    return mt_cf::data_create(&e.cpu, sa, 16, false);
}

using Bytes = std::vector<std::uint8_t>;

// A client frame as CMSocket::Send builds it: {u32 length incl. the header, u16 0xFFFF,
// payload}.
Bytes frame(const Bytes &payload) {
    const u32 n = 6u + static_cast<u32>(payload.size());
    Bytes f = {static_cast<std::uint8_t>(n),
               static_cast<std::uint8_t>(n >> 8),
               static_cast<std::uint8_t>(n >> 16),
               static_cast<std::uint8_t>(n >> 24),
               0xFF,
               0xFF};
    f.insert(f.end(), payload.begin(), payload.end());
    return f;
}
// The payload of op 2 / op 4: {u16 op, u16 len, receipt}.
Bytes receipt_payload(std::uint16_t op, const std::string &receipt) {
    Bytes p = {static_cast<std::uint8_t>(op), static_cast<std::uint8_t>(op >> 8),
               static_cast<std::uint8_t>(receipt.size()),
               static_cast<std::uint8_t>(receipt.size() >> 8)};
    p.insert(p.end(), receipt.begin(), receipt.end());
    return p;
}
Bytes join(const Bytes &a, const Bytes &b) {
    Bytes out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}
u32 cf_data(Env &e, const Bytes &bytes) {
    return mt_cf::data_create(&e.cpu, bytes.data(), static_cast<u32>(bytes.size()), false);
}

// A server frame as CMSocket::OnData and CDataMgr::OnServerPacket read it: (op, status, id).
struct Answer {
    u32 op, status;
    std::string id;
};
Answer parse_answer(const Bytes &f) {
    REQUIRE(f.size() >= 10u);
    const u32 length = f[0] | f[1] << 8 | f[2] << 16 | static_cast<u32>(f[3]) << 24;
    CHECK(length == f.size());
    CHECK((f[4] == 0 && f[5] == 0));
    const u32 len = f[9];
    REQUIRE(f.size() == 10u + len);
    return {static_cast<u32>(f[6] | f[7] << 8), f[8],
            std::string(reinterpret_cast<const char *>(f.data()) + 10, len)};
}

// A receipt list file (crestore, ...) in Documents/: i16 count, {u8 flag, i16 len, bytes}.
void write_receipt_list(const char *name, const std::vector<std::string> &receipts) {
    std::string bytes;
    bytes += static_cast<char>(receipts.size());
    bytes += '\0';
    for (const std::string &r : receipts) {
        bytes += '\1';
        bytes += static_cast<char>(r.size() & 0xFF);
        bytes += static_cast<char>(r.size() >> 8);
        bytes += r;
    }
    const fs::path dir(mt_saves_documents_dir());
    fs::create_directories(dir);
    std::ofstream(dir / name, std::ios::binary) << bytes;
}

std::string synthetic(const char *id) {
    char text[200];
    REQUIRE(mt_stubs_receipt_text(id, text, sizeof text) != 0u);
    return text;
}

} // namespace

TEST_CASE("synthetic receipts: the text, the product it names, and real receipts") {
    char text[200];
    const u32 n = mt_stubs_receipt_text(k_pow, text, sizeof text);
    CHECK(std::string(text) == std::string(MT_STUBS_RECEIPT_PREFIX) + k_pow);
    CHECK(n == std::strlen(text));
    char shorter[10];
    CHECK(mt_stubs_receipt_text(k_pow, shorter, sizeof shorter) == 0u); // too short: none
    char id[128];
    const auto *b = reinterpret_cast<const std::uint8_t *>(text);
    REQUIRE(mt_stubs_receipt_product(b, n, id, sizeof id) == 1);
    CHECK(std::string(id) == k_pow);
    // With its NUL, as the game sends it, or cut at a blank.
    REQUIRE(mt_stubs_receipt_product(b, n + 1, id, sizeof id) == 1);
    CHECK(std::string(id) == k_pow);
    const std::string blank = synthetic(k_pow) + " trailing";
    REQUIRE(mt_stubs_receipt_product(reinterpret_cast<const std::uint8_t *>(blank.data()),
                                     static_cast<u32>(blank.size()), id, sizeof id) == 1);
    CHECK(std::string(id) == k_pow);
    // A real App Store receipt (base64 of a signed plist), an empty one, the prefix alone.
    for (const std::string &real :
         {std::string(k_real_receipt), std::string(), std::string(MT_STUBS_RECEIPT_PREFIX)}) {
        CHECK(mt_stubs_receipt_product(reinterpret_cast<const std::uint8_t *>(real.data()),
                                       static_cast<u32>(real.size()), id, sizeof id) == 0);
    }
}

TEST_CASE("StoreKit with a payment policy: accepted payments complete with a synthetic receipt, "
          "refused ones are cancelled") {
    Env e;
    g_shop.clear();
    g_asked.clear();
    REQUIRE(mt_objc_add_host_class(e.rt, &k_paid_shop, 0, 0) == 0);
    const u32 queue = e.send(e.cls("SKPaymentQueue"), "defaultQueue");
    const u32 shop = e.send(e.cls("TestPaidShop"), "new");
    e.send(queue, "addTransactionObserver:", {shop});
    mt_storekit_set_policy(test_policy, &g_accept);

    g_accept = 1; // completed (SKPaymentTransactionStatePurchased), the receipt with its NUL
    buy(e, shop, k_pow);
    REQUIRE(g_asked.size() == 1u);
    CHECK(g_asked[0] == k_pow);
    REQUIRE(g_shop.size() == 3u);
    const std::string receipt = synthetic(k_pow);
    CHECK(g_shop[2] == "state 1 receipt " + receipt + std::string(1, '\0') + "|" +
                           std::to_string(receipt.size() + 1));

    g_accept = 0; // the policy refuses (not sold, not enough meso): cancelled, no receipt
    buy(e, shop, "com.nexonmobile.maplestory.thief.mesopack6");
    REQUIRE(g_asked.size() == 2u);
    CHECK(g_asked[1] == "com.nexonmobile.maplestory.thief.mesopack6");
    REQUIRE(g_shop.size() == 6u);
    CHECK(g_shop[5] == "state 2");

    mt_storekit_set_policy(nullptr, nullptr); // off: cancelled without asking
    buy(e, shop, k_pow);
    CHECK(g_asked.size() == 2u);
    REQUIRE(g_shop.size() == 9u);
    CHECK(g_shop[8] == "state 2");
    CHECK(e.send(e.send(queue, "transactions"), "count") == 0u);
    e.send(queue, "removeTransactionObserver:", {shop});
    e.send(shop, "release");
}

TEST_CASE("the fake receipt server: connects only for a synthetic receipt, answers op 0/2/4") {
    Env e;
    g_events.clear();
    const u32 callout = imp(record_socket);
    const u32 server = sockaddr(e, 50, 17, 236, 149, 20001);
    const u32 other = sockaddr(e, 127, 0, 0, 1, 20001);
    const auto connect = [&e](u32 sock, u32 address) {
        return e.call(mt_hle_CFSocketConnectToAddress,
                      {sock, address, lo(bits(10.0)), hi(bits(10.0))});
    };
    const auto make_socket = [&e, callout] {
        return e.call(mt_hle_CFSocketCreate, {0u, 2u, 1u, 0u, 2u | 4u | 8u, callout, 0u});
    };
    const mt_stubs_server_stats before = mt_stubs_get_server_stats();

    // Nothing waits (the Pigmy hatch; cash_shop = off with no synthetic receipt left): it fails
    // at once and no event ever comes. A real App Store receipt in a list changes nothing.
    u32 sock = make_socket();
    CHECK(connect(sock, server) == 0xFFFFFFFFu);
    write_receipt_list("crestore", {k_real_receipt});
    CHECK(connect(sock, server) == 0xFFFFFFFFu);
    CHECK(mt_stubs_pump(&e.cpu) == 0u);
    CHECK(g_events.empty());
    e.call(mt_hle_CFSocketInvalidate, {sock});

    // A synthetic receipt waits in riprestore1 (a revive bought before a crash): connected, but
    // only to the receipt server's address.
    write_receipt_list("riprestore1", {synthetic(k_pow)});
    sock = make_socket();
    CHECK(connect(sock, other) == 0xFFFFFFFFu);
    CHECK(connect(sock, server) == 0u);
    CHECK(g_events.empty());
    CHECK(mt_stubs_pump(&e.cpu) == 1u); // kCFSocketConnectCallBack, kCFSocketWriteCallBack
    REQUIRE(g_events.size() == 2u);
    CHECK(g_events[0].type == 4u);
    CHECK(g_events[1].type == 8u);
    CHECK(g_events[0].data.empty());

    // Two frames in one send (CMSocket::FlushSendQueue): hello, then the receipt (op 2) with the
    // NUL the game sends.
    const Bytes receipt = frame(receipt_payload(2, synthetic(k_pow) + '\0'));
    CHECK(e.call(mt_hle_CFSocketSendData,
                 {sock, 0u, cf_data(e, join(frame({0, 0}), receipt)), 0u}) == 0u);
    CHECK(g_events.size() == 2u); // the answers come on the next pump
    CHECK(mt_stubs_pump(&e.cpu) == 2u);
    REQUIRE(g_events.size() == 4u);
    CHECK(g_events[2].type == 3u);
    Answer a = parse_answer(g_events[2].data);
    CHECK((a.op == 1u && a.status == 0u && a.id.empty()));
    CHECK(g_events[3].type == 3u);
    a = parse_answer(g_events[3].data);
    CHECK((a.op == 3u && a.status == 0u && a.id == k_pow));

    // A restore (op 4) of the Comeback Pack; a keep-alive (0xB4) gets no answer.
    const char *comeback = "com.nexonmobile.maplestory.thief.ComebackPack";
    const Bytes restore = frame(receipt_payload(4, synthetic(comeback)));
    CHECK(e.call(mt_hle_CFSocketSendData,
                 {sock, 0u, cf_data(e, join(restore, frame({0xB4, 0}))), 0u}) == 0u);
    CHECK(mt_stubs_pump(&e.cpu) == 1u);
    REQUIRE(g_events.size() == 5u);
    a = parse_answer(g_events[4].data);
    CHECK((a.op == 5u && a.status == 0u && a.id == comeback));

    // A real receipt is never confirmed: status 1 (the game's popup 0x20, as the dead server's).
    const Bytes real = frame(receipt_payload(2, k_real_receipt));
    CHECK(e.call(mt_hle_CFSocketSendData, {sock, 0u, cf_data(e, real), 0u}) == 0u);
    CHECK(mt_stubs_pump(&e.cpu) == 1u);
    REQUIRE(g_events.size() == 6u);
    a = parse_answer(g_events[5].data);
    CHECK((a.op == 3u && a.status == 1u && a.id.empty()));

    // A malformed frame (a length past the data) is ignored.
    CHECK(e.call(mt_hle_CFSocketSendData,
                 {sock, 0u, cf_data(e, {0xFF, 0, 0, 0, 0xFF, 0xFF, 0, 0}), 0u}) == 0u);
    CHECK(mt_stubs_pump(&e.cpu) == 0u);

    // CMSocket::Disconnect: invalidated, so nothing more is sent or answered.
    const u32 hello = cf_data(e, frame({0, 0}));
    CHECK(e.call(mt_hle_CFSocketSendData, {sock, 0u, hello, 0u}) == 0u);
    e.call(mt_hle_CFSocketInvalidate, {sock});
    CHECK(mt_stubs_pump(&e.cpu) == 1u); // the answer posted before is dropped
    CHECK(g_events.size() == 6u);
    CHECK(e.call(mt_hle_CFSocketSendData, {sock, 0u, hello, 0u}) == 0xFFFFFFFFu);

    const mt_stubs_server_stats after = mt_stubs_get_server_stats();
    CHECK(after.connects - before.connects == 4u);
    CHECK(after.accepted - before.accepted == 1u);
    CHECK(after.frames - before.frames == 6u);
    CHECK(after.answers - before.answers == 4u);
    CHECK(after.confirmed - before.confirmed == 2u);
    CHECK(after.rejected - before.rejected == 1u);
}

// --- the offline cash shop on a synthetic guest (#156, ADR 0072) ------------------------------

namespace {

// The guest the cash shop reads (maplethief/cashshop.h): SkyCanvas 0x11A950 -> the state +0xC
// (a CStateGame: vtable pointer 0x119B78) -> the hero +0x30 (level +0x80, meso +0xD0); the game
// mode 0x11AA00, the open popup 0x108C64; CDataMgr's cached slot buffers +0x4DC.
struct CashGuest {
    Env &e;
    u32 hero = 0, mgr = 0, buffers = 0;
    explicit CashGuest(Env &env) : e(env) {
        mt_segment data{};
        std::strcpy(data.name, "__DATA");
        data.vmaddr = 0x108000u;
        data.vmsize = 0x14000u;
        data.initprot = MT_PROT_READ | MT_PROT_WRITE;
        REQUIRE(mt_mem_map_segment(e.mem, &data, nullptr, 0u) == 0);
        const u32 canvas = mt_heap_calloc(e.mem, 1, 0x40);
        const u32 state = mt_heap_calloc(e.mem, 1, 0x40);
        hero = mt_heap_calloc(e.mem, 1, 0x164);
        mgr = mt_heap_calloc(e.mem, 1, 0x600);
        buffers = mt_heap_calloc(e.mem, 3, 4);
        REQUIRE((canvas != 0 && state != 0 && hero != 0 && mgr != 0 && buffers != 0));
        set(0x11A950u, canvas);
        set(canvas + 0x0Cu, state);
        set(state, 0x119B78u);
        set(state + 0x30u, hero);
        set(mgr + 0x4DCu, buffers);
        set(hero + 0x80u, 10);   // level
        set(hero + 0x94u, 100);  // HP
        set(hero + 0x9Cu, 100);  // max HP
        set(hero + 0xD0u, 1000); // meso
    }
    void set(u32 addr, u32 v) { std::memcpy(e.cpu.mem + addr, &v, 4); }
    void set(u32 addr, int v) { set(addr, static_cast<u32>(v)); }
    int get(u32 addr) const {
        int v;
        std::memcpy(&v, e.cpu.mem + addr, 4);
        return v;
    }
    int meso() const { return get(hero + 0xD0u); }
    // The generated code's observer at the entry of `fn`.
    void enter(u32 fn, u32 r0, u32 r1 = 0) {
        e.cpu.r[0] = r0;
        e.cpu.r[1] = r1;
        mt_observe_entry(&e.cpu, fn);
    }
    void load_slot(int slot, bool cached) {
        set(buffers + 4u * static_cast<u32>(slot), cached ? 0x01000040u : 0u);
        enter(MT_EVENTS_FN_LOAD_SAVE_DATA, mgr, static_cast<u32>(slot));
    }
};

// CHero::ExpToLevelUp(hero) 0x65919 and CHero::setMeso(hero, delta, log) 0x618E1, as the
// game's: 20 EXP a level, and no payment below 0 meso ("Not enough Mesos.").
int g_set_meso_calls = 0, g_not_enough = 0;
void exp_to_level_up(mt_cpu *cpu) {
    int level;
    std::memcpy(&level, cpu->mem + cpu->r[0] + 0x80u, 4);
    cpu->r[0] = static_cast<u32>(20 * level);
}
void set_meso(mt_cpu *cpu) {
    g_set_meso_calls++;
    CHECK(cpu->r[2] == 1u);
    int meso;
    std::memcpy(&meso, cpu->mem + cpu->r[0] + 0xD0u, 4);
    const int delta = static_cast<int>(cpu->r[1]);
    if (meso + delta < 0) {
        g_not_enough++;
        cpu->r[0] = 0;
        return;
    }
    meso += delta;
    std::memcpy(cpu->mem + cpu->r[0] + 0xD0u, &meso, 4);
    cpu->r[0] = 1;
}

std::int64_t quoted(const mt_cash_view &v, u32 product) {
    for (u32 i = 0; i < v.count; i++) {
        if (v.quotes[i].product == product) {
            return v.quotes[i].price;
        }
    }
    return -2;
}

mt_cash_counters counters_file(int slot) {
    char path[1100];
    REQUIRE(mt_cash_shop_counters_path(slot, path, sizeof path) == 0);
    const std::string text = slurp(path);
    mt_cash_counters c{};
    CHECK(mt_cash_counters_parse(text.data(), text.size(), &c) == 0);
    return c;
}

} // namespace

TEST_CASE("the cash shop for meso: counters from the game events, quotes, setMeso payments") {
    Env e;
    CashGuest guest(e);
    g_shop.clear();
    g_set_meso_calls = g_not_enough = 0;
    objc_test::add_imp(0x65919u, exp_to_level_up);
    objc_test::add_imp(0x618E1u, set_meso);
    REQUIRE(mt_objc_add_host_class(e.rt, &k_paid_shop, 0, 0) == 0);
    const u32 queue = e.send(e.cls("SKPaymentQueue"), "defaultQueue");
    const u32 shop = e.send(e.cls("TestPaidShop"), "new");
    e.send(queue, "addTransactionObserver:", {shop});

    mt_cash_shop_uninstall();
    // The defaults without the floors, so that the level and the steps show alone: a level-10
    // hero earns 200 EXP x 1.64 = 328 meso over the level.
    mt_cash_knobs knobs = *mt_cash_default_knobs();
    for (u32 &r : knobs.resale) {
        r = 0;
    }
    knobs.wealth_floor_pct = 0;
    knobs.min_price = 0;
    mt_cash_shop_set_knobs(&knobs);
    mt_cash_shop_install(MT_CASH_SHOP_MESO);
    CHECK(mt_cash_shop_mode() == MT_CASH_SHOP_MESO);
    const std::string exp40 = std::string(MT_CASH_PRODUCT_PREFIX) + "exppack1";
    const std::string marble = std::string(MT_CASH_PRODUCT_PREFIX) + "SPMarbleSP1Plus";

    // Slot 0's character (cached), no counters file yet: they take the hero's level.
    guest.load_slot(0, true);
    mt_cash_shop_turn(&e.cpu);
    mt_cash_view v = mt_cash_shop_view();
    CHECK(v.screen == MT_CASH_SCREEN_NONE);
    CHECK((v.slot == 0 && v.level == 10 && v.meso == 1000));
    CHECK(counters_file(0).level == 10);

    // The cash shop (game mode 0xB): its 8 tiles, the meso packs not sold.
    guest.set(0x11AA00u, 0xB);
    mt_cash_shop_turn(&e.cpu);
    v = mt_cash_shop_view();
    CHECK(v.screen == MT_CASH_SCREEN_SHOP);
    CHECK(v.count == 8u);
    CHECK(quoted(v, MT_CASH_EXP40) == 330);        // 328, rounded up to 10
    CHECK(quoted(v, MT_CASH_EXP80) == 660);        // 656
    CHECK(quoted(v, MT_CASH_SP_MARBLE) == 410);    // 125 %
    CHECK(quoted(v, MT_CASH_INITIALIZE_SP) == 90); // 25 %: 82
    CHECK(quoted(v, MT_CASH_MESOPACK6) == -1);
    CHECK(mt_cash_shop_quote(MT_CASH_EXP40) == 330);

    // Bought: setMeso(hero, -330, 1), the receipt, the counters; the next one is dearer.
    buy(e, shop, exp40.c_str());
    REQUIRE(g_shop.size() == 3u);
    CHECK(g_shop[2].rfind("state 1 receipt " + synthetic(exp40.c_str()), 0) == 0);
    CHECK(guest.meso() == 670);
    CHECK(g_set_meso_calls == 1);
    CHECK(mt_cash_shop_get_stats().sold == 1u);
    CHECK(mt_cash_shop_get_stats().last_price == 330);
    CHECK(mt_cash_shop_counters().bought[MT_CASH_EXP40] == 1u);
    CHECK(counters_file(0).bought[MT_CASH_EXP40] == 1u);
    mt_cash_shop_turn(&e.cpu);
    v = mt_cash_shop_view();
    CHECK(quoted(v, MT_CASH_EXP40) == 500); // x 1.5: 492
    CHECK(quoted(v, MT_CASH_EXP80) == 660); // its own count
    CHECK(v.meso == 670);

    // Not enough meso: the game's setMeso refuses ("Not enough Mesos."), the payment is
    // cancelled. A meso pack is never sold: setMeso is not even called.
    guest.set(guest.hero + 0xD0u, 300);
    buy(e, shop, marble.c_str());
    CHECK(g_shop.back() == "state 2");
    CHECK(g_not_enough == 1);
    CHECK(guest.meso() == 300);
    buy(e, shop, (std::string(MT_CASH_PRODUCT_PREFIX) + "mesopack6").c_str());
    CHECK(g_shop.back() == "state 2");
    CHECK(g_set_meso_calls == 2);
    CHECK(mt_cash_shop_get_stats().refused == 2u);
    CHECK(mt_cash_shop_counters().bought[MT_CASH_SP_MARBLE] == 0u);

    // A fall: the death popup 0x68, opened again after a sub-popup (one death).
    guest.set(0x11AA00u, 0);
    guest.set(0x108C64u, 0x68);
    guest.enter(MT_EVENTS_FN_POPUP_OPEN, guest.mgr, 0x68u);
    guest.enter(MT_EVENTS_FN_POPUP_OPEN, guest.mgr, 0x16u); // another popup: no death
    guest.enter(MT_EVENTS_FN_POPUP_OPEN, guest.mgr, 0x68u);
    CHECK(mt_cash_shop_counters().deaths == 1u);
    mt_cash_shop_turn(&e.cpu);
    v = mt_cash_shop_view();
    CHECK(v.screen == MT_CASH_SCREEN_DEATH_POPUP);
    CHECK(v.count == 2u);
    const std::int64_t first = quoted(v, MT_CASH_POW);
    CHECK(first == 330);
    CHECK(quoted(v, MT_CASH_COMEBACK) == 660);

    // A paid revive, then the next fall at this level: two steps dearer.
    guest.set(guest.hero + 0xD0u, 10000);
    buy(e, shop, k_pow);
    CHECK(guest.meso() == 10000 - 330);
    guest.enter(MT_EVENTS_FN_REVIVE, guest.hero);
    guest.enter(MT_EVENTS_FN_POPUP_OPEN, guest.mgr, 0x68u);
    mt_cash_shop_turn(&e.cpu);
    v = mt_cash_shop_view();
    const mt_cash_counters c = mt_cash_shop_counters();
    CHECK((c.deaths == 2u && c.paid_revives == 1u));
    CHECK(quoted(v, MT_CASH_POW) == 740); // 328 x 1.5^2 = 738
    CHECK(quoted(v, MT_CASH_POW) > first);
    mt_cash_counters saved = counters_file(0);
    CHECK((saved.level == 10 && saved.deaths == 2u && saved.paid_revives == 1u));
    CHECK((saved.bought[MT_CASH_POW] == 1u && saved.bought[MT_CASH_EXP40] == 1u));

    // A level-up cools everything down.
    guest.enter(MT_EVENTS_FN_LEVEL_UP, guest.hero);
    guest.set(guest.hero + 0x80u, 11);
    mt_cash_shop_turn(&e.cpu);
    saved = counters_file(0);
    CHECK((saved.level == 11 && saved.deaths == 0u && saved.paid_revives == 0u));
    CHECK(saved.bought[MT_CASH_EXP40] == 0u);
    CHECK(quoted(mt_cash_shop_view(), MT_CASH_POW) == 370); // 220 x 1.64 = 360.8

    // A new character in slot 1 starts afresh; slot 0's counters are read back.
    guest.load_slot(1, false);
    CHECK(counters_file(1).level == 1);
    CHECK(mt_cash_shop_counters().level == 1);
    guest.load_slot(0, true);
    CHECK(mt_cash_shop_counters().level == 11);

    // Off: nothing is quoted and the store cancels without asking.
    mt_cash_shop_install(MT_CASH_SHOP_OFF);
    mt_cash_shop_turn(&e.cpu);
    CHECK(mt_cash_shop_view().screen == MT_CASH_SCREEN_NONE);
    const int calls = g_set_meso_calls;
    buy(e, shop, k_pow);
    CHECK(g_shop.back() == "state 2");
    CHECK(g_set_meso_calls == calls);

    mt_cash_shop_uninstall();
    e.send(queue, "removeTransactionObserver:", {shop});
    e.send(shop, "release");
}

// --- the local Game Center (#154, ADR 0084) ---------------------------------------------------

namespace {

// -[GameCenterManager ReportScore:forCategory:]'s sends.
void report_score(Env &e, const char *category, std::uint64_t value) {
    const u32 score =
        e.send(e.send(e.cls("GKScore"), "alloc"), "initWithCategory:", {e.string(category)});
    e.send(score, "setValue:", {lo(value), hi(value)});
    e.send(score, "reportScoreWithCompletionHandler:", {0u});
    e.send(score, "release");
}

// The leaderboard controller's delegate (EAGLView's -leaderboardViewControllerDidFinish:).
std::vector<u32> g_finished;
void delegate_finished(mt_cpu *cpu) { g_finished.push_back(cpu->r[2]); }
const mt_objc_host_method k_lb_delegate_methods[] = {
    {"leaderboardViewControllerDidFinish:", delegate_finished}};
const mt_objc_host_class k_lb_delegate = {
    "TestLeaderboardDelegate", "NSObject", 4, k_lb_delegate_methods, 1, nullptr, 0};

std::vector<std::string> g_shown;
void show_leaderboard(void *, const char *category) { g_shown.emplace_back(category); }

} // namespace

TEST_CASE("GameKit: the local leaderboard keeps each player's scores across a restart") {
    {
        Env e;
        mt_gamekit_set_player_alias("Slot 1");
        report_score(e, k_ranking, 500);
        report_score(e, k_ranking, 700);
        report_score(e, k_ranking, 650); // lower: the best stays, the last changes
        mt_gamekit_set_player_alias("Slot 2");
        CHECK(std::string(mt_gamekit_player_alias()) == "Slot 2");
        report_score(e, k_ranking, 0x100000002ull);
        mt_gamekit_set_player_alias(nullptr);
        CHECK(std::string(mt_gamekit_player_alias()) == "Player");
        report_score(e, "another.category", 5);
        const std::string json = slurp(gamecenter_file());
        INFO(json);
        CHECK(json.find(std::string("{\"category\": \"") + k_ranking +
                        "\", \"player\": \"Slot 1\", \"best\": 700, \"last\": 650, "
                        "\"reports\": 3}") != std::string::npos);
        // The category's own line counts every player's reports, as before.
        CHECK(json.find(std::string("{\"category\": \"") + k_ranking +
                        "\", \"best\": 4294967298, \"last\": 4294967298, \"reports\": 4}") !=
              std::string::npos);
        CHECK(mt_objc_undeliverable_count(e.rt) == 0u);
    }
    {
        // A new process reads them back, best first.
        Env e(false);
        mt_gamekit_entry rows[4]{};
        REQUIRE(mt_gamekit_entries(k_ranking, rows, 4) == 2u);
        CHECK(std::string(rows[0].player) == "Slot 2");
        CHECK(rows[0].best == 0x100000002ll);
        CHECK(rows[0].reports == 1);
        CHECK(std::string(rows[1].player) == "Slot 1");
        CHECK((rows[1].best == 700 && rows[1].last == 650 && rows[1].reports == 3));
        CHECK(std::string(rows[1].category) == k_ranking);
        // Every category; a short array gets the best ones and the count of all.
        CHECK(mt_gamekit_entries(nullptr, rows, 4) == 3u);
        CHECK(std::string(rows[2].player) == "Player");
        CHECK(std::string(rows[2].category) == "another.category");
        mt_gamekit_entry one{};
        CHECK(mt_gamekit_entries(nullptr, &one, 1) == 3u);
        CHECK(std::string(one.player) == "Slot 2");
        CHECK(mt_gamekit_entries("nothing.here", rows, 4) == 0u);
        // A report after the restart adds to the entry.
        mt_gamekit_set_player_alias("Slot 1");
        report_score(e, k_ranking, 800);
        mt_gamekit_set_player_alias(nullptr);
        REQUIRE(mt_gamekit_entries(k_ranking, rows, 4) == 2u);
        CHECK(std::string(rows[1].player) == "Slot 1");
        CHECK((rows[1].best == 800 && rows[1].reports == 4));
    }
}

TEST_CASE("GameKit: with the host's hook the leaderboard waits until the host closes it") {
    Env e;
    REQUIRE(mt_objc_add_host_class(e.rt, &k_lb_delegate, 0, 0) == 0);
    g_finished.clear();
    g_shown.clear();
    const u32 delegate = e.send(e.cls("TestLeaderboardDelegate"), "new");
    // -[EAGLView viewGameCenterRank:] (0x4E09): alloc, init, the category, the delegate.
    auto controller = [&e, delegate] {
        const u32 c = e.send(e.send(e.cls("GKLeaderboardViewController"), "alloc"), "init");
        e.send(c, "setCategory:", {e.string(k_ranking)});
        e.send(c, "setLeaderboardDelegate:", {delegate});
        return c;
    };
    // Without a hook (ADR 0034): finished at once, on the next pump.
    const u32 first = controller();
    e.send(first, "viewDidAppear:", {1u});
    CHECK(mt_gamekit_leaderboard_is_open() == 0);
    mt_stubs_pump(&e.cpu);
    REQUIRE(g_finished.size() == 1u);
    CHECK(g_finished[0] == first);

    // With it: the host is told the category and the controller waits for it.
    mt_gamekit_set_leaderboard_hook(show_leaderboard, nullptr);
    const u32 second = controller();
    e.send(second, "viewDidAppear:", {1u});
    REQUIRE(g_shown.size() == 1u);
    CHECK(g_shown[0] == k_ranking);
    CHECK(mt_gamekit_leaderboard_is_open() == 1);
    mt_stubs_pump(&e.cpu);
    mt_stubs_pump(&e.cpu);
    CHECK(g_finished.size() == 1u);
    mt_gamekit_leaderboard_close();
    CHECK(mt_gamekit_leaderboard_is_open() == 0);
    CHECK(g_finished.size() == 1u); // never inside the call
    mt_stubs_pump(&e.cpu);
    REQUIRE(g_finished.size() == 2u);
    CHECK(g_finished[1] == second);
    mt_gamekit_leaderboard_close(); // nothing open: nothing happens
    mt_stubs_pump(&e.cpu);
    CHECK(g_finished.size() == 2u);

    // A new runtime drops a controller that was still waiting.
    const u32 third = controller();
    e.send(third, "viewDidAppear:", {1u});
    CHECK(mt_gamekit_leaderboard_is_open() == 1);
    mt_gamekit_set_leaderboard_hook(nullptr, nullptr);
    mt_stubs_reset();
    CHECK(mt_gamekit_leaderboard_is_open() == 0);
    e.send(first, "release");
    e.send(second, "release");
    e.send(delegate, "release");
    CHECK(mt_objc_undeliverable_count(e.rt) == 0u);
}

TEST_CASE("local achievements: the game events unlock each once, it persists and queues a toast") {
    {
        Env e;
        CashGuest guest(e);
        mt_achievement_clear_notices();
        REQUIRE(mt_achievements_count() == 7u);
        for (u32 i = 0; i < mt_achievements_count(); i++) {
            CHECK(mt_achievement_unlocked(i) == 0);
        }
        CHECK(mt_achievement_at(7) == nullptr);
        mt_achievements_install(1);
        CHECK(mt_achievements_installed() == 1);

        guest.load_slot(1, false); // a new character in slot 1 (the second)
        CHECK(std::string(mt_gamekit_player_alias()) == "Slot 2");
        guest.set(guest.hero + 0x80u, 9); // level 9 -> 10
        guest.enter(MT_EVENTS_FN_LEVEL_UP, guest.hero);
        guest.set(guest.hero + 0x80u, 29); // -> 30: Rookie is already there
        guest.enter(MT_EVENTS_FN_LEVEL_UP, guest.hero);
        guest.enter(MT_EVENTS_FN_POPUP_OPEN, guest.mgr, 0x68u);
        guest.enter(MT_EVENTS_FN_POPUP_OPEN, guest.mgr, 0x68u); // again: no second unlock
        guest.enter(MT_EVENTS_FN_POPUP_OPEN, guest.mgr, 0x20u); // another popup
        guest.enter(MT_EVENTS_FN_REVIVE, guest.hero);
        guest.load_slot(0, true); // slot 0's saved character
        CHECK(std::string(mt_gamekit_player_alias()) == "Slot 1");

        std::vector<u32> notices;
        u32 index = 99;
        while (mt_achievement_next_notice(&index) != 0) {
            notices.push_back(index);
        }
        CHECK(notices == std::vector<u32>{0u, 1u, 2u, 5u, 6u});
        CHECK(std::string(mt_achievement_at(1)->title) == "Rookie");
        CHECK(mt_achievement_unlock(1) == 0); // already
        CHECK(mt_achievement_unlock(7) == -1);
        CHECK(mt_achievement_next_notice(&index) == 0);
        const std::string json = slurp(gamecenter_file());
        CHECK(json.find("{\"identifier\": \"maplethief.local.level_10\", \"percentComplete\": "
                        "100}") != std::string::npos);
        mt_achievements_install(0);
        CHECK(mt_achievements_installed() == 0);
        guest.set(guest.hero + 0x80u, 49); // off: level 50 unlocks nothing
        guest.enter(MT_EVENTS_FN_LEVEL_UP, guest.hero);
        CHECK(mt_achievement_unlocked(3) == 0);
        CHECK(mt_achievement_next_notice(&index) == 0);
        mt_gamekit_set_player_alias(nullptr);
    }
    {
        // A new process: unlocked stays unlocked, and the game's GameKit sees them too.
        Env e(false);
        const std::vector<int> want = {1, 1, 1, 0, 0, 1, 1};
        for (u32 i = 0; i < mt_achievements_count(); i++) {
            CAPTURE(i);
            CHECK(mt_achievement_unlocked(i) == want[i]);
        }
        g_calls.clear();
        mt_guest_fn keep = [](mt_cpu *cpu) {
            g_calls.push_back({cpu->r[0], cpu->r[1], cpu->r[2]});
            mt_objc_retain(cpu, cpu->r[1]);
        };
        e.send(e.cls("GKAchievement"), "loadAchievementsWithCompletionHandler:", {e.block(keep)});
        mt_stubs_pump(&e.cpu);
        REQUIRE(g_calls.size() == 1u);
        CHECK(e.send(g_calls[0].a, "count") == 5u);
        e.send(g_calls[0].a, "release");
        CHECK(mt_achievement_unlock(3) == 1); // Veteran, now
        u32 index = 0;
        CHECK(mt_achievement_next_notice(&index) == 1);
        CHECK(index == 3u);
    }
}

// --- the dead online UI (#154, ADR 0084) ------------------------------------------------------

namespace {

u32 fbits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}

// A stand-in for the game's MainMenuView: the two methods the overrides wrap, with the ivars
// they read (+152 moregame, +156 gamecenter).
struct MenuCalls {
    std::vector<u32> init_args;   // initWithFrame:'s four words, as the original got them
    std::vector<u32> button_args; // addButtonWithImageName:...'s twelve words
    u32 inits = 0, buttons = 0;
};
MenuCalls g_menu;

void menu_init_with_frame(mt_cpu *cpu) {
    const u32 self = cpu->r[0];
    mt_args a = mt_objc_args(cpu);
    u32 frame[4];
    for (u32 &w : frame) {
        w = mt_arg_u32(&a);
    }
    g_menu.inits++;
    g_menu.init_args.assign(frame, frame + 4);
    mt_objc *rt = mt_objc_current();
    const u32 me = mt_objc_msg_super(cpu, self, mt_objc_class_named(rt, "MainMenuView"),
                                     mt_objc_sel(rt, "initWithFrame:"), frame, 4);
    for (const u32 at : {152u, 156u}) {
        const u32 v = mt_objc_send0(cpu, mt_objc_class_named(rt, "UIView"), "alloc");
        const u32 view = mt_objc_msg(cpu, v, mt_objc_sel(rt, "initWithFrame:"), frame, 4);
        std::memcpy(cpu->mem + me + at, &view, 4);
    }
    cpu->r[0] = me;
}

void menu_add_button(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    g_menu.button_args.clear();
    for (int i = 0; i < 12; i++) {
        g_menu.button_args.push_back(mt_arg_u32(&a));
    }
    g_menu.buttons++;
    mt_objc *rt = mt_objc_current();
    cpu->r[0] = mt_objc_send1(cpu, mt_objc_class_named(rt, "UIButton"), "buttonWithType:", 0u);
}

const mt_objc_host_method k_menu_methods[] = {
    {"initWithFrame:", menu_init_with_frame},
    {"addButtonWithImageName:selectedImageName:withFrame:withEdgeInsets:actionForTouchDown:"
     "actionForTouchUpInside:",
     menu_add_button},
};
const mt_objc_host_class k_test_menu = {"MainMenuView", "UIView", 172, k_menu_methods, 2,
                                        nullptr,        0};

struct Menu {
    Env &e;
    u32 view = 0;
    explicit Menu(Env &env) : e(env) {
        view = e.send(e.send(e.cls("MainMenuView"), "alloc"),
                      "initWithFrame:", {fbits(0), fbits(0), fbits(480), fbits(320)});
    }
    u32 ivar(u32 at) const { return e.get32(view + at); }
    bool hidden(u32 v) { return e.send(v, "isHidden") != 0u; }
    // A button as -drawRect: and -clickAbout: add them: image names, a frame, insets, the
    // touch-down and touch-up-inside selectors.
    u32 button(const char *up) {
        return e.send(view,
                      "addButtonWithImageName:selectedImageName:withFrame:withEdgeInsets:"
                      "actionForTouchDown:actionForTouchUpInside:",
                      {0x1111u, 0x2222u, fbits(436), fbits(262), fbits(57), fbits(57), fbits(0),
                       fbits(-28), fbits(0), fbits(0), 0u, mt_objc_sel(e.rt, up)});
    }
};

} // namespace

TEST_CASE("offline UI: the dead online buttons and images of MainMenuView are hidden") {
    CHECK(mt_offline_ui_hides_action(MT_OFFLINE_HIDE_LINKS, "clickMoreGame:") == 1);
    CHECK(mt_offline_ui_hides_action(MT_OFFLINE_HIDE_LINKS, "clickADButton:") == 1);
    CHECK(mt_offline_ui_hides_action(MT_OFFLINE_HIDE_LINKS, "clickADClose:") == 1);
    CHECK(mt_offline_ui_hides_action(MT_OFFLINE_HIDE_LINKS, "clickLinkURL4:") == 1);
    CHECK(mt_offline_ui_hides_action(MT_OFFLINE_HIDE_LINKS, "clickGameCenter:") == 0);
    CHECK(mt_offline_ui_hides_action(MT_OFFLINE_HIDE_GAME_CENTER, "clickGameCenter:") == 1);
    CHECK(mt_offline_ui_hides_action(MT_OFFLINE_HIDE_GAME_CENTER, "clickMoreGame:") == 0);
    for (const char *live : {"clickPlay:", "clickHelp:", "clickAbout:", "closeAbout:",
                             "clickAboutBuild:", "closeHelp:"}) {
        CAPTURE(live);
        CHECK(mt_offline_ui_hides_action(MT_OFFLINE_HIDE_LINKS | MT_OFFLINE_HIDE_GAME_CENTER,
                                         live) == 0);
    }

    SUBCASE("off: nothing is installed, the menu is the game's") {
        Env e;
        REQUIRE(mt_objc_add_host_class(e.rt, &k_test_menu, 0, 0) == 0);
        mt_offline_ui_configure(0);
        const u32 before = mt_objc_get_stats(e.rt).overrides;
        CHECK(mt_offline_ui_install(e.rt) == 0u);
        CHECK(mt_objc_get_stats(e.rt).overrides == before);
        Menu m(e);
        CHECK_FALSE(m.hidden(m.ivar(152)));
        CHECK_FALSE(m.hidden(m.button("clickMoreGame:")));
    }
    SUBCASE("hide_online_ui with the local Game Center: the banner, More Games, the links") {
        Env e;
        REQUIRE(mt_objc_add_host_class(e.rt, &k_test_menu, 0, 0) == 0);
        mt_offline_ui_configure(MT_OFFLINE_HIDE_LINKS);
        const mt_offline_ui_stats was = mt_offline_ui_get_stats();
        CHECK(mt_offline_ui_install(e.rt) == 2u);
        CHECK(mt_offline_ui_get_stats().installed == 2u);
        g_menu = MenuCalls{};
        Menu m(e);
        // The game's method ran first, with its arguments as sent.
        CHECK(g_menu.inits == 1u);
        CHECK(g_menu.init_args == std::vector<u32>{fbits(0), fbits(0), fbits(480), fbits(320)});
        CHECK(m.view != 0u);
        CHECK(m.hidden(m.ivar(152)));       // moregame
        CHECK_FALSE(m.hidden(m.ivar(156))); // gamecenter
        CHECK(mt_offline_ui_get_stats().views_hidden == was.views_hidden + 1u);
        for (const char *dead :
             {"clickMoreGame:", "clickADButton:", "clickADClose:", "clickLinkURL1:",
              "clickLinkURL2:", "clickLinkURL3:", "clickLinkURL4:"}) {
            CAPTURE(dead);
            const u32 b = m.button(dead);
            REQUIRE(b != 0u);
            CHECK(m.hidden(b));
        }
        CHECK(g_menu.buttons == 7u);
        REQUIRE(g_menu.button_args.size() == 12u);
        CHECK(g_menu.button_args[0] == 0x1111u);
        CHECK(g_menu.button_args[2] == fbits(436));
        CHECK(g_menu.button_args[7] == fbits(-28)); // the insets' left
        CHECK(g_menu.button_args[10] == 0u);        // no touch-down action
        CHECK(g_menu.button_args[11] == mt_objc_sel(e.rt, "clickLinkURL4:"));
        CHECK(mt_offline_ui_get_stats().buttons_hidden == was.buttons_hidden + 7u);
        for (const char *live : {"clickPlay:", "clickHelp:", "clickAbout:", "clickGameCenter:"}) {
            CAPTURE(live);
            CHECK_FALSE(m.hidden(m.button(live)));
        }
        CHECK(mt_objc_undeliverable_count(e.rt) == 0u);
    }
    SUBCASE("without the local Game Center its button and image are hidden too") {
        Env e;
        REQUIRE(mt_objc_add_host_class(e.rt, &k_test_menu, 0, 0) == 0);
        mt_offline_ui_configure(MT_OFFLINE_HIDE_LINKS | MT_OFFLINE_HIDE_GAME_CENTER);
        CHECK(mt_offline_ui_install(e.rt) == 2u);
        Menu m(e);
        CHECK(m.hidden(m.ivar(152)));
        CHECK(m.hidden(m.ivar(156)));
        CHECK(m.hidden(m.button("clickGameCenter:")));
        CHECK(m.hidden(m.button("clickMoreGame:")));
        CHECK_FALSE(m.hidden(m.button("clickPlay:")));
    }
    SUBCASE("an image without MainMenuView: nothing to install") {
        Env e;
        mt_offline_ui_configure(MT_OFFLINE_HIDE_LINKS);
        CHECK(mt_offline_ui_install(e.rt) == 0u);
        bool logged = false;
        for (const std::string &l : g_log) {
            logged = logged || l.find("no MainMenuView") != std::string::npos;
        }
        CHECK(logged);
    }
    mt_offline_ui_configure(0);
}
