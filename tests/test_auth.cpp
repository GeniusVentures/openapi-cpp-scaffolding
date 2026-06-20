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
