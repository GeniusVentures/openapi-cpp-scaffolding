/**
 * @file       test_order_entry_store.cpp
 * @brief      Google Test for the generated OrderEntry composite-widget store
 * @date       2026-08-27
 * @author     Kenneth L. Hurley
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>

#include "commerce/generated/model/Order.h"
#include "OrderEntryOpenApiImporter.hpp"
#include "OrderEntryStore.hpp"

namespace
{

/// Upper bound for every wait-condition in this suite; a broken store fails the test, not the CI.
constexpr std::chrono::milliseconds kWaitTimeout {5000};

/// Invocations a single state mutation must produce.
constexpr unsigned int kSingleMutationInvocations = 1;

/// Invocations after the second mutation: the first subscription delivered the
/// first snapshot, then TWO live subscriptions each deliver the second (1 + 2).
constexpr unsigned int kSecondMutationInvocations = 3;

/// First subscription id handed out (mirrors kInitialSubscriptionId).
constexpr unsigned int kFirstSubscriptionId = 1;

/// Known order id used by the SetState/Snapshot happy path.
constexpr const char* const kSnapshotOrderId = "order-snapshot-42";

/// Replacement id arriving through the command envelope.
constexpr const char* const kReplacedOrderId = "order-replaced-99";

/// Id observed by the subscription recorder.
constexpr const char* const kNotifiedOrderId = "order-notified-7";

/// Valid three-letter currency satisfying Money::validate.
constexpr const char* const kValidCurrency = "USD";

/// Invalid currency (length 4) rejected by Money::validate.
constexpr const char* const kInvalidCurrency = "EURO";

/// Decimal amount paired with the currency fields above.
constexpr double kTotalAmount = 12.50;

/// Valid currency different from kValidCurrency (mismatch rejection).
constexpr const char* const kOtherValidCurrency = "EUR";

/// Canonical menu-item line unit price (Classic Burger, minor units).
constexpr int32_t kBurgerUnitAmount = 1250;

/// Canonical line quantity for the burger line.
constexpr int64_t kBurgerQuantity = 2;

/// Single-item quantity for the fries line in the derived-formula case.
constexpr int64_t kSingleQuantity = 1;

/// Extra-pickle modifier delta on the canonical burger line (minor units).
constexpr int32_t kPickleDeltaAmount = 50;

/// Doubled modifier delta used by the set_modifiers recompute case.
constexpr int32_t kDoublePickleDeltaAmount = 100;

/// Computed total of the canonical burger line: (1250 + 50) x 2.
constexpr int32_t kBurgerLineTotalAmount = 2600;

/// Recomputed burger total after the doubled delta: (1250 + 100) x 2.
constexpr int32_t kUpdatedBurgerLineTotalAmount = 2700;

/// Unit price of the cheap line used by the CR-04 negative-total case.
constexpr int32_t kCheapUnitAmount = 100;

/// Negative delta exceeding the cheap unit price (CR-04 parity).
constexpr int32_t kOversizedNegativeDelta = -200;

/// Unit price of the fries line in the derived-formula case.
constexpr int32_t kFriesUnitAmount = 800;

/// Unit price of the soda line in the derived-formula case.
constexpr int32_t kSodaUnitAmount = 500;

/// Quantity of the soda line in the derived-formula case.
constexpr int64_t kSodaQuantity = 3;

/// Unit price of the client-only custom line (excluded from derived money).
constexpr int32_t kCustomItemAmount = 999;

/// Expected derived subtotal of the formula case: 2500 + 800 + 1650.
constexpr int32_t kDerivedFormulaSubtotal = 4950;

/// Client-minted line ids used across the command-table cases.
constexpr const char* const kFirstLineId = "line-1";
constexpr const char* const kSecondLineId = "line-2";
constexpr const char* const kThirdLineId = "line-3";
constexpr const char* const kCustomLineId = "line-custom";

/// Menu item ids referenced by the command-table lines.
constexpr const char* const kBurgerProductId = "menu-burger";
constexpr const char* const kFriesProductId = "menu-fries";
constexpr const char* const kSodaProductId = "menu-soda";

/// Per-line send-state literals mirrored from the store's client extension.
constexpr const char* const kLineStateUnsent = "unsent";
constexpr const char* const kLineStateSending = "sending";
constexpr const char* const kLineStateSent = "sent";

/// Send-failure banner code carried by the failed end_send case.
constexpr const char* const kSendErrorTotalMismatch = "TOTAL_MISMATCH";

/// Hold mark applied by the successful end_send case.
constexpr const char* const kMarkHold = "hold";

/// API-sourced California tax rate in basis points (9.25 percent, Q1).
constexpr int64_t kCaliforniaTaxRateBps = 925;

/// Expected tax on subtotal 2600 at 925 bps (round half up): 241.
constexpr int32_t kExpectedTaxAmount = 241;

/// Expected total and balance_due with tax on subtotal 2600.
constexpr int32_t kExpectedTotalWithTax = 2841;

/// Out-of-range quantities flanking the store's [1, 99] bound.
constexpr int64_t kTooSmallQuantity = 0;
constexpr int64_t kTooLargeQuantity = 100;

/// Kitchen-note length exactly at the store's 140-character cap.
constexpr std::size_t kMaxLengthNote = 140;

/// Kitchen-note length one past the cap.
constexpr std::size_t kOverlongNoteLength = 141;

/// One four-byte astral emoji (U+1F600) — one CODE POINT, the unit the
/// note cap counts (Utf8CodePointCount), four UTF-8 bytes.
constexpr const char* kAstralEmoji = "\xF0\x9F\x98\x80";

/// Emoji count whose UTF-8 byte length (4x) far exceeds the cap while
/// the code-point count stays inside it.
constexpr std::size_t kEmojiNoteCount = 71;

/// Out-of-range guest counts flanking the store's [1, 20] bound.
constexpr int64_t kTooSmallGuestCount = 0;
constexpr int64_t kTooLargeGuestCount = 21;

/// In-range guest count accepted by set_guest_count.
constexpr int64_t kValidGuestCount = 2;

/// Out-of-range tax rates flanking the store's [0, 10000] bps bound.
constexpr int64_t kNegativeTaxRateBps = -1;
constexpr int64_t kTooLargeTaxRateBps = 10001;

/// Non-integral tax rate rejected by set_tax_rate.
constexpr double kFractionalTaxRateBps = 925.5;

///
/// Builds a minimal-but-valid Order wire payload: every field from_json
/// reads with j.at() (the required set) plus a Money total that passes
/// validate(). Optional fields are omitted.
///
nlohmann::json MakeValidOrderPayload(const std::string& id)
{
    return {
        {"id", id},
        {"tenant_id", "tenant-1"},
        {"organization_id", "org-1"},
        {"created_at", "2026-01-01T00:00:00Z"},
        {"updated_at", "2026-01-01T00:00:00Z"},
        {"status", "open"},
        {"channel", "pos"},
        {"fulfillment_type", "counter"},
        {"total", {{"amount", kTotalAmount}, {"currency", kValidCurrency}}}
    };
}

///
/// Builds a typed Order carrying only the given id. SetState performs no
/// validation (the importer seam contract), so a default-constructed Money
/// is acceptable for the typed-setter paths.
///
org::openapitools::server::model::Order MakeTypedOrder(const std::string& id)
{
    org::openapitools::server::model::Order order;
    order.setId(id);
    return order;
}

///
/// Builds a minimal-but-valid add-line envelope in the locked Phase-2
/// command-table shape (camelCase keys): every field the store's command
/// codec reads except the optional modifiers and lineTotal.
///
nlohmann::json MakeValidLinePayload(const std::string& id, const std::string& productId,
                                     int32_t unitAmount, int64_t quantity)
{
    nlohmann::json line;
    line["id"] = id;
    line["productId"] = productId;
    line["description"] = "Classic Burger";
    line["quantity"] = quantity;
    line["unitPrice"] = {{"amount", unitAmount}, {"currency", kValidCurrency}};
    return line;
}

///
/// Builds a one-modifier array whose priceDelta carries the given amount.
///
nlohmann::json MakeModifierArray(int32_t deltaAmount)
{
    nlohmann::json modifier;
    modifier["groupId"] = "group-pickles";
    modifier["modifierId"] = "modifier-extra-pickle";
    modifier["name"] = "Extra pickle";
    modifier["priceDelta"] = {{"amount", deltaAmount}, {"currency", kValidCurrency}};
    nlohmann::json modifiers = nlohmann::json::array();
    modifiers.push_back(modifier);
    return modifiers;
}

///
/// Builds an add_item / add_custom_item command envelope around a line.
///
nlohmann::json MakeAddLineCommand(const char* const kind, const nlohmann::json& line)
{
    return {{"kind", kind}, {"line", line}};
}

// ============================================================================
// Subscription recorder — mutex-guarded invocation log with a wait-condition
// ============================================================================

///
/// Records callback invocations and their last delivered order id. The
/// condition variable implements the project's wait-condition template: tests
/// block on a predicate (never on a sleep) to observe store pushes.
///
class SubscriptionRecorder
{
public:
    ///
    /// Callback body registered with Subscribe; safe to copy into std::function.
    ///
    void Record(const nlohmann::json& snapshot)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_invocationCount;
            if (snapshot.is_object() && snapshot.contains("id") && snapshot.at("id").is_string())
            {
                m_lastId = snapshot.at("id").get<std::string>();
            }
        }
        m_condition.notify_all();
    }

    ///
    /// Waits until at least the expected number of invocations has been recorded.
    ///
    /// @return bool true when the predicate was satisfied within kWaitTimeout.
    ///
    bool WaitForInvocations(unsigned int expectedCount)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_condition.wait_for(lock, kWaitTimeout, [this, expectedCount] {
            return m_invocationCount >= expectedCount;
        });
    }

    ///
    /// Returns the number of recorded invocations.
    ///
    unsigned int InvocationCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_invocationCount;
    }

    ///
    /// Returns the order id carried by the most recent snapshot.
    ///
    std::string LastId() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_lastId;
    }

private:
    mutable std::mutex m_mutex;                 ///< Guards the counters below.
    std::condition_variable m_condition;        ///< Signalled on every Record.
    unsigned int m_invocationCount = 0;         ///< Total callbacks delivered.
    std::string m_lastId;                       ///< Last delivered snapshot id.
};

// ============================================================================
// Snapshot / SetState
// ============================================================================

TEST(OrderEntryStoreTest, SnapshotReflectsSetState)
{
    genius::stores::OrderEntryStore store;
    store.SetState(MakeTypedOrder(kSnapshotOrderId));

    const nlohmann::json snapshot = store.Snapshot();
    ASSERT_TRUE(snapshot.is_object());
    ASSERT_TRUE(snapshot.contains("id"));
    EXPECT_EQ(kSnapshotOrderId, snapshot.at("id").get<std::string>());
}

TEST(OrderEntryStoreTest, SnapshotOfFreshStoreIsObject)
{
    genius::stores::OrderEntryStore store;
    EXPECT_TRUE(store.Snapshot().is_object());
}

// ============================================================================
// ApplyCommand
// ============================================================================

TEST(OrderEntryStoreTest, ApplyCommandReplaceCommitsValidState)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json command = {
        {"kind", "replace"},
        {"state", MakeValidOrderPayload(kReplacedOrderId)}
    };

    EXPECT_TRUE(store.ApplyCommand(command));
    EXPECT_EQ(kReplacedOrderId, store.Snapshot().at("id").get<std::string>());
}

TEST(OrderEntryStoreTest, ApplyCommandRejectsUnknownKind)
{
    genius::stores::OrderEntryStore store;
    store.SetState(MakeTypedOrder(kSnapshotOrderId));

    const nlohmann::json command = {{"kind", "unknown"}};
    EXPECT_FALSE(store.ApplyCommand(command));
    EXPECT_EQ(kSnapshotOrderId, store.Snapshot().at("id").get<std::string>());
}

TEST(OrderEntryStoreTest, ApplyCommandRejectsInvalidPayload)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json payload = MakeValidOrderPayload(kReplacedOrderId);
    payload["total"]["currency"] = kInvalidCurrency;
    const nlohmann::json command = {{"kind", "replace"}, {"state", payload}};

    EXPECT_FALSE(store.ApplyCommand(command));
}

TEST(OrderEntryStoreTest, ApplyCommandRejectsMissingRequiredField)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json payload = MakeValidOrderPayload(kReplacedOrderId);
    payload.erase("status");
    const nlohmann::json command = {{"kind", "replace"}, {"state", payload}};

    EXPECT_FALSE(store.ApplyCommand(command));
}

TEST(OrderEntryStoreTest, ApplyCommandCommitsBeforeSubscriberExceptionPropagates)
{
    genius::stores::OrderEntryStore store;
    SubscriptionRecorder recorder;
    store.Subscribe([&recorder](const nlohmann::json& snapshot)
    {
        recorder.Record(snapshot);
    });
    store.Subscribe([](const nlohmann::json&)
    {
        throw std::runtime_error("subscriber failure");
    });

    const nlohmann::json command = {
        {"kind", "replace"},
        {"state", MakeValidOrderPayload(kReplacedOrderId)}
    };

    // WR-02 corrected contract: the command committed before the second
    // subscriber threw, so the exception must propagate raw (it is not a
    // rejection) and the committed state must survive it.
    EXPECT_THROW(store.ApplyCommand(command), std::runtime_error);
    EXPECT_EQ(kReplacedOrderId, store.Snapshot().at("id").get<std::string>());
    EXPECT_EQ(kSingleMutationInvocations, recorder.InvocationCount());
}

// ============================================================================
// Subscribe / Unsubscribe
// ============================================================================

TEST(OrderEntryStoreTest, SubscribeDeliversEachSnapshotOnce)
{
    genius::stores::OrderEntryStore store;
    SubscriptionRecorder recorder;
    const unsigned int subscriptionId =
        store.Subscribe([&recorder](const nlohmann::json& snapshot)
        {
            recorder.Record(snapshot);
        });
    EXPECT_GE(subscriptionId, kFirstSubscriptionId);

    store.SetState(MakeTypedOrder(kNotifiedOrderId));
    ASSERT_TRUE(recorder.WaitForInvocations(kSingleMutationInvocations));
    EXPECT_EQ(kSingleMutationInvocations, recorder.InvocationCount());
    EXPECT_EQ(kNotifiedOrderId, recorder.LastId());

    const unsigned int secondSubscriptionId =
        store.Subscribe([&recorder](const nlohmann::json& snapshot)
        {
            recorder.Record(snapshot);
        });
    EXPECT_GT(secondSubscriptionId, subscriptionId);

    store.SetState(MakeTypedOrder(kSnapshotOrderId));
    ASSERT_TRUE(recorder.WaitForInvocations(kSecondMutationInvocations));
    EXPECT_EQ(kSecondMutationInvocations, recorder.InvocationCount());
    EXPECT_EQ(kSnapshotOrderId, recorder.LastId());
}

TEST(OrderEntryStoreTest, UnsubscribeStopsCallbacks)
{
    genius::stores::OrderEntryStore store;
    SubscriptionRecorder recorder;
    const unsigned int subscriptionId =
        store.Subscribe([&recorder](const nlohmann::json& snapshot)
        {
            recorder.Record(snapshot);
        });

    store.SetState(MakeTypedOrder(kNotifiedOrderId));
    ASSERT_TRUE(recorder.WaitForInvocations(kSingleMutationInvocations));

    store.Unsubscribe(subscriptionId);
    store.SetState(MakeTypedOrder(kSnapshotOrderId));

    // SetState dispatches synchronously: once it returns, no further callback
    // can arrive, so the count is deterministic without any sleep.
    EXPECT_EQ(kSingleMutationInvocations, recorder.InvocationCount());
    EXPECT_EQ(kNotifiedOrderId, recorder.LastId());
}

// ============================================================================
// Importer (D-04 seam)
// ============================================================================

TEST(OrderEntryImporterTest, ImportCommitsValidPayload)
{
    genius::stores::OrderEntryStore store;
    const genius::stores::OrderEntryOpenApiImporter importer;

    EXPECT_TRUE(importer.Import(store, MakeValidOrderPayload(kReplacedOrderId)));
    EXPECT_EQ(kReplacedOrderId, store.Snapshot().at("id").get<std::string>());
}

TEST(OrderEntryImporterTest, ImportRejectsInvalidPayloadWithoutCommitting)
{
    genius::stores::OrderEntryStore store;
    store.SetState(MakeTypedOrder(kSnapshotOrderId));
    const genius::stores::OrderEntryOpenApiImporter importer;

    nlohmann::json payload = MakeValidOrderPayload(kReplacedOrderId);
    payload["total"]["currency"] = kInvalidCurrency;

    EXPECT_FALSE(importer.Import(store, payload));
    EXPECT_EQ(kSnapshotOrderId, store.Snapshot().at("id").get<std::string>());
}

TEST(OrderEntryImporterTest, ImportRejectsMissingRequiredFieldWithoutCommitting)
{
    genius::stores::OrderEntryStore store;
    store.SetState(MakeTypedOrder(kSnapshotOrderId));
    const genius::stores::OrderEntryOpenApiImporter importer;

    nlohmann::json payload = MakeValidOrderPayload(kReplacedOrderId);
    payload.erase("status");

    EXPECT_FALSE(importer.Import(store, payload));
    EXPECT_EQ(kSnapshotOrderId, store.Snapshot().at("id").get<std::string>());
}

TEST(OrderEntryImporterTest, ImportCommitsBeforeSubscriberExceptionPropagates)
{
    genius::stores::OrderEntryStore store;
    SubscriptionRecorder recorder;
    store.Subscribe([&recorder](const nlohmann::json& snapshot)
    {
        recorder.Record(snapshot);
    });
    store.Subscribe([](const nlohmann::json&)
    {
        throw std::runtime_error("subscriber failure");
    });
    const genius::stores::OrderEntryOpenApiImporter importer;

    // PR #15 P2 parity with the WR-02 store contract: the import committed
    // before the second subscriber threw, so the exception must propagate
    // raw (a false rejection would claim nothing was committed) and the
    // committed state must survive it.
    EXPECT_THROW(importer.Import(store, MakeValidOrderPayload(kReplacedOrderId)), std::runtime_error);
    EXPECT_EQ(kReplacedOrderId, store.Snapshot().at("id").get<std::string>());
    EXPECT_EQ(kSingleMutationInvocations, recorder.InvocationCount());
}

// ============================================================================
// Phase-2 command table (add_item .. clear_send_error) -- additive coverage
// ============================================================================

TEST(OrderEntryStoreTest, AddItemComputesLineTotalLikeBackend)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    line["modifiers"] = MakeModifierArray(kPickleDeltaAmount);

    EXPECT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));

    const nlohmann::json snapshot = store.Snapshot();
    ASSERT_TRUE(snapshot.contains("lines"));
    ASSERT_EQ(1u, snapshot.at("lines").size());
    // (1250 + 50) x 2 == 2600: unitPrice x qty + deltaUnitSum x qty in int64.
    EXPECT_EQ(kBurgerLineTotalAmount,
              snapshot.at("lines").at(0).at("line_total").at("amount").get<int32_t>());
    EXPECT_EQ(kBurgerLineTotalAmount,
              snapshot.at("client").at("derived").at("subtotal").at("amount").get<int32_t>());
}

TEST(OrderEntryStoreTest, AddItemRejectsInconsistentDeclaredLineTotal)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    line["lineTotal"] = {{"amount", kBurgerLineTotalAmount + 1}, {"currency", kValidCurrency}};

    EXPECT_FALSE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));

    // Rejected before the commit window: nothing was mutated.
    const nlohmann::json snapshot = store.Snapshot();
    EXPECT_FALSE(snapshot.at("client").at("lines").contains(kFirstLineId));
    EXPECT_EQ(0, snapshot.at("client").at("derived").at("subtotal").at("amount").get<int32_t>());
}

TEST(OrderEntryStoreTest, AddItemRejectsCurrencyMismatch)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json first =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    EXPECT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", first)));

    nlohmann::json second =
        MakeValidLinePayload(kSecondLineId, kFriesProductId, kFriesUnitAmount, kBurgerQuantity);
    second["unitPrice"]["currency"] = kOtherValidCurrency;

    EXPECT_FALSE(store.ApplyCommand(MakeAddLineCommand("add_item", second)));
    EXPECT_FALSE(store.Snapshot().at("client").at("lines").contains(kSecondLineId));
}

TEST(OrderEntryStoreTest, AddItemRejectsDuplicateLineId)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));

    EXPECT_FALSE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));
    EXPECT_EQ(1u, store.Snapshot().at("lines").size());
}

TEST(OrderEntryStoreTest, AddItemRejectsNegativeComputedLineTotal)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kCheapUnitAmount, kBurgerQuantity);
    line["modifiers"] = MakeModifierArray(kOversizedNegativeDelta);

    // (100 - 200) x 2 == -200: CR-04 parity -- negative money never commits.
    EXPECT_FALSE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));

    const nlohmann::json snapshot = store.Snapshot();
    EXPECT_FALSE(snapshot.at("client").at("lines").contains(kFirstLineId));
    EXPECT_EQ(0, snapshot.at("client").at("derived").at("subtotal").at("amount").get<int32_t>());
}

TEST(OrderEntryStoreTest, AddCustomItemAcceptedAndMarkedCustomAndExcludedFromDerived)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json line = MakeValidLinePayload(kCustomLineId, "", kCustomItemAmount, kBurgerQuantity);

    EXPECT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_custom_item", line)));

    const nlohmann::json snapshot = store.Snapshot();
    const nlohmann::json& clientLines = snapshot.at("client").at("lines");
    ASSERT_TRUE(clientLines.contains(kCustomLineId));
    EXPECT_TRUE(clientLines.at(kCustomLineId).at("custom").get<bool>());
    EXPECT_EQ(kLineStateUnsent, clientLines.at(kCustomLineId).at("state").get<std::string>());
    // Custom lines are visible on the check but never server-billable (Q2).
    EXPECT_EQ(0, snapshot.at("client").at("derived").at("subtotal").at("amount").get<int32_t>());
}

TEST(OrderEntryStoreTest, SetQuantityRejectedOnSentLine)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));
    ASSERT_TRUE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kFirstLineId})}}));
    ASSERT_TRUE(store.ApplyCommand({{"kind", "end_send"},
                                    {"line_ids", nlohmann::json::array({kFirstLineId})},
                                    {"outcome", "sent"}}));

    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "set_quantity"}, {"line_id", kFirstLineId}, {"quantity", kSodaQuantity}}));
    EXPECT_EQ(kBurgerQuantity,
              store.Snapshot().at("lines").at(0).at("quantity").get<int64_t>());
}

TEST(OrderEntryStoreTest, SetQuantityRejectsOutOfRange)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));

    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "set_quantity"}, {"line_id", kFirstLineId}, {"quantity", kTooSmallQuantity}}));
    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "set_quantity"}, {"line_id", kFirstLineId}, {"quantity", kTooLargeQuantity}}));

    // Both rejections left the line untouched.
    EXPECT_EQ(kBurgerQuantity,
              store.Snapshot().at("lines").at(0).at("quantity").get<int64_t>());
}

TEST(OrderEntryStoreTest, SetNoteEnforcesMaxLength)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));

    const std::string overlongNote(kOverlongNoteLength, 'x');
    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "set_note"}, {"line_id", kFirstLineId}, {"note", overlongNote}}));

    const std::string maxLengthNote(kMaxLengthNote, 'x');
    EXPECT_TRUE(store.ApplyCommand(
        {{"kind", "set_note"}, {"line_id", kFirstLineId}, {"note", maxLengthNote}}));
    EXPECT_EQ(maxLengthNote,
              store.Snapshot().at("client").at("lines").at(kFirstLineId).at("note").get<std::string>());

    // Code points, not bytes: an astral emoji is ONE code point (four
    // UTF-8 bytes) — 71 fit the 140-character cap even though the old
    // byte count said 284 (codex PR review, P2; the Dart twin counts
    // runes — 142 UTF-16 units — for the same acceptance).
    std::string emojiNote;
    for (std::size_t i = 0; i < kEmojiNoteCount; ++i)
    {
        emojiNote += kAstralEmoji;
    }
    EXPECT_TRUE(store.ApplyCommand(
        {{"kind", "set_note"}, {"line_id", kFirstLineId}, {"note", emojiNote}}));
    EXPECT_EQ(emojiNote,
              store.Snapshot().at("client").at("lines").at(kFirstLineId).at("note").get<std::string>());

    std::string overlongEmojiNote;
    for (std::size_t i = 0; i < kOverlongNoteLength; ++i)
    {
        overlongEmojiNote += kAstralEmoji;
    }
    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "set_note"}, {"line_id", kFirstLineId}, {"note", overlongEmojiNote}}));
}

TEST(OrderEntryStoreTest, SetNoteRejectedOnSentLine)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));
    ASSERT_TRUE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kFirstLineId})}}));
    ASSERT_TRUE(store.ApplyCommand({{"kind", "end_send"},
                                    {"line_ids", nlohmann::json::array({kFirstLineId})},
                                    {"outcome", "sent"}}));

    const std::string note(kMaxLengthNote, 'x');
    EXPECT_FALSE(store.ApplyCommand({{"kind", "set_note"}, {"line_id", kFirstLineId}, {"note", note}}));
}

TEST(OrderEntryStoreTest, SetModifiersRecomputesLineTotal)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    line["modifiers"] = MakeModifierArray(kPickleDeltaAmount);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));
    ASSERT_EQ(kBurgerLineTotalAmount,
              store.Snapshot().at("client").at("derived").at("subtotal").at("amount").get<int32_t>());

    EXPECT_TRUE(store.ApplyCommand({{"kind", "set_modifiers"},
                                    {"line_id", kFirstLineId},
                                    {"modifiers", MakeModifierArray(kDoublePickleDeltaAmount)}}));

    const nlohmann::json snapshot = store.Snapshot();
    EXPECT_EQ(kUpdatedBurgerLineTotalAmount,
              snapshot.at("lines").at(0).at("line_total").at("amount").get<int32_t>());
    EXPECT_EQ(kUpdatedBurgerLineTotalAmount,
              snapshot.at("client").at("derived").at("subtotal").at("amount").get<int32_t>());
}

TEST(OrderEntryStoreTest, RemoveLineUnsentOnly)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json first =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    const nlohmann::json second =
        MakeValidLinePayload(kSecondLineId, kFriesProductId, kFriesUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", first)));
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", second)));

    // Unsent lines remove cleanly: typed line and client entry both drop.
    EXPECT_TRUE(store.ApplyCommand({{"kind", "remove_line"}, {"line_id", kFirstLineId}}));
    const nlohmann::json snapshot = store.Snapshot();
    EXPECT_EQ(1u, snapshot.at("lines").size());
    EXPECT_FALSE(snapshot.at("client").at("lines").contains(kFirstLineId));

    // Sent lines no longer remove.
    ASSERT_TRUE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kSecondLineId})}}));
    ASSERT_TRUE(store.ApplyCommand({{"kind", "end_send"},
                                    {"line_ids", nlohmann::json::array({kSecondLineId})},
                                    {"outcome", "sent"}}));
    EXPECT_FALSE(store.ApplyCommand({{"kind", "remove_line"}, {"line_id", kSecondLineId}}));
}

TEST(OrderEntryStoreTest, VoidAndCompLineMarkClientStateAndDropFromDerived)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json first =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    const nlohmann::json second =
        MakeValidLinePayload(kSecondLineId, kFriesProductId, kFriesUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", first)));
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", second)));

    EXPECT_TRUE(store.ApplyCommand({{"kind", "void_line"}, {"line_id", kFirstLineId}}));
    EXPECT_TRUE(store.ApplyCommand({{"kind", "comp_line"}, {"line_id", kSecondLineId}}));

    const nlohmann::json snapshot = store.Snapshot();
    const nlohmann::json& clientLines = snapshot.at("client").at("lines");
    EXPECT_TRUE(clientLines.at(kFirstLineId).at("voided").get<bool>());
    EXPECT_TRUE(clientLines.at(kSecondLineId).at("comped").get<bool>());
    // void_line / comp_line leave the send state untouched.
    EXPECT_EQ(kLineStateUnsent, clientLines.at(kFirstLineId).at("state").get<std::string>());
    // Both lines left the billable set: the derived subtotal is now zero.
    EXPECT_EQ(0, snapshot.at("client").at("derived").at("subtotal").at("amount").get<int32_t>());
}

TEST(OrderEntryStoreTest, BeginSendMarksSendingAndRejectsNonUnsentIds)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json first =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    const nlohmann::json second =
        MakeValidLinePayload(kSecondLineId, kFriesProductId, kFriesUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", first)));
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", second)));

    EXPECT_TRUE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kFirstLineId})}}));
    EXPECT_EQ(kLineStateSending,
              store.Snapshot().at("client").at("lines").at(kFirstLineId).at("state").get<std::string>());

    // An already-sending id rejects the batch...
    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kFirstLineId})}}));
    // ...and a mixed batch rejects WHOLE, leaving the unsent member untouched.
    EXPECT_FALSE(store.ApplyCommand({{"kind", "begin_send"},
                                     {"line_ids", nlohmann::json::array({kFirstLineId, kSecondLineId})}}));
    EXPECT_EQ(kLineStateUnsent,
              store.Snapshot().at("client").at("lines").at(kSecondLineId).at("state").get<std::string>());

    // Voided lines left service and never fire.
    ASSERT_TRUE(store.ApplyCommand({{"kind", "void_line"}, {"line_id", kSecondLineId}}));
    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kSecondLineId})}}));
}

TEST(OrderEntryStoreTest, EndSendFailedRevertsAndSetsSendError)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));
    ASSERT_TRUE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kFirstLineId})}}));
    ASSERT_TRUE(store.ApplyCommand({{"kind", "end_send"},
                                    {"outcome", "failed"},
                                    {"line_ids", nlohmann::json::array({kFirstLineId})},
                                    {"error", kSendErrorTotalMismatch}}));

    const nlohmann::json snapshot = store.Snapshot();
    const nlohmann::json& clientLine = snapshot.at("client").at("lines").at(kFirstLineId);
    EXPECT_EQ(kLineStateUnsent, clientLine.at("state").get<std::string>());
    EXPECT_TRUE(clientLine.at("mark").is_null());
    EXPECT_EQ(kSendErrorTotalMismatch,
              snapshot.at("client").at("send_error").get<std::string>());
}

TEST(OrderEntryStoreTest, EndSendSentClearsSendError)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));
    ASSERT_TRUE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kFirstLineId})}}));
    ASSERT_TRUE(store.ApplyCommand({{"kind", "end_send"},
                                    {"outcome", "failed"},
                                    {"line_ids", nlohmann::json::array({kFirstLineId})},
                                    {"error", kSendErrorTotalMismatch}}));
    ASSERT_EQ(kSendErrorTotalMismatch,
              store.Snapshot().at("client").at("send_error").get<std::string>());

    // Manual re-Send after the failure (D-07), then resolve with a mark.
    ASSERT_TRUE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kFirstLineId})}}));
    EXPECT_TRUE(store.ApplyCommand({{"kind", "end_send"},
                                    {"outcome", "sent"},
                                    {"mark", kMarkHold},
                                    {"line_ids", nlohmann::json::array({kFirstLineId})}}));

    const nlohmann::json snapshot = store.Snapshot();
    const nlohmann::json& clientLine = snapshot.at("client").at("lines").at(kFirstLineId);
    EXPECT_EQ(kLineStateSent, clientLine.at("state").get<std::string>());
    EXPECT_EQ(kMarkHold, clientLine.at("mark").get<std::string>());
    EXPECT_TRUE(snapshot.at("client").at("send_error").is_null());
}

TEST(OrderEntryStoreTest, ClearSendErrorNullsBanner)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));
    ASSERT_TRUE(store.ApplyCommand(
        {{"kind", "begin_send"}, {"line_ids", nlohmann::json::array({kFirstLineId})}}));
    ASSERT_TRUE(store.ApplyCommand({{"kind", "end_send"},
                                    {"outcome", "failed"},
                                    {"line_ids", nlohmann::json::array({kFirstLineId})},
                                    {"error", kSendErrorTotalMismatch}}));

    EXPECT_TRUE(store.ApplyCommand({{"kind", "clear_send_error"}}));
    EXPECT_TRUE(store.Snapshot().at("client").at("send_error").is_null());
}

TEST(OrderEntryStoreTest, SetGuestCountRejectsOutOfRange)
{
    genius::stores::OrderEntryStore store;

    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "set_guest_count"}, {"guests", kTooSmallGuestCount}}));
    EXPECT_FALSE(store.ApplyCommand(
        {{"kind", "set_guest_count"}, {"guests", kTooLargeGuestCount}}));
    // The fresh-check default guest count survives both rejections.
    EXPECT_EQ(1, store.Snapshot().at("client").at("guest_count").get<int64_t>());

    EXPECT_TRUE(store.ApplyCommand({{"kind", "set_guest_count"}, {"guests", kValidGuestCount}}));
    EXPECT_EQ(kValidGuestCount,
              store.Snapshot().at("client").at("guest_count").get<int64_t>());
}

TEST(OrderEntryStoreTest, SetTaxRateComputesTaxOnBillableSubtotal)
{
    genius::stores::OrderEntryStore store;
    nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    line["modifiers"] = MakeModifierArray(kPickleDeltaAmount);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));

    EXPECT_TRUE(store.ApplyCommand({{"kind", "set_tax_rate"}, {"bps", kCaliforniaTaxRateBps}}));

    const nlohmann::json snapshot = store.Snapshot();
    const nlohmann::json& derived = snapshot.at("client").at("derived");
    // (2600 x 925 + 5000) / 10000 == 241 -- integer math, round half up (Q1).
    EXPECT_EQ(kExpectedTaxAmount, derived.at("tax").at("amount").get<int32_t>());
    EXPECT_EQ(kExpectedTotalWithTax, derived.at("total").at("amount").get<int32_t>());
    EXPECT_EQ(kExpectedTotalWithTax, derived.at("balance_due").at("amount").get<int32_t>());
    EXPECT_EQ(kCaliforniaTaxRateBps,
              snapshot.at("client").at("tax_rate_bps").get<int64_t>());
}

TEST(OrderEntryStoreTest, SetTaxRateRejectsOutOfRange)
{
    genius::stores::OrderEntryStore store;

    EXPECT_FALSE(store.ApplyCommand({{"kind", "set_tax_rate"}, {"bps", kNegativeTaxRateBps}}));
    EXPECT_FALSE(store.ApplyCommand({{"kind", "set_tax_rate"}, {"bps", kTooLargeTaxRateBps}}));
    EXPECT_FALSE(store.ApplyCommand({{"kind", "set_tax_rate"}, {"bps", kFractionalTaxRateBps}}));

    // The fresh-check default rate (0, tax 0 until the API provides one) survives.
    const nlohmann::json snapshot = store.Snapshot();
    EXPECT_EQ(0, snapshot.at("client").at("tax_rate_bps").get<int64_t>());
    EXPECT_EQ(0, snapshot.at("client").at("derived").at("tax").at("amount").get<int32_t>());
}

TEST(OrderEntryStoreTest, DerivedTotalsMirrorBackendFormula)
{
    genius::stores::OrderEntryStore store;
    const nlohmann::json burger =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    const nlohmann::json fries =
        MakeValidLinePayload(kSecondLineId, kFriesProductId, kFriesUnitAmount, kSingleQuantity);
    nlohmann::json soda =
        MakeValidLinePayload(kThirdLineId, kSodaProductId, kSodaUnitAmount, kSodaQuantity);
    soda["modifiers"] = MakeModifierArray(kPickleDeltaAmount);
    const nlohmann::json custom = MakeValidLinePayload(kCustomLineId, "", kCustomItemAmount, kBurgerQuantity);

    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", burger)));
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", fries)));
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", soda)));
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_custom_item", custom)));

    // Billable subtotal: 2500 + 800 + 1650 -- the custom line never counts.
    const nlohmann::json snapshot = store.Snapshot();
    const nlohmann::json& derived = snapshot.at("client").at("derived");
    EXPECT_EQ(kDerivedFormulaSubtotal, derived.at("subtotal").at("amount").get<int32_t>());
    EXPECT_EQ(0, derived.at("tax").at("amount").get<int32_t>());
    EXPECT_EQ(0, derived.at("discount").at("amount").get<int32_t>());
    EXPECT_EQ(0, derived.at("service").at("amount").get<int32_t>());
    EXPECT_EQ(kDerivedFormulaSubtotal, derived.at("total").at("amount").get<int32_t>());
    EXPECT_EQ(kDerivedFormulaSubtotal, derived.at("balance_due").at("amount").get<int32_t>());
}

TEST(OrderEntryStoreTest, SnapshotIsAppendOnly)
{
    genius::stores::OrderEntryStore store;
    store.SetState(MakeTypedOrder(kSnapshotOrderId));
    const nlohmann::json line =
        MakeValidLinePayload(kFirstLineId, kBurgerProductId, kBurgerUnitAmount, kBurgerQuantity);
    ASSERT_TRUE(store.ApplyCommand(MakeAddLineCommand("add_item", line)));

    // Bare Order keys still resolve BESIDE the reserved client key (Pitfall 5).
    const nlohmann::json snapshot = store.Snapshot();
    ASSERT_TRUE(snapshot.is_object());
    EXPECT_TRUE(snapshot.contains("id"));
    EXPECT_EQ(kSnapshotOrderId, snapshot.at("id").get<std::string>());
    EXPECT_TRUE(snapshot.contains("status"));
    EXPECT_TRUE(snapshot.contains("total"));
    ASSERT_TRUE(snapshot.contains("client"));
    ASSERT_TRUE(snapshot.at("client").contains("derived"));

    // A replace round-trip of a snapshot-shaped payload (which now carries
    // the reserved client key) still parses and commits: the model codec
    // ignores unknown keys.
    nlohmann::json payload = MakeValidOrderPayload(kReplacedOrderId);
    payload["client"] = snapshot.at("client");
    const nlohmann::json replaceCommand = {{"kind", "replace"}, {"state", payload}};
    EXPECT_TRUE(store.ApplyCommand(replaceCommand));
    EXPECT_EQ(kReplacedOrderId, store.Snapshot().at("id").get<std::string>());
    EXPECT_TRUE(store.Snapshot().contains("client"));
}

}  // namespace
