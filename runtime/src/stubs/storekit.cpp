// StoreKit offline: a store that sells nothing (#52, ADR 0034).
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
#include <cstdint>

#include "stubs/stubs_internal.h"

namespace mt_stubs_impl {
namespace {

// SKPaymentTransactionState
constexpr std::int32_t k_purchasing = 0;
constexpr std::int32_t k_failed = 2;

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

// The payment fails as cancelled: words = {queue, transaction (retained at -addPayment:)}.
void deliver_cancelled(mt_cpu *cpu, const u32 *w) {
    const u32 queue = w[0], transaction = w[1];
    if (TransactionState *ts = find<TransactionState>(transaction, &k_transaction_state)) {
        ts->state = k_failed;
    }
    if (words_contains(g_sk.transactions, transaction)) {
        log("StoreKit: the payment is cancelled: nothing is sold offline (ADR 0034)");
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

// -[SKPaymentQueue addPayment:]: a transaction that fails on the next pump.
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
    const u32 words[2] = {self, mt_objc_retain(cpu, transaction)};
    if (!post(deliver_cancelled, words, 2)) {
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

// -[SKPaymentTransaction transactionReceipt]: no purchase ever completes, so no receipt
// (-[InAppPurchaseManager completeTransaction:], its only sender, is not reached).
void transaction_receipt(mt_cpu *cpu) { mt_ret_u32(cpu, 0u); }

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
