/**
 * @file       identity_auth_handlers.hpp
 * @brief      Declaration for init_identity_overrides — called from hand-written identity_plugin.cpp
 * @date       2026-06-17
 * @author     Kenneth L. Hurley
 *
 * init_identity_overrides(pm, locator) is called by the hand-written
 * identity_plugin.cpp after the generated base class initializes. It sets up
 * storage/JWT, seeds admin if DB is empty, and registers auth handlers at
 * kOverrideHandlerPriority.
 */

#ifndef IDENTITY_AUTH_HANDLERS_HPP
#define IDENTITY_AUTH_HANDLERS_HPP

class PluginManager;
class IServiceLocator;

/**
 * @brief Called from generated identity_plugin.cpp Initialize().
 *
 * Sets up storage engine and JWT secret references, seeds the admin user
 * if the DB is empty, and registers real auth handlers (login, logout, me, refresh)
 * at kOverrideHandlerPriority (200) to override generated stubs (priority 0).
 *
 * @param pm       PluginManager from service locator
 * @param locator  Service locator for StorageEngine and JwtSecret
 */
void init_identity_overrides(PluginManager* pm, IServiceLocator& locator);

#endif // IDENTITY_AUTH_HANDLERS_HPP
