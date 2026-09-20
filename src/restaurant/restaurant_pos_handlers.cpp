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
 *
 * Phase 3.1 adds the table handlers (tables_list/tables_create/tables_get/
 * tables_seat/tables_update/tables_delete) on the same identity pattern, with strict
 * key-set write contracts (T-03.1-04) and server-owned lifecycle transitions
 * — pos_status/open_order_ids/guest_count/server_id/opened_at only ever move
 * through server events (create-default/seat/order/bus), never client input.
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
#include "restaurant/generated/model/SeatTable_request.h"
#include "restaurant/generated/model/TableCreate.h"
#include "restaurant/generated/model/TableUpdate.h"
#include "nlohmann/json.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <vector>

using json = nlohmann::json;
using namespace gnus::hash;

static IStorageEngine* s_storage = nullptr;

// ============================================================================
// Helpers
// ============================================================================

static constexpr unsigned int kDefaultListLimit = 50;   ///< Matches generated kDefaultPaginationLimit + Dart client default
static constexpr unsigned int kUuidHexLength    = 32;   ///< Stored ids are 32 hex chars, no hyphens
static constexpr int          kHexDigitMax      = 15;   ///< Largest index into kHexChars
static constexpr const char*  kHexChars         = "0123456789abcdef";

/// Generate a random UUID (32 hex characters, no hyphens)
///
/// Every nibble is drawn from std::random_device — the CSPRNG-backed source —
/// instead of std::mt19937, whose 19937-bit state is recoverable from ~20
/// observed ids, after which every future id minted on that thread (orders,
/// items, tickets, across all tenants) would be predictable. The nibble
/// index uses std::uniform_int_distribution<int> — a type on the standard's
/// supported list — and is cast to size_t for indexing (IN-05).
static std::string GenerateUuid() noexcept
{
    static thread_local std::random_device randomDevice;
    static thread_local std::uniform_int_distribution<int> dist(0, kHexDigitMax);

    std::string result;
    result.reserve(kUuidHexLength);
    for (unsigned int i = 0; i < kUuidHexLength; ++i)
    {
        result += kHexChars[static_cast<size_t>(dist(randomDevice))];
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

/// Literal prefix shared by every tables route (GET/POST collection,
/// GET/PATCH by-id, and the /seat sub-route)
static constexpr const char* kTablesRoutePrefix = "/api/v1/restaurant/tables/";

/**
 * @brief      Extract the table id segment from a tables route path
 *
 * The dispatch layer substitutes the REAL id into the path (e.g.
 * /api/v1/restaurant/tables/<id>/seat), so the generated stubs'
 * rfind(kPathSeparator) trick would grab "seat" on the seat route. This
 * helper anchors on the literal tables prefix and takes the segment after it
 * up to the next '/' or the string end.
 *
 * @param      urlPath  Dispatched URL path with the real id substituted
 *
 * @return     The table id segment, or "" when the prefix is absent
 */
static std::string TableIdFromPath(const std::string& urlPath)
{
    const size_t prefixPos = urlPath.find(kTablesRoutePrefix);
    if (prefixPos == std::string::npos)
    {
        return std::string();
    }
    const size_t idStart = prefixPos + std::char_traits<char>::length(kTablesRoutePrefix);
    const size_t nextSlash = urlPath.find('/', idStart);
    return (nextSlash == std::string::npos)
        ? urlPath.substr(idStart)
        : urlPath.substr(idStart, nextSlash - idStart);
}

/**
 * @brief      Check that every body key is within the allowed key set
 *
 * Strict key-set posture for table writes (T-03.1-04): table documents carry
 * server-owned lifecycle fields (pos_status, guest_count, server_id,
 * opened_at, open_order_ids) that must never be client-writable, so a body
 * carrying ANY key outside the write contract is rejected outright instead of
 * riding along silently (Phase 2 creates store raw bodies; tables deviate
 * deliberately — a silent no-op on unknown keys would mask client bugs).
 *
 * @param      body         Parsed request body (must be a JSON object)
 * @param      allowedKeys  The exact write-contract key set
 *
 * @return     true when every body key is in allowedKeys
 */
static bool BodyKeysWithin(const json& body, const std::vector<std::string>& allowedKeys)
{
    for (const auto& item : body.items())
    {
        bool allowed = false;
        for (const std::string& allowedKey : allowedKeys)
        {
            if (item.key() == allowedKey)
            {
                allowed = true;
                break;
            }
        }
        if (!allowed)
        {
            return false;
        }
    }
    return true;
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

/**
 * @brief      List kitchen tickets with keyset cursor paging
 *
 * Supersedes the generated dump-all kitchen-tickets list stub so ticket
 * reads are tenant-filtered like the writes this phase added (Codex P1 —
 * same WR-02 class as the orders list).
 *
 * @param      ctx    Request context (auth, tenant, query string)
 *
 * @return     JSON envelope string, or error envelope
 */
static std::string kitchen_tickets_list(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& /*body*/)
{
    return list_entity(ctx, "kitchen-tickets");
}

/**
 * @brief      List tables with keyset cursor paging (TBL-03)
 *
 * One-line delegation to the shared list core: supersedes the generated
 * listTables stub, which scanned every tenant's tables with no filter and no
 * paging. The shared core gives tables the D-03 tenant filter, D-07/D-09
 * keyset paging, the HANDLER-06 auth check, and corrupt-row skipping.
 *
 * @param      ctx    Request context (auth, tenant, query string)
 *
 * @return     JSON envelope string, or error envelope
 */
static std::string tables_list(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& /*body*/)
{
    return list_entity(ctx, "tables");
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
        // (missing required field) or json::type_error (mistyped field), and
        // validate() throws ValidationException on contract constraints the
        // parse does not enforce; both map to INVALID_REQUEST below
        json requestData = json::parse(body);
        const org::openapitools::server::model::MenuCategoryCreate dto =
            requestData.get<org::openapitools::server::model::MenuCategoryCreate>();
        dto.validate();

        return stamp_and_persist(ctx, "menu-categories", requestData);
    }
    catch (const std::exception&)
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
        // Strict contract validation — same discipline as above, plus the
        // generated contract constraints via validate()
        json requestData = json::parse(body);
        const org::openapitools::server::model::MenuItemCreate dto =
            requestData.get<org::openapitools::server::model::MenuItemCreate>();
        dto.validate();

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
    catch (const std::exception&)
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
 * minting or lenient backfill of modifier audit fields. Each embedded
 * modifier's tenant_id/organization_id is rewritten from the request context
 * — the same D-03 server-authoritative overwrite the group document gets —
 * so a stored group can never claim a tenant its modifiers contradict
 * (IN-01); every other modifier field keeps its parsed client value. The
 * group document is stored as parsed, modifiers inline, so the list handler
 * embeds them for free.
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
        // Modifier array, plus the generated contract constraints via
        // validate()
        json requestData = json::parse(body);
        const org::openapitools::server::model::ModifierGroupCreate dto =
            requestData.get<org::openapitools::server::model::ModifierGroupCreate>();
        dto.validate();

        // D-03 tenancy for inline modifiers (IN-01): the group body-stamp
        // below overwrites client tenant values, so the embedded modifiers
        // get the same overwrite — otherwise a stored group could claim
        // tenant "default" while its modifiers claim another tenant. Only
        // the two tenancy fields are rewritten; id/created_at/updated_at/
        // name/price_delta keep their parsed client values (D-08 untouched).
        if (requestData.contains("modifiers"))
        {
            for (auto& modifier : requestData["modifiers"])
            {
                modifier["tenant_id"]       = ctx.tenantId;
                modifier["organization_id"] = ctx.organizationId;
            }
        }

        return stamp_and_persist(ctx, "modifier-groups", requestData);
    }
    catch (const std::exception&)
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
        // Strict contract validation — same discipline as above, plus the
        // generated contract constraints via validate()
        json requestData = json::parse(body);
        const org::openapitools::server::model::KitchenTicketCreate dto =
            requestData.get<org::openapitools::server::model::KitchenTicketCreate>();
        dto.validate();

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
    catch (const std::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid kitchen ticket request body"}})";
    }
}

/**
 * @brief      Create a table (TBL-03) with server-defaulted lifecycle fields
 *
 * Parses and validates the body against the generated TableCreate contract
 * (from_json requires name, capacity, and status; optional section/asset_id/
 * metadata) and enforces the strict key-set posture — a body carrying any key
 * outside {name, section, capacity, status, asset_id, metadata} is rejected
 * INVALID_REQUEST with nothing persisted (T-03.1-04: pos_status, guest_count,
 * server_id, opened_at, and open_order_ids are server-owned lifecycle fields;
 * the generated passthrough would store whatever keys arrive). Before the
 * shared stamp-and-persist tail, the lifecycle fields are server-defaulted —
 * pos_status "empty" and open_order_ids [] — so every readable table is a
 * complete document (dart Table models require the fields; without the
 * defaults every read row would carry dart nulls).
 *
 * @param      ctx    Request context (auth, tenant, organization)
 * @param      body   Raw JSON request body (TableCreate)
 *
 * @return     JSON document echo, or error envelope
 */
static std::string tables_create(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& body)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    try
    {
        json requestData = json::parse(body);

        // Strict key-set posture — lifecycle fields are never client-writable
        if (!BodyKeysWithin(requestData, {"name", "section", "capacity", "status", "asset_id", "metadata"}))
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Table create body contains keys outside the contract"}})";
        }

        // Strict contract validation — from_json throws json::out_of_range
        // (missing required field) or json::type_error (mistyped field), and
        // validate() throws ValidationException on contract constraints the
        // parse does not enforce; both map to INVALID_REQUEST below
        const org::openapitools::server::model::TableCreate dto =
            requestData.get<org::openapitools::server::model::TableCreate>();
        dto.validate();

        // Server-defaulted lifecycle fields — a freshly created table starts
        // empty with no open checks; only server events (seat/order/bus) ever
        // move these (the key-set rejection above means the defaults always
        // apply; the contains guards keep the rule explicit and local)
        if (!requestData.contains("pos_status"))
        {
            requestData["pos_status"] = "empty";
        }
        if (!requestData.contains("open_order_ids"))
        {
            requestData["open_order_ids"] = json::array();
        }

        return stamp_and_persist(ctx, "tables", requestData);
    }
    catch (const std::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid table request body"}})";
    }
}

// ============================================================================
// Table Lifecycle Handlers
// ============================================================================

/**
 * @brief      Get a single table (TBL-03) with the tenant boundary
 *
 * Supersedes the generated getTable stub, which returned any stored table to
 * any caller with no tenant check. A row belonging to another tenant returns
 * the same NOT_FOUND envelope as a missing id — cross-tenant reads must not
 * be distinguishable from missing (T-03.1-05).
 *
 * @param      ctx      Request context (auth, tenant)
 * @param      urlPath  Dispatched URL path carrying the real table id
 *
 * @return     JSON document, or error envelope
 */
static std::string tables_get(const RequestContext& ctx, const std::string& /*method*/, const std::string& urlPath, const std::string& /*body*/)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    const std::string id = TableIdFromPath(urlPath);
    if (id.empty())
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid table id"}})";
    }

    auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
    if (!keyResult.has_value())
    {
        return R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})";
    }

    std::string value;
    if (!s_storage->Get(keyResult.value(), value))
    {
        return R"({"error":{"code":"NOT_FOUND","message":"Table not found"}})";
    }

    try
    {
        json item = json::parse(value);

        // D-03 tenant check — an other-tenant row is indistinguishable from
        // missing (legacy rows without tenant_id count as "default")
        if (item.value("tenant_id", "default") != ctx.tenantId)
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Table not found"}})";
        }

        // Backfill multi-tenant fields for records created before tenant
        // stamping (matches generated get behavior)
        if (!item.contains("tenant_id"))
        {
            item["tenant_id"] = "default";
        }
        if (!item.contains("organization_id"))
        {
            item["organization_id"] = "default";
        }
        return item.dump();
    }
    catch (const json::exception&)
    {
        // A corrupt stored row surfaces as a bad request, never a crash
        return R"({"error":{"code":"INVALID_REQUEST","message":"Stored table document is corrupt"}})";
    }
}

/**
 * @brief      Seat a table — persist the SEAT event (TBL-04)
 *
 * Supersedes the generated seatTable stub, which answered INVALID_PATH (a
 * dead stub). The body is validated against the generated SeatTable_request
 * contract under the strict key-set posture {party_size, customer_id,
 * booking_id} and party_size must be at least 1. The SEAT event then moves
 * every lifecycle field server-side:
 *
 * - guest_count  = the request's party_size
 * - server_id    = ctx.userId (JWT-verified caller — never a body field,
 *                  T-03.1-06)
 * - opened_at    = server now() ONLY when the stored doc has no non-null
 *                  opened_at (first seat — preserves elapsed dining time on a
 *                  mid-meal party-size change)
 * - pos_status   = "seated" ONLY when the current value is absent, null, or
 *                  "empty" (never regresses order_placed/order_served on a
 *                  party change mid-meal)
 * - status       = "occupied" always
 *
 * customer_id/booking_id are parsed and validated for contract compliance but
 * deliberately NOT persisted — the Table contract has no home for them.
 *
 * @param      ctx      Request context (auth, tenant)
 * @param      urlPath  Dispatched URL path carrying the real table id
 * @param      body     Raw JSON request body (SeatTable_request)
 *
 * @return     Updated JSON document, or error envelope
 */
static std::string tables_seat(const RequestContext& ctx, const std::string& /*method*/, const std::string& urlPath, const std::string& body)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    const std::string id = TableIdFromPath(urlPath);
    if (id.empty())
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid table id"}})";
    }

    auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
    if (!keyResult.has_value())
    {
        return R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})";
    }
    const std::string key = keyResult.value();

    std::string value;
    if (!s_storage->Get(key, value))
    {
        return R"({"error":{"code":"NOT_FOUND","message":"Table not found"}})";
    }

    try
    {
        json doc = json::parse(value);

        // D-03 tenant check — an other-tenant row is indistinguishable from
        // missing (legacy rows without tenant_id count as "default")
        if (doc.value("tenant_id", "default") != ctx.tenantId)
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Table not found"}})";
        }

        // Strict key-set posture — the seating event accepts exactly three keys
        json requestBody = json::parse(body);
        if (!BodyKeysWithin(requestBody, {"party_size", "customer_id", "booking_id"}))
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Seat table body contains keys outside the contract"}})";
        }
        const org::openapitools::server::model::SeatTable_request dto =
            requestBody.get<org::openapitools::server::model::SeatTable_request>();
        dto.validate();
        if (dto.getPartySize() < 1)
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Party size must be at least 1"}})";
        }

        // SEAT event rules (TBL-04) — server-computed lifecycle only
        doc["guest_count"] = dto.getPartySize();
        doc["server_id"]   = ctx.userId;

        // opened_at only on the first seat (absent or null stored value)
        const json storedOpenedAt = doc.value("opened_at", json());
        if (storedOpenedAt.is_null())
        {
            doc["opened_at"] = GetCurrentTimestamp();
        }

        // pos_status advances only from empty — an order_placed/order_served
        // table keeps its progress through a party-size change
        const json storedPosStatus = doc.value("pos_status", json());
        if (storedPosStatus.is_null() ||
            (storedPosStatus.is_string() && storedPosStatus.get<std::string>() == "empty"))
        {
            doc["pos_status"] = "seated";
        }

        doc["status"]     = "occupied";
        doc["updated_at"] = GetCurrentTimestamp();

        if (!s_storage->Put(key, doc.dump()))
        {
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to store entity"}})";
        }
        return doc.dump();
    }
    catch (const std::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid seat table request body"}})";
    }
}

/**
 * @brief      Update a table — contract-strict fields + BUS/RESET (TBL-05)
 *
 * Supersedes the generated updateTable stub, which raw-merged ANY body keys
 * into the stored doc (a client could overwrite pos_status/open_order_ids/
 * guest_count directly). The body is validated against the generated
 * TableUpdate contract under the strict key-set posture {name, section,
 * capacity, status, asset_id, metadata}; only the six contract keys present
 * in the body are applied to the stored document (metadata replaces
 * wholesale, matching the generated top-level merge semantics).
 *
 * BUS/RESET compensating event: when the body sets status to "available" or
 * "dirty" the seating lifecycle resets — pos_status "empty", guest_count/
 * server_id/opened_at null, open_order_ids []. The spec has no dedicated
 * unseat route, so the status patch is its vehicle (touch-pos D-04). The
 * reset is unconditional by design (minimal v1; a guard against bussing
 * tables with open checks is a one-line follow-up if a consumer needs it).
 *
 * @param      ctx      Request context (auth, tenant)
 * @param      urlPath  Dispatched URL path carrying the real table id
 * @param      body     Raw JSON request body (TableUpdate)
 *
 * @return     Updated JSON document, or error envelope
 */
static std::string tables_update(const RequestContext& ctx, const std::string& /*method*/, const std::string& urlPath, const std::string& body)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    const std::string id = TableIdFromPath(urlPath);
    if (id.empty())
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid table id"}})";
    }

    auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
    if (!keyResult.has_value())
    {
        return R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})";
    }
    const std::string key = keyResult.value();

    std::string value;
    if (!s_storage->Get(key, value))
    {
        return R"({"error":{"code":"NOT_FOUND","message":"Table not found"}})";
    }

    try
    {
        json stored = json::parse(value);

        // D-03 tenant check — an other-tenant row is indistinguishable from
        // missing (legacy rows without tenant_id count as "default")
        if (stored.value("tenant_id", "default") != ctx.tenantId)
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Table not found"}})";
        }

        // Strict key-set posture — lifecycle fields are never client-writable
        json requestBody = json::parse(body);
        if (!BodyKeysWithin(requestBody, {"name", "section", "capacity", "status", "asset_id", "metadata"}))
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Table update body contains keys outside the contract"}})";
        }
        const org::openapitools::server::model::TableUpdate dto =
            requestBody.get<org::openapitools::server::model::TableUpdate>();
        dto.validate();

        // Apply only the contract keys present in the body
        static const std::vector<std::string> kTableContractKeys = {
            "name", "section", "capacity", "status", "asset_id", "metadata"};
        for (const std::string& contractKey : kTableContractKeys)
        {
            if (requestBody.contains(contractKey))
            {
                stored[contractKey] = requestBody[contractKey];
            }
        }

        // BUS/RESET — the body setting status to available/dirty is the
        // compensating unseat event; the seating lifecycle returns to empty
        const std::string bodyStatus = requestBody.value("status", std::string());
        if (bodyStatus == "available" || bodyStatus == "dirty")
        {
            stored["pos_status"]     = "empty";
            stored["guest_count"]    = nullptr;
            stored["server_id"]      = nullptr;
            stored["opened_at"]      = nullptr;
            stored["open_order_ids"] = json::array();
        }

        stored["updated_at"] = GetCurrentTimestamp();

        if (!s_storage->Put(key, stored.dump()))
        {
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to update entity"}})";
        }
        return stored.dump();
    }
    catch (const std::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid table update request body"}})";
    }
}

/**
 * @brief      Delete a table with the tenant boundary (CR-01)
 *
 * Supersedes the generated deleteTable stub, which ignored the request
 * context entirely and deleted ANY tenant's row by id (storage keys carry no
 * tenant segment, so this handler-level check is the isolation barrier for
 * the by-id routes). The stored row is read and tenant-checked exactly as
 * tables_get does: a missing row OR a row belonging to another tenant
 * returns the same NOT_FOUND envelope — cross-tenant deletes are
 * indistinguishable from missing (T-03.1-05). Only after the boundary
 * passes is the row deleted through the storage engine, returning the
 * generated stub contract's success shape. Straight tenant-checked delete
 * only — open-check/lifecycle guards are deliberately out of scope.
 *
 * @param      ctx      Request context (auth, tenant)
 * @param      urlPath  Dispatched URL path carrying the real table id
 *
 * @return     {"deleted":true}, or error envelope
 */
static std::string tables_delete(const RequestContext& ctx, const std::string& /*method*/, const std::string& urlPath, const std::string& /*body*/)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    const std::string id = TableIdFromPath(urlPath);
    if (id.empty())
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid table id"}})";
    }

    auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
    if (!keyResult.has_value())
    {
        return R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})";
    }

    std::string value;
    if (!s_storage->Get(keyResult.value(), value))
    {
        return R"({"error":{"code":"NOT_FOUND","message":"Table not found"}})";
    }

    try
    {
        const json item = json::parse(value);

        // D-03 tenant check — an other-tenant row is indistinguishable from
        // missing (legacy rows without tenant_id count as "default")
        if (item.value("tenant_id", "default") != ctx.tenantId)
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Table not found"}})";
        }
    }
    catch (const json::exception&)
    {
        // A corrupt stored row surfaces as a bad request, never a crash
        return R"({"error":{"code":"INVALID_REQUEST","message":"Stored table document is corrupt"}})";
    }

    if (!s_storage->Delete(keyResult.value()))
    {
        return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to delete entity"}})";
    }
    return R"({"deleted":true})";
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
 * Also registers the Phase 3.1 table handlers: GET+POST
 * /api/v1/restaurant/tables and GET/PATCH/DELETE
 * /api/v1/restaurant/tables/{tableId}
 * (listTables/createTable/getTable/updateTable/deleteTable), keyed on the
 * exact generated METHOD+path strings so the priority-200 overrides
 * supersede the priority-0 stubs.
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
    pm->RegisterHandler("GET", "/api/v1/restaurant/kitchen-tickets", "list_kitchen_tickets", kitchen_tickets_list, "Restaurant", kOverrideHandlerPriority);

    SPDLOG_INFO("Registering restaurant POS create override handlers");
    pm->RegisterHandler("POST", "/api/v1/restaurant/menu-categories", "menu_categories_create", menu_categories_create, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/restaurant/menu-items", "menu_items_create", menu_items_create, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/restaurant/modifier-groups", "modifier_groups_create", modifier_groups_create, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/restaurant/kitchen-tickets", "kitchen_tickets_create", kitchen_tickets_create, "Restaurant", kOverrideHandlerPriority);

    SPDLOG_INFO("Registering restaurant POS table override handlers");
    pm->RegisterHandler("GET", "/api/v1/restaurant/tables", "tables_list", tables_list, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/restaurant/tables", "tables_create", tables_create, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("GET", "/api/v1/restaurant/tables/{tableId}", "tables_get", tables_get, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/restaurant/tables/{tableId}/seat", "tables_seat", tables_seat, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("PATCH", "/api/v1/restaurant/tables/{tableId}", "tables_update", tables_update, "Restaurant", kOverrideHandlerPriority);
    pm->RegisterHandler("DELETE", "/api/v1/restaurant/tables/{tableId}", "tables_delete", tables_delete, "Restaurant", kOverrideHandlerPriority);
}
