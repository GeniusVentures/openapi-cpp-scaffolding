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

using json = nlohmann::json;
using namespace gnus::hash;

static IStorageEngine* s_storage = nullptr;
static std::string s_jwtSecret;

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

        std::string tok = CreateJwtToken(s_jwtSecret, userId, u.value("tenant_id",""), u.value("organization_id",""), 3600);
        if (tok.empty()) return R"({"error":{"code":"TOKEN_ERROR","message":"Failed to create token"}})";

        StripPasswordFields(u);
        json r; r["access_token"]=tok; r["token_type"]="Bearer"; r["expires_in"]=3600; r["user"]=u;
        return r.dump();
    } catch (const json::parse_error&) { return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})"; }
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
        std::string old = req["token"].get<std::string>(), nw;
        if (!RefreshJwtToken(old, s_jwtSecret, 86400, nw, 3600))
            return R"({"error":{"code":"INVALID_TOKEN","message":"Token is invalid or expired beyond refresh window"}})";
        json r; r["access_token"]=nw; r["token_type"]="Bearer"; r["expires_in"]=3600;
        return r.dump();
    } catch (const json::parse_error&) { return R"({"error":{"code":"PARSE_ERROR","message":"Invalid JSON body"}})"; }
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
            u["role"]="admin"; u["status"]="active"; u["created_at"]="2026-01-01T00:00:00Z"; u["updated_at"]="2026-01-01T00:00:00Z";
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

    pm->RegisterHandler("POST", "/api/v1/auth/login",    "auth_login",         auth_login,         "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/auth/logout",   "auth_logout",        auth_logout,        "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("GET",  "/api/v1/auth/me",       "auth_getCurrentUser",auth_getCurrentUser,"Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/auth/refresh",  "auth_refreshToken",  auth_refreshToken,  "Identity", kOverrideHandlerPriority);
}
