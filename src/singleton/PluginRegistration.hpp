/**
 * @file       PluginRegistration.hpp
 * @brief      Macros for plugin metadata and handler registration
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#ifndef PLUGINREGISTRATION_HPP
#define PLUGINREGISTRATION_HPP

#include <memory>
#include <string>
#include <vector>

#include "singleton/PluginManager.hpp"
#include "singleton/IServiceLocator.hpp"
#include "singleton/fnv1a.hpp"

///
/// Stores plugin priority and URL paths in the plugin class.
/// Place inside the plugin class body.
///
/// Usage:
///     class HrmPlugin : public IPlugin
///     {
///         REGISTER_PLUGIN(100, {"/api/v1/employees", "/api/v1/shifts"})
///     public:
///         bool Initialize(IServiceLocator& manager) noexcept override { ... }
///     };
///
#define REGISTER_PLUGIN(pri, urlPathsVar)                                          \
    unsigned int           m_priority = pri;                                       \
    std::vector<std::string> m_urlPaths = std::vector<std::string>urlPathsVar;    \
public:                                                                            \
    unsigned int GetPriority() const noexcept override { return m_priority; }      \
    const std::vector<std::string>& GetUrlPaths() const noexcept override          \
    { return m_urlPaths; }                                                         \
private:

///
/// Registers a handler with the PluginManager retrieved from the service locator.
/// Use inside Initialize(IServiceLocator& manager).
///
/// Usage:
///     bool Initialize(IServiceLocator& manager) noexcept override
///     {
///         auto* pm = manager.GetService<PluginManager>(Fnv1a("PluginManager"));
///         pm->RegisterPlugin(shared_from_this(), GetPriority(), m_urlPaths);
///         REGISTER_HANDLER(pm, "GET", "/api/v1/employees", listEmployees)
///         REGISTER_HANDLER(pm, "POST", "/api/v1/employees", createEmployee)
///         return true;
///     }
///
#define REGISTER_HANDLER(pm, method, urlPath, fn, priority)                   \
    pm->RegisterHandler(method, urlPath, #fn, fn, GetName(), priority);

///
/// Exports a C function that creates a plugin instance.
/// Place at file scope in the plugin .cpp, after the class definition.
///
/// Usage:
///     EXPORT_PLUGIN(HRMPlugin)
///
#define EXPORT_PLUGIN(ClassName)                                                   \
    extern "C" __attribute__((visibility("default")))                              \
    IPlugin* CreatePlugin() { return new ClassName(); }

#endif // PLUGINREGISTRATION_HPP
