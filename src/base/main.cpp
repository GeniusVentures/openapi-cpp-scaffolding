/**
 * @file       main.cpp
 * @brief      Server entry point — HTTP server with plugin loading and lifecycle
 * @date       2026-05-25
 * @author     Kenneth L. Hurley
 */

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "singleton/PluginManager.hpp"
#include "singleton/CServiceLocator.hpp"
#include "singleton/fnv1a.hpp"
#include "storage/RocksDBEngine.hpp"
#include "storage/KeyBuilder.hpp"
#include "config/GsmConfig.hpp"
#include "identity/auth_utils.hpp"
#include "logging.hpp"
#include "storage/GsmStorageManager.hpp"

namespace net       = boost::asio;
namespace beast     = boost::beast;
namespace http      = beast::http;
using tcp           = net::ip::tcp;

namespace
{

/// PluginManager pointer — set in main(), used by HttpSession for routing.
PluginManager* g_pluginManager = nullptr;

/// Executable directory — all assets live next to the binary (cmake copies them there).
std::string g_exeDir;

/// JWT signing secret — loaded from environment or config file.
std::string g_jwtSecret;

///
/// Get the directory containing the executable.
///
std::string GetExecutableDir()
{
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
    {
        std::string path(buf);
        auto pos = path.rfind('/');
        if (pos != std::string::npos)
        {
            return path.substr(0, pos);
        }
    }
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1)
    {
        buf[len] = '\0';
        std::string path(buf);
        auto pos = path.rfind('/');
        if (pos != std::string::npos)
        {
            return path.substr(0, pos);
        }
    }
#elif defined(_WIN32)
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0)
    {
        std::string path(buf, len);
        auto pos = path.rfind('\\');
        if (pos != std::string::npos)
        {
            return path.substr(0, pos);
        }
    }
#endif
    return ".";
}

///
/// Check if a file or directory exists.
///
bool PathExists(const std::string& path)
{
    struct stat s;
    return stat(path.c_str(), &s) == 0;
}

///
/// Read a file into a string. Returns empty string on failure.
///
std::string ReadFile(const std::string& path);

///
/// Load the JWT signing secret from environment variable or config file.
/// Priority: GENIUS_JWT_SECRET env var > ./data/jwt_secret file > generate and persist.
/// Validates that the secret is at least 32 characters (256 bits).
///
void LoadJwtSecret()
{
    // Priority 1: Environment variable
    const char* envSecret = std::getenv("GENIUS_JWT_SECRET");
    if (envSecret != nullptr && envSecret[0] != '\0')
    {
        g_jwtSecret = envSecret;
        if (g_jwtSecret.size() >= 32)
        {
            SPDLOG_INFO("JWT secret loaded from environment.");
            return;
        }
        SPDLOG_ERROR("GENIUS_JWT_SECRET is too short (minimum 32 characters).");
        std::exit(1);
    }

    // Priority 2: Config file
    std::string secretPath = g_exeDir + "/data/jwt_secret";
    std::string fileContent = ReadFile(secretPath);
    if (!fileContent.empty())
    {
        // Strip trailing whitespace/newline
        auto end = fileContent.find_last_not_of(" \t\r\n");
        if (end != std::string::npos)
        {
            fileContent.erase(end + 1);
        }
        g_jwtSecret = fileContent;
        if (g_jwtSecret.size() >= 32)
        {
            SPDLOG_INFO("JWT secret loaded from config file.");
            return;
        }
        SPDLOG_ERROR("JWT secret file is too short (minimum 32 characters).");
        std::exit(1);
    }

    // Priority 3: Generate random secret and persist to file
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 15);
    static constexpr const char* hexChars = "0123456789abcdef";

    g_jwtSecret.reserve(64);
    for (int i = 0; i < 64; ++i)
    {
        g_jwtSecret += hexChars[dist(gen)];
    }

    // Create parent directories and write secret to file
    std::filesystem::path secretDir = std::filesystem::path(secretPath).parent_path();
    std::filesystem::create_directories(secretDir);

    std::ofstream out(secretPath, std::ios::binary);
    if (out.is_open())
    {
        out << g_jwtSecret;
        out.close();
        SPDLOG_INFO("JWT secret generated and saved to {}", secretPath);
    }
    else
    {
        SPDLOG_WARN("Could not persist JWT secret to {}. Using in-memory secret.", secretPath);
    }
}

std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
    {
        return {};
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

///
/// Infer MIME type from file extension.
///
std::string GetMimeType(const std::string& path)
{
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".html") == 0)
    {
        return "text/html";
    }
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".css") == 0)
    {
        return "text/css";
    }
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".js") == 0)
    {
        return "application/javascript";
    }
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, ".json") == 0)
    {
        return "application/json";
    }
    return "text/plain";
}

///
/// HTTP session — handles one request and sends a response
///
class HttpSession : public std::enable_shared_from_this<HttpSession>
{
    tcp::socket                          m_socket;
    beast::flat_buffer                   m_buffer;
    http::request<http::string_body>     m_request;
    http::response<http::string_body>    m_response;

public:
    explicit HttpSession(tcp::socket socket)
        : m_socket(std::move(socket))
    {
    }

    void Start()
    {
        ReadRequest();
    }

private:
    void ReadRequest()
    {
        auto self = shared_from_this();

        http::async_read(m_socket, m_buffer, m_request,
            [self](beast::error_code ec, std::size_t)
            {
                if (!ec)
                {
                    self->HandleRequest();
                }
            });
    }

    void SetCorsHeaders()
    {
        m_response.set(http::field::access_control_allow_origin, "*");
        m_response.set(http::field::access_control_allow_methods,
                       "GET, POST, PUT, PATCH, DELETE, OPTIONS");
        m_response.set(http::field::access_control_allow_headers,
                       "Content-Type, Authorization, "
                       "X-Tenant-Id, X-Organization-Id, X-Location-Id, "
                       "Idempotency-Key, Accept");
        m_response.set(http::field::access_control_max_age, "86400");
    }

    void HandleRequest()
    {
        m_response.version(m_request.version());
        m_response.set(http::field::server, "Genius AI Boss");
        m_response.keep_alive(false);
        SetCorsHeaders();

        const std::string fullTarget = std::string(m_request.target());
        const std::string method = std::string(m_request.method_string());

        // Strip query string for routing — handlers match on path only
        auto queryPos = fullTarget.find('?');
        const std::string target = (queryPos != std::string::npos)
            ? fullTarget.substr(0, queryPos)
            : fullTarget;

        // Handle CORS preflight
        if (method == "OPTIONS")
        {
            m_response.result(http::status::no_content);
            m_response.prepare_payload();
            WriteResponse();
            return;
        }

        if (target == "/health" && method == "GET")
        {
            m_response.set(http::field::content_type, "application/json");
            m_response.result(http::status::ok);
            m_response.body() = "{\"status\":\"ok\"}";
        }
        else if (method == "GET" && (target == "/swagger" || target == "/swagger/"))
        {
            ServeStaticFile(g_exeDir + "/swagger/index.html");
        }
        else if (method == "GET" && target == "/swagger/specs")
        {
            // List available spec files in the json/ directory
            std::string jsonDir = g_exeDir + "/json";
            std::string jsonList = "[";
            bool first = true;
            for (const auto& entry : std::filesystem::directory_iterator(jsonDir))
            {
                if (entry.path().extension() == ".json")
                {
                    if (!first) jsonList += ",";
                    jsonList += "\"" + entry.path().filename().string() + "\"";
                    first = false;
                }
            }
            jsonList += "]";
            m_response.set(http::field::content_type, "application/json");
            m_response.result(http::status::ok);
            m_response.body() = jsonList;
        }
        else if (method == "GET" && target.compare(0, 15, "/swagger/specs/") == 0)
        {
            std::string filename = target.substr(15);
            // Prevent directory traversal
            if (filename.find("..") == std::string::npos)
            {
                ServeStaticFile(g_exeDir + "/json/" + filename);
            }
            else
            {
                m_response.set(http::field::content_type, "application/json");
                m_response.result(http::status::bad_request);
                m_response.body() = "{\"error\":\"invalid path\"}";
            }
        }
        else
        {
            m_response.set(http::field::content_type, "application/json");
            std::string body = m_request.body();

            // Build request context
            RequestContext ctx;

            // Paths that do not require JWT authentication
            const bool isUnauthenticatedPath =
                (method == "POST" && target == "/api/v1/auth/login") ||
                (method == "POST" && target == "/api/v1/auth/refresh");

            if (!isUnauthenticatedPath)
            {
                // JWT middleware — require valid Bearer token
                std::string authHeader;
                auto authIt = m_request.find(http::field::authorization);
                if (authIt != m_request.end())
                {
                    authHeader = std::string(authIt->value());
                }

                constexpr size_t kBearerPrefixLen = 7; // length of "Bearer "
                if (authHeader.size() <= kBearerPrefixLen ||
                    authHeader.compare(0, kBearerPrefixLen, "Bearer ") != 0)
                {
                    m_response.result(http::status::unauthorized);
                    m_response.body() =
                        R"({"error":{"code":"UNAUTHORIZED","message":"Missing or invalid Authorization header"}})";
                    m_response.prepare_payload();
                    WriteResponse();
                    return;
                }

                std::string token = authHeader.substr(kBearerPrefixLen);

                if (!ValidateJwtToken(token, g_jwtSecret, ctx))
                {
                    m_response.result(http::status::unauthorized);
                    m_response.body() =
                        R"({"error":{"code":"INVALID_TOKEN","message":"Token is invalid or expired"}})";
                    m_response.prepare_payload();
                    WriteResponse();
                    return;
                }
            }

            std::string result = g_pluginManager->Route(ctx, method, target, body);

            if (result.empty())
            {
                m_response.result(http::status::not_found);
                m_response.body() =
                    "{\"error\":\"no handler found for path: " + target + "\"}";
            }
            else
            {
                m_response.result(http::status::ok);
                m_response.body() = result;
            }
        }

        m_response.prepare_payload();
        WriteResponse();
    }

    void ServeStaticFile(const std::string& relativePath)
    {
        std::string content = ReadFile(relativePath);
        if (content.empty())
        {
            m_response.set(http::field::content_type, "application/json");
            m_response.result(http::status::not_found);
            m_response.body() = "{\"error\":\"file not found: " + relativePath + "\"}";
            return;
        }

        m_response.set(http::field::content_type, GetMimeType(relativePath));
        m_response.result(http::status::ok);
        m_response.body() = std::move(content);
    }

    void WriteResponse()
    {
        auto self = shared_from_this();

        http::async_write(m_socket, m_response,
            [self](beast::error_code ec, std::size_t)
            {
                self->m_socket.shutdown(tcp::socket::shutdown_send, ec);
            });
    }
};

///
/// HTTP server — accepts connections and spawns sessions
///
class HttpServer
{
    tcp::acceptor      m_acceptor;

public:
    HttpServer(net::io_context& ioc, const std::string& host, unsigned short port)
        : m_acceptor(ioc)
    {
        beast::error_code ec;

        // Parse address and create endpoint
        auto addr = net::ip::make_address(host, ec);
        if (ec)
        {
            SPDLOG_ERROR("Invalid address '{}': {}", host, ec.message());
            return;
        }
        tcp::endpoint endpoint(addr, port);

        m_acceptor.open(endpoint.protocol(), ec);
        if (ec)
        {
            SPDLOG_ERROR("Failed to open acceptor: {}", ec.message());
            return;
        }

        m_acceptor.set_option(net::socket_base::reuse_address(true), ec);
        m_acceptor.bind(endpoint, ec);
        if (ec)
        {
            SPDLOG_ERROR("Failed to bind to {}:{} — {}", host, port, ec.message());
            return;
        }

        m_acceptor.listen(net::socket_base::max_listen_connections, ec);
        if (ec)
        {
            SPDLOG_ERROR("Failed to listen: {}", ec.message());
            return;
        }
    }

    void Start()
    {
        AcceptLoop();
    }

    void Stop()
    {
        beast::error_code ec;
        m_acceptor.close(ec);
    }

private:
    void AcceptLoop()
    {
        m_acceptor.async_accept(
            [this](beast::error_code ec, tcp::socket socket)
            {
                if (!ec)
                {
                    std::make_shared<HttpSession>(std::move(socket))->Start();
                }

                if (m_acceptor.is_open())
                {
                    AcceptLoop();
                }
            });
    }
};

} // namespace

int main(int argc, char* argv[])
{
    g_exeDir = GetExecutableDir();

    // Plugin directory: search upward from exeDir for a plugins/ directory
    std::string pluginDir;
    {
        std::string dir = g_exeDir;
        for (int i = 0; i < 10; ++i)
        {
            if (PathExists(dir + "/plugins"))
            {
                pluginDir = dir + "/plugins";
                break;
            }
            auto pos = dir.rfind('/');
            if (pos == std::string::npos || pos == 0)
            {
                break;
            }
            dir = dir.substr(0, pos);
        }
        if (pluginDir.empty())
        {
            pluginDir = g_exeDir + "/plugins";
        }
    }
    std::string host       = "127.0.0.1";
    unsigned short port    = 3000;

    // Parse positional args (skip flags)
    spdlog::level::level_enum logLevel = spdlog::level::info;
    int positionalIdx = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg(argv[i]);
        if (arg == "--debug")
        {
            logLevel = spdlog::level::debug;
        }
        else if (positionalIdx == 0)
        {
            pluginDir = arg;
            ++positionalIdx;
        }
        else if (positionalIdx == 1)
        {
            host = arg;
            ++positionalIdx;
        }
        else if (positionalIdx == 2)
        {
            port = static_cast<unsigned short>(std::atoi(argv[i]));
            ++positionalIdx;
        }
    }

    InitLogging(logLevel);

    SPDLOG_INFO("Starting Genius AI Boss server...");
    SPDLOG_INFO("Plugin directory: {}", pluginDir);

    net::io_context ioc;

    // Load JWT signing secret (from env var, config file, or generate)
    LoadJwtSecret();

    // Create service locator and register PluginManager
    CServiceLocator serviceLocator;
    PluginManager pluginManager;
    g_pluginManager = &pluginManager;
    serviceLocator.RegisterService(gnus::hash::Fnv1a("PluginManager"), &pluginManager);
    serviceLocator.RegisterService(gnus::hash::Fnv1a("JwtSecret"), &g_jwtSecret);

    // Create and register the GSM storage manager (owns the RocksDB engine).
    // GsmStorageManager is registered under Fnv1a("StorageManager") for domain
    // plugins to resolve via the service locator. The raw IStorageEngine is also
    // registered under the legacy Fnv1a("StorageEngine") key for backward
    // compatibility with existing scaffold plugins (identity auth handlers).
    gsm::config::GsmConfig gsmConfig;
    gsmConfig.databasePath = g_exeDir + "/data/db";
    std::filesystem::create_directories(gsmConfig.databasePath);

    auto storageResult = gsm::storage::GsmStorageManager::Create(gsmConfig);
    if (!storageResult.has_value())
    {
        SPDLOG_ERROR("Failed to create storage manager at {}", gsmConfig.databasePath);
        return 1;
    }
    auto storageManager = std::move(storageResult.value());
    serviceLocator.RegisterService(
        gnus::hash::Fnv1a("StorageManager"), storageManager.get());
    serviceLocator.RegisterService(
        gnus::hash::Fnv1a("StorageEngine"), &storageManager->Engine());
    SPDLOG_INFO("Storage manager initialized at {}", gsmConfig.databasePath);

    SPDLOG_INFO("Loading plugins from {}...", pluginDir);
    pluginManager.LoadAllPlugins(pluginDir);

    SPDLOG_INFO("Loaded {} plugin(s).", pluginManager.GetPluginCount());

    SPDLOG_INFO("Initializing plugins...");
    pluginManager.InitializeAll(serviceLocator);

    SPDLOG_INFO("{} handler(s) registered.", pluginManager.GetHandlerCount());

    HttpServer httpServer(ioc, host, port);
    httpServer.Start();

    SPDLOG_INFO("Listening on http://{}:{}", host, port);
    SPDLOG_INFO("Server running. Press Ctrl+C to stop.");

    net::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait(
        [&](beast::error_code ec, int signalNumber)
        {
            SPDLOG_INFO("Received signal {}, shutting down...", signalNumber);
            httpServer.Stop();
            ioc.stop();
        });

    ioc.run();

    SPDLOG_INFO("Shutting down plugins...");
    pluginManager.ShutdownAll();

    SPDLOG_INFO("Goodbye.");
    return 0;
}
