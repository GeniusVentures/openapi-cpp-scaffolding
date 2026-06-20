/**
 * @file       identity_plugin_impl.cpp
 * @brief      Hand-written Identity plugin — derives from generated IdentityPlugin
 * @date       2026-06-17
 * @author     Kenneth L. Hurley
 *
 * Subclasses the auto-generated IdentityPlugin (generated/identity_plugin.hpp)
 * to register real auth handlers (login, /me, refresh, logout) at
 * kOverrideHandlerPriority (200), superseding the generated CRUD stubs.
 *
 * USE_DERIVED_CLASS=ON in CMakeLists.txt skips the generated export shim;
 * EXPORT_PLUGIN lives here instead.
 */
#include "identity_plugin.hpp"
#include "identity/identity_auth_handlers.hpp"

class IdentityPluginImpl : public IdentityPlugin
{
public:
    bool Initialize(IServiceLocator& manager) noexcept override
    {
        if (!IdentityPlugin::Initialize(manager))
        {
            return false;
        }
        auto* pm = manager.GetService<PluginManager>(Fnv1a("PluginManager"));
        init_identity_overrides(pm, manager);
        return true;
    }
};

EXPORT_PLUGIN(IdentityPluginImpl)
