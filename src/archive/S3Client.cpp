/**
 * @file       S3Client.cpp
 * @brief      S3-compatible object storage client implementation
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "archive/S3Client.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <iomanip>
#include <sstream>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <openssl/evp.h>
#include <openssl/hmac.h>

// ============================================================================
// Constants
// ============================================================================

constexpr char kHttpsScheme[] = "https://";
constexpr char kHttpScheme[] = "http://";
constexpr unsigned int kDefaultHttpsPort = 443;
constexpr unsigned int kDefaultHttpPort = 80;
constexpr int kSslShutdownMaxRetries = 3;

// ============================================================================
// ArchiveConfig
// ============================================================================

bool ArchiveConfig::IsValid() const noexcept
{
    return !endpoint.empty() &&
           !bucket.empty() &&
           !access_key.empty() &&
           !secret_key.empty();
}

// ============================================================================
// S3Client Construction / Destruction
// ============================================================================

S3Client::S3Client(const ArchiveConfig& config) noexcept
    : m_config(config)
{
}

S3Client::~S3Client() noexcept
{
    Disconnect();
}

S3Client::S3Client(S3Client&& other) noexcept
    : m_config(std::move(other.m_config))
    , m_stream(std::move(other.m_stream))
    , m_ioc()
{
}

S3Client& S3Client::operator=(S3Client&& other) noexcept
{
    if (this != &other)
    {
        Disconnect();
        m_config = std::move(other.m_config);
        m_stream = std::move(other.m_stream);
    }
    return *this;
}

// ============================================================================
// URL Parsing Helper
// ============================================================================

///
/// Parse a URL string to extract host, port, and whether it uses TLS.
/// Supports "https://host:port" and "http://host:port" formats.
/// @param  url       The URL string to parse
/// @param  host_out  Output: the hostname
/// @param  port_out  Output: the port number
/// @return true if TLS (https), false if plain HTTP
///
static bool ParseUrl(
    const std::string& url,
    std::string& host_out,
    unsigned int& port_out) noexcept
{
    bool useTls = false;
    std::string remaining;

    if (url.rfind(kHttpsScheme, 0) == 0)
    {
        useTls = true;
        remaining = url.substr(std::strlen(kHttpsScheme));
        port_out = kDefaultHttpsPort;
    }
    else if (url.rfind(kHttpScheme, 0) == 0)
    {
        useTls = false;
        remaining = url.substr(std::strlen(kHttpScheme));
        port_out = kDefaultHttpPort;
    }
    else
    {
        // Default to HTTPS
        useTls = true;
        remaining = url;
        port_out = kDefaultHttpsPort;
    }

    // Strip trailing path
    auto pathPos = remaining.find('/');
    if (pathPos != std::string::npos)
    {
        remaining = remaining.substr(0, pathPos);
    }

    // Check for port
    auto colonPos = remaining.find(':');
    if (colonPos != std::string::npos)
    {
        host_out = remaining.substr(0, colonPos);
        std::string portStr = remaining.substr(colonPos + 1);
        try
        {
            port_out = static_cast<unsigned int>(std::stoul(portStr));
        }
        catch (...)
        {
            // Keep default port on parse failure
        }
    }
    else
    {
        host_out = remaining;
    }

    return useTls;
}

// ============================================================================
// Cryptographic Utilities
// ============================================================================

std::string S3Client::Sha256Hex(const std::string& data) noexcept
{
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr)
    {
        SPDLOG_ERROR("S3Client::Sha256Hex — EVP_MD_CTX_new failed");
        return {};
    }

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1;
    if (ok)
    {
        ok = EVP_DigestUpdate(ctx, data.data(), data.size()) == 1;
    }
    if (ok)
    {
        ok = EVP_DigestFinal_ex(ctx, hash, &hashLen) == 1;
    }

    EVP_MD_CTX_free(ctx);

    if (!ok || hashLen == 0)
    {
        SPDLOG_ERROR("S3Client::Sha256Hex — digest computation failed");
        return {};
    }

    static constexpr char kHexChars[] = "0123456789abcdef";
    std::string result;
    result.reserve(hashLen * 2);
    for (unsigned int i = 0; i < hashLen; ++i)
    {
        result += kHexChars[(hash[i] >> 4) & 0x0F];
        result += kHexChars[hash[i] & 0x0F];
    }
    return result;
}

std::string S3Client::HmacSha256(const std::string& key, const std::string& data) noexcept
{
    unsigned char hmac[EVP_MAX_MD_SIZE];
    unsigned int hmacLen = 0;

    unsigned char* result = HMAC(
        EVP_sha256(),
        key.data(), static_cast<int>(key.size()),
        reinterpret_cast<const unsigned char*>(data.data()),
        static_cast<int>(data.size()),
        hmac, &hmacLen);

    if (result == nullptr || hmacLen == 0)
    {
        SPDLOG_ERROR("S3Client::HmacSha256 — HMAC computation failed");
        return {};
    }

    return std::string(reinterpret_cast<const char*>(hmac), hmacLen);
}

std::string S3Client::ComputePayloadHash(const std::string& payload) noexcept
{
    return Sha256Hex(payload);
}

// ============================================================================
// AWS Signature V4
// ============================================================================

std::string S3Client::ComputeSignature(
    const std::string& method,
    const std::string& uri,
    const std::string& queryString,
    const std::map<std::string, std::string>& headers,
    const std::string& payloadHash,
    const std::string& dateStamp,
    const std::string& amzDate) const noexcept
{
    // 1. Build canonical headers and signed headers list
    std::string canonicalHeaders;
    std::string signedHeaders;

    for (const auto& [name, value] : headers)
    {
        canonicalHeaders += name;
        canonicalHeaders += ":";
        canonicalHeaders += value;
        canonicalHeaders += "\n";

        if (!signedHeaders.empty())
        {
            signedHeaders += ";";
        }
        signedHeaders += name;
    }

    // 2. Build canonical request
    std::string canonicalRequest = method + "\n" +
                                   uri + "\n" +
                                   queryString + "\n" +
                                   canonicalHeaders + "\n" +
                                   signedHeaders + "\n" +
                                   payloadHash;

    std::string hashedCanonicalRequest = Sha256Hex(canonicalRequest);

    // 3. Build credential scope
    std::string credentialScope = dateStamp + "/" +
                                  m_config.region + "/" +
                                  kAwsService + "/aws4_request";

    // 4. Build string to sign
    std::string stringToSign = std::string(kAwsAlgorithm) + "\n" +
                               amzDate + "\n" +
                               credentialScope + "\n" +
                               hashedCanonicalRequest;

    // 5. Derive signing key via HMAC chain
    std::string kDate = HmacSha256("AWS4" + m_config.secret_key, dateStamp);
    std::string kRegion = HmacSha256(kDate, m_config.region);
    std::string kService = HmacSha256(kRegion, kAwsService);
    std::string kSigning = HmacSha256(kService, "aws4_request");

    // 6. Compute final signature
    std::string signature = HmacSha256(kSigning, stringToSign);

    // 7. Convert to hex
    static constexpr char kHexChars[] = "0123456789abcdef";
    std::string hexSignature;
    hexSignature.reserve(signature.size() * 2);
    for (unsigned char byte : signature)
    {
        hexSignature += kHexChars[(byte >> 4) & 0x0F];
        hexSignature += kHexChars[byte & 0x0F];
    }

    return hexSignature;
}

// ============================================================================
// Connection Management
// ============================================================================

bool S3Client::ConnectToEndpoint() noexcept
{
    try
    {
        Disconnect();

        std::string host;
        unsigned int port = kDefaultHttpsPort;
        bool useTls = ParseUrl(m_config.endpoint, host, port);

        if (host.empty())
        {
            SPDLOG_ERROR("S3Client::ConnectToEndpoint — empty host in endpoint URL: {}",
                         m_config.endpoint);
            return false;
        }

        // Resolve host
        boost::asio::ip::tcp::resolver resolver(m_ioc);
        auto results = resolver.resolve(host, std::to_string(port));

        if (results.empty())
        {
            SPDLOG_ERROR("S3Client::ConnectToEndpoint — DNS resolution failed for: {}",
                         host);
            return false;
        }

        if (useTls)
        {
            // Create SSL context
            boost::asio::ssl::context sslCtx(boost::asio::ssl::context::tlsv12_client);
            sslCtx.set_default_verify_paths();

            // Create SSL stream
            m_stream = std::make_unique<boost::beast::ssl_stream<boost::beast::tcp_stream>>(
                m_ioc, sslCtx);

            // SNI hostname
            SSL_set_tlsext_host_name(m_stream->native_handle(), host.c_str());

            // TCP connect
            boost::beast::get_lowest_layer(*m_stream).connect(results);

            // TLS handshake
            m_stream->handshake(boost::asio::ssl::stream_base::client);
        }
        else
        {
            SPDLOG_WARN("S3Client::ConnectToEndpoint — non-TLS connection to {}:{} "
                        "(not recommended for production)", host, port);
            // For non-TLS, we'd need a different stream type.
            // For now, TLS is required for S3.
            return false;
        }

        SPDLOG_INFO("S3Client::ConnectToEndpoint — connected to {}:{}", host, port);
        return true;
    }
    catch (const std::exception& ex)
    {
        SPDLOG_ERROR("S3Client::ConnectToEndpoint — connection failed: {}", ex.what());
        m_stream.reset();
        return false;
    }
}

bool S3Client::PutObject(const std::string& s3Key, const std::string& content) noexcept
{
    if (!IsConnected())
    {
        SPDLOG_ERROR("S3Client::PutObject — not connected");
        return false;
    }

    try
    {
        // Get current UTC date
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        std::tm utcTime{};
#if defined(_WIN32)
        gmtime_s(&utcTime, &time);
#else
        gmtime_r(&time, &utcTime);
#endif

        char dateStampBuf[9];
        char amzDateBuf[17];
        std::strftime(dateStampBuf, sizeof(dateStampBuf), "%Y%m%d", &utcTime);
        std::strftime(amzDateBuf, sizeof(amzDateBuf), "%Y%m%dT%H%M%SZ", &utcTime);

        std::string dateStamp(dateStampBuf);
        std::string amzDate(amzDateBuf);

        // Build target URI
        std::string uri = "/" + m_config.bucket + "/" + s3Key;

        // Compute payload hash
        std::string payloadHash = ComputePayloadHash(content);

        // Parse endpoint to get host for the Host header
        std::string host;
        unsigned int port = kDefaultHttpsPort;
        ParseUrl(m_config.endpoint, host, port);

        // Build headers map
        std::map<std::string, std::string> headers;
        headers["host"] = host;
        headers["x-amz-date"] = amzDate;
        headers["x-amz-content-sha256"] = payloadHash;
        headers["content-type"] = "application/x-ndjson";

        // Compute signature
        std::string signature = ComputeSignature(
            "PUT", uri, "", headers, payloadHash, dateStamp, amzDate);

        // Build authorization header
        std::string credentialScope = dateStamp + "/" +
                                      m_config.region + "/" +
                                      kAwsService + "/aws4_request";
        std::string authorization = std::string(kAwsAlgorithm) + " " +
                                    "Credential=" + m_config.access_key + "/" + credentialScope + ", " +
                                    "SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date, " +
                                    "Signature=" + signature;

        // Build HTTP request
        boost::beast::http::request<boost::beast::http::string_body> req;
        req.method(boost::beast::http::verb::put);
        req.target(uri);
        req.version(11);
        req.set(boost::beast::http::field::host, host);
        req.set(boost::beast::http::field::content_type, "application/x-ndjson");
        req.set(boost::beast::http::field::content_length,
                std::to_string(content.size()));
        req.set("Authorization", authorization);
        req.set("x-amz-date", amzDate);
        req.set("x-amz-content-sha256", payloadHash);
        req.body() = content;
        req.prepare_payload();

        // Send request
        boost::beast::http::write(*m_stream, req);

        // Read response
        boost::beast::flat_buffer buffer;
        boost::beast::http::response<boost::beast::http::string_body> res;
        boost::beast::http::read(*m_stream, buffer, res);

        auto status = res.result();
        bool success = (status >= boost::beast::http::status::ok &&
                        status < boost::beast::http::status::multiple_choices);

        if (!success)
        {
            SPDLOG_ERROR("S3Client::PutObject — S3 returned {}: {}",
                         static_cast<unsigned>(status), res.body());
        }

        return success;
    }
    catch (const std::exception& ex)
    {
        SPDLOG_ERROR("S3Client::PutObject — request failed: {}", ex.what());
        return false;
    }
}

void S3Client::Disconnect() noexcept
{
    if (m_stream == nullptr)
    {
        return;
    }

    try
    {
        // Attempt graceful SSL shutdown (ignore errors — the connection may already be closed)
        for (int i = 0; i < kSslShutdownMaxRetries; ++i)
        {
            try
            {
                m_stream->shutdown();
                break;
            }
            catch (...)
            {
                // SSL shutdown can throw if the peer already disconnected
            }
        }
    }
    catch (...)
    {
        // Ignore all shutdown errors
    }

    m_stream.reset();
}

bool S3Client::IsConnected() const noexcept
{
    return m_stream != nullptr;
}
