list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(DomainPlugin)
build_domain_plugin(messaging generated)
