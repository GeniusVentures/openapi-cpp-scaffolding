/**
 * @file       tunnel_plugin.cpp
 * @brief      Tunnel plugin — manages cloudflared lifecycle and Cloudflare API integration
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "singleton/IPlugin.hpp"
#include "singleton/PluginRegistration.hpp"
#include "singleton/PluginManager.hpp"
#include "singleton/IServiceLocator.hpp"
#include "singleton/fnv1a.hpp"
#include "storage/IStorageEngine.hpp"
#include "tunnel/CloudflareClient.hpp"
#include "tunnel/SubprocessManager.hpp"
#include "tunnel/BinaryDiscovery.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
using namespace gnus::hash;

/// Constants
inline constexpr unsigned int kTunnelPluginPriority = 120;  ///< Loads after archive (110)
inline constexpr size_t kMaxLogLines = 200;                 ///< Ring buffer size for log lines
inline constexpr unsigned int kDefaultStatusLogLines = 10;  ///< Default log lines in status response
inline constexpr unsigned int kSetupConnectMaxRetries = 10; ///< Max connection verification retries
inline constexpr unsigned int kSetupConnectRetryMs = 1000;  ///< Delay between connection retries (ms)
inline constexpr char kDefaultTunnelName[] = "genius-server"; ///< Default tunnel name
inline constexpr char kDefaultServiceUrl[] = "http://localhost:3000"; ///< Default service URL

// ============================================================================
// Tunnel configuration structure
// ============================================================================

///
/// Persisted tunnel plugin configuration stored in RocksDB.
///
struct TunnelPluginConfig
{
    std::string account_id;        ///< Cloudflare account ID
    std::string tunnel_id;         ///< Tunnel UUID
    std::string tunnel_name;       ///< Tunnel name
    std::string tunnel_token;      ///< Token for cloudflared authentication
    std::string api_token;         ///< Cloudflare API bearer token
    std::string hostname;          ///< Public hostname (e.g. "api.genius.ai")
    std::string service_url;       ///< Local service URL (e.g. "http://localhost:3000")
    std::string binary_path;       ///< cloudflared binary path (empty = auto-detect)
    bool auto_start = true;        ///< Auto-start cloudflared on server restart
    std::string log_level;         ///< cloudflared log level (info, debug, warn, error)
    std::string created_at;        ///< ISO 8601 creation timestamp

    ///
    /// Validate that the minimum required fields are present.
    /// @return true if account_id, tunnel_id, and tunnel_token are non-empty
    ///
    bool IsValid() const noexcept
    {
        return !account_id.empty() &&
               !tunnel_id.empty() &&
               !tunnel_token.empty();
    }
};

// ============================================================================
// File-scope globals (set during Initialize, used by handlers)
// ============================================================================

static IStorageEngine* g_storage = nullptr;                     ///< Storage engine (not owned)
static std::unique_ptr<SubprocessManager> g_subprocess;         ///< cloudflared process manager
static std::unique_ptr<CloudflareClient> g_client;              ///< Cloudflare API client
static TunnelPluginConfig g_config;                             ///< Current tunnel configuration
static std::vector<std::string> g_logLines;                     ///< Ring buffer of log lines
static std::mutex g_logMutex;                                   ///< Mutex for log line buffer

// ============================================================================
// Config Persistence Helpers
// ============================================================================

/**
 * @brief  Persist tunnel configuration to RocksDB under key "tunnel/config".
 * @param  storage  Storage engine instance
 * @param  config   Configuration to save
 * @return true on success, false on error
 */
static bool SaveConfig(IStorageEngine* storage, const TunnelPluginConfig& config) noexcept
{
    if (storage == nullptr)
    {
        return false;
    }

    try
    {
        json j;
        j["account_id"] = config.account_id;
        j["tunnel_id"] = config.tunnel_id;
        j["tunnel_name"] = config.tunnel_name;
        j["tunnel_token"] = config.tunnel_token;
        j["api_token"] = config.api_token;
        j["hostname"] = config.hostname;
        j["service_url"] = config.service_url;
        j["binary_path"] = config.binary_path;
        j["auto_start"] = config.auto_start;
        j["log_level"] = config.log_level;
        j["created_at"] = config.created_at;
        return storage->Put("tunnel/config", j.dump());
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("SaveConfig failed: {}", e.what());
        return false;
    }
}

/**
 * @brief  Load tunnel configuration from RocksDB key "tunnel/config".
 * @param  storage      Storage engine instance
 * @param  config_out   Output — populated with stored configuration
 * @return true if a valid config was loaded, false otherwise
 */
static bool LoadConfig(IStorageEngine* storage, TunnelPluginConfig& config_out) noexcept
{
    if (storage == nullptr)
    {
        return false;
    }

    std::string value;
    if (!storage->Get("tunnel/config", value))
    {
        return false;
    }

    try
    {
        json j = json::parse(value);
        config_out.account_id = j.value("account_id", "");
        config_out.tunnel_id = j.value("tunnel_id", "");
        config_out.tunnel_name = j.value("tunnel_name", "");
        config_out.tunnel_token = j.value("tunnel_token", "");
        config_out.api_token = j.value("api_token", "");
        config_out.hostname = j.value("hostname", "");
        config_out.service_url = j.value("service_url", "http://localhost:3000");
        config_out.binary_path = j.value("binary_path", "");
        config_out.auto_start = j.value("auto_start", true);
        config_out.log_level = j.value("log_level", "info");
        config_out.created_at = j.value("created_at", "");
        return config_out.IsValid();
    }
    catch (const json::parse_error& e)
    {
        SPDLOG_ERROR("LoadConfig parse error: {}", e.what());
        return false;
    }
}

// ============================================================================
// Admin API Handler Functions
// ============================================================================

/**
 * @brief  Configure tunnel — validate config, persist to RocksDB, optionally start cloudflared.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/tunnel/configure)
 * @param  body     JSON body with account_id, tunnel_id, tunnel_token, hostname, service_url, etc.
 * @return JSON success or error response
 */
static std::string configureTunnel(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& body)
{
    try
    {
        json requestData = json::parse(body);

        // Validate required fields (threat T-15-04)
        if (!requestData.contains("account_id") ||
            !requestData.contains("tunnel_id") ||
            !requestData.contains("tunnel_token"))
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"account_id, tunnel_id, and tunnel_token are required"}})";
        }

        std::string accountId = requestData["account_id"].get<std::string>();
        std::string tunnelId = requestData["tunnel_id"].get<std::string>();
        std::string tunnelToken = requestData["tunnel_token"].get<std::string>();

        // Reject empty values (threat T-15-04)
        if (accountId.empty() || tunnelId.empty() || tunnelToken.empty())
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"account_id, tunnel_id, and tunnel_token must be non-empty"}})";
        }

        // Build config from JSON
        TunnelPluginConfig config;
        config.account_id = accountId;
        config.tunnel_id = tunnelId;
        config.tunnel_token = tunnelToken;
        config.api_token = requestData.value("api_token", std::string(""));
        config.tunnel_name = requestData.value("tunnel_name", std::string(""));
        config.hostname = requestData.value("hostname", std::string(""));
        config.service_url = requestData.value("service_url", std::string("http://localhost:3000"));
        config.binary_path = requestData.value("binary_path", std::string(""));
        config.auto_start = requestData.value("auto_start", true);
        config.log_level = requestData.value("log_level", std::string("info"));
        config.created_at = requestData.value("created_at", std::string(""));

        // Persist config to RocksDB
        if (!SaveConfig(g_storage, config))
        {
            SPDLOG_ERROR("configureTunnel — failed to persist config to RocksDB");
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to persist configuration"}})";
        }

        g_config = config;

        // If auto_start and cloudflared not running, discover binary and spawn
        if (g_config.auto_start && g_subprocess && !g_subprocess->GetStatus().running)
        {
            std::string binaryPath = BinaryDiscovery::FindBinary(
                g_config.binary_path, "");
            if (binaryPath.empty())
            {
                SPDLOG_WARN("configureTunnel — cloudflared binary not found, "
                            "tunnel will not auto-start");
            }
            else
            {
                std::vector<std::string> args = {
                    "tunnel", "run", "--token", g_config.tunnel_token
                };
                if (g_subprocess->Spawn(binaryPath, args))
                {
                    SPDLOG_INFO("configureTunnel — cloudflared auto-started");
                }
                else
                {
                    SPDLOG_ERROR("configureTunnel — failed to auto-start cloudflared");
                }
            }
        }

        SPDLOG_INFO("Tunnel configured (account: {}, tunnel: {})",
                     g_config.account_id, g_config.tunnel_id);
        return R"({"success":true,"message":"Tunnel configured successfully"})";
    }
    catch (const json::parse_error&)
    {
        return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})";
    }
}

/**
 * @brief  Get tunnel status — configured state, process health, remote connection status.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (GET)
 * @param  urlPath  URL path (/api/v1/admin/tunnel/status)
 * @param  body     Empty
 * @return JSON object with tunnel status fields (threat T-15-05: no token exposure)
 */
static std::string tunnelStatus(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& /*body*/)
{
    json response;
    response["configured"] = g_config.IsValid();

    if (!g_config.IsValid())
    {
        return response.dump();
    }

    // Process status
    if (g_subprocess)
    {
        ProcessStatus ps = g_subprocess->GetStatus();
        response["running"] = ps.running;
        response["process_status"] = ps.running ? "running" :
                                     (ps.crashed ? "crashed" : "stopped");
        response["restart_count"] = ps.restartCount;
        response["last_error"] = ps.lastError;
    }
    else
    {
        response["running"] = false;
        response["process_status"] = "stopped";
        response["restart_count"] = 0;
    }

    // Remote connection status from Cloudflare API
    if (g_subprocess && g_subprocess->GetStatus().running &&
        !g_config.api_token.empty())
    {
        // Create a temporary client for status check
        CloudflareClient statusClient(g_config.api_token);
        if (statusClient.Connect())
        {
            std::string remoteStatus;
            int connectionCount = 0;
            if (statusClient.GetConnections(g_config.account_id,
                                            g_config.tunnel_id,
                                            remoteStatus,
                                            connectionCount))
            {
                response["remote_status"] = remoteStatus;
                response["connection_count"] = connectionCount;
            }
            else
            {
                response["remote_status"] = "unknown";
                response["connection_count"] = 0;
            }
        }
    }

    // Recent log lines
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        json logs = json::array();
        size_t start = (g_logLines.size() > kDefaultStatusLogLines) ?
                       g_logLines.size() - kDefaultStatusLogLines : 0;
        for (size_t i = start; i < g_logLines.size(); ++i)
        {
            logs.push_back(g_logLines[i]);
        }
        response["recent_logs"] = logs;
    }

    return response.dump();
}

/**
 * @brief  Start the cloudflared subprocess.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/tunnel/start)
 * @param  body     Empty
 * @return JSON success or error response
 */
static std::string startTunnel(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& /*body*/)
{
    if (!g_config.IsValid())
    {
        return R"({"error":{"code":"NOT_CONFIGURED","message":"Tunnel not configured — call /configure first"}})";
    }

    if (!g_subprocess)
    {
        return R"({"error":{"code":"INTERNAL_ERROR","message":"SubprocessManager not initialized"}})";
    }

    if (g_subprocess->GetStatus().running)
    {
        return R"({"error":{"code":"ALREADY_RUNNING","message":"cloudflared is already running"}})";
    }

    // Discover binary
    std::string binaryPath = BinaryDiscovery::FindBinary(g_config.binary_path, "");
    if (binaryPath.empty())
    {
        return R"({"error":{"code":"BINARY_NOT_FOUND","message":"cloudflared binary not found — configure binary_path or install cloudflared"}})";
    }

    // Build args and spawn (threat T-15-06: args constructed internally, no user-supplied raw command)
    std::vector<std::string> args = {
        "tunnel", "run", "--token", g_config.tunnel_token
    };

    if (g_subprocess->Spawn(binaryPath, args))
    {
        SPDLOG_INFO("startTunnel — cloudflared started");
        return R"({"success":true,"message":"cloudflared started"})";
    }

    return R"({"error":{"code":"SPAWN_FAILED","message":"Failed to start cloudflared"}})";
}

/**
 * @brief  Stop the cloudflared subprocess.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/tunnel/stop)
 * @param  body     Empty
 * @return JSON success or error response
 */
static std::string stopTunnel(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& /*body*/)
{
    if (!g_subprocess || !g_subprocess->GetStatus().running)
    {
        return R"({"error":{"code":"NOT_RUNNING","message":"cloudflared is not running"}})";
    }

    g_subprocess->Stop();
    SPDLOG_INFO("stopTunnel — cloudflared stopped");
    return R"({"success":true,"message":"cloudflared stopped"})";
}

/**
 * @brief  Restart the cloudflared subprocess.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/tunnel/restart)
 * @param  body     Empty
 * @return JSON success or error response
 */
static std::string restartTunnel(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& /*body*/)
{
    if (!g_config.IsValid())
    {
        return R"({"error":{"code":"NOT_CONFIGURED","message":"Tunnel not configured"}})";
    }

    if (!g_subprocess)
    {
        return R"({"error":{"code":"INTERNAL_ERROR","message":"SubprocessManager not initialized"}})";
    }

    if (!g_subprocess->GetStatus().running)
    {
        // Not running — just start
        return startTunnel({}, "", "", "");
    }

    g_subprocess->Restart();
    SPDLOG_INFO("restartTunnel — cloudflared restarted");
    return R"({"success":true,"message":"cloudflared restarted"})";
}

/**
 * @brief  Get recent cloudflared log lines.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (GET)
 * @param  urlPath  URL path (/api/v1/admin/tunnel/logs?limit=N)
 * @param  body     Empty
 * @return JSON object with log lines array
 */
static std::string tunnelLogs(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& urlPath,
    const std::string& /*body*/)
{
    // Parse optional ?limit=N query parameter
    unsigned int limit = kDefaultStatusLogLines;
    auto queryPos = urlPath.find("?limit=");
    if (queryPos != std::string::npos)
    {
        try
        {
            limit = static_cast<unsigned int>(
                std::stoul(urlPath.substr(queryPos + 7)));
        }
        catch (...)
        {
            // Keep default on parse failure
        }
    }

    std::lock_guard<std::mutex> lock(g_logMutex);

    json response;
    json logs = json::array();
    size_t start = (g_logLines.size() > limit) ?
                   g_logLines.size() - limit : 0;
    for (size_t i = start; i < g_logLines.size(); ++i)
    {
        logs.push_back(g_logLines[i]);
    }
    response["logs"] = logs;
    response["total_lines"] = g_logLines.size();

    return response.dump();
}

// ============================================================================
// Setup Wizard Handler
// ============================================================================

/**
 * @brief  Strip a URL scheme prefix (http:// or https://) from a service URL.
 * @param  url  Input URL string
 * @return URL without scheme prefix
 */
static std::string StripScheme(const std::string& url) noexcept
{
    constexpr char kHttpPrefix[] = "http://";
    constexpr char kHttpsPrefix[] = "https://";

    if (url.compare(0, sizeof(kHttpPrefix) - 1, kHttpPrefix) == 0)
    {
        return url.substr(sizeof(kHttpPrefix) - 1);
    }
    if (url.compare(0, sizeof(kHttpsPrefix) - 1, kHttpsPrefix) == 0)
    {
        return url.substr(sizeof(kHttpsPrefix) - 1);
    }
    return url;
}

/**
 * @brief  Setup wizard — end-to-end tunnel provisioning in a single API call.
 *
 * Validates a Cloudflare API token, creates a tunnel, configures ingress rules,
 * optionally creates a DNS CNAME record, retrieves the tunnel token, discovers
 * and starts cloudflared, verifies the connection, and persists config to RocksDB.
 *
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/tunnel/setup)
 * @param  body     JSON body with api_token, hostname, and optional fields
 * @return JSON success or error response with step-specific error code
 */
static std::string setupTunnel(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& body)
{
    try
    {
        // =====================================================================
        // Step 1: Parse and validate input
        // =====================================================================
        json requestData = json::parse(body);

        if (!requestData.contains("api_token") || !requestData.contains("hostname"))
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"api_token and hostname are required"}})";
        }

        std::string apiToken = requestData["api_token"].get<std::string>();
        std::string hostname = requestData["hostname"].get<std::string>();

        if (apiToken.empty() || hostname.empty())
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"api_token and hostname are required"}})";
        }

        std::string tunnelName = requestData.value("tunnel_name", std::string(kDefaultTunnelName));
        std::string serviceUrl = requestData.value("service_url", std::string(kDefaultServiceUrl));
        std::string zoneId = requestData.value("zone_id", std::string(""));
        std::string providedAccountId = requestData.value("account_id", std::string(""));

        SPDLOG_INFO("setupTunnel — starting setup for hostname: {}", hostname);

        // =====================================================================
        // Step 2: Create temporary CloudflareClient
        // =====================================================================
        CloudflareClient tempClient(apiToken);
        if (!tempClient.Connect())
        {
            SPDLOG_ERROR("setupTunnel — failed to connect to Cloudflare API");
            return R"({"error":{"code":"CONNECTION_FAILED","message":"Failed to connect to Cloudflare API"}})";
        }

        // =====================================================================
        // Step 3: Validate API token
        // =====================================================================
        std::string accountIdFromToken;
        if (!tempClient.VerifyToken(accountIdFromToken))
        {
            SPDLOG_ERROR("setupTunnel — token verification failed");
            return R"({"error":{"code":"INVALID_TOKEN","message":"Cloudflare API token is invalid or expired"}})";
        }

        SPDLOG_DEBUG("setupTunnel — token verified, account: {}", accountIdFromToken);

        // =====================================================================
        // Step 4: Resolve account_id
        // =====================================================================
        std::string accountId;
        if (!providedAccountId.empty())
        {
            accountId = providedAccountId;
            SPDLOG_DEBUG("setupTunnel — using provided account_id: {}", accountId);
        }
        else
        {
            std::vector<std::pair<std::string, std::string>> accounts;
            if (!tempClient.ListAccounts(accounts) || accounts.empty())
            {
                SPDLOG_ERROR("setupTunnel — no accounts found for token");
                return R"({"error":{"code":"NO_ACCOUNTS","message":"No Cloudflare accounts found for this token"}})";
            }
            accountId = accounts.front().first;
            SPDLOG_DEBUG("setupTunnel — auto-detected account_id: {}", accountId);
        }

        // =====================================================================
        // Step 5: Create tunnel
        // =====================================================================
        TunnelInfo tunnelInfo;
        if (!tempClient.CreateTunnel(accountId, tunnelName, tunnelInfo))
        {
            SPDLOG_ERROR("setupTunnel — CreateTunnel failed: {}", tempClient.GetLastError());
            return R"({"error":{"code":"TUNNEL_CREATE_FAILED","message":"Failed to create tunnel: )"
                   + tempClient.GetLastError() + R"("}})";
        }

        SPDLOG_INFO("setupTunnel — tunnel created: {} ({})", tunnelInfo.tunnel_name,
                     tunnelInfo.tunnel_id);

        // =====================================================================
        // Step 6: Configure ingress rules
        // =====================================================================
        TunnelConfig config;
        IngressRule mainRule;
        mainRule.hostname = hostname;
        mainRule.service = "http://" + StripScheme(serviceUrl);
        config.ingress.push_back(mainRule);

        IngressRule catchAll;
        catchAll.hostname = "";
        catchAll.service = "http_status:404";
        config.ingress.push_back(catchAll);

        if (!tempClient.SetTunnelConfig(accountId, tunnelInfo.tunnel_id, config))
        {
            SPDLOG_ERROR("setupTunnel — SetTunnelConfig failed: {}", tempClient.GetLastError());
            return R"({"error":{"code":"INGRESS_CONFIG_FAILED","message":"Failed to configure tunnel ingress: )"
                   + tempClient.GetLastError() + R"("}})";
        }

        SPDLOG_DEBUG("setupTunnel — ingress configured: {} -> {}", hostname, mainRule.service);

        // =====================================================================
        // Step 7: DNS CNAME setup (optional — only if zone_id provided)
        // =====================================================================
        if (!zoneId.empty())
        {
            if (!tempClient.CreateDnsRecord(zoneId, hostname, tunnelInfo.tunnel_id))
            {
                // DNS failure is non-fatal — log warning and continue
                SPDLOG_WARN("setupTunnel — DNS CNAME creation failed (non-fatal): {}",
                            tempClient.GetLastError());
            }
            else
            {
                SPDLOG_INFO("setupTunnel — DNS CNAME created for {}", hostname);
            }
        }
        else
        {
            SPDLOG_DEBUG("setupTunnel — zone_id not provided, skipping DNS setup");
        }

        // =====================================================================
        // Step 8: Get tunnel token
        // =====================================================================
        std::string tunnelToken;
        if (!tempClient.GetTunnelToken(accountId, tunnelInfo.tunnel_id, tunnelToken))
        {
            SPDLOG_ERROR("setupTunnel — GetTunnelToken failed: {}", tempClient.GetLastError());
            return R"({"error":{"code":"TOKEN_FETCH_FAILED","message":"Failed to retrieve tunnel token: )"
                   + tempClient.GetLastError() + R"("}})";
        }

        SPDLOG_DEBUG("setupTunnel — tunnel token retrieved");

        // =====================================================================
        // Step 9: Discover and start cloudflared
        // =====================================================================
        std::string binaryPath = BinaryDiscovery::FindBinary("", "");
        if (binaryPath.empty())
        {
            SPDLOG_ERROR("setupTunnel — cloudflared binary not found in system PATH");
            return R"({"error":{"code":"BINARY_NOT_FOUND","message":"cloudflared binary not found in system PATH"}})";
        }

        SPDLOG_DEBUG("setupTunnel — cloudflared found at: {}", binaryPath);

        std::vector<std::string> args = {"tunnel", "run", "--token", tunnelToken};
        if (!g_subprocess->Spawn(binaryPath, args))
        {
            SPDLOG_ERROR("setupTunnel — failed to spawn cloudflared");
            return R"({"error":{"code":"SPAWN_FAILED","message":"Failed to start cloudflared process"}})";
        }

        SPDLOG_INFO("setupTunnel — cloudflared spawned");

        // =====================================================================
        // Step 10: Verify connection (poll up to kSetupConnectMaxRetries seconds)
        // =====================================================================
        std::string remoteStatus;
        int connectionCount = 0;
        bool connected = false;

        for (unsigned int attempt = 0; attempt < kSetupConnectMaxRetries; ++attempt)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSetupConnectRetryMs));

            if (tempClient.GetConnections(accountId, tunnelInfo.tunnel_id,
                                          remoteStatus, connectionCount))
            {
                SPDLOG_DEBUG("setupTunnel — connection check attempt {}: status={}, count={}",
                             attempt + 1, remoteStatus, connectionCount);
                if (connectionCount > 0)
                {
                    connected = true;
                    break;
                }
            }
        }

        if (!connected)
        {
            SPDLOG_WARN("setupTunnel — connection not verified within {} seconds, "
                        "cloudflared is running but may still be connecting",
                        kSetupConnectMaxRetries);
        }

        // =====================================================================
        // Step 11: Persist config to RocksDB
        // =====================================================================
        g_config.account_id = accountId;
        g_config.tunnel_id = tunnelInfo.tunnel_id;
        g_config.tunnel_name = tunnelName;
        g_config.tunnel_token = tunnelToken;
        g_config.api_token = apiToken;
        g_config.hostname = hostname;
        g_config.service_url = serviceUrl;
        g_config.binary_path = binaryPath;
        g_config.auto_start = true;
        g_config.log_level = "info";
        g_config.created_at = requestData.value("created_at", std::string(""));

        if (!SaveConfig(g_storage, g_config))
        {
            SPDLOG_ERROR("setupTunnel — failed to persist config to RocksDB");
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to persist tunnel configuration"}})";
        }

        SPDLOG_INFO("setupTunnel — config persisted to RocksDB");

        // =====================================================================
        // Step 12: Return success
        // =====================================================================
        json response;
        response["success"] = true;
        response["tunnel_id"] = tunnelInfo.tunnel_id;
        response["tunnel_name"] = tunnelName;
        response["hostname"] = hostname;
        response["status"] = connected ? "connected" : "pending";
        response["connection_count"] = connectionCount;
        response["message"] = "Tunnel created and cloudflared started successfully";

        SPDLOG_INFO("setupTunnel — complete: tunnel={}, hostname={}, connected={}",
                     tunnelInfo.tunnel_id, hostname, connected);

        return response.dump();
    }
    catch (const json::parse_error&)
    {
        return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})";
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("setupTunnel — unexpected error: {}", e.what());
        return R"({"error":{"code":"INTERNAL_ERROR","message":"Unexpected error: )"
               + std::string(e.what()) + R"("}})";
    }
}

// ============================================================================
// Tunnel Plugin Class
// ============================================================================

/**
 * @brief  Tunnel plugin — manages cloudflared lifecycle and Cloudflare API integration.
 *
 * Provides admin API endpoints for tunnel configuration, start/stop/restart,
 * status monitoring, log viewing, and setup wizard. Configuration persists to RocksDB
 * and auto-starts cloudflared on server restart if configured.
 */
class TunnelPlugin : public IPlugin
{
    REGISTER_PLUGIN(kTunnelPluginPriority, ({"/api/v1/admin/tunnel/configure",
        "/api/v1/admin/tunnel/status", "/api/v1/admin/tunnel/start",
        "/api/v1/admin/tunnel/stop", "/api/v1/admin/tunnel/restart",
        "/api/v1/admin/tunnel/logs", "/api/v1/admin/tunnel/setup"}))

public:
    ~TunnelPlugin() override = default;

    std::string GetName() override { return "Tunnel"; }

    /**
     * @brief  Initialize the tunnel plugin.
     *
     * Retrieves services from the locator, registers admin API handlers,
     * sets up log capture from SubprocessManager, loads config from RocksDB,
     * and auto-starts cloudflared if configured.
     *
     * @param  manager  Service locator for accessing server services
     * @return true on success, false on error
     */
    bool Initialize(IServiceLocator& manager) noexcept override
    {
        SPDLOG_INFO("Tunnel plugin initializing...");

        auto* pm = manager.GetService<PluginManager>(Fnv1a("PluginManager"));
        if (pm == nullptr)
        {
            SPDLOG_ERROR("TunnelPlugin — PluginManager not found");
            return false;
        }

        g_storage = manager.GetService<IStorageEngine>(Fnv1a("StorageEngine"));
        if (g_storage == nullptr)
        {
            SPDLOG_ERROR("TunnelPlugin — StorageEngine not found");
            return false;
        }

        SPDLOG_DEBUG("Tunnel plugin services loaded");

        // Register admin API handlers
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/tunnel/configure", configureTunnel, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "GET", "/api/v1/admin/tunnel/status", tunnelStatus, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/tunnel/start", startTunnel, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/tunnel/stop", stopTunnel, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/tunnel/restart", restartTunnel, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "GET", "/api/v1/admin/tunnel/logs", tunnelLogs, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/tunnel/setup", setupTunnel, kOverrideHandlerPriority)

        // Create SubprocessManager
        g_subprocess = std::make_unique<SubprocessManager>();

        // Set up log callback that pushes to ring buffer
        g_subprocess->SetLogCallback([](const std::string& line)
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            if (g_logLines.size() >= kMaxLogLines)
            {
                g_logLines.erase(g_logLines.begin());
            }
            g_logLines.push_back(line);
        });

        // Load config from RocksDB
        TunnelPluginConfig savedConfig;
        if (LoadConfig(g_storage, savedConfig))
        {
            g_config = savedConfig;
            SPDLOG_INFO("Tunnel config loaded (account: {}, tunnel: {})",
                        g_config.account_id, g_config.tunnel_id);

            // Auto-start cloudflared if configured
            if (g_config.auto_start && g_config.IsValid())
            {
                std::string binaryPath = BinaryDiscovery::FindBinary(
                    g_config.binary_path, "");
                if (!binaryPath.empty())
                {
                    std::vector<std::string> args = {
                        "tunnel", "run", "--token", g_config.tunnel_token
                    };
                    if (g_subprocess->Spawn(binaryPath, args))
                    {
                        SPDLOG_INFO("Tunnel auto-started with saved config");
                    }
                    else
                    {
                        SPDLOG_ERROR("Tunnel auto-start failed");
                    }
                }
                else
                {
                    SPDLOG_WARN("Tunnel auto-start skipped — cloudflared binary not found");
                }
            }
        }

        SPDLOG_INFO("Tunnel plugin initialized with 7 handlers");
        return true;
    }

    /**
     * @brief  Graceful shutdown — stop cloudflared and persist config.
     * @return true on success
     */
    bool Shutdown() noexcept override
    {
        SPDLOG_INFO("Tunnel plugin shutting down...");

        // Stop cloudflared subprocess
        if (g_subprocess && g_subprocess->GetStatus().running)
        {
            g_subprocess->Stop();
        }

        // Persist config so it auto-restarts on next launch
        if (g_storage != nullptr && g_config.IsValid())
        {
            SaveConfig(g_storage, g_config);
        }

        return true;
    }

    /**
     * @brief  Release resources after shutdown.
     * @return true on success
     */
    bool DeInit() noexcept override
    {
        g_subprocess.reset();
        g_client.reset();
        g_storage = nullptr;
        g_config = TunnelPluginConfig{};
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            g_logLines.clear();
        }
        SPDLOG_INFO("Tunnel plugin deinitialized");
        return true;
    }
};

EXPORT_PLUGIN(TunnelPlugin)
