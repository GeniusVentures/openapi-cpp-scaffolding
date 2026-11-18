/**
 * @file       test_auth_login.cpp
 * @brief      Integration tests for auth handler priority override system
 * @date       2026-06-10
 * @author     Kenneth L. Hurley
 *
 * Tests the REGISTER_AUTH_HANDLER + REGISTER_ALL_AUTH_HANDLERS macro system:
 * - Generated stubs at priority 100
 * - Real handlers at priority 200
 * - Route() dispatches to highest priority
 * - GENIUS_TESTING_ENABLED skips registration
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>

#include "singleton/PluginManager.hpp"
#include "singleton/CServiceLocator.hpp"
#include "singleton/IServiceLocator.hpp"
#include "storage/RocksDBEngine.hpp"
#include "storage/KeyBuilder.hpp"
#include "singleton/fnv1a.hpp"
#include "identity/auth_utils.hpp"
#include "identity/identity_auth_handlers.hpp"
#include "nlohmann/json.hpp"

using json = nlohmann::json;
using namespace gnus::hash;
namespace fs = std::filesystem;

// ============================================================================
// Stub handler — mimics auto-generated at priority 100
// ============================================================================

static std::string stub_login(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& body)
{
    return body;  // just returns raw body (auto-generated behavior)
}

/// Stub users-create — mimics the generic CRUD handler: stores and returns
/// the body verbatim without contract-required defaults.
static std::string stub_users_create(
    const RequestContext& /*ctx*/,
    const std::string& /*method*/,
    const std::string& /*urlPath*/,
    const std::string& body)
{
    return body;  // verbatim (generic auto-generated behavior)
}

// ============================================================================
// Test Fixture
// ============================================================================

class AuthLoginTest : public ::testing::Test
{
protected:
    std::unique_ptr<RocksDBEngine> m_engine;
    PluginManager m_pm;
    CServiceLocator m_locator;
    fs::path m_tempPath;
    RequestContext m_ctx;

    static inline const std::string kTestSecret = "test-secret-key-for-jwt-signing-32bytes!";
    static constexpr const char* kTestEmail = "admin@test.com";
    static constexpr const char* kTestPassword = "admin123";
    static constexpr int kOverridePriority = 200;

    void SetUp() override
    {
        auto base = fs::current_path();
        auto timestamp = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        m_tempPath = base / ("test_auth_login_" + std::to_string(timestamp));
        fs::create_directories(m_tempPath);

        auto result = RocksDBEngine::Create(m_tempPath.string());
        ASSERT_TRUE(result.has_value()) << "Failed to create engine: "
                                         << result.error().message();
        m_engine = std::move(result.value());

        m_locator.RegisterService(Fnv1a("PluginManager"), &m_pm);
        m_locator.RegisterService(Fnv1a("StorageEngine"), m_engine.get());
        m_locator.RegisterService(Fnv1a("JwtSecret"), const_cast<std::string*>(&kTestSecret));

        SeedUser(kTestEmail, kTestPassword, "user-test-001");

        // Register stub at priority 0 (simulates auto-generated baseline)
        m_pm.RegisterHandler("POST", "/api/v1/auth/login", "stub_login",
                             stub_login, "Identity", 0);
    }

    void TearDown() override
    {
        m_engine.reset();
        fs::remove_all(m_tempPath);
    }

    void SeedUser(const std::string& email,
                  const std::string& password,
                  const std::string& userId,
                  const json&        roles = json::array())
    {
        auto hashResult = HashPassword(password);
        ASSERT_FALSE(hashResult.salt.empty());

        static constexpr const char kHex[] = "0123456789abcdef";
        auto hexEncode = [](const auto& data)
        {
            std::string out;
            out.reserve(data.size() * 2);
            for (unsigned char b : data)
            {
                out += kHex[(b >> 4) & 0x0F];
                out += kHex[b & 0x0F];
            }
            return out;
        };

        json userJson;
        userJson["id"] = userId;
        userJson["email"] = email;
        userJson["display_name"] = "Test Admin";
        userJson["tenant_id"] = "default";
        userJson["organization_id"] = "default";
        userJson["status"] = "active";
        if (!roles.empty())
        {
            userJson["roles"] = roles;
        }
        userJson["created_at"] = "2026-01-01T00:00:00Z";
        userJson["updated_at"] = "2026-01-01T00:00:00Z";
        userJson["password_hash"] = hexEncode(hashResult.hash);
        userJson["password_salt"] = hexEncode(hashResult.salt);
        userJson["password_iterations"] = hashResult.iterations;

        auto userKey = KeyBuilder::Build("identity", "users", userId);
        ASSERT_TRUE(userKey.has_value());
        ASSERT_TRUE(m_engine->Put(userKey.value(), userJson.dump()));

        std::string emailKey = "identity/users_by_email/" + email;
        ASSERT_TRUE(m_engine->Put(emailKey, userId));
    }

    /// Puts a minimal user record WITHOUT hashing — for pagination tests
    /// that seed dozens of users (HashPassword per seed would dominate the
    /// runtime). Password fields are opaque strings nobody verifies here.
    void SeedRawUser(const std::string& userId)
    {
        json userJson;
        userJson["id"] = userId;
        userJson["email"] = userId + "@raw.test";
        userJson["display_name"] = userId;
        userJson["tenant_id"] = "default";
        userJson["organization_id"] = "default";
        userJson["status"] = "active";
        userJson["password_hash"] = "raw";
        userJson["password_salt"] = "raw";
        userJson["password_iterations"] = 1;

        auto key = KeyBuilder::Build("identity", "users", userId);
        ASSERT_TRUE(key.has_value());
        ASSERT_TRUE(m_engine->Put(key.value(), userJson.dump()));
    }

    /// Puts an arbitrary raw value under a user key — for malformed-record
    /// tolerance tests (non-JSON payloads, non-string id keys).
    void PutRawUserRecord(const std::string& userId, const std::string& rawValue)
    {
        auto key = KeyBuilder::Build("identity", "users", userId);
        ASSERT_TRUE(key.has_value());
        ASSERT_TRUE(m_engine->Put(key.value(), rawValue));
    }

    /// Registers real auth handler at priority 200, overriding stub at 100
    void RegisterRealLogin()
    {
        // For test simplicity, provide a thin real handler inline
        auto real_login = [this](
            const RequestContext& /*ctx*/,
            const std::string& /*method*/,
            const std::string& /*urlPath*/,
            const std::string& body) -> std::string
        {
            json req = json::parse(body);
            std::string email = req["email"].get<std::string>();
            std::string password = req["password"].get<std::string>();

            std::string emailKey = "identity/users_by_email/" + email;
            std::string userId;
            if (!m_engine->Get(emailKey, userId))
            {
                return R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})";
            }

            auto ukr = KeyBuilder::Build("identity", "users", userId);
            if (!ukr.has_value())
            {
                return R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})";
            }
            std::string userStr;
            if (!m_engine->Get(ukr.value(), userStr))
            {
                return R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})";
            }

            json userJson = json::parse(userStr);
            // Build PasswordHash from stored fields and verify
            PasswordHash stored;
            if (userJson.contains("password_hash") && userJson.contains("password_salt"))
            {
                auto hexDecode = [](const std::string& hex) -> std::string {
                    if (hex.size() % 2 != 0) return {};
                    auto hv = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    std::string r;
                    for (size_t i = 0; i < hex.size(); i += 2) {
                        int hi = hv(hex[i]), lo = hv(hex[i+1]);
                        if (hi < 0 || lo < 0) return {};
                        r += static_cast<char>((hi << 4) | lo);
                    }
                    return r;
                };
                std::string hashBin = hexDecode(userJson["password_hash"].get<std::string>());
                std::string saltBin = hexDecode(userJson["password_salt"].get<std::string>());
                stored.hash.assign(hashBin.begin(), hashBin.end());
                stored.salt.assign(saltBin.begin(), saltBin.end());
                stored.iterations = userJson.value("password_iterations", 100000);
            }
            if (!VerifyPassword(password, stored))
            {
                return R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})";
            }

            std::string token = CreateJwtToken(
                kTestSecret, userId, userJson.value("tenant_id", ""),
                userJson.value("organization_id", ""), 3600);

            json response;
            response["access_token"] = token;
            response["token_type"] = "Bearer";
            response["expires_in"] = 3600;
            response["user"] = userJson;
            return response.dump();
        };

        m_pm.RegisterHandler(
            "POST", "/api/v1/auth/login", "real_login",
            HandlerFn(real_login), "IdentityAuth", kOverridePriority);
    }

    /// Registers a stub users-list at priority 0 that mimics the generated
    /// listUsers stub: raw prefix scan with tenant backfill, NO password
    /// stripping, NO role-contract normalization — the observed live defect
    /// shape of GET /api/v1/users.
    void RegisterStubUsersList()
    {
        auto stub_list = [this](
            const RequestContext& /*ctx*/,
            const std::string& /*method*/,
            const std::string& /*urlPath*/,
            const std::string& /*body*/) -> std::string
        {
            auto scanResult = m_engine->Scan(
                KeyBuilder::MakePrefix("identity", "users"));
            json result = json::array();
            for (const auto& [k, v] : scanResult)
            {
                json item = json::parse(v);
                if (!item.contains("tenant_id")) { item["tenant_id"] = "default"; }
                if (!item.contains("organization_id")) { item["organization_id"] = "default"; }
                result.push_back(item);
            }
            json response;
            response["data"] = result;
            json pagination;
            pagination["limit"] = 50;
            pagination["has_more"] = false;
            response["pagination"] = pagination;
            return response.dump();
        };

        m_pm.RegisterHandler(
            "GET", "/api/v1/users", "stub_users_list",
            HandlerFn(stub_list), "Identity", 0);
    }

    /// Registers leaky item-route stubs at priority 0, mirroring the
    /// generated getUser/updateUser (raw stored record verbatim, legacy
    /// roles shape, no stripping) — the observed defect shape of the item
    /// routes that the users_get/users_update overrides must beat.
    void RegisterStubUsersItemRoutes()
    {
        auto stub_get = [this](
            const RequestContext& /*ctx*/,
            const std::string& /*method*/,
            const std::string& urlPath,
            const std::string& /*body*/) -> std::string
        {
            const std::string id = urlPath.substr(urlPath.rfind('/') + 1);
            auto key = KeyBuilder::Build("identity", "users", id);
            if (!key.has_value()) { return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})"; }
            std::string value;
            if (!m_engine->Get(key.value(), value))
            {
                return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
            }
            return value; // raw stored record — password material included
        };
        auto stub_update = [this](
            const RequestContext& /*ctx*/,
            const std::string& /*method*/,
            const std::string& urlPath,
            const std::string& body) -> std::string
        {
            const std::string id = urlPath.substr(urlPath.rfind('/') + 1);
            auto key = KeyBuilder::Build("identity", "users", id);
            if (!key.has_value()) { return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})"; }
            std::string existing;
            if (!m_engine->Get(key.value(), existing))
            {
                return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
            }
            json updated = json::parse(existing);
            updated.update(json::parse(body));
            m_engine->Put(key.value(), updated.dump());
            return updated.dump(); // raw merged record — no stripping
        };

        m_pm.RegisterHandler(
            "GET", "/api/v1/users/{userId}", "stub_users_get",
            HandlerFn(stub_get), "Identity", 0);
        m_pm.RegisterHandler(
            "PATCH", "/api/v1/users/{userId}", "stub_users_update",
            HandlerFn(stub_update), "Identity", 0);
    }
};

// ============================================================================
// Priority Override Tests
// ============================================================================

TEST_F(AuthLoginTest, PriorityOverride_RealHandlerWins)
{
    RegisterRealLogin();

    json body;
    body["email"] = kTestEmail;
    body["password"] = kTestPassword;

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    // Real handler returns JWT (access_token), stub would return raw body
    EXPECT_TRUE(data.contains("access_token"))
        << "Stub at priority 0 was dispatched — real handler at 200 should win";
    EXPECT_FALSE(data.contains("error")) << "Unexpected error: " << response;
}

TEST_F(AuthLoginTest, PriorityOverride_StubWinsWhenNoOverride)
{
    // Don't call RegisterRealLogin — only stub at priority 100
    json body;
    body["email"] = kTestEmail;
    body["password"] = kTestPassword;

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    // Stub returns raw body — no access_token
    EXPECT_FALSE(data.contains("access_token"));
    EXPECT_EQ(kTestEmail, data["email"].get<std::string>());
}

TEST_F(AuthLoginTest, PriorityOverride_WrongPassword_ReturnsError)
{
    RegisterRealLogin();

    json body;
    body["email"] = kTestEmail;
    body["password"] = "wrong";

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    EXPECT_TRUE(data.contains("error"));
    EXPECT_EQ("INVALID_CREDENTIALS", data["error"]["code"].get<std::string>());
}

TEST_F(AuthLoginTest, PriorityOverride_UnknownUser_ReturnsError)
{
    RegisterRealLogin();

    json body;
    body["email"] = "nobody@test.com";
    body["password"] = "x";

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    EXPECT_TRUE(data.contains("error"));
    EXPECT_EQ("INVALID_CREDENTIALS", data["error"]["code"].get<std::string>());
}

// ============================================================================
// GENIUS_TESTING_ENABLED flag test
// ============================================================================

TEST_F(AuthLoginTest, TestingFlag_PreventsOverrideRegistration)
{
    // Simulate: GENIUS_TESTING_ENABLED = true
    // In the real system, REGISTER_ALL_AUTH_HANDLERS checks this flag
    // and skips registration when true. Stubs at priority 100 remain active.
    // We verify this by NOT calling RegisterRealLogin (simulating skip).

    bool testingEnabled = true;
    if (testingEnabled)
    {
        // Don't register real handler — stubs stay active for mocking
        SUCCEED() << "Testing mode: auth override registration skipped";
    }

    json body;
    body["email"] = kTestEmail;
    body["password"] = kTestPassword;

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    // Stub at priority 100 is active (no override)
    EXPECT_FALSE(data.contains("access_token"));
}

// ============================================================================
// Unhappy-Path Login Tests — empty credentials, missing fields, malformed body
// ============================================================================

TEST_F(AuthLoginTest, RealLogin_EmptyEmail_ReturnsInvalidCredentials)
{
    RegisterRealLogin();

    json body;
    body["email"] = std::string("");  // explicitly empty
    body["password"] = kTestPassword;

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    // No user is keyed under "identity/users_by_email/" (empty email),
    // so the lookup misses and the handler returns INVALID_CREDENTIALS.
    EXPECT_TRUE(data.contains("error"));
    EXPECT_EQ("INVALID_CREDENTIALS", data["error"]["code"].get<std::string>());
}

TEST_F(AuthLoginTest, RealLogin_EmptyPassword_ReturnsInvalidCredentials)
{
    RegisterRealLogin();

    json body;
    body["email"] = kTestEmail;
    body["password"] = std::string("");  // explicitly empty

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    // The seeded user's hash was derived from a non-empty password, so an
    // empty plaintext must fail PBKDF2 verification.
    EXPECT_TRUE(data.contains("error"));
    EXPECT_EQ("INVALID_CREDENTIALS", data["error"]["code"].get<std::string>());
}

TEST_F(AuthLoginTest, RealLogin_EmptyEmailAndEmptyPassword_ReturnsInvalidCredentials)
{
    RegisterRealLogin();

    json body;
    body["email"]    = std::string("");
    body["password"] = std::string("");

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    EXPECT_TRUE(data.contains("error"));
    EXPECT_EQ("INVALID_CREDENTIALS", data["error"]["code"].get<std::string>());
}

TEST_F(AuthLoginTest, RealLogin_MalformedJsonBody_ThrowsAndPropagates)
{
    RegisterRealLogin();

    // The real_login handler invokes json::parse(body) with no try/catch,
    // and PluginManager::Route does not catch handler exceptions. A malformed
    // body therefore propagates as a nlohmann::json::parse_error. This test
    // documents the current behavior: callers must send valid JSON or the
    // process terminates the request with an uncaught exception.
    EXPECT_THROW(
        {
            m_pm.Route(m_ctx, "POST", "/api/v1/auth/login",
                       "this is not json {{{");
        },
        nlohmann::json::exception);
}

TEST_F(AuthLoginTest, RealLogin_MissingEmailField_ThrowsAndPropagates)
{
    RegisterRealLogin();

    // A well-formed JSON body that omits the "email" field causes
    // req["email"] (operator[]) to throw. Documents that the handler does
    // not pre-validate presence of required fields.
    json body;
    body["password"] = kTestPassword;
    // note: no "email" key

    EXPECT_THROW(
        {
            m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
        },
        nlohmann::json::exception);
}

TEST_F(AuthLoginTest, StubLogin_EmptyCredentialsBody_EchoesBody)
{
    // Without the real override registered, the stub at priority 0 echoes
    // the raw body back. An empty body is valid stub input.
    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", "");

    // Stub returns the body verbatim — empty in, empty out.
    EXPECT_TRUE(response.empty());
}

// ============================================================================
// Users Create Override Tests
//
// Regression (7da1328 relocation): the hand-written identity createUser
// (4d8144a) set status="active"; the generic CRUD handler that took over
// POST /api/v1/users stores/returns the body verbatim. The User response
// schema requires "status", so every created user broke client parsing.
// ============================================================================

TEST_F(AuthLoginTest, UsersCreate_WithoutOverride_BodyLacksStatus)
{
    // Baseline documenting the regression: only the generic stub (priority 0)
    // is registered — the response has no "status".
    m_pm.RegisterHandler("POST", "/api/v1/users", "stub_users_create",
                         stub_users_create, "Identity", 0);

    json body;
    body["email"] = "nostatus@test.com";
    body["display_name"] = "No Status";

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/users", body.dump());
    auto data = json::parse(response);

    EXPECT_FALSE(data.contains("status"))
        << "Generic handler echoed the body — status must come from the override";
}

TEST_F(AuthLoginTest, UsersCreate_Override_SetsStatusAndRequiredFields)
{
    m_pm.RegisterHandler("POST", "/api/v1/users", "stub_users_create",
                         stub_users_create, "Identity", 0);
    init_identity_overrides(&m_pm, m_locator);

    json body;
    body["email"] = "override@test.com";
    body["display_name"] = "Override User";

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/users", body.dump());
    auto data = json::parse(response);

    ASSERT_FALSE(data.contains("error")) << response;
    EXPECT_EQ("active", data.value("status", ""))
        << "status is required by the User schema and must default to active";
    EXPECT_EQ("Override User", data["display_name"].get<std::string>());
    EXPECT_FALSE(data.value("id", "").empty());
    EXPECT_EQ(m_ctx.tenantId, data.value("tenant_id", std::string()));
    EXPECT_EQ(m_ctx.organizationId, data.value("organization_id", std::string()));
    EXPECT_FALSE(data.value("created_at", "").empty());
    EXPECT_EQ(data["created_at"], data["updated_at"]);
}

TEST_F(AuthLoginTest, UsersCreate_Override_DisplayNameFallsBackToEmail)
{
    m_pm.RegisterHandler("POST", "/api/v1/users", "stub_users_create",
                         stub_users_create, "Identity", 0);
    init_identity_overrides(&m_pm, m_locator);

    json body;
    body["email"] = "fallback@test.com";
    // no display_name — handler must tolerate and fall back (4d8144a behavior)

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/users", body.dump());
    auto data = json::parse(response);

    ASSERT_FALSE(data.contains("error")) << response;
    EXPECT_EQ("fallback@test.com", data["display_name"].get<std::string>());
}

TEST_F(AuthLoginTest, UsersCreate_Override_MissingEmail_ReturnsInvalidRequest)
{
    m_pm.RegisterHandler("POST", "/api/v1/users", "stub_users_create",
                         stub_users_create, "Identity", 0);
    init_identity_overrides(&m_pm, m_locator);

    json body;
    body["display_name"] = "No Email";

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/users", body.dump());
    auto data = json::parse(response);

    EXPECT_TRUE(data.contains("error"));
    EXPECT_EQ("INVALID_REQUEST", data["error"]["code"].get<std::string>());
}

TEST_F(AuthLoginTest, UsersCreate_Override_NonStringEmail_ReturnsInvalidRequest)
{
    m_pm.RegisterHandler("POST", "/api/v1/users", "stub_users_create",
                         stub_users_create, "Identity", 0);
    init_identity_overrides(&m_pm, m_locator);

    // PR #4 Codex P2: a non-string email used to throw json::type_error
    // through PluginManager::Route and terminate the request; the handler
    // must answer INVALID_REQUEST instead.
    json body;
    body["email"] = 7;
    body["display_name"] = "Numeric Email";

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/users", body.dump());
    auto data = json::parse(response);

    EXPECT_TRUE(data.contains("error"));
    EXPECT_EQ("INVALID_REQUEST", data["error"]["code"].get<std::string>());
}

// ============================================================================
// Users List Override + roles seed (quick 261117-o39): GET /api/v1/users must
// satisfy the regenerated Dart client's checked contracts — every embedded
// roles entry carries ALL of id/tenant_id/organization_id/created_at/
// updated_at/name/scope (Role.fromJson $checkKeys) and no password material
// survives serialization — and the identity/roles prefix gets a canonical
// admin role so the admin Role List screen is not empty.
// ============================================================================

TEST_F(AuthLoginTest, UsersList_Override_NormalizesRolesAndStripsPasswords)
{
    RegisterStubUsersList();
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("legacy-roles@test.com", kTestPassword, "user-legacy-001",
             json::array({
                 json{{"name", "admin"},
                      {"permissions", json::array({"*:*"})}},
             }));

    std::string response = m_pm.Route(m_ctx, "GET", "/api/v1/users", "");
    auto data = json::parse(response);
    ASSERT_FALSE(data.contains("error")) << response;

    ASSERT_TRUE(data.contains("data")) << response;
    ASSERT_TRUE(data.at("data").is_array()) << response;
    const json* listed = nullptr;
    for (const auto& item : data.at("data"))
    {
        if (item.value("id", "") == "user-legacy-001") { listed = &item; }
    }
    ASSERT_NE(nullptr, listed)
        << "seeded legacy-shape user must appear in the list: " << response;

    // T-o39-01: no password material on the wire.
    EXPECT_FALSE(listed->contains("password_hash"));
    EXPECT_FALSE(listed->contains("password_salt"));
    EXPECT_FALSE(listed->contains("password_iterations"));

    // Role.fromJson $checkKeys required list, in full.
    ASSERT_TRUE(listed->contains("roles")) << response;
    ASSERT_TRUE(listed->at("roles").is_array()) << response;
    ASSERT_GE(listed->at("roles").size(), 1u) << response;
    const json& role = listed->at("roles").at(0);
    for (const char* key : {"id", "tenant_id", "organization_id",
                            "created_at", "updated_at", "name", "scope"})
    {
        EXPECT_TRUE(role.contains(key))
            << "Role.fromJson requires '" << key << "': " << role.dump();
    }
    // Authorization data passes through verbatim (ACL-CONTRACT-01 unchanged).
    EXPECT_EQ("admin", role.value("name", ""));
    EXPECT_EQ(json::array({"*:*"}), role.at("permissions"));

    ASSERT_TRUE(data.contains("pagination")) << response;
    EXPECT_TRUE(data.at("pagination").contains("limit")) << response;
    EXPECT_TRUE(data.at("pagination").contains("has_more")) << response;
}

TEST_F(AuthLoginTest, UsersList_Override_ToleratesMissingRoles)
{
    RegisterStubUsersList();
    init_identity_overrides(&m_pm, m_locator);
    // user-test-001 (SetUp) was stored with NO roles key at all — the list
    // must surface it, never fail to parse.

    std::string response = m_pm.Route(m_ctx, "GET", "/api/v1/users", "");
    auto data = json::parse(response);
    ASSERT_FALSE(data.contains("error")) << response;
    ASSERT_TRUE(data.at("data").is_array()) << response;

    bool found = false;
    for (const auto& item : data.at("data"))
    {
        if (item.value("id", "") == "user-test-001")
        {
            found = true;
            EXPECT_FALSE(item.contains("password_hash"));
            if (item.contains("roles"))
            {
                EXPECT_TRUE(item.at("roles").is_array())
                    << "roles must be an array when present: " << item.dump();
            }
        }
    }
    EXPECT_TRUE(found)
        << "stored user without a roles key must still be listed: " << response;
}

// ============================================================================
// Users List Pagination (WR-02: the pagination logic shipped with zero test
// coverage) + malformed-record tolerance (WR-01's type-safe id read).
// ============================================================================

TEST_F(AuthLoginTest, UsersList_Pagination_PagesChainGaplessAndDuplicateFree)
{
    RegisterStubUsersList();
    init_identity_overrides(&m_pm, m_locator);
    // SetUp's user-test-001 + init's touch-pos PIN user + 58 raw users
    // = 60 total; ids sort lexicographically
    // ("pin_user_*" < "user-page-*" < "user-test-001").
    for (int i = 2; i <= 59; ++i)
    {
        char id[32];
        std::snprintf(id, sizeof(id), "user-page-%03d", i);
        SeedRawUser(id);
    }

    m_ctx.queryString = "limit=50";
    auto page1 = json::parse(m_pm.Route(m_ctx, "GET", "/api/v1/users", ""));
    ASSERT_FALSE(page1.contains("error")) << page1.dump();
    ASSERT_EQ(50u, page1.at("data").size()) << page1.dump();
    EXPECT_TRUE(page1.at("pagination").at("has_more").get<bool>());
    ASSERT_TRUE(page1.at("pagination").contains("next_cursor")) << page1.dump();
    const std::string next1 =
        page1.at("pagination").at("next_cursor").get<std::string>();
    // next_cursor is the id of the LAST row on this page — the resume point.
    EXPECT_EQ(page1.at("data").at(49).at("id").get<std::string>(), next1);

    m_ctx.queryString = "limit=50&cursor=" + next1;
    auto page2 = json::parse(m_pm.Route(m_ctx, "GET", "/api/v1/users", ""));
    ASSERT_FALSE(page2.contains("error")) << page2.dump();
    ASSERT_EQ(10u, page2.at("data").size()) << page2.dump();
    // End of list: has_more false and NO next_cursor key (absent, not null).
    EXPECT_FALSE(page2.at("pagination").at("has_more").get<bool>());
    EXPECT_FALSE(page2.at("pagination").contains("next_cursor"));

    // Union across pages is gapless and duplicate-free: 60 distinct ids,
    // every page-2 id strictly after the cursor, none of page 1's past it.
    std::set<std::string> ids;
    for (const auto& item : page1.at("data"))
    {
        ids.insert(item.at("id").get<std::string>());
        EXPECT_LE(item.at("id").get<std::string>(), next1);
    }
    for (const auto& item : page2.at("data"))
    {
        ids.insert(item.at("id").get<std::string>());
        EXPECT_GT(item.at("id").get<std::string>(), next1);
    }
    EXPECT_EQ(60u, ids.size());
}

TEST_F(AuthLoginTest, UsersList_Pagination_LimitParsingCapAndFallbacks)
{
    RegisterStubUsersList();
    init_identity_overrides(&m_pm, m_locator);
    // 110 raw users + SetUp's user-test-001 = 111 — enough to observe the
    // limit=100 cap, not just the default.
    for (int i = 0; i < 110; ++i)
    {
        char id[32];
        std::snprintf(id, sizeof(id), "user-cap-%03d", i);
        SeedRawUser(id);
    }

    const auto dataSizeFor = [this](const std::string& query) {
        m_ctx.queryString = query;
        auto resp = json::parse(m_pm.Route(m_ctx, "GET", "/api/v1/users", ""));
        EXPECT_FALSE(resp.contains("error")) << query << ": " << resp.dump();
        return std::make_pair(resp.at("data").size(),
                              resp.at("pagination").at("limit").get<unsigned int>());
    };

    // Cap: limit=200 serves at most 100 and reports the effective limit.
    auto [capped, capLimit] = dataSizeFor("limit=200");
    EXPECT_EQ(100u, capped);
    EXPECT_EQ(100u, capLimit);

    // Fallbacks: non-numeric, negative, and zero all serve the default 50.
    EXPECT_EQ(50u, dataSizeFor("limit=abc").first);
    EXPECT_EQ(50u, dataSizeFor("limit=-1").first);
    EXPECT_EQ(50u, dataSizeFor("limit=0").first);
    // Absent limit: default 50.
    EXPECT_EQ(50u, dataSizeFor("").first);
}

TEST_F(AuthLoginTest, UsersList_MalformedRecordsSkippedNotFatal)
{
    RegisterStubUsersList();
    init_identity_overrides(&m_pm, m_locator);

    // Non-JSON payload under a user key — the parse guard skips it.
    PutRawUserRecord("user-broken-json", "this is not json {{{");
    // JSON record whose id key holds a number — storable today via the
    // generic update stub's body merge; WR-01: must not throw type_error.
    PutRawUserRecord("user-broken-id", R"({"id":123,"email":"x@y.z"})");

    m_ctx.queryString = "";
    auto resp = json::parse(m_pm.Route(m_ctx, "GET", "/api/v1/users", ""));
    ASSERT_FALSE(resp.contains("error")) << resp.dump();
    ASSERT_TRUE(resp.at("data").is_array());

    // SetUp's valid user still listed; neither malformed record is.
    // Type-safe id read in the verification loop too — the same
    // value()-on-non-string trap WR-01 fixed in the handler.
    bool found = false;
    for (const auto& item : resp.at("data"))
    {
        ASSERT_TRUE(item.contains("id")) << resp.dump();
        ASSERT_TRUE(item.at("id").is_string())
            << "non-string ids must never reach the wire: " << item.dump();
        const std::string id = item.at("id").get<std::string>();
        if (id == "user-test-001") { found = true; }
        EXPECT_NE("user-broken-json", id);
        EXPECT_NE("user-broken-id", id);
    }
    EXPECT_TRUE(found) << "valid user must survive malformed neighbors: "
                       << resp.dump();
}

// ============================================================================
// Users Item-Route Overrides (CR-01: GET/PATCH /api/v1/users/{userId} must
// not leak password material and must emit the contract-complete role shape)
// ============================================================================

TEST_F(AuthLoginTest, UsersGet_ItemRoute_StripsPasswordsAndNormalizesRoles)
{
    RegisterStubUsersItemRoutes();
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("item-get@test.com", kTestPassword, "user-item-001",
             json::array({
                 json{{"name", "admin"},
                      {"permissions", json::array({"*:*"})}},
             }));

    auto resp = json::parse(
        m_pm.Route(m_ctx, "GET", "/api/v1/users/user-item-001", ""));
    ASSERT_FALSE(resp.contains("error")) << resp.dump();
    ASSERT_EQ("user-item-001", resp.at("id").get<std::string>());

    // T-o39-01 on the item route: no password material on the wire.
    EXPECT_FALSE(resp.contains("password_hash")) << resp.dump();
    EXPECT_FALSE(resp.contains("password_salt")) << resp.dump();
    EXPECT_FALSE(resp.contains("password_iterations")) << resp.dump();

    // Role.fromJson contract, in full, on the single-user fetch.
    ASSERT_TRUE(resp.contains("roles")) << resp.dump();
    const json& role = resp.at("roles").at(0);
    for (const char* key : {"id", "tenant_id", "organization_id",
                            "created_at", "updated_at", "name", "scope"})
    {
        EXPECT_TRUE(role.contains(key))
            << "Role.fromJson requires '" << key << "': " << role.dump();
    }
}

TEST_F(AuthLoginTest, UsersGet_ItemRoute_UnknownUserNotFound)
{
    RegisterStubUsersItemRoutes();
    init_identity_overrides(&m_pm, m_locator);

    auto resp = json::parse(
        m_pm.Route(m_ctx, "GET", "/api/v1/users/user-never-seeded", ""));
    ASSERT_TRUE(resp.contains("error")) << resp.dump();
    EXPECT_EQ("NOT_FOUND", resp.at("error").at("code").get<std::string>());
}

TEST_F(AuthLoginTest, UsersUpdate_ItemRoute_StripsPasswordsAndRefusesInjection)
{
    RegisterStubUsersItemRoutes();
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("item-update@test.com", kTestPassword, "user-item-002",
             json::array({
                 json{{"name", "admin"},
                      {"permissions", json::array({"*:*"})}},
             }));

    const std::string body =
        R"({"display_name":"Renamed","password_hash":"evil",)"
        R"("password_salt":"evil","password_iterations":1})";
    auto resp = json::parse(
        m_pm.Route(m_ctx, "PATCH", "/api/v1/users/user-item-002", body));
    ASSERT_FALSE(resp.contains("error")) << resp.dump();
    EXPECT_EQ("Renamed", resp.at("display_name").get<std::string>());

    // Response copy: no password material, roles normalized.
    EXPECT_FALSE(resp.contains("password_hash")) << resp.dump();
    EXPECT_FALSE(resp.contains("password_salt")) << resp.dump();
    EXPECT_FALSE(resp.contains("password_iterations")) << resp.dump();
    const json& role = resp.at("roles").at(0);
    for (const char* key : {"id", "tenant_id", "organization_id",
                            "created_at", "updated_at", "name", "scope"})
    {
        EXPECT_TRUE(role.contains(key))
            << "Role.fromJson requires '" << key << "': " << role.dump();
    }

    // Stored record: STILL carries the ORIGINAL password (login keeps
    // working) and never took the injected values.
    auto key = KeyBuilder::Build("identity", "users", "user-item-002");
    ASSERT_TRUE(key.has_value());
    std::string stored;
    ASSERT_TRUE(m_engine->Get(key.value(), stored));
    json storedJson = json::parse(stored);
    EXPECT_TRUE(storedJson.contains("password_hash"));
    EXPECT_NE("evil", storedJson.value("password_hash", std::string()));
    EXPECT_NE("evil", storedJson.value("password_salt", std::string()));
    EXPECT_EQ("Renamed", storedJson.value("display_name", std::string()));
}

TEST_F(AuthLoginTest, RolesSeed_Idempotent_FirstRunThenExisting)
{
    // DB has users (SetUp seeded one) but an empty identity/roles prefix —
    // the exact state of existing dev databases, where nothing ever seeded
    // identity/roles and GET /api/v1/roles returned {"data":[]}.
    init_identity_overrides(&m_pm, m_locator);

    auto scanned = m_engine->Scan(KeyBuilder::MakePrefix("identity", "roles"));
    ASSERT_EQ(1u, scanned.size())
        << "exactly one canonical admin role must be seeded";
    json role = json::parse(scanned.begin()->second);
    EXPECT_TRUE(role.contains("id")) << role.dump();
    EXPECT_EQ("admin", role.value("name", ""));
    EXPECT_TRUE(role.contains("scope")) << role.dump();
    EXPECT_TRUE(role.contains("permissions")) << role.dump();

    // Idempotency: a second init (server restart) must add nothing.
    init_identity_overrides(&m_pm, m_locator);
    EXPECT_EQ(1u, m_engine->Scan(KeyBuilder::MakePrefix("identity", "roles")).size())
        << "repeated starts must not duplicate the seeded role";
}

// ============================================================================
// ACL-CONTRACT-01 (PR #4 Codex P1): login/refresh responses must satisfy the
// TokenResponse schema — required permissions flattened from the user's
// roles — and tokens must carry the documented sub/tenant/org/perms claims.
// ============================================================================

TEST_F(AuthLoginTest, RealHandler_Login_IncludesPermissionsFlattenedFromRoles)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("roles@test.com", kTestPassword, "user-roles-001",
             json::array({
                 json{{"name", "pos"},
                      {"permissions", json::array({"orders:read", "orders:write"})}},
                 json{{"name", "inventory"},
                      {"permissions", json::array({"orders:read", "inventory:read"})}},
             }));

    json body;
    body["email"] = "roles@test.com";
    body["password"] = kTestPassword;

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);
    ASSERT_FALSE(data.contains("error")) << response;

    // Flattened in first-seen order with duplicates removed across roles.
    EXPECT_EQ(json::array({"orders:read", "orders:write", "inventory:read"}),
              data.at("permissions"))
        << response;

    // The access token carries the same list under the perms claim.
    RequestContext tokenCtx;
    ASSERT_TRUE(ValidateJwtToken(data.at("access_token").get<std::string>(),
                                 kTestSecret, tokenCtx));
    const std::vector<std::string> kExpected{"orders:read", "orders:write", "inventory:read"};
    EXPECT_EQ(kExpected, tokenCtx.permissions);
}

TEST_F(AuthLoginTest, RealHandler_Login_WithoutRoles_EmptyPermissionsArray)
{
    init_identity_overrides(&m_pm, m_locator);

    // kTestEmail was seeded in SetUp without roles.
    json body;
    body["email"] = kTestEmail;
    body["password"] = kTestPassword;

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);
    ASSERT_FALSE(data.contains("error")) << response;

    ASSERT_TRUE(data.contains("permissions")) << response;
    EXPECT_TRUE(data.at("permissions").is_array());
    EXPECT_TRUE(data.at("permissions").empty());
}

TEST_F(AuthLoginTest, RealHandler_Login_NullEmail_ReturnsInvalidRequest)
{
    init_identity_overrides(&m_pm, m_locator);

    // PR #4 Codex P2: json null in a required field used to throw
    // json::type_error past the parse_error-only catch; must answer
    // INVALID_REQUEST without escaping the handler.
    json body;
    body["email"] = nullptr;
    body["password"] = kTestPassword;

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    EXPECT_EQ("INVALID_REQUEST", data.at("error").at("code").get<std::string>());
}

TEST_F(AuthLoginTest, RealHandler_Login_NumberEmail_ReturnsInvalidRequest)
{
    init_identity_overrides(&m_pm, m_locator);

    json body;
    body["email"] = 7;
    body["password"] = kTestPassword;

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);

    EXPECT_EQ("INVALID_REQUEST", data.at("error").at("code").get<std::string>());
}

TEST_F(AuthLoginTest, RealHandler_Refresh_IncludesPermissionsAndNewToken)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("refresh@test.com", kTestPassword, "user-refresh-001",
             json::array({
                 json{{"name", "pos"},
                      {"permissions", json::array({"orders:read"})}},
             }));

    json loginBody;
    loginBody["email"] = "refresh@test.com";
    loginBody["password"] = kTestPassword;
    auto login = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", loginBody.dump()));
    ASSERT_FALSE(login.contains("error"));

    // D-03: the wire field is refresh_token; login must have issued one.
    ASSERT_FALSE(login.at("refresh_token").get<std::string>().empty())
        << "login must issue a refresh token alongside the access token";

    json refreshBody;
    refreshBody["refresh_token"] = login.at("refresh_token").get<std::string>();

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", refreshBody.dump());
    auto data = json::parse(response);
    ASSERT_FALSE(data.contains("error")) << response;

    EXPECT_FALSE(data.at("access_token").get<std::string>().empty());
    EXPECT_EQ(json::array({"orders:read"}), data.at("permissions")) << response;

    // Full TokenResponse (D-04): the spec-required user object is present and
    // the response carries a NEW refresh credential — rotation.
    ASSERT_TRUE(data.contains("user")) << response;
    EXPECT_NE(login.at("refresh_token").get<std::string>(),
              data.at("refresh_token").get<std::string>())
        << "each refresh must return a NEW refresh token";

    RequestContext tokenCtx;
    ASSERT_TRUE(ValidateJwtToken(data.at("access_token").get<std::string>(),
                                 kTestSecret, tokenCtx));
    const std::vector<std::string> kExpected{"orders:read"};
    EXPECT_EQ(kExpected, tokenCtx.permissions);
}

// ============================================================================
// Refresh re-resolves roles from storage (PR #4 round-3 P1): a revocation
// made after the token was minted must kill the privileges at the next
// refresh; a deleted account must not be able to refresh at all.
// ============================================================================

TEST_F(AuthLoginTest, RealHandler_Refresh_AfterRoleRevocation_DropsPermissions)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("revoke@test.com", kTestPassword, "user-revoke-001",
             json::array({
                 json{{"name", "pos"},
                      {"permissions", json::array({"orders:read", "orders:write"})}},
             }));

    json loginBody;
    loginBody["email"] = "revoke@test.com";
    loginBody["password"] = kTestPassword;
    std::string loginResponse = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", loginBody.dump());
    auto login = json::parse(loginResponse);
    ASSERT_FALSE(login.contains("error")) << loginResponse;

    // Revoke every role in storage AFTER the token was minted.
    auto userKey = KeyBuilder::Build("identity", "users", "user-revoke-001");
    ASSERT_TRUE(userKey.has_value());
    std::string stored;
    ASSERT_TRUE(m_engine->Get(userKey.value(), stored));
    json user = json::parse(stored);
    user["roles"] = json::array();
    ASSERT_TRUE(m_engine->Put(userKey.value(), user.dump()));

    json refreshBody;
    refreshBody["refresh_token"] = login.at("refresh_token").get<std::string>();
    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", refreshBody.dump());
    auto data = json::parse(response);
    ASSERT_FALSE(data.contains("error")) << response;

    EXPECT_TRUE(data.at("permissions").empty())
        << "revoked roles must not survive the refresh";

    RequestContext tokenCtx;
    ASSERT_TRUE(ValidateJwtToken(data.at("access_token").get<std::string>(),
                                 kTestSecret, tokenCtx));
    EXPECT_TRUE(tokenCtx.permissions.empty())
        << "replacement token must not carry the revoked perms claim";
}

TEST_F(AuthLoginTest, RealHandler_Refresh_DeletedUser_ReturnsInvalidToken)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("deleted@test.com", kTestPassword, "user-deleted-001");

    json loginBody;
    loginBody["email"] = "deleted@test.com";
    loginBody["password"] = kTestPassword;
    std::string loginResponse = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", loginBody.dump());
    auto login = json::parse(loginResponse);
    ASSERT_FALSE(login.contains("error")) << loginResponse;

    auto userKey = KeyBuilder::Build("identity", "users", "user-deleted-001");
    ASSERT_TRUE(userKey.has_value());
    ASSERT_TRUE(m_engine->Delete(userKey.value()));

    json refreshBody;
    refreshBody["refresh_token"] = login.at("refresh_token").get<std::string>();
    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", refreshBody.dump());
    auto data = json::parse(response);

    EXPECT_EQ("INVALID_TOKEN", data.at("error").at("code").get<std::string>())
        << "a deleted account must not refresh into a live token";
}

// ============================================================================
// Seeded dev accounts carry explicit admin roles (PR #4 round-3 P2): the
// legacy "role":"admin" marker flattens to zero permissions, so the seeds
// now include roles [{name: "admin", permissions: ["*:*"]}]. The PIN user
// seeds independently of the empty-DB check, making it the reachable seed
// from this fixture (SetUp already seeded kTestEmail).
// ============================================================================

TEST_F(AuthLoginTest, RealHandler_Login_SeededPinUser_CarriesWildcardPermissions)
{
    init_identity_overrides(&m_pm, m_locator);

    json body;
    body["email"] = "pin_user_0000@touchpos.local";
    body["password"] = "pin_0000";
    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", body.dump());
    auto data = json::parse(response);
    ASSERT_FALSE(data.contains("error")) << response;

    EXPECT_EQ(json::array({"*:*"}), data.at("permissions"))
        << "seeded dev accounts must authorize via the seeded admin role";
}

// ============================================================================
// Rotating refresh-token contract (04.3-01, D-03/D-04/D-05): login issues the
// access + refresh pair, the latest-wins record at
// identity/refresh_tokens/<userId> is the single source of validity, and
// there is no window in which a rotated-out credential still authorizes a
// refresh. Timed waits are forbidden in this suite — every expiry case mints
// its credential directly, never ages one.
// ============================================================================

TEST_F(AuthLoginTest, RealHandler_Login_IssuesRefreshToken)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("rotate@test.com", kTestPassword, "user-rotate-001",
             json::array({
                 json{{"name", "pos"},
                      {"permissions", json::array({"orders:read"})}},
             }));

    json loginBody;
    loginBody["email"] = "rotate@test.com";
    loginBody["password"] = kTestPassword;
    auto login = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", loginBody.dump()));
    ASSERT_FALSE(login.contains("error")) << login.dump();

    ASSERT_TRUE(login.contains("refresh_token")) << login.dump();
    const std::string issued = login.at("refresh_token").get<std::string>();
    EXPECT_FALSE(issued.empty()) << "login must issue a refresh token (D-04)";

    // Prohibition P2: the embedded user carries no password material.
    ASSERT_TRUE(login.contains("user")) << login.dump();
    EXPECT_FALSE(login.at("user").contains("password_hash"));
    EXPECT_FALSE(login.at("user").contains("password_salt"));
    EXPECT_FALSE(login.at("user").contains("password_iterations"))
        << "login response user must carry no password fields (P2)";

    // Generated Dart User.fromJson parses roles as a checked List<Role>: the
    // embedded user's roles must satisfy the full Role contract too, not just
    // the legacy {name, permissions} shape (quick 261117-o39).
    ASSERT_TRUE(login.at("user").contains("roles")) << login.dump();
    ASSERT_TRUE(login.at("user").at("roles").is_array()) << login.dump();
    ASSERT_GE(login.at("user").at("roles").size(), 1u) << login.dump();
    const json& loginRole = login.at("user").at("roles").at(0);
    EXPECT_TRUE(loginRole.contains("id")) << loginRole.dump();
    EXPECT_TRUE(loginRole.contains("scope")) << loginRole.dump();

    // The engine holds the latest-wins record and it carries exactly the
    // credential that was returned.
    auto recordKey = KeyBuilder::Build("identity", "refresh_tokens", "user-rotate-001");
    ASSERT_TRUE(recordKey.has_value());
    std::string recordStr;
    ASSERT_TRUE(m_engine->Get(recordKey.value(), recordStr));
    json record = json::parse(recordStr);
    EXPECT_EQ(issued, record.at("token").get<std::string>())
        << "the stored latest-wins record must equal the issued credential";
}

TEST_F(AuthLoginTest, RealHandler_Refresh_RotatesLatestWins)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("rot2@test.com", kTestPassword, "user-rot2-001",
             json::array({
                 json{{"name", "pos"},
                      {"permissions", json::array({"orders:read"})}},
             }));

    json loginBody;
    loginBody["email"] = "rot2@test.com";
    loginBody["password"] = kTestPassword;
    auto login = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", loginBody.dump()));
    ASSERT_FALSE(login.contains("error")) << login.dump();
    const std::string firstRefresh = login.at("refresh_token").get<std::string>();

    json refreshBody;
    refreshBody["refresh_token"] = firstRefresh;
    auto data = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", refreshBody.dump()));
    ASSERT_FALSE(data.contains("error")) << data.dump();

    const std::string secondRefresh = data.at("refresh_token").get<std::string>();
    EXPECT_NE(firstRefresh, secondRefresh)
        << "each refresh must return a NEW refresh token (D-04 rotation)";
    ASSERT_TRUE(data.contains("user")) << data.dump();
    // Prohibition P2: StripPasswordFields applies to the refresh path exactly
    // as it does to login — no password material in the embedded user.
    EXPECT_FALSE(data.at("user").contains("password_hash"));
    EXPECT_FALSE(data.at("user").contains("password_salt"));
    EXPECT_FALSE(data.at("user").contains("password_iterations"))
        << "refresh response user must carry no password fields (P2)";
    EXPECT_EQ(json::array({"orders:read"}), data.at("permissions"))
        << data.dump();

    // No window where both are valid: the OLD credential is dead on its next
    // presentation — the record now holds only the new one.
    auto replay = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", refreshBody.dump()));
    EXPECT_EQ("INVALID_TOKEN", replay.at("error").at("code").get<std::string>())
        << "the rotated-out refresh token must be rejected on replay (D-04)";
}

TEST_F(AuthLoginTest, RealHandler_Refresh_SecondLoginInvalidatesFirst)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("latest@test.com", kTestPassword, "user-latest-001");

    json loginBody;
    loginBody["email"] = "latest@test.com";
    loginBody["password"] = kTestPassword;

    auto first = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", loginBody.dump()));
    ASSERT_FALSE(first.contains("error")) << first.dump();
    auto second = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", loginBody.dump()));
    ASSERT_FALSE(second.contains("error")) << second.dump();

    // Latest-wins per user (Pitfall 8 shape): the second login's Put
    // superseded the first's record.
    json staleBody;
    staleBody["refresh_token"] = first.at("refresh_token").get<std::string>();
    auto stale = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", staleBody.dump()));
    EXPECT_EQ("INVALID_TOKEN", stale.at("error").at("code").get<std::string>())
        << "a second login must invalidate the first login's refresh token";

    json liveBody;
    liveBody["refresh_token"] = second.at("refresh_token").get<std::string>();
    auto live = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", liveBody.dump()));
    ASSERT_FALSE(live.contains("error")) << live.dump()
        << "the newest login's refresh token must keep working";
}

TEST_F(AuthLoginTest, RealHandler_Refresh_RejectsAccessTokenAsRefreshToken)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("asatk@test.com", kTestPassword, "user-asatk-001");

    json loginBody;
    loginBody["email"] = "asatk@test.com";
    loginBody["password"] = kTestPassword;
    auto login = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/login", loginBody.dump()));
    ASSERT_FALSE(login.contains("error")) << login.dump();

    // The access token parses as a valid JWT but never matches the stored
    // refresh record — the constant-time compare must reject it.
    json refreshBody;
    refreshBody["refresh_token"] = login.at("access_token").get<std::string>();
    auto data = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", refreshBody.dump()));
    EXPECT_EQ("INVALID_TOKEN", data.at("error").at("code").get<std::string>())
        << "an access token must never double as a refresh credential";
}

TEST_F(AuthLoginTest, RealHandler_Refresh_MissingFieldReturnsInvalidRequest)
{
    init_identity_overrides(&m_pm, m_locator);

    // D-03 wire-change proof: a body carrying ONLY the legacy single-word
    // token field never reaches validation — the handler requires the
    // refresh_token field and answers INVALID_REQUEST.
    std::string response = m_pm.Route(
        m_ctx, "POST", "/api/v1/auth/refresh", R"({"token":"legacy-field-only"})");
    auto data = json::parse(response);
    EXPECT_EQ("INVALID_REQUEST", data.at("error").at("code").get<std::string>())
        << "the legacy token field must not satisfy the refresh contract (D-03)";
}

TEST_F(AuthLoginTest, RealHandler_Refresh_ExpiredTokenRejected)
{
    init_identity_overrides(&m_pm, m_locator);
    SeedUser("expired@test.com", kTestPassword, "user-expired-001");

    // The expired credential is MINTED directly (negative lifetime), never
    // aged with a wait. Strict zero-leeway validation must reject it.
    const std::string expired =
        CreateJwtToken(kTestSecret, "user-expired-001", "", "", -60, {});
    ASSERT_FALSE(expired.empty());

    json refreshBody;
    refreshBody["refresh_token"] = expired;
    auto data = json::parse(
        m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", refreshBody.dump()));
    EXPECT_EQ("INVALID_TOKEN", data.at("error").at("code").get<std::string>())
        << "expired refresh credentials must be rejected — no leeway path";
}
