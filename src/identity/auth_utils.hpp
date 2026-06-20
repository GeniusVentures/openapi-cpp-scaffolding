/**
 * @file       auth_utils.hpp
 * @brief      JWT token creation/validation and PBKDF2 password hashing utilities
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#ifndef AUTH_UTILS_HPP
#define AUTH_UTILS_HPP

#include <string>
#include <vector>

#include "singleton/IPlugin.hpp"

///
/// PBKDF2 password hash result containing the derived key, salt, and iteration count.
///
struct PasswordHash
{
    std::vector<unsigned char> salt;       ///< 16-byte random salt
    std::vector<unsigned char> hash;       ///< 32-byte derived key
    int                        iterations = 100000; ///< PBKDF2 iteration count
};

///
/// Create an HS256-signed JWT token with identity claims.
/// @param secret          Signing secret (minimum 32 bytes recommended)
/// @param userId          User identifier claim
/// @param tenantId        Tenant identifier claim
/// @param orgId           Organization identifier claim
/// @param expirySeconds   Token lifetime in seconds (default 3600)
/// @return Compact JWT string in header.payload.signature format
///
std::string CreateJwtToken(
    const std::string& secret,
    const std::string& userId,
    const std::string& tenantId,
    const std::string& orgId,
    int                expirySeconds = 3600) noexcept;

///
/// Validate an HS256-signed JWT token and extract claims into a RequestContext.
/// @param token      Compact JWT string to validate
/// @param secret     Signing secret to verify against
/// @param ctx_out    Output context populated on success
/// @return true if signature valid, not expired, and all claims present; false otherwise
///
bool ValidateJwtToken(
    const std::string& token,
    const std::string& secret,
    RequestContext&     ctx_out) noexcept;

///
/// Hash a password using PBKDF2-HMAC-SHA256 with a random 16-byte salt.
/// @param password   Plaintext password to hash
/// @return PasswordHash containing 16-byte salt, 32-byte derived key, and iteration count
///
PasswordHash HashPassword(const std::string& password) noexcept;

///
/// Verify a password against a stored PBKDF2 hash using constant-time comparison.
/// @param password   Plaintext password to verify
/// @param stored     Previously computed PasswordHash to compare against
/// @return true if the derived key matches the stored hash; false otherwise
///
bool VerifyPassword(
    const std::string&    password,
    const PasswordHash&   stored) noexcept;

///
/// Validate a JWT token with extended leeway for refresh operations.
/// Allows tokens that are recently expired (within the leeway window) to be accepted.
/// Extracts claims and creates a new token with a fresh expiry.
/// @param token         Compact JWT string (possibly expired)
/// @param secret        Signing secret to verify against
/// @param leewaySeconds How far past expiry to still accept (e.g. 86400 for 24 hours)
/// @param newToken_out  Output — newly created JWT with fresh expiry
/// @param expirySeconds New token lifetime in seconds (default 3600)
/// @return true if token was valid (within leeway) and new token created; false otherwise
///
bool RefreshJwtToken(
    const std::string& token,
    const std::string& secret,
    int                leewaySeconds,
    std::string&       newToken_out,
    int                expirySeconds = 3600) noexcept;

#endif // AUTH_UTILS_HPP
