/**
 * @file       test_table_handlers.cpp
 * @brief      Phase 3.1 (TBL-03..06) behavior suite for the table handlers
 * @date       2026-09-20
 * @author     Kenneth L. Hurley
 *
 * One Google Test file dispatching through PluginManager::Route() over a
 * fresh RocksDB temp-dir fixture, proving every table handler behavior
 * locked in plan 03.1-02: the SEAT event rules (full transition, reseat
 * party change keeping opened_at, zero-party rejection, unknown/other-tenant
 * NOT_FOUND, no pos_status regression after a linked order), the updateTable
 * contract (floor-plan metadata persistence, contract-field application with
 * updated_at advance, the BUS/RESET compensating event, non-contract key
 * rejection, other-tenant NOT_FOUND), listTables (tenant filter + keyset
 * paging, invalid cursor, unauthenticated rejection), createTable (lifecycle
 * defaults, lifecycle-key rejection, missing-capacity rejection), and the
 * orders_create table linkage (open_order_ids append + order_placed, second
 * order append, dangling/other-tenant table_id rejection). Group 6 (TBL-06)
 * asserts the committed dev.json table seeds and the applier's match-POST
 * idempotency through the real handlers. Group 7 (CR-01) proves the
 * tables_delete override: a cross-tenant DELETE is indistinguishable from a
 * missing id and leaves the row stored, while the owning tenant's DELETE
 * succeeds with the generated stub shape and removes the row.
 *
 * Route note (deferred-items.md): the orders create override lives at
 * /api/v1/orders (commerce spec paths carry no /commerce segment);
 * restaurant routes DO carry the /restaurant segment. By-id table routes
 * substitute the REAL minted id — Route() matches the concrete path against
 * the {tableId} pattern. Dispatch is synchronous — plain sequential
 * asserts, no condition variables, no sleeps. The dev database at
 * {exeDir}/data/db is never touched: every test owns a throwaway RocksDB
 * temp dir.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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

static constexpr size_t  kUuidHexLength       = 32;   ///< Minted ids are 32 hex chars
static constexpr int32_t kFixtureCapacity     = 4;    ///< Capacity of the canonical fixture table
static constexpr int32_t kPatchedCapacity     = 6;    ///< Capacity after the contract-field PATCH
static constexpr int32_t kZeroPartySize       = 0;    ///< Seat rejection boundary
static constexpr int32_t kSeatPartyTwo        = 2;    ///< First-seat party size
static constexpr int32_t kSeatPartyThree      = 3;    ///< Post-order re-seat party size
static constexpr int32_t kSeatPartyFour       = 4;    ///< Full-transition party size
static constexpr int32_t kSeatPartyFive       = 5;    ///< Reseat party size
static constexpr int32_t kItemPriceAmount     = 1000; ///< Minor units of the seeded order item
static constexpr double  kOrderQuantity       = 2.0;  ///< Line quantity of every linked order
static constexpr int32_t kRecomputedTotal     = kItemPriceAmount * static_cast<int32_t>(kOrderQuantity);  ///< D-01 expected total
static constexpr double  kFloorPlanX          = 0.42; ///< Floor-plan metadata x in [0,1]
static constexpr double  kFloorPlanY          = 0.67; ///< Floor-plan metadata y in [0,1]
static constexpr size_t  kListSeedCount       = 5;    ///< Default-tenant rows seeded for the list tests
static constexpr size_t  kOtherTenantSeedCount = 2;   ///< Other-tenant rows seeded for the list tests
static constexpr unsigned int kListPageSize   = 2;    ///< Explicit page size for the paging test
static constexpr unsigned int kMaxTraversalPages = 6; ///< Safety bound for cursor loops
static constexpr size_t  kOpenOrderIdsFirstCount  = 1;  ///< open_order_ids after the first linked order
static constexpr size_t  kOpenOrderIdsSecondCount = 2;  ///< open_order_ids after the second linked order
static constexpr size_t  kStoredTableCountOne = 1;    ///< Table rows after one create
static constexpr size_t  kExpectedDevTables  = 8;   ///< dev.json declares exactly 8 positioned tables (TBL-06)
static constexpr int32_t kMinTableCapacity  = 1;   ///< Contract floor for every declared capacity
static constexpr size_t  kExpectedSections  = 2;   ///< dev.json declares exactly Main + Patio
static constexpr double  kPositionMin       = 0.0; ///< Normalized floor-plan lower bound
static constexpr double  kPositionMax       = 1.0; ///< Normalized floor-plan upper bound
static constexpr unsigned long long kTablesListLimit = 1000; ///< The applier's match-GET page size (K_TABLES_LIST_LIMIT)
static constexpr size_t  kZeroCreatedRows    = 0;   ///< Apply creates nothing when every name already matches
static constexpr size_t  kSecondApplyCreated = 0;   ///< Idempotent second apply creates nothing

static const std::string kDefaultTenant   = "default";
static const std::string kOtherTenant     = "other";
static const std::string kUsdCurrency     = "USD";
static const std::string kTestUserId      = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";  ///< Authenticated fixture user
static const std::string kDraftStatus     = "draft";
static const std::string kPosChannel      = "pos";
static const std::string kPickupType      = "pickup";
static const std::string kAvailableStatus = "available";
static const std::string kOccupiedStatus  = "occupied";
static const std::string kDirtyStatus     = "dirty";
static const std::string kPosStatusEmpty       = "empty";        ///< Server-default / bus-reset pos_status
static const std::string kPosStatusSeated      = "seated";       ///< SEAT-event pos_status
static const std::string kPosStatusOrderPlaced = "order_placed"; ///< ORDER-CREATE pos_status
static const std::string kPatchedName     = "renamed-table";    ///< Contract-field PATCH name
static const std::string kPatchedSection  = "Terrace";          ///< Contract-field PATCH section
static const std::string kMainSection     = "Main";             ///< dev.json floor-plan section
static const std::string kPatioSection    = "Patio";            ///< dev.json floor-plan section
static const std::string kOldTimestamp    = "2020-01-01T00:00:00Z";  ///< Deterministic stored created_at for the updated_at-advance proof
static const std::string kEmptyJsonObject = "{}";
static const std::string kDeletedTrueJson = "{\"deleted\":true}";  ///< Generated delete-stub success shape

// Well-formed 32-hex fixture ids (unknown refs are NOT stored anywhere)
static const std::string kMissingTableId       = "cccccccccccccccccccccccccccccccc";  ///< Well-formed but unstored
static const std::string kSeedMenuItemId       = "dddddddddddddddddddddddddddddddd";  ///< Seeded order line item
static const std::string kSeedTableId1         = "10000000000000000000000000000001";  ///< List-test seed rows (default tenant)
static const std::string kSeedTableId2         = "20000000000000000000000000000002";
static const std::string kSeedTableId3         = "30000000000000000000000000000003";
static const std::string kSeedTableId4         = "40000000000000000000000000000004";
static const std::string kSeedTableId5         = "50000000000000000000000000000005";
static const std::string kOtherTenantTableId   = "a0000000000000000000000000000001";  ///< Cross-tenant seed rows
static const std::string kOtherTenantTableId2  = "a0000000000000000000000000000002";

// Override routes under test (see the file header for the route notes)
static const std::string kTablesPath = "/api/v1/restaurant/tables";
static const std::string kOrdersPath = "/api/v1/orders";

// ============================================================================
// Test Fixture — fresh RocksDB + both plugins with their override registrations
// ============================================================================

/**
 * @brief      Route()-dispatch fixture for the table override handlers
 *
 * Clones the test_pos_handlers fixture pattern: a unique temp RocksDB dir
 * per test, a service locator carrying PluginManager + StorageEngine, and
 * the generated RestaurantPlugin/CommercePlugin registered and initialized
 * (their priority-0 stubs), followed by the two init_*_pos_overrides calls —
 * exactly what the hand-written *PluginImpl::Initialize functions do in
 * production.
 */
class TableHandlersTest : public ::testing::Test
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
        m_tempPath = base / ("test_table_handlers_" + std::to_string(timestamp));
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

        // Authenticated default tenant for all tests; the unauthenticated
        // test uses a local empty-userId context instead
        m_ctx.tenantId       = kDefaultTenant;
        m_ctx.organizationId = kDefaultTenant;
        m_ctx.userId         = kTestUserId;
        m_ctx.queryString    = "";
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
    /// The by-id table route carrying the REAL minted id (pattern dispatch)
    ///
    static std::string TablePath(const std::string& id)
    {
        return kTablesPath + "/" + id;
    }

    ///
    /// The seat sub-route carrying the REAL minted id (pattern dispatch)
    ///
    static std::string SeatPath(const std::string& id)
    {
        return TablePath(id) + "/seat";
    }

    ///
    /// Create a minimal available table through the real POST route; returns
    /// the minted id
    ///
    std::string CreateTable(const std::string& name, int32_t capacity)
    {
        json body;
        body["name"]     = name;
        body["capacity"] = capacity;
        body["status"]   = kAvailableStatus;
        return ParseId(PostJson(kTablesPath, body.dump()));
    }

    ///
    /// Dispatch a seat request with the given party size
    ///
    std::string SeatRequest(const std::string& id, int32_t partySize)
    {
        json body;
        body["party_size"] = partySize;
        return Route("POST", SeatPath(id), body.dump());
    }

    ///
    /// GET the table by id through the real route; asserts a non-error read
    ///
    json GetTableAsJson(const std::string& id)
    {
        const std::string result = Route("GET", TablePath(id));
        EXPECT_EQ(result.find("\"error\""), std::string::npos)
            << "Expected table read, got: " << result;
        return json::parse(result);
    }

    ///
    /// Read the stored table document directly from the engine; a gtest
    /// fatal assert in a non-void helper only returns from the helper, so
    /// the caller must check this result before using doc (test_locations
    /// precedent)
    ///
    [[nodiscard]] bool ReadStoredTable(const std::string& id, json& doc)
    {
        auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
        if (!keyResult.has_value())
        {
            ADD_FAILURE() << "Failed to build the table key for " << id;
            return false;
        }
        std::string stored;
        if (!m_engine->Get(keyResult.value(), stored))
        {
            ADD_FAILURE() << "No stored table row for id " << id;
            return false;
        }
        doc = json::parse(stored);
        return true;
    }

    ///
    /// Write a table document directly to storage under a chosen tenant, the
    /// way a cross-tenant writer (or an older deployment) would have left it
    /// (bypasses the API stamp)
    ///
    void PutTableDoc(const std::string& id,
                     const std::string& tenant,
                     const std::string& name)
    {
        json doc;
        doc["id"]        = id;
        doc["name"]      = name;
        doc["capacity"]  = kFixtureCapacity;
        doc["status"]    = kAvailableStatus;
        doc["tenant_id"] = tenant;
        auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
        ASSERT_TRUE(keyResult.has_value());
        ASSERT_TRUE(m_engine->Put(keyResult.value(), doc.dump()));
    }

    ///
    /// Seed the canonical order line item directly into storage — the
    /// orders_create handler resolves its price from restaurant/menu-items
    ///
    void SeedOrderMenuItem()
    {
        json item;
        item["id"]        = kSeedMenuItemId;
        item["name"]      = "order-line-item";
        item["tenant_id"] = kDefaultTenant;
        item["price"]     = json{{"amount", kItemPriceAmount}, {"currency", kUsdCurrency}};
        auto keyResult = KeyBuilder::Build("restaurant", "menu-items", kSeedMenuItemId);
        ASSERT_TRUE(keyResult.has_value());
        ASSERT_TRUE(m_engine->Put(keyResult.value(), item.dump()));
    }

    ///
    /// The canonical one-line linked-order body: Phase 2 order shape with the
    /// client total set to the D-01 recomputation so the exact-match passes
    ///
    static json MakeOrderBody(const std::string& tableId)
    {
        json line;
        line["product_id"] = kSeedMenuItemId;
        line["quantity"]   = kOrderQuantity;
        line["unit_price"] = json{{"amount", kItemPriceAmount}, {"currency", kUsdCurrency}};
        line["line_total"] = json{{"amount", kRecomputedTotal}, {"currency", kUsdCurrency}};

        json body;
        body["status"]           = kDraftStatus;
        body["channel"]          = kPosChannel;
        body["fulfillment_type"] = kPickupType;
        body["total"]            = json{{"amount", kRecomputedTotal}, {"currency", kUsdCurrency}};
        body["table_id"]         = tableId;
        body["lines"]            = json::array({ line });
        return body;
    }

    ///
    /// POST a linked order at /api/v1/orders and return the minted order id
    ///
    std::string CreateLinkedOrder(const std::string& tableId)
    {
        return ParseId(PostJson(kOrdersPath, MakeOrderBody(tableId).dump()));
    }

    ///
    /// Count the stored rows under the restaurant/tables prefix
    ///
    size_t CountStoredTables()
    {
        return m_engine->Scan(KeyBuilder::MakePrefix("restaurant", "tables")).size();
    }

    ///
    /// Count the stored rows under the commerce/orders prefix
    ///
    size_t CountStoredOrders()
    {
        return m_engine->Scan(KeyBuilder::MakePrefix("commerce", "orders")).size();
    }

    ///
    /// Parse the committed dev setup JSON (the stream open is checked — and
    /// reported — before anything about its contents is read; a gtest fatal
    /// assert in a void helper only returns from the helper, so the caller
    /// must check this result before using doc — test_locations precedent)
    ///
    [[nodiscard]] static bool LoadDevJson(json& doc)
    {
        std::ifstream stream(SETUP_DEV_JSON);
        if (!stream.is_open())
        {
            ADD_FAILURE() << "Cannot open committed dev setup JSON: " << SETUP_DEV_JSON;
            return false;
        }
        doc = json::parse(stream);
        return true;
    }

    ///
    /// True when the list envelope's data array carries a row with the name
    ///
    static bool PageContainsTableName(const json& page, const std::string& name)
    {
        for (const auto& row : page.at("data"))
        {
            if (row.contains("name") && row.at("name").get<std::string>() == name)
            {
                return true;
            }
        }
        return false;
    }

    ///
    /// Replicate the applier's client-side algorithm (scripts/apply_setup.sh
    /// ensure_table_row): per declared row, GET the list with the applier's
    /// page size (?limit=1000), match .data[] on the declared name, POST the
    /// declared row — re-serialized from dev.json, never re-typed — ONLY when
    /// absent. Returns the number of rows POSTed.
    ///
    size_t ApplyDeclaredTables()
    {
        json doc;
        if (!LoadDevJson(doc))
        {
            return kZeroCreatedRows;
        }
        size_t createdCount = 0;
        for (const auto& declared : doc.at("tables"))
        {
            m_ctx.queryString = "limit=" + std::to_string(kTablesListLimit);
            const json page = ListAsJson(kTablesPath);
            const std::string name = declared.at("name").get<std::string>();
            if (!PageContainsTableName(page, name))
            {
                const std::string response = Route("POST", kTablesPath, declared.dump());
                EXPECT_EQ(response.find("\"error\""), std::string::npos)
                    << "Expected table seed create success, got: " << response;
                ++createdCount;
            }
        }
        m_ctx.queryString = "";
        return createdCount;
    }
};

// ============================================================================
// GROUP 1 — seatTable (TBL-04 SEAT event rules)
// ============================================================================

///
/// Seating an empty table drives the full SEAT transition: the response AND
/// a follow-up GET show guest_count 4, pos_status seated, status occupied,
/// server_id equal to the JWT-verified caller, and a non-empty opened_at.
///
TEST_F(TableHandlersTest, SeatSeatsEmptyTableWithFullTransition)
{
    const std::string id = CreateTable("t-full-transition", kFixtureCapacity);

    const json seatDoc = json::parse(SeatRequest(id, kSeatPartyFour));
    EXPECT_EQ(seatDoc.at("guest_count").get<int32_t>(), kSeatPartyFour);
    EXPECT_EQ(seatDoc.at("pos_status").get<std::string>(), kPosStatusSeated);
    EXPECT_EQ(seatDoc.at("status").get<std::string>(), kOccupiedStatus);
    EXPECT_EQ(seatDoc.at("server_id").get<std::string>(), kTestUserId);
    EXPECT_FALSE(seatDoc.at("opened_at").get<std::string>().empty());

    // Persistence proof through the real GET-by-id route
    const json stored = GetTableAsJson(id);
    EXPECT_EQ(stored.at("guest_count").get<int32_t>(), kSeatPartyFour);
    EXPECT_EQ(stored.at("pos_status").get<std::string>(), kPosStatusSeated);
    EXPECT_EQ(stored.at("status").get<std::string>(), kOccupiedStatus);
    EXPECT_EQ(stored.at("server_id").get<std::string>(), kTestUserId);
    EXPECT_FALSE(stored.at("opened_at").get<std::string>().empty());
}

///
/// Reseating with a different party size updates guest_count but keeps the
/// opened_at stamped by the FIRST seat — elapsed dining time is preserved
/// across a mid-meal party change.
///
TEST_F(TableHandlersTest, SeatReseatUpdatesPartyKeepsOpenedAt)
{
    const std::string id = CreateTable("t-reseat", kFixtureCapacity);

    const json firstSeat = json::parse(SeatRequest(id, kSeatPartyTwo));
    const std::string firstOpenedAt = firstSeat.at("opened_at").get<std::string>();
    EXPECT_FALSE(firstOpenedAt.empty());

    const json secondSeat = json::parse(SeatRequest(id, kSeatPartyFive));
    EXPECT_EQ(secondSeat.at("guest_count").get<int32_t>(), kSeatPartyFive);
    EXPECT_EQ(secondSeat.at("opened_at").get<std::string>(), firstOpenedAt)
        << "Reseat must keep the first seat's opened_at";

    const json stored = GetTableAsJson(id);
    EXPECT_EQ(stored.at("guest_count").get<int32_t>(), kSeatPartyFive);
    EXPECT_EQ(stored.at("opened_at").get<std::string>(), firstOpenedAt);
}

///
/// A zero party size rejects with INVALID_REQUEST and the stored document is
/// byte-for-byte unchanged.
///
TEST_F(TableHandlersTest, SeatZeroPartySizeRejected)
{
    const std::string id = CreateTable("t-zero-party", kFixtureCapacity);

    json before;
    ASSERT_TRUE(ReadStoredTable(id, before));

    const std::string result = SeatRequest(id, kZeroPartySize);
    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;

    json after;
    ASSERT_TRUE(ReadStoredTable(id, after));
    EXPECT_EQ(after.dump(), before.dump())
        << "Rejected seat must leave storage unchanged";
    EXPECT_EQ(after.at("pos_status").get<std::string>(), kPosStatusEmpty);
}

///
/// A seat against a well-formed but unknown table id returns NOT_FOUND.
///
TEST_F(TableHandlersTest, SeatUnknownTableNotFound)
{
    json body;
    body["party_size"] = kSeatPartyTwo;

    const std::string result =
        Route("POST", SeatPath(kMissingTableId), body.dump());
    EXPECT_NE(result.find("NOT_FOUND"), std::string::npos)
        << "Expected NOT_FOUND, got: " << result;
}

///
/// A seat against another tenant's stored table returns the SAME NOT_FOUND
/// envelope as a missing id — cross-tenant access is indistinguishable from
/// absence (T-03.1-05).
///
TEST_F(TableHandlersTest, SeatOtherTenantTableNotFound)
{
    PutTableDoc(kOtherTenantTableId, kOtherTenant, "foreign-seat");

    json body;
    body["party_size"] = kSeatPartyTwo;

    const std::string missingResult =
        Route("POST", SeatPath(kMissingTableId), body.dump());
    const std::string otherTenantResult =
        Route("POST", SeatPath(kOtherTenantTableId), body.dump());

    EXPECT_NE(otherTenantResult.find("NOT_FOUND"), std::string::npos)
        << "Expected NOT_FOUND, got: " << otherTenantResult;
    EXPECT_EQ(otherTenantResult, missingResult)
        << "Other-tenant seat must be indistinguishable from a missing table";
}

///
/// Reseating a table that already has a linked order keeps pos_status at
/// order_placed while guest_count follows the new party — the no-regression
/// rule (SEAT advances pos_status only from empty).
///
TEST_F(TableHandlersTest, SeatAfterOrderKeepsPosStatus)
{
    const std::string id = CreateTable("t-seat-after-order", kFixtureCapacity);
    SeatRequest(id, kSeatPartyTwo);

    SeedOrderMenuItem();
    const std::string orderId = CreateLinkedOrder(id);
    EXPECT_EQ(GetTableAsJson(id).at("pos_status").get<std::string>(),
              kPosStatusOrderPlaced);

    const json reseat = json::parse(SeatRequest(id, kSeatPartyThree));
    EXPECT_EQ(reseat.at("guest_count").get<int32_t>(), kSeatPartyThree);
    EXPECT_EQ(reseat.at("pos_status").get<std::string>(), kPosStatusOrderPlaced)
        << "A party change must never regress order_placed";

    const json stored = GetTableAsJson(id);
    EXPECT_EQ(stored.at("guest_count").get<int32_t>(), kSeatPartyThree);
    EXPECT_EQ(stored.at("pos_status").get<std::string>(), kPosStatusOrderPlaced);
    EXPECT_EQ(stored.at("server_id").get<std::string>(), kTestUserId);
    (void)orderId;
}

// ============================================================================
// GROUP 2 — updateTable (TBL-05 contract + BUS/RESET)
// ============================================================================

///
/// A PATCH carrying only floor-plan metadata persists the position verbatim
/// — the touch-pos metadata contract (TableUpdate metadata).
///
TEST_F(TableHandlersTest, UpdateMetadataPersistsFloorPlanPosition)
{
    const std::string id = CreateTable("t-floor-plan", kFixtureCapacity);

    json position;
    position["x"] = kFloorPlanX;
    position["y"] = kFloorPlanY;
    json metadata;
    metadata["position"] = position;

    json body;
    body["metadata"] = metadata;
    const std::string result = Route("PATCH", TablePath(id), body.dump());
    EXPECT_EQ(result.find("\"error\""), std::string::npos)
        << "Expected update success, got: " << result;

    const json stored = GetTableAsJson(id);
    const json& storedPosition = stored.at("metadata").at("position");
    EXPECT_DOUBLE_EQ(storedPosition.at("x").get<double>(), kFloorPlanX);
    EXPECT_DOUBLE_EQ(storedPosition.at("y").get<double>(), kFloorPlanY);
    // Untouched contract keys keep their created values
    EXPECT_EQ(stored.at("capacity").get<int32_t>(), kFixtureCapacity);
    EXPECT_EQ(stored.at("status").get<std::string>(), kAvailableStatus);
}

///
/// A PATCH of name/section/capacity applies all three contract fields and
/// advances updated_at past the stored created_at. The stored created_at is
/// aged to a fixed old timestamp first — GetCurrentTimestamp has one-second
/// resolution, so a same-run create+patch pair would otherwise compare equal
/// and the advance assertion would be timing-dependent.
///
TEST_F(TableHandlersTest, UpdateContractFieldsApply)
{
    const std::string id = CreateTable("t-contract-fields", kFixtureCapacity);

    // Age the stored audit fields deterministically (same row, older clock)
    json aged;
    ASSERT_TRUE(ReadStoredTable(id, aged));
    aged["created_at"] = kOldTimestamp;
    aged["updated_at"] = kOldTimestamp;
    auto keyResult = KeyBuilder::Build("restaurant", "tables", id);
    ASSERT_TRUE(keyResult.has_value());
    ASSERT_TRUE(m_engine->Put(keyResult.value(), aged.dump()));

    json body;
    body["name"]     = kPatchedName;
    body["section"]  = kPatchedSection;
    body["capacity"] = kPatchedCapacity;
    const std::string result = Route("PATCH", TablePath(id), body.dump());
    EXPECT_EQ(result.find("\"error\""), std::string::npos)
        << "Expected update success, got: " << result;

    const json stored = GetTableAsJson(id);
    EXPECT_EQ(stored.at("name").get<std::string>(), kPatchedName);
    EXPECT_EQ(stored.at("section").get<std::string>(), kPatchedSection);
    EXPECT_EQ(stored.at("capacity").get<int32_t>(), kPatchedCapacity);
    EXPECT_EQ(stored.at("created_at").get<std::string>(), kOldTimestamp);
    const std::string updated_at = stored.at("updated_at").get<std::string>();
    EXPECT_GT(updated_at, kOldTimestamp)
        << "PATCH must refresh updated_at past the stored created_at";
}

///
/// PATCHing status to dirty is the BUS/RESET vehicle: a seated table with an
/// open order resets to pos_status empty, guest_count/server_id/opened_at
/// null, and an empty open_order_ids — the compensating unseat event.
///
TEST_F(TableHandlersTest, UpdateStatusDirtyResetsLifecycle)
{
    const std::string id = CreateTable("t-bus-reset", kFixtureCapacity);
    SeatRequest(id, kSeatPartyFour);
    SeedOrderMenuItem();
    const std::string orderId = CreateLinkedOrder(id);

    const json seated = GetTableAsJson(id);
    EXPECT_EQ(seated.at("pos_status").get<std::string>(), kPosStatusOrderPlaced);
    EXPECT_EQ(seated.at("open_order_ids").size(), kOpenOrderIdsFirstCount);

    json body;
    body["status"] = kDirtyStatus;
    const std::string result = Route("PATCH", TablePath(id), body.dump());
    EXPECT_EQ(result.find("\"error\""), std::string::npos)
        << "Expected update success, got: " << result;

    const json stored = GetTableAsJson(id);
    EXPECT_EQ(stored.at("status").get<std::string>(), kDirtyStatus);
    EXPECT_EQ(stored.at("pos_status").get<std::string>(), kPosStatusEmpty);
    EXPECT_TRUE(stored.at("guest_count").is_null());
    EXPECT_TRUE(stored.at("server_id").is_null());
    EXPECT_TRUE(stored.at("opened_at").is_null());
    ASSERT_TRUE(stored.at("open_order_ids").is_array());
    EXPECT_TRUE(stored.at("open_order_ids").empty());
    (void)orderId;
}

///
/// A PATCH carrying a lifecycle key (pos_status, or the open_order_ids
/// variant) rejects with INVALID_REQUEST and storage is unchanged —
/// lifecycle fields are server-event-only (T-03.1-04).
///
TEST_F(TableHandlersTest, UpdateRejectsNonContractKey)
{
    const std::string id = CreateTable("t-non-contract", kFixtureCapacity);
    SeatRequest(id, kSeatPartyTwo);

    json before;
    ASSERT_TRUE(ReadStoredTable(id, before));

    json posStatusBody;
    posStatusBody["pos_status"] = kPosStatusSeated;
    const std::string posStatusResult =
        Route("PATCH", TablePath(id), posStatusBody.dump());
    EXPECT_NE(posStatusResult.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST for pos_status, got: " << posStatusResult;

    json openOrdersBody;
    openOrdersBody["open_order_ids"] = json::array({ kMissingTableId });
    const std::string openOrdersResult =
        Route("PATCH", TablePath(id), openOrdersBody.dump());
    EXPECT_NE(openOrdersResult.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST for open_order_ids, got: " << openOrdersResult;

    json after;
    ASSERT_TRUE(ReadStoredTable(id, after));
    EXPECT_EQ(after.dump(), before.dump())
        << "Rejected patches must leave storage unchanged";
}

///
/// A PATCH against another tenant's stored table returns the SAME NOT_FOUND
/// envelope as a missing id, and the other-tenant row is untouched.
///
TEST_F(TableHandlersTest, UpdateOtherTenantNotFound)
{
    PutTableDoc(kOtherTenantTableId, kOtherTenant, "foreign-update");

    json body;
    body["name"] = kPatchedName;

    const std::string missingResult = Route("PATCH", TablePath(kMissingTableId), body.dump());
    const std::string otherTenantResult =
        Route("PATCH", TablePath(kOtherTenantTableId), body.dump());

    EXPECT_NE(otherTenantResult.find("NOT_FOUND"), std::string::npos)
        << "Expected NOT_FOUND, got: " << otherTenantResult;
    EXPECT_EQ(otherTenantResult, missingResult)
        << "Other-tenant update must be indistinguishable from a missing table";
}

// ============================================================================
// GROUP 3 — listTables (TBL-03 list core)
// ============================================================================

///
/// The tables list filters by tenant and pages with keyset cursors: 5
/// default-tenant rows seeded directly to storage (plus 2 other-tenant rows
/// that must never appear) traverse in pages of 2 with every page carrying
/// only default-tenant rows, and the traversal ends on a last page with
/// has_more false and no next_cursor.
///
TEST_F(TableHandlersTest, ListTenantFilterAndKeysetPaging)
{
    const std::vector<std::string> seedIds =
    {
        kSeedTableId1, kSeedTableId2, kSeedTableId3, kSeedTableId4, kSeedTableId5,
    };
    unsigned int seedIndex = 0;
    for (const std::string& seedId : seedIds)
    {
        ++seedIndex;
        PutTableDoc(seedId, kDefaultTenant, "seed-table-" + std::to_string(seedIndex));
    }
    PutTableDoc(kOtherTenantTableId,  kOtherTenant, "foreign-table-1");
    PutTableDoc(kOtherTenantTableId2, kOtherTenant, "foreign-table-2");

    std::set<std::string> seen;
    std::string cursor;
    unsigned int pages = 0;
    bool terminated = false;
    while (pages < kMaxTraversalPages)
    {
        ++pages;
        m_ctx.queryString = "limit=" + std::to_string(kListPageSize) +
                            (cursor.empty() ? "" : "&cursor=" + cursor);
        const json page = ListAsJson(kTablesPath);

        for (const auto& row : page.at("data"))
        {
            const std::string rowId = row.at("id").get<std::string>();
            EXPECT_EQ(row.at("tenant_id").get<std::string>(), kDefaultTenant)
                << "Only default-tenant rows may appear, got id " << rowId;
            seen.insert(rowId);
        }

        if (!page.at("pagination").at("has_more").get<bool>())
        {
            EXPECT_FALSE(page.at("pagination").contains("next_cursor"))
                << "Terminal page must not carry a next_cursor";
            terminated = true;
            break;
        }
        cursor = page.at("pagination").at("next_cursor").get<std::string>();
        EXPECT_FALSE(cursor.empty());
    }

    EXPECT_TRUE(terminated) << "Traversal must terminate within the safety bound";
    EXPECT_EQ(seen.size(), kListSeedCount)
        << "Every default-tenant row must be visited exactly once";
    for (const std::string& seedId : seedIds)
    {
        EXPECT_EQ(seen.count(seedId), 1u) << "Missing seeded row " << seedId;
    }
    EXPECT_EQ(seen.count(kOtherTenantTableId), 0u)
        << "Cross-tenant table must never appear";
    EXPECT_EQ(seen.count(kOtherTenantTableId2), 0u)
        << "Cross-tenant table must never appear";
    m_ctx.queryString = "";
}

///
/// A malformed (non-32-hex) cursor is rejected with INVALID_REQUEST and no
/// data envelope (D-07 posture).
///
TEST_F(TableHandlersTest, ListInvalidCursorRejected)
{
    m_ctx.queryString = "cursor=xyz";
    const std::string result = Route("GET", kTablesPath);

    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;
    EXPECT_EQ(result.find("\"data\""), std::string::npos)
        << "Envelope must not carry data, got: " << result;
    m_ctx.queryString = "";
}

///
/// An empty-userId RequestContext gets the UNAUTHORIZED envelope from the
/// tables list (HANDLER-06 defense-in-depth).
///
TEST_F(TableHandlersTest, ListUnauthenticatedRejected)
{
    RequestContext anonCtx;
    anonCtx.tenantId       = kDefaultTenant;
    anonCtx.organizationId = kDefaultTenant;
    // userId deliberately empty — no authenticated user

    const std::string result = m_pm.Route(anonCtx, "GET", kTablesPath, kEmptyJsonObject);
    EXPECT_NE(result.find("\"code\":\"UNAUTHORIZED\""), std::string::npos)
        << "Expected UNAUTHORIZED envelope, got: " << result;
}

// ============================================================================
// GROUP 4 — createTable (TBL-03 create contract)
// ============================================================================

///
/// A minimal create (name/capacity/status) stores and echoes the
/// server-defaulted lifecycle fields: pos_status empty and an empty
/// open_order_ids array.
///
TEST_F(TableHandlersTest, CreateDefaultsLifecycleFields)
{
    json body;
    body["name"]     = "t-create-defaults";
    body["capacity"] = kFixtureCapacity;
    body["status"]   = kAvailableStatus;

    const json echo = json::parse(PostJson(kTablesPath, body.dump()));
    const std::string id = echo.at("id").get<std::string>();
    EXPECT_EQ(id.size(), kUuidHexLength);
    EXPECT_EQ(echo.at("tenant_id").get<std::string>(), kDefaultTenant);
    EXPECT_EQ(echo.at("pos_status").get<std::string>(), kPosStatusEmpty);
    ASSERT_TRUE(echo.at("open_order_ids").is_array());
    EXPECT_TRUE(echo.at("open_order_ids").empty());

    // Persistence proof through the real GET-by-id route
    const json stored = GetTableAsJson(id);
    EXPECT_EQ(stored.at("pos_status").get<std::string>(), kPosStatusEmpty);
    ASSERT_TRUE(stored.at("open_order_ids").is_array());
    EXPECT_TRUE(stored.at("open_order_ids").empty());
}

///
/// A create body carrying a lifecycle key (pos_status seated) rejects with
/// INVALID_REQUEST and nothing is stored — lifecycle fields are never
/// client-writable (T-03.1-04).
///
TEST_F(TableHandlersTest, CreateRejectsLifecycleKeys)
{
    json body;
    body["name"]      = "t-create-lifecycle";
    body["capacity"]  = kFixtureCapacity;
    body["status"]    = kAvailableStatus;
    body["pos_status"] = kPosStatusSeated;

    const std::string result = Route("POST", kTablesPath, body.dump());
    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;

    EXPECT_EQ(CountStoredTables(), 0) << "Rejected table must not persist";
}

///
/// A create body missing the required capacity rejects with INVALID_REQUEST
/// and nothing is stored (generated TableCreate::from_json contract).
///
TEST_F(TableHandlersTest, CreateMissingCapacityRejected)
{
    json body;
    body["name"]   = "t-create-no-capacity";
    body["status"] = kAvailableStatus;

    const std::string result = Route("POST", kTablesPath, body.dump());
    EXPECT_NE(result.find("INVALID_REQUEST"), std::string::npos)
        << "Expected INVALID_REQUEST, got: " << result;

    EXPECT_EQ(CountStoredTables(), 0) << "Rejected table must not persist";
}

// ============================================================================
// GROUP 5 — orders_create table linkage (TBL-02)
// ============================================================================

///
/// A valid one-line order carrying table_id persists the order AND drives
/// the linked table's transition: the order id appears in open_order_ids
/// and pos_status becomes order_placed.
///
TEST_F(TableHandlersTest, OrderWithTableAppendsOpenOrderAndPlacesStatus)
{
    const std::string tableId = CreateTable("t-order-link", kFixtureCapacity);
    SeedOrderMenuItem();

    const std::string orderId = CreateLinkedOrder(tableId);
    EXPECT_EQ(orderId.size(), kUuidHexLength);

    // The order itself persists with its table link and recomputed total
    auto keyResult = KeyBuilder::Build("commerce", "orders", orderId);
    ASSERT_TRUE(keyResult.has_value());
    std::string storedOrder;
    ASSERT_TRUE(m_engine->Get(keyResult.value(), storedOrder));
    const json orderDoc = json::parse(storedOrder);
    EXPECT_EQ(orderDoc.at("table_id").get<std::string>(), tableId);
    EXPECT_EQ(orderDoc.at("total").at("amount").get<int32_t>(), kRecomputedTotal);

    // The table shows the order and is order_placed
    const json table = GetTableAsJson(tableId);
    ASSERT_EQ(table.at("open_order_ids").size(), kOpenOrderIdsFirstCount);
    EXPECT_EQ(table.at("open_order_ids")[0].get<std::string>(), orderId);
    EXPECT_EQ(table.at("pos_status").get<std::string>(), kPosStatusOrderPlaced);
}

///
/// A second linked order appends its id after the first — both ids present
/// in open_order_ids — and the first order's stored document is untouched.
///
TEST_F(TableHandlersTest, SecondOrderAppendsSecondId)
{
    const std::string tableId = CreateTable("t-order-second", kFixtureCapacity);
    SeedOrderMenuItem();

    const std::string firstEcho = PostJson(kOrdersPath, MakeOrderBody(tableId).dump());
    const std::string firstOrderId = ParseId(firstEcho);
    const std::string secondOrderId = CreateLinkedOrder(tableId);

    const json table = GetTableAsJson(tableId);
    ASSERT_EQ(table.at("open_order_ids").size(), kOpenOrderIdsSecondCount);
    EXPECT_EQ(table.at("open_order_ids")[0].get<std::string>(), firstOrderId);
    EXPECT_EQ(table.at("open_order_ids")[1].get<std::string>(), secondOrderId);
    EXPECT_EQ(table.at("pos_status").get<std::string>(), kPosStatusOrderPlaced);

    // The first order's stored document is unchanged by the second create
    auto keyResult = KeyBuilder::Build("commerce", "orders", firstOrderId);
    ASSERT_TRUE(keyResult.has_value());
    std::string storedFirst;
    ASSERT_TRUE(m_engine->Get(keyResult.value(), storedFirst));
    EXPECT_EQ(storedFirst, firstEcho)
        << "The second linked order must not modify the first order's row";
}

///
/// An order whose table_id is well-formed but unknown rejects with
/// INVALID_REFERENCE and nothing persists: no order row appears under
/// commerce/orders and the table count is unchanged.
///
TEST_F(TableHandlersTest, OrderDanglingTableIdRejectedNothingPersisted)
{
    const std::string tableId = CreateTable("t-order-dangling", kFixtureCapacity);
    SeedOrderMenuItem();
    ASSERT_EQ(CountStoredTables(), kStoredTableCountOne);

    const std::string result =
        Route("POST", kOrdersPath, MakeOrderBody(kMissingTableId).dump());
    EXPECT_NE(result.find("INVALID_REFERENCE"), std::string::npos)
        << "Expected INVALID_REFERENCE, got: " << result;

    EXPECT_EQ(CountStoredOrders(), 0) << "Rejected order must not persist";
    EXPECT_EQ(CountStoredTables(), kStoredTableCountOne)
        << "The dangling reference must not disturb stored tables";
    (void)tableId;
}

///
/// An order whose table_id resolves to ANOTHER tenant's table rejects with
/// INVALID_REFERENCE — existence alone is not enough, the reference must
/// resolve within the caller's tenant (WR-01 posture).
///
TEST_F(TableHandlersTest, OrderOtherTenantTableIdRejected)
{
    PutTableDoc(kOtherTenantTableId, kOtherTenant, "foreign-order-target");
    SeedOrderMenuItem();

    json foreignBefore;
    ASSERT_TRUE(ReadStoredTable(kOtherTenantTableId, foreignBefore));

    const std::string result =
        Route("POST", kOrdersPath, MakeOrderBody(kOtherTenantTableId).dump());
    EXPECT_NE(result.find("INVALID_REFERENCE"), std::string::npos)
        << "Expected INVALID_REFERENCE, got: " << result;
    EXPECT_NE(result.find("another tenant"), std::string::npos)
        << "Expected the cross-tenant message, got: " << result;

    EXPECT_EQ(CountStoredOrders(), 0) << "Cross-tenant order must not persist";

    json foreignAfter;
    ASSERT_TRUE(ReadStoredTable(kOtherTenantTableId, foreignAfter));
    EXPECT_EQ(foreignAfter.dump(), foreignBefore.dump())
        << "The referenced other-tenant table must be untouched";
}

// ============================================================================
// GROUP 6 — dev.json seed expectations + applier idempotency (TBL-06)
// ============================================================================

///
/// The committed dev setup JSON declares exactly 8 tables, every one
/// available with capacity >= 1, unique names, and sections exactly
/// {Main, Patio} (TBL-06 seed contract).
///
TEST_F(TableHandlersTest, DevJsonTablesMatchContract)
{
    json doc;
    if (!LoadDevJson(doc))
    {
        return;
    }

    ASSERT_TRUE(doc.at("tables").is_array());
    const json& tables = doc.at("tables");
    ASSERT_EQ(tables.size(), kExpectedDevTables);

    std::set<std::string> names;
    std::set<std::string> sections;
    for (const auto& row : tables)
    {
        EXPECT_EQ(row.at("status").get<std::string>(), kAvailableStatus);
        EXPECT_GE(row.at("capacity").get<int32_t>(), kMinTableCapacity);
        const std::string name = row.at("name").get<std::string>();
        EXPECT_FALSE(name.empty());
        names.insert(name);
        sections.insert(row.at("section").get<std::string>());
    }

    EXPECT_EQ(names.size(), kExpectedDevTables)
        << "Declared table names must be unique";
    EXPECT_EQ(sections.size(), kExpectedSections);
    EXPECT_EQ(sections.count(kMainSection), 1u);
    EXPECT_EQ(sections.count(kPatioSection), 1u);
}

///
/// Every declared table position is a normalized floor-plan coordinate:
/// numeric x and y each within [0.0, 1.0].
///
TEST_F(TableHandlersTest, DevJsonPositionsNormalized)
{
    json doc;
    if (!LoadDevJson(doc))
    {
        return;
    }

    const json& tables = doc.at("tables");
    ASSERT_EQ(tables.size(), kExpectedDevTables);
    for (const auto& row : tables)
    {
        const json& position = row.at("metadata").at("position");
        ASSERT_TRUE(position.at("x").is_number());
        ASSERT_TRUE(position.at("y").is_number());
        const double x = position.at("x").get<double>();
        const double y = position.at("y").get<double>();
        EXPECT_GE(x, kPositionMin);
        EXPECT_LE(x, kPositionMax);
        EXPECT_GE(y, kPositionMin);
        EXPECT_LE(y, kPositionMax);
    }
}

///
/// The applier's match-POST algorithm is idempotent against the REAL plugin
/// + override handlers: applying the declared tables creates exactly 8 rows,
/// and a second apply of the same match-on-name logic creates nothing —
/// exactly 8 stored restaurant/tables rows remain, their names matching the
/// declaration with no duplicates (the Phase 3 test_locations_setup
/// idempotency pattern applied to tables).
///
TEST_F(TableHandlersTest, ApplyAlgorithmIdempotentTwoRunsEightRows)
{
    json doc;
    if (!LoadDevJson(doc))
    {
        return;
    }
    std::set<std::string> declaredNames;
    for (const auto& declared : doc.at("tables"))
    {
        declaredNames.insert(declared.at("name").get<std::string>());
    }
    ASSERT_EQ(declaredNames.size(), kExpectedDevTables);

    const size_t createdFirst = ApplyDeclaredTables();
    ASSERT_EQ(createdFirst, kExpectedDevTables);
    EXPECT_EQ(CountStoredTables(), kExpectedDevTables);

    const size_t createdSecond = ApplyDeclaredTables();
    EXPECT_EQ(createdSecond, kSecondApplyCreated)
        << "The second apply must find every declared name and POST nothing";
    EXPECT_EQ(CountStoredTables(), kExpectedDevTables)
        << "Two apply loops must leave exactly 8 rows — no duplicates";

    // The stored rows ARE the declaration — same name set, no duplicates
    const auto rows = m_engine->Scan(KeyBuilder::MakePrefix("restaurant", "tables"));
    ASSERT_EQ(rows.size(), kExpectedDevTables);
    std::set<std::string> storedNames;
    for (const auto& [key, value] : rows)
    {
        const json row = json::parse(value);
        storedNames.insert(row.at("name").get<std::string>());
    }
    EXPECT_EQ(storedNames.size(), kExpectedDevTables)
        << "Stored table names must be unique";
    EXPECT_EQ(storedNames, declaredNames)
        << "Stored names must match the declaration exactly";
}

// ============================================================================
// GROUP 7 — deleteTable (CR-01 tenant isolation)
// ============================================================================

///
/// A DELETE from another tenant returns the SAME NOT_FOUND envelope as a
/// missing id (byte-equality) and leaves the row stored — cross-tenant
/// deletes are indistinguishable from absence (T-03.1-05) — while the owning
/// tenant's DELETE succeeds with the generated stub success shape and the
/// row is gone afterwards (follow-up GET is NOT_FOUND). This is the CR-01
/// regression: the route must never fall through to the tenant-blind
/// priority-0 stub.
///
TEST_F(TableHandlersTest, DeleteOtherTenantNotFoundThenOwnerSucceeds)
{
    const std::string id = CreateTable("t-delete-tenant", kFixtureCapacity);

    // Tenant B: the foreign delete is indistinguishable from a missing id
    m_ctx.tenantId       = kOtherTenant;
    m_ctx.organizationId = kOtherTenant;
    const std::string missingResult     = Route("DELETE", TablePath(kMissingTableId));
    const std::string otherTenantResult = Route("DELETE", TablePath(id));
    EXPECT_NE(otherTenantResult.find("NOT_FOUND"), std::string::npos)
        << "Expected NOT_FOUND, got: " << otherTenantResult;
    EXPECT_EQ(otherTenantResult, missingResult)
        << "Other-tenant delete must be indistinguishable from a missing table";
    m_ctx.tenantId       = kDefaultTenant;
    m_ctx.organizationId = kDefaultTenant;

    // The rejected delete left the row readable through the real GET route
    EXPECT_EQ(GetTableAsJson(id).at("id").get<std::string>(), id)
        << "A rejected cross-tenant delete must leave the row stored";

    // The owning tenant's delete succeeds with the stub contract shape
    const std::string deleteResult = Route("DELETE", TablePath(id));
    EXPECT_EQ(deleteResult, kDeletedTrueJson)
        << "Delete must return the generated stub success shape";

    // The row is gone — the by-id GET is now NOT_FOUND
    const std::string getResult = Route("GET", TablePath(id));
    EXPECT_NE(getResult.find("NOT_FOUND"), std::string::npos)
        << "Expected NOT_FOUND after delete, got: " << getResult;
}
