/**
 * @file       test_pos_handlers.cpp
 * @brief      Phase 2 (D-04) behavior suite for the POS override handlers
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * One Google Test file (D-04) dispatching through PluginManager::Route() over
 * a fresh RocksDB temp-dir fixture, exercising the 8 override routes exactly
 * as production dispatch does: the three restaurant menu lists (cursor paging
 * edges, D-07/D-09), the five create endpoints (POST -> list round-trip, D-02
 * INVALID_REFERENCE, D-08 strict inline modifiers, D-01 order recompute), the
 * D-03 tenant filter, and HANDLER-06 bearer rejection with an empty-userId
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
static constexpr int32_t           kItemPrice          = 1000;   ///< Minor units of the canonical seeded item
static constexpr int32_t           kWrongClientUnitPrice = 1;    ///< Deliberately wrong client line money
static constexpr int32_t           kWrongClientLineTotal = 1;    ///< Deliberately wrong client line money
static constexpr int32_t           kModifierDelta      = 50;     ///< Minor units of the inline test modifier
static constexpr int32_t           kFractionalPrice    = 101;    ///< 101 x 0.5 = 50.5 rounds half-up to 51
static constexpr double            kHalfQuantity       = 0.5;    ///< Fractional order quantity
static constexpr int32_t           kHalfUpLineTotal    = 51;     ///< llround(101 * 0.5)
static constexpr int32_t           kMatchQty           = 2;      ///< Quantity for the recompute/mismatch tests
static constexpr int32_t           kRecomputedTotal    = kItemPrice * kMatchQty;  ///< Server-recomputed order total
static constexpr int32_t           kOverflowSeedPrice  = std::numeric_limits<int32_t>::max();  ///< Overflow-guard seed
static constexpr int32_t           kOverflowQty        = 2;      ///< INT32_MAX x 2 overflows int32
static constexpr size_t            kUuidHexLength      = 32;     ///< Minted ids are 32 hex chars
static constexpr unsigned int      kMaxTraversalPages  = kSeedCount;  ///< Safety bound for cursor loops

static const std::string kDefaultTenant  = "default";
static const std::string kOtherTenant    = "other";
static const std::string kUsdCurrency    = "USD";
static const std::string kEuroCurrency   = "EUR";
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
/// one of the 8 override routes (HANDLER-06 — the evidence for success
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

    // Round-trip through the generated dump-all orders list stub (still live
    // at GET /api/v1/orders — see the route note in the file header)
    m_ctx.queryString = "";
    const json page = ListAsJson(kOrdersPath);
    ASSERT_EQ(page.at("data").size(), 1);
    EXPECT_EQ(page.at("data")[0].at("id").get<std::string>(), orderId);
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
