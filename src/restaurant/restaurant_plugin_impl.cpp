/**
 * @file       restaurant_plugin_impl.cpp
 * @brief      Hand-written Restaurant plugin — derives from generated RestaurantPlugin
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * Subclasses the auto-generated RestaurantPlugin (generated/restaurant_plugin.hpp)
 * to register POS override handlers (menu-categories/menu-items/modifier-groups
 * lists) at kOverrideHandlerPriority (200), superseding the generated CRUD stubs.
 *
 * USE_DERIVED_CLASS=ON in CMakeLists.txt skips the generated export shim;
 * EXPORT_PLUGIN lives here instead.
 */
#include "restaurant_plugin.hpp"
#include "restaurant/restaurant_pos_handlers.hpp"

class RestaurantPluginImpl : public RestaurantPlugin
{
public:
    bool Initialize(IServiceLocator& manager) noexcept override
    {
        if (!RestaurantPlugin::Initialize(manager))
        {
            return false;
        }
        auto* pm = manager.GetService<PluginManager>(Fnv1a("PluginManager"));
        init_restaurant_pos_overrides(pm, manager);
        return true;
    }
};

EXPORT_PLUGIN(RestaurantPluginImpl)
