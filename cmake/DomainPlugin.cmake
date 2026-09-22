# DomainPlugin.cmake
# Shared macro for building domain plugin .dylib/.so/.dll files.
#
# Usage:
#   build_domain_plugin(<prefix>)                   — hand-written _plugin.cpp in module root
#   build_domain_plugin(<prefix> <subdir>)          — generated _plugin.cpp in <subdir>/
#
# The generated plugin wrapper ({prefix}_plugin.cpp) is produced by
# generate_plugin.py during the backend_apis ExternalProject step.

macro(build_domain_plugin PREFIX PLUGIN_SUBDIR)
    if(${ARGC} EQUAL 1)
        set(_SUBDIR "")
    else()
        set(_SUBDIR "${PLUGIN_SUBDIR}/")
    endif()

    set(PLUGIN_CPP "${CMAKE_CURRENT_SOURCE_DIR}/${_SUBDIR}${PREFIX}_plugin.cpp")

    # Mark as generated — overwritten by generate_plugin.py during build
    set_source_files_properties(${PLUGIN_CPP} PROPERTIES GENERATED TRUE)

    # Model sources from OpenAPI-generated code (cpp-pistache-server template)
    file(GLOB MODEL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/generated/model/*.cpp")

    option(USE_DERIVED_CLASS "Provide a hand-written derived plugin class with its own EXPORT_PLUGIN" OFF)

    # Per-prefix variant for a derived class supplied by the CONSUMING parent
    # repo: the parent sets USE_DERIVED_CLASS_<PREFIX>=ON (uppercased) in its
    # cache BEFORE add_subdirectory() of the scaffold, then appends its impl
    # sources to ${PREFIX}_api from outside this repo.
    string(TOUPPER "${PREFIX}" _PREFIX_UPPER)
    option(USE_DERIVED_CLASS_${_PREFIX_UPPER} "Derived plugin class for ${PREFIX} supplied by the consuming parent" OFF)

    # When either is ON, skip the generated export shim — the derived class
    # provides the one .cpp with EXPORT_PLUGIN.
    if(NOT USE_DERIVED_CLASS AND NOT USE_DERIVED_CLASS_${_PREFIX_UPPER})
        set(_PLUGIN_SOURCES ${PLUGIN_CPP} ${MODEL_SOURCES})
    else()
        set(_PLUGIN_SOURCES ${MODEL_SOURCES})
    endif()

    add_library(${PREFIX}_api SHARED ${_PLUGIN_SOURCES})

    target_include_directories(${PREFIX}_api PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated"
        "${CMAKE_CURRENT_SOURCE_DIR}/generated/model"
        "${_THIRDPARTY_DIR}/json/single_include"
    )

    target_link_libraries(${PREFIX}_api PRIVATE
        component_factory
        storage
    )

    # Depend on the ExternalProject that generates code + plugin wrapper
    if(TARGET ${PREFIX}_api_ep)
        add_dependencies(${PREFIX}_api ${PREFIX}_api_ep)
    endif()

    # Output to plugins/ directory for easy server loading
    set_target_properties(${PREFIX}_api PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
        PREFIX ""
    )

    # Install plugin .dylib/.so/.dll to <prefix>/plugins/
    install(TARGETS ${PREFIX}_api
        LIBRARY DESTINATION plugins
        RUNTIME DESTINATION plugins
    )
endmacro()
