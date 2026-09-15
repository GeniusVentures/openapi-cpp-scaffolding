/**
 * @file       restaurant_pos_handlers.cpp
 * @brief      POS override handler implementations for the restaurant plugin
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * init_restaurant_pos_overrides(pm, locator) — called from hand-written
 * restaurant_plugin_impl.cpp (RestaurantPluginImpl::Initialize). Resolves the
 * storage engine and registers POS list handlers (menu-categories,
 * menu-items, modifier-groups) and create handlers (the same three menu
 * entities plus kitchen-tickets) at kOverrideHandlerPriority (200),
 * superseding the generated stubs (priority 0): the lists get keyset cursor
 * paging, the D-03 tenant filter, and the HANDLER-06 handler-level auth
 * check; the creates get strict Create-model validation, the D-02
 * referential-integrity checks, and D-03 body-stamp tenancy.
 */

#include "restaurant/restaurant_pos_handlers.hpp"
#include "singleton/PluginRegistration.hpp"
#include "singleton/IServiceLocator.hpp"
#include "singleton/fnv1a.hpp"
#include "singleton/PluginManager.hpp"
#include "storage/IStorageEngine.hpp"
#include "storage/KeyBuilder.hpp"
#include "restaurant/generated/model/KitchenTicketCreate.h"
#include "restaurant/generated/model/MenuCategoryCreate.h"
#include "restaurant/generated/model/MenuItemCreate.h"
#include "restaurant/generated/model/ModifierGroupCreate.h"
#include "nlohmann/json.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>

using json = nlohmann::json;
using namespace gnus::hash;

static IStorageEngine* s_storage = nullptr;

// ============================================================================
// Helpers
// ============================================================================

static constexpr unsigned int kDefaultListLimit = 50;   ///< Matches generated kDefaultPaginationLimit + Dart client default
static constexpr unsigned int kUuidHexLength    = 32;   ///< Stored ids are 32 hex chars, no hyphens
static constexpr uint8_t      kHexDigitMax      = 15;
static constexpr const char*  kHexChars         = "0123456789abcdef";

/// Generate a random UUID (32 hex characters, no hyphens)
///
/// Every nibble is drawn from std::random_device — the CSPRNG-backed source —
/// instead of std::mt19937, whose 19937-bit state is recoverable from ~20
/// observed ids, after which every future id minted on that thread (orders,
/// items, tickets, across all tenants) would be predictable.
static std::string GenerateUuid() noexcept
{
    static thread_local std::random_device randomDevice;
    static thread_local std::uniform_int_distribution<uint8_t> dist(0, kHexDigitMax);

    std::string result;
    result.reserve(kUuidHexLength);
    for (unsigned int i = 0; i < kUuidHexLength; ++i)
    {
        result += kHexChars[dist(randomDevice)];
    }
    return result;
}

/// Get current UTC timestamp in ISO 8601 format
static std::string GetCurrentTimestamp() noexcept
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time), "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

/// Parsed list-query parameters (D-06 transport, D-09 limit, D-07 cursor)
struct ListQuery
{
    unsigned long long limit       = kDefaultListLimit;  ///< Effective page size (no clamp — D-09)
    std::string        cursor;                            ///< Last-seen id for keyset paging ("" = first page)
    bool               cursorValid = true;                ///< false → INVALID_REQUEST (D-07)
};

/**
 * @brief      Check that a value is a decimal digit
 *
 * @param      c      Character to test
 *
 * @return     true when c is '0'..'9'
 */
static bool IsDigit(char c) noexcept
{
    return (c >= '0') && (c <= '9');
}

/**
 * @brief      Check that a value is exactly 32 hex characters (any case)
 *
 * @param      value  Candidate cursor/id string
 *
 * @return     true when value is a valid 32-hex id (D-07 cursor posture)
 */
static bool IsValidHexId(const std::string& value) noexcept
{
    if (value.size() != kUuidHexLength)
    {
        return false;
    }
    for (const char c : value)
    {
        const bool isHex = IsDigit(c) ||
                           ((c >= 'a') && (c <= 'f')) ||
                           ((c >= 'A') && (c <= 'F'));
        if (!isHex)
        {
            return false;
        }
    }
    return true;
}

/**
 * @brief      Parse a limit query value
 *
 * D-09: absent, zero, non-numeric, or negative values yield the default 50;
 * any positive integer is honored verbatim with no clamp. Values beyond the
 * unsigned 64-bit range saturate (still no clamp below it).
 *
 * @param      value  Raw query value for the "limit" key
 *
 * @return     Effective limit
 */
static unsigned long long ParseLimitValue(const std::string& value) noexcept
{
    constexpr unsigned long long kDecimalRadix = 10;
    const unsigned long long kCeiling = std::numeric_limits<unsigned long long>::max();

    bool numeric = !value.empty();
    for (const char c : value)
    {
        if (!IsDigit(c))
        {
            numeric = false;
            break;
        }
    }
    if (!numeric)
    {
        return kDefaultListLimit;
    }

    unsigned long long parsed = 0;
    for (const char c : value)
    {
        const unsigned long long digit = static_cast<unsigned long long>(c - '0');
        if (parsed > ((kCeiling - digit) / kDecimalRadix))
        {
            return kCeiling;
        }
        parsed = (parsed * kDecimalRadix) + digit;
    }
    return (parsed == 0) ? kDefaultListLimit : parsed;
}

/**
 * @brief      Parse limit/cursor from the raw query string
 *
 * Manual parsing on '&' and '=' — no new dependencies. The "q", "sort", and
 * "location_id" parameters are ignored (out of scope).
 *
 * @param      queryString  Raw query string after '?' ("" when absent)
 *
 * @return     Parsed ListQuery; cursorValid is false when a non-empty cursor
 *             is not exactly 32 hex characters (D-07 strict posture)
 */
static ListQuery ParseListQuery(const std::string& queryString)
{
    ListQuery query;
    std::string remaining = queryString;
    while (!remaining.empty())
    {
        const size_t ampPos = remaining.find('&');
        const std::string pair = (ampPos != std::string::npos)
            ? remaining.substr(0, ampPos)
            : remaining;
        remaining = (ampPos != std::string::npos)
            ? remaining.substr(ampPos + 1)
            : std::string();

        const size_t eqPos = pair.find('=');
        if (eqPos == std::string::npos)
        {
            continue;
        }
        const std::string key = pair.substr(0, eqPos);
        const std::string value = pair.substr(eqPos + 1);

        if (key == "limit")
        {
            query.limit = ParseLimitValue(value);
        }
        else if (key == "cursor")
        {
            query.cursor = value;
        }
    }

    if (!query.cursor.empty() && !IsValidHexId(query.cursor))
    {
        query.cursorValid = false;
    }
    return query;
}

/**
 * @brief      Shared list core — scan + tenant filter + keyset paging
 *
 * Scans the entity prefix (lexicographic key order), filters rows by
 * ctx.tenantId (missing tenant_id counts as "default", matching the generated
 * backfill — D-03), applies keyset semantics (skip ids not strictly after the
 * cursor), and returns the snake_case paging envelope with next_cursor
 * present only when more tenant-matching items remain. Stored modifier
 * groups already carry their modifiers inline. Corrupt rows are warned and
 * skipped (T-02-04).
 *
 * @param      ctx      Request context (auth, tenant, query string)
 * @param      entity   Entity segment, e.g. "menu-items"
 *
 * @return     JSON envelope string, or error envelope
 */
static std::string list_entity(const RequestContext& ctx, const std::string& entity)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    const ListQuery query = ParseListQuery(ctx.queryString);
    if (!query.cursorValid)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid cursor"}})";
    }

    const std::string prefix = KeyBuilder::MakePrefix("restaurant", entity);
    const auto scanResult = s_storage->Scan(prefix);

    json data = json::array();
    std::string lastId;
    bool hasMore = false;

    for (const auto& [key, value] : scanResult)
    {
        json item;
        try
        {
            item = json::parse(value);
            // D-03 tenant filter — legacy rows without tenant_id count as "default"
            if (item.value("tenant_id", "default") != ctx.tenantId)
            {
                continue;
            }
        }
        catch (const json::exception&)
        {
            SPDLOG_WARN("Skipping corrupt restaurant/{} row at key {}", entity, key);
            continue;
        }

        // Keyset paging — Scan is lexicographic, so ids after the cursor are
        // exactly the keys comparing greater than it
        const std::string id = key.substr(key.rfind('/') + 1);
        if (!query.cursor.empty() && id.compare(query.cursor) <= 0)
        {
            continue;
        }

        if (data.size() >= query.limit)
        {
            hasMore = true;
            break;
        }

        // Backfill multi-tenant fields for records created before tenant
        // stamping (matches generated list behavior)
        if (!item.contains("tenant_id"))
        {
            item["tenant_id"] = "default";
        }
        if (!item.contains("organization_id"))
        {
            item["organization_id"] = "default";
        }

        data.push_back(item);
        lastId = id;
    }

    json response;
    response["data"] = data;
    json pagination;
    pagination["limit"] = query.limit;
    pagination["has_more"] = hasMore;
    if (hasMore)
    {
        pagination["next_cursor"] = lastId;
    }
    response["pagination"] = pagination;
    return response.dump();
}

// ============================================================================
// List Handlers
// ============================================================================

/**
 * @brief      List menu categories with keyset cursor paging
 *
 * @param      ctx    Request context (auth, tenant, query string)
 *
 * @return     JSON envelope string, or error envelope
 */
static std::string menu_categories_list(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& /*body*/)
{
    return list_entity(ctx, "menu-categories");
}

/**
 * @brief      List menu items with keyset cursor paging
 *
 * @param      ctx    Request context (auth, tenant, query string)
 *
 * @return     JSON envelope string, or error envelope
 */
static std::string menu_items_list(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& /*body*/)
{
    return list_entity(ctx, "menu-items");
}

/**
 * @brief      List modifier groups (modifiers inline in stored documents)
 *
 * @param      ctx    Request context (auth, tenant, query string)
 *
 * @return     JSON envelope string, or error envelope
 */
static std::string modifier_groups_list(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& /*body*/)
{
    return list_entity(ctx, "modifier-groups");
}

// ============================================================================
// Create Handlers
// ============================================================================

/**
 * @brief      Shared create tail — stamp server-authoritative fields + persist
 *
 * Stamps id (freshly minted 32-hex UUID), tenant_id/organization_id from the
 * request context (D-03 body-stamp — overwriting any client-sent values), and
 * created_at/updated_at (updated_at == created_at on create), then persists
 * the document on the same flat KeyBuilder key the generated handlers use
 * (key layout unchanged — D-03 keeps the generated by-ID routes coherent).
 *
 * @param      ctx           Request context (tenant, organization)
 * @param      entity        Entity segment, e.g. "menu-items"
 * @param[in,out] requestData  Parsed + validated request document
 *
 * @return     JSON document echo, or INVALID_KEY / STORAGE_ERROR envelope
 */
static std::string stamp_and_persist(const RequestContext& ctx, const std::string& entity, json& requestData)
{
    const std::string id = GenerateUuid();
    requestData["id"] = id;
    requestData["tenant_id"] = ctx.tenantId;
    requestData["organization_id"] = ctx.organizationId;
    requestData["created_at"] = GetCurrentTimestamp();
    requestData["updated_at"] = requestData["created_at"];

    auto keyResult = KeyBuilder::Build("restaurant", entity, id);
    if (!keyResult.has_value())
    {
        return R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})";
    }
    if (!s_storage->Put(keyResult.value(), requestData.dump()))
    {
        return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to store entity"}})";
    }
    return requestData.dump();
}

/**
 * @brief      Create a menu category (HANDLER-03)
 *
 * Parses and validates the body against the generated MenuCategoryCreate
 * contract (from_json requires menu_id and name; optional sort_order/status)
 * before anything is stamped or persisted. menu_id is deliberately NOT
 * integrity-checked (D-02 enumerates exactly three reference checks — this
 * is not one of them).
 *
 * @param      ctx   Request context (auth, tenant, organization)
 * @param      body  Raw JSON request body (MenuCategoryCreate)
 *
 * @return     JSON document echo, or error envelope
 */
static std::string menu_categories_create(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& body)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    try
    {
        // Strict contract validation — from_json throws json::out_of_range
        // (missing required field) or json::type_error (mistyped field); the
        // parsed DTO is not otherwise needed, the parse IS the check
        json requestData = json::parse(body);
        [[maybe_unused]] const org::openapitools::server::model::MenuCategoryCreate dto =
            requestData.get<org::openapitools::server::model::MenuCategoryCreate>();

        return stamp_and_persist(ctx, "menu-categories", requestData);
    }
    catch (const json::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid menu category request body"}})";
    }
}

/**
 * @brief      Create a menu item (HANDLER-03) with D-02 reference integrity
 *
 * Parses and validates the body against the generated MenuItemCreate contract
 * (from_json requires category_id, name, price, and status). When the body
 * carries modifier_group_ids, every entry is point-Get against
 * restaurant/modifier-groups BEFORE any stamping or Put — a dangling
 * reference returns INVALID_REFERENCE and nothing is persisted (D-02).
 * category_id is deliberately NOT integrity-checked (D-02 enumerates exactly
 * three reference checks — this is not one of them).
 *
 * @param      ctx   Request context (auth, tenant, organization)
 * @param      body  Raw JSON request body (MenuItemCreate)
 *
 * @return     JSON document echo, or error envelope
 */
static std::string menu_items_create(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& body)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    try
    {
        // Strict contract validation — same json::exception discipline as above
        json requestData = json::parse(body);
        const org::openapitools::server::model::MenuItemCreate dto =
            requestData.get<org::openapitools::server::model::MenuItemCreate>();

        // D-02 referential integrity: every modifier_group_ids entry must
        // resolve to a stored restaurant/modifier-groups record
        if (dto.modifierGroupIdsIsSet())
        {
            for (const std::string& refId : dto.getModifierGroupIds())
            {
                auto refKeyResult = KeyBuilder::Build("restaurant", "modifier-groups", refId);
                if (!refKeyResult.has_value())
                {
                    return R"({"error":{"code":"INVALID_KEY","message":"Failed to build modifier group key"}})";
                }
                std::string refDoc;
                if (!s_storage->Get(refKeyResult.value(), refDoc))
                {
                    return R"({"error":{"code":"INVALID_REFERENCE","message":"Unknown modifier_group_id reference"}})";
                }

                // D-02 tenant scoping — the referenced group must belong to
                // the caller's tenant (legacy rows without tenant_id count as
                // "default"); corrupt rows throw into the catch below
                const json refItem = json::parse(refDoc);
                if (refItem.value("tenant_id", "default") != ctx.tenantId)
                {
                    return R"({"error":{"code":"INVALID_REFERENCE","message":"modifier_group_id belongs to another tenant"}})";
                }
            }
        }

        return stamp_and_persist(ctx, "menu-items", requestData);
    }
    catch (const json::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid menu item request body"}})";
    }
}

/**
 * @brief      Create a modifier group with inline modifiers (HANDLER-03, D-08)
 *
 * Parses and validates the body against the generated ModifierGroupCreate
 * contract (from_json requires name; optional min_selected/max_selected/
 * required/modifiers). Inline Modifier elements parse strict contract-literal
 * (D-08): EVERY element must carry the full generated Modifier field set —
 * id, tenant_id, organization_id, created_at, updated_at, name, price_delta —
 * or from_json throws into the catch below (INVALID_REQUEST). No server-side
 * minting or lenient backfill of modifier audit fields. The group document is
 * stored as parsed, modifiers inline, so the list handler embeds them for
 * free.
 *
 * @param      ctx   Request context (auth, tenant, organization)
 * @param      body  Raw JSON request body (ModifierGroupCreate)
 *
 * @return     JSON document echo, or error envelope
 */
static std::string modifier_groups_create(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& body)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    try
    {
        // Strict contract-literal validation including the D-08 inline
        // Modifier array — the parsed DTO is not otherwise needed
        json requestData = json::parse(body);
        [[maybe_unused]] const org::openapitools::server::model::ModifierGroupCreate dto =
            requestData.get<org::openapitools::server::model::ModifierGroupCreate>();

        return stamp_and_persist(ctx, "modifier-groups", requestData);
    }
    catch (const json::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid modifier group request body"}})";
    }
}

/**
 * @brief      Create a kitchen ticket (HANDLER-04) with D-02 order reference
 *
 * Parses and validates the body against the generated KitchenTicketCreate
 * contract (from_json requires order_id, station, and status). Before any
 * stamping or Put, the order_id is point-Get against commerce/orders on the
 * shared storage engine — a dangling reference returns INVALID_REFERENCE and
 * nothing is persisted (D-02). The echoed/stored document is contract-shaped
 * KitchenTicket JSON: Create fields plus the five server stamps (id,
 * tenant_id, organization_id, created_at, updated_at), which is exactly the
 * full KitchenTicket model's required set.
 *
 * @param      ctx   Request context (auth, tenant, organization)
 * @param      body  Raw JSON request body (KitchenTicketCreate)
 *
 * @return     JSON document echo, or error envelope
 */
static std::string kitchen_tickets_create(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& body)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    try
    {
        // Strict contract validation — same json::exception discipline as above
        json requestData = json::parse(body);
        const org::openapitools::server::model::KitchenTicketCreate dto =
            requestData.get<org::openapitools::server::model::KitchenTicketCreate>();

        // D-02 referential integrity: the order must exist in commerce/orders
        // before anything is stamped or persisted
        auto orderKeyResult = KeyBuilder::Build("commerce", "orders", dto.getOrderId());
        if (!orderKeyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Failed to build order key"}})";
        }
        std::string orderDoc;
        if (!s_storage->Get(orderKeyResult.value(), orderDoc))
        {
            return R"({"error":{"code":"INVALID_REFERENCE","message":"Unknown order_id reference"}})";
        }

        // D-02 tenant scoping — the referenced order must belong to the
        // caller's tenant (legacy rows without tenant_id count as "default");
        // corrupt rows throw into the catch below
        const json referencedOrder = json::parse(orderDoc);
        if (referencedOrder.value("tenant_id", "default") != ctx.tenantId)
        {
            return R"({"error":{"code":"INVALID_REFERENCE","message":"order_id belongs to another tenant"}})";
        }

        return stamp_and_persist(ctx, "kitchen-tickets", requestData);
    }
    catch (const json::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid kitchen ticket request body"}})";
    }
}

// ============================================================================
// init_restaurant_pos_overrides — called from RestaurantPluginImpl::Initialize()
// ============================================================================

/**
 * @brief      Resolve storage and register the POS override handlers
 *
 * Registers GET menu-categories/menu-items/modifier-groups and POST
 * menu-categories/menu-items/modifier-groups/kitchen-tickets at
 * kOverrideHandlerPriority (200), superseding the generated stubs
 * (priority 0). Owner name is "Restaurant" (the plugin's GetName()).
 * No seed data — Phase 2 ships none.
 *
 * @param      pm       PluginManager from service locator
 * @param      locator  Service locator for StorageEngine
 */
void init_restaurant_pos_overrides(PluginManager* pm, IServiceLocator& locator)
{
    s_storage = locator.GetService<IStorageEngine>(Fnv1a("StorageEngine"));
    if (!s_storage)
    {
        return;
    }

    SPDLOG_INFO("Registering restaurant POS list override handlers");
    pm->RegisterHandler("GET", "/api/v1/restaurant/menu-categories", "list_menu_categories", menu_categories_list, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("GET", "/api/v1/restaurant/menu-items", "list_menu_items", menu_items_list, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("GET", "/api/v1/restaurant/modifier-groups", "list_modifier_groups", modifier_groups_list, "Restaurant", kOverrideHandlerPriority);

    SPDLOG_INFO("Registering restaurant POS create override handlers");
    pm->RegisterHandler("POST", "/api/v1/restaurant/menu-categories", "menu_categories_create", menu_categories_create, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/restaurant/menu-items", "menu_items_create", menu_items_create, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/restaurant/modifier-groups", "modifier_groups_create", modifier_groups_create, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/restaurant/kitchen-tickets", "kitchen_tickets_create", kitchen_tickets_create, "Restaurant", kOverrideHandlerPriority);
}
