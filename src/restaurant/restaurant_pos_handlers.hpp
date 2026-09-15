/**
 * @file       restaurant_pos_handlers.hpp
 * @brief      Declaration for init_restaurant_pos_overrides — called from hand-written restaurant_plugin_impl.cpp
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * init_restaurant_pos_overrides(pm, locator) is called by the hand-written
 * restaurant_plugin_impl.cpp after the generated base class initializes. It
 * sets up the storage engine reference and registers POS list handlers at
 * kOverrideHandlerPriority.
 */

#ifndef RESTAURANT_POS_HANDLERS_HPP
#define RESTAURANT_POS_HANDLERS_HPP

class PluginManager;
class IServiceLocator;

/**
 * @brief Called from RestaurantPluginImpl::Initialize().
 *
 * Resolves the storage engine and registers real POS list handlers
 * (menu-categories, menu-items, modifier-groups) at
 * kOverrideHandlerPriority (200) to override generated stubs (priority 0).
 * No seed data — Phase 2 ships none.
 *
 * @param pm       PluginManager from service locator
 * @param locator  Service locator for StorageEngine
 */
void init_restaurant_pos_overrides(PluginManager* pm, IServiceLocator& locator);

#endif // RESTAURANT_POS_HANDLERS_HPP
