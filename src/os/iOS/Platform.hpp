/**
 * @file       Platform.hpp
 * @brief      iOS platform abstractions (POSIX subset)
 * @date       2026-06-21
 */
#ifndef GENIUS_OS_PLATFORM_HPP
#define GENIUS_OS_PLATFORM_HPP

#include <cstdint>
#include <string>

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>

namespace genius::os
{

// -- DllHandle type (POSIX) --
using DllHandle = void*;

// -- Shared library loading (POSIX) --
inline DllHandle LoadDll(const char* path)
{
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}

inline void UnloadDll(DllHandle handle)
{
    dlclose(handle);
}

inline void* GetSymbol(DllHandle handle, const char* name)
{
    return dlsym(handle, name);
}

inline bool IsSharedLibrary(const std::string& filename)
{
    static constexpr const char* kDylibExtension = ".dylib";
    const auto len = filename.size();
    const auto extLen = std::char_traits<char>::length(kDylibExtension);
    return (len > extLen && filename.compare(len - extLen, extLen, kDylibExtension) == 0);
}

// -- Executable path (iOS: dyld fallback to current dir) --
inline std::string GetExecutableDir()
{
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
    {
        std::string path(buf);
        auto pos = path.rfind('/');
        if (pos != std::string::npos)
        {
            return path.substr(0, pos);
        }
    }
    return ".";
}

// -- Path existence --
inline bool PathExists(const std::string& path)
{
    struct stat s;
    return stat(path.c_str(), &s) == 0;
}

} // namespace genius::os

#endif // GENIUS_OS_PLATFORM_HPP
