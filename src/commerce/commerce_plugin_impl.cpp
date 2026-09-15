/**
 * @file       commerce_plugin_impl.cpp
 * @brief      Hand-written Commerce plugin — derives from generated CommercePlugin
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * Subclasses the auto-generated CommercePlugin (generated/commerce_plugin.hpp)
 * to register the POS override handler (orders_create with menu-authoritative
 * total recompute) at kOverrideHandlerPriority (200), superseding the generated
 * CRUD stub.
 *
 * USE_DERIVED_CLASS=ON in CMakeLists.txt skips the generated export shim;
 * EXPORT_PLUGIN lives here instead.
 */
#include "commerce_plugin.hpp"
#include "commerce/commerce_pos_handlers.hpp"

class CommercePluginImpl : public CommercePlugin
{
public:
    bool Initialize(IServiceLocator& manager) noexcept override
    {
        if (!CommercePlugin::Initialize(manager))
        {
            return false;
        }
        auto* pm = manager.GetService<PluginManager>(Fnv1a("PluginManager"));
        init_commerce_pos_overrides(pm, manager);
        return true;
    }
};

EXPORT_PLUGIN(CommercePluginImpl)
