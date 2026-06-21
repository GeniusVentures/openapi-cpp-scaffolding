/**
 * @file       Platform.hpp
 * @brief      Windows platform abstractions
 * @date       2026-06-21
 */
#ifndef GENIUS_OS_PLATFORM_HPP
#define GENIUS_OS_PLATFORM_HPP

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <sys/stat.h>

namespace genius::os
{

// -- DllHandle type (Windows) --
using DllHandle = HMODULE;

// -- Shared library loading (Windows) --
inline DllHandle LoadDll(const char* path)
{
    return LoadLibraryA(path);
}

inline void UnloadDll(DllHandle handle)
{
    FreeLibrary(handle);
}

inline void* GetSymbol(DllHandle handle, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
}

inline bool IsSharedLibrary(const std::string& filename)
{
    static constexpr const char* kDllExtension = ".dll";
    const auto len = filename.size();
    const auto extLen = std::char_traits<char>::length(kDllExtension);
    return (len > extLen && filename.compare(len - extLen, extLen, kDllExtension) == 0);
}

// -- Executable path --
inline std::string GetExecutableDir()
{
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (len > 0)
    {
        std::string path(buf, len);
        auto pos = path.rfind('\\');
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
