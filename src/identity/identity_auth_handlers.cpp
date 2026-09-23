/**
 * @file       identity_auth_handlers.cpp
 * @brief      Real auth handler implementations for the identity plugin
 * @date       2026-06-10
 * @author     Kenneth L. Hurley
 *
 * init_identity_overrides(pm, locator) — called from hand-written
 * identity_plugin.cpp (IdentityPluginImpl::Initialize). Sets up storage/JWT,
 * seeds admin, registers auth handlers at kOverrideHandlerPriority (200).
 */

#include "identity/identity_auth_handlers.hpp"
#include "singleton/PluginRegistration.hpp"
#include "singleton/IServiceLocator.hpp"
#include "singleton/fnv1a.hpp"
#include "identity/auth_utils.hpp"
#include "storage/IStorageEngine.hpp"
#include "storage/KeyBuilder.hpp"
#include "nlohmann/json.hpp"
#include <spdlog/spdlog.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <sstream>

using json = nlohmann::json;
using namespace gnus::hash;

static IStorageEngine* s_storage = nullptr;
static std::string s_jwtSecret;

// ============================================================================
// Auth Constants
// ============================================================================

static constexpr int kTokenExpirySeconds   = 3600;   ///< Access-token lifetime
static constexpr int kRefreshLeewaySeconds = 86400;  ///< Refresh accepts tokens expired ≤24h

// ============================================================================
// Hex Helpers
// ============================================================================

static constexpr const char kHex[] = "0123456789abcdef";

static std::string HexDecode(const std::string& hex) noexcept
{
    if (hex.size() % 2 != 0) { return {}; }
    auto hv = [](char c) { return (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; };
    std::string r; r.reserve(hex.size()/2);
    for (size_t i = 0; i < hex.size(); i += 2) { int hi=hv(hex[i]), lo=hv(hex[i+1]); if (hi<0||lo<0) return {}; r+=char((hi<<4)|lo); }
    return r;
}

static std::string HexEncode(const std::string& input) noexcept
{
    std::string r; r.reserve(input.size()*2);
    for (unsigned char b : input) { r += kHex[(b>>4)&0xF]; r += kHex[b&0xF]; }
    return r;
}

static PasswordHash BuildStoredHash(const json& u) noexcept
{
    PasswordHash s;
    if (!u.contains("password_hash") || !u.contains("password_salt")) return s;
    auto hb = HexDecode(u["password_hash"].get<std::string>());
    auto sb = HexDecode(u["password_salt"].get<std::string>());
    s.hash.assign(hb.begin(), hb.end()); s.salt.assign(sb.begin(), sb.end());
    s.iterations = u.value("password_iterations", 100000);
    return s;
}

static void StripPasswordFields(json& u) noexcept { u.erase("password_hash"); u.erase("password_salt"); u.erase("password_iterations"); }

// ============================================================================
// Auth Handlers
// ============================================================================

/**
 * @brief      Flatten a user's role permissions per ACL-CONTRACT-01
 *
 * Walks user["roles"] (array of Role objects) and collects every string from
 * each role's permissions array in first-seen order, skipping duplicates so
 * overlapping roles contribute one entry. Users without a roles array yield
 * an empty list.
 *
 * @param      user   Stored user document
 *
 * @return     Flattened {domain}:{action} permission strings
 */
static std::vector<std::string> FlattenRolePermissions(const json& user)
{
    std::vector<std::string> permissions;
    if (!user.contains("roles") || !user.at("roles").is_array())
    {
        return permissions;
    }
    for (const auto& role : user.at("roles"))
    {
        if (!role.is_object() || !role.contains("permissions") || !role.at("permissions").is_array())
        {
            continue;
        }
        for (const auto& permission : role.at("permissions"))
        {
            if (permission.is_string() &&
                std::find(permissions.begin(), permissions.end(), permission.get<std::string>()) == permissions.end())
            {
                permissions.push_back(permission.get<std::string>());
            }
        }
    }
    return permissions;
}

// ============================================================================
// Seeded-account roles
// ============================================================================

static constexpr const char* kAdminRoleName           = "admin";
static constexpr const char* kAdminWildcardPermission = "*:*";

/**
 * @brief      Default roles document for seeded dev accounts
 *
 * ACL-CONTRACT-01 flattens permissions from user["roles"]; the seeded
 * accounts previously carried only the legacy "role":"admin" marker, which
 * flattens to an EMPTY permission list. Seed an explicit admin role with the
 * wildcard permission so seeded logins actually authorize.
 *
 * @return     roles array [{name: "admin", permissions: ["*:*"]}]
 */
static json DefaultAdminRoles()
{
    return json::array({json{{"name", kAdminRoleName},
                             {"permissions", json::array({kAdminWildcardPermission})}}});
}

static std::string auth_login(const RequestContext& /*ctx*/, const std::string& /*m*/, const std::string& /*p*/, const std::string& body)
{
    try {
        json req = json::parse(body);
        if (!req.contains("email") || !req.contains("password"))
            return R"({"error":{"code":"INVALID_REQUEST","message":"Email and password are required"}})";
        std::string email = req["email"].get<std::string>();
        std::string pass  = req["password"].get<std::string>();

        std::string userId;
        if (!s_storage->Get("identity/users_by_email/"+email, userId))
            return R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})";

        auto ukr = KeyBuilder::Build("identity", "users", userId);
        if (!ukr.has_value()) return R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})";
        std::string userStr;
        if (!s_storage->Get(ukr.value(), userStr))
            return R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})";

        json u = json::parse(userStr);
        if (!VerifyPassword(pass, BuildStoredHash(u)))
            return R"({"error":{"code":"INVALID_CREDENTIALS","message":"Invalid email or password"}})";

        std::vector<std::string> permissions = FlattenRolePermissions(u);
        std::string tok = CreateJwtToken(s_jwtSecret, userId, u.value("tenant_id",""), u.value("organization_id",""), kTokenExpirySeconds, permissions);
        if (tok.empty()) return R"({"error":{"code":"TOKEN_ERROR","message":"Failed to create token"}})";

        StripPasswordFields(u);
        json r; r["access_token"]=tok; r["token_type"]="Bearer"; r["expires_in"]=kTokenExpirySeconds; r["user"]=u; r["permissions"]=permissions;
        return r.dump();
    } catch (const json::parse_error&) { return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})"; }
      catch (const json::exception&)   { return R"({"error":{"code":"INVALID_REQUEST","message":"Request body fields have invalid types"}})"; }
}

static std::string auth_logout(const RequestContext&, const std::string&, const std::string&, const std::string&)
{ return R"({"success":true})"; }

static std::string auth_getCurrentUser(const RequestContext& ctx, const std::string&, const std::string&, const std::string&)
{
    if (ctx.userId.empty()) return R"({"error":{"code":"UNAUTHORIZED","message":"No authenticated user"}})";
    auto ukr = KeyBuilder::Build("identity", "users", ctx.userId);
    if (!ukr.has_value()) return R"({"error":{"code":"NOT_FOUND","message":"Invalid user ID"}})";
    std::string userStr;
    if (!s_storage->Get(ukr.value(), userStr)) return R"({"error":{"code":"NOT_FOUND","message":"User not found"}})";
    json u = json::parse(userStr); StripPasswordFields(u); return u.dump();
}

static std::string auth_refreshToken(const RequestContext&, const std::string&, const std::string&, const std::string& body)
{
    try {
        json req = json::parse(body);
        if (!req.contains("token")) return R"({"error":{"code":"INVALID_REQUEST","message":"Token is required"}})";
        std::string old = req["token"].get<std::string>();

        // Validate with refresh leeway, then re-resolve the user's CURRENT
        // roles from storage — never carry the old token's perms claim, so a
        // revoked role dies at the next refresh even while the old token is
        // still within its refresh window.
        RequestContext ctx;
        if (!ValidateJwtToken(old, s_jwtSecret, ctx, kRefreshLeewaySeconds))
            return R"({"error":{"code":"INVALID_TOKEN","message":"Token is invalid or expired beyond refresh window"}})";

        auto ukr = KeyBuilder::Build("identity", "users", ctx.userId);
        if (!ukr.has_value()) return R"({"error":{"code":"INVALID_TOKEN","message":"Account no longer exists"}})";
        std::string userStr;
        if (!s_storage->Get(ukr.value(), userStr))
            return R"({"error":{"code":"INVALID_TOKEN","message":"Account no longer exists"}})";

        json u = json::parse(userStr);
        std::vector<std::string> permissions = FlattenRolePermissions(u);
        std::string nw = CreateJwtToken(s_jwtSecret, ctx.userId, u.value("tenant_id", ctx.tenantId), u.value("organization_id", ctx.organizationId), kTokenExpirySeconds, permissions);
        if (nw.empty()) return R"({"error":{"code":"TOKEN_ERROR","message":"Failed to create token"}})";

        json r; r["access_token"]=nw; r["token_type"]="Bearer"; r["expires_in"]=kTokenExpirySeconds; r["permissions"]=permissions;
        return r.dump();
    } catch (const json::parse_error&) { return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})"; }
      catch (const json::exception&)   { return R"({"error":{"code":"INVALID_REQUEST","message":"Request body fields have invalid types"}})"; }
}

// ============================================================================
// Users Create Override
//
// Restores the hand-written create semantics (4d8144a) lost when generated
// CRUD took over POST /api/v1/users at the 7da1328 relocation: the User
// response schema requires "status", which the generic handler never sets.
// ============================================================================

static constexpr unsigned int kUuidHexLength    = 32;
static constexpr uint8_t      kHexDigitMax      = 15;
static constexpr const char*  kHexChars         = "0123456789abcdef";
static constexpr const char*  kUserStatusActive = "active";

/// Generate a random UUID (32 hex characters, no hyphens)
static std::string GenerateUserUuid() noexcept
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
 * @brief      Create a user with contract-required defaults
 *
 * Injects the fields the generic CRUD handler cannot: status defaults to
 * active (required by the User response schema), display_name falls back
 * to email. Tenant/organization come from the request context.
 *
 * @param      ctx    Request context (tenant/organization stamping)
 * @param      body   JSON body — UserCreate (email required)
 *
 * @return     JSON user record, or error JSON
 */
static std::string users_create(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& body)
{
    try
    {
        json requestData = json::parse(body);
        if (!requestData.contains("email"))
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Email is required"}})";
        }
        std::string email = requestData["email"].get<std::string>();
        std::string id = GenerateUserUuid();
        requestData["id"] = id;
        requestData["display_name"] = requestData.value("display_name", email);
        requestData["tenant_id"] = ctx.tenantId;
        requestData["organization_id"] = ctx.organizationId;
        requestData["status"] = kUserStatusActive;
        requestData["created_at"] = GetCurrentTimestamp();
        requestData["updated_at"] = requestData["created_at"];

        auto keyResult = KeyBuilder::Build("identity", "users", id);
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
    catch (const json::parse_error&)
    {
        return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})";
    }
    catch (const json::exception&)
    {
        return R"({"error":{"code":"INVALID_REQUEST","message":"Request body fields have invalid types"}})";
    }
}

// ============================================================================
// init_identity_overrides — called from generated Initialize()
// ============================================================================

void init_identity_overrides(PluginManager* pm, IServiceLocator& locator)
{
    s_storage = locator.GetService<IStorageEngine>(Fnv1a("StorageEngine"));
    auto* s = locator.GetService<std::string>(Fnv1a("JwtSecret"));
    if (s) s_jwtSecret = *s;
    if (!s_storage) return;

    // Seed admin if DB empty
    if (s_storage->Scan(KeyBuilder::MakePrefix("identity", "users")).empty())
    {
        auto hr = HashPassword("admin");
        if (!hr.salt.empty())
        {
            json u; u["id"]="admin-00000000000000000000000000000001"; u["email"]="admin";
            u["display_name"]="Admin"; u["tenant_id"]="default"; u["organization_id"]="default";
            u["role"]="admin"; u["roles"]=DefaultAdminRoles(); u["status"]="active"; u["created_at"]="2026-01-01T00:00:00Z"; u["updated_at"]="2026-01-01T00:00:00Z";
            u["password_hash"]=HexEncode(std::string(hr.hash.begin(),hr.hash.end()));
            u["password_salt"]=HexEncode(std::string(hr.salt.begin(),hr.salt.end()));
            u["password_iterations"]=hr.iterations;
            auto uk = KeyBuilder::Build("identity","users",u["id"].get<std::string>());
            auto ek = KeyBuilder::Build("identity","users_by_email","admin");
            if (uk.has_value() && ek.has_value())
            {
                s_storage->WriteBatch({{uk.value(),u.dump()},{ek.value(),u["id"].get<std::string>()}},{});
                SPDLOG_INFO("Seeded admin user (email: admin, password: admin)");
            }
        }
    }

    // Seed touch-pos PIN quick-login user if absent (dev PIN 0000). Seeded
    // independently of the empty-DB check so existing dev databases get it too.
    {
        std::string existingPinUser;
        auto pk = KeyBuilder::Build("identity", "users_by_email", "pin_user_0000@touchpos.local");
        if (pk.has_value() && !s_storage->Get(pk.value(), existingPinUser))
        {
            auto hr = HashPassword("pin_0000");
            if (!hr.salt.empty())
            {
                json u; u["id"]=GenerateUserUuid(); u["email"]="pin_user_0000@touchpos.local";
                u["display_name"]="Pin User"; u["tenant_id"]="default"; u["organization_id"]="default";
                u["role"]="admin"; u["roles"]=DefaultAdminRoles(); u["status"]="active"; u["created_at"]="2026-01-01T00:00:00Z"; u["updated_at"]="2026-01-01T00:00:00Z";
                u["password_hash"]=HexEncode(std::string(hr.hash.begin(),hr.hash.end()));
                u["password_salt"]=HexEncode(std::string(hr.salt.begin(),hr.salt.end()));
                u["password_iterations"]=hr.iterations;
                auto uk = KeyBuilder::Build("identity","users",u["id"].get<std::string>());
                auto ek = KeyBuilder::Build("identity","users_by_email","pin_user_0000@touchpos.local");
                if (uk.has_value() && ek.has_value())
                {
                    s_storage->WriteBatch({{uk.value(),u.dump()},{ek.value(),u["id"].get<std::string>()}},{});
                    SPDLOG_INFO("Seeded touch-pos PIN user (email: pin_user_0000@touchpos.local, PIN: 0000)");
                }
            }
        }
    }

    pm->RegisterHandler("POST", "/api/v1/auth/login",    "auth_login",         auth_login,         "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/auth/logout",   "auth_logout",        auth_logout,        "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("GET",  "/api/v1/auth/me",       "auth_getCurrentUser",auth_getCurrentUser,"Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/auth/refresh",  "auth_refreshToken",  auth_refreshToken,  "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/users",         "users_create",       users_create,       "Identity", kOverrideHandlerPriority);
}
