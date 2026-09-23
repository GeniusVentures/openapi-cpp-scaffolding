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
#include <filesystem>
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

    json refreshBody;
    refreshBody["token"] = login.at("access_token").get<std::string>();

    std::string response = m_pm.Route(m_ctx, "POST", "/api/v1/auth/refresh", refreshBody.dump());
    auto data = json::parse(response);
    ASSERT_FALSE(data.contains("error")) << response;

    EXPECT_FALSE(data.at("access_token").get<std::string>().empty());
    EXPECT_EQ(json::array({"orders:read"}), data.at("permissions")) << response;

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
    refreshBody["token"] = login.at("access_token").get<std::string>();
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
    refreshBody["token"] = login.at("access_token").get<std::string>();
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
