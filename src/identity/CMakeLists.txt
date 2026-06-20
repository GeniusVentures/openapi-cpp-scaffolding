list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(DomainPlugin)

# Provide a derived class with real auth handlers instead of CRUD stubs
set(USE_DERIVED_CLASS ON)
build_domain_plugin(identity generated)

add_library(auth_utils STATIC auth_utils.cpp)
target_include_directories(auth_utils PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../jwt-cpp/include")
target_link_libraries(auth_utils PUBLIC OpenSSL::Crypto component_factory spdlog::spdlog)

# Derived plugin class (EXPORT_PLUGIN lives here) + auth override handlers
target_sources(identity_api PRIVATE identity_plugin_impl.cpp identity_auth_handlers.cpp)
target_link_libraries(identity_api PRIVATE auth_utils)
