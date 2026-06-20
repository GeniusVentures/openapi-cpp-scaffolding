/**
 * @file       archive_plugin.cpp
 * @brief      Archive plugin — streams storage changes to S3-compatible storage
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "singleton/IPlugin.hpp"
#include "singleton/PluginRegistration.hpp"
#include "singleton/PluginManager.hpp"
#include "singleton/IServiceLocator.hpp"
#include "singleton/fnv1a.hpp"
#include "storage/IStorageEngine.hpp"
#include "archive/S3Client.hpp"
#include "archive/StreamingWorker.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace gnus::hash;

/// Constants
inline constexpr unsigned int kArchivePluginPriority = 110;  ///< Loads after identity (100)

// ============================================================================
// File-scope globals (set during Initialize, used by handlers)
// ============================================================================

static IStorageEngine* g_storage = nullptr;             ///< Storage engine (not owned)
static StreamingWorker* g_worker = nullptr;              ///< Current worker (owned via g_workerOwner)
static std::unique_ptr<StreamingWorker> g_workerOwner;   ///< Owner of the worker
static ArchiveConfig g_config;                           ///< Current archive configuration

// ============================================================================
// Config Persistence Helpers
// ============================================================================

/**
 * @brief  Persist archive configuration to RocksDB under key "archive/config".
 * @param  storage  Storage engine instance
 * @param  config   Configuration to save
 * @return true on success, false on error
 */
static bool SaveConfig(IStorageEngine* storage, const ArchiveConfig& config) noexcept
{
    if (storage == nullptr)
    {
        return false;
    }

    try
    {
        json j;
        j["endpoint"] = config.endpoint;
        j["bucket"] = config.bucket;
        j["access_key"] = config.access_key;
        j["secret_key"] = config.secret_key;
        j["region"] = config.region;
        j["prefix"] = config.prefix;
        return storage->Put("archive/config", j.dump());
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("SaveConfig failed: {}", e.what());
        return false;
    }
}

/**
 * @brief  Load archive configuration from RocksDB key "archive/config".
 * @param  storage      Storage engine instance
 * @param  config_out   Output — populated with stored configuration
 * @return true if a valid config was loaded, false otherwise
 */
static bool LoadConfig(IStorageEngine* storage, ArchiveConfig& config_out) noexcept
{
    if (storage == nullptr)
    {
        return false;
    }

    std::string value;
    if (!storage->Get("archive/config", value))
    {
        return false;
    }

    try
    {
        json j = json::parse(value);
        config_out.endpoint = j.value("endpoint", "");
        config_out.bucket = j.value("bucket", "");
        config_out.access_key = j.value("access_key", "");
        config_out.secret_key = j.value("secret_key", "");
        config_out.region = j.value("region", "us-east-1");
        config_out.prefix = j.value("prefix", "genius-archive");
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
 * @brief  Configure archive target — validate config, persist to RocksDB, start worker.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/archive/configure)
 * @param  body     JSON body with endpoint, bucket, access_key, secret_key, region, prefix
 * @return JSON success or error response
 */
static std::string configureArchive(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& body)
{
    try
    {
        json requestData = json::parse(body);

        // Validate required fields
        if (!requestData.contains("endpoint") || !requestData.contains("bucket") ||
            !requestData.contains("access_key") || !requestData.contains("secret_key"))
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"endpoint, bucket, access_key, and secret_key are required"}})";
        }

        // Build config from JSON
        ArchiveConfig config;
        config.endpoint = requestData["endpoint"].get<std::string>();
        config.bucket = requestData["bucket"].get<std::string>();
        config.access_key = requestData["access_key"].get<std::string>();
        config.secret_key = requestData["secret_key"].get<std::string>();
        config.region = requestData.value("region", std::string("us-east-1"));
        config.prefix = requestData.value("prefix", std::string("genius-archive"));

        // Validate config
        if (!config.IsValid())
        {
            return R"({"error":{"code":"INVALID_CONFIG","message":"Configuration validation failed — endpoint, bucket, access_key, and secret_key must be non-empty"}})";
        }

        // Persist config to RocksDB
        if (!SaveConfig(g_storage, config))
        {
            SPDLOG_ERROR("configureArchive — failed to persist config to RocksDB");
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to persist configuration"}})";
        }

        // Stop existing worker if running
        if (g_worker != nullptr)
        {
            g_worker->Stop();
        }

        // Create and start new worker
        g_config = config;
        g_workerOwner = std::make_unique<StreamingWorker>(g_config);
        g_worker = g_workerOwner.get();
        g_worker->Start();

        SPDLOG_INFO("Archive configured and streaming started (bucket: {})", g_config.bucket);
        return R"({"success":true,"message":"Archive configured and streaming started"})";
    }
    catch (const json::parse_error&)
    {
        return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})";
    }
}

/**
 * @brief  Get archive streaming status.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (GET)
 * @param  urlPath  URL path (/api/v1/admin/archive/status)
 * @param  body     Empty
 * @return JSON object with worker status fields
 */
static std::string archiveStatus(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& /*body*/)
{
    if (g_worker == nullptr)
    {
        return R"({"configured":false,"streaming":false})";
    }

    WorkerStatus status = g_worker->GetStatus();

    json response;
    response["configured"] = true;
    response["streaming"] = true;
    response["queue_size"] = status.queue_size;
    response["is_paused"] = status.is_paused;
    response["is_connected"] = status.is_connected;
    response["total_streamed"] = status.total_streamed;
    response["total_errors"] = status.total_errors;
    response["last_error"] = status.last_error;

    return response.dump();
}

/**
 * @brief  Pause archive streaming.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/archive/pause)
 * @param  body     Empty
 * @return JSON success or error response
 */
static std::string archivePause(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& /*body*/)
{
    if (g_worker == nullptr)
    {
        return R"({"error":{"code":"NOT_CONFIGURED","message":"Archive not configured"}})";
    }

    g_worker->Pause();
    SPDLOG_INFO("Archive streaming paused");
    return R"({"success":true,"message":"Archive streaming paused"})";
}

/**
 * @brief  Resume archive streaming.
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/archive/resume)
 * @param  body     Empty
 * @return JSON success or error response
 */
static std::string archiveResume(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& /*body*/)
{
    if (g_worker == nullptr)
    {
        return R"({"error":{"code":"NOT_CONFIGURED","message":"Archive not configured"}})";
    }

    g_worker->Resume();
    SPDLOG_INFO("Archive streaming resumed");
    return R"({"success":true,"message":"Archive streaming resumed"})";
}

/**
 * @brief  Test connection to the S3 endpoint.
 *
 * Creates a temporary S3Client and attempts to connect.
 * Accepts the same fields as configureArchive; if body is empty or missing
 * fields, uses the currently saved config.
 *
 * @param  ctx      Request context (authenticated)
 * @param  method   HTTP method (POST)
 * @param  urlPath  URL path (/api/v1/admin/archive/test)
 * @param  body     JSON body with endpoint, bucket, access_key, secret_key (or empty to use saved config)
 * @return JSON success or CONNECTION_FAILED error
 */
static std::string archiveTestConnection(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& body)
{
    ArchiveConfig testConfig;

    // If body has fields, use them; otherwise fall back to saved config
    if (!body.empty())
    {
        try
        {
            json requestData = json::parse(body);

            testConfig.endpoint = requestData.value("endpoint", "");
            testConfig.bucket = requestData.value("bucket", "");
            testConfig.access_key = requestData.value("access_key", "");
            testConfig.secret_key = requestData.value("secret_key", "");
            testConfig.region = requestData.value("region", std::string("us-east-1"));
            testConfig.prefix = requestData.value("prefix", std::string("genius-archive"));
        }
        catch (const json::parse_error&)
        {
            return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})";
        }
    }
    else if (g_config.IsValid())
    {
        testConfig = g_config;
    }
    else
    {
        return R"({"error":{"code":"NOT_CONFIGURED","message":"No configuration provided and no saved config available"}})";
    }

    if (!testConfig.IsValid())
    {
        return R"({"error":{"code":"INVALID_CONFIG","message":"Configuration validation failed — endpoint, bucket, access_key, and secret_key must be non-empty"}})";
    }

    // Create temporary client and test connection
    S3Client testClient(testConfig);
    bool connected = testClient.ConnectToEndpoint();
    testClient.Disconnect();

    if (connected)
    {
        json response;
        response["success"] = true;
        response["message"] = "Connection to S3 endpoint successful";
        response["endpoint"] = testConfig.endpoint;
        response["bucket"] = testConfig.bucket;
        return response.dump();
    }

    json errorResponse;
    errorResponse["error"]["code"] = "CONNECTION_FAILED";
    errorResponse["error"]["message"] = "Failed to connect to S3 endpoint";
    errorResponse["error"]["endpoint"] = testConfig.endpoint;
    return errorResponse.dump();
}

// ============================================================================
// Archive Plugin Class
// ============================================================================

/**
 * @brief  Archive plugin — captures storage changes and streams them to S3
 *
 * Registers as a change callback on IStorageEngine during Initialize().
 * Changes are queued in a StreamingWorker and flushed as JSONL batches to S3.
 * Admin API endpoints allow runtime configuration and control.
 * Configuration is persisted to RocksDB and auto-loaded on restart.
 */
class ArchivePlugin : public IPlugin
{
    REGISTER_PLUGIN(kArchivePluginPriority, ({"/api/v1/admin/archive/configure",
        "/api/v1/admin/archive/status", "/api/v1/admin/archive/pause",
        "/api/v1/admin/archive/resume", "/api/v1/admin/archive/test"}))

public:
    ~ArchivePlugin() override = default;

    std::string GetName() override { return "Archive"; }

    /**
     * @brief  Initialize the archive plugin.
     *
     * Retrieves services from the locator, registers a change callback on
     * IStorageEngine, and attempts to auto-start the worker if a saved
     * configuration exists in RocksDB.
     *
     * @param  manager  Service locator for accessing server services
     * @return true on success, false on error
     */
    bool Initialize(IServiceLocator& manager) noexcept override
    {
        SPDLOG_INFO("Archive plugin initializing...");

        auto* pm = manager.GetService<PluginManager>(Fnv1a("PluginManager"));
        if (pm == nullptr)
        {
            SPDLOG_ERROR("ArchivePlugin — PluginManager not found");
            return false;
        }

        g_storage = manager.GetService<IStorageEngine>(Fnv1a("StorageEngine"));
        if (g_storage == nullptr)
        {
            SPDLOG_ERROR("ArchivePlugin — StorageEngine not found");
            return false;
        }

        SPDLOG_DEBUG("Archive plugin services loaded");

        // Register change callback on storage engine
        g_storage->RegisterChangeCallback(
            [](const std::string& key, const std::string& value, bool isDelete)
            {
                if (g_worker != nullptr)
                {
                    g_worker->Enqueue(key, value, isDelete);
                }
            });

        // Register admin API handlers
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/archive/configure", configureArchive, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "GET", "/api/v1/admin/archive/status", archiveStatus, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/archive/pause", archivePause, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/archive/resume", archiveResume, kOverrideHandlerPriority)
        REGISTER_HANDLER(pm, "POST", "/api/v1/admin/archive/test", archiveTestConnection, kOverrideHandlerPriority)

        // Auto-start worker if saved config exists
        ArchiveConfig savedConfig;
        if (LoadConfig(g_storage, savedConfig))
        {
            g_config = savedConfig;
            g_workerOwner = std::make_unique<StreamingWorker>(g_config);
            g_worker = g_workerOwner.get();
            g_worker->Start();
            SPDLOG_INFO("Archive auto-started with saved config (bucket: {})", g_config.bucket);
        }

        SPDLOG_INFO("Archive plugin initialized with 5 handlers");
        return true;
    }

    /**
     * @brief  Graceful shutdown — persist config and stop the worker thread.
     * @return true on success
     */
    bool Shutdown() noexcept override
    {
        SPDLOG_INFO("Archive plugin shutting down...");

        // Persist config so it auto-restarts on next launch
        if (g_storage != nullptr && g_config.IsValid())
        {
            SaveConfig(g_storage, g_config);
        }

        if (g_worker != nullptr)
        {
            g_worker->Stop();
        }
        return true;
    }

    /**
     * @brief  Release resources after shutdown.
     * @return true on success
     */
    bool DeInit() noexcept override
    {
        g_workerOwner.reset();
        g_worker = nullptr;
        g_storage = nullptr;
        g_config = ArchiveConfig{};
        SPDLOG_INFO("Archive plugin deinitialized");
        return true;
    }
};

EXPORT_PLUGIN(ArchivePlugin)
