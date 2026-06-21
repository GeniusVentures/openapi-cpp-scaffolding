# platform.cmake — resolves the os/ platform subdirectory name from CMAKE_SYSTEM_NAME.
#
# GENIUS_PLATFORM     OSX | Linux | Windows | iOS | Android
#
# Usage:
#   include(cmake/platform.cmake)
#   target_include_directories(target PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/os/${GENIUS_PLATFORM}")

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(TARGET_PLATFORM "OSX")
else()
    set(TARGET_PLATFORM "${CMAKE_SYSTEM_NAME}")
endif()

set(SUPPORTED_PLATFORMS "OSX;Linux;Windows;iOS;Android"
    CACHE STRING "Semicolon-separated list of supported target platforms"
)

# supported platform check
if(NOT TARGET_PLATFORM IN_LIST SUPPORTED_PLATFORMS)
    message(FATAL_ERROR
        "Unsupported platform: '${TARGET_PLATFORM}'. "
        "Supported platforms: '${SUPPORTED_PLATFORMS}'"
    )
else()
    message(STATUS "Building for platform: ${TARGET_PLATFORM}")
endif()