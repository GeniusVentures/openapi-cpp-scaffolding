/**
 * @file       commerce_pos_handlers.cpp
 * @brief      POS override handler implementations for the commerce plugin
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * init_commerce_pos_overrides(pm, locator) — called from hand-written
 * commerce_plugin_impl.cpp (CommercePluginImpl::Initialize). Resolves the
 * storage engine and registers the POS order handlers at
 * kOverrideHandlerPriority (200), superseding the generated CRUD stubs
 * (priority 0): orders_create with the D-01 menu-authoritative total
 * recompute, the D-02 product_id reference check, and the HANDLER-06
 * handler-level auth check; orders_list with the D-03 tenant filter,
 * D-07/D-09 keyset paging, and corrupt-row skip.
 *
 * Route note: the commerce spec paths carry no /commerce segment (unlike
 * restaurant) — the orders route is POST /api/v1/orders (commerce_openapi.json,
 * the generated CommercePlugin, and the regenerated dart-dio client all agree).
 * The override is registered at that exact route key so the priority-200
 * handler supersedes the priority-0 stub per PluginManager semantics
 * (overrides are keyed by "METHOD /path").
 *
 * Phase 3.1 adds the TBL-02 table linkage inside orders_create: a set
 * table_id is reference-validated against restaurant/tables (same tenant)
 * before the order persists, and drives the linked table's open_order_ids/
 * pos_status transition after it — server-side events only.
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
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <vector>

using json = nlohmann::json;
using namespace gnus::hash;

static IStorageEngine* s_storage = nullptr;

/// Serializes the linked-table read-modify-write in orders_create's
/// post-persist transition (PR #24 review, P1): two concurrent creates for
/// the same table each appended to a snapshot and the second Put dropped
/// the first order id. File-local by design — only this translation
/// unit's orders_create performs the TBL-02 table transition, so the
/// mutex needs no cross-library visibility.
static std::mutex s_tableAggregateMutex;

// ============================================================================
// Helpers
// ============================================================================

static constexpr unsigned int kUuidHexLength = 32;   ///< Stored ids are 32 hex chars, no hyphens
static constexpr int          kHexDigitMax   = 15;   ///< Largest index into kHexChars
static constexpr const char*  kHexChars      = "0123456789abcdef";
static constexpr unsigned int kDefaultListLimit = 50; ///< Matches generated kDefaultPaginationLimit + Dart client default

/// WR-07: largest line quantity for which price x quantity stays inside
/// std::llround's int64 domain for ANY int32 price (kMaxQuantity squared is
/// below INT64_MAX), so llround never receives an argument whose rounded
/// value it cannot represent — its return value is unspecified there
static constexpr double kMaxQuantity =
    static_cast<double>(std::numeric_limits<int32_t>::max());

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
// Order List Handler
// ============================================================================

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
 * Manual parsing on '&' and '=' — no new dependencies (same helper set as the
 * restaurant list handlers; the duplication is the documented file-local
 * statics pattern).
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
 * @brief      List orders with the D-03 tenant filter and keyset paging
 *
 * Mirrors the restaurant list_entity core: scans the commerce/orders prefix
 * (lexicographic key order), filters rows by ctx.tenantId (missing tenant_id
 * counts as "default", matching the generated backfill — D-03), applies
 * keyset semantics (skip ids not strictly after the cursor), honors D-07
 * (invalid cursor → INVALID_REQUEST) and D-09 (uncapped limit), skips corrupt
 * rows with a warning instead of throwing through Route(), and returns the
 * snake_case paging envelope with next_cursor present only when more
 * tenant-matching orders remain. Replaces the generated priority-0 listOrders
 * stub, which returned every tenant's orders to any authenticated caller.
 *
 * @param      ctx   Request context (auth, tenant, query string)
 *
 * @return     JSON envelope string, or error envelope
 */
static std::string orders_list(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& /*body*/)
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

    const std::string prefix = KeyBuilder::MakePrefix("commerce", "orders");
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
            SPDLOG_WARN("Skipping corrupt commerce/orders row at key {}", key);
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
 * and a line currency that differs from the stored item's currency is rejected,
 * as are adjustment currencies (line tax/discount, order tax/tip/discount)
 * that differ from the item/order currency when set (WR-06).
 * lineTotal = llround(item price amount x quantity) + line tax - line
 * discount — the single rounding point applies to the price x quantity term
 * (half-up; prices and quantities are non-negative) and the folded line
 * adjustments are exact — then
 * subtotal = Sum(lineTotal), total = subtotal + tax + tip - discount, all in
 * int64 with an int32 overflow guard. Adjustment amounts (line tax/discount,
 * order tax/tip/discount) must be non-negative and negative computed
 * line/order totals are rejected — this path never produces negative money
 * (CR-04). Quantity is additionally bounded (kMaxQuantity) so the
 * price x quantity product never leaves llround's int64 domain (WR-07).
 * The client's required total must equal
 * the recomputed total exactly (no epsilon) or the order is rejected with
 * TOTAL_MISMATCH and nothing is persisted. Persisted documents get
 * server-recomputed money plus id/tenant/organization/timestamps stamped from
 * the request context (D-03 body-stamp tenancy, overwriting client values).
 *
 * TBL-02 table linkage (Phase 3.1): when the body's table_id is set and
 * non-empty, the referenced restaurant/tables row is point-Get and
 * tenant-checked BEFORE anything is computed or persisted — a dangling or
 * other-tenant table_id rejects with INVALID_REFERENCE and nothing persists
 * (the D-02 KitchenTicket.order_id precedent extended to Order.table_id).
 * After the order Put succeeds, the linked table transitions server-side:
 * the order id is appended to open_order_ids (deduped) and pos_status
 * becomes "order_placed" unconditionally — a new check on a served table
 * genuinely returns it to order_placed. An explicit JSON null table_id is
 * erased before DTO extraction — null is absent per the published spec
 * language ("absent or null for tableless channels", WR-01) — and an empty
 * string parses and is treated as absent (and erased before persisting,
 * WR-03).
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

    // TBL-02 table-linkage locals — captured during the pre-persist reference
    // validation inside the try, consumed by the post-persist transition. An
    // order whose table_id is unset leaves tableLinked false and this handler
    // never touches the restaurant/tables prefix (tableless flows unchanged).
    json linkedTableDoc;
    std::string linkedTableKey;
    bool tableLinked = false;
    try
    {
        // Parse + contract-validate: model from_json throws out_of_range
        // (missing required field) and type_error (mistyped field) — the json
        // base exception covers both (Pitfall 5)
        requestData = json::parse(body);

        // WR-01: the spec declares table_id "absent or null for tableless
        // channels such as quick order" — treat an explicit JSON null as
        // absent so regenerated clients that serialize unset nullable fields
        // as null parse cleanly (the generated from_json throws
        // type_error.302 on a null string field)
        if (requestData.contains("table_id") && requestData["table_id"].is_null())
        {
            requestData.erase("table_id");
        }

        dto = requestData.get<org::openapitools::server::model::OrderCreate>();

        // Generated contract constraints the parse does not enforce (e.g.
        // Money currency must be exactly 3 characters) — validate() throws
        // ValidationException on failure, caught below as INVALID_REQUEST
        dto.validate();

        // D-01: an order with no lines has no server-derived pricing — the
        // generated model treats lines as optional, so reject an unset or
        // empty lines array before any money math (nothing persists)
        if (!dto.linesIsSet() || dto.getLines().empty())
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Order must contain at least one line"}})";
        }

        // TBL-02: pre-persist table reference validation — the table must
        // exist and belong to the caller's tenant before anything is computed
        // or persisted. A null table_id never reaches this guard (erased
        // before DTO extraction, WR-01); an empty string is absent.
        if (dto.tableIdIsSet() && !dto.getTableId().empty())
        {
            auto tableKeyResult = KeyBuilder::Build("restaurant", "tables", dto.getTableId());
            if (!tableKeyResult.has_value())
            {
                return R"({"error":{"code":"INVALID_KEY","message":"Failed to build table key"}})";
            }
            std::string tableDoc;
            if (!s_storage->Get(tableKeyResult.value(), tableDoc))
            {
                return R"({"error":{"code":"INVALID_REFERENCE","message":"Unknown table_id reference"}})";
            }

            // Corrupt stored rows surface through the shared json-exception
            // discipline below (INVALID_REQUEST)
            linkedTableDoc = json::parse(tableDoc);

            // D-02 tenant scoping — the referenced table must belong to the
            // caller's tenant (legacy rows without tenant_id count as
            // "default")
            if (linkedTableDoc.value("tenant_id", "default") != ctx.tenantId)
            {
                return R"({"error":{"code":"INVALID_REFERENCE","message":"table_id belongs to another tenant"}})";
            }
            linkedTableKey = tableKeyResult.value();
            tableLinked = true;
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

            // WR-07: bound the quantity before the llround product — a huge
            // or non-finite quantity pushes price x quantity past the int64
            // range, where std::llround's return value is unspecified and
            // the FitsInInt32 guard below would only judge that garbage
            // value (business-range enforcement stays with the guard)
            if (!std::isfinite(lines[i].getQuantity()) ||
                lines[i].getQuantity() > kMaxQuantity)
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Order line quantity out of range"}})";
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

            // D-02 tenant scoping — the referenced item must belong to the
            // caller's tenant, or its price/currency would leak cross-tenant
            // into this order (legacy rows without tenant_id count as "default")
            if (item.value("tenant_id", "default") != ctx.tenantId)
            {
                return R"({"error":{"code":"INVALID_REFERENCE","message":"product_id belongs to another tenant"}})";
            }

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

            // WR-06: when set, line adjustment currencies must match the
            // item currency — the generated OrderLine::validate() never
            // reaches these optional fields, so without this check a USD
            // order could persist EUR (or non-3-char) adjustments folded
            // into USD money
            if (lines[i].taxTotalIsSet() &&
                lines[i].getTaxTotal().getCurrency() != itemCurrency)
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Line tax currency must match the menu item currency"}})";
            }
            if (lines[i].discountTotalIsSet() &&
                lines[i].getDiscountTotal().getCurrency() != itemCurrency)
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Line discount currency must match the menu item currency"}})";
            }

            // Line-level client adjustments (WR-03): optional tax_total /
            // discount_total fold into the recomputed line total so the
            // stored document stays internally consistent — the amounts
            // contribute zero when unset (exact int64 arithmetic)
            const int64_t lineTaxTotal = lines[i].taxTotalIsSet()
                ? static_cast<int64_t>(lines[i].getTaxTotal().getAmount())
                : 0;
            const int64_t lineDiscountTotal = lines[i].discountTotalIsSet()
                ? static_cast<int64_t>(lines[i].getDiscountTotal().getAmount())
                : 0;

            // CR-04: adjustments are semantically non-negative money — a
            // negative line tax persists negative money (the refund document
            // this path must never produce) and a negative line discount
            // acts as a hidden surcharge; reject before any arithmetic
            // (unset fields extract 0, which passes)
            if (lineTaxTotal < 0 || lineDiscountTotal < 0)
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Line adjustments must be non-negative"}})";
            }

            // D-01 recompute — llround is the ONLY rounding point (half-up;
            // prices and quantities are non-negative), int64 intermediates:
            // lineTotal = (price x qty) + line tax - line discount
            const int64_t lineTotal =
                static_cast<int64_t>(std::llround(static_cast<double>(itemPriceAmount) * lines[i].getQuantity()))
                + lineTaxTotal - lineDiscountTotal;
            // CR-04: even all-non-negative inputs can drive the fold
            // negative (discount exceeding the price x qty term)
            if (lineTotal < 0)
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Line adjustments exceed the line amount"}})";
            }
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
    catch (const std::exception&)
    {
        // json::exception (parse/DTO/referenced-doc) and ValidationException
        // (generated contract constraints) both map to INVALID_REQUEST
        return R"({"error":{"code":"INVALID_REQUEST","message":"Invalid order request body"}})";
    }

    // Optional client money contributes zero when unset (exact int64 arithmetic)
    const int64_t taxTotal      = dto.taxTotalIsSet()      ? static_cast<int64_t>(dto.getTaxTotal().getAmount())      : 0;
    const int64_t tipTotal      = dto.tipTotalIsSet()      ? static_cast<int64_t>(dto.getTipTotal().getAmount())      : 0;
    const int64_t discountTotal = dto.discountTotalIsSet() ? static_cast<int64_t>(dto.getDiscountTotal().getAmount()) : 0;

    // CR-04: order-level adjustments are semantically non-negative money —
    // a negative tax/tip/discount enters the total formula as negative
    // money or a hidden surcharge; reject before any arithmetic (unset
    // fields extract 0, which passes)
    if (taxTotal < 0 || tipTotal < 0 || discountTotal < 0)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Order adjustments must be non-negative"}})";
    }

    // WR-06: when set, order-level adjustment currencies must match the
    // order total's currency (which the per-line check above already pinned
    // to every line's item currency) — OrderCreate::validate() never
    // reaches these optional fields, so without this check a USD order
    // could persist EUR tax/tip/discount folded into its USD total
    const std::string orderCurrency = dto.getTotal().getCurrency();
    if ((dto.taxTotalIsSet() && dto.getTaxTotal().getCurrency() != orderCurrency) ||
        (dto.tipTotalIsSet() && dto.getTipTotal().getCurrency() != orderCurrency) ||
        (dto.discountTotalIsSet() && dto.getDiscountTotal().getCurrency() != orderCurrency))
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Order adjustment currency must match the order currency"}})";
    }

    if (!FitsInInt32(subtotal))
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Amount overflow"}})";
    }
    const int64_t total = subtotal + taxTotal + tipTotal - discountTotal;
    // CR-04: an order discount exceeding the order amount drives the
    // computed total negative even with all-non-negative inputs
    if (total < 0)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Order total must be non-negative"}})";
    }
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

    // WR-03: an empty-string table_id is "treated as absent" for linkage but
    // the raw body is persisted as-is — the field's contract is
    // format: uuid, nullable: true, never "". Erase it so the stored
    // document matches the published contract.
    if (requestData.contains("table_id") && requestData["table_id"].get<std::string>().empty())
    {
        requestData.erase("table_id");
    }

    // Persist the stamped, server-authoritative document
    const std::string id = GenerateUuid();
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

    // TBL-02 post-persist transition: append the order id to the linked
    // table's open_order_ids (deduped — initialize the array when the doc
    // lacks it or carries a corrupt non-array value) and set pos_status to
    // "order_placed" unconditionally — a table with newly fired items IS
    // order_placed, even one previously order_served. The transition holds
    // the table-aggregate mutex and refetches the row under it: the
    // validation-time snapshot is stale by now (a concurrent create, seat,
    // or update may have transitioned the table since it was read), so
    // appending to the snapshot would drop that write. Not atomic with the
    // order Put (T-03.1-07 accepted): a failure here surfaces STORAGE_ERROR
    // and logs both ids as the reconciliation trail.
    if (tableLinked)
    {
        std::lock_guard<std::mutex> tableLock(s_tableAggregateMutex);
        std::string freshTableDoc;
        if (!s_storage->Get(linkedTableKey, freshTableDoc))
        {
            SPDLOG_ERROR("Order {} persisted but the linked table {} vanished before the transition",
                         id, dto.getTableId());
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to update linked table"}})";
        }
        try
        {
            linkedTableDoc = json::parse(freshTableDoc);
        }
        catch (const json::exception&)
        {
            SPDLOG_ERROR("Order {} persisted but the linked table {} row is corrupt", id, dto.getTableId());
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to update linked table"}})";
        }

        // The refetched row must still be the caller's tenant — a delete
        // and cross-tenant recreate between validation and this Put would
        // otherwise append this order to another tenant's aggregate
        if (linkedTableDoc.value("tenant_id", "default") != ctx.tenantId)
        {
            SPDLOG_ERROR("Order {} persisted but the linked table {} changed tenant before the transition",
                         id, dto.getTableId());
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to update linked table"}})";
        }

        if (!linkedTableDoc.contains("open_order_ids") ||
            !linkedTableDoc["open_order_ids"].is_array())
        {
            linkedTableDoc["open_order_ids"] = json::array();
        }
        auto& openOrderIds = linkedTableDoc["open_order_ids"];
        const json orderIdValue(id);
        if (std::find(openOrderIds.begin(), openOrderIds.end(), orderIdValue) == openOrderIds.end())
        {
            openOrderIds.push_back(orderIdValue);
        }
        linkedTableDoc["pos_status"]  = "order_placed";
        linkedTableDoc["updated_at"]  = GetCurrentTimestamp();
        if (!s_storage->Put(linkedTableKey, linkedTableDoc.dump()))
        {
            SPDLOG_ERROR("Order {} persisted but the linked table {} transition failed", id, dto.getTableId());
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to update linked table"}})";
        }
    }
    return requestData.dump();
}

// ============================================================================
// init_commerce_pos_overrides — called from CommercePluginImpl::Initialize()
// ============================================================================

/**
 * @brief      Resolve storage and register the POS order override handlers
 *
 * Registers POST /api/v1/orders (create) and GET /api/v1/orders (list) at
 * kOverrideHandlerPriority (200), superseding the generated createOrder and
 * listOrders stubs (priority 0). The commerce spec paths carry no /commerce
 * segment, so /api/v1/orders is both the generated stubs' route and the route
 * the regenerated dart-dio clients call. Owner name is "Commerce" (the
 * plugin's GetName()).
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

    SPDLOG_INFO("Registering commerce POS order override handlers");
    pm->RegisterHandler("POST", "/api/v1/orders", "orders_create", orders_create, "Commerce", kOverrideHandlerPriority);
    pm->RegisterHandler("GET", "/api/v1/orders", "orders_list", orders_list, "Commerce", kOverrideHandlerPriority);
}
