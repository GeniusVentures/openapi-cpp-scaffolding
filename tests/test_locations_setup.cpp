/**
 * @file       test_locations_setup.cpp
 * @brief      Phase 3 (TAX-01..03) location tax-rate contract + setup suite
 * @date       2026-09-16
 * @author     Kenneth L. Hurley
 *
 * Three groups, all driven by the committed parent-repo dev setup JSON
 * (setup/environments/dev.json — the single expected-values source, baked
 * in at configure time via the SETUP_DEV_JSON compile definition; the
 * stream open is asserted before any value is read, T-03-09):
 *
 *   1. TAX-03 JSON expectations — dev.json declares exactly one location,
 *      status "active", tax_rate 9.25 within [0,100], California address.
 *   2. TAX-02 handler-level apply idempotency — replicating the applier's
 *      client-side algorithm (GET the list, match .data[] on the declared
 *      code, POST the declared row ONLY when absent) against the REAL
 *      generated LocationsPlugin over a fresh RocksDB temp dir: applying
 *      twice leaves exactly one stored row for the code; a raw duplicate
 *      POST bypassing the match mints a fresh id — why the client-side
 *      matching is mandatory (createLocation never upserts).
 *   3. TAX-01 model round-trip — the regenerated Location model parses
 *      tax_rate as double with IsSet semantics and to_json re-emits it.
 *
 * Dispatch is synchronous through PluginManager::Route() — no condition
 * variables, no sleeps. The dev database at {exeDir}/data/db is never
 * touched: every test runs on a throwaway temp dir (T-03-11). No
 * credentials, tokens, or network appear here (T-03-10).
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

// Generated plugin base class — header-inline; NEVER include the
// locations_plugin_impl.cpp file here (EXPORT_PLUGIN would be defined twice)
#include "locations_plugin.hpp"

#include "model/Location.h"
#include "singleton/PluginManager.hpp"
#include "singleton/CServiceLocator.hpp"
#include "storage/RocksDBEngine.hpp"
#include "storage/KeyBuilder.hpp"

using json = nlohmann::json;

namespace fs = std::filesystem;

namespace model = org::openapitools::server::model;

// ============================================================================
// Test constants (no magic numbers)
// ============================================================================

static constexpr double kExpectedTaxRate       = 9.25;   ///< D-01 seed percent asserted everywhere (dev.json carries it)
static constexpr double kTaxRateMin            = 0.0;    ///< Contract lower bound (spec minimum)
static constexpr double kTaxRateMax            = 100.0;  ///< Contract upper bound (spec maximum)
static constexpr size_t kExpectedLocationCount = 1;      ///< D-03: exactly one dev location
static constexpr size_t kDuplicateRowCount     = 2;      ///< Rows for the code after a raw unmatched duplicate POST
static constexpr size_t kErasedKeyCount        = 1;      ///< json::erase return when the key was present
static constexpr size_t kCountryCodeLength     = 2;      ///< ISO 3166-1 alpha-2 (Address.country)
static constexpr size_t kUuidHexLength         = 32;     ///< Minted ids are 32 hex chars

static const std::string kDefaultTenant        = "default";
static const std::string kActiveStatus         = "active";
static const std::string kCaliforniaRegion     = "CA";
static const std::string kUnitedStatesCountry  = "US";
static const std::string kTestUserId           = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";  ///< Authenticated fixture user
static const std::string kModelFixtureId       = "11111111111111111111111111111111";  ///< 32-hex model-body fixture id
static const std::string kTestTimestamp        = "2026-09-16T00:00:00Z";

static const std::string kLocationsPath = "/api/v1/locations";

// ============================================================================
// Test Fixture — fresh RocksDB temp dir + the generated LocationsPlugin
// ============================================================================

/**
 * @brief      Route()-dispatch fixture for the locations setup contract
 *
 * Clones the test_pos_handlers fixture pattern: a unique temp RocksDB dir
 * per test, a service locator carrying PluginManager + StorageEngine, and
 * the generated LocationsPlugin registered and initialized — there is NO
 * derived/override class for locations; LocationsPlugin IS the served
 * handler (stubs registered at the generated plugin's priority 100).
 */
class LocationsSetupTest : public ::testing::Test
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
        m_tempPath = base / ("test_locations_setup_" + std::to_string(timestamp));
        fs::create_directories(m_tempPath);

        auto result = RocksDBEngine::Create(m_tempPath.string());
        ASSERT_TRUE(result.has_value()) << "Failed to create engine: "
                                        << result.error().message();
        m_engine = std::move(result.value());

        m_locator.RegisterService(Fnv1a("PluginManager"), &m_pm);
        m_locator.RegisterService(Fnv1a("StorageEngine"), m_engine.get());

        // Generated base IS the served handler for locations — no override
        // class exists and no init_*_overrides call is needed
        auto locations = std::make_shared<LocationsPlugin>();
        m_pm.RegisterPlugin(locations, locations->GetPriority(), locations->GetUrlPaths());
        ASSERT_TRUE(locations->Initialize(m_locator));

        // Authenticated default tenant for all tests
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
    /// GET a list endpoint and parse the JSON envelope
    ///
    json ListAsJson(const std::string& path)
    {
        return json::parse(Route("GET", path));
    }

    ///
    /// Parse the committed dev setup JSON (T-03-09: the stream open is
    /// asserted before anything about its contents)
    ///
    static void LoadDevJson(json& doc)
    {
        std::ifstream stream(SETUP_DEV_JSON);
        ASSERT_TRUE(stream.is_open())
            << "Cannot open committed dev setup JSON: " << SETUP_DEV_JSON;
        doc = json::parse(stream);
    }

    ///
    /// The single declared location row from dev.json (D-03)
    ///
    static void LoadDeclaredLocation(json& declared)
    {
        json doc;
        LoadDevJson(doc);
        ASSERT_TRUE(doc.at("locations").is_array());
        declared = doc.at("locations").front();
    }

    ///
    /// True when the list envelope's data array carries a row with the code
    ///
    static bool ListContainsCode(const json& page, const std::string& code)
    {
        for (const auto& row : page.at("data"))
        {
            if (row.contains("code") && row.at("code").get<std::string>() == code)
            {
                return true;
            }
        }
        return false;
    }

    ///
    /// Count the list envelope's data rows carrying the code
    ///
    static size_t CountRowsWithCode(const json& page, const std::string& code)
    {
        size_t count = 0;
        for (const auto& row : page.at("data"))
        {
            if (row.contains("code") && row.at("code").get<std::string>() == code)
            {
                ++count;
            }
        }
        return count;
    }

    ///
    /// The first data row carrying the code (null json when absent)
    ///
    static json FindRowWithCode(const json& page, const std::string& code)
    {
        for (const auto& row : page.at("data"))
        {
            if (row.contains("code") && row.at("code").get<std::string>() == code)
            {
                return row;
            }
        }
        return json();
    }

    ///
    /// Replicate the applier's client-side algorithm (scripts/apply_setup.sh):
    /// GET the list once, match each declared row's code against .data[], POST
    /// the declared row — re-serialized from dev.json, never re-typed — ONLY
    /// when absent. Returns the number of rows POSTed and appends the ids the
    /// create responses minted.
    ///
    size_t ApplyDeclaredLocations(std::vector<std::string>& createdIds)
    {
        json doc;
        LoadDevJson(doc);
        const json page = ListAsJson(kLocationsPath);
        size_t createdCount = 0;
        for (const auto& declared : doc.at("locations"))
        {
            const std::string code = declared.at("code").get<std::string>();
            if (!ListContainsCode(page, code))
            {
                const json created = json::parse(PostJson(kLocationsPath, declared.dump()));
                createdIds.push_back(created.at("id").get<std::string>());
                ++createdCount;
            }
        }
        return createdCount;
    }

    ///
    /// Body carrying every from_json-required key: the declared dev.json row
    /// plus the storage stamps the create handler would have added
    ///
    void MakeModelBody(json& body)
    {
        json declared;
        LoadDeclaredLocation(declared);
        body = declared;
        body["id"]              = kModelFixtureId;
        body["tenant_id"]       = kDefaultTenant;
        body["organization_id"] = kDefaultTenant;
        body["created_at"]      = kTestTimestamp;
        body["updated_at"]      = kTestTimestamp;
    }
};

// ============================================================================
// Group 1 — TAX-03: dev.json expected-values contract
// ============================================================================

///
/// The committed dev setup JSON declares exactly one location, active, with
/// tax_rate 9.25 inside the 0-100 contract bounds and a California address.
///
TEST_F(LocationsSetupTest, DevJsonDeclaresOneActiveCaliforniaLocation)
{
    json doc;
    LoadDevJson(doc);

    ASSERT_TRUE(doc.at("locations").is_array());
    ASSERT_EQ(doc.at("locations").size(), kExpectedLocationCount);

    const json& declared = doc.at("locations").front();
    EXPECT_EQ(declared.at("status").get<std::string>(), kActiveStatus);
    EXPECT_FALSE(declared.at("code").get<std::string>().empty());

    const double taxRate = declared.at("tax_rate").get<double>();
    EXPECT_DOUBLE_EQ(taxRate, kExpectedTaxRate);
    EXPECT_GE(taxRate, kTaxRateMin);
    EXPECT_LE(taxRate, kTaxRateMax);

    const json& address = declared.at("address");
    EXPECT_EQ(address.at("region").get<std::string>(), kCaliforniaRegion);
    const std::string country = address.at("country").get<std::string>();
    EXPECT_EQ(country, kUnitedStatesCountry);
    EXPECT_EQ(country.size(), kCountryCodeLength);
}

// ============================================================================
// Group 2 — TAX-02: handler-level apply idempotency through the REAL
// generated LocationsPlugin (Route() dispatch, fresh RocksDB temp dir)
// ============================================================================

///
/// The first apply POSTs the declared row (no error envelope, minted 32-hex
/// id, default tenant) and the stored row round-trips status/tax_rate; a
/// second apply of the same match-on-code logic leaves exactly one stored
/// row for the declared code — no duplicate.
///
TEST_F(LocationsSetupTest, DeclaredLocationAppliesIdempotently)
{
    json declared;
    LoadDeclaredLocation(declared);
    const std::string code = declared.at("code").get<std::string>();

    std::vector<std::string> firstIds;
    const size_t createdFirst = ApplyDeclaredLocations(firstIds);
    ASSERT_EQ(createdFirst, kExpectedLocationCount);
    ASSERT_EQ(firstIds.size(), kExpectedLocationCount);
    EXPECT_EQ(firstIds.front().size(), kUuidHexLength);

    const json firstPage = ListAsJson(kLocationsPath);
    ASSERT_EQ(CountRowsWithCode(firstPage, code), kExpectedLocationCount);
    const json stored = FindRowWithCode(firstPage, code);
    ASSERT_FALSE(stored.is_null());
    EXPECT_EQ(stored.at("id").get<std::string>(), firstIds.front());
    EXPECT_EQ(stored.at("tenant_id").get<std::string>(), kDefaultTenant);
    EXPECT_EQ(stored.at("status").get<std::string>(), kActiveStatus);
    EXPECT_DOUBLE_EQ(stored.at("tax_rate").get<double>(), kExpectedTaxRate);

    std::vector<std::string> secondIds;
    const size_t createdSecond = ApplyDeclaredLocations(secondIds);
    EXPECT_EQ(createdSecond, 0);
    EXPECT_TRUE(secondIds.empty());

    const json secondPage = ListAsJson(kLocationsPath);
    EXPECT_EQ(CountRowsWithCode(secondPage, code), kExpectedLocationCount);
}

///
/// createLocation never upserts — a raw duplicate POST (bypassing the
/// match-on-code) mints a DIFFERENT id and stores a second row for the code.
/// This is why the applier's client-side matching is mandatory. Runs on
/// fresh fixture storage so it cannot pollute the idempotency test above.
///
TEST_F(LocationsSetupTest, RawDuplicatePostMintsFreshId)
{
    json declared;
    LoadDeclaredLocation(declared);
    const std::string code = declared.at("code").get<std::string>();

    const json first = json::parse(PostJson(kLocationsPath, declared.dump()));
    const json second = json::parse(PostJson(kLocationsPath, declared.dump()));

    const std::string firstId = first.at("id").get<std::string>();
    const std::string secondId = second.at("id").get<std::string>();
    EXPECT_EQ(firstId.size(), kUuidHexLength);
    EXPECT_EQ(secondId.size(), kUuidHexLength);
    EXPECT_NE(firstId, secondId);

    const json page = ListAsJson(kLocationsPath);
    EXPECT_EQ(CountRowsWithCode(page, code), kDuplicateRowCount);
}

// ============================================================================
// Group 3 — TAX-01: regenerated Location model round-trip (beyond compile)
// ============================================================================

///
/// from_json round-trips the declared 9.25 tax_rate as double with IsSet
/// true, and to_json re-emits the "tax_rate" key.
///
TEST_F(LocationsSetupTest, LocationModelRoundTripsTaxRate)
{
    json body;
    MakeModelBody(body);
    ASSERT_TRUE(body.contains("tax_rate"));

    const model::Location location = body.get<model::Location>();
    EXPECT_TRUE(location.taxRateIsSet());
    EXPECT_DOUBLE_EQ(location.getTaxRate(), kExpectedTaxRate);

    json serialized;
    to_json(serialized, location);
    ASSERT_TRUE(serialized.contains("tax_rate"));
    EXPECT_DOUBLE_EQ(serialized.at("tax_rate").get<double>(), kExpectedTaxRate);
}

///
/// An absent tax_rate key leaves the model unset and to_json omits the key.
///
TEST_F(LocationsSetupTest, LocationModelLeavesTaxRateUnsetWhenKeyAbsent)
{
    json body;
    MakeModelBody(body);
    ASSERT_EQ(body.erase("tax_rate"), kErasedKeyCount);

    const model::Location location = body.get<model::Location>();
    EXPECT_FALSE(location.taxRateIsSet());

    json serialized;
    to_json(serialized, location);
    EXPECT_FALSE(serialized.contains("tax_rate"));
}
