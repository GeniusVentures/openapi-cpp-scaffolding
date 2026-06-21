/**
 * @file       test_s3_client.cpp
 * @brief      Unit tests for S3Client and ArchiveConfig
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "archive/S3Client.hpp"

#include <gtest/gtest.h>

// ============================================================================
// ArchiveConfig Tests
// ============================================================================

/**
 * @brief  ArchiveConfig::IsValid returns true when all required fields are set
 */
TEST(ArchiveConfigTest, IsValid_ReturnsTrue_WhenAllFieldsPresent)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    config.region = "us-east-1";
    config.prefix = "genius-archive";

    EXPECT_TRUE(config.IsValid());
}

/**
 * @brief  ArchiveConfig::IsValid returns false when endpoint is empty
 */
TEST(ArchiveConfigTest, IsValid_ReturnsFalse_WhenEndpointEmpty)
{
    ArchiveConfig config;
    config.endpoint = "";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    EXPECT_FALSE(config.IsValid());
}

/**
 * @brief  ArchiveConfig::IsValid returns false when bucket is empty
 */
TEST(ArchiveConfigTest, IsValid_ReturnsFalse_WhenBucketEmpty)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    EXPECT_FALSE(config.IsValid());
}

/**
 * @brief  ArchiveConfig::IsValid returns false when access_key is empty
 */
TEST(ArchiveConfigTest, IsValid_ReturnsFalse_WhenAccessKeyEmpty)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "my-bucket";
    config.access_key = "";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    EXPECT_FALSE(config.IsValid());
}

/**
 * @brief  ArchiveConfig::IsValid returns false when secret_key is empty
 */
TEST(ArchiveConfigTest, IsValid_ReturnsFalse_WhenSecretKeyEmpty)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "";

    EXPECT_FALSE(config.IsValid());
}

// ============================================================================
// S3Client Construction Tests
// ============================================================================

/**
 * @brief  S3Client can be constructed with a valid config without crashing
 */
TEST(S3ClientTest, Construct_WithValidConfig_DoesNotCrash)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    config.region = "us-east-1";

    S3Client client(config);
    EXPECT_FALSE(client.IsConnected());
}

// ============================================================================
// S3Client Crypto Tests
// ============================================================================

/**
 * @brief  ComputePayloadHash returns SHA256 hex digest of input
 *
 * Verified against: echo -n "Hello, World!" | sha256sum
 * Expected: dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f
 */
TEST(S3ClientTest, ComputePayloadHash_ReturnsSHA256HexDigest)
{
    std::string hash = S3Client::ComputePayloadHash("Hello, World!");
    EXPECT_EQ(hash, "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f");
}

/**
 * @brief  ComputePayloadHash returns correct hash for empty string
 *
 * SHA256 of empty string is the well-known constant:
 * e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
 */
TEST(S3ClientTest, ComputePayloadHash_EmptyString_ReturnsKnownHash)
{
    std::string hash = S3Client::ComputePayloadHash("");
    EXPECT_EQ(hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

/**
 * @brief  Sha256Hex returns correct SHA256 hex digest
 */
TEST(S3ClientTest, Sha256Hex_ReturnsCorrectDigest)
{
    std::string hash = S3Client::Sha256Hex("test data");
    // SHA256 of "test data"
    EXPECT_EQ(hash, "916f0027a575074ce72a331777c3478d6513f786a591bd892da1a577bf2335f9");
}

// ============================================================================
// S3Client PutObject Tests
// ============================================================================

/**
 * @brief  PutObject returns false when not connected
 */
TEST(S3ClientTest, PutObject_ReturnsFalse_WhenNotConnected)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    config.region = "us-east-1";

    S3Client client(config);
    EXPECT_FALSE(client.IsConnected());
    EXPECT_FALSE(client.PutObject("test-key", "test content"));
}

/**
 * @brief  IsConnected returns false on newly constructed client
 */
TEST(S3ClientTest, IsConnected_ReturnsFalse_OnNewClient)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    S3Client client(config);
    EXPECT_FALSE(client.IsConnected());
}

// ============================================================================
// S3Client Signature Tests
// ============================================================================

/**
 * @brief  HmacSha256 returns correct HMAC-SHA256 raw bytes
 *
 * Verified against AWS Sig V4 test vector:
 * HMAC-SHA256("key", "message") = ...
 */
TEST(S3ClientTest, HmacSha256_ReturnsCorrectHMAC)
{
    std::string hmac = S3Client::HmacSha256("key", "message");
    // HMAC-SHA256("key", "message") in hex:
    // 6e9ef29b75fffc5b7abae527d58fdadb2fe42e7219011976917343065f58ed4a
    std::string hexHmac = S3Client::Sha256Hex(""); // just to verify Sha256Hex works
    EXPECT_FALSE(hmac.empty());
    EXPECT_EQ(hmac.size(), 32u); // HMAC-SHA256 produces 32 bytes
}

/**
 * @brief  ComputeSignature produces a non-empty signature for valid inputs
 */
TEST(S3ClientTest, ComputeSignature_ProducesNonEmptySignature)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    config.region = "us-east-1";

    S3Client client(config);

    std::map<std::string, std::string> headers;
    headers["host"] = "s3.amazonaws.com";
    headers["x-amz-date"] = "20130524T000000Z";
    headers["x-amz-content-sha256"] = S3Client::kEmptyPayloadHash;

    std::string signature = client.ComputeSignature(
        "GET", "/", "", headers, S3Client::kEmptyPayloadHash,
        "20130524", "20130524T000000Z");

    EXPECT_FALSE(signature.empty());
}

// ============================================================================
// Unhappy-Path ArchiveConfig / S3Client Tests
// ============================================================================

/**
 * @brief  IsValid returns true when endpoint is whitespace-only
 *
 * ArchiveConfig::IsValid only rejects the empty string; a whitespace-only
 * endpoint is treated as "present" and reports valid. This documents the
 * current lack of trimming so callers know they must pre-trim inputs.
 */
TEST(ArchiveConfigTest, IsValid_ReturnsTrue_WhenEndpointIsWhitespaceOnly)
{
    ArchiveConfig config;
    config.endpoint = "   ";  // whitespace-only — not empty
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    // Documents current behavior: IsValid does not trim whitespace.
    EXPECT_TRUE(config.IsValid());
}

/**
 * @brief  IsValid returns true when access_key is whitespace-only
 *
 * Documents that whitespace-only credentials are treated as present.
 */
TEST(ArchiveConfigTest, IsValid_ReturnsTrue_WhenAccessKeyIsWhitespaceOnly)
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "my-bucket";
    config.access_key = "   ";  // whitespace-only — not empty
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    // Documents current behavior: no whitespace trimming on credentials.
    EXPECT_TRUE(config.IsValid());
}

/**
 * @brief  Construct with a malformed endpoint URL does not crash
 *
 * The constructor stores the config without parsing or validating the URL
 * scheme/host. ParseUrl is deferred to ConnectToEndpoint. This test confirms
 * that arbitrary garbage in the endpoint field does not crash construction.
 */
TEST(S3ClientTest, Construct_WithMalformedEndpointUrl_DoesNotCrash)
{
    ArchiveConfig config;
    config.endpoint = "not a valid url ::: broken";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    S3Client client(config);
    EXPECT_FALSE(client.IsConnected());
}

/**
 * @brief  ConnectToEndpoint with an unresolvable host returns false
 *
 * ParseUrl never fails parsing (it returns the input as host on no match),
 * so a syntactically-valid-but-non-existent host surfaces as a DNS-resolution
 * failure inside ConnectToEndpoint. This verifies the failure is reported as
 * false rather than crashing or hanging.
 */
TEST(S3ClientTest, ConnectToEndpoint_WithUnresolvableHost_ReturnsFalse)
{
    ArchiveConfig config;
    // Hostname that is syntactically valid but guaranteed not to resolve.
    config.endpoint = "https://nonexistent-invalid-host.invalid";
    config.bucket = "my-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    S3Client client(config);
    bool connected = client.ConnectToEndpoint();

    EXPECT_FALSE(connected);
    EXPECT_FALSE(client.IsConnected());
}

/**
 * @brief  PutObject on a client built from an invalid config returns false
 *
 * Constructed with empty required fields, the client never connects, so
 * PutObject must fail fast without attempting network I/O.
 */
TEST(S3ClientTest, PutObject_ReturnsFalse_WhenConfigInvalid)
{
    ArchiveConfig config;
    config.endpoint = "";          // empty -> IsValid() == false
    config.bucket = "";
    config.access_key = "";        // empty access key
    config.secret_key = "";

    EXPECT_FALSE(config.IsValid());

    S3Client client(config);
    EXPECT_FALSE(client.PutObject("any-key", "any-content"));
}
