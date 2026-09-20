/**
 * @file       restaurant_pos_handlers.hpp
 * @brief      Declaration for init_restaurant_pos_overrides — called from hand-written restaurant_plugin_impl.cpp
 * @date       2026-09-15
 * @author     Kenneth L. Hurley
 *
 * init_restaurant_pos_overrides(pm, locator) is called by the hand-written
 * restaurant_plugin_impl.cpp after the generated base class initializes. It
 * sets up the storage engine reference and registers POS list handlers at
 * kOverrideHandlerPriority. Phase 3.1 adds the table handlers (listTables,
 * createTable, getTable, seatTable, updateTable, deleteTable) on the same
 * pattern with strict key-set write contracts, server-owned lifecycle
 * transitions, and the D-03 tenant boundary on every by-id route.
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
 * Phase 3.1 table handlers: GET+POST /api/v1/restaurant/tables,
 * GET+PATCH+DELETE /api/v1/restaurant/tables/{tableId}, and POST
 * /api/v1/restaurant/tables/{tableId}/seat — strict key-set write contracts,
 * server-defaulted lifecycle on create, event-driven pos_status/
 * open_order_ids/guest_count/server_id/opened_at transitions on seat/update,
 * tenant-checked delete on DELETE (CR-01).
 *
 * @param pm       PluginManager from service locator
 * @param locator  Service locator for StorageEngine
 */
void init_restaurant_pos_overrides(PluginManager* pm, IServiceLocator& locator);

#endif // RESTAURANT_POS_HANDLERS_HPP
