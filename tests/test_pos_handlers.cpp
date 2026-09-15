/**
 * @file       test_pos_handlers.cpp
 * @brief      Phase 2 (D-04) behavior suite for the POS override handlers
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * One Google Test file (D-04) dispatching through PluginManager::Route() over
 * a fresh RocksDB temp-dir fixture, exercising the 9 override routes exactly
 * as production dispatch does: the three restaurant menu lists (cursor paging
 * edges, D-07/D-09), the orders list (D-03 tenant filter, D-07/D-09), the
 * five create endpoints (POST -> list round-trip, D-02 INVALID_REFERENCE,
 * D-08 strict inline modifiers, D-01 order recompute), the D-03 tenant
 * filter, and HANDLER-06 bearer rejection with an empty-userId
 * RequestContext. Dispatch is synchronous — plain sequential asserts, no
 * condition variables, no sleeps.
 *
 * Route note (deferred-items.md): the commerce spec paths carry no /commerce
 * segment — the orders create override and its generated list stub live at
 * /api/v1/orders, NOT /api/v1/commerce/orders. Restaurant routes DO carry the
 * /restaurant segment.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"

// Generated plugin base classes — header-inline; NEVER include the
// *_plugin_impl.cpp files here (EXPORT_PLUGIN would be defined twice)
#include "restaurant_plugin.hpp"
#include "commerce_plugin.hpp"

#include "restaurant/restaurant_pos_handlers.hpp"
#include "commerce/commerce_pos_handlers.hpp"
#include "singleton/PluginManager.hpp"
#include "singleton/CServiceLocator.hpp"
#include "storage/RocksDBEngine.hpp"
#include "storage/KeyBuilder.hpp"

using json = nlohmann::json;

namespace fs = std::filesystem;

// ============================================================================
// Test constants (no magic numbers)
// ============================================================================

static constexpr unsigned int      kSeedCount          = 5;      ///< Entities seeded per menu list test
static constexpr unsigned int      kPageLimit          = 2;      ///< Explicit page size for paging tests
static constexpr unsigned long long kDefaultListLimit  = 50;     ///< Handler default when limit is absent/zero/invalid
static constexpr unsigned long long kLargeLimit        = 1000;   ///< D-09: honored verbatim, never clamped
static constexpr unsigned long long kOrdersPageLimit   = 1;      ///< IN-06: explicit page size for the orders-list paging tests
static constexpr unsigned int      kOrdersSeedCount    = 2;      ///< IN-06: orders created for the orders-list paging tests
static constexpr int32_t           kItemPrice          = 1000;   ///< Minor units of the canonical seeded item
static constexpr int32_t           kWrongClientUnitPrice = 1;    ///< Deliberately wrong client line money
static constexpr int32_t           kWrongClientLineTotal = 1;    ///< Deliberately wrong client line money
static constexpr int32_t           kModifierDelta      = 50;     ///< Minor units of the inline test modifier
static constexpr int32_t           kFractionalPrice    = 101;    ///< 101 x 0.5 = 50.5 rounds half-up to 51
static constexpr double            kHalfQuantity       = 0.5;    ///< Fractional order quantity
static constexpr double            kHugeQuantity       = 1e18;   ///< WR-07: 1000 x 1e18 leaves llround's int64 domain
static constexpr double            kAbsurdQuantity     = 1e300;  ///< WR-07: parseable double far past every money range
static constexpr int32_t           kHalfUpLineTotal    = 51;     ///< llround(101 * 0.5)
static constexpr int32_t           kMatchQty           = 2;      ///< Quantity for the recompute/mismatch tests
static constexpr int32_t           kRecomputedTotal    = kItemPrice * kMatchQty;  ///< Server-recomputed order total
static constexpr int32_t           kLineTaxAmount      = 100;    ///< Minor units of line-level tax in the fold test
static constexpr int32_t           kLineDiscountAmount = 50;     ///< Minor units of line-level discount in the fold test
static constexpr int32_t           kFoldedLineTotal    = kItemPrice * kMatchQty + kLineTaxAmount - kLineDiscountAmount;  ///< price*qty + line tax - line discount
static constexpr int32_t           kOverflowSeedPrice  = std::numeric_limits<int32_t>::max();  ///< Overflow-guard seed
static constexpr int32_t           kOverflowQty        = 2;      ///< INT32_MAX x 2 overflows int32
static constexpr int32_t           kNegativeAdjustmentAmount = 5000;  ///< CR-04 attack: -5000 tax against price x qty 2000
static constexpr int32_t           kNegativeTipAmount  = 100;    ///< Negative order tip variant (CR-04)
static constexpr int32_t           kExcessAdjustmentAmount = 9000;  ///< CR-04: exceeds the 2000 line/order amount it adjusts
static constexpr size_t            kUuidHexLength      = 32;     ///< Minted ids are 32 hex chars
static constexpr unsigned int      kMaxTraversalPages  = kSeedCount;  ///< Safety bound for cursor loops

static const std::string kDefaultTenant  = "default";
static const std::string kOtherTenant    = "other";
static const std::string kUsdCurrency    = "USD";
static const std::string kEuroCurrency   = "EUR";
static const std::string kShortCurrency  = "US";    ///< 2 chars — violates the 3-char Money currency constraint
static const std::string kLongCurrency   = "USDX";  ///< 4 chars — violates the 3-char Money currency constraint
static const std::string kActiveStatus   = "active";
static const std::string kDraftStatus    = "draft";
static const std::string kPosChannel     = "pos";
static const std::string kPickupType     = "pickup";
static const std::string kQueuedStatus   = "queued";
static const std::string kGrillStation   = "grill";
static const std::string kTestTimestamp  = "2026-09-15T00:00:00Z";
static const std::string kEmptyJsonObject = "{}";

// Well-formed 32-hex fixture ids (menu_id/category_id are NOT integrity-checked — D-02 scope)
static const std::string kSeedMenuId          = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const std::string kSeedCategoryId      = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
static const std::string kMissingRefId        = "cccccccccccccccccccccccccccccccc";
static const std::string kOtherTenantItemId   = "dddddddddddddddddddddddddddddddd";
static const std::string kOtherTenantGroupId  = "88888888888888888888888888888888";
static const std::string kOtherTenantOrderId  = "77777777777777777777777777777777";
static const std::string kInlineModifierId    = "ffffffffffffffffffffffffffffffff";
static const std::string kTestUserId          = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";

// Override routes under test (see file header for the /api/v1/orders route note)
static const std::string kMenuCategoriesPath = "/api/v1/restaurant/menu-categories";
static const std::string kMenuItemsPath      = "/api/v1/restaurant/menu-items";
static const std::string kModifierGroupsPath = "/api/v1/restaurant/modifier-groups";
static const std::string kKitchenTicketsPath = "/api/v1/restaurant/kitchen-tickets";
static const std::string kOrdersPath         = "/api/v1/orders";

static const std::vector<std::string> kMenuListPaths =
{
    kMenuCategoriesPath,
    kMenuItemsPath,
    kModifierGroupsPath,
};

// ============================================================================
// Test Fixture — fresh RocksDB + both plugins with their override registrations
// ============================================================================

/**
 * @brief      Route()-dispatch fixture for the POS override handlers
 *
 * Clones the test_archive_handlers fixture pattern: a unique temp RocksDB dir
 * per test, a service locator carrying PluginManager + StorageEngine, and the
 * generated RestaurantPlugin/CommercePlugin registered and initialized (their
 * priority-0 stubs), followed by the two init_*_pos_overrides calls — exactly
 * what the hand-written *PluginImpl::Initialize functions do in production.
 */
class PosHandlersTest : public ::testing::Test
{
protected:
    std::unique_ptr<RocksDBEngine> m_engine;
    PluginManager                  m_pm;
    CServiceLocator                m_locator;
    fs::path                       m_tempPath;
    RequestContext                 m_ctx;

    void SetUp() override
    {
        auto base = fs::current_path();
        auto timestamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        m_tempPath = base / ("test_pos_handlers_" + std::to_string(timestamp));
        fs::create_directories(m_tempPath);

        auto result = RocksDBEngine::Create(m_tempPath.string());
        ASSERT_TRUE(result.has_value()) << "Failed to create engine: "
                                        << result.error().message();
        m_engine = std::move(result.value());

        m_locator.RegisterService(Fnv1a("PluginManager"), &m_pm);
        m_locator.RegisterService(Fnv1a("StorageEngine"), m_engine.get());

        // Generated base first (stubs at priority 0), then the override
        // registrations at kOverrideHandlerPriority (200) — the same call
        // order as RestaurantPluginImpl::Initialize in production
        auto restaurant = std::make_shared<RestaurantPlugin>();
        m_pm.RegisterPlugin(restaurant, restaurant->GetPriority(), restaurant->GetUrlPaths());
        ASSERT_TRUE(restaurant->Initialize(m_locator));
        init_restaurant_pos_overrides(&m_pm, m_locator);

        auto commerce = std::make_shared<CommercePlugin>();
        m_pm.RegisterPlugin(commerce, commerce->GetPriority(), commerce->GetUrlPaths());
        ASSERT_TRUE(commerce->Initialize(m_locator));
        init_commerce_pos_overrides(&m_pm, m_locator);

        // Authenticated default tenant for all tests; bearer tests use a
        // local empty-userId context instead
        m_ctx.tenantId       = kDefaultTenant;
        m_ctx.organizationId = kDefaultTenant;
        m_ctx.userId         = kTestUserId;
    }

    void TearDown() override
    {
        m_pm.ShutdownAll();
        m_engine.reset();
        std::error_code ec;
        fs::remove_all(m_tempPath, ec);
    }

    ///
    /// Dispatch a request through PluginManager::Route() with the fixture ctx
    ///
    std::string Route(const std::string& method,
                      const std::string& path,
                      const std::string& body = "")
    {
        return m_pm.Route(m_ctx, method, path, body);
    }

    ///
    /// POST a JSON body and assert the response is not an error envelope
    ///
    std::string PostJson(const std::string& path, const std::string& body)
    {
        const std::string result = Route("POST", path, body);
        EXPECT_EQ(result.find("\"error\""), std::string::npos)
            << "Expected create success, got: " << result;
        return result;
    }

    ///
    /// Extract the minted id from a successful create response
    ///
    static std::string ParseId(const std::string& result)
    {
        return json::parse(result).at("id").get<std::string>();
    }

    ///
    /// GET a list endpoint and parse the JSON envelope
    ///
    json ListAsJson(const std::string& path)
    {
        return json::parse(Route("GET", path));
    }

    ///
    /// Collect the ids from a list envelope's data array
    ///
    static std::vector<std::string> DataIds(const json& listResponse)
    {
        std::vector<std::string> ids;
        for (const auto& entry : listResponse.at("data"))
        {
            ids.push_back(entry.at("id").get<std::string>());
        }
        return ids;
    }

    ///
    /// Seed kSeedCount categories, modifier groups, and menu items through
    /// the real POST routes (tenant "default")
    ///
    void SeedMenuEntities()
    {
        for (unsigned int i = 0; i < kSeedCount; ++i)
        {
            const std::string suffix = std::to_string(i);
            PostJson(kMenuCategoriesPath,
                R"({"menu_id":")" + kSeedMenuId + R"(","name":"category-)" + suffix + R"("})");
            PostJson(kModifierGroupsPath, R"({"name":"group-)" + suffix + R"("})");
            CreateMenuItem("item-" + suffix, kItemPrice, kUsdCurrency);
        }
    }

    ///
    /// Create a bare modifier group (name only); returns the minted id
    ///
    std::string CreateModifierGroup(const std::string& name)
    {
        json body;
        body["name"] = name;
        return ParseId(PostJson(kModifierGroupsPath, body.dump()));
    }

    ///
    /// Create a menu item at the given price; returns the minted id
    ///
    std::string CreateMenuItem(const std::string& name,
                               int32_t priceAmount,
                               const std::string& currency,
                               const std::vector<std::string>& modifierGroupIds = {})
    {
        json price;
        price["amount"]   = priceAmount;
        price["currency"] = currency;

        json body;
        body["category_id"] = kSeedCategoryId;
        body["name"]        = name;
        body["price"]       = price;
        body["status"]      = kActiveStatus;
        if (!modifierGroupIds.empty())
        {
            body["modifier_group_ids"] = modifierGroupIds;
        }
        return ParseId(PostJson(kMenuItemsPath, body.dump()));
    }

    ///
    /// Create an order at /api/v1/orders with DELIBERATELY WRONG client line
    /// money (server authority — D-01 recomputes everything); the caller
    /// supplies the client total. Returns the minted order id.
    ///
    std::string CreateOrder(const std::string& itemId,
                            double quantity,
                            int32_t totalAmount,
                            const std::string& currency)
    {
        json wrongUnit;
        wrongUnit["amount"]   = kWrongClientUnitPrice;
        wrongUnit["currency"] = currency;
        json wrongLineTotal;
        wrongLineTotal["amount"]   = kWrongClientLineTotal;
        wrongLineTotal["currency"] = currency;

        json line;
        line["product_id"] = itemId;
        line["quantity"]   = quantity;
        line["unit_price"] = wrongUnit;
        line["line_total"] = wrongLineTotal;

        json total;
        total["amount"]   = totalAmount;
        total["currency"] = currency;

        json body;
        body["status"]           = kDraftStatus;
        body["channel"]          = kPosChannel;
        body["fulfillment_type"] = kPickupType;
        body["total"]            = total;
        body["lines"]            = json::array({ line });
        return ParseId(PostJson(kOrdersPath, body.dump()));
    }

    ///
    /// Write a document owned by the OTHER tenant directly to storage, the
    /// way a cross-tenant writer would have left it (bypasses the API stamp)
    ///
    void PutOtherTenantDoc(const std::string& domain,
                           const std::string& entity,
                           const std::string& id,
                           const json& doc)
    {
        json otherDoc = doc;
        otherDoc["id"]        = id;
        otherDoc["tenant_id"] = kOtherTenant;
        auto keyResult = KeyBuilder::Build(domain, entity, id);
        ASSERT_TRUE(keyResult.has_value());
        ASSERT_TRUE(m_engine->Put(keyResult.value(), otherDoc.dump()));
    }

    ///
    /// Build a fully-formed inline Modifier carrying the complete D-08 field
    /// set (id, tenant_id, organization_id, created_at, updated_at, name,
    /// price_delta)
    ///
    static json FullModifier()
    {
        json priceDelta;
        priceDelta["amount"]   = kModifierDelta;
        priceDelta["currency"] = kUsdCurrency;

        json modifier;
        modifier["id"]              = kInlineModifierId;
        modifier["tenant_id"]       = kDefaultTenant;
        modifier["organization_id"] = kDefaultTenant;
        modifier["created_at"]      = kTestTimestamp;
        modifier["updated_at"]      = kTestTimestamp;
        modifier["name"]            = "extra-shot";
        modifier["price_delta"]     = priceDelta;
        return modifier;
    }
};

// ============================================================================
// Task 1 — List / Paging / Tenant / Bearer tests
// ============================================================================

///
/// No query string: first page holds every item, the default limit is 50,
/// has_more is false, and no next_cursor key is emitted.
///
TEST_F(PosHandlersTest, ListDefaultsReturnFirstPage)
{
    SeedMenuEntities();
    for (const std::string& path : kMenuListPaths)
    {
        m_ctx.queryString = "";
        const json page = ListAsJson(path);

        EXPECT_EQ(page.at("data").size(), kSeedCount) << "path: " << path;
        EXPECT_EQ(page.at("pagination").at("limit").get<unsigned long long>(),
                  kDefaultListLimit) << "path: " << path;
        EXPECT_FALSE(page.at("pagination").at("has_more").get<bool>()) << "path: " << path;
        EXPECT_FALSE(page.at("pagination").contains("next_cursor")) << "path: " << path;
    }
}

///
/// limit=2 returns exactly 2 rows, has_more true, and a non-empty next_cursor.
///
TEST_F(PosHandlersTest, ListHonorsExplicitLimit)
{
    SeedMenuEntities();
    for (const std::string& path : kMenuListPaths)
    {
        m_ctx.queryString = "limit=" + std::to_string(kPageLimit);
        const json page = ListAsJson(path);

        EXPECT_EQ(page.at("data").size(), kPageLimit) << "path: " << path;
        EXPECT_TRUE(page.at("pagination").at("has_more").get<bool>()) << "path: " << path;
        EXPECT_FALSE(page.at("pagination").at("next_cursor").get<std::string>().empty())
            << "path: " << path;
    }
}

///
/// Cursor traversal in pages of 2 over 5 seeded rows visits every id exactly
/// once and terminates on a last page with has_more false and no next_cursor.
///
TEST_F(PosHandlersTest, ListCursorTraversalReachesAllItems)
{
    SeedMenuEntities();
    for (const std::string& path : kMenuListPaths)
    {
        std::set<std::string> seen;
        std::string cursor;
        unsigned int pages = 0;
        while (pages < kMaxTraversalPages)
        {
            ++pages;
            m_ctx.queryString = "limit=" + std::to_string(kPageLimit) +
                                (cursor.empty() ? "" : "&cursor=" + cursor);
            const json page = ListAsJson(path);
            for (const std::string& id : DataIds(page))
            {
                seen.insert(id);
            }

            if (!page.at("pagination").at("has_more").get<bool>())
            {
                EXPECT_FALSE(page.at("pagination").contains("next_cursor"))
                    << "path: " << path;
                break;
            }
            cursor = page.at("pagination").at("next_cursor").get<std::string>();
            EXPECT_FALSE(cursor.empty()) << "path: " << path;
        }

        EXPECT_EQ(seen.size(), kSeedCount) << "path: " << path;
    }
}

///
/// limit=0 and limit=-1 both fall back to the default limit of 50 (D-09).
///
TEST_F(PosHandlersTest, ListLimitZeroFallsBackToDefault)
{
    SeedMenuEntities();
    const std::vector<std::string> fallbackQueries = { "limit=0", "limit=-1" };
    for (const std::string& path : kMenuListPaths)
    {
        for (const std::string& rawQuery : fallbackQueries)
        {
            m_ctx.queryString = rawQuery;
            const json page = ListAsJson(path);
            EXPECT_EQ(page.at("pagination").at("limit").get<unsigned long long>(),
                      kDefaultListLimit) << "path: " << path << " query: " << rawQuery;
        }
    }
}

///
/// A non-numeric limit falls back to the default limit of 50.
///
TEST_F(PosHandlersTest, ListNonNumericLimitFallsBackToDefault)
{
    SeedMenuEntities();
    for (const std::string& path : kMenuListPaths)
    {
        m_ctx.queryString = "limit=abc";
        const json page = ListAsJson(path);
        EXPECT_EQ(page.at("pagination").at("limit").get<unsigned long long>(),
                  kDefaultListLimit) << "path: " << path;
    }
}

///
/// A large valid limit is honored verbatim — all kSeedCount rows return in a
/// single page with has_more false and no next_cursor (D-09: no clamp).
///
TEST_F(PosHandlersTest, ListLargeLimitHonoredVerbatim)
{
    SeedMenuEntities();
    for (const std::string& path : kMenuListPaths)
    {
        m_ctx.queryString = "limit=" + std::to_string(kLargeLimit);
        const json page = ListAsJson(path);

        EXPECT_EQ(page.at("data").size(), kSeedCount) << "path: " << path;
        EXPECT_EQ(page.at("pagination").at("limit").get<unsigned long long>(),
                  kLargeLimit) << "path: " << path;
        EXPECT_FALSE(page.at("pagination").at("has_more").get<bool>()) << "path: " << path;
        EXPECT_FALSE(page.at("pagination").contains("next_cursor")) << "path: " << path;
    }
}

///
/// A malformed (non-32-hex) cursor is rejected with INVALID_REQUEST (D-07).
///
TEST_F(PosHandlersTest, ListInvalidCursorRejected)
{
    SeedMenuEntities();
    for (const std::string& path : kMenuListPaths)
    {
        m_ctx.queryString = "cursor=zz-not-hex";
        const std::string result = Route("GET", path);

        EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
            << "Expected INVALID_REQUEST, got: " << result;
        EXPECT_EQ(result.find("\"data\""), std::string::npos)
            << "Envelope must not carry data, got: " << result;
    }
}

///
/// A cross-tenant document written directly to storage is excluded from the
/// tenant-default list results (D-03 filter).
///
TEST_F(PosHandlersTest, ListTenantFilterExcludesOtherTenant)
{
    SeedMenuEntities();

    json otherDoc;
    otherDoc["id"]        = kOtherTenantItemId;
    otherDoc["tenant_id"] = kOtherTenant;
    otherDoc["name"]      = "other-tenant-item";
    auto keyResult = KeyBuilder::Build("restaurant", "menu-items", kOtherTenantItemId);
    ASSERT_TRUE(keyResult.has_value());
    ASSERT_TRUE(m_engine->Put(keyResult.value(), otherDoc.dump()));

    m_ctx.queryString = "";
    const json page = ListAsJson(kMenuItemsPath);

    ASSERT_EQ(page.at("data").size(), kSeedCount);
    const std::vector<std::string> ids = DataIds(page);
    EXPECT_EQ(std::find(ids.begin(), ids.end(), kOtherTenantItemId), ids.end())
        << "Cross-tenant item must not appear in tenant-default results";
}

///
/// A modifier group created with one fully-formed inline modifier (D-08 field
/// set) lists back with the modifier fields embedded in the group document.
///
TEST_F(PosHandlersTest, ModifierGroupsListEmbedsModifiers)
{
    json body;
    body["name"]      = "espresso-options";
    body["modifiers"] = json::array({ FullModifier() });
    PostJson(kModifierGroupsPath, body.dump());

    m_ctx.queryString = "";
    const json page = ListAsJson(kModifierGroupsPath);

    ASSERT_EQ(page.at("data").size(), 1);
    const json& group = page.at("data")[0];
    ASSERT_TRUE(group.contains("modifiers"));
    ASSERT_EQ(group.at("modifiers").size(), 1);

    const json& modifier = group.at("modifiers")[0];
    EXPECT_EQ(modifier.at("id").get<std::string>(), kInlineModifierId);
    EXPECT_EQ(modifier.at("name").get<std::string>(), "extra-shot");
    EXPECT_EQ(modifier.at("price_delta").at("amount").get<int32_t>(), kModifierDelta);
    EXPECT_EQ(modifier.at("price_delta").at("currency").get<std::string>(), kUsdCurrency);
}

///
/// An empty-userId RequestContext gets the UNAUTHORIZED envelope from every
/// one of the 9 override routes (HANDLER-06 — the evidence for success
/// criterion 5).
///
TEST_F(PosHandlersTest, BearerRejectionOnAllOverrideRoutes)
{
    RequestContext anonCtx;
    anonCtx.tenantId       = kDefaultTenant;
    anonCtx.organizationId = kDefaultTenant;
    // userId deliberately empty — no authenticated user

    const std::vector<std::pair<std::string, std::string>> overrideRoutes =
    {
        { "GET",  kMenuCategoriesPath },
        { "POST", kMenuCategoriesPath },
        { "GET",  kMenuItemsPath },
        { "POST", kMenuItemsPath },
        { "GET",  kModifierGroupsPath },
        { "POST", kModifierGroupsPath },
        { "POST", kKitchenTicketsPath },
        { "GET",  kOrdersPath },
        { "POST", kOrdersPath },
    };

    for (const auto& route : overrideRoutes)
    {
        const std::string result =
            m_pm.Route(anonCtx, route.first, route.second, kEmptyJsonObject);
        EXPECT_NE(result.find("\"code\":\"UNAUTHORIZED\""), std::string::npos)
            << "Expected UNAUTHORIZED envelope on " << route.first << " "
            << route.second << ", got: " << result;
    }
}

// ============================================================================
// Task 2 — Menu round-trip + D-08/D-02 rejection + kitchen-ticket tests
// ============================================================================

///
/// POST menu-categories stamps id/tenant/organization/timestamps and the
/// created category round-trips through the list endpoint.
///
TEST_F(PosHandlersTest, MenuCategoryCreatePersistsAndRoundTrips)
{
    const std::string body = R"({
        "menu_id": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "name": "drinks",
        "status": "active"
    })";

    m_ctx.queryString = "";
    const json doc = json::parse(PostJson(kMenuCategoriesPath, body));

    const std::string id = doc.at("id").get<std::string>();
    EXPECT_EQ(id.size(), kUuidHexLength);
    EXPECT_EQ(doc.at("tenant_id").get<std::string>(), kDefaultTenant);
    EXPECT_EQ(doc.at("organization_id").get<std::string>(), kDefaultTenant);
    EXPECT_FALSE(doc.at("created_at").get<std::string>().empty());
    EXPECT_FALSE(doc.at("updated_at").get<std::string>().empty());

    const json page = ListAsJson(kMenuCategoriesPath);
    ASSERT_EQ(page.at("data").size(), 1);
    EXPECT_EQ(page.at("data")[0].at("id").get<std::string>(), id);
    EXPECT_EQ(page.at("data")[0].at("name").get<std::string>(), "drinks");
}

///
/// POST modifier-groups with ONE fully-formed inline modifier (complete D-08
/// field set) persists and round-trips with the modifier inline.
///
TEST_F(PosHandlersTest, ModifierGroupCreatePersistsAndRoundTrips)
{
    json body;
    body["name"]      = "milk-options";
    body["modifiers"] = json::array({ FullModifier() });

    m_ctx.queryString = "";
    const json doc = json::parse(PostJson(kModifierGroupsPath, body.dump()));

    const std::string id = doc.at("id").get<std::string>();
    EXPECT_EQ(id.size(), kUuidHexLength);
    EXPECT_EQ(doc.at("tenant_id").get<std::string>(), kDefaultTenant);
    ASSERT_EQ(doc.at("modifiers").size(), 1);
    EXPECT_EQ(doc.at("modifiers")[0].at("id").get<std::string>(), kInlineModifierId);

    const json page = ListAsJson(kModifierGroupsPath);
    ASSERT_EQ(page.at("data").size(), 1);
    const json& stored = page.at("data")[0];
    EXPECT_EQ(stored.at("id").get<std::string>(), id);
    ASSERT_TRUE(stored.contains("modifiers"));
    ASSERT_EQ(stored.at("modifiers").size(), 1);
    EXPECT_EQ(stored.at("modifiers")[0].at("name").get<std::string>(), "extra-shot");
    EXPECT_EQ(stored.at("modifiers")[0].at("price_delta").at("amount").get<int32_t>(),
              kModifierDelta);
}

///
/// Inline modifiers whose client-sent tenant_id/organization_id differ from
/// the request context are rewritten to the ctx values — the same D-03
/// body-stamp the group document gets — so a stored group can never claim a
/// different tenant than its embedded modifiers. Every other modifier field
/// keeps its parsed client value; the D-08 strict field-set parse is
/// untouched (IN-01).
///
TEST_F(PosHandlersTest, ModifierGroupCreateRestampsInlineModifierTenancy)
{
    json modifier = FullModifier();
    modifier["tenant_id"]       = kOtherTenant;
    modifier["organization_id"] = kOtherTenant;

    json body;
    body["name"]      = "cross-tenant-modifiers";
    body["modifiers"] = json::array({ modifier });
    PostJson(kModifierGroupsPath, body.dump());

    m_ctx.queryString = "";
    const json page = ListAsJson(kModifierGroupsPath);
    ASSERT_EQ(page.at("data").size(), 1);
    ASSERT_TRUE(page.at("data")[0].contains("modifiers"));
    ASSERT_EQ(page.at("data")[0].at("modifiers").size(), 1);

    const json& storedModifier = page.at("data")[0].at("modifiers")[0];
    EXPECT_EQ(storedModifier.at("tenant_id").get<std::string>(), kDefaultTenant)
        << "Modifier tenant must be restamped from ctx, not stored verbatim";
    EXPECT_EQ(storedModifier.at("organization_id").get<std::string>(), kDefaultTenant)
        << "Modifier organization must be restamped from ctx, not stored verbatim";
    EXPECT_EQ(storedModifier.at("id").get<std::string>(), kInlineModifierId)
        << "Non-tenancy fields keep their parsed client values (D-08)";
    EXPECT_EQ(storedModifier.at("created_at").get<std::string>(), kTestTimestamp)
        << "Modifier audit fields are not server-minted (D-08)";
    EXPECT_EQ(storedModifier.at("name").get<std::string>(), "extra-shot");
}

///
/// POST menu-items referencing a real modifier group keeps the
/// modifier_group_ids reference in the persisted document (dependency order:
/// group before item — Pitfall 8).
///
TEST_F(PosHandlersTest, MenuItemCreatePersistsAndRoundTrips)
{
    const std::string groupId = CreateModifierGroup("size-options");
    const std::string itemId =
        CreateMenuItem("mocha", kItemPrice, kUsdCurrency, { groupId });

    m_ctx.queryString = "";
    const json page = ListAsJson(kMenuItemsPath);
    ASSERT_EQ(page.at("data").size(), 1);
    const json& stored = page.at("data")[0];
    EXPECT_EQ(stored.at("id").get<std::string>(), itemId);
    EXPECT_EQ(stored.at("name").get<std::string>(), "mocha");
    EXPECT_EQ(stored.at("price").at("amount").get<int32_t>(), kItemPrice);
    EXPECT_EQ(stored.at("price").at("currency").get<std::string>(), kUsdCurrency);
    EXPECT_EQ(stored.at("status").get<std::string>(), kActiveStatus);
    ASSERT_TRUE(stored.contains("modifier_group_ids"));
    ASSERT_EQ(stored.at("modifier_group_ids").size(), 1);
    EXPECT_EQ(stored.at("modifier_group_ids")[0].get<std::string>(), groupId);
    EXPECT_EQ(stored.at("tenant_id").get<std::string>(), kDefaultTenant);
}

///
/// A menu-item body missing the required price field is rejected with
/// INVALID_REQUEST and nothing is persisted.
///
TEST_F(PosHandlersTest, MenuItemCreateRejectsMissingRequiredField)
{
    // Everything valid except price (the single violated field)
    const std::string body = R"({
        "category_id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "name": "priceless-item",
        "status": "active"
    })";

    const std::string result = Route("POST", kMenuItemsPath, body);
    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kMenuItemsPath).at("data").size(), 0)
        << "Rejected item must not persist";
}

///
/// An inline modifier carrying only name + price_delta (missing the D-08
/// audit-field set) rejects the whole modifier-group create with
/// INVALID_REQUEST — no server-side minting of modifier audit fields.
///
TEST_F(PosHandlersTest, ModifierGroupCreateRejectsModifierMissingAuditFields)
{
    const std::string body = R"({
        "name": "incomplete-group",
        "modifiers": [{
            "name": "no-audit-fields",
            "price_delta": {"amount": 50, "currency": "USD"}
        }]
    })";

    const std::string result = Route("POST", kModifierGroupsPath, body);
    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kModifierGroupsPath).at("data").size(), 0)
        << "Rejected group must not persist";
}

///
/// A well-formed but non-existent modifier_group_id rejects the menu-item
/// create with INVALID_REFERENCE and nothing is persisted (D-02).
///
TEST_F(PosHandlersTest, MenuItemCreateRejectsDanglingModifierGroup)
{
    const std::string body = R"({
        "category_id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "name": "dangling-ref-item",
        "price": {"amount": 1000, "currency": "USD"},
        "status": "active",
        "modifier_group_ids": [")" + kMissingRefId + R"("]
    })";

    const std::string result = Route("POST", kMenuItemsPath, body);
    EXPECT_NE(result.find("INVALID_REFERENCE"), std::string::npos)
        << "Expected INVALID_REFERENCE, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kMenuItemsPath).at("data").size(), 0)
        << "Rejected item must not persist";
}

///
/// A modifier group that exists but belongs to ANOTHER tenant rejects the
/// menu-item create with INVALID_REFERENCE — existence alone is not enough,
/// the reference must resolve within the caller's tenant (WR-01).
///
TEST_F(PosHandlersTest, MenuItemCreateRejectsCrossTenantModifierGroup)
{
    PutOtherTenantDoc("restaurant", "modifier-groups", kOtherTenantGroupId,
                      json{{"name", "other-tenant-group"}});

    const std::string body = R"({
        "category_id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "name": "cross-tenant-ref-item",
        "price": {"amount": 1000, "currency": "USD"},
        "status": "active",
        "modifier_group_ids": [")" + kOtherTenantGroupId + R"("]
    })";

    const std::string result = Route("POST", kMenuItemsPath, body);
    EXPECT_NE(result.find("INVALID_REFERENCE"), std::string::npos)
        << "Expected INVALID_REFERENCE, got: " << result;
    EXPECT_NE(result.find("another tenant"), std::string::npos)
        << "Expected the cross-tenant message, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kMenuItemsPath).at("data").size(), 0)
        << "Cross-tenant-referencing item must not persist";
}

///
/// A kitchen ticket against a real order persists contract-shaped with ctx
/// tenant stamps and round-trips through the generated dump-all list stub
/// (dependency order: item -> order -> ticket — Pitfall 8).
///
TEST_F(PosHandlersTest, KitchenTicketCreatePersists)
{
    const std::string itemId  = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    const std::string orderId = CreateOrder(itemId, kMatchQty, kRecomputedTotal, kUsdCurrency);

    const std::string body = R"({
        "order_id": ")" + orderId + R"(",
        "station": "grill",
        "status": "queued"
    })";

    m_ctx.queryString = "";
    const json doc = json::parse(PostJson(kKitchenTicketsPath, body));

    const std::string ticketId = doc.at("id").get<std::string>();
    EXPECT_EQ(ticketId.size(), kUuidHexLength);
    EXPECT_EQ(doc.at("tenant_id").get<std::string>(), kDefaultTenant);
    EXPECT_EQ(doc.at("organization_id").get<std::string>(), kDefaultTenant);
    EXPECT_FALSE(doc.at("created_at").get<std::string>().empty());
    EXPECT_FALSE(doc.at("updated_at").get<std::string>().empty());
    EXPECT_EQ(doc.at("order_id").get<std::string>(), orderId);
    EXPECT_EQ(doc.at("station").get<std::string>(), kGrillStation);
    EXPECT_EQ(doc.at("status").get<std::string>(), kQueuedStatus);

    // Persistence proof through the generated kitchen-tickets list stub
    const json page = ListAsJson(kKitchenTicketsPath);
    ASSERT_EQ(page.at("data").size(), 1);
    EXPECT_EQ(page.at("data")[0].at("id").get<std::string>(), ticketId);
    EXPECT_EQ(page.at("data")[0].at("order_id").get<std::string>(), orderId);
}

///
/// A well-formed but non-existent order_id rejects the kitchen-ticket create
/// with INVALID_REFERENCE and nothing is persisted (D-02).
///
TEST_F(PosHandlersTest, KitchenTicketCreateRejectsDanglingOrder)
{
    const std::string body = R"({
        "order_id": ")" + kMissingRefId + R"(",
        "station": "grill",
        "status": "queued"
    })";

    const std::string result = Route("POST", kKitchenTicketsPath, body);
    EXPECT_NE(result.find("INVALID_REFERENCE"), std::string::npos)
        << "Expected INVALID_REFERENCE, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kKitchenTicketsPath).at("data").size(), 0)
        << "Rejected ticket must not persist";
}

///
/// An order that exists but belongs to ANOTHER tenant rejects the
/// kitchen-ticket create with INVALID_REFERENCE — a tenant-A ticket must not
/// link to a tenant-B order (WR-01).
///
TEST_F(PosHandlersTest, KitchenTicketCreateRejectsCrossTenantOrder)
{
    PutOtherTenantDoc("commerce", "orders", kOtherTenantOrderId,
                      json{{"status", "draft"}});

    const std::string body = R"({
        "order_id": ")" + kOtherTenantOrderId + R"(",
        "station": "grill",
        "status": "queued"
    })";

    const std::string result = Route("POST", kKitchenTicketsPath, body);
    EXPECT_NE(result.find("INVALID_REFERENCE"), std::string::npos)
        << "Expected INVALID_REFERENCE, got: " << result;
    EXPECT_NE(result.find("another tenant"), std::string::npos)
        << "Expected the cross-tenant message, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kKitchenTicketsPath).at("data").size(), 0)
        << "Cross-tenant ticket must not persist";
}

// ============================================================================
// Task 3 — D-01 order recompute tests
// ============================================================================

///
/// An order with deliberately wrong client line money and the CORRECT total
/// persists with server-recomputed values: stored unit_price equals the
/// stored menu price, and line_total/subtotal/total equal the recomputation
/// (client money is never stored as-is — D-01, T-02-01).
///
TEST_F(PosHandlersTest, OrderCreatePersistsWithServerRecomputedTotals)
{
    const std::string itemId  = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    const std::string orderId = CreateOrder(itemId, kMatchQty, kRecomputedTotal, kUsdCurrency);
    EXPECT_EQ(orderId.size(), kUuidHexLength);

    // Direct engine read of the persisted document on the flat KeyBuilder key
    auto keyResult = KeyBuilder::Build("commerce", "orders", orderId);
    ASSERT_TRUE(keyResult.has_value());
    std::string stored;
    ASSERT_TRUE(m_engine->Get(keyResult.value(), stored));
    const json doc = json::parse(stored);

    ASSERT_EQ(doc.at("lines").size(), 1);
    EXPECT_EQ(doc.at("lines")[0].at("unit_price").at("amount").get<int32_t>(), kItemPrice)
        << "Stored unit price must be the menu price, not the client's";
    EXPECT_EQ(doc.at("lines")[0].at("unit_price").at("currency").get<std::string>(), kUsdCurrency);
    EXPECT_EQ(doc.at("lines")[0].at("line_total").at("amount").get<int32_t>(), kRecomputedTotal);
    EXPECT_EQ(doc.at("subtotal").at("amount").get<int32_t>(), kRecomputedTotal);
    EXPECT_EQ(doc.at("total").at("amount").get<int32_t>(), kRecomputedTotal);
    EXPECT_EQ(doc.at("total").at("currency").get<std::string>(), kUsdCurrency);
    EXPECT_EQ(doc.at("tenant_id").get<std::string>(), kDefaultTenant);

    // Round-trip through the tenant-filtered orders list override at
    // GET /api/v1/orders (see the route note in the file header)
    m_ctx.queryString = "";
    const json page = ListAsJson(kOrdersPath);
    ASSERT_EQ(page.at("data").size(), 1);
    EXPECT_EQ(page.at("data")[0].at("id").get<std::string>(), orderId);
}

///
/// An order written by another tenant directly to storage is excluded from
/// the tenant-default orders list — the GET /api/v1/orders override filters
/// by ctx.tenantId like the restaurant lists (WR-02, D-03).
///
TEST_F(PosHandlersTest, OrdersListTenantFilterExcludesOtherTenant)
{
    const std::string itemId  = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    const std::string orderId = CreateOrder(itemId, kMatchQty, kRecomputedTotal, kUsdCurrency);
    PutOtherTenantDoc("commerce", "orders", kOtherTenantOrderId,
                      json{{"status", "draft"}});

    m_ctx.queryString = "";
    const json page = ListAsJson(kOrdersPath);

    ASSERT_EQ(page.at("data").size(), 1);
    const std::vector<std::string> ids = DataIds(page);
    EXPECT_EQ(ids[0], orderId)
        << "The caller's own order must be listed";
    EXPECT_EQ(std::find(ids.begin(), ids.end(), kOtherTenantOrderId), ids.end())
        << "Cross-tenant order must not appear in tenant-default results";
}

///
/// A malformed (non-32-hex) cursor on the orders list is rejected with
/// INVALID_REQUEST (D-07 posture, matching the restaurant lists).
///
TEST_F(PosHandlersTest, OrdersListInvalidCursorRejected)
{
    const std::string itemId  = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    CreateOrder(itemId, kMatchQty, kRecomputedTotal, kUsdCurrency);

    m_ctx.queryString = "cursor=zz-not-hex";
    const std::string result = Route("GET", kOrdersPath);

    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;
    EXPECT_EQ(result.find("\"data\""), std::string::npos)
        << "Envelope must not carry data, got: " << result;
}

///
/// GET /api/v1/orders honors an explicit limit and falls back to the default
/// 50: limit=1 over two stored orders returns one row with has_more true and
/// a non-empty next_cursor, while limit=0 and an absent query both return
/// every order in a single default-sized page (IN-06, D-09).
///
TEST_F(PosHandlersTest, OrdersListHonorsExplicitLimitAndDefaultFallback)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    CreateOrder(itemId, kMatchQty, kRecomputedTotal, kUsdCurrency);
    CreateOrder(itemId, kMatchQty, kRecomputedTotal, kUsdCurrency);

    m_ctx.queryString = "limit=" + std::to_string(kOrdersPageLimit);
    const json page = ListAsJson(kOrdersPath);
    EXPECT_EQ(page.at("data").size(), kOrdersPageLimit)
        << "Explicit limit must be honored verbatim";
    EXPECT_EQ(page.at("pagination").at("limit").get<unsigned long long>(),
              kOrdersPageLimit);
    EXPECT_TRUE(page.at("pagination").at("has_more").get<bool>())
        << "A page cut short by the limit must report has_more";
    EXPECT_FALSE(page.at("pagination").at("next_cursor").get<std::string>().empty())
        << "A has_more page must carry a cursor for the next traversal step";

    const std::vector<std::string> fallbackQueries = { "limit=0", "" };
    for (const std::string& rawQuery : fallbackQueries)
    {
        m_ctx.queryString = rawQuery;
        const json fullPage = ListAsJson(kOrdersPath);
        EXPECT_EQ(fullPage.at("data").size(), kOrdersSeedCount)
            << "query: '" << rawQuery << "'";
        EXPECT_EQ(fullPage.at("pagination").at("limit").get<unsigned long long>(),
                  kDefaultListLimit) << "query: '" << rawQuery << "'";
        EXPECT_FALSE(fullPage.at("pagination").at("has_more").get<bool>())
            << "query: '" << rawQuery << "'";
        EXPECT_FALSE(fullPage.at("pagination").contains("next_cursor"))
            << "query: '" << rawQuery << "'";
    }
}

///
/// Cursor traversal over GET /api/v1/orders in pages of 1 visits every order
/// exactly once and terminates on a last page with has_more false and no
/// next_cursor (IN-06 — the orders list is a separate copy of the paging
/// core, not a shared call into the restaurant list_entity).
///
TEST_F(PosHandlersTest, OrdersListCursorTraversalReachesAllOrders)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    const std::string firstOrderId =
        CreateOrder(itemId, kMatchQty, kRecomputedTotal, kUsdCurrency);
    const std::string secondOrderId =
        CreateOrder(itemId, kMatchQty, kRecomputedTotal, kUsdCurrency);

    std::set<std::string> seen;
    std::string cursor;
    unsigned int pages = 0;
    while (pages < kMaxTraversalPages)
    {
        ++pages;
        m_ctx.queryString = "limit=" + std::to_string(kOrdersPageLimit) +
                            (cursor.empty() ? "" : "&cursor=" + cursor);
        const json page = ListAsJson(kOrdersPath);
        for (const std::string& id : DataIds(page))
        {
            seen.insert(id);
        }

        if (!page.at("pagination").at("has_more").get<bool>())
        {
            EXPECT_FALSE(page.at("pagination").contains("next_cursor"))
                << "Terminal page must not carry a next_cursor";
            break;
        }
        cursor = page.at("pagination").at("next_cursor").get<std::string>();
        EXPECT_FALSE(cursor.empty());
    }

    EXPECT_EQ(pages, kOrdersSeedCount)
        << "Two orders at limit=1 must traverse in exactly two pages";
    EXPECT_EQ(seen.size(), kOrdersSeedCount)
        << "Traversal must visit every order";
    EXPECT_EQ(seen.count(firstOrderId), 1u)
        << "Each order must be visited exactly once";
    EXPECT_EQ(seen.count(secondOrderId), 1u)
        << "Each order must be visited exactly once";
}

///
/// A client total off by one minor unit rejects the order with TOTAL_MISMATCH
/// and nothing is persisted (D-01 exact-equality check, T-02-01).
///
TEST_F(PosHandlersTest, OrderCreateRejectsTotalMismatch)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    const std::string body = R"({
        "status": "draft",
        "channel": "pos",
        "fulfillment_type": "pickup",
        "total": {"amount": )" + std::to_string(kRecomputedTotal + 1) + R"(, "currency": "USD"},
        "lines": [{
            "product_id": ")" + itemId + R"(",
            "quantity": 2,
            "unit_price": {"amount": 1000, "currency": "USD"},
            "line_total": {"amount": 2000, "currency": "USD"}
        }]
    })";

    const std::string result = Route("POST", kOrdersPath, body);
    EXPECT_NE(result.find("TOTAL_MISMATCH"), std::string::npos)
        << "Expected TOTAL_MISMATCH, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Mismatched order must not persist";
}

///
/// A well-formed but non-existent product_id rejects the order with
/// INVALID_REFERENCE (D-01/D-02) and nothing is persisted.
///
TEST_F(PosHandlersTest, OrderCreateRejectsUnknownProduct)
{
    const std::string body = R"({
        "status": "draft",
        "channel": "pos",
        "fulfillment_type": "pickup",
        "total": {"amount": 1000, "currency": "USD"},
        "lines": [{
            "product_id": ")" + kMissingRefId + R"(",
            "quantity": 1,
            "unit_price": {"amount": 1000, "currency": "USD"},
            "line_total": {"amount": 1000, "currency": "USD"}
        }]
    })";

    const std::string result = Route("POST", kOrdersPath, body);
    EXPECT_NE(result.find("INVALID_REFERENCE"), std::string::npos)
        << "Expected INVALID_REFERENCE, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Rejected order must not persist";
}

///
/// A menu item that exists but belongs to ANOTHER tenant rejects the order
/// with INVALID_REFERENCE — otherwise the tenant-B item's price and currency
/// would be used and echoed into a tenant-A order (cross-tenant disclosure,
/// WR-01).
///
TEST_F(PosHandlersTest, OrderCreateRejectsCrossTenantProduct)
{
    json price;
    price["amount"]   = kItemPrice;
    price["currency"] = kUsdCurrency;
    PutOtherTenantDoc("restaurant", "menu-items", kOtherTenantItemId,
                      json{{"name", "other-tenant-latte"}, {"price", price}});

    const std::string body = R"({
        "status": "draft",
        "channel": "pos",
        "fulfillment_type": "pickup",
        "total": {"amount": 2000, "currency": "USD"},
        "lines": [{
            "product_id": ")" + kOtherTenantItemId + R"(",
            "quantity": 2,
            "unit_price": {"amount": 1000, "currency": "USD"},
            "line_total": {"amount": 2000, "currency": "USD"}
        }]
    })";

    const std::string result = Route("POST", kOrdersPath, body);
    EXPECT_NE(result.find("INVALID_REFERENCE"), std::string::npos)
        << "Expected INVALID_REFERENCE, got: " << result;
    EXPECT_NE(result.find("another tenant"), std::string::npos)
        << "Expected the cross-tenant message, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Cross-tenant-priced order must not persist";
}

///
/// A fractional quantity rounds half-up at the single llround point: stored
/// price 101 minor units x quantity 0.5 = 50.5 -> stored line total 51.
///
TEST_F(PosHandlersTest, OrderCreateFractionalQuantityRoundsHalfUp)
{
    const std::string itemId =
        CreateMenuItem("cheap-cookie", kFractionalPrice, kUsdCurrency);
    const std::string orderId =
        CreateOrder(itemId, kHalfQuantity, kHalfUpLineTotal, kUsdCurrency);

    auto keyResult = KeyBuilder::Build("commerce", "orders", orderId);
    ASSERT_TRUE(keyResult.has_value());
    std::string stored;
    ASSERT_TRUE(m_engine->Get(keyResult.value(), stored));
    const json doc = json::parse(stored);

    ASSERT_EQ(doc.at("lines").size(), 1);
    EXPECT_EQ(doc.at("lines")[0].at("line_total").at("amount").get<int32_t>(),
              kHalfUpLineTotal)
        << "101 x 0.5 = 50.5 must round half-up to 51";
    EXPECT_EQ(doc.at("subtotal").at("amount").get<int32_t>(), kHalfUpLineTotal);
    EXPECT_EQ(doc.at("total").at("amount").get<int32_t>(), kHalfUpLineTotal);
}

///
/// A line priced in a currency different from the stored item's currency is
/// rejected with INVALID_REQUEST (currency agreement, D-01).
///
TEST_F(PosHandlersTest, OrderCreateRejectsCurrencyMismatch)
{
    const std::string itemId = CreateMenuItem("euro-latte", kItemPrice, kUsdCurrency);
    const std::string body = R"({
        "status": "draft",
        "channel": "pos",
        "fulfillment_type": "pickup",
        "total": {"amount": 2000, "currency": "USD"},
        "lines": [{
            "product_id": ")" + itemId + R"(",
            "quantity": 2,
            "unit_price": {"amount": 1000, "currency": "EUR"},
            "line_total": {"amount": 2000, "currency": "EUR"}
        }]
    })";

    const std::string result = Route("POST", kOrdersPath, body);
    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;
    EXPECT_NE(result.find("Currency mismatch"), std::string::npos)
        << "Expected the currency-mismatch message, got: " << result;
}

///
/// Line-level tax_total/discount_total fold into the recomputed line_total
/// (price x qty + tax - discount) and flow through subtotal/total, so the
/// stored document is internally consistent; a client total that ignores the
/// fold still rejects with TOTAL_MISMATCH (WR-03).
///
TEST_F(PosHandlersTest, OrderCreateFoldsLineTaxAndDiscountIntoTotals)
{
    const std::string itemId = CreateMenuItem("mocha", kItemPrice, kUsdCurrency);

    json lineTax;
    lineTax["amount"]   = kLineTaxAmount;
    lineTax["currency"] = kUsdCurrency;
    json lineDiscount;
    lineDiscount["amount"]   = kLineDiscountAmount;
    lineDiscount["currency"] = kUsdCurrency;
    json total;
    total["amount"]   = kFoldedLineTotal;
    total["currency"] = kUsdCurrency;

    json line;
    line["product_id"]     = itemId;
    line["quantity"]       = kMatchQty;
    line["unit_price"]     = json{{"amount", kWrongClientUnitPrice}, {"currency", kUsdCurrency}};
    line["line_total"]     = json{{"amount", kWrongClientLineTotal}, {"currency", kUsdCurrency}};
    line["tax_total"]      = lineTax;
    line["discount_total"] = lineDiscount;

    json body;
    body["status"]           = kDraftStatus;
    body["channel"]          = kPosChannel;
    body["fulfillment_type"] = kPickupType;
    body["total"]            = total;
    body["lines"]            = json::array({ line });

    const std::string orderId = ParseId(PostJson(kOrdersPath, body.dump()));

    auto keyResult = KeyBuilder::Build("commerce", "orders", orderId);
    ASSERT_TRUE(keyResult.has_value());
    std::string stored;
    ASSERT_TRUE(m_engine->Get(keyResult.value(), stored));
    const json doc = json::parse(stored);

    ASSERT_EQ(doc.at("lines").size(), 1);
    EXPECT_EQ(doc.at("lines")[0].at("line_total").at("amount").get<int32_t>(), kFoldedLineTotal)
        << "line_total must fold price*qty + tax - discount (2000 + 100 - 50)";
    EXPECT_EQ(doc.at("lines")[0].at("tax_total").at("amount").get<int32_t>(), kLineTaxAmount);
    EXPECT_EQ(doc.at("lines")[0].at("discount_total").at("amount").get<int32_t>(), kLineDiscountAmount);
    EXPECT_EQ(doc.at("subtotal").at("amount").get<int32_t>(), kFoldedLineTotal);
    EXPECT_EQ(doc.at("total").at("amount").get<int32_t>(), kFoldedLineTotal);

    // A client total that ignores the fold still rejects with TOTAL_MISMATCH
    json mismatchBody = body;
    mismatchBody["total"]["amount"] = kFoldedLineTotal - 1;
    const std::string mismatchResult = Route("POST", kOrdersPath, mismatchBody.dump());
    EXPECT_NE(mismatchResult.find("TOTAL_MISMATCH"), std::string::npos)
        << "Expected TOTAL_MISMATCH for unfolded client total, got: " << mismatchResult;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 1)
        << "Only the folded-total order may persist";
}

///
/// Money currencies that violate the generated 3-character contract
/// constraint (2-char order total currency, 4-char item price currency) are
/// rejected with INVALID_REQUEST — the generated validate() now runs on the
/// parsed create DTOs (WR-05).
///
TEST_F(PosHandlersTest, CreateHandlersRejectInvalidCurrencyLength)
{
    const std::string orderBody = R"({
        "status": "draft",
        "channel": "pos",
        "fulfillment_type": "pickup",
        "total": {"amount": 1000, "currency": ")" + kShortCurrency + R"("},
        "lines": [{
            "product_id": "cccccccccccccccccccccccccccccccc",
            "quantity": 1,
            "unit_price": {"amount": 1000, "currency": "USD"},
            "line_total": {"amount": 1000, "currency": "USD"}
        }]
    })";

    const std::string orderResult = Route("POST", kOrdersPath, orderBody);
    EXPECT_NE(orderResult.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST for 2-char total currency, got: " << orderResult;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Order with invalid currency must not persist";

    const std::string itemBody = R"({
        "category_id": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
        "name": "bad-currency-item",
        "price": {"amount": 1000, "currency": ")" + kLongCurrency + R"("},
        "status": "active"
    })";

    const std::string itemResult = Route("POST", kMenuItemsPath, itemBody);
    EXPECT_NE(itemResult.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST for 4-char price currency, got: " << itemResult;

    EXPECT_EQ(ListAsJson(kMenuItemsPath).at("data").size(), 0)
        << "Item with invalid currency must not persist";
}

///
/// An order whose line money matches the stored item currency but whose
/// order-level total carries a different currency is rejected with
/// INVALID_REQUEST — mixed-currency money documents must not persist (CR-02).
///
TEST_F(PosHandlersTest, OrderCreateRejectsTotalCurrencyMismatch)
{
    const std::string itemId = CreateMenuItem("usd-latte", kItemPrice, kUsdCurrency);
    const std::string body = R"({
        "status": "draft",
        "channel": "pos",
        "fulfillment_type": "pickup",
        "total": {"amount": 2000, "currency": ")" + kEuroCurrency + R"("},
        "lines": [{
            "product_id": ")" + itemId + R"(",
            "quantity": 2,
            "unit_price": {"amount": 1000, "currency": "USD"},
            "line_total": {"amount": 2000, "currency": "USD"}
        }]
    })";

    const std::string result = Route("POST", kOrdersPath, body);
    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;
    EXPECT_NE(result.find("total currency must match"), std::string::npos)
        << "Expected the order-currency message, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Mixed-currency order must not persist";
}

///
/// Negative and zero line quantities are rejected with INVALID_REQUEST and
/// nothing is persisted — this path must never produce negative (refund) or
/// zero-priced money (CR-03).
///
TEST_F(PosHandlersTest, OrderCreateRejectsNonPositiveQuantity)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    const std::vector<double> invalidQuantities = { -1.0, 0.0 };

    for (const double quantity : invalidQuantities)
    {
        const std::string body = R"({
            "status": "draft",
            "channel": "pos",
            "fulfillment_type": "pickup",
            "total": {"amount": )" + std::to_string(kRecomputedTotal) + R"(, "currency": "USD"},
            "lines": [{
                "product_id": ")" + itemId + R"(",
                "quantity": )" + std::to_string(quantity) + R"(,
                "unit_price": {"amount": 1000, "currency": "USD"},
                "line_total": {"amount": )" + std::to_string(kRecomputedTotal) + R"(, "currency": "USD"}
            }]
        })";

        const std::string result = Route("POST", kOrdersPath, body);
        EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
            << "Expected INVALID_REQUEST for quantity " << quantity << ", got: " << result;
        EXPECT_NE(result.find("quantity must be positive"), std::string::npos)
            << "Expected the quantity message for quantity " << quantity << ", got: " << result;
    }

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Non-positive-quantity orders must not persist";
}

///
/// Negative line-level adjustment amounts (line tax_total or line
/// discount_total) are rejected with INVALID_REQUEST and nothing persists —
/// a negative line tax persists negative line/subtotal/total money (the
/// refund document this path must never produce) and a negative line
/// discount acts as a hidden surcharge; adjustment amounts are semantically
/// non-negative money (CR-04).
///
TEST_F(PosHandlersTest, OrderCreateRejectsNegativeLineAdjustments)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);

    json baseLine;
    baseLine["product_id"] = itemId;
    baseLine["quantity"]   = kMatchQty;
    baseLine["unit_price"] = json{{"amount", kWrongClientUnitPrice}, {"currency", kUsdCurrency}};
    baseLine["line_total"] = json{{"amount", kWrongClientLineTotal}, {"currency", kUsdCurrency}};

    // The CR-04 attack shape: line tax -5000 on price x qty = 2000 (the
    // finding persisted total -4000), plus the negative-discount surcharge
    json negativeTaxLine = baseLine;
    negativeTaxLine["tax_total"] =
        json{{"amount", -kNegativeAdjustmentAmount}, {"currency", kUsdCurrency}};
    json negativeDiscountLine = baseLine;
    negativeDiscountLine["discount_total"] =
        json{{"amount", -kLineDiscountAmount}, {"currency", kUsdCurrency}};

    for (const json& line : { negativeTaxLine, negativeDiscountLine })
    {
        json body;
        body["status"]           = kDraftStatus;
        body["channel"]          = kPosChannel;
        body["fulfillment_type"] = kPickupType;
        body["total"]            = json{{"amount", kRecomputedTotal}, {"currency", kUsdCurrency}};
        body["lines"]            = json::array({ line });

        const std::string result = Route("POST", kOrdersPath, body.dump());
        EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
            << "Expected INVALID_REQUEST for a negative line adjustment, got: " << result;
        EXPECT_NE(result.find("Line adjustments must be non-negative"), std::string::npos)
            << "Expected the negative-line-adjustment message, got: " << result;
    }

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Negative-line-adjustment orders must not persist";
}

///
/// Negative order-level adjustment amounts (tax_total, tip_total, or
/// discount_total) are rejected with INVALID_REQUEST and nothing persists —
/// e.g. subtotal 2000 + tax -5000 would persist a total of -3000 (CR-04).
///
TEST_F(PosHandlersTest, OrderCreateRejectsNegativeOrderAdjustments)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);

    json line;
    line["product_id"] = itemId;
    line["quantity"]   = kMatchQty;
    line["unit_price"] = json{{"amount", kWrongClientUnitPrice}, {"currency", kUsdCurrency}};
    line["line_total"] = json{{"amount", kWrongClientLineTotal}, {"currency", kUsdCurrency}};

    const std::vector<std::pair<std::string, int32_t>> negativeAdjustments =
    {
        { "tax_total",      -kNegativeAdjustmentAmount },
        { "tip_total",      -kNegativeTipAmount },
        { "discount_total", -kLineDiscountAmount },
    };

    for (const auto& adjustment : negativeAdjustments)
    {
        json body;
        body["status"]           = kDraftStatus;
        body["channel"]          = kPosChannel;
        body["fulfillment_type"] = kPickupType;
        body["total"]            = json{{"amount", kRecomputedTotal}, {"currency", kUsdCurrency}};
        body[adjustment.first]   =
            json{{"amount", adjustment.second}, {"currency", kUsdCurrency}};
        body["lines"]            = json::array({ line });

        const std::string result = Route("POST", kOrdersPath, body.dump());
        EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
            << "Expected INVALID_REQUEST for a negative order-level "
            << adjustment.first << ", got: " << result;
        EXPECT_NE(result.find("Order adjustments must be non-negative"), std::string::npos)
            << "Expected the negative-order-adjustment message, got: " << result;
    }

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Negative-order-adjustment orders must not persist";
}

///
/// Adjustments exceeding the amount they adjust are rejected even though
/// every input amount is non-negative: a line discount larger than the
/// price x quantity term and an order discount larger than the order amount
/// both drive the computed money negative, which this path must never
/// persist (CR-04).
///
TEST_F(PosHandlersTest, OrderCreateRejectsAdjustmentsExceedingAmount)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);

    json plainLine;
    plainLine["product_id"] = itemId;
    plainLine["quantity"]   = kMatchQty;
    plainLine["unit_price"] = json{{"amount", kWrongClientUnitPrice}, {"currency", kUsdCurrency}};
    plainLine["line_total"] = json{{"amount", kWrongClientLineTotal}, {"currency", kUsdCurrency}};

    // Line-level: discount 9000 against a price x qty term of 2000
    json excessiveLine = plainLine;
    excessiveLine["discount_total"] =
        json{{"amount", kExcessAdjustmentAmount}, {"currency", kUsdCurrency}};

    json lineBody;
    lineBody["status"]           = kDraftStatus;
    lineBody["channel"]          = kPosChannel;
    lineBody["fulfillment_type"] = kPickupType;
    lineBody["total"]            = json{{"amount", kRecomputedTotal}, {"currency", kUsdCurrency}};
    lineBody["lines"]            = json::array({ excessiveLine });

    const std::string lineResult = Route("POST", kOrdersPath, lineBody.dump());
    EXPECT_NE(lineResult.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST for a line discount exceeding the line amount, got: "
        << lineResult;
    EXPECT_NE(lineResult.find("Line adjustments exceed the line amount"), std::string::npos)
        << "Expected the excessive-line-adjustment message, got: " << lineResult;

    // Order-level: discount 9000 against subtotal 2000 (no line adjustments)
    json orderBody;
    orderBody["status"]           = kDraftStatus;
    orderBody["channel"]          = kPosChannel;
    orderBody["fulfillment_type"] = kPickupType;
    orderBody["total"]            = json{{"amount", kRecomputedTotal}, {"currency", kUsdCurrency}};
    orderBody["discount_total"]   =
        json{{"amount", kExcessAdjustmentAmount}, {"currency", kUsdCurrency}};
    orderBody["lines"]            = json::array({ plainLine });

    const std::string orderResult = Route("POST", kOrdersPath, orderBody.dump());
    EXPECT_NE(orderResult.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST for an order discount exceeding the order amount, got: "
        << orderResult;
    EXPECT_NE(orderResult.find("Order total must be non-negative"), std::string::npos)
        << "Expected the negative-order-total message, got: " << orderResult;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Excessive-adjustment orders must not persist";
}

///
/// Line-level adjustment currencies that differ from the stored item's
/// currency are rejected with INVALID_REQUEST and nothing persists — the
/// WR-03 fold made tax_total/discount_total load-bearing, so their currency
/// must be pinned to the item currency like unit_price, which the generated
/// OrderLine::validate() never reaches (WR-06).
///
TEST_F(PosHandlersTest, OrderCreateRejectsLineAdjustmentCurrencyMismatch)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);

    json baseLine;
    baseLine["product_id"] = itemId;
    baseLine["quantity"]   = kMatchQty;
    baseLine["unit_price"] = json{{"amount", kWrongClientUnitPrice}, {"currency", kUsdCurrency}};
    baseLine["line_total"] = json{{"amount", kWrongClientLineTotal}, {"currency", kUsdCurrency}};

    // Adjustment amounts are ordinary values; only their currency is wrong,
    // so the rejection can only come from the adjustment currency checks
    json eurTaxLine = baseLine;
    eurTaxLine["tax_total"] = json{{"amount", kLineTaxAmount}, {"currency", kEuroCurrency}};
    json eurDiscountLine = baseLine;
    eurDiscountLine["discount_total"] =
        json{{"amount", kLineDiscountAmount}, {"currency", kEuroCurrency}};

    const std::vector<std::pair<json, std::string>> mismatchCases =
    {
        { eurTaxLine,      "Line tax currency must match" },
        { eurDiscountLine, "Line discount currency must match" },
    };

    for (const auto& mismatch : mismatchCases)
    {
        json body;
        body["status"]           = kDraftStatus;
        body["channel"]          = kPosChannel;
        body["fulfillment_type"] = kPickupType;
        body["total"]            = json{{"amount", kRecomputedTotal}, {"currency", kUsdCurrency}};
        body["lines"]            = json::array({ mismatch.first });

        const std::string result = Route("POST", kOrdersPath, body.dump());
        EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
            << "Expected INVALID_REQUEST for a line adjustment currency mismatch, got: " << result;
        EXPECT_NE(result.find(mismatch.second), std::string::npos)
            << "Expected the line-adjustment-currency message, got: " << result;
    }

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Mixed-currency line adjustments must not persist";
}

///
/// Order-level adjustment currencies that differ from the order total's
/// currency are rejected with INVALID_REQUEST and nothing persists —
/// tax_total/tip_total/discount_total fold into the recomputed total, so
/// they must be denominated in the order currency, which the per-line check
/// already pinned to the item currency (WR-06).
///
TEST_F(PosHandlersTest, OrderCreateRejectsOrderAdjustmentCurrencyMismatch)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);

    json line;
    line["product_id"] = itemId;
    line["quantity"]   = kMatchQty;
    line["unit_price"] = json{{"amount", kWrongClientUnitPrice}, {"currency", kUsdCurrency}};
    line["line_total"] = json{{"amount", kWrongClientLineTotal}, {"currency", kUsdCurrency}};

    const std::vector<std::string> adjustmentFields =
    {
        "tax_total",
        "tip_total",
        "discount_total",
    };

    for (const std::string& adjustmentField : adjustmentFields)
    {
        json body;
        body["status"]            = kDraftStatus;
        body["channel"]           = kPosChannel;
        body["fulfillment_type"]  = kPickupType;
        body["total"]             = json{{"amount", kRecomputedTotal}, {"currency", kUsdCurrency}};
        body[adjustmentField]     = json{{"amount", kLineTaxAmount}, {"currency", kEuroCurrency}};
        body["lines"]             = json::array({ line });

        const std::string result = Route("POST", kOrdersPath, body.dump());
        EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
            << "Expected INVALID_REQUEST for an order-level " << adjustmentField
            << " currency mismatch, got: " << result;
        EXPECT_NE(result.find("Order adjustment currency must match"), std::string::npos)
            << "Expected the order-adjustment-currency message, got: " << result;
    }

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Mixed-currency order adjustments must not persist";
}

///
/// Quantities whose price x quantity product would leave std::llround's
/// int64 domain (1e18 against a 1000-unit price, and 1e300 outright) are
/// rejected cleanly with INVALID_REQUEST and nothing persists — llround's
/// return value is unspecified for out-of-range arguments, so the bound is
/// enforced before the rounding, not after it (WR-07).
///
TEST_F(PosHandlersTest, OrderCreateRejectsHugeQuantity)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    const std::vector<double> hugeQuantities = { kHugeQuantity, kAbsurdQuantity };

    for (const double quantity : hugeQuantities)
    {
        const std::string body = R"({
            "status": "draft",
            "channel": "pos",
            "fulfillment_type": "pickup",
            "total": {"amount": 1, "currency": "USD"},
            "lines": [{
                "product_id": ")" + itemId + R"(",
                "quantity": )" + std::to_string(quantity) + R"(,
                "unit_price": {"amount": 1, "currency": "USD"},
                "line_total": {"amount": 1, "currency": "USD"}
            }]
        })";

        const std::string result = Route("POST", kOrdersPath, body);
        EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
            << "Expected INVALID_REQUEST for quantity " << std::to_string(quantity)
            << ", got: " << result;
        EXPECT_NE(result.find("quantity out of range"), std::string::npos)
            << "Expected the quantity-range message for quantity "
            << std::to_string(quantity) << ", got: " << result;
    }

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Huge-quantity orders must not persist";
}

///
/// A syntactically invalid JSON body returns the INVALID_REQUEST envelope —
/// the override's json::exception discipline (Pitfall 5), and PARSE_ERROR is
/// absent, proving the priority-200 override answered, not the stub (T-02-04).
///
TEST_F(PosHandlersTest, OrderCreateRejectsMalformedBody)
{
    const std::string result = Route("POST", kOrdersPath, "not-json");

    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;
    EXPECT_EQ(result.find("PARSE_ERROR"), std::string::npos)
        << "The override (not the generated stub) must answer this route";
}

///
/// An order with an absent or empty lines array is rejected with
/// INVALID_REQUEST and nothing is persisted — a lineless order has no
/// server-derived pricing, so its total would be fully client-controlled
/// (CR-01, D-01/T-02-01).
///
TEST_F(PosHandlersTest, OrderCreateRejectsEmptyLines)
{
    const std::string itemId = CreateMenuItem("latte", kItemPrice, kUsdCurrency);
    (void)itemId;

    // Absent lines and an explicitly empty lines array are both rejected
    const std::vector<std::string> linelessBodies =
    {
        R"({
            "status": "draft",
            "channel": "pos",
            "fulfillment_type": "pickup",
            "total": {"amount": 999999, "currency": "USD"}
        })",
        R"({
            "status": "draft",
            "channel": "pos",
            "fulfillment_type": "pickup",
            "total": {"amount": 999999, "currency": "USD"},
            "lines": []
        })",
    };

    for (const std::string& body : linelessBodies)
    {
        const std::string result = Route("POST", kOrdersPath, body);
        EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
            << "Expected INVALID_REQUEST, got: " << result;
        EXPECT_NE(result.find("at least one line"), std::string::npos)
            << "Expected the empty-lines message, got: " << result;
    }

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Lineless orders must not persist";
}

///
/// A stored item price at INT32_MAX with quantity 2 trips the int32 overflow
/// guard and returns the INVALID_REQUEST envelope (D-01 overflow discipline).
///
TEST_F(PosHandlersTest, OrderCreateRejectsOverflowingAmount)
{
    const std::string itemId = CreateMenuItem("golden-soup", kOverflowSeedPrice, kUsdCurrency);
    const std::string body = R"({
        "status": "draft",
        "channel": "pos",
        "fulfillment_type": "pickup",
        "total": {"amount": 1, "currency": "USD"},
        "lines": [{
            "product_id": ")" + itemId + R"(",
            "quantity": 2,
            "unit_price": {"amount": 1, "currency": "USD"},
            "line_total": {"amount": 1, "currency": "USD"}
        }]
    })";

    const std::string result = Route("POST", kOrdersPath, body);
    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST from the overflow guard, got: " << result;
    EXPECT_NE(result.find("Amount overflow"), std::string::npos)
        << "Expected the overflow-guard message, got: " << result;

    m_ctx.queryString = "";
    EXPECT_EQ(ListAsJson(kOrdersPath).at("data").size(), 0)
        << "Overflowing order must not persist";
}
