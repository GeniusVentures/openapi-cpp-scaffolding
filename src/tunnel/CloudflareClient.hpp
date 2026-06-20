/**
 * @file       CloudflareClient.hpp
 * @brief      REST API client for Cloudflare Tunnel operations via api.cloudflare.com
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */
#ifndef CLOUDFLARECLIENT_HPP
#define CLOUDFLARECLIENT_HPP

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

///
/// Information about a Cloudflare tunnel returned by the REST API.
///
struct TunnelInfo
{
    std::string tunnel_id;       ///< Tunnel UUID
    std::string tunnel_name;     ///< Human-readable tunnel name
    std::string status;          ///< Tunnel status: inactive, degraded, healthy, down
    std::string tunnel_token;    ///< Token for cloudflared authentication
    std::string created_at;      ///< ISO 8601 creation timestamp
};

///
/// A single ingress rule for a Cloudflare tunnel.
///
struct IngressRule
{
    std::string hostname;        ///< Hostname to match (empty for catch-all)
    std::string service;         ///< Service URL (e.g. "http://localhost:3000" or "http_status:404")
};

///
/// Tunnel configuration containing ingress rules.
///
struct TunnelConfig
{
    std::vector<IngressRule> ingress;   ///< Ordered ingress rules (last must be catch-all)
};

///
/// REST API client for Cloudflare Tunnel management.
///
/// Communicates with api.cloudflare.com via HTTPS (Boost.Beast + OpenSSL).
/// Supports tunnel CRUD, token retrieval, ingress configuration,
/// connection status checks, and DNS record creation.
///
/// Connection lifecycle: Connect() -> API calls -> Disconnect().
///
/// Non-copyable, movable.
///
class CloudflareClient
{
public:
    ///
    /// Construct a CloudflareClient with the given API token.
    /// Does not establish a connection — call Connect() before API methods.
    /// @param  apiToken  Cloudflare API bearer token (requires Zero Trust: Tunnel scope)
    ///
    explicit CloudflareClient(const std::string& apiToken) noexcept;

    ///
    /// Destructor. Disconnects if still connected.
    ///
    ~CloudflareClient() noexcept;

    // Non-copyable
    CloudflareClient(const CloudflareClient&) = delete;
    CloudflareClient& operator=(const CloudflareClient&) = delete;

    // Movable
    CloudflareClient(CloudflareClient&& other) noexcept;
    CloudflareClient& operator=(CloudflareClient&& other) noexcept;

    ///
    /// Establish an HTTPS connection to api.cloudflare.com (port 443).
    /// @return true on success, false on error
    ///
    bool Connect() noexcept;

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
    // Cloudflare REST API Methods
    // ========================================================================

    ///
    /// Verify the API token and retrieve the account ID.
    /// @param  accountId_out  Output: first account ID from the accounts list
    /// @return true if token is valid and account was retrieved
    ///
    bool VerifyToken(std::string& accountId_out) noexcept;

    ///
    /// List all accounts accessible with the current API token.
    /// @param  accounts_out  Output: vector of (account_id, account_name) pairs
    /// @return true on success
    ///
    bool ListAccounts(std::vector<std::pair<std::string, std::string>>& accounts_out) noexcept;

    ///
    /// Create a new Cloudflare tunnel.
    /// @param  accountId  Cloudflare account ID
    /// @param  name       Tunnel name (e.g. "genius-server")
    /// @param  info_out   Output: populated TunnelInfo for the created tunnel
    /// @return true on success
    ///
    bool CreateTunnel(const std::string& accountId,
                      const std::string& name,
                      TunnelInfo& info_out) noexcept;

    ///
    /// Retrieve tunnel details by ID.
    /// @param  accountId  Cloudflare account ID
    /// @param  tunnelId   Tunnel UUID
    /// @param  info_out   Output: populated TunnelInfo
    /// @return true on success
    ///
    bool GetTunnelInfo(const std::string& accountId,
                       const std::string& tunnelId,
                       TunnelInfo& info_out) noexcept;

    ///
    /// Retrieve the token used by cloudflared to authenticate.
    /// @param  accountId   Cloudflare account ID
    /// @param  tunnelId    Tunnel UUID
    /// @param  token_out   Output: tunnel token string
    /// @return true on success
    ///
    bool GetTunnelToken(const std::string& accountId,
                        const std::string& tunnelId,
                        std::string& token_out) noexcept;

    ///
    /// Set the ingress configuration for a tunnel.
    /// @param  accountId  Cloudflare account ID
    /// @param  tunnelId   Tunnel UUID
    /// @param  config     Tunnel configuration with ingress rules
    /// @return true on success
    ///
    bool SetTunnelConfig(const std::string& accountId,
                         const std::string& tunnelId,
                         const TunnelConfig& config) noexcept;

    ///
    /// Check tunnel connection status and active connection count.
    /// @param  accountId          Cloudflare account ID
    /// @param  tunnelId           Tunnel UUID
    /// @param  status_out         Output: tunnel status string (healthy, degraded, etc.)
    /// @param  connectionCount_out  Output: number of active connections
    /// @return true on success
    ///
    bool GetConnections(const std::string& accountId,
                        const std::string& tunnelId,
                        std::string& status_out,
                        int& connectionCount_out) noexcept;

    ///
    /// Delete a tunnel.
    /// @param  accountId  Cloudflare account ID
    /// @param  tunnelId   Tunnel UUID
    /// @return true on success
    ///
    bool DeleteTunnel(const std::string& accountId,
                      const std::string& tunnelId) noexcept;

    ///
    /// Create a DNS CNAME record for a tunnel hostname.
    /// @param  zoneId    Cloudflare zone ID
    /// @param  name      DNS record name (e.g. "api")
    /// @param  tunnelId  Tunnel UUID (CNAME target: {tunnel_id}.cfargotunnel.com)
    /// @return true on success
    ///
    bool CreateDnsRecord(const std::string& zoneId,
                         const std::string& name,
                         const std::string& tunnelId) noexcept;

    ///
    /// Get the last error message from a failed API call.
    /// @return Human-readable error description
    ///
    const std::string& GetLastError() const noexcept;

private:
    ///
    /// Send an HTTPS request to the Cloudflare API and return the response body.
    /// Opens a new TLS connection per request (same pattern as S3Client).
    /// @param  method  HTTP method (GET, POST, PUT, DELETE)
    /// @param  path    API path (e.g. "/client/v4/accounts")
    /// @param  body    Request body (empty for GET/DELETE)
    /// @return Response body string, or empty string on error
    ///
    std::string SendRequest(const std::string& method,
                            const std::string& path,
                            const std::string& body = "") noexcept;

    std::string m_apiToken;                              ///< Bearer token
    std::string m_lastError;                             ///< Last error message
    std::unique_ptr<boost::beast::ssl_stream<
        boost::beast::tcp_stream>> m_stream;             ///< TLS stream
    boost::asio::io_context m_ioc;                       ///< I/O context
};

#endif // CLOUDFLARECLIENT_HPP
