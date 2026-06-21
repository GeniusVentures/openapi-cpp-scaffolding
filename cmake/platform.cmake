# platform.cmake — resolves the os/ platform subdirectory name from CMAKE_SYSTEM_NAME.
#
# GENIUS_PLATFORM     OSX | Linux | Windows | iOS | Android
#
# Usage:
#   include(cmake/platform.cmake)
#   target_include_directories(target PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/os/${GENIUS_PLATFORM}")

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(GENIUS_PLATFORM "OSX")
else
    set(GENIUS_PLATFORM ${CMAKE_SYSTEM_NAME})
endif()

option(SUPPORTED_PLATFORMS "List of supported platforms" "OSX;Linux;Windows;iOS;Android")

# supported platform check
if(NOT GENIUS_PLATFORM IN_LIST SUPPORTED_PLATFORMS)
    message(FATAL_ERROR "Unsupported platform: ${GENIUS_PLATFORM}")
else()
    message(STATUS "Building for platform: ${GENIUS_PLATFORM}")
    endif()