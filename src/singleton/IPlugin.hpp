/**
 * @file       IPlugin.hpp
 * @brief      Plugin interface extending IComponent with lifecycle management
 * @date       2026-05-25
 * @author     Kenneth L. Hurley
 */
#ifndef IPLUGIN_HPP
#define IPLUGIN_HPP

#include <cstdint>
#include <functional>
#include <string>
#include "singleton/IComponent.hpp"

class IServiceLocator;

///
/// Per-request context carrying identity and tenant information.
/// Populated by authentication middleware before handler dispatch.
///
struct RequestContext
{
    std::string tenantId;
    std::string organizationId;
    std::string userId;
    std::string locationId;
    bool        authenticated = false;
};

///
/// Function signature for plugin request handlers.
/// Accepts the request context, HTTP method, URL path, and raw JSON body string.
/// Returns a raw JSON response string.
///
using HandlerFn = std::function<std::string(const RequestContext& ctx,
                                            const std::string& method,
                                            const std::string& urlPath,
                                            const std::string& body)>;

///
/// Interface for loadable plugins.
/// Plugins are discovered via dlopen, self-register via constructor attribute,
/// and are initialized by PluginManager in priority order.
///
class IPlugin : public IComponent
{
public:
    ~IPlugin() override = default;

    ///
    /// Priority determines initialization order (lower number = earlier).
    /// Multiple plugins may share the same priority.
    ///
    virtual unsigned int GetPriority() const noexcept = 0;

    ///
    /// URL paths this plugin handles. Used by LoadAllPlugins for registration.
    ///
    virtual const std::vector<std::string>& GetUrlPaths() const noexcept = 0;

    ///
    /// Called after all plugins are loaded. Register handlers and begin work.
    /// The service locator provides access to PluginManager and other services.
    /// @param manager  Service locator for accessing server services.
    ///
    virtual bool Initialize(IServiceLocator& manager) noexcept = 0;

    ///
    /// Graceful stop. Cease accepting new work, drain in-flight.
    ///
    virtual bool Shutdown() noexcept = 0;

    ///
    /// Release resources after shutdown is complete.
    /// All registered handlers for this plugin are removed by PluginManager.
    ///
    virtual bool DeInit() noexcept = 0;
};

#endif // IPLUGIN_HPP
