# platform.cmake — resolves the os/ platform subdirectory name from CMAKE_SYSTEM_NAME.
#
# GENIUS_PLATFORM     OSX | Linux | Windows | iOS | Android
#
# Usage:
#   include(cmake/platform.cmake)
#   target_include_directories(target PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/os/${GENIUS_PLATFORM}")

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(GENIUS_PLATFORM "OSX")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(GENIUS_PLATFORM "Linux")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(GENIUS_PLATFORM "Windows")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(GENIUS_PLATFORM "Android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(GENIUS_PLATFORM "iOS")
else()
    message(FATAL_ERROR "Unsupported CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME}")
endif()
