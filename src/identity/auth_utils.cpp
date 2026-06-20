/**
 * @file       auth_utils.cpp
 * @brief      JWT token creation/validation and PBKDF2 password hashing implementations
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */

#include "identity/auth_utils.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/rand.h>

// Suppress picojson tautological comparison warnings in jwt-cpp headers
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wtautological-type-limit-compare"
#include <jwt-cpp/jwt.h>
#pragma clang diagnostic pop

// ============================================================================
// Constants
// ============================================================================

static constexpr int    kSaltLength       = 16;
static constexpr int    kHashLength       = 32;
static constexpr int    kDefaultIterations = 100000;

// ============================================================================
// JWT Utilities
// ============================================================================

std::string CreateJwtToken(
    const std::string& secret,
    const std::string& userId,
    const std::string& tenantId,
    const std::string& orgId,
    int                expirySeconds) noexcept
{
    try
    {
        const auto now = std::chrono::system_clock::now();
        const auto exp = now + std::chrono::seconds{expirySeconds};

        auto token = jwt::create()
                         .set_type("JWS")
                         .set_issued_at(now)
                         .set_expires_at(exp)
                         .set_payload_claim("user_id", jwt::claim(userId))
                         .set_payload_claim("tenant_id", jwt::claim(tenantId))
                         .set_payload_claim("org_id", jwt::claim(orgId))
                         .sign(jwt::algorithm::hs256{secret});

        return token;
    }
    catch (...)
    {
        return {};
    }
}

bool ValidateJwtToken(
    const std::string& token,
    const std::string& secret,
    RequestContext&     ctx_out) noexcept
{
    try
    {
        auto decoded = jwt::decode(token);

        jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{secret})
            .verify(decoded);

        ctx_out.userId         = decoded.get_payload_claim("user_id").as_string();
        ctx_out.tenantId       = decoded.get_payload_claim("tenant_id").as_string();
        ctx_out.organizationId = decoded.get_payload_claim("org_id").as_string();
        ctx_out.authenticated  = true;

        return true;
    }
    catch (...)
    {
        return false;
    }
}

// ============================================================================
// Password Hashing Utilities
// ============================================================================

PasswordHash HashPassword(const std::string& password) noexcept
{
    PasswordHash result;
    result.salt.resize(kSaltLength);
    result.hash.resize(kHashLength);
    result.iterations = kDefaultIterations;

    // Generate random salt
    if (RAND_bytes(result.salt.data(), kSaltLength) != 1)
    {
        return PasswordHash{};
    }

    // Derive key via PBKDF2-HMAC-SHA256
    if (PKCS5_PBKDF2_HMAC(
            password.c_str(),
            static_cast<int>(password.size()),
            result.salt.data(),
            kSaltLength,
            kDefaultIterations,
            EVP_sha256(),
            kHashLength,
            result.hash.data()) != 1)
    {
        return PasswordHash{};
    }

    return result;
}

bool VerifyPassword(
    const std::string&  password,
    const PasswordHash& stored) noexcept
{
    if (stored.salt.size() != static_cast<size_t>(kSaltLength) ||
        stored.hash.size() != static_cast<size_t>(kHashLength))
    {
        return false;
    }

    std::vector<unsigned char> derived(kHashLength);

    if (PKCS5_PBKDF2_HMAC(
            password.c_str(),
            static_cast<int>(password.size()),
            stored.salt.data(),
            static_cast<int>(stored.salt.size()),
            stored.iterations,
            EVP_sha256(),
            kHashLength,
            derived.data()) != 1)
    {
        return false;
    }

    // Constant-time comparison to prevent timing attacks
    return std::equal(derived.begin(), derived.end(), stored.hash.begin());
}

bool RefreshJwtToken(
    const std::string& token,
    const std::string& secret,
    int                leewaySeconds,
    std::string&       newToken_out,
    int                expirySeconds) noexcept
{
    try
    {
        auto decoded = jwt::decode(token);

        jwt::verify()
            .allow_algorithm(jwt::algorithm::hs256{secret})
            .leeway(static_cast<size_t>(leewaySeconds))
            .verify(decoded);

        std::string userId   = decoded.get_payload_claim("user_id").as_string();
        std::string tenantId = decoded.get_payload_claim("tenant_id").as_string();
        std::string orgId    = decoded.get_payload_claim("org_id").as_string();

        newToken_out = CreateJwtToken(secret, userId, tenantId, orgId, expirySeconds);
        return !newToken_out.empty();
    }
    catch (...)
    {
        return false;
    }
}
