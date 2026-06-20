find_program(OPENAPI_GENERATOR_CLI openapi-generator-cli)

if(NOT OPENAPI_GENERATOR_CLI)
    message(FATAL_ERROR "openapi-generator-cli not found. "
                        "Install with: pnpm install -g @openapitools/openapi-generator-cli")
endif()

find_program(PYTHON3_EXECUTABLE python3)

if(NOT PYTHON3_EXECUTABLE)
    message(FATAL_ERROR "python3 not found. Required for plugin wrapper generation.")
endif()

include(ExternalProject)

# Scan submodule-internal specs (e.g., identity_openapi.json — ships with scaffolding)
file(GLOB SPEC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../json/*_openapi.json")

# Scan parent-project specs (e.g., gsm_openapi.json in api-specs/ — project-specific,
# outside the submodule so the parent repo owns them)
file(GLOB EXTERNAL_SPEC_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../api-specs/*_openapi.json")
list(APPEND SPEC_FILES ${EXTERNAL_SPEC_FILES})

set(BACKEND_API_TARGETS "")
set(BACKEND_PLUGIN_TARGETS "")

foreach(SPEC_FILE ${SPEC_FILES})
    get_filename_component(SPEC_BASENAME ${SPEC_FILE} NAME_WE)
    string(REPLACE "_openapi" "" PREFIX ${SPEC_BASENAME})

    set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${PREFIX}")
    set(GEN_DIR "${SRC_DIR}/generated")
    set(PLUGIN_CPP "${GEN_DIR}/${PREFIX}_plugin.cpp")

    # Create cmake-time placeholder — overwritten by generate_plugin.py during build
    if(NOT EXISTS "${PLUGIN_CPP}")
        file(WRITE "${PLUGIN_CPP}" "// Placeholder — overwritten by generate_plugin.py during build\n")
    endif()

    # Step 1: OpenAPI code generation (existing)
    # ExternalProject target uses _ep suffix to avoid conflict with add_library target
    ExternalProject_Add(${PREFIX}_api_ep
        PREFIX ${PREFIX}_api_ep
        SOURCE_DIR "${GEN_DIR}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        PATCH_COMMAND ""
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        CMAKE_CACHE_ARGS
            -DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}
            -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>
    )

    ExternalProject_Add_Step(${PREFIX}_api_ep generate_code
        COMMAND ${OPENAPI_GENERATOR_CLI} generate
            -i ${SPEC_FILE}
            -g cpp-pistache-server
            -o ${GEN_DIR}
        DEPENDS ${SPEC_FILE}
        DEPENDEES configure
        WORKING_DIRECTORY "${SRC_DIR}"
        COMMENT "Generating C++ API code for ${PREFIX} from ${SPEC_BASENAME}"
        ALWAYS OFF
    )

    # Step 2: Fix generated code issues (destructor syntax)
    if(PYTHON3_EXECUTABLE)
        ExternalProject_Add_Step(${PREFIX}_api_ep fix_generated
            COMMAND sh -c "${PYTHON3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/fix_generated_destructors.py ${GEN_DIR}/model/*.cpp"
            DEPENDEES generate_code
            WORKING_DIRECTORY "${SRC_DIR}"
            COMMENT "Fixing generated code for ${PREFIX}"
            ALWAYS OFF
        )

        # Step 3: Generate plugin wrapper from OpenAPI spec
        ExternalProject_Add_Step(${PREFIX}_api_ep generate_plugin
            COMMAND ${PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate_plugin.py
                ${SPEC_FILE}
                ${PLUGIN_CPP}
            DEPENDS ${SPEC_FILE}
            DEPENDEES fix_generated
            WORKING_DIRECTORY "${SRC_DIR}"
            COMMENT "Generating plugin wrapper for ${PREFIX}"
            ALWAYS OFF
        )
    endif()

    list(APPEND BACKEND_API_TARGETS ${PREFIX}_api_ep)

    # Step 3: Add domain subdirectory for building the plugin .dylib
    if(EXISTS "${SRC_DIR}/CMakeLists.txt")
        add_subdirectory(${SRC_DIR})
        list(APPEND BACKEND_PLUGIN_TARGETS ${PREFIX}_api_plugin)
    endif()
endforeach()

add_custom_target(backend_apis DEPENDS ${BACKEND_API_TARGETS})

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/outcome)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/singleton)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/storage)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/base)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/archive)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/tunnel)
