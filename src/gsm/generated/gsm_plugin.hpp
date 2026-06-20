/**
 * @file       gsm_plugin.hpp
 * @brief      Auto-generated OOP plugin wrapper for GSM API
 * @date       auto-generated
 *
 * DO NOT EDIT — regenerated from gsm_openapi.json
 *
 * Handler stubs are protected virtual instance methods. Storage is
 * a protected instance member (m_storage). Initialize() delegates
 * handler registration to the overridable RegisterStubHandlers().
 */

#ifndef JOBS_PLUGIN_HPP
#define JOBS_PLUGIN_HPP

#include "singleton/IPlugin.hpp"
#include "singleton/PluginRegistration.hpp"
#include "singleton/PluginManager.hpp"
#include "storage/IStorageEngine.hpp"
#include "storage/KeyBuilder.hpp"
#include "nlohmann/json.hpp"

#include <chrono>
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

using json = nlohmann::json;
using namespace gnus::hash;

///
/// Auto-generated plugin class for Jobs.
/// Derive from this class to override individual handlers without
/// editing this generated file.
///
class JobsPlugin : public IPlugin
{
protected:
    static constexpr unsigned int kDefaultPluginPriority  = 100;
    static constexpr unsigned int kDefaultPaginationLimit = 50;
    static constexpr unsigned int kUuidHexLength         = 32;
    static constexpr uint8_t      kHexDigitMax           = 15;
    static constexpr char         kPathSeparator         = '/';

    IStorageEngine* m_storage = nullptr;

    REGISTER_PLUGIN(kDefaultPluginPriority, ({"/api/v1/evidence/{evidenceId}", "/api/v1/jobs", "/api/v1/jobs/{jobId}", "/api/v1/jobs/{jobId}/evidence", "/api/v1/jobs/{jobId}/reports", "/api/v1/jobs/{jobId}/reviews", "/api/v1/reports/{reportId}"}))

    /// Generate a random UUID (32 hex characters, no hyphens)
    static std::string GenerateUuid()
    {
        static thread_local std::mt19937 rng(std::random_device{}());
        static thread_local std::uniform_int_distribution<uint8_t> dist(0, kHexDigitMax);
        static constexpr const char* kHexChars = "0123456789abcdef";

        std::string result;
        result.reserve(kUuidHexLength);
        for (unsigned int i = 0; i < kUuidHexLength; ++i)
        {
            result += kHexChars[dist(rng)];
        }
        return result;
    }

    /// Get current UTC timestamp in ISO 8601 format
    static std::string GetCurrentTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

///
/// CRUD handler for getEvidence — get a single evidence entity
///
    virtual std::string getEvidence(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("jobs", "evidence", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})";
        }
        std::string key = keyResult.value();

        std::string value;
        if (!m_storage->Get(key, value))
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
        }
        json item = json::parse(value);
        if (!item.contains("tenant_id")) { item["tenant_id"] = "default"; }
        if (!item.contains("organization_id")) { item["organization_id"] = "default"; }
        return item.dump();
    }

///
/// CRUD handler for listJobs — list jobs entities
///
    virtual std::string listJobs(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("jobs", "jobs");
        auto scanResult = m_storage->Scan(prefix);

        json result = json::array();
        for (const auto& [k, v] : scanResult)
        {
            json item = json::parse(v);
            // Backfill multi-tenant fields for records created before
            // tenant stamping was added to create handlers.
            if (!item.contains("tenant_id"))
            {
                item["tenant_id"] = "default";
            }
            if (!item.contains("organization_id"))
            {
                item["organization_id"] = "default";
            }
            result.push_back(item);
        }
        json response;
        response["data"] = result;
        json pagination;
        pagination["limit"] = kDefaultPaginationLimit;
        pagination["has_more"] = false;
        response["pagination"] = pagination;
        return response.dump();
    }

///
/// CRUD handler for createJob — create a new jobs entity
///
    virtual std::string createJob(
        const RequestContext& ctx,
        const std::string& /*method*/,
        const std::string& /*urlPath*/,
        const std::string& body)
    {
        try
        {
            json requestData = json::parse(body);
            std::string id = GenerateUuid();
            requestData["id"] = id;
            requestData["tenant_id"] = ctx.tenantId;
            requestData["organization_id"] = ctx.organizationId;
            requestData["created_at"] = GetCurrentTimestamp();
            requestData["updated_at"] = requestData["created_at"];

            auto keyResult = KeyBuilder::Build("jobs", "jobs", id);
            if (!keyResult.has_value())
            {
                return R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})";
            }
            std::string key = keyResult.value();

            if (!m_storage->Put(key, requestData.dump()))
            {
                return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to store entity"}})";
            }
            return requestData.dump();
        }
        catch (const json::parse_error&)
        {
            return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})";
        }
    }

///
/// CRUD handler for getJob — get a single jobs entity
///
    virtual std::string getJob(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("jobs", "jobs", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})";
        }
        std::string key = keyResult.value();

        std::string value;
        if (!m_storage->Get(key, value))
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
        }
        json item = json::parse(value);
        if (!item.contains("tenant_id")) { item["tenant_id"] = "default"; }
        if (!item.contains("organization_id")) { item["organization_id"] = "default"; }
        return item.dump();
    }

///
/// CRUD handler for updateJob — update an existing jobs entity
///
    virtual std::string updateJob(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("jobs", "jobs", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})";
        }
        std::string key = keyResult.value();

        std::string existing;
        if (!m_storage->Get(key, existing))
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
        }

        try
        {
            json updated = json::parse(existing);
            updated.update(json::parse(body));
            updated["updated_at"] = GetCurrentTimestamp();

            if (!m_storage->Put(key, updated.dump()))
            {
                return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to update entity"}})";
            }
            return updated.dump();
        }
        catch (const json::parse_error&)
        {
            return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})";
        }
    }

///
/// CRUD handler for deleteJob — delete a jobs entity
///
    virtual std::string deleteJob(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("jobs", "jobs", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})";
        }
        std::string key = keyResult.value();

        if (!m_storage->Delete(key))
        {
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to delete entity"}})";
        }
        return R"({"deleted":true})";
    }

///
/// CRUD handler for listEvidence — get a single evidence entity
///
    virtual std::string listEvidence(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("jobs", "evidence", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})";
        }
        std::string key = keyResult.value();

        std::string value;
        if (!m_storage->Get(key, value))
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
        }
        json item = json::parse(value);
        if (!item.contains("tenant_id")) { item["tenant_id"] = "default"; }
        if (!item.contains("organization_id")) { item["organization_id"] = "default"; }
        return item.dump();
    }

///
/// Fallback handler for createReport — operation type not recognized
///
    virtual std::string createReport(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        return R"({"error":{"code":"INVALID_PATH","message":"Operation not supported"}})";
    }

///
/// CRUD handler for listReviews — get a single reviews entity
///
    virtual std::string listReviews(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("jobs", "reviews", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})";
        }
        std::string key = keyResult.value();

        std::string value;
        if (!m_storage->Get(key, value))
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
        }
        json item = json::parse(value);
        if (!item.contains("tenant_id")) { item["tenant_id"] = "default"; }
        if (!item.contains("organization_id")) { item["organization_id"] = "default"; }
        return item.dump();
    }

///
/// Fallback handler for createReview — operation type not recognized
///
    virtual std::string createReview(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        return R"({"error":{"code":"INVALID_PATH","message":"Operation not supported"}})";
    }

///
/// CRUD handler for getReport — get a single reports entity
///
    virtual std::string getReport(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("jobs", "reports", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})";
        }
        std::string key = keyResult.value();

        std::string value;
        if (!m_storage->Get(key, value))
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
        }
        json item = json::parse(value);
        if (!item.contains("tenant_id")) { item["tenant_id"] = "default"; }
        if (!item.contains("organization_id")) { item["organization_id"] = "default"; }
        return item.dump();
    }

    virtual void RegisterStubHandlers(PluginManager* pm)
    {
        pm->RegisterHandler("GET", "/api/v1/evidence/{evidenceId}", "getEvidence",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getEvidence(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/jobs", "listJobs",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listJobs(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/jobs", "createJob",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createJob(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/jobs/{jobId}", "getJob",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getJob(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/jobs/{jobId}", "updateJob",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateJob(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/jobs/{jobId}", "deleteJob",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteJob(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/jobs/{jobId}/evidence", "listEvidence",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listEvidence(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/jobs/{jobId}/reports", "createReport",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createReport(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/jobs/{jobId}/reviews", "listReviews",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listReviews(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/jobs/{jobId}/reviews", "createReview",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createReview(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/reports/{reportId}", "getReport",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getReport(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
    }

public:
    ~JobsPlugin() override = default;

    std::string GetName() override { return "Jobs"; }

    bool Initialize(IServiceLocator& manager) noexcept override
    {
        auto* pm = manager.GetService<PluginManager>(Fnv1a("PluginManager"));
        if (pm == nullptr) { return false; }
        m_storage = manager.GetService<IStorageEngine>(Fnv1a("StorageEngine"));
        if (m_storage == nullptr) { return false; }
        RegisterStubHandlers(pm);
        return true;
    }

    bool Shutdown() noexcept override
    {
        return true;
    }

    bool DeInit() noexcept override
    {
        return true;
    }
};

#endif // JOBS_PLUGIN_HPP
