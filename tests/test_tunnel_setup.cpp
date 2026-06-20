/**
 * @file       test_tunnel_setup.cpp
 * @brief      Tests for tunnel setup wizard handler — input validation and error responses
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

// Include the plugin source to get TunnelPlugin class and handler implementations
#include "tunnel/tunnel_plugin.cpp"

namespace fs = std::filesystem;

// ============================================================================
// Test Fixture — creates a fresh RocksDB instance and initializes the plugin
// ============================================================================

class TunnelSetupTest : public ::testing::Test
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
        m_tempPath = base / ("test_tunnel_setup_" + std::to_string(timestamp));
        fs::create_directories(m_tempPath);

        auto result = RocksDBEngine::Create(m_tempPath.string());
        ASSERT_TRUE(result.has_value()) << "Failed to create engine: "
                                        << result.error().message();
        m_engine = std::move(result.value());

        m_locator.RegisterService(Fnv1a("PluginManager"), &m_pm);
        m_locator.RegisterService(Fnv1a("StorageEngine"), m_engine.get());

        // Create and initialize the tunnel plugin
        auto plugin = std::make_shared<TunnelPlugin>();
        m_pm.RegisterPlugin(plugin, 120,
            {"/api/v1/admin/tunnel/configure",
             "/api/v1/admin/tunnel/status",
             "/api/v1/admin/tunnel/start",
             "/api/v1/admin/tunnel/stop",
             "/api/v1/admin/tunnel/restart",
             "/api/v1/admin/tunnel/logs",
             "/api/v1/admin/tunnel/setup"});
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
// Input Validation Tests — missing required fields
// ============================================================================

///
/// setupTunnel with missing api_token returns INVALID_REQUEST.
///
TEST_F(TunnelSetupTest, SetupTunnel_MissingApiToken_ReturnsInvalidRequest)
{
    std::string body = R"({
        "hostname": "api.genius.ai"
    })";

    std::string result = Route("POST", "/api/v1/admin/tunnel/setup", body);

    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST error, got: " << result;
}

///
/// setupTunnel with missing hostname returns INVALID_REQUEST.
///
TEST_F(TunnelSetupTest, SetupTunnel_MissingHostname_ReturnsInvalidRequest)
{
    std::string body = R"({
        "api_token": "v1.abc123"
    })";

    std::string result = Route("POST", "/api/v1/admin/tunnel/setup", body);

    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST error, got: " << result;
}

///
/// setupTunnel with empty api_token returns INVALID_REQUEST.
///
TEST_F(TunnelSetupTest, SetupTunnel_EmptyApiToken_ReturnsInvalidRequest)
{
    std::string body = R"({
        "api_token": "",
        "hostname": "api.genius.ai"
    })";

    std::string result = Route("POST", "/api/v1/admin/tunnel/setup", body);

    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST error, got: " << result;
}

///
/// setupTunnel with empty hostname returns INVALID_REQUEST.
///
TEST_F(TunnelSetupTest, SetupTunnel_EmptyHostname_ReturnsInvalidRequest)
{
    std::string body = R"({
        "api_token": "v1.abc123",
        "hostname": ""
    })";

    std::string result = Route("POST", "/api/v1/admin/tunnel/setup", body);

    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST error, got: " << result;
}

///
/// setupTunnel with invalid JSON body returns PARSE_ERROR.
///
TEST_F(TunnelSetupTest, SetupTunnel_InvalidJson_ReturnsParseError)
{
    std::string result = Route("POST", "/api/v1/admin/tunnel/setup", "not-json");

    EXPECT_NE(result.find("PARSE_ERROR"), std::string::npos)
        << "Expected PARSE_ERROR error, got: " << result;
}

///
/// setupTunnel with both required fields present does not return INVALID_REQUEST.
/// (May fail at Cloudflare API call, but input validation passes.)
///
TEST_F(TunnelSetupTest, SetupTunnel_ValidInput_PassesValidation)
{
    std::string body = R"({
        "api_token": "v1.invalid-but-structurally-valid",
        "hostname": "api.genius.ai",
        "tunnel_name": "test-tunnel"
    })";

    std::string result = Route("POST", "/api/v1/admin/tunnel/setup", body);

    // Should NOT be an input validation error — the error should come from
    // Cloudflare API call or binary discovery, not from input validation
    EXPECT_EQ(result.find("INVALID_REQUEST"), std::string::npos)
        << "Input validation should pass with valid fields, got: " << result;
    // Should have an error (since the token is invalid) but from a later step
    EXPECT_NE(result.find("error"), std::string::npos)
        << "Expected error from Cloudflare API or binary discovery, got: " << result;
}
