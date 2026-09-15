list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(DomainPlugin)

# Provide a derived class with real POS handlers instead of CRUD stubs
set(USE_DERIVED_CLASS ON)
build_domain_plugin(commerce generated)

# Derived plugin class (EXPORT_PLUGIN lives here) + POS override handlers
target_sources(commerce_api PRIVATE commerce_plugin_impl.cpp commerce_pos_handlers.cpp)
target_link_libraries(commerce_api PRIVATE spdlog::spdlog)
