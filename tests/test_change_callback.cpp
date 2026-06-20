/**
 * @file       test_change_callback.cpp
 * @brief      Unit tests for storage engine change callback mechanism
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "outcome/outcome.hpp"
#include "storage/RocksDBEngine.hpp"

namespace fs = std::filesystem;

// ============================================================================
// Callback invocation record
// ============================================================================

///
/// Records a single callback invocation for test verification.
///
struct CallbackRecord
{
    std::string key;        ///< The key that was modified
    std::string value;      ///< The value (empty for deletes)
    bool isDelete;          ///< true if this was a delete operation
};

// ============================================================================
// Test Fixture — creates a fresh RocksDB instance in a unique temp directory
// ============================================================================

class ChangeCallbackTest : public ::testing::Test
{
protected:
    std::unique_ptr<RocksDBEngine> m_engine;
    fs::path m_tempPath;

    void SetUp() override
    {
        auto base = fs::current_path();
        auto timestamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        m_tempPath = base / ("test_tmp_db_" + std::to_string(timestamp));
        fs::create_directories(m_tempPath);

        auto result = RocksDBEngine::Create(m_tempPath.string());
        ASSERT_TRUE(result.has_value()) << "Failed to create engine at: "
                                        << m_tempPath.string()
                                        << " error: "
                                        << result.error().message();
        m_engine = std::move(result.value());
    }

    void TearDown() override
    {
        m_engine.reset();
        std::error_code ec;
        fs::remove_all(m_tempPath, ec);
    }
};

// ============================================================================
// Single Callback Tests
// ============================================================================

///
/// Verify that a registered callback fires on Put with correct arguments.
///
TEST_F(ChangeCallbackTest, RegisterCallback_PutFiresCallback)
{
    std::vector<CallbackRecord> records;
    std::mutex recordsMutex;

    m_engine->RegisterChangeCallback(
        [&records, &recordsMutex](const std::string& key,
                                  const std::string& value,
                                  bool isDelete)
        {
            std::lock_guard<std::mutex> lock(recordsMutex);
            records.push_back({key, value, isDelete});
        });

    constexpr auto* kTestKey = "hrm/employees/aaaa0000000000000000000000000001";
    constexpr auto* kTestValue = R"({"name":"Alice"})";

    ASSERT_TRUE(m_engine->Put(kTestKey, kTestValue));

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, kTestKey);
    EXPECT_EQ(records[0].value, kTestValue);
    EXPECT_FALSE(records[0].isDelete);
}

///
/// Verify that a registered callback fires on Delete with isDelete=true
/// and an empty value.
///
TEST_F(ChangeCallbackTest, RegisterCallback_DeleteFiresCallback)
{
    std::vector<CallbackRecord> records;
    std::mutex recordsMutex;

    m_engine->RegisterChangeCallback(
        [&records, &recordsMutex](const std::string& key,
                                  const std::string& value,
                                  bool isDelete)
        {
            std::lock_guard<std::mutex> lock(recordsMutex);
            records.push_back({key, value, isDelete});
        });

    constexpr auto* kTestKey = "hrm/employees/bbbb0000000000000000000000000002";

    // Put first so Delete has something to remove
    ASSERT_TRUE(m_engine->Put(kTestKey, R"({"name":"Bob"})"));
    records.clear();

    ASSERT_TRUE(m_engine->Delete(kTestKey));

    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0].key, kTestKey);
    EXPECT_TRUE(records[0].value.empty());
    EXPECT_TRUE(records[0].isDelete);
}

// ============================================================================
// Multiple Callback Tests
// ============================================================================

///
/// Verify that multiple registered callbacks all fire on a single Put.
///
TEST_F(ChangeCallbackTest, RegisterCallback_MultipleCallbacks)
{
    std::vector<CallbackRecord> recordsA;
    std::vector<CallbackRecord> recordsB;
    std::mutex mutexA;
    std::mutex mutexB;

    m_engine->RegisterChangeCallback(
        [&recordsA, &mutexA](const std::string& key,
                             const std::string& value,
                             bool isDelete)
        {
            std::lock_guard<std::mutex> lock(mutexA);
            recordsA.push_back({key, value, isDelete});
        });

    m_engine->RegisterChangeCallback(
        [&recordsB, &mutexB](const std::string& key,
                             const std::string& value,
                             bool isDelete)
        {
            std::lock_guard<std::mutex> lock(mutexB);
            recordsB.push_back({key, value, isDelete});
        });

    constexpr auto* kTestKey = "hrm/employees/cccc0000000000000000000000000003";
    constexpr auto* kTestValue = R"({"name":"Charlie"})";

    ASSERT_TRUE(m_engine->Put(kTestKey, kTestValue));

    ASSERT_EQ(recordsA.size(), 1u);
    EXPECT_EQ(recordsA[0].key, kTestKey);
    EXPECT_EQ(recordsA[0].value, kTestValue);
    EXPECT_FALSE(recordsA[0].isDelete);

    ASSERT_EQ(recordsB.size(), 1u);
    EXPECT_EQ(recordsB[0].key, kTestKey);
    EXPECT_EQ(recordsB[0].value, kTestValue);
    EXPECT_FALSE(recordsB[0].isDelete);
}

// ============================================================================
// WriteBatch Tests
// ============================================================================

///
/// Verify that a callback fires once per operation in a WriteBatch
/// (2 puts + 1 delete = 3 callback invocations, in order).
///
TEST_F(ChangeCallbackTest, RegisterCallback_WriteBatch)
{
    std::vector<CallbackRecord> records;
    std::mutex recordsMutex;

    m_engine->RegisterChangeCallback(
        [&records, &recordsMutex](const std::string& key,
                                  const std::string& value,
                                  bool isDelete)
        {
            std::lock_guard<std::mutex> lock(recordsMutex);
            records.push_back({key, value, isDelete});
        });

    // Pre-populate a key that will be deleted
    constexpr auto* kDeleteKey = "hrm/employees/dddd0000000000000000000000000004";
    ASSERT_TRUE(m_engine->Put(kDeleteKey, R"({"name":"DeleteMe"})"));
    records.clear();

    constexpr auto* kPut1Key = "hrm/employees/eeee0000000000000000000000000005";
    constexpr auto* kPut1Value = R"({"name":"Eve"})";
    constexpr auto* kPut2Key = "hrm/employees/ffff0000000000000000000000000006";
    constexpr auto* kPut2Value = R"({"name":"Frank"})";

    std::vector<std::pair<std::string, std::string>> puts = {
        {kPut1Key, kPut1Value},
        {kPut2Key, kPut2Value}
    };
    std::vector<std::string> deletes = { kDeleteKey };

    ASSERT_TRUE(m_engine->WriteBatch(puts, deletes));

    // Should have 3 callback invocations: 2 puts then 1 delete
    ASSERT_EQ(records.size(), 3u);

    // First put
    EXPECT_EQ(records[0].key, kPut1Key);
    EXPECT_EQ(records[0].value, kPut1Value);
    EXPECT_FALSE(records[0].isDelete);

    // Second put
    EXPECT_EQ(records[1].key, kPut2Key);
    EXPECT_EQ(records[1].value, kPut2Value);
    EXPECT_FALSE(records[1].isDelete);

    // Delete
    EXPECT_EQ(records[2].key, kDeleteKey);
    EXPECT_TRUE(records[2].value.empty());
    EXPECT_TRUE(records[2].isDelete);
}

// ============================================================================
// No Callbacks Registered
// ============================================================================

///
/// Verify that Put and Delete succeed without error when no callbacks
/// are registered.
///
TEST_F(ChangeCallbackTest, RegisterCallback_NoCallbacksRegistered)
{
    constexpr auto* kTestKey = "hrm/employees/aaaa0000000000000000000000000007";
    constexpr auto* kTestValue = R"({"name":"Grace"})";

    // Should succeed without crash
    EXPECT_TRUE(m_engine->Put(kTestKey, kTestValue));
    EXPECT_TRUE(m_engine->Delete(kTestKey));

    // WriteBatch with no callbacks should also succeed
    std::vector<std::pair<std::string, std::string>> puts = {
        {kTestKey, kTestValue}
    };
    std::vector<std::string> deletes;
    EXPECT_TRUE(m_engine->WriteBatch(puts, deletes));
}

// ============================================================================
// Exception Safety
// ============================================================================

///
/// Verify that a callback that throws an exception does not crash the engine
/// and does not prevent subsequent callbacks from firing.
///
TEST_F(ChangeCallbackTest, RegisterCallback_CallbackExceptionDoesNotCrash)
{
    std::vector<CallbackRecord> goodRecords;
    std::mutex goodMutex;

    // First callback throws
    m_engine->RegisterChangeCallback(
        [](const std::string& /*key*/,
           const std::string& /*value*/,
           bool /*isDelete*/)
        {
            throw std::runtime_error("intentional test exception");
        });

    // Second callback should still fire
    m_engine->RegisterChangeCallback(
        [&goodRecords, &goodMutex](const std::string& key,
                                   const std::string& value,
                                   bool isDelete)
        {
            std::lock_guard<std::mutex> lock(goodMutex);
            goodRecords.push_back({key, value, isDelete});
        });

    constexpr auto* kTestKey = "hrm/employees/bbbb0000000000000000000000000008";
    constexpr auto* kTestValue = R"({"name":"Hank"})";

    // Should not crash despite the throwing callback
    ASSERT_TRUE(m_engine->Put(kTestKey, kTestValue));

    // The good callback should still have been called
    ASSERT_EQ(goodRecords.size(), 1u);
    EXPECT_EQ(goodRecords[0].key, kTestKey);
    EXPECT_EQ(goodRecords[0].value, kTestValue);
    EXPECT_FALSE(goodRecords[0].isDelete);
}
