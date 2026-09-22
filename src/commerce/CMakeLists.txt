list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(DomainPlugin)

# Derived plugin class (with POS override handlers) is supplied by the
# consuming parent repo via the USE_DERIVED_CLASS_COMMERCE hook — see
# cmake/DomainPlugin.cmake.
build_domain_plugin(commerce generated)
