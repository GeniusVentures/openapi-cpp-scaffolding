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
#include <openssl/crypto.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>

using json = nlohmann::json;
using namespace gnus::hash;

static IStorageEngine* s_storage = nullptr;
static std::string s_jwtSecret;

// ============================================================================
// Auth Constants
// ============================================================================

static constexpr int kTokenExpirySeconds       = 3600;   ///< Access-token lifetime (D-05)
static constexpr int kRefreshTokenExpirySeconds = 604800; ///< Refresh-credential lifetime — 7d (D-05)

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
// Role contract (regenerated Dart client — checked deserialization)
// ============================================================================
//
// The regenerated api_client (engine 7.23.0) parses User.roles with
// checked: true: Role.fromJson's $checkKeys REQUIRES id, tenant_id,
// organization_id, created_at, updated_at, name and scope on every entry.
// Stored users embed the ACL-CONTRACT-01 shape ({name, permissions} only),
// so user-emitting paths backfill the display-only keys at read time while
// preserving name/permissions verbatim — authorization decisions stay
// server-side over the stored permission data (T-o39-02 accepted).

static constexpr const char* kAdminRoleName              = "admin";
static constexpr const char* kAdminWildcardPermission    = "*:*";
/// Deterministic id for the seeded/normalized admin role — stable across
/// restarts so normalized output and the identity/roles seed agree.
static constexpr const char* kAdminRoleId                = "adminrole00000000000000000000001";
/// RoleScopeEnum values are tenant | organization | location; the seeded
/// admin path only ever uses tenant.
static constexpr const char* kRoleDefaultScope           = "tenant";
static constexpr const char* kRoleDefaultTenantId        = "default";
static constexpr const char* kRoleDefaultOrganizationId  = "default";
static constexpr const char* kRoleDefaultTimestamp       = "2026-01-01T00:00:00Z";

static std::string GenerateUserUuid() noexcept; // defined below (Users Create Override)

/**
 * @brief      Backfill the Role contract's required keys on user["roles"] in place
 *
 * For each element of user["roles"] (when it is an array), missing keys are
 * backfilled with deterministic values: id reuses the shared admin-role id
 * constant when name=="admin" (else a generated 32-hex uuid), tenant_id and
 * organization_id default to "default", created_at/updated_at to the epoch
 * constant, scope to "tenant". Existing name/permissions pass through
 * untouched so FlattenRolePermissions (ACL-CONTRACT-01) behavior is
 * unchanged. Users without a roles array (or with non-object entries) are
 * left as-is.
 *
 * @param      user   Stored user document (mutated in place)
 */
static void NormalizeEmbeddedRoles(json& user) noexcept
{
    if (!user.contains("roles") || !user.at("roles").is_array())
    {
        return;
    }
    for (auto& role : user.at("roles"))
    {
        if (!role.is_object())
        {
            continue;
        }
        if (!role.contains("id") || !role.at("id").is_string() ||
            role.at("id").get<std::string>().empty())
        {
            const bool isAdminRole = role.contains("name") && role.at("name").is_string() &&
                                     role.at("name").get<std::string>() == kAdminRoleName;
            role["id"] = isAdminRole ? std::string(kAdminRoleId) : GenerateUserUuid();
        }
        if (!role.contains("tenant_id"))       { role["tenant_id"]       = kRoleDefaultTenantId; }
        if (!role.contains("organization_id")) { role["organization_id"] = kRoleDefaultOrganizationId; }
        if (!role.contains("created_at"))      { role["created_at"]      = kRoleDefaultTimestamp; }
        if (!role.contains("updated_at"))      { role["updated_at"]      = kRoleDefaultTimestamp; }
        if (!role.contains("scope"))           { role["scope"]           = kRoleDefaultScope; }
    }
}

/**
 * @brief      Resolve a user document's role_ids/legacy role into embedded roles
 *
 * The admin UI creates and edits users through the UserCreate/UserUpdate
 * contract, whose only role field is role_ids[] — the create/update paths
 * then stored that key verbatim, and users_list (which normalizes only
 * "roles") showed role: null for every created user (walk 2026-11-17 N1).
 * This helper is the one write-side agreement point: a non-empty role_ids
 * array REPLACES the embedded roles with the identity/roles records it
 * names (contract-complete copies — NormalizeEmbeddedRoles backfills any
 * missing key); a record no role row matches keeps its id as a minimal
 * embedded role so a picked role never silently vanishes. A legacy
 * "role": "<name>" string fills in only when no roles exist (the seeded
 * accounts already carry both).
 *
 * Storage is read through s_storage (identity/roles prefix); callers are
 * the users_create/users_update/users_list handlers, all post-init.
 *
 * @param      user  User document, mutated in place (role_ids removed)
 */
static void ResolveRolesFromRoleIds(json& user)
{
    // Authoritative input: a non-empty role_ids array replaces the roles.
    if (user.contains("role_ids") && user.at("role_ids").is_array() &&
        !user.at("role_ids").empty())
    {
        json roles = json::array();
        for (const auto& roleId : user.at("role_ids"))
        {
            if (!roleId.is_string() || roleId.get<std::string>().empty())
            {
                continue;
            }
            const std::string id = roleId.get<std::string>();
            json role;
            auto keyResult = KeyBuilder::Build("identity", "roles", id);
            std::string stored;
            if (keyResult.has_value() && s_storage->Get(keyResult.value(), stored))
            {
                try
                {
                    role = json::parse(stored);
                }
                catch (const json::exception&)
                {
                    role = json::object();
                }
            }
            if (!role.is_object())
            {
                // Unknown id — keep the pick visible rather than dropping it.
                role = json{{"id", id}, {"name", id}, {"permissions", json::array()}};
            }
            if (!role.contains("id") || !role.at("id").is_string() ||
                role.at("id").get<std::string>().empty())
            {
                role["id"] = id;
            }
            roles.push_back(role);
        }
        user["roles"] = roles;
        user.erase("role_ids");
        return;
    }

    // Legacy writer: a role NAME with no roles array — resolve by name.
    if ((!user.contains("roles") || !user.at("roles").is_array() ||
         user.at("roles").empty()) &&
        user.contains("role") && user.at("role").is_string())
    {
        const std::string name = user.at("role").get<std::string>();
        if (!name.empty())
        {
            auto scanResult = s_storage->Scan(KeyBuilder::MakePrefix("identity", "roles"));
            for (const auto& [k, v] : scanResult)
            {
                try
                {
                    json record = json::parse(v);
                    if (record.is_object() && record.contains("name") &&
                        record.at("name").is_string() &&
                        record.at("name").get<std::string>() == name)
                    {
                        user["roles"] = json::array({record});
                        NormalizeEmbeddedRoles(user);
                        return;
                    }
                }
                catch (const json::exception&)
                {
                    continue;
                }
            }
            // No role row carries that name — minimal embedded role.
            json role = json{{"id", GenerateUserUuid()}, {"name", name}};
            user["roles"] = json::array({role});
            NormalizeEmbeddedRoles(user);
        }
    }
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
 * @brief      Mint a rotating refresh credential and persist the latest-wins record (D-04)
 *
 * One refresh token per user: the record at identity/refresh_tokens/<userId>
 * is overwritten on every login/refresh, so issuing a new credential
 * invalidates the previously issued one atomically in a single key. The
 * credential carries a random jti claim so two mints for the same user in the
 * same second still differ byte-for-byte.
 *
 * @param      userId    User the credential belongs to (record key + sub claim)
 * @param      tenantId  Tenant claim (from the current user document)
 * @param      orgId     Organization claim (from the current user document)
 * @param      tokenOut  Output — the minted refresh credential (empty on failure)
 *
 * @return     nullopt on success; the error-envelope JSON string on failure
 */
static std::optional<std::string> IssueRefreshToken(const std::string& userId,
                                                    const std::string& tenantId,
                                                    const std::string& orgId,
                                                    std::string&       tokenOut)
{
    tokenOut = CreateRefreshToken(s_jwtSecret, userId, tenantId, orgId, kRefreshTokenExpirySeconds);
    if (tokenOut.empty())
    {
        return std::optional<std::string>(R"({"error":{"code":"TOKEN_ERROR","message":"Failed to create token"}})");
    }

    auto keyResult = KeyBuilder::Build("identity", "refresh_tokens", userId);
    if (!keyResult.has_value())
    {
        return std::optional<std::string>(R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})");
    }

    json record;
    record["token"]      = tokenOut;
    record["updated_at"] = GetCurrentTimestamp();
    if (!s_storage->Put(keyResult.value(), record.dump()))
    {
        return std::optional<std::string>(R"({"error":{"code":"STORAGE_ERROR","message":"Failed to store entity"}})");
    }
    return std::nullopt;
}

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

/**
 * @brief      Default roles document for seeded dev accounts
 *
 * ACL-CONTRACT-01 flattens permissions from user["roles"]; the seeded
 * accounts previously carried only the legacy "role":"admin" marker, which
 * flattens to an EMPTY permission list. Seed an explicit admin role with the
 * wildcard permission so seeded logins actually authorize.
 *
 * The role is contract-complete for the regenerated Dart client (every
 * Role.fromJson $checkKeys required key present: id, tenant_id,
 * organization_id, created_at, updated_at, name, scope) so seeded accounts
 * parse under checked deserialization exactly like listed users.
 *
 * @return     roles array [{id: kAdminRoleId, name: "admin",
 *             permissions: ["*:*"], tenant_id/organization_id: "default",
 *             created_at/updated_at: epoch, scope: "tenant"}]
 */
static json DefaultAdminRoles()
{
    return json::array({json{{"id",               kAdminRoleId},
                             {"name",             kAdminRoleName},
                             {"permissions",      json::array({kAdminWildcardPermission})},
                             {"tenant_id",        kRoleDefaultTenantId},
                             {"organization_id",  kRoleDefaultOrganizationId},
                             {"created_at",       kRoleDefaultTimestamp},
                             {"updated_at",       kRoleDefaultTimestamp},
                             {"scope",            kRoleDefaultScope}}});
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

        // D-04: login issues the access + rotating refresh pair; the mint
        // overwrites the latest-wins record, invalidating any previous login's
        // refresh credential.
        std::string refreshTok;
        if (auto err = IssueRefreshToken(userId, u.value("tenant_id",""), u.value("organization_id",""), refreshTok))
            return *err;

        StripPasswordFields(u);
        NormalizeEmbeddedRoles(u);
        json r; r["access_token"]=tok; r["refresh_token"]=refreshTok; r["token_type"]="Bearer"; r["expires_in"]=kTokenExpirySeconds; r["user"]=u; r["permissions"]=permissions;
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
    json u = json::parse(userStr); StripPasswordFields(u); NormalizeEmbeddedRoles(u); return u.dump();
}

static std::string auth_refreshToken(const RequestContext&, const std::string&, const std::string&, const std::string& body)
{
    try {
        json req = json::parse(body);
        // D-03: the wire field is refresh_token; the legacy token field no
        // longer exists on this handler.
        if (!req.contains("refresh_token") || !req["refresh_token"].is_string() || req["refresh_token"].get<std::string>().empty())
            return R"({"error":{"code":"INVALID_REQUEST","message":"Refresh token is required"}})";
        std::string presented = req["refresh_token"].get<std::string>();

        // D-04: STRICT validation — zero leeway; the stored latest-wins record
        // is the single validity notion. Then re-resolve the user's CURRENT
        // roles from storage — never carry the old credential's perms claim,
        // so a revoked role dies at the next refresh.
        RequestContext ctx;
        if (!ValidateJwtToken(presented, s_jwtSecret, ctx, 0))
            return R"({"error":{"code":"INVALID_TOKEN","message":"Token is invalid or expired"}})";

        auto rrKey = KeyBuilder::Build("identity", "refresh_tokens", ctx.userId);
        if (!rrKey.has_value())
            return R"({"error":{"code":"INVALID_TOKEN","message":"No active session — sign in again"}})";
        std::string recordStr;
        if (!s_storage->Get(rrKey.value(), recordStr))
            return R"({"error":{"code":"INVALID_TOKEN","message":"No active session — sign in again"}})";
        json record = json::parse(recordStr);
        std::string stored = record.value("token", std::string());
        // Constant-time compare: equal sizes first, then CRYPTO_memcmp. A
        // mismatch means the presented credential was superseded (rotated or
        // a newer login) — there is no window where both are valid.
        if (stored.size() != presented.size() ||
            CRYPTO_memcmp(stored.data(), presented.data(), presented.size()) != 0)
            return R"({"error":{"code":"INVALID_TOKEN","message":"Refresh token is no longer valid"}})";

        auto ukr = KeyBuilder::Build("identity", "users", ctx.userId);
        if (!ukr.has_value()) return R"({"error":{"code":"INVALID_TOKEN","message":"Account no longer exists"}})";
        std::string userStr;
        if (!s_storage->Get(ukr.value(), userStr))
            return R"({"error":{"code":"INVALID_TOKEN","message":"Account no longer exists"}})";

        json u = json::parse(userStr);
        std::vector<std::string> permissions = FlattenRolePermissions(u);
        std::string nw = CreateJwtToken(s_jwtSecret, ctx.userId, u.value("tenant_id", ctx.tenantId), u.value("organization_id", ctx.organizationId), kTokenExpirySeconds, permissions);
        if (nw.empty()) return R"({"error":{"code":"TOKEN_ERROR","message":"Failed to create token"}})";

        // Rotation: mint the NEW refresh credential and overwrite the record —
        // the previous credential dies with the same Put.
        std::string newRefresh;
        if (auto err = IssueRefreshToken(ctx.userId, u.value("tenant_id", ctx.tenantId), u.value("organization_id", ctx.organizationId), newRefresh))
            return *err;

        StripPasswordFields(u);
        NormalizeEmbeddedRoles(u);
        json r; r["access_token"]=nw; r["refresh_token"]=newRefresh; r["token_type"]="Bearer"; r["expires_in"]=kTokenExpirySeconds; r["user"]=u; r["permissions"]=permissions;
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

/**
 * @brief      Create a user with contract-required defaults
 *
 * Injects the fields the generic CRUD handler cannot: status defaults to
 * active (required by the User response schema), display_name falls back
 * to email. Tenant/organization come from the request context.
 *
 * The password never persists or echoes: it is hashed on the way in
 * (password_hash/salt/iterations — the same triple the seeded admin gets)
 * and stripped from the response. Login resolves users through the
 * users_by_email index, so that index row is written atomically with the
 * user document — without it, and without the hash triple, a created user
 * could never authenticate. Duplicate emails are rejected for the same
 * reason: a second create would silently re-point the index and orphan the
 * first account.
 *
 * @param      ctx    Request context (tenant/organization stamping)
 * @param      body   JSON body — UserCreate (email required)
 *
 * @return     JSON user record (no password material), or error JSON
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

        auto emailKeyResult = KeyBuilder::Build("identity", "users_by_email", email);
        if (!emailKeyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Failed to build email index key"}})";
        }
        std::string existingId;
        if (s_storage->Get(emailKeyResult.value(), existingId))
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Email already registered"}})";
        }

        if (requestData.contains("password"))
        {
            if (!requestData["password"].is_string() ||
                requestData["password"].get<std::string>().empty())
            {
                return R"({"error":{"code":"INVALID_REQUEST","message":"Password must be a non-empty string"}})";
            }
            auto hr = HashPassword(requestData["password"].get<std::string>());
            if (hr.salt.empty())
            {
                return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to hash password"}})";
            }
            requestData["password_hash"]      = HexEncode(std::string(hr.hash.begin(), hr.hash.end()));
            requestData["password_salt"]      = HexEncode(std::string(hr.salt.begin(), hr.salt.end()));
            requestData["password_iterations"] = hr.iterations;
            requestData.erase("password");
        }

        std::string id = GenerateUserUuid();
        requestData["id"] = id;
        requestData["display_name"] = requestData.value("display_name", email);
        requestData["tenant_id"] = ctx.tenantId;
        requestData["organization_id"] = ctx.organizationId;
        requestData["status"] = kUserStatusActive;
        requestData["created_at"] = GetCurrentTimestamp();
        requestData["updated_at"] = requestData["created_at"];
        // role_ids (the UserCreate contract's role field) resolves to
        // embedded roles here — stored verbatim it was never read back by
        // the role normalizer, so created users showed role: null
        // (walk 2026-11-17 N1).
        ResolveRolesFromRoleIds(requestData);
        NormalizeEmbeddedRoles(requestData);

        auto keyResult = KeyBuilder::Build("identity", "users", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Failed to build storage key"}})";
        }
        if (!s_storage->WriteBatch({{keyResult.value(), requestData.dump()},
                                    {emailKeyResult.value(), id}}, {}))
        {
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to store entity"}})";
        }
        StripPasswordFields(requestData);
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
// Users List Override
//
// GET /api/v1/users was never overridden after the 7da1328 generated-CRUD
// relocation (only POST was — see the Users Create Override above), so the
// generic listUsers stub serialized raw storage records: password_hash/
// salt/iterations leaked on the wire (T-o39-01) and the legacy embedded
// roles shape ({name, permissions}) broke the regenerated Dart client's
// checked Role.fromJson contract, which is what failed the admin User List
// screen. This override mirrors the stub's scan shape and adds the contract
// fixes per record.
// ============================================================================

static constexpr unsigned int kUsersListDefaultLimit = 50;
static constexpr unsigned int kUsersListMaxLimit     = 100;

/**
 * @brief      List users with password stripping and role normalization
 *
 * Scans the identity/users prefix, backfills tenant_id/organization_id for
 * pre-stamping records (same as the generated stub), strips password
 * material and normalizes embedded roles to the Role contract, then wraps
 * in the wire envelope {"data": [...], "pagination": {limit, has_more,
 * next_cursor}}. limit comes from ?limit=N (default 50, capped at 100);
 * cursor comes from ?cursor=ID (the next_cursor of a previous page) and
 * skips records at or before it, so pages chain. next_cursor carries the
 * id of the last row on this page whenever has_more is true. Empty storage
 * is valid and yields an empty data array — never an error envelope.
 *
 * @param      ctx    Request context (raw queryString carries ?limit=N&cursor=ID)
 *
 * @return     JSON list response
 */
static std::string users_list(const RequestContext& ctx, const std::string& /*method*/, const std::string& /*urlPath*/, const std::string& /*body*/)
{
    unsigned int limit = kUsersListDefaultLimit;
    const size_t limitPos = ctx.queryString.find("limit=");
    if (limitPos != std::string::npos)
    {
        const size_t valuePos = limitPos + 6; // strlen("limit=")
        const size_t endPos   = ctx.queryString.find('&', valuePos);
        const std::string value = ctx.queryString.substr(
            valuePos, endPos == std::string::npos ? std::string::npos : endPos - valuePos);
        try
        {
            const long parsed = std::stol(value);
            if (parsed > 0)
            {
                limit = static_cast<unsigned int>(std::min<long>(parsed, kUsersListMaxLimit));
            }
        }
        catch (const std::exception&)
        {
            // Non-numeric limit — keep the default.
        }
    }

    // Opaque continuation token: the id of the last row served on the
    // previous page. Records at or before it were already delivered.
    std::string cursor;
    const size_t cursorPos = ctx.queryString.find("cursor=");
    if (cursorPos != std::string::npos)
    {
        const size_t valuePos = cursorPos + 7; // strlen("cursor=")
        const size_t endPos   = ctx.queryString.find('&', valuePos);
        cursor = ctx.queryString.substr(
            valuePos, endPos == std::string::npos ? std::string::npos : endPos - valuePos);
    }

    auto scanResult = s_storage->Scan(KeyBuilder::MakePrefix("identity", "users"));

    json result = json::array();
    for (const auto& [k, v] : scanResult)
    {
        json item;
        try
        {
            item = json::parse(v);
        }
        catch (const json::exception&)
        {
            continue; // skip malformed records rather than fail the whole list
        }
        // Backfill multi-tenant fields for records created before tenant
        // stamping was added to create handlers (same as the generated stub).
        if (!item.contains("tenant_id"))       { item["tenant_id"] = "default"; }
        if (!item.contains("organization_id")) { item["organization_id"] = "default"; }
        // Cursor pagination (scan order is key order = id order, so the
        // lexicographic comparison chains pages without gaps or repeats).
        if (!cursor.empty() && item.value("id", std::string()) <= cursor)
        {
            continue;
        }
        StripPasswordFields(item);
        // Heal records stored before the role_ids→roles agreement: their
        // role_ids/legacy role strings resolve at read time, so users
        // created before the fix still show their role (walk 2026-11-17 N1).
        ResolveRolesFromRoleIds(item);
        NormalizeEmbeddedRoles(item);
        result.push_back(item);
    }

    const bool hasMore = result.size() > static_cast<size_t>(limit);
    if (hasMore)
    {
        result.erase(result.begin() + static_cast<long>(limit), result.end());
    }

    json response;
    response["data"] = result;
    json pagination;
    pagination["limit"] = limit;
    pagination["has_more"] = hasMore;
    if (hasMore && !result.empty())
    {
        // Next page starts strictly after the last id on this page. Only
        // emitted when there is a next page — absent means "end of list".
        const std::string nextCursor = result.back().value("id", std::string());
        if (!nextCursor.empty())
        {
            pagination["next_cursor"] = nextCursor;
        }
    }
    response["pagination"] = pagination;
    return response.dump();
}

// ============================================================================
// Users Update Override
//
// PATCH /api/v1/users/{userId} fell through to the generated stub, which
// merges the raw body into the stored record: role_ids landed verbatim next
// to the embedded roles and never resolved (walk 2026-11-17 N1), and the
// stub echoed password material back on the wire. This override keeps the
// stub's merge semantics and adds the same role_ids→roles agreement as
// create, plus password stripping on the response.
// ============================================================================

/**
 * @brief      Merge-patch a user, resolving role_ids to embedded roles
 *
 * Loads the stored record, applies the body's keys on top (same semantics
 * as the generated stub — absent keys keep their stored values, so
 * password_hash survives an edit), then runs ResolveRolesFromRoleIds so a
 * non-empty role_ids array in the body replaces the embedded roles.
 *
 * @param      urlPath  Request path; the id follows the last '/'
 * @param      body     JSON merge patch (UserUpdate contract)
 *
 * @return     Updated user document, password fields stripped
 */
static std::string users_update(const RequestContext& /*ctx*/, const std::string& /*method*/, const std::string& urlPath, const std::string& body)
{
    try
    {
        const json updates = json::parse(body);
        if (!updates.is_object())
        {
            return R"({"error":{"code":"INVALID_REQUEST","message":"Request body must be a JSON object"}})";
        }
        const std::string id = urlPath.substr(urlPath.rfind('/') + 1);
        auto keyResult = KeyBuilder::Build("identity", "users", id);
        if (!keyResult.has_value())
        {
            return R"({"error":{"code":"INVALID_KEY","message":"Invalid entity ID"}})";
        }
        std::string existing;
        if (!s_storage->Get(keyResult.value(), existing))
        {
            return R"({"error":{"code":"NOT_FOUND","message":"Entity not found"}})";
        }
        json updated = json::parse(existing);
        updated.update(updates);
        updated["updated_at"] = GetCurrentTimestamp();
        ResolveRolesFromRoleIds(updated);
        NormalizeEmbeddedRoles(updated);
        if (!s_storage->Put(keyResult.value(), updated.dump()))
        {
            return R"({"error":{"code":"STORAGE_ERROR","message":"Failed to update entity"}})";
        }
        StripPasswordFields(updated);
        return updated.dump();
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

    // Seed the canonical admin role for the admin Role List screen. Nothing
    // else populates the identity/roles prefix (dev.json seeds restaurant
    // fixtures only), so GET /api/v1/roles returned {"data":[]} and the
    // screen rendered empty. Seeded independently of the empty-users check
    // (PIN-user precedent above) so existing dev databases get the role on
    // the next server start; guarded by a prefix-empty check plus a Get
    // probe so repeated starts add nothing.
    {
        auto rk = KeyBuilder::Build("identity", "roles", kAdminRoleId);
        std::string existingRole;
        if (rk.has_value() && !s_storage->Get(rk.value(), existingRole) &&
            s_storage->Scan(KeyBuilder::MakePrefix("identity", "roles")).empty())
        {
            json r; r["id"]=kAdminRoleId; r["name"]=kAdminRoleName;
            r["permissions"]=json::array({kAdminWildcardPermission});
            r["tenant_id"]="default"; r["organization_id"]="default";
            r["created_at"]="2026-01-01T00:00:00Z"; r["updated_at"]="2026-01-01T00:00:00Z";
            r["scope"]="tenant";
            if (s_storage->Put(rk.value(), r.dump()))
            {
                SPDLOG_INFO("Seeded admin role (id: {}, scope: tenant, permission: *:*)", kAdminRoleId);
            }
        }
    }

    pm->RegisterHandler("POST", "/api/v1/auth/login",    "auth_login",         auth_login,         "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/auth/logout",   "auth_logout",        auth_logout,        "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("GET",  "/api/v1/auth/me",       "auth_getCurrentUser",auth_getCurrentUser,"Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/auth/refresh",  "auth_refreshToken",  auth_refreshToken,  "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("POST", "/api/v1/users",         "users_create",       users_create,       "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("GET",  "/api/v1/users",         "users_list",         users_list,         "Identity", kOverrideHandlerPriority);
    pm->RegisterHandler("PATCH", "/api/v1/users/{userId}", "users_update",    users_update,       "Identity", kOverrideHandlerPriority);
}
