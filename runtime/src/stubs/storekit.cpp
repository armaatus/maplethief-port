// StoreKit offline: a store that sells nothing (#52, ADR 0034), or what the offline cash shop
// sells for meso (#156, ADR 0072).
//
// The cash shop and the revival popup buy through InAppPurchaseManager
// (docs/re/objc-shell.md#inapppurchasemanager): the engine shows EAGLView's full-screen
// "Please Wait..." view, sets CDataMgr+0x534 (purchase pending) and sends
// -loadProductsInfo:, which starts an SKProductsRequest. Its delegate callback adds an
// SKPayment for the first product, and the queue's observer callback sends
// -failedTransaction: for a failed transaction, which makes -getIsPurchaseSuccess return 2;
// CStateGame::Paint then clears +0x534, removes the "Please Wait..." view and resets the
// purchase state. That is what iOS did when the player cancelled the purchase sheet, the
// best-trodden path of the game's purchase code, so the emulation answers every purchase so:
//
//   +canMakePayments       YES (with NO, InAppPurchaseManager is never created and a purchase
//                          leaves "Please Wait..." up for ever: CStateGame::OnKeyPressed sends
//                          -loadProductsInfo: to nil)
//   SKProductsRequest      answers on the next pump with one product per request
//   -addPayment:           the transaction fails on the next pump
//                          (SKPaymentTransactionStateFailed, as for SKErrorPaymentCancelled)
//
// No receipt is ever produced, so CMSocket (the receipt server, docs/re/network.md) is not
// reached from a purchase.
//
// With a payment policy (mt_storekit_set_policy: the offline cash shop, maplethief/cashshop.h)
// -addPayment: asks it about the payment's product. A payment it accepts completes on the next
// pump instead (SKPaymentTransactionStatePurchased) with a synthetic receipt, NSData of the
// NUL-terminated text "maplethief-offline 1 <product id>": -completeTransaction: keeps it, and
// CStateGame::Paint writes it to a receipt list and sends it to the receipt server, which
// network.cpp's fake server confirms (docs/re/cash-shop.md). A completed transaction is never
// delivered again (StoreKit would at the next launch, and the game would put it in
// totalrestore), so nothing of it outlives the session but the game's own receipt list.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "snapshot/snapshot_internal.h"
#include "stubs/stubs_internal.h"

namespace mt_stubs_impl {
namespace {

// SKPaymentTransactionState
constexpr std::int32_t k_purchasing = 0;
constexpr std::int32_t k_purchased = 1;
constexpr std::int32_t k_failed = 2;

// The payment policy (kept across runtimes and processes, like the hooks).
struct Policy {
    mt_storekit_policy_fn fn = nullptr;
    void *user = nullptr;
};
Policy g_policy;

struct StoreKit {
    u32 queue;          // the +defaultQueue singleton (kept for the runtime's lifetime)
    Words observers;    // not retained, as in StoreKit
    Words transactions; // unfinished transactions, retained
};
StoreKit g_sk;

struct RequestState {
    u32 identifiers; // retained NSSet
    u32 delegate;    // not retained (assign)
};
struct ResponseState {
    u32 products; // retained NSArray
};
struct ProductState {
    u32 identifiers; // retained NSSet of the request it answers
};
struct PaymentState {
    u32 product; // retained
};
struct TransactionState {
    u32 payment; // retained
    std::int32_t state;
    u32 receipt; // retained NSData (a completed purchase), or 0
};

void release_request(mt_cpu *cpu, u32, void *s) {
    mt_objc_release(cpu, static_cast<RequestState *>(s)->identifiers);
}
void release_response(mt_cpu *cpu, u32, void *s) {
    mt_objc_release(cpu, static_cast<ResponseState *>(s)->products);
}
void release_product(mt_cpu *cpu, u32, void *s) {
    mt_objc_release(cpu, static_cast<ProductState *>(s)->identifiers);
}
void release_payment(mt_cpu *cpu, u32, void *s) {
    mt_objc_release(cpu, static_cast<PaymentState *>(s)->product);
}
void release_transaction(mt_cpu *cpu, u32, void *s) {
    mt_objc_release(cpu, static_cast<TransactionState *>(s)->payment);
    mt_objc_release(cpu, static_cast<TransactionState *>(s)->receipt);
}

const mt_objc_state_type k_request_state = {"SKProductsRequest", sizeof(RequestState),
                                            release_request};
const mt_objc_state_type k_response_state = {"SKProductsResponse", sizeof(ResponseState),
                                             release_response};
const mt_objc_state_type k_product_state = {"SKProduct", sizeof(ProductState), release_product};
const mt_objc_state_type k_payment_state = {"SKPayment", sizeof(PaymentState), release_payment};
const mt_objc_state_type k_transaction_state = {"SKPaymentTransaction", sizeof(TransactionState),
                                                release_transaction};

template <typename T> T *find(u32 obj, const mt_objc_state_type *type) {
    return static_cast<T *>(mt_objc_find_state(runtime(), obj, type));
}
template <typename T> T *make(u32 obj, const mt_objc_state_type *type) {
    return obj != 0 ? static_cast<T *>(mt_objc_state(runtime(), obj, type)) : nullptr;
}

// A new instance of the host class `name` (retain count 1).
u32 create(const char *name) {
    mt_objc *rt = runtime();
    return mt_objc_create_instance(rt, mt_objc_class_named(rt, name));
}

// --- deliveries ------------------------------------------------------------------------------

// [delegate productsRequest:request didReceiveResponse:response] with one product: words =
// {request (retained at -start)}.
void deliver_products(mt_cpu *cpu, const u32 *w) {
    const u32 request = w[0];
    const RequestState *rs = find<RequestState>(request, &k_request_state);
    const u32 product = create("SKProduct");
    if (ProductState *ps = make<ProductState>(product, &k_product_state)) {
        ps->identifiers = mt_objc_retain(cpu, rs != nullptr ? rs->identifiers : 0u);
    }
    const u32 products =
        mt_objc_send1(cpu, mt_objc_class_named(runtime(), "NSArray"), "arrayWithObject:", product);
    mt_objc_release(cpu, product);
    const u32 response = create("SKProductsResponse");
    if (ResponseState *s = make<ResponseState>(response, &k_response_state)) {
        s->products = mt_objc_retain(cpu, products);
    }
    if (rs != nullptr && rs->delegate != 0) {
        mt_objc_send2(cpu, rs->delegate, "productsRequest:didReceiveResponse:", request, response);
    }
    mt_objc_release(cpu, response);
    mt_objc_release(cpu, request);
}

// The transaction's new state goes to the queue's observers: words = {queue, transaction
// (retained at -addPayment:), state}.
void deliver_outcome(mt_cpu *cpu, const u32 *w) {
    const u32 queue = w[0], transaction = w[1];
    const std::int32_t state = static_cast<std::int32_t>(w[2]);
    TransactionState *ts = find<TransactionState>(transaction, &k_transaction_state);
    if (ts != nullptr) {
        ts->state = state;
    }
    if (words_contains(g_sk.transactions, transaction)) {
        if (state == k_purchased) {
            log("StoreKit: the payment completes with a synthetic receipt (ADR 0072)");
        } else {
            log(g_policy.fn != nullptr
                    ? "StoreKit: the payment is cancelled: not sold or not paid (ADR 0072)"
                    : "StoreKit: the payment is cancelled: nothing is sold offline (ADR 0034)");
        }
        const u32 list = mt_objc_send1(cpu, mt_objc_class_named(runtime(), "NSArray"),
                                       "arrayWithObject:", transaction);
        // A copy: an observer may add or remove observers.
        Words observers{};
        for (u32 i = 0; i < g_sk.observers.count; i++) {
            words_push(observers, g_sk.observers.items[i]);
        }
        for (u32 i = 0; i < observers.count; i++) {
            mt_objc_send2(cpu, observers.items[i], "paymentQueue:updatedTransactions:", queue,
                          list);
        }
        words_free(observers);
    }
    mt_objc_release(cpu, transaction);
}

// --- SKPaymentQueue ------------------------------------------------------------------------

// +[SKPaymentQueue canMakePayments]: YES, so that InAppPurchaseManager exists and every
// purchase ends in the game's own "cancelled" path (ADR 0034).
void queue_can_make_payments(mt_cpu *cpu) { mt_ret_u32(cpu, 1u); }

// +[SKPaymentQueue defaultQueue]
void queue_default(mt_cpu *cpu) {
    mt_objc *rt = runtime();
    if (g_sk.queue == 0) {
        g_sk.queue = mt_objc_create_instance(rt, mt_objc_self(cpu));
    }
    mt_ret_u32(cpu, g_sk.queue);
}

void queue_add_observer(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 observer = mt_arg_u32(&a);
    runtime();
    if (observer != 0 && !words_contains(g_sk.observers, observer)) {
        words_push(g_sk.observers, observer);
    }
}

void queue_remove_observer(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 observer = mt_arg_u32(&a);
    runtime();
    words_remove(g_sk.observers, observer);
}

// The product id of a payment: payment -> its SKProduct -> the identifiers of the request it
// answers (one, -loadProductsInfo:'s). False with "" if there is none.
bool payment_product_id(mt_cpu *cpu, u32 payment, char *out, u32 size) {
    out[0] = '\0';
    const PaymentState *ps = find<PaymentState>(payment, &k_payment_state);
    const ProductState *pr =
        ps != nullptr ? find<ProductState>(ps->product, &k_product_state) : nullptr;
    if (pr == nullptr || pr->identifiers == 0) {
        return false;
    }
    const u32 all = mt_objc_send0(cpu, pr->identifiers, "allObjects");
    if (all == 0 || mt_objc_send0(cpu, all, "count") == 0) {
        return false;
    }
    return string_text(cpu, mt_objc_send1(cpu, all, "objectAtIndex:", 0u), out, size);
}

// -[SKPaymentQueue addPayment:]: a transaction that completes on the next pump if the policy
// accepts it, and fails as cancelled otherwise.
void queue_add_payment(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 payment = mt_arg_u32(&a);
    const u32 self = mt_objc_self(cpu);
    const u32 transaction = create("SKPaymentTransaction");
    TransactionState *ts = make<TransactionState>(transaction, &k_transaction_state);
    if (ts == nullptr || !words_push(g_sk.transactions, transaction)) {
        log("StoreKit: out of memory: a payment is dropped");
        mt_objc_release(cpu, transaction);
        return;
    }
    ts->payment = mt_objc_retain(cpu, payment);
    ts->state = k_purchasing;
    std::int32_t outcome = k_failed;
    char product[160];
    if (g_policy.fn != nullptr && payment_product_id(cpu, payment, product, sizeof product) &&
        g_policy.fn(g_policy.user, cpu, product) != 0) {
        char text[200];
        const u32 n = mt_stubs_receipt_text(product, text, sizeof text);
        // The NUL inside the data: the game takes the receipt's length with strlen.
        ts->receipt = n != 0 ? new_data(cpu, text, n + 1) : 0u;
        outcome = ts->receipt != 0 ? k_purchased : k_failed;
        if (ts->receipt == 0) {
            log("StoreKit: no receipt could be made for %s: the payment is cancelled", product);
        }
    }
    const u32 words[3] = {self, mt_objc_retain(cpu, transaction), static_cast<u32>(outcome)};
    if (!post(deliver_outcome, words, 3)) {
        mt_objc_release(cpu, transaction);
    }
}

// -[SKPaymentQueue finishTransaction:]
void queue_finish(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 transaction = mt_arg_u32(&a);
    runtime();
    if (words_remove(g_sk.transactions, transaction)) {
        mt_objc_release(cpu, transaction);
    }
}

// -[SKPaymentQueue transactions]: the unfinished ones, an autoreleased NSMutableArray.
void queue_transactions(mt_cpu *cpu) {
    mt_objc *rt = runtime();
    u32 list = mt_objc_send0(cpu, mt_objc_class_named(rt, "NSMutableArray"), "alloc");
    list = mt_objc_send1(cpu, list, "initWithCapacity:", g_sk.transactions.count);
    for (u32 i = 0; i < g_sk.transactions.count && list != 0; i++) {
        mt_objc_send1(cpu, list, "addObject:", g_sk.transactions.items[i]);
    }
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, list));
}

// --- SKProductsRequest, SKProductsResponse ----------------------------------------------------

// -[SKProductsRequest initWithProductIdentifiers:(NSSet *)]
void request_init(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 identifiers = mt_arg_u32(&a);
    const u32 self = mt_objc_self(cpu);
    if (RequestState *s = make<RequestState>(self, &k_request_state)) {
        mt_objc_retain(cpu, identifiers);
        mt_objc_release(cpu, s->identifiers);
        s->identifiers = identifiers;
    }
    mt_ret_u32(cpu, self);
}

void request_set_delegate(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 delegate = mt_arg_u32(&a);
    if (RequestState *s = make<RequestState>(mt_objc_self(cpu), &k_request_state)) {
        s->delegate = delegate;
    }
}

// -[SKProductsRequest start]: the request stays alive until its answer is delivered.
void request_start(mt_cpu *cpu) {
    const u32 self = mt_objc_self(cpu);
    runtime();
    mt_objc_retain(cpu, self);
    if (!post(deliver_products, &self, 1)) {
        mt_objc_release(cpu, self);
    }
}

void response_products(mt_cpu *cpu) {
    const ResponseState *s = find<ResponseState>(mt_objc_self(cpu), &k_response_state);
    mt_ret_u32(cpu, s != nullptr ? s->products : 0u);
}

// -[SKProductsResponse invalidProductIdentifiers]: none (nil answers -count with 0).
void response_invalid(mt_cpu *cpu) { mt_ret_u32(cpu, 0u); }

// --- SKPayment, SKPaymentTransaction ------------------------------------------------------------

// +[SKPayment paymentWithProduct:]: autoreleased.
void payment_with_product(mt_cpu *cpu) {
    mt_args a = mt_objc_args(cpu);
    const u32 product = mt_arg_u32(&a);
    const u32 payment = mt_objc_create_instance(runtime(), mt_objc_self(cpu));
    if (PaymentState *s = make<PaymentState>(payment, &k_payment_state)) {
        s->product = mt_objc_retain(cpu, product);
    }
    mt_ret_u32(cpu, mt_objc_autorelease(cpu, payment));
}

void transaction_state(mt_cpu *cpu) {
    const TransactionState *s = find<TransactionState>(mt_objc_self(cpu), &k_transaction_state);
    mt_ret_u32(cpu, static_cast<u32>(s != nullptr ? s->state : k_failed));
}

void transaction_payment(mt_cpu *cpu) {
    const TransactionState *s = find<TransactionState>(mt_objc_self(cpu), &k_transaction_state);
    mt_ret_u32(cpu, s != nullptr ? s->payment : 0u);
}

// -[SKPaymentTransaction transactionReceipt]: the synthetic receipt of a completed purchase,
// else nil (-[InAppPurchaseManager completeTransaction:] is its only sender).
void transaction_receipt(mt_cpu *cpu) {
    const TransactionState *s = find<TransactionState>(mt_objc_self(cpu), &k_transaction_state);
    mt_ret_u32(cpu, s != nullptr ? s->receipt : 0u);
}

const mt_objc_host_method k_queue_class_methods[] = {
    {"canMakePayments", queue_can_make_payments},
    {"defaultQueue", queue_default},
};
const mt_objc_host_method k_queue_methods[] = {
    {"addTransactionObserver:", queue_add_observer},
    {"removeTransactionObserver:", queue_remove_observer},
    {"addPayment:", queue_add_payment},
    {"finishTransaction:", queue_finish},
    {"transactions", queue_transactions},
};
const mt_objc_host_method k_request_methods[] = {
    {"initWithProductIdentifiers:", request_init},
    {"setDelegate:", request_set_delegate},
    {"start", request_start},
};
const mt_objc_host_method k_response_methods[] = {
    {"products", response_products},
    {"invalidProductIdentifiers", response_invalid},
};
const mt_objc_host_method k_payment_class_methods[] = {
    {"paymentWithProduct:", payment_with_product},
};
const mt_objc_host_method k_transaction_methods[] = {
    {"transactionState", transaction_state},
    {"payment", transaction_payment},
    {"transactionReceipt", transaction_receipt},
};

template <typename T, std::size_t N> constexpr u32 countof(const T (&)[N]) {
    return static_cast<u32>(N);
}

} // namespace

void storekit_reset() {
    words_free(g_sk.observers);
    words_free(g_sk.transactions);
    g_sk = StoreKit{};
}

// --- save states (#203, ADR 0105) ---------------------------------------------------------------
//
// The StoreKit objects only live in the cash shop (game mode 0xB, never a safe point), but their
// values are plain, so they have serializers; the module keeps the payment queue singleton, its
// observers and the unfinished transactions (guest objects as their addresses).

namespace {

template <typename T> bool save_words(const void *state, mt_snap::Writer &w) {
    u32 v[sizeof(T) / 4];
    std::memcpy(v, state, sizeof v);
    for (const u32 x : v) {
        w.u32(x);
    }
    return true;
}
template <typename T> bool load_words(void *state, mt_snap::Reader &r) {
    u32 v[sizeof(T) / 4];
    for (u32 &x : v) {
        if (!r.u32(&x)) {
            return false;
        }
    }
    std::memcpy(state, v, sizeof v);
    return true;
}
// Every StoreKit state is whole 32-bit words (guest objects, a state number).
static_assert(sizeof(RequestState) == 8 && sizeof(ResponseState) == 4 &&
              sizeof(ProductState) == 4 && sizeof(PaymentState) == 4 &&
              sizeof(TransactionState) == 12);

bool save_request(mt_cpu *, u32, const void *s, mt_snap::Writer &w) {
    return save_words<RequestState>(s, w);
}
bool load_request(mt_cpu *, u32, void *s, mt_snap::Reader &r) {
    return load_words<RequestState>(s, r);
}
bool save_response(mt_cpu *, u32, const void *s, mt_snap::Writer &w) {
    return save_words<ResponseState>(s, w);
}
bool load_response(mt_cpu *, u32, void *s, mt_snap::Reader &r) {
    return load_words<ResponseState>(s, r);
}
bool save_product(mt_cpu *, u32, const void *s, mt_snap::Writer &w) {
    return save_words<ProductState>(s, w);
}
bool load_product(mt_cpu *, u32, void *s, mt_snap::Reader &r) {
    return load_words<ProductState>(s, r);
}
bool save_payment(mt_cpu *, u32, const void *s, mt_snap::Writer &w) {
    return save_words<PaymentState>(s, w);
}
bool load_payment(mt_cpu *, u32, void *s, mt_snap::Reader &r) {
    return load_words<PaymentState>(s, r);
}
bool save_transaction(mt_cpu *, u32, const void *s, mt_snap::Writer &w) {
    return save_words<TransactionState>(s, w);
}
bool load_transaction(mt_cpu *, u32, void *s, mt_snap::Reader &r) {
    return load_words<TransactionState>(s, r);
}

void put_list(mt_snap::Writer &w, const Words &list) {
    w.words(std::vector<u32>(list.items, list.items + list.count));
}

bool save_sk_module(mt_cpu *, mt_snap::Writer &w, std::string *) {
    w.u32(g_sk.queue);
    put_list(w, g_sk.observers);
    put_list(w, g_sk.transactions);
    return true;
}
bool load_sk_module(mt_cpu *, mt_snap::Reader &r, std::function<void(mt_cpu *)> *apply) {
    u32 queue = 0;
    std::vector<u32> observers, transactions;
    if (!r.u32(&queue) || !r.words(&observers) || !r.words(&transactions)) {
        return false;
    }
    *apply = [queue, observers, transactions](mt_cpu *) {
        words_free(g_sk.observers);
        words_free(g_sk.transactions);
        g_sk = StoreKit{};
        g_sk.queue = queue;
        for (const u32 o : observers) {
            words_push(g_sk.observers, o);
        }
        for (const u32 t : transactions) {
            words_push(g_sk.transactions, t);
        }
    };
    return true;
}

} // namespace

extern const mt_snap::StateOps k_snap_sk_request, k_snap_sk_response, k_snap_sk_product,
    k_snap_sk_payment, k_snap_sk_transaction;
extern const mt_snap::Module k_snap_storekit;
const mt_snap::StateOps k_snap_sk_request = {
    &k_request_state, "SKProductsRequest", save_request, load_request, nullptr, nullptr};
const mt_snap::StateOps k_snap_sk_response = {
    &k_response_state, "SKProductsResponse", save_response, load_response, nullptr, nullptr};
const mt_snap::StateOps k_snap_sk_product = {&k_product_state, "SKProduct", save_product,
                                             load_product,     nullptr,     nullptr};
const mt_snap::StateOps k_snap_sk_payment = {&k_payment_state, "SKPayment", save_payment,
                                             load_payment,     nullptr,     nullptr};
const mt_snap::StateOps k_snap_sk_transaction = {&k_transaction_state,
                                                 "SKPaymentTransaction",
                                                 save_transaction,
                                                 load_transaction,
                                                 nullptr,
                                                 nullptr};
const mt_snap::Module k_snap_storekit = {"storekit", save_sk_module, load_sk_module};

const mt_objc_host_class k_sk_payment_queue = {"SKPaymentQueue",
                                               "NSObject",
                                               4,
                                               k_queue_methods,
                                               countof(k_queue_methods),
                                               k_queue_class_methods,
                                               countof(k_queue_class_methods)};
const mt_objc_host_class k_sk_products_request = {
    "SKProductsRequest", "NSObject", 4, k_request_methods, countof(k_request_methods), nullptr, 0};
const mt_objc_host_class k_sk_products_response = {
    "SKProductsResponse",        "NSObject", 4, k_response_methods,
    countof(k_response_methods), nullptr,    0};
const mt_objc_host_class k_sk_product = {"SKProduct", "NSObject", 4, nullptr, 0, nullptr, 0};
const mt_objc_host_class k_sk_payment = {"SKPayment",
                                         "NSObject",
                                         4,
                                         nullptr,
                                         0,
                                         k_payment_class_methods,
                                         countof(k_payment_class_methods)};
const mt_objc_host_class k_sk_payment_transaction = {
    "SKPaymentTransaction",         "NSObject", 4, k_transaction_methods,
    countof(k_transaction_methods), nullptr,    0};

} // namespace mt_stubs_impl

using namespace mt_stubs_impl;

extern "C" void mt_storekit_set_policy(mt_storekit_policy_fn fn, void *user) noexcept {
    g_policy.fn = fn;
    g_policy.user = fn != nullptr ? user : nullptr;
}

extern "C" std::uint32_t mt_stubs_receipt_text(const char *product_id, char *out,
                                               std::uint32_t size) noexcept {
    if (product_id == nullptr || out == nullptr || size == 0) {
        return 0;
    }
    const int n = std::snprintf(out, size, "%s%s", MT_STUBS_RECEIPT_PREFIX, product_id);
    if (n < 0 || static_cast<std::uint32_t>(n) >= size) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<std::uint32_t>(n);
}

extern "C" int mt_stubs_receipt_product(const std::uint8_t *bytes, std::uint32_t size, char *out,
                                        std::uint32_t out_size) noexcept {
    const std::uint32_t prefix = static_cast<std::uint32_t>(std::strlen(MT_STUBS_RECEIPT_PREFIX));
    if (out == nullptr || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (bytes == nullptr || size <= prefix ||
        std::memcmp(bytes, MT_STUBS_RECEIPT_PREFIX, prefix) != 0) {
        return 0;
    }
    std::uint32_t n = 0;
    for (std::uint32_t i = prefix; i < size && bytes[i] != 0 && bytes[i] != ' '; i++) {
        const std::uint8_t c = bytes[i];
        if (c < 0x21 || c > 0x7E || n + 1 >= out_size) {
            out[0] = '\0';
            return 0;
        }
        out[n++] = static_cast<char>(c);
    }
    out[n] = '\0';
    return n != 0 ? 1 : 0;
}
