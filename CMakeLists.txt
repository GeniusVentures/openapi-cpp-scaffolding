include(${CMAKE_CURRENT_LIST_DIR}/cmake/platform.cmake)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/json)

if(BUILD_TESTING)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/tests)
endif()
