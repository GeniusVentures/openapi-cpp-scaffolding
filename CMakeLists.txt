add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src)

if(BUILD_TESTING)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/tests)
endif()

# Install submodule-internal OpenAPI specs (identity, etc.)
install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/json"
    DESTINATION backend
    FILES_MATCHING PATTERN "*.json"
)

# Install parent-project OpenAPI specs if api-specs/ exists
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../api-specs")
    install(DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../api-specs"
        DESTINATION backend
        FILES_MATCHING PATTERN "*.json"
    )
endif()
