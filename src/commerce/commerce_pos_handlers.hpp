/**
 * @file       commerce_pos_handlers.hpp
 * @brief      Declaration for init_commerce_pos_overrides — called from hand-written commerce_plugin_impl.cpp
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * init_commerce_pos_overrides(pm, locator) is called by the hand-written
 * commerce_plugin_impl.cpp after the generated base class initializes. It
 * sets up the storage engine reference and registers the POS order-create
 * override handler (D-01 server-side total recompute) at
 * kOverrideHandlerPriority.
 */

#ifndef COMMERCE_POS_HANDLERS_HPP
#define COMMERCE_POS_HANDLERS_HPP

class PluginManager;
class IServiceLocator;

/**
 * @brief Called from CommercePluginImpl::Initialize().
 *
 * Resolves the storage engine and registers the real POS order-create
 * handler (orders_create — menu-authoritative total recompute per D-01,
 * product_id reference check per D-02) at kOverrideHandlerPriority (200)
 * to override the generated stub (priority 0).
 *
 * @param pm       PluginManager from service locator
 * @param locator  Service locator for StorageEngine
 */
void init_commerce_pos_overrides(PluginManager* pm, IServiceLocator& locator);

#endif // COMMERCE_POS_HANDLERS_HPP
