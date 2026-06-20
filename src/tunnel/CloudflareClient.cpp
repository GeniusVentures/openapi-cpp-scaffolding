/**
 * @file       CloudflareClient.cpp
 * @brief      REST API client for Cloudflare Tunnel operations — implementation
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "tunnel/CloudflareClient.hpp"

#include <spdlog/spdlog.h>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// Constants
// ============================================================================

/// Cloudflare API hostname
constexpr char kCloudflareApiHost[] = "api.cloudflare.com";

/// Cloudflare REST API base path
constexpr char kApiBasePath[] = "/client/v4";

/// Default HTTPS port
constexpr unsigned int kHttpsPort = 443;

/// Maximum number of SSL shutdown retries
constexpr int kSslShutdownMaxRetries = 3;

// ============================================================================
// Construction / Destruction
// ============================================================================

CloudflareClient::CloudflareClient(const std::string& apiToken) noexcept
    : m_apiToken(apiToken)
{
}

CloudflareClient::~CloudflareClient() noexcept
{
    Disconnect();
}

CloudflareClient::CloudflareClient(CloudflareClient&& other) noexcept
    : m_apiToken(std::move(other.m_apiToken))
    , m_lastError(std::move(other.m_lastError))
    , m_stream(std::move(other.m_stream))
    , m_ioc()
{
}

CloudflareClient& CloudflareClient::operator=(CloudflareClient&& other) noexcept
{
    if (this != &other)
    {
        Disconnect();
        m_apiToken = std::move(other.m_apiToken);
        m_lastError = std::move(other.m_lastError);
        m_stream = std::move(other.m_stream);
    }
    return *this;
}

// ============================================================================
// Connection Management
// ============================================================================

bool CloudflareClient::Connect() noexcept
{
    try
    {
        Disconnect();

        // Resolve api.cloudflare.com
        boost::asio::ip::tcp::resolver resolver(m_ioc);
        auto results = resolver.resolve(kCloudflareApiHost, "443");

        if (results.empty())
        {
            SPDLOG_ERROR("CloudflareClient::Connect — DNS resolution failed for: {}",
                         kCloudflareApiHost);
            m_lastError = "DNS resolution failed";
            return false;
        }

        // Create SSL context
        boost::asio::ssl::context sslCtx(boost::asio::ssl::context::tlsv12_client);
        sslCtx.set_default_verify_paths();

        // Create SSL stream
        m_stream = std::make_unique<boost::beast::ssl_stream<
            boost::beast::tcp_stream>>(m_ioc, sslCtx);

        // SNI hostname
        SSL_set_tlsext_host_name(m_stream->native_handle(), kCloudflareApiHost);

        // TCP connect
        boost::beast::get_lowest_layer(*m_stream).connect(results);

        // TLS handshake
        m_stream->handshake(boost::asio::ssl::stream_base::client);

        SPDLOG_INFO("CloudflareClient::Connect — connected to {}", kCloudflareApiHost);
        return true;
    }
    catch (const std::exception& ex)
    {
        SPDLOG_ERROR("CloudflareClient::Connect — connection failed: {}", ex.what());
        m_lastError = std::string("Connection failed: ") + ex.what();
        m_stream.reset();
        return false;
    }
}

void CloudflareClient::Disconnect() noexcept
{
    if (m_stream == nullptr)
    {
        return;
    }

    try
    {
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

bool CloudflareClient::IsConnected() const noexcept
{
    return m_stream != nullptr;
}

// ============================================================================
// SendRequest — HTTPS request/response helper
// ============================================================================

std::string CloudflareClient::SendRequest(
    const std::string& method,
    const std::string& path,
    const std::string& body) noexcept
{
    if (m_stream == nullptr)
    {
        SPDLOG_ERROR("CloudflareClient::SendRequest — not connected");
        m_lastError = "Not connected";
        return {};
    }

    try
    {
        // Determine HTTP verb
        boost::beast::http::verb verb;
        if (method == "GET")
        {
            verb = boost::beast::http::verb::get;
        }
        else if (method == "POST")
        {
            verb = boost::beast::http::verb::post;
        }
        else if (method == "PUT")
        {
            verb = boost::beast::http::verb::put;
        }
        else if (method == "DELETE")
        {
            verb = boost::beast::http::verb::delete_;
        }
        else if (method == "PATCH")
        {
            verb = boost::beast::http::verb::patch;
        }
        else
        {
            SPDLOG_ERROR("CloudflareClient::SendRequest — unsupported method: {}", method);
            m_lastError = "Unsupported HTTP method: " + method;
            return {};
        }

        // Build full path with API base
        std::string fullPath = std::string(kApiBasePath) + path;

        // Build HTTP request
        boost::beast::http::request<boost::beast::http::string_body> req;
        req.method(verb);
        req.target(fullPath);
        req.version(11);
        req.set(boost::beast::http::field::host, kCloudflareApiHost);
        req.set(boost::beast::http::field::content_type, "application/json");
        req.set(boost::beast::http::field::authorization,
                std::string("Bearer ") + m_apiToken);

        if (!body.empty())
        {
            req.body() = body;
            req.prepare_payload();
        }

        // Send request
        boost::beast::http::write(*m_stream, req);

        // Read response
        boost::beast::flat_buffer buffer;
        boost::beast::http::response<boost::beast::http::string_body> res;
        boost::beast::http::read(*m_stream, buffer, res);

        auto status = res.result();

        if (status < boost::beast::http::status::ok ||
            status >= boost::beast::http::status::multiple_choices)
        {
            SPDLOG_ERROR("CloudflareClient::SendRequest — {} {} returned {}: {}",
                         method, fullPath,
                         static_cast<unsigned>(status), res.body());
            m_lastError = "HTTP " + std::to_string(static_cast<unsigned>(status)) +
                          ": " + res.body();
            return {};
        }

        return res.body();
    }
    catch (const std::exception& ex)
    {
        SPDLOG_ERROR("CloudflareClient::SendRequest — {} {} failed: {}",
                     method, path, ex.what());
        m_lastError = std::string("Request failed: ") + ex.what();
        return {};
    }
}

// ============================================================================
// API Methods
// ============================================================================

bool CloudflareClient::VerifyToken(std::string& accountId_out) noexcept
{
    // Step 1: Verify token is valid
    std::string responseBody = SendRequest("GET", "/user/tokens/verify");
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "Token verification failed";
            SPDLOG_ERROR("CloudflareClient::VerifyToken — token invalid: {}",
                         parsed.dump());
            return false;
        }
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::VerifyToken — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }

    // Step 2: Get account ID from /accounts
    std::vector<std::pair<std::string, std::string>> accounts;
    if (!ListAccounts(accounts) || accounts.empty())
    {
        m_lastError = "No accounts found for token";
        return false;
    }

    accountId_out = accounts.front().first;
    return true;
}

bool CloudflareClient::ListAccounts(
    std::vector<std::pair<std::string, std::string>>& accounts_out) noexcept
{
    std::string responseBody = SendRequest("GET", "/accounts");
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "ListAccounts failed";
            return false;
        }

        accounts_out.clear();
        for (const auto& account : parsed["result"])
        {
            std::string id = account.value("id", "");
            std::string name = account.value("name", "");
            if (!id.empty())
            {
                accounts_out.emplace_back(id, name);
            }
        }
        return true;
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::ListAccounts — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }
}

bool CloudflareClient::CreateTunnel(
    const std::string& accountId,
    const std::string& name,
    TunnelInfo& info_out) noexcept
{
    json body;
    body["name"] = name;
    body["tunnel_secret"] = "";   // Let Cloudflare generate
    body["config_src"] = "cloudflare";

    std::string path = "/accounts/" + accountId + "/cfd_tunnel";
    std::string responseBody = SendRequest("POST", path, body.dump());
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "CreateTunnel failed";
            SPDLOG_ERROR("CloudflareClient::CreateTunnel — {}", parsed.dump());
            return false;
        }

        const auto& result = parsed["result"];
        info_out.tunnel_id = result.value("id", "");
        info_out.tunnel_name = result.value("name", "");
        info_out.status = result.value("status", "");
        info_out.created_at = result.value("created_at", "");
        return true;
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::CreateTunnel — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }
}

bool CloudflareClient::GetTunnelInfo(
    const std::string& accountId,
    const std::string& tunnelId,
    TunnelInfo& info_out) noexcept
{
    std::string path = "/accounts/" + accountId + "/cfd_tunnel/" + tunnelId;
    std::string responseBody = SendRequest("GET", path);
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "GetTunnelInfo failed";
            return false;
        }

        const auto& result = parsed["result"];
        info_out.tunnel_id = result.value("id", "");
        info_out.tunnel_name = result.value("name", "");
        info_out.status = result.value("status", "");
        info_out.created_at = result.value("created_at", "");
        return true;
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::GetTunnelInfo — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }
}

bool CloudflareClient::GetTunnelToken(
    const std::string& accountId,
    const std::string& tunnelId,
    std::string& token_out) noexcept
{
    std::string path = "/accounts/" + accountId + "/cfd_tunnel/" + tunnelId + "/token";
    std::string responseBody = SendRequest("GET", path);
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "GetTunnelToken failed";
            return false;
        }

        token_out = parsed["result"].value("token", "");
        return !token_out.empty();
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::GetTunnelToken — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }
}

bool CloudflareClient::SetTunnelConfig(
    const std::string& accountId,
    const std::string& tunnelId,
    const TunnelConfig& config) noexcept
{
    json ingressArray = json::array();
    for (const auto& rule : config.ingress)
    {
        json ingressRule;
        if (!rule.hostname.empty())
        {
            ingressRule["hostname"] = rule.hostname;
        }
        ingressRule["service"] = rule.service;
        ingressArray.push_back(ingressRule);
    }

    json body;
    body["config"]["ingress"] = ingressArray;

    std::string path = "/accounts/" + accountId + "/cfd_tunnel/" + tunnelId + "/configurations";
    std::string responseBody = SendRequest("PUT", path, body.dump());
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "SetTunnelConfig failed";
            SPDLOG_ERROR("CloudflareClient::SetTunnelConfig — {}", parsed.dump());
            return false;
        }
        return true;
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::SetTunnelConfig — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }
}

bool CloudflareClient::GetConnections(
    const std::string& accountId,
    const std::string& tunnelId,
    std::string& status_out,
    int& connectionCount_out) noexcept
{
    std::string path = "/accounts/" + accountId + "/cfd_tunnel/" + tunnelId + "/connections";
    std::string responseBody = SendRequest("GET", path);
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "GetConnections failed";
            return false;
        }

        const auto& result = parsed["result"];
        status_out = result.value("status", "unknown");
        connectionCount_out = 0;

        if (result.contains("conns") && result["conns"].is_array())
        {
            connectionCount_out = static_cast<int>(result["conns"].size());
        }
        return true;
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::GetConnections — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }
}

bool CloudflareClient::DeleteTunnel(
    const std::string& accountId,
    const std::string& tunnelId) noexcept
{
    std::string path = "/accounts/" + accountId + "/cfd_tunnel/" + tunnelId;
    std::string responseBody = SendRequest("DELETE", path);
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "DeleteTunnel failed";
            return false;
        }
        return true;
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::DeleteTunnel — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }
}

bool CloudflareClient::CreateDnsRecord(
    const std::string& zoneId,
    const std::string& name,
    const std::string& tunnelId) noexcept
{
    json body;
    body["type"] = "CNAME";
    body["name"] = name;
    body["content"] = tunnelId + ".cfargotunnel.com";
    body["proxied"] = true;

    std::string path = "/zones/" + zoneId + "/dns_records";
    std::string responseBody = SendRequest("POST", path, body.dump());
    if (responseBody.empty())
    {
        return false;
    }

    try
    {
        json parsed = json::parse(responseBody);
        if (!parsed.value("success", false))
        {
            m_lastError = "CreateDnsRecord failed";
            SPDLOG_ERROR("CloudflareClient::CreateDnsRecord — {}", parsed.dump());
            return false;
        }
        return true;
    }
    catch (const json::parse_error& ex)
    {
        SPDLOG_ERROR("CloudflareClient::CreateDnsRecord — JSON parse error: {}", ex.what());
        m_lastError = "JSON parse error";
        return false;
    }
}

const std::string& CloudflareClient::GetLastError() const noexcept
{
    return m_lastError;
}
