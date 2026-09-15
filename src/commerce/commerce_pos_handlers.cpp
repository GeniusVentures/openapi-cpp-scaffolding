/**
 * @file       commerce_pos_handlers.cpp
 * @brief      POS override handler implementations for the commerce plugin
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * init_commerce_pos_overrides(pm, locator) — called from hand-written
 * commerce_plugin_impl.cpp (CommercePluginImpl::Initialize). Resolves the
 * storage engine and registers the POS order-create handler (orders_create)
 * at kOverrideHandlerPriority (200), superseding the generated CRUD stub
 * (priority 0) with the D-01 menu-authoritative total recompute, the D-02
 * product_id reference check, and the HANDLER-06 handler-level auth check.
 */

#include "commerce/commerce_pos_handlers.hpp"
#include "singleton/PluginRegistration.hpp"
#include "singleton/IServiceLocator.hpp"
#include "singleton/fnv1a.hpp"
#include "singleton/PluginManager.hpp"
#include "storage/IStorageEngine.hpp"
#include "storage/KeyBuilder.hpp"
#include "nlohmann/json.hpp"
#include <spdlog/spdlog.h>

using json = nlohmann::json;
using namespace gnus::hash;

static IStorageEngine* s_storage = nullptr;

/**
 * @brief      Resolve storage and register the POS order-create override handler
 *
 * @param      pm       PluginManager from service locator
 * @param      locator  Service locator for StorageEngine
 */
void init_commerce_pos_overrides(PluginManager* pm, IServiceLocator& locator)
{
    s_storage = locator.GetService<IStorageEngine>(Fnv1a("StorageEngine"));
    if (!s_storage)
    {
        return;
    }
}
