/**
 * @file       commerce_pos_handlers.cpp
 * @brief      POS override handler implementations for the commerce plugin
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * init_commerce_pos_overrides(pm, locator) — called from hand-written
 * commerce_plugin_impl.cpp (CommercePluginImpl::Initialize). Resolves the
 * storage engine and registers the POS order-create handler (orders_create)
 * at kOverrideHandlerPriority (200), superseding the generated CRUD stub
 * (priority 0) with the D-01 menu-authoritative total recompute, the D-02
 * product_id reference check, and the HANDLER-06 handler-level auth check.
 *
 * Route note: the commerce spec paths carry no /commerce segment (unlike
 * restaurant) — the orders route is POST /api/v1/orders (commerce_openapi.json,
 * the generated CommercePlugin, and the regenerated dart-dio client all agree).
 * The override is registered at that exact route key so the priority-200
 * handler supersedes the priority-0 stub per PluginManager semantics
 * (overrides are keyed by "METHOD /path").
 */

#include "commerce/commerce_pos_handlers.hpp"
#include "singleton/PluginRegistration.hpp"
#include "singleton/IServiceLocator.hpp"
#include "singleton/fnv1a.hpp"
#include "singleton/PluginManager.hpp"
#include "storage/IStorageEngine.hpp"
#include "storage/KeyBuilder.hpp"
#include "commerce/generated/model/OrderCreate.h"
#include "nlohmann/json.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <cmath>
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

static constexpr unsigned int kUuidHexLength = 32;   ///< Stored ids are 32 hex chars, no hyphens
static constexpr uint8_t      kHexDigitMax   = 15;
static constexpr const char*  kHexChars      = "0123456789abcdef";

/// Generate a random UUID (32 hex characters, no hyphens)
static std::string GenerateUuid() noexcept
{
    static thread_local std::mt19937 rng(std::random_device{}());
    static thread_local std::uniform_int_distribution<uint8_t> dist(0, kHexDigitMax);

    std::string result;
    result.reserve(kUuidHexLength);
    for (unsigned int i = 0; i < kUuidHexLength; ++i)
    {
        result += kHexChars[dist(rng)];
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

/**
 * @brief      Check that an int64 money amount fits the int32 Money.amount range
 *
 * D-01 overflow guard: every recomputed amount must be representable as the
 * contract's int32 minor units before it is written back.
 *
 * @param      value  Computed amount (int64 intermediate)
 *
 * @return     true when value fits int32_t
 */
static bool FitsInInt32(int64_t value) noexcept
{
    return (value >= static_cast<int64_t>(std::numeric_limits<int32_t>::min())) &&
           (value <= static_cast<int64_t>(std::numeric_limits<int32_t>::max()));
}

// ============================================================================
// Order Create Handler
// ============================================================================

/**
 * @brief      Create an order with menu-authoritative totals (D-01 recompute)
 *
 * Never trusts client-sent unit_price/line_total/subtotal/total. A body with
 * an absent or empty lines array is rejected up front (no server-derived
 * pricing would exist). Per line, the
 * unit price is resolved by point-Get from restaurant/menu-items (shared
 * storage engine); unknown product_id is rejected with INVALID_REFERENCE (D-02)
 * and a line currency that differs from the stored item's currency is rejected.
 * lineTotal = llround(item price amount x quantity) — the single rounding point
 * (half-up; prices and quantities are non-negative) — then
 * subtotal = Sum(lineTotal), total = subtotal + tax + tip - discount, all in
 * int64 with an int32 overflow guard. The client's required total must equal
 * the recomputed total exactly (no epsilon) or the order is rejected with
 * TOTAL_MISMATCH and nothing is persisted. Persisted documents get
 * server-recomputed money plus id/tenant/organization/timestamps stamped from
 * the request context (D-03 body-stamp tenancy, overwriting client values).
 *
 * @param      ctx   Request context (auth, tenant, organization)
 * @param      body  Raw JSON request body (OrderCreate)
 *
 * @return     JSON document echo, or error envelope
 */
static std::string orders_create(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& body)
{
    // HANDLER-06 defense-in-depth auth check (behind the main.cpp JWT middleware)
    if (ctx.userId.empty())
    {
        return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    }

    json requestData;
    org::openapitools::server::model::OrderCreate dto;
    int64_t subtotal = 0;
    try
    {
        // Parse + contract-validate: model from_json throws out_of_range
        // (missing required field) and type_error (mistyped field) — the json
        // base exception covers both (Pitfall 5)
        requestData = json::parse(body);
        dto = requestData.get<org::openapitools::server::model::OrderCreate>();

        // D-01: an order with no lines has no server-derived pricing — the
        // generated model treats lines as optional, so reject an unset or
        // empty lines array before any money math (nothing persists)
        if (!dto.linesIsSet() || dto.getLines().empty())
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Order must contain at least one line"}})";
        }

        const auto& lines = dto.getLines();
        for (size_t i = 0; i < lines.size(); ++i)
        {
            // Quantities must be positive before any money math — a negative
            // quantity would persist negative money (a refund document) and a
            // zero quantity persists zero-priced lines; refund flows are out
            // of phase scope, so this path must never produce either
            if (lines[i].getQuantity() <= 0.0)
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Order line quantity must be positive"}})";
            }

            // D-02: resolve the product from storage before anything is computed
            auto itemKeyResult = KeyBuilder::Build("restaurant", "menu-items", lines[i].getProductId());
            if (!itemKeyResult.has_value())
            {
                return R"({"error":{"code":"INVALID_KEY","message":"Failed to build menu item key"}})";
            }
            std::string itemDoc;
            if (!s_storage->Get(itemKeyResult.value(), itemDoc))
            {
                return R"({"error":{"code":"INVALID_REFERENCE","message":"Unknown product_id in order line"}})";
            }

            // Corrupt stored rows surface through the shared json-exception
            // discipline below (INVALID_REQUEST)
            const json item = json::parse(itemDoc);
            const json itemPrice = item.at("price");
            const std::string itemCurrency = itemPrice.at("currency").get<std::string>();
            const int64_t itemPriceAmount = static_cast<int64_t>(itemPrice.at("amount").get<int32_t>());

            // Currency agreement: the line must be priced in the item's currency
            if (lines[i].getUnitPrice().getCurrency() != itemCurrency)
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Currency mismatch between order line and menu item"}})";
            }

            // The order-level (total/subtotal) currency is client-controlled —
            // pin it to the item currency too, or a USD-priced order could be
            // persisted with EUR/JPY-labeled money (mixed-currency document)
            if (dto.getTotal().getCurrency() != itemCurrency)
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Order total currency must match the menu item currency"}})";
            }

            // D-01 recompute — llround is the ONLY rounding point (half-up;
            // prices and quantities are non-negative), int64 intermediates
            const int64_t lineTotal =
                static_cast<int64_t>(std::llround(static_cast<double>(itemPriceAmount) * lines[i].getQuantity()));
            if (!FitsInInt32(lineTotal) || !FitsInInt32(subtotal + lineTotal))
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Amount overflow"}})";
            }
            subtotal += lineTotal;

            // Server-authoritative overwrite before persistence (client money
            // is never stored as-is)
            requestData["lines"][i]["unit_price"]["amount"]   = static_cast<int32_t>(itemPriceAmount);
            requestData["lines"][i]["unit_price"]["currency"] = itemCurrency;
            requestData["lines"][i]["line_total"]["amount"]   = static_cast<int32_t>(lineTotal);
            requestData["lines"][i]["line_total"]["currency"] = itemCurrency;
        }
    }
    catch (const json::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid order request body"}})";
    }

    // Optional client money contributes zero when unset (exact int64 arithmetic)
    const int64_t taxTotal      = dto.taxTotalIsSet()      ? static_cast<int64_t>(dto.getTaxTotal().getAmount())      : 0;
    const int64_t tipTotal      = dto.tipTotalIsSet()      ? static_cast<int64_t>(dto.getTipTotal().getAmount())      : 0;
    const int64_t discountTotal = dto.discountTotalIsSet() ? static_cast<int64_t>(dto.getDiscountTotal().getAmount()) : 0;

    if (!FitsInInt32(subtotal))
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Amount overflow"}})";
    }
    const int64_t total = subtotal + taxTotal + tipTotal - discountTotal;
    if (!FitsInInt32(total))
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Amount overflow"}})";
    }

    // D-01 exact-equality mismatch check — no epsilon, nothing persisted on failure
    const int64_t clientTotal = static_cast<int64_t>(dto.getTotal().getAmount());
    if (total != clientTotal)
    {
        return R"({"error":{"code":"TOTAL_MISMATCH","message":"Client total does not match server-recomputed total"}})";
    }

    // Persist the stamped, server-authoritative document
    const std::string id = GenerateUuid();
    const std::string orderCurrency = dto.getTotal().getCurrency();
    requestData["subtotal"]["amount"]   = static_cast<int32_t>(subtotal);
    requestData["subtotal"]["currency"] = orderCurrency;
    requestData["total"]["amount"]      = static_cast<int32_t>(total);
    requestData["total"]["currency"]    = orderCurrency;
    requestData["id"]            = id;
    requestData["tenant_id"]     = ctx.tenantId;
    requestData["organization_id"] = ctx.organizationId;
    requestData["created_at"]    = GetCurrentTimestamp();
    requestData["updated_at"]    = requestData["created_at"];

    auto keyResult = KeyBuilder::Build("commerce", "orders", id);
    if (!keyResult.has_value())
    {
        return R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})";
    }
    if (!s_storage->Put(keyResult.value(), requestData.dump()))
    {
        return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to store entity"}})";
    }

    SPDLOG_INFO("Order created: {}", id);
    return requestData.dump();
}

// ============================================================================
// init_commerce_pos_overrides — called from CommercePluginImpl::Initialize()
// ============================================================================

/**
 * @brief      Resolve storage and register the POS order-create override handler
 *
 * Registers POST /api/v1/orders at kOverrideHandlerPriority (200), superseding
 * the generated createOrder stub (priority 0). The commerce spec paths carry
 * no /commerce segment, so /api/v1/orders is both the generated stub's route
 * and the route the regenerated dart-dio clients call. Owner name is
 * "Commerce" (the plugin's GetName()).
 *
 * @param      pm       PluginManager from service locator
 * @param      locator  Service locator for StorageEngine
 */
void init_commerce_pos_overrides(PluginManager* pm, IServiceLocator& locator)
{
    s_storage = locator.GetService<IStorageEngine>(Fnv1a("StorageEngine"));
    if (!s_storage)
    {
        return;
    }

    SPDLOG_INFO("Registering commerce POS order-create override handler");
    pm->RegisterHandler("POST", "/api/v1/orders", "orders_create", orders_create, "Commerce", kOverrideHandlerPriority);
}
