/**
 * @file       S3Client.hpp
 * @brief      S3-compatible object storage client with AWS Signature V4 signing
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */
#ifndef S3CLIENT_HPP
#define S3CLIENT_HPP

#include <map>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

///
/// Configuration for an S3-compatible archive target.
/// All fields except region and prefix are required for a valid configuration.
///
struct ArchiveConfig
{
    std::string endpoint;       ///< S3 endpoint URL (e.g. "https://s3.amazonaws.com")
    std::string bucket;         ///< S3 bucket name
    std::string access_key;     ///< AWS access key ID
    std::string secret_key;     ///< AWS secret access key
    std::string region = "us-east-1";   ///< AWS region (default: us-east-1)
    std::string prefix = "genius-archive";  ///< Key prefix for archived files

    ///
    /// Validate that all required configuration fields are non-empty.
    /// @return true if endpoint, bucket, access_key, and secret_key are all non-empty
    ///
    bool IsValid() const noexcept;
};

///
/// S3-compatible object storage client.
/// Provides HTTPS PUT operations with AWS Signature V4 request signing.
/// Uses Boost.Beast for HTTP/TLS and OpenSSL for cryptographic operations.
///
/// Non-copyable, movable.
///
class S3Client
{
public:
    ///
    /// AWS Signature V4 algorithm identifier
    ///
    static constexpr char kAwsAlgorithm[] = "AWS4-HMAC-SHA256";

    ///
    /// AWS service identifier for S3
    ///
    static constexpr char kAwsService[] = "s3";

    ///
    /// SHA256 hex digest of an empty string (used for unsigned payloads)
    ///
    static constexpr char kEmptyPayloadHash[] =
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

    ///
    /// Construct an S3Client with the given configuration.
    /// Does not establish a connection — call ConnectToEndpoint() before PutObject().
    /// @param  config  S3-compatible endpoint configuration
    ///
    explicit S3Client(const ArchiveConfig& config) noexcept;

    ///
    /// Destructor. Disconnects if still connected.
    ///
    ~S3Client() noexcept;

    // Non-copyable
    S3Client(const S3Client&) = delete;
    S3Client& operator=(const S3Client&) = delete;

    // Movable
    S3Client(S3Client&& other) noexcept;
    S3Client& operator=(S3Client&& other) noexcept;

    ///
    /// Establish a TLS connection to the S3 endpoint.
    /// Parses the endpoint URL, resolves the host, performs TCP connect,
    /// TLS handshake, and SNI hostname setup.
    /// @return true on success, false on error
    ///
    bool ConnectToEndpoint() noexcept;

    ///
    /// Upload an object to S3 via HTTP PUT with AWS Signature V4.
    /// Must be connected (ConnectToEndpoint() returned true) before calling.
    /// @param  s3Key     The S3 object key (path within the bucket)
    /// @param  content   The object content (body of the PUT request)
    /// @return true on 2xx response, false on error
    ///
    bool PutObject(const std::string& s3Key, const std::string& content) noexcept;

    ///
    /// Gracefully shut down and close the TLS connection.
    ///
    void Disconnect() noexcept;

    ///
    /// Check if the client has an active connection.
    /// @return true if the TLS stream is open
    ///
    bool IsConnected() const noexcept;

    // ========================================================================
    // Cryptographic Utilities (public for testing)
    // ========================================================================

    ///
    /// Compute SHA256 hex digest of the input data.
    /// @param  data  Input data to hash
    /// @return Lowercase hex-encoded SHA256 digest (64 characters)
    ///
    static std::string Sha256Hex(const std::string& data) noexcept;

    ///
    /// Compute HMAC-SHA256 of data using the given key.
    /// @param  key   HMAC key
    /// @param  data  Data to authenticate
    /// @return Raw HMAC-SHA256 bytes (32 bytes)
    ///
    static std::string HmacSha256(const std::string& key, const std::string& data) noexcept;

    ///
    /// Compute SHA256 hex digest of a payload (alias for Sha256Hex).
    /// @param  payload  Input data to hash
    /// @return Lowercase hex-encoded SHA256 digest
    ///
    static std::string ComputePayloadHash(const std::string& payload) noexcept;

    ///
    /// Compute an AWS Signature V4 signature for the given request parameters.
    /// @param  method       HTTP method (e.g. "PUT")
    /// @param  uri          Request URI path (e.g. "/bucket/key")
    /// @param  queryString  URL query string (empty if none)
    /// @param  headers      Map of header name (lowercase) to header value
    /// @param  payloadHash  SHA256 hex digest of the request body
    /// @param  dateStamp    Date in YYYYMMDD format
    /// @param  amzDate      Full date-time in YYYYMMDDTHHMMSSZ format
    /// @return Hex-encoded signature string
    ///
    std::string ComputeSignature(
        const std::string& method,
        const std::string& uri,
        const std::string& queryString,
        const std::map<std::string, std::string>& headers,
        const std::string& payloadHash,
        const std::string& dateStamp,
        const std::string& amzDate) const noexcept;

private:
    ArchiveConfig m_config;
    std::unique_ptr<boost::beast::ssl_stream<boost::beast::tcp_stream>> m_stream;
    boost::asio::io_context m_ioc;
};

#endif // S3CLIENT_HPP
