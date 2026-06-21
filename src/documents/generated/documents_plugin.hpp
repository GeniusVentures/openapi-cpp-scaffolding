/**
 * @file       documents_plugin.hpp
 * @brief      Auto-generated OOP plugin wrapper for AI-Native Business OS API - Documents
 * @date       auto-generated
 *
 * DO NOT EDIT — regenerated from documents_openapi.json
 *
 * Handler stubs are protected virtual instance methods. Storage is
 * a protected instance member (m_storage). Initialize() delegates
 * handler registration to the overridable RegisterStubHandlers().
 */

#ifndef DOCUMENTS_PLUGIN_HPP
#define DOCUMENTS_PLUGIN_HPP

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
/// Auto-generated plugin class for Documents.
/// Derive from this class to override individual handlers without
/// editing this generated file.
///
class DocumentsPlugin : public IPlugin
{
protected:
    static constexpr unsigned int kDefaultPluginPriority  = 100;
    static constexpr unsigned int kDefaultPaginationLimit = 50;
    static constexpr unsigned int kUuidHexLength         = 32;
    static constexpr uint8_t      kHexDigitMax           = 15;
    static constexpr char         kPathSeparator         = '/';

    IStorageEngine* m_storage = nullptr;

    REGISTER_PLUGIN(kDefaultPluginPriority, ({"/api/v1/documents", "/api/v1/documents/{documentId}"}))

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
/// CRUD handler for listDocuments — list documents entities
///
    virtual std::string listDocuments(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("documents", "documents");
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
/// CRUD handler for createDocument — create a new documents entity
///
    virtual std::string createDocument(
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

            auto keyResult = KeyBuilder::Build("documents", "documents", id);
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
/// CRUD handler for getDocument — get a single documents entity
///
    virtual std::string getDocument(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("documents", "documents", id);
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
/// CRUD handler for updateDocument — update an existing documents entity
///
    virtual std::string updateDocument(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("documents", "documents", id);
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
/// CRUD handler for deleteDocument — delete a documents entity
///
    virtual std::string deleteDocument(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("documents", "documents", id);
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

    virtual void RegisterStubHandlers(PluginManager* pm)
    {
        pm->RegisterHandler("GET", "/api/v1/documents", "listDocuments",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listDocuments(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/documents", "createDocument",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createDocument(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/documents/{documentId}", "getDocument",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getDocument(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/documents/{documentId}", "updateDocument",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateDocument(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/documents/{documentId}", "deleteDocument",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteDocument(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
    }

public:
    ~DocumentsPlugin() override = default;

    std::string GetName() override { return "Documents"; }

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

#endif // DOCUMENTS_PLUGIN_HPP
