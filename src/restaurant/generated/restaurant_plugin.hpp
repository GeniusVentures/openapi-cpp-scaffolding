/**
 * @file       restaurant_plugin.hpp
 * @brief      Auto-generated OOP plugin wrapper for AI-Native Business OS API - Restaurant
 * @date       auto-generated
 *
 * DO NOT EDIT — regenerated from restaurant_openapi.json
 *
 * Handler stubs are protected virtual instance methods. Storage is
 * a protected instance member (m_storage). Initialize() delegates
 * handler registration to the overridable RegisterStubHandlers().
 */

#ifndef RESTAURANT_PLUGIN_HPP
#define RESTAURANT_PLUGIN_HPP

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
/// Auto-generated plugin class for Restaurant.
/// Derive from this class to override individual handlers without
/// editing this generated file.
///
class RestaurantPlugin : public IPlugin
{
protected:
    static constexpr unsigned int kDefaultPluginPriority  = 100;
    static constexpr unsigned int kDefaultPaginationLimit = 50;
    static constexpr unsigned int kUuidHexLength         = 32;
    static constexpr uint8_t      kHexDigitMax           = 15;
    static constexpr char         kPathSeparator         = '/';

    IStorageEngine* m_storage = nullptr;

    REGISTER_PLUGIN(kDefaultPluginPriority, ({"/api/v1/restaurant/delivery-assignments", "/api/v1/restaurant/delivery-assignments/{deliveryAssignmentId}", "/api/v1/restaurant/ingredients", "/api/v1/restaurant/ingredients/{ingredientId}", "/api/v1/restaurant/kds/board", "/api/v1/restaurant/kitchen-tickets", "/api/v1/restaurant/kitchen-tickets/{kitchenTicketId}", "/api/v1/restaurant/menu-categories", "/api/v1/restaurant/menu-categories/{menuCategoryId}", "/api/v1/restaurant/menu-items", "/api/v1/restaurant/menu-items/{menuItemId}", "/api/v1/restaurant/menus", "/api/v1/restaurant/menus/{restaurantMenuId}", "/api/v1/restaurant/modifier-groups", "/api/v1/restaurant/modifier-groups/{modifierGroupId}", "/api/v1/restaurant/recipes", "/api/v1/restaurant/recipes/{recipeId}", "/api/v1/restaurant/riders", "/api/v1/restaurant/riders/{riderId}", "/api/v1/restaurant/tables", "/api/v1/restaurant/tables/{tableId}", "/api/v1/restaurant/tables/{tableId}/seat"}))

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
/// CRUD handler for listDeliveryAssignments — list delivery-assignments entities
///
    virtual std::string listDeliveryAssignments(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "delivery-assignments");
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
/// CRUD handler for createDeliveryAssignment — create a new delivery-assignments entity
///
    virtual std::string createDeliveryAssignment(
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

            auto keyResult = KeyBuilder::Build("restaurant", "delivery-assignments", id);
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
/// CRUD handler for getDeliveryAssignment — get a single delivery-assignments entity
///
    virtual std::string getDeliveryAssignment(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "delivery-assignments", id);
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
/// CRUD handler for updateDeliveryAssignment — update an existing delivery-assignments entity
///
    virtual std::string updateDeliveryAssignment(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "delivery-assignments", id);
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
/// CRUD handler for deleteDeliveryAssignment — delete a delivery-assignments entity
///
    virtual std::string deleteDeliveryAssignment(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "delivery-assignments", id);
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
/// CRUD handler for listIngredients — list ingredients entities
///
    virtual std::string listIngredients(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "ingredients");
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
/// CRUD handler for createIngredient — create a new ingredients entity
///
    virtual std::string createIngredient(
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

            auto keyResult = KeyBuilder::Build("restaurant", "ingredients", id);
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
/// CRUD handler for getIngredient — get a single ingredients entity
///
    virtual std::string getIngredient(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "ingredients", id);
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
/// CRUD handler for updateIngredient — update an existing ingredients entity
///
    virtual std::string updateIngredient(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "ingredients", id);
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
/// CRUD handler for deleteIngredient — delete a ingredients entity
///
    virtual std::string deleteIngredient(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "ingredients", id);
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
/// CRUD handler for getKitchenBoard — list board entities
///
    virtual std::string getKitchenBoard(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "board");
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
/// CRUD handler for listKitchenTickets — list kitchen-tickets entities
///
    virtual std::string listKitchenTickets(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "kitchen-tickets");
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
/// CRUD handler for createKitchenTicket — create a new kitchen-tickets entity
///
    virtual std::string createKitchenTicket(
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

            auto keyResult = KeyBuilder::Build("restaurant", "kitchen-tickets", id);
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
/// CRUD handler for getKitchenTicket — get a single kitchen-tickets entity
///
    virtual std::string getKitchenTicket(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "kitchen-tickets", id);
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
/// CRUD handler for updateKitchenTicket — update an existing kitchen-tickets entity
///
    virtual std::string updateKitchenTicket(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "kitchen-tickets", id);
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
/// CRUD handler for deleteKitchenTicket — delete a kitchen-tickets entity
///
    virtual std::string deleteKitchenTicket(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "kitchen-tickets", id);
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
/// CRUD handler for listMenuCategorys — list menu-categories entities
///
    virtual std::string listMenuCategorys(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "menu-categories");
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
/// CRUD handler for createMenuCategory — create a new menu-categories entity
///
    virtual std::string createMenuCategory(
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

            auto keyResult = KeyBuilder::Build("restaurant", "menu-categories", id);
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
/// CRUD handler for getMenuCategory — get a single menu-categories entity
///
    virtual std::string getMenuCategory(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menu-categories", id);
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
/// CRUD handler for updateMenuCategory — update an existing menu-categories entity
///
    virtual std::string updateMenuCategory(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menu-categories", id);
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
/// CRUD handler for deleteMenuCategory — delete a menu-categories entity
///
    virtual std::string deleteMenuCategory(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menu-categories", id);
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
/// CRUD handler for listMenuItems — list menu-items entities
///
    virtual std::string listMenuItems(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "menu-items");
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
/// CRUD handler for createMenuItem — create a new menu-items entity
///
    virtual std::string createMenuItem(
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

            auto keyResult = KeyBuilder::Build("restaurant", "menu-items", id);
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
/// CRUD handler for getMenuItem — get a single menu-items entity
///
    virtual std::string getMenuItem(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menu-items", id);
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
/// CRUD handler for updateMenuItem — update an existing menu-items entity
///
    virtual std::string updateMenuItem(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menu-items", id);
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
/// CRUD handler for deleteMenuItem — delete a menu-items entity
///
    virtual std::string deleteMenuItem(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menu-items", id);
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
/// CRUD handler for listRestaurantMenus — list menus entities
///
    virtual std::string listRestaurantMenus(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "menus");
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
/// CRUD handler for createRestaurantMenu — create a new menus entity
///
    virtual std::string createRestaurantMenu(
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

            auto keyResult = KeyBuilder::Build("restaurant", "menus", id);
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
/// CRUD handler for getRestaurantMenu — get a single menus entity
///
    virtual std::string getRestaurantMenu(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menus", id);
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
/// CRUD handler for updateRestaurantMenu — update an existing menus entity
///
    virtual std::string updateRestaurantMenu(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menus", id);
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
/// CRUD handler for deleteRestaurantMenu — delete a menus entity
///
    virtual std::string deleteRestaurantMenu(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "menus", id);
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
/// CRUD handler for listModifierGroups — list modifier-groups entities
///
    virtual std::string listModifierGroups(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "modifier-groups");
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
/// CRUD handler for createModifierGroup — create a new modifier-groups entity
///
    virtual std::string createModifierGroup(
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

            auto keyResult = KeyBuilder::Build("restaurant", "modifier-groups", id);
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
/// CRUD handler for getModifierGroup — get a single modifier-groups entity
///
    virtual std::string getModifierGroup(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "modifier-groups", id);
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
/// CRUD handler for updateModifierGroup — update an existing modifier-groups entity
///
    virtual std::string updateModifierGroup(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "modifier-groups", id);
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
/// CRUD handler for deleteModifierGroup — delete a modifier-groups entity
///
    virtual std::string deleteModifierGroup(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "modifier-groups", id);
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
/// CRUD handler for listRecipes — list recipes entities
///
    virtual std::string listRecipes(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "recipes");
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
/// CRUD handler for createRecipe — create a new recipes entity
///
    virtual std::string createRecipe(
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

            auto keyResult = KeyBuilder::Build("restaurant", "recipes", id);
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
/// CRUD handler for getRecipe — get a single recipes entity
///
    virtual std::string getRecipe(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "recipes", id);
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
/// CRUD handler for updateRecipe — update an existing recipes entity
///
    virtual std::string updateRecipe(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "recipes", id);
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
/// CRUD handler for deleteRecipe — delete a recipes entity
///
    virtual std::string deleteRecipe(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "recipes", id);
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
/// CRUD handler for listRiders — list riders entities
///
    virtual std::string listRiders(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "riders");
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
/// CRUD handler for createRider — create a new riders entity
///
    virtual std::string createRider(
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

            auto keyResult = KeyBuilder::Build("restaurant", "riders", id);
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
/// CRUD handler for getRider — get a single riders entity
///
    virtual std::string getRider(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "riders", id);
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
/// CRUD handler for updateRider — update an existing riders entity
///
    virtual std::string updateRider(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "riders", id);
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
/// CRUD handler for deleteRider — delete a riders entity
///
    virtual std::string deleteRider(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "riders", id);
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
/// CRUD handler for listTables — list tables entities
///
    virtual std::string listTables(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string prefix = KeyBuilder::MakePrefix("restaurant", "tables");
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
/// CRUD handler for createTable — create a new tables entity
///
    virtual std::string createTable(
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

            auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
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
/// CRUD handler for getTable — get a single tables entity
///
    virtual std::string getTable(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
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
/// CRUD handler for updateTable — update an existing tables entity
///
    virtual std::string updateTable(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
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
/// CRUD handler for deleteTable — delete a tables entity
///
    virtual std::string deleteTable(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        std::string id = urlPath.substr(urlPath.rfind(kPathSeparator) + 1);

        auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
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
/// Fallback handler for seatTable — operation type not recognized
///
    virtual std::string seatTable(
        const RequestContext& /*ctx*/,
        const std::string& /*method*/,
        const std::string& urlPath,
        const std::string& body)
    {
        return R"({"error":{"code":"INVALID_PATH","message":"Operation not supported"}})";
    }

    virtual void RegisterStubHandlers(PluginManager* pm)
    {
        pm->RegisterHandler("GET", "/api/v1/restaurant/delivery-assignments", "listDeliveryAssignments",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listDeliveryAssignments(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/delivery-assignments", "createDeliveryAssignment",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createDeliveryAssignment(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/delivery-assignments/{deliveryAssignmentId}", "getDeliveryAssignment",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getDeliveryAssignment(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/delivery-assignments/{deliveryAssignmentId}", "updateDeliveryAssignment",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateDeliveryAssignment(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/delivery-assignments/{deliveryAssignmentId}", "deleteDeliveryAssignment",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteDeliveryAssignment(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/ingredients", "listIngredients",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listIngredients(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/ingredients", "createIngredient",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createIngredient(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/ingredients/{ingredientId}", "getIngredient",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getIngredient(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/ingredients/{ingredientId}", "updateIngredient",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateIngredient(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/ingredients/{ingredientId}", "deleteIngredient",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteIngredient(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/kds/board", "getKitchenBoard",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getKitchenBoard(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/kitchen-tickets", "listKitchenTickets",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listKitchenTickets(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/kitchen-tickets", "createKitchenTicket",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createKitchenTicket(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/kitchen-tickets/{kitchenTicketId}", "getKitchenTicket",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getKitchenTicket(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/kitchen-tickets/{kitchenTicketId}", "updateKitchenTicket",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateKitchenTicket(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/kitchen-tickets/{kitchenTicketId}", "deleteKitchenTicket",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteKitchenTicket(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/menu-categories", "listMenuCategorys",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listMenuCategorys(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/menu-categories", "createMenuCategory",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createMenuCategory(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/menu-categories/{menuCategoryId}", "getMenuCategory",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getMenuCategory(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/menu-categories/{menuCategoryId}", "updateMenuCategory",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateMenuCategory(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/menu-categories/{menuCategoryId}", "deleteMenuCategory",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteMenuCategory(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/menu-items", "listMenuItems",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listMenuItems(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/menu-items", "createMenuItem",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createMenuItem(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/menu-items/{menuItemId}", "getMenuItem",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getMenuItem(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/menu-items/{menuItemId}", "updateMenuItem",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateMenuItem(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/menu-items/{menuItemId}", "deleteMenuItem",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteMenuItem(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/menus", "listRestaurantMenus",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listRestaurantMenus(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/menus", "createRestaurantMenu",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createRestaurantMenu(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/menus/{restaurantMenuId}", "getRestaurantMenu",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getRestaurantMenu(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/menus/{restaurantMenuId}", "updateRestaurantMenu",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateRestaurantMenu(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/menus/{restaurantMenuId}", "deleteRestaurantMenu",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteRestaurantMenu(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/modifier-groups", "listModifierGroups",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listModifierGroups(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/modifier-groups", "createModifierGroup",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createModifierGroup(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/modifier-groups/{modifierGroupId}", "getModifierGroup",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getModifierGroup(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/modifier-groups/{modifierGroupId}", "updateModifierGroup",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateModifierGroup(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/modifier-groups/{modifierGroupId}", "deleteModifierGroup",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteModifierGroup(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/recipes", "listRecipes",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listRecipes(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/recipes", "createRecipe",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createRecipe(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/recipes/{recipeId}", "getRecipe",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getRecipe(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/recipes/{recipeId}", "updateRecipe",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateRecipe(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/recipes/{recipeId}", "deleteRecipe",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteRecipe(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/riders", "listRiders",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listRiders(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/riders", "createRider",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createRider(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/riders/{riderId}", "getRider",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getRider(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/riders/{riderId}", "updateRider",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateRider(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/riders/{riderId}", "deleteRider",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteRider(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/tables", "listTables",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return listTables(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/tables", "createTable",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return createTable(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("GET", "/api/v1/restaurant/tables/{tableId}", "getTable",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return getTable(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("PATCH", "/api/v1/restaurant/tables/{tableId}", "updateTable",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return updateTable(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("DELETE", "/api/v1/restaurant/tables/{tableId}", "deleteTable",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return deleteTable(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
        pm->RegisterHandler("POST", "/api/v1/restaurant/tables/{tableId}/seat", "seatTable",
            [this](const RequestContext& ctx, const std::string& m, const std::string& p, const std::string& b) {
                return seatTable(ctx, m, p, b);
            }, GetName(), kStubHandlerPriority);
    }

public:
    ~RestaurantPlugin() override = default;

    std::string GetName() override { return "Restaurant"; }

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

#endif // RESTAURANT_PLUGIN_HPP
