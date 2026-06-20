/**
 * @file       test_archive_handlers.cpp
 * @brief      Integration tests for archive admin API handlers
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "singleton/PluginManager.hpp"
#include "singleton/CServiceLocator.hpp"
#include "storage/RocksDBEngine.hpp"

// Include the plugin source to get ArchivePlugin class and handler implementations
#include "archive/archive_plugin.cpp"

namespace fs = std::filesystem;

// ============================================================================
// Test Fixture — creates a fresh RocksDB instance and initializes the plugin
// ============================================================================

class ArchiveHandlerTest : public ::testing::Test
{
protected:
    std::unique_ptr<RocksDBEngine> m_engine;
    PluginManager m_pm;
    CServiceLocator m_locator;
    fs::path m_tempPath;
    RequestContext m_ctx;

    void SetUp() override
    {
        auto base = fs::current_path();
        auto timestamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        m_tempPath = base / ("test_archive_handlers_" + std::to_string(timestamp));
        fs::create_directories(m_tempPath);

        auto result = RocksDBEngine::Create(m_tempPath.string());
        ASSERT_TRUE(result.has_value()) << "Failed to create engine: "
                                        << result.error().message();
        m_engine = std::move(result.value());

        m_locator.RegisterService(Fnv1a("PluginManager"), &m_pm);
        m_locator.RegisterService(Fnv1a("StorageEngine"), m_engine.get());

        // Create and initialize the archive plugin
        auto plugin = std::make_shared<ArchivePlugin>();
        m_pm.RegisterPlugin(plugin, 110,
            {"/api/v1/admin/archive/configure",
             "/api/v1/admin/archive/status",
             "/api/v1/admin/archive/pause",
             "/api/v1/admin/archive/resume",
             "/api/v1/admin/archive/test"});
        ASSERT_TRUE(plugin->Initialize(m_locator));
    }

    void TearDown() override
    {
        m_pm.ShutdownAll();
        m_engine.reset();
        std::error_code ec;
        fs::remove_all(m_tempPath, ec);
    }

    ///
    /// Dispatch a request through PluginManager::Route()
    ///
    std::string Route(const std::string& method,
                      const std::string& path,
                      const std::string& body = "")
    {
        return m_pm.Route(m_ctx, method, path, body);
    }
};

// ============================================================================
// configureArchive Tests
// ============================================================================

///
/// configureArchive with valid JSON stores config and returns success.
///
TEST_F(ArchiveHandlerTest, ConfigureArchive_ValidConfig_ReturnsSuccess)
{
    std::string body = R"({
        "endpoint": "https://s3.amazonaws.com",
        "bucket": "test-bucket",
        "access_key": "AKIAIOSFODNN7EXAMPLE",
        "secret_key": "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
    })";

    std::string result = Route("POST", "/api/v1/admin/archive/configure", body);

    EXPECT_NE(result.find("\"success\""), std::string::npos)
        << "Expected success response, got: " << result;
    EXPECT_NE(result.find("true"), std::string::npos)
        << "Expected success:true, got: " << result;
}

///
/// configureArchive with missing required fields returns INVALID_REQUEST error.
///
TEST_F(ArchiveHandlerTest, ConfigureArchive_MissingFields_ReturnsInvalidRequest)
{
    // Missing secret_key
    std::string body = R"({
        "endpoint": "https://s3.amazonaws.com",
        "bucket": "test-bucket",
        "access_key": "AKIAIOSFODNN7EXAMPLE"
    })";

    std::string result = Route("POST", "/api/v1/admin/archive/configure", body);

    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST error, got: " << result;
}

///
/// configureArchive with empty endpoint returns INVALID_CONFIG error.
///
TEST_F(ArchiveHandlerTest, ConfigureArchive_InvalidConfig_ReturnsInvalidConfig)
{
    std::string body = R"({
        "endpoint": "",
        "bucket": "test-bucket",
        "access_key": "AKIAIOSFODNN7EXAMPLE",
        "secret_key": "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
    })";

    std::string result = Route("POST", "/api/v1/admin/archive/configure", body);

    EXPECT_NE(result.find("INVALID_CONFIG"), std::string::npos)
        << "Expected INVALID_CONFIG error, got: " << result;
}

///
/// configureArchive with invalid JSON body returns PARSE_ERROR.
///
TEST_F(ArchiveHandlerTest, ConfigureArchive_InvalidJson_ReturnsParseError)
{
    std::string result = Route("POST", "/api/v1/admin/archive/configure", "not-json");

    EXPECT_NE(result.find("PARSE_ERROR"), std::string::npos)
        << "Expected PARSE_ERROR, got: " << result;
}

// ============================================================================
// archiveStatus Tests
// ============================================================================

///
/// archiveStatus returns configured:false when no config has been set.
///
TEST_F(ArchiveHandlerTest, ArchiveStatus_NotConfigured_ReturnsFalse)
{
    std::string result = Route("GET", "/api/v1/admin/archive/status");

    EXPECT_NE(result.find("\"configured\":false"), std::string::npos)
        << "Expected configured:false, got: " << result;
}

///
/// archiveStatus returns worker state after configuration.
///
TEST_F(ArchiveHandlerTest, ArchiveStatus_Configured_ReturnsWorkerState)
{
    // Configure first
    std::string configBody = R"({
        "endpoint": "https://s3.amazonaws.com",
        "bucket": "test-bucket",
        "access_key": "AKIAIOSFODNN7EXAMPLE",
        "secret_key": "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
    })";
    Route("POST", "/api/v1/admin/archive/configure", configBody);

    std::string result = Route("GET", "/api/v1/admin/archive/status");

    EXPECT_NE(result.find("\"configured\":true"), std::string::npos)
        << "Expected configured:true, got: " << result;
    EXPECT_NE(result.find("\"streaming\":true"), std::string::npos)
        << "Expected streaming:true, got: " << result;
    EXPECT_NE(result.find("queue_size"), std::string::npos)
        << "Expected queue_size field, got: " << result;
    EXPECT_NE(result.find("is_paused"), std::string::npos)
        << "Expected is_paused field, got: " << result;
    EXPECT_NE(result.find("is_connected"), std::string::npos)
        << "Expected is_connected field, got: " << result;
}

// ============================================================================
// archivePause / archiveResume Tests
// ============================================================================

///
/// archivePause returns NOT_CONFIGURED when no worker exists.
///
TEST_F(ArchiveHandlerTest, ArchivePause_NotConfigured_ReturnsError)
{
    std::string result = Route("POST", "/api/v1/admin/archive/pause");

    EXPECT_NE(result.find("NOT_CONFIGURED"), std::string::npos)
        << "Expected NOT_CONFIGURED error, got: " << result;
}

///
/// archivePause returns success after configuration.
///
TEST_F(ArchiveHandlerTest, ArchivePause_Configured_ReturnsSuccess)
{
    // Configure first
    std::string configBody = R"({
        "endpoint": "https://s3.amazonaws.com",
        "bucket": "test-bucket",
        "access_key": "AKIAIOSFODNN7EXAMPLE",
        "secret_key": "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
    })";
    Route("POST", "/api/v1/admin/archive/configure", configBody);

    std::string result = Route("POST", "/api/v1/admin/archive/pause");

    EXPECT_NE(result.find("\"success\""), std::string::npos)
        << "Expected success response, got: " << result;
    EXPECT_NE(result.find("true"), std::string::npos)
        << "Expected success:true, got: " << result;
}

///
/// archiveResume returns NOT_CONFIGURED when no worker exists.
///
TEST_F(ArchiveHandlerTest, ArchiveResume_NotConfigured_ReturnsError)
{
    std::string result = Route("POST", "/api/v1/admin/archive/resume");

    EXPECT_NE(result.find("NOT_CONFIGURED"), std::string::npos)
        << "Expected NOT_CONFIGURED error, got: " << result;
}

///
/// archiveResume returns success after configuration and pause.
///
TEST_F(ArchiveHandlerTest, ArchiveResume_AfterPause_ReturnsSuccess)
{
    // Configure first
    std::string configBody = R"({
        "endpoint": "https://s3.amazonaws.com",
        "bucket": "test-bucket",
        "access_key": "AKIAIOSFODNN7EXAMPLE",
        "secret_key": "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
    })";
    Route("POST", "/api/v1/admin/archive/configure", configBody);
    Route("POST", "/api/v1/admin/archive/pause");

    std::string result = Route("POST", "/api/v1/admin/archive/resume");

    EXPECT_NE(result.find("\"success\""), std::string::npos)
        << "Expected success response, got: " << result;
    EXPECT_NE(result.find("true"), std::string::npos)
        << "Expected success:true, got: " << result;
}

// ============================================================================
// archiveTestConnection Tests
// ============================================================================

///
/// archiveTestConnection with unreachable endpoint returns CONNECTION_FAILED.
///
TEST_F(ArchiveHandlerTest, ArchiveTestConnection_Unreachable_ReturnsConnectionFailed)
{
    std::string body = R"({
        "endpoint": "https://localhost:1",
        "bucket": "test-bucket",
        "access_key": "AKIAIOSFODNN7EXAMPLE",
        "secret_key": "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"
    })";

    std::string result = Route("POST", "/api/v1/admin/archive/test", body);

    EXPECT_NE(result.find("CONNECTION_FAILED"), std::string::npos)
        << "Expected CONNECTION_FAILED error, got: " << result;
}

///
/// archiveTestConnection with no body and no saved config returns error.
///
TEST_F(ArchiveHandlerTest, ArchiveTestConnection_NoConfig_ReturnsError)
{
    std::string result = Route("POST", "/api/v1/admin/archive/test", "");

    EXPECT_NE(result.find("error"), std::string::npos)
        << "Expected error response, got: " << result;
}

// ============================================================================
// Configuration Persistence Tests
// ============================================================================

///
/// Configuration persists to RocksDB and is loaded on restart.
///
TEST_F(ArchiveHandlerTest, ConfigPersistence_SurvivesRestart)
{
    // Configure the archive
    std::string configBody = R"({
        "endpoint": "https://s3.amazonaws.com",
        "bucket": "persist-test-bucket",
        "access_key": "AKIAIOSFODNN7EXAMPLE",
        "secret_key": "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY",
        "region": "eu-west-1",
        "prefix": "test-prefix"
    })";
    Route("POST", "/api/v1/admin/archive/configure", configBody);

    // Shutdown the plugin (saves config)
    m_pm.ShutdownAll();

    // Verify config is in RocksDB
    std::string storedConfig;
    ASSERT_TRUE(m_engine->Get("archive/config", storedConfig));
    EXPECT_NE(storedConfig.find("persist-test-bucket"), std::string::npos);
    EXPECT_NE(storedConfig.find("eu-west-1"), std::string::npos);

    // Create a new plugin instance (simulates server restart)
    auto newPlugin = std::make_shared<ArchivePlugin>();
    PluginManager newPm;
    CServiceLocator newLocator;
    newLocator.RegisterService(Fnv1a("PluginManager"), &newPm);
    newLocator.RegisterService(Fnv1a("StorageEngine"), m_engine.get());

    newPm.RegisterPlugin(newPlugin, 110,
        {"/api/v1/admin/archive/configure",
         "/api/v1/admin/archive/status",
         "/api/v1/admin/archive/pause",
         "/api/v1/admin/archive/resume",
         "/api/v1/admin/archive/test"});
    ASSERT_TRUE(newPlugin->Initialize(newLocator));

    // Status should show configured:true (auto-started from saved config)
    std::string statusResult = newPm.Route(m_ctx, "GET", "/api/v1/admin/archive/status", "");
    EXPECT_NE(statusResult.find("\"configured\":true"), std::string::npos)
        << "Expected auto-start after restart, got: " << statusResult;

    newPm.ShutdownAll();
}
