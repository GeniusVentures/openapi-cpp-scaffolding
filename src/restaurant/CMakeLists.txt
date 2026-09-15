list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(DomainPlugin)

# Provide a derived class with real POS handlers instead of CRUD stubs
set(USE_DERIVED_CLASS ON)
build_domain_plugin(restaurant generated)

# Derived plugin class (EXPORT_PLUGIN lives here) + POS override handlers
target_sources(restaurant_api PRIVATE restaurant_plugin_impl.cpp restaurant_pos_handlers.cpp)
target_link_libraries(restaurant_api PRIVATE spdlog::spdlog)
