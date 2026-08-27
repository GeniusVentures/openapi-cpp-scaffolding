/**
 * @file       test_order_entry_store.cpp
 * @brief      Google Test for the generated OrderEntry composite-widget store
 * @date       2026-08-27
 * @author     Kenneth L. Hurley
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <chrono>
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

}  // namespace
