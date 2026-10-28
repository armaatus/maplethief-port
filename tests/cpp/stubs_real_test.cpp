// The offline service stubs (#52, ADR 0034) under the game's own recompiled code: the armv7
// slice is mapped, linked, placed and bound, the ObjC start-up runs (as in objc_real_test.cpp),
// and then the image's GameCenterManager and InAppPurchaseManager methods run against the
// GameKit and StoreKit host classes:
//
//     -[GameCenterManager init], -AuthenticationUser (its completion block sets
//     -[EAGLView setGameCenterLoginOKorError:YES]), -SubmitAchievement:PercentComplete: (load
//     block, NSMutableDictionary cache, report), -ReportScore:forCategory: (GKScore, int64)
//     -[InAppPurchaseManager init], -loadProductsInfo:, then on the pumps
//     -productsRequest:didReceiveResponse: -> addPayment: and
//     -paymentQueue:updatedTransactions: -> -failedTransaction: (getIsPurchaseSuccess 2)
//
// Completion handlers and StoreKit answers come from mt_stubs_pump, as the run loop (#54)
// will call it. gamecenter.json goes to a temporary data directory and is checked.
//
// GameCenterManager's completion blocks call -callDelegateOnMainThread:, which sends a block
// to the main queue with the runtime's dispatch_async (#50); the test drains it with
// mt_gcd_drain_main, as the run loop will, and -callDelegate:withArg:error: then asserts
// +[NSThread isMainThread] (a test override that counts the delegate calls). The Foundation
// classes these paths send to (NSString, NSArray, NSMutableArray, NSSet, NSMutableDictionary)
// are the runtime's own (#49), on CoreFoundation bound with mt_cf_start as the boot does.
// +[GameCenterManager IsGameCenterAvailable] (NSClassFromString, UIDevice) is left to the
// end-to-end run (#117).
//
// Needs work/slices/MapleThief.armv7 and the generated code; exits 77, which CTest reports as
// skipped, without the slice.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "foundation/foundation.h"
#include "guest_trap.h"
#include "maplethief/cf.h"
#include "maplethief/gcd.h"
#include "maplethief/hle/imports.h"
#include "maplethief/memory.h"
#include "maplethief/objc/host.h"
#include "maplethief/objc/objc.h"
#include "maplethief/recomp/dispatch.h"
#include "maplethief/sjlj.h"
#include "maplethief/stubs.h"

namespace {

using u32 = std::uint32_t;
namespace fs = std::filesystem;

int g_failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) {
        g_failures++;
    }
}

void print_log(void *, const char *line) { std::printf("     | %s\n", line); }

u32 rd32(const std::vector<std::uint8_t> &f, std::size_t at) {
    u32 v = 0;
    if (at + 4 <= f.size()) {
        std::memcpy(&v, f.data() + at, 4);
    }
    return v;
}

// (address, 32-bit entries) of a __DATA section of the thin 32-bit Mach-O.
void section(const std::vector<std::uint8_t> &f, const char *name, u32 *addr, u32 *count) {
    *addr = *count = 0;
    const u32 ncmds = rd32(f, 16);
    std::size_t at = 28;
    for (u32 i = 0; i < ncmds && at + 8 <= f.size(); i++) {
        const u32 cmd = rd32(f, at), size = rd32(f, at + 4);
        if (cmd == 1u && at + 56 <= f.size()) { // LC_SEGMENT
            const u32 nsects = rd32(f, at + 48);
            for (u32 s = 0; s < nsects && at + 56 + 68 * (s + 1) <= f.size(); s++) {
                const std::size_t sect = at + 56 + 68 * s;
                char sectname[17] = {}, segname[17] = {};
                std::memcpy(sectname, f.data() + sect, 16);
                std::memcpy(segname, f.data() + sect + 16, 16);
                if (std::strcmp(segname, "__DATA") == 0 && std::strcmp(sectname, name) == 0) {
                    *addr = rd32(f, sect + 32);
                    *count = rd32(f, sect + 36) / 4;
                }
            }
        }
        if (size < 8) {
            break;
        }
        at += size;
    }
}

// Installs fn as the method `sel` of the class `name` (mt_objc_override).
void method(mt_objc *rt, const char *cls, int class_method, const char *sel, mt_guest_fn fn) {
    if (mt_objc_override(rt, cls, class_method, sel, fn) != 0) {
        check(false,
              std::string("test method ") + (class_method ? "+[" : "-[") + cls + " " + sel + "]");
    }
}

// +[NSThread isMainThread]: -callDelegate:withArg:error: asserts it. Counts the delegate calls.
u32 g_delegate_calls = 0;
void thread_is_main(mt_cpu *cpu) {
    g_delegate_calls++;
    mt_ret_u32(cpu, 1);
}

struct Run {
    mt_cpu *cpu;
    mt_objc *rt;
};
Run *g_run = nullptr;

u32 msg(u32 obj, const char *sel, std::vector<u32> args = {}) {
    return mt_objc_msg(g_run->cpu, obj, mt_objc_sel(g_run->rt, sel), args.data(),
                       static_cast<u32>(args.size()));
}
u32 cls(const char *name) { return mt_objc_class_named(g_run->rt, name); }

std::string slurp(const fs::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
fs::path gamecenter_json() {
    char path[1100];
    return mt_stubs_gamecenter_path(path, sizeof path) == 0 ? fs::path(path) : fs::path();
}

constexpr const char *k_ranking = "com.nexonmobile.maplestory.thief.totalranking";

void run_game_center(void *) {
    Run *r = g_run;
    char line[200];
    const u32 gcm = msg(msg(cls("GameCenterManager"), "alloc"), "init");
    check(gcm != 0, "[[GameCenterManager alloc] init] (recompiled 0xC0345)");

    // The EAGLView whose -setGameCenterLoginOKorError: the authentication block calls, through
    // [[EAGLView_Instance Instance] objView]. Not initialized: only its ivar is used.
    const u32 view = msg(cls("EAGLView"), "alloc");
    msg(msg(cls("EAGLView_Instance"), "Instance"), "setObjView:", {view});
    check(msg(view, "getGameCenterLoginOKorError") == 0, "Game Center login flag starts NO");

    // -AuthenticationUser: [[GKLocalPlayer localPlayer] authenticateWithCompletionHandler:^...]
    msg(gcm, "AuthenticationUser");
    const u32 player = msg(cls("GKLocalPlayer"), "localPlayer");
    check(msg(player, "isAuthenticated") == 0 && mt_stubs_pending() == 1,
          "-AuthenticationUser asked GKLocalPlayer to authenticate; the answer is pending");
    u32 n = mt_stubs_pump(r->cpu);
    std::snprintf(line, sizeof line,
                  "pump: %u delivery; the recompiled block (0xC0419) set the login flag and "
                  "queued %u main-queue block(s)",
                  n, mt_gcd_main_pending());
    check(n == 1 && msg(player, "isAuthenticated") == 1 &&
              msg(view, "getGameCenterLoginOKorError") == 1 && mt_gcd_main_pending() == 1,
          line);
    n = mt_gcd_drain_main(r->cpu);
    check(
        n == 1 && g_delegate_calls == 1,
        "mt_gcd_drain_main ran -callDelegateOnMainThread:'s block -> -callDelegate:withArg:error:");

    // -SubmitAchievement:PercentComplete: (not sent by the shipped game, which reports none, but
    // the path GKTapper's code takes): no cache yet -> +loadAchievementsWithCompletionHandler:,
    // whose block (0xC05E5) fills the NSMutableDictionary cache and submits again -> new
    // GKAchievement, -reportAchievementWithCompletionHandler:.
    const u32 identifier = mt_ns::string_from_utf8(r->cpu, "thief.test.achievement");
    const double pct = 100.0;
    u32 bits[2];
    std::memcpy(bits, &pct, 8);
    msg(gcm, "SubmitAchievement:PercentComplete:", {identifier, bits[0], bits[1]});
    check(mt_stubs_pending() == 1, "-SubmitAchievement: loads the achievements first");
    n = mt_stubs_pump(r->cpu);
    const std::string after_load = slurp(gamecenter_json());
    check(n == 1 && msg(gcm, "MyAttainedAchievement") != 0 &&
              after_load.find("{\"identifier\": \"thief.test.achievement\", "
                              "\"percentComplete\": 100}") != std::string::npos,
          "the load block cached the achievements and the achievement was reported and saved "
          "to gamecenter.json");
    n = mt_stubs_pump(r->cpu);
    const u32 drained = mt_gcd_drain_main(r->cpu);
    check(n == 1 && drained == 1 && g_delegate_calls == 2,
          "the report's completion block ran, and its delegate call on the main queue");

    // -ReportScore:forCategory: from CMapleWorld::changeMap: a 64-bit value.
    const u32 category = mt_ns::string_from_utf8(r->cpu, k_ranking);
    const std::int64_t score = 123456789012LL;
    msg(gcm, "ReportScore:forCategory:",
        {static_cast<u32>(score), static_cast<u32>(score >> 32), category});
    const std::string after_score = slurp(gamecenter_json());
    check(after_score.find(std::string("{\"category\": \"") + k_ranking +
                           "\", \"best\": 123456789012, \"last\": 123456789012, "
                           "\"reports\": 1}") != std::string::npos,
          "-ReportScore:forCategory: (recompiled 0xBFE41) saved the 64-bit score");
    n = mt_stubs_pump(r->cpu);
    const u32 drained_score = mt_gcd_drain_main(r->cpu);
    check(n == 1 && drained_score == 1 && g_delegate_calls == 3,
          "the score's completion block ran, and its delegate call on the main queue");
    std::printf("     gamecenter.json:\n%s", after_score.c_str());
    msg(category, "release");
    msg(identifier, "release");
}

void run_in_app_purchase(void *) {
    Run *r = g_run;
    // -[EAGLView initWithCoder:]: +canMakePayments YES -> [[InAppPurchaseManager alloc] init],
    // which observes the default queue.
    check(msg(cls("SKPaymentQueue"), "canMakePayments") == 1, "+[SKPaymentQueue canMakePayments]");
    const u32 iap = msg(msg(cls("InAppPurchaseManager"), "alloc"), "init");
    check(iap != 0, "[[InAppPurchaseManager alloc] init] (recompiled 0xBF72D)");
    // CStateGame::OnKeyPressed: -loadProductsInfo:@"<product>" after addLoadingView.
    const u32 product =
        mt_ns::string_from_utf8(r->cpu, "com.nexonmobile.maplestory.thief.powpackage");
    msg(iap, "loadProductsInfo:", {product});
    check(mt_stubs_pending() == 1, "-loadProductsInfo: (0xBF695) started a products request");
    u32 n = mt_stubs_pump(r->cpu);
    const u32 queue = msg(cls("SKPaymentQueue"), "defaultQueue");
    check(n == 1 && msg(msg(queue, "transactions"), "count") == 1,
          "-productsRequest:didReceiveResponse: (0xBE559) bought the product: one transaction");
    check(msg(iap, "getIsPurchaseSuccess") != 2, "no purchase result yet");
    n = mt_stubs_pump(r->cpu);
    check(n == 1 && msg(iap, "getIsPurchaseSuccess") == 2,
          "-paymentQueue:updatedTransactions: (0xBF0D5) -> -failedTransaction: (0xBF025): "
          "getIsPurchaseSuccess 2, which CStateGame::Paint turns into removeLoadingView");
    check(msg(msg(queue, "transactions"), "count") == 0,
          "-failedTransaction: finished the transaction");
    msg(product, "release");
}

} // namespace

int main() {
    std::ifstream in(MT_ARMV7_SLICE, std::ios::binary);
    if (!in) {
        std::printf("skipped: %s is missing (run scripts/bootstrap)\n", MT_ARMV7_SLICE);
        return 77;
    }
    const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    mt_mem_config mcfg = mt_mem_config_default();
    mcfg.reservation = MT_MEM_RESERVE_FOR_BUILD;
    mt_mem *mem = mt_mem_create(&mcfg);
    if (mem == nullptr) {
        std::printf("FAIL cannot create the address space\n");
        return 1;
    }
    static mt_cpu cpu;
    mt_mem_attach(mem, &cpu);
    check(mt_mem_map_macho(mem, file.data(), file.size()) == 0, "the slice is mapped");

    mt_import_config cfg = mt_import_config_default(); // stop on anything unimplemented
    cfg.log = print_log;
    u32 n_hle = 0;
    const mt_hle_entry *hle = mt_hle_registry(&n_hle);
    mt_imports_link(mt_import_descs, mt_import_count, mt_import_table, hle, n_hle, &cfg);
    std::vector<u32> addrs(mt_import_count);
    check(mt_imports_place(&cpu, mt_import_descs, mt_import_count, hle, n_hle, mt_mem_guest_alloc,
                           mem, addrs.data()) == 0 &&
              mt_imports_apply_binds(&cpu, mt_import_binds, mt_import_bind_count, addrs.data(),
                                     mt_import_count) == 0,
          "the imports are placed and bound");
    mt_sjlj_state *sjlj = mt_sjlj_state_of(&cpu);
    mt_sjlj_init(sjlj);
    mt_sjlj_bind_personalities(sjlj, mt_import_descs, mt_import_count);

    mt_objc_image image{};
    section(file, "__objc_classlist", &image.classlist, &image.classlist_count);
    section(file, "__objc_catlist", &image.catlist, &image.catlist_count);
    section(file, "__objc_selrefs", &image.selrefs, &image.selrefs_count);
    mt_objc *rt = mt_objc_create(&cpu, mt_mem_guest_alloc, mem, &cfg);
    mt_objc_set_free(rt, mt_mem_guest_free);
    const u32 problems = mt_objc_start(rt, mt_import_descs, mt_import_count, addrs.data(), &image);
    mt_objc_set_current(rt);
    check(problems == 0, "the ObjC start-up has no problems");
    for (const char *name : {"GKLocalPlayer", "GKScore", "GKAchievement", "SKPaymentQueue",
                             "SKProductsRequest", "SKPayment"}) {
        const u32 c = mt_objc_class_named(rt, name);
        check(c != 0 && mt_objc_class_is_placeholder(rt, c) == 0,
              std::string(name) + " is a host class, not a placeholder");
    }

    mt_cf_start(&cpu, rt);
    method(rt, "NSThread", 1, "isMainThread", thread_is_main);

    const fs::path dir = fs::temp_directory_path() / "maplethief-stubs-real-test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    mt_stubs_set_data_dir(dir.string().c_str());

    mt_stack stack{};
    check(mt_stack_create(mem, MT_STACK_MAIN_SIZE, &stack) == 0, "a guest stack");
    cpu.r[MT_SP] = stack.top;
    cpu.r[MT_LR] = MT_HLE_RETURN;
    Run run{&cpu, rt};
    g_run = &run;
    const u32 pool = mt_objc_pool_push(rt);
    for (auto fn : {run_game_center, run_in_app_purchase}) {
        if (mt_test::stops(fn, nullptr)) {
            std::printf("FAIL the guest stopped: trap %u at 0x%08X (detail 0x%08X)\n",
                        mt_test::g_trap.kind, mt_test::g_trap.pc, mt_test::g_trap.detail);
            g_failures++;
        }
    }
    mt_objc_pool_pop(rt, pool);
    check(mt_objc_undeliverable_count(rt) == 0, "no undeliverable message");
    mt_stubs_reset();
    mt_stubs_set_data_dir(nullptr);
    fs::remove_all(dir, ec);
    mt_gcd_reset();
    mt_objc_destroy(rt);
    mt_mem_destroy(mem);
    std::printf("%s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
