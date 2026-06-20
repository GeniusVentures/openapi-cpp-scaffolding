/**
 * @file       PluginManager.hpp
 * @brief      Singleton orchestrator for plugin lifecycle, URL routing, and handler dispatch
 * @date       2026-05-25
 * @author     Kenneth L. Hurley
 */
#ifndef PLUGINMANAGER_HPP
#define PLUGINMANAGER_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "singleton/IPlugin.hpp"

/// Handler priority constants. Higher wins in Route() dispatch.
static constexpr unsigned int kStubHandlerPriority     = 0;    ///< Generated stubs (baseline)
static constexpr unsigned int kMockHandlerPriority     = 100;  ///< Test mocks
static constexpr unsigned int kOverrideHandlerPriority = 200;  ///< Production overrides

class PluginManager
{

public:
    PluginManager() = default;
    ~PluginManager() = default;

    // Non-copyable
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

private:
    struct PluginEntry
    {
        std::shared_ptr<IPlugin> plugin;
        std::vector<std::string> urlPaths;
        void* dlHandle;
    };

    struct HandlerEntry
    {
        HandlerFn   fn;
        std::string method;
        std::string ownerPluginName;
        std::string functionName;
        uint64_t    hash;
    };

    struct PatternEntry
    {
        std::string method;
        std::string pattern;
        unsigned int priority;
        HandlerEntry handler;
    };

    std::unordered_map<std::string, PluginEntry>          m_plugins;
    std::unordered_map<std::string, std::string>          m_routes;
    /// Key: "METHOD /path" for exact match lookup
    /// Value: ordered map of handlers by priority (highest wins)
    std::unordered_map<std::string, std::map<unsigned int, HandlerEntry>> m_handlers;
    /// Fallback list for pattern-based matching (e.g. paths with {param} segments)
    std::vector<PatternEntry>                             m_patternHandlers;
    std::multimap<unsigned int, std::string>                       m_initQueue;
    std::vector<std::string>                              m_registrationOrder;

    ///
    /// Returns true if requestPath matches the registered pattern.
    /// Pattern segments like {param} match any non-empty segment.
    ///
    static bool MatchesPattern(const std::string& pattern,
                               const std::string& requestPath);

public:
    ///
    /// Called from __attribute__((constructor)) in each plugin .dylib.
    /// Registers the plugin instance and its URL path prefixes.
    /// The dlHandle is tracked internally by LoadAllPlugins after dlopen returns.
    ///
    void RegisterPlugin(std::shared_ptr<IPlugin>       plugin,
                        unsigned int                    priority,
                        const std::vector<std::string>& urlPaths);

    ///
    /// Registers a handler function for an HTTP method and URL path.
    /// FNV-1a hash is computed internally from functionName.
    /// Higher priority handlers override lower ones for the same method+path.
    void RegisterHandler(const std::string& method,
                         const std::string& urlPath,
                         const std::string& functionName,
                         const HandlerFn&   fn,
                         const std::string& ownerPluginName,
                         unsigned int       priority);

    ///
    /// Scans pluginDir for shared libraries (*.so / *.dylib / *.dll) and
    /// dlopen each one. The constructor attribute in each library triggers
    /// RegisterPlugin and RegisterHandler calls automatically.
    ///
    void LoadAllPlugins(const std::string& pluginDir);

    ///
    /// Calls Initialize() on every registered plugin in ascending priority
    /// order (lowest priority number first).
    /// @param manager  Service locator passed to each plugin's Initialize()
    ///
    void InitializeAll(IServiceLocator& manager);

    ///
    /// Calls Shutdown() then DeInit() on every plugin in reverse priority
    /// order, then dlclose each handle.
    ///
    void ShutdownAll();

    ///
    /// Looks up the handler for the given HTTP method and URL path and dispatches
    /// with the request context and JSON body. First tries an exact match on
    /// "METHOD /path", then falls back to segment-by-segment pattern matching
    /// where {param} segments match any non-empty path segment.
    ///
    /// @param  ctx         Request context carrying identity and tenant info
    /// @param  method      HTTP method (e.g. "GET", "POST")
    /// @param  urlPath     Full URL path (e.g. "/api/hrm/employees")
    /// @param  jsonBody    Raw JSON body passed to the handler
    /// @return             Handler response string, or empty string if not found
    ///
    std::string Route(const RequestContext& ctx,
                      const std::string& method,
                      const std::string& urlPath,
                      const std::string& jsonBody);

    ///
    /// Returns the number of registered plugins.
    ///
    size_t GetPluginCount() const noexcept;

    ///
    /// Returns the number of registered handlers across all plugins.
    ///
    size_t GetHandlerCount() const noexcept;
};

#endif // PLUGINMANAGER_HPP
