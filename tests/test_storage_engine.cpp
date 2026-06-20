/**
 * @file       test_storage_engine.cpp
 * @brief      Integration tests for RocksDBEngine and unit tests for KeyBuilder
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <rocksdb/db.h>

#include "outcome/outcome.hpp"
#include "storage/KeyBuilder.hpp"
#include "storage/RocksDBEngine.hpp"
#include "storage/StorageError.hpp"

namespace fs = std::filesystem;

// ============================================================================
// Test Fixture — creates a fresh RocksDB instance in a unique temp directory
// ============================================================================

class StorageEngineTest : public ::testing::Test
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
// KeyBuilder Unit Tests
// ============================================================================

TEST(KeyBuilderTest, Build_ProducesCorrectKey)
{
    auto result = KeyBuilder::Build(
        "hrm", "employees", "a1b2c3d4-e5f6-7890-abcd-ef1234567890");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "hrm/employees/a1b2c3d4e5f67890abcdef1234567890");
}

TEST(KeyBuilderTest, Build_StripsHyphens)
{
    auto result = KeyBuilder::Build(
        "commerce", "orders", "11112222-3333-4444-5555-666677778888");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "commerce/orders/11112222333344445555666677778888");
}

TEST(KeyBuilderTest, Build_RejectsEmptyDomain)
{
    auto result = KeyBuilder::Build("", "employees", "abc123");
    EXPECT_FALSE(result.has_value());
}

TEST(KeyBuilderTest, Build_RejectsTraversalInEntity)
{
    auto result = KeyBuilder::Build("hrm", "..", "abc123");
    EXPECT_FALSE(result.has_value());
}

TEST(KeyBuilderTest, Build_RejectsSlashInId)
{
    auto result = KeyBuilder::Build("hrm", "employees", "abc/123");
    EXPECT_FALSE(result.has_value());
}

TEST(KeyBuilderTest, Parse_ValidKey)
{
    auto result = KeyBuilder::Parse("hrm/employees/abc123");
    ASSERT_TRUE(result.has_value());

    const auto& parts = result.value();
    EXPECT_EQ(parts.domain, "hrm");
    EXPECT_EQ(parts.entity, "employees");
    EXPECT_EQ(parts.id, "abc123");
}

TEST(KeyBuilderTest, Parse_InvalidKey)
{
    // No slashes — invalid
    auto result = KeyBuilder::Parse("invalid");
    EXPECT_FALSE(result.has_value());
}

TEST(KeyBuilderTest, MakePrefix_DomainAndEntity)
{
    std::string prefix = KeyBuilder::MakePrefix("hrm", "employees");
    EXPECT_EQ(prefix, "hrm/employees/");
}

TEST(KeyBuilderTest, MakePrefix_DomainOnly)
{
    std::string prefix = KeyBuilder::MakePrefix("hrm");
    EXPECT_EQ(prefix, "hrm/");
}

// ============================================================================
// CRUD Integration Tests
// ============================================================================

TEST_F(StorageEngineTest, PutGet_RoundTrip)
{
    std::string key = "hrm/employees/a1b2c3d4e5f67890abcdef1234567890";
    std::string value = R"({"name":"Alice","role":"engineer"})";

    ASSERT_TRUE(m_engine->Put(key, value));

    std::string retrieved;
    ASSERT_TRUE(m_engine->Get(key, retrieved));
    EXPECT_EQ(retrieved, value);
}

TEST_F(StorageEngineTest, Get_NonExistentKeyReturnsFalse)
{
    std::string value;
    EXPECT_FALSE(m_engine->Get("hrm/employees/nonexistent", value));
}

TEST_F(StorageEngineTest, Delete_RemovesKey)
{
    std::string key = "hrm/employees/deadbeef000000000000000000000000";
    std::string value = R"({"name":"Bob"})";

    ASSERT_TRUE(m_engine->Put(key, value));
    ASSERT_TRUE(m_engine->Delete(key));

    std::string retrieved;
    EXPECT_FALSE(m_engine->Get(key, retrieved));
}

TEST_F(StorageEngineTest, Put_OverwritesExistingValue)
{
    std::string key = "hrm/employees/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    std::string value1 = R"({"version":1})";
    std::string value2 = R"({"version":2})";

    ASSERT_TRUE(m_engine->Put(key, value1));
    ASSERT_TRUE(m_engine->Put(key, value2));

    std::string retrieved;
    ASSERT_TRUE(m_engine->Get(key, retrieved));
    EXPECT_EQ(retrieved, value2);
}

// ============================================================================
// Prefix Scan Tests
// ============================================================================

TEST_F(StorageEngineTest, Scan_ReturnsMatchingKeys)
{
    std::string prefix = "hrm/employees/";

    ASSERT_TRUE(m_engine->Put(prefix + "aaaa0000000000000000000000000001", R"({"n":"A"})"));
    ASSERT_TRUE(m_engine->Put(prefix + "bbbb0000000000000000000000000002", R"({"n":"B"})"));
    ASSERT_TRUE(m_engine->Put(prefix + "cccc0000000000000000000000000003", R"({"n":"C"})"));

    auto results = m_engine->Scan(prefix);
    EXPECT_EQ(results.size(), 3u);
}

TEST_F(StorageEngineTest, Scan_ExcludesNonMatchingKeys)
{
    ASSERT_TRUE(m_engine->Put("hrm/employees/aaaa0000000000000000000000000001", R"({"n":"A"})"));
    ASSERT_TRUE(m_engine->Put("hrm/employees/bbbb0000000000000000000000000002", R"({"n":"B"})"));
    ASSERT_TRUE(m_engine->Put("commerce/orders/cccc0000000000000000000000000003", R"({"n":"C"})"));

    auto results = m_engine->Scan("hrm/employees/");
    EXPECT_EQ(results.size(), 2u);

    for ( const auto& pair : results )
    {
        EXPECT_TRUE(pair.first.find("hrm/employees/") == 0);
    }
}

TEST_F(StorageEngineTest, Scan_EmptyResultForNoMatch)
{
    auto results = m_engine->Scan("nonexistent/prefix/");
    EXPECT_TRUE(results.empty());
}

// ============================================================================
// Batch Write Tests
// ============================================================================

TEST_F(StorageEngineTest, WriteBatch_PutsMultipleKeys)
{
    std::vector<std::pair<std::string, std::string>> puts = {
        {"hrm/employees/aaaa0000000000000000000000000001", R"({"n":"A"})"},
        {"hrm/employees/bbbb0000000000000000000000000002", R"({"n":"B"})"},
        {"hrm/employees/cccc0000000000000000000000000003", R"({"n":"C"})"}
    };
    std::vector<std::string> deletes;

    ASSERT_TRUE(m_engine->WriteBatch(puts, deletes));

    for ( const auto& put : puts )
    {
        std::string value;
        ASSERT_TRUE(m_engine->Get(put.first, value));
        EXPECT_EQ(value, put.second);
    }
}

TEST_F(StorageEngineTest, WriteBatch_DeletesKeys)
{
    std::string key = "hrm/employees/deadbeef000000000000000000000000";
    ASSERT_TRUE(m_engine->Put(key, R"({"name":"DeleteMe"})"));

    std::vector<std::pair<std::string, std::string>> puts;
    std::vector<std::string> deletes = { key };

    ASSERT_TRUE(m_engine->WriteBatch(puts, deletes));

    std::string value;
    EXPECT_FALSE(m_engine->Get(key, value));
}

TEST_F(StorageEngineTest, WriteBatch_MixedPutAndDelete)
{
    std::string existingKey = "hrm/employees/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    ASSERT_TRUE(m_engine->Put(existingKey, R"({"old":true})"));

    std::string newKey = "hrm/employees/bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    std::vector<std::pair<std::string, std::string>> puts = {
        {newKey, R"({"new":true})"}
    };
    std::vector<std::string> deletes = { existingKey };

    ASSERT_TRUE(m_engine->WriteBatch(puts, deletes));

    std::string value;
    EXPECT_FALSE(m_engine->Get(existingKey, value));
    ASSERT_TRUE(m_engine->Get(newKey, value));
    EXPECT_EQ(value, R"({"new":true})");
}
