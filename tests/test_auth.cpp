/**
 * @file       test_auth.cpp
 * @brief      Unit tests for JWT token and PBKDF2 password utilities
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

#include "identity/auth_utils.hpp"

// ============================================================================
// Constants
// ============================================================================

static constexpr const char* kTestSecret  = "test-secret-key-for-jwt-signing-32b!";
static constexpr const char* kTestUserId  = "user-123";
static constexpr const char* kTestTenant  = "tenant-456";
static constexpr const char* kTestOrg     = "org-789";

// ============================================================================
// JWT Token Tests
// ============================================================================

TEST(AuthJwtTest, CreateJwtToken_ReturnsValidJwtFormat)
{
    const auto token = CreateJwtToken(kTestSecret, kTestUserId, kTestTenant, kTestOrg);

    // A valid JWT has exactly two '.' separators (header.payload.signature)
    const auto firstDot  = token.find('.');
    const auto secondDot = token.find('.', firstDot + 1);

    EXPECT_NE(std::string::npos, firstDot);
    EXPECT_NE(std::string::npos, secondDot);
    EXPECT_EQ(std::string::npos, token.find('.', secondDot + 1));
}

TEST(AuthJwtTest, ValidateJwtToken_ValidToken_RoundTrip)
{
    const auto token = CreateJwtToken(kTestSecret, kTestUserId, kTestTenant, kTestOrg);

    RequestContext ctx;
    const bool result = ValidateJwtToken(token, kTestSecret, ctx);

    EXPECT_TRUE(result);
    EXPECT_EQ(kTestUserId, ctx.userId);
    EXPECT_EQ(kTestTenant, ctx.tenantId);
    EXPECT_EQ(kTestOrg, ctx.organizationId);
    EXPECT_TRUE(ctx.authenticated);
}

TEST(AuthJwtTest, ValidateJwtToken_ExpiredToken_ReturnsFalse)
{
    // Create a token that expired 60 seconds ago
    const auto token = CreateJwtToken(kTestSecret, kTestUserId, kTestTenant, kTestOrg, -60);

    RequestContext ctx;
    const bool result = ValidateJwtToken(token, kTestSecret, ctx);

    EXPECT_FALSE(result);
    EXPECT_FALSE(ctx.authenticated);
}

TEST(AuthJwtTest, ValidateJwtToken_WrongSecret_ReturnsFalse)
{
    const auto token = CreateJwtToken("secret-A", kTestUserId, kTestTenant, kTestOrg);

    RequestContext ctx;
    const bool result = ValidateJwtToken(token, "secret-B-different", ctx);

    EXPECT_FALSE(result);
    EXPECT_FALSE(ctx.authenticated);
}

TEST(AuthJwtTest, ValidateJwtToken_TamperedToken_ReturnsFalse)
{
    const auto token = CreateJwtToken(kTestSecret, kTestUserId, kTestTenant, kTestOrg);

    // Tamper with the payload by flipping a character
    std::string tampered = token;
    const auto firstDot  = tampered.find('.');
    const auto secondDot = tampered.find('.', firstDot + 1);
    if (secondDot > firstDot + 1)
    {
        tampered[firstDot + 1] = (tampered[firstDot + 1] == 'A') ? 'B' : 'A';
    }

    RequestContext ctx;
    const bool result = ValidateJwtToken(tampered, kTestSecret, ctx);

    EXPECT_FALSE(result);
}

// ============================================================================
// Password Hashing Tests
// ============================================================================

TEST(AuthPasswordTest, HashPassword_ReturnsCorrectSizes)
{
    const auto result = HashPassword("my-secure-password");

    EXPECT_EQ(16u, result.salt.size());
    EXPECT_EQ(32u, result.hash.size());
    EXPECT_EQ(100000, result.iterations);
}

TEST(AuthPasswordTest, VerifyPassword_CorrectPassword_ReturnsTrue)
{
    const auto stored = HashPassword("correct-password");

    EXPECT_TRUE(VerifyPassword("correct-password", stored));
}

TEST(AuthPasswordTest, VerifyPassword_WrongPassword_ReturnsFalse)
{
    const auto stored = HashPassword("correct-password");

    EXPECT_FALSE(VerifyPassword("wrong-password", stored));
}

TEST(AuthPasswordTest, HashPassword_SamePasswordDifferentSalts)
{
    const auto hash1 = HashPassword("same-password");
    const auto hash2 = HashPassword("same-password");

    // Salts must differ (random)
    EXPECT_NE(hash1.salt, hash2.salt);

    // But both must verify correctly
    EXPECT_TRUE(VerifyPassword("same-password", hash1));
    EXPECT_TRUE(VerifyPassword("same-password", hash2));
}

// ============================================================================
// Unhappy-Path Password / JWT Tests
// ============================================================================

TEST(AuthPasswordTest, HashPassword_EmptyPassword_StillProducesValidSizedHash)
{
    // Hashing the empty string must still produce a 16-byte salt and 32-byte
    // derived key (PBKDF2 accepts a zero-length input). The result is a valid
    // PasswordHash, distinct from the failure sentinel returned on RAND/PBKDF2
    // errors (which has empty salt/hash).
    const auto result = HashPassword("");

    EXPECT_EQ(16u, result.salt.size());
    EXPECT_EQ(32u, result.hash.size());
    EXPECT_EQ(100000, result.iterations);

    // The empty password verifies against its own hash.
    EXPECT_TRUE(VerifyPassword("", result));

    // A non-empty password does not verify against the empty-password hash.
    EXPECT_FALSE(VerifyPassword("non-empty", result));
}

TEST(AuthPasswordTest, VerifyPassword_EmptyPassword_AgainstNonEmptyHash_ReturnsFalse)
{
    const auto stored = HashPassword("real-password");

    // Empty plaintext must not verify against a hash of a non-empty password.
    EXPECT_FALSE(VerifyPassword("", stored));
}

TEST(AuthPasswordTest, VerifyPassword_ZeroLengthSalt_ReturnsFalse)
{
    // A stored hash with a zero-length salt is rejected by the size guard
    // before PBKDF2 is invoked (kSaltLength=16).
    PasswordHash malformed;
    // salt and hash intentionally left empty; iterations left at default
    EXPECT_TRUE(malformed.salt.empty());
    EXPECT_TRUE(malformed.hash.empty());

    EXPECT_FALSE(VerifyPassword("any-password", malformed));
}

TEST(AuthPasswordTest, VerifyPassword_ZeroLengthHash_ReturnsFalse)
{
    // A stored hash with a zero-length derived key is rejected by the size
    // guard (kHashLength=32), even if the salt is the correct length.
    PasswordHash malformed;
    malformed.salt.assign(16, 0xAB);  // valid 16-byte salt
    // hash intentionally left empty
    EXPECT_TRUE(malformed.hash.empty());

    EXPECT_FALSE(VerifyPassword("any-password", malformed));
}

TEST(AuthPasswordTest, VerifyPassword_WrongSaltSize_ReturnsFalse)
{
    // A 15-byte salt (off by one) is rejected by the size guard.
    PasswordHash malformed;
    malformed.salt.assign(15, 0xAB);
    malformed.hash.assign(32, 0xCD);

    EXPECT_FALSE(VerifyPassword("any-password", malformed));
}

TEST(AuthJwtTest, ValidateJwtToken_EmptyToken_ReturnsFalse)
{
    // An empty token string cannot be decoded and must be rejected.
    RequestContext ctx;
    const bool result = ValidateJwtToken("", kTestSecret, ctx);

    EXPECT_FALSE(result);
    EXPECT_FALSE(ctx.authenticated);
}

TEST(AuthJwtTest, ValidateJwtToken_MalformedToken_ReturnsFalse)
{
    // A token that is not a valid compact JWT (wrong segment count, non-base64
    // payload, etc.) must be rejected without crashing.
    RequestContext ctx;
    const bool result = ValidateJwtToken("not.a.valid.jwt", kTestSecret, ctx);

    EXPECT_FALSE(result);
    EXPECT_FALSE(ctx.authenticated);
}

TEST(AuthJwtTest, ValidateJwtToken_EmptySecret_RejectsValidFormatToken)
{
    // A token signed with a real secret must not validate against an empty
    // secret. (This documents signature verification, not token creation.)
    const auto token = CreateJwtToken(kTestSecret, kTestUserId, kTestTenant, kTestOrg);

    RequestContext ctx;
    const bool result = ValidateJwtToken(token, "", ctx);

    EXPECT_FALSE(result);
    EXPECT_FALSE(ctx.authenticated);
}

TEST(AuthJwtTest, CreateJwtToken_EmptySecret_ProducesNonEmptyToken)
{
    // jwt-cpp's hs256 accepts any secret length (no minimum enforced by the
    // library), so even an empty secret produces a signed token. Callers must
    // enforce the 32-byte minimum themselves; this test documents that the
    // utility does not reject short/empty secrets at creation time.
    const auto token = CreateJwtToken("", kTestUserId, kTestTenant, kTestOrg);

    EXPECT_FALSE(token.empty());
}
