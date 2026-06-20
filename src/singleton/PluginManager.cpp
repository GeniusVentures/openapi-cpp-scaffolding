/**
 * @file       PluginManager.cpp
 * @brief      Plugin lifecycle, URL routing, and handler dispatch implementation
 * @date       2026-05-25
 * @author     Kenneth L. Hurley
 */

#include "PluginManager.hpp"

#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <dirent.h>
#endif

#include "singleton/fnv1a.hpp"

namespace
{

#ifdef _WIN32

using DllHandle = HMODULE;

DllHandle LoadDll(const char* path)
{
    return LoadLibraryA(path);
}

void UnloadDll(DllHandle handle)
{
    FreeLibrary(handle);
}

void* GetSymbol(DllHandle handle, const char* name)
{
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
}

bool IsSharedLibrary(const std::string& filename)
{
    const auto len = filename.size();
    return (len > 4 && filename.compare(len - 4, 4, ".dll") == 0);
}

#else // macOS / Linux

using DllHandle = void*;

#if defined(__APPLE__)
static constexpr const char* kDylibExtension = ".dylib";
#elif defined(__ANDROID__)
static constexpr const char* kDylibExtension = ".so";
#else
static constexpr const char* kDylibExtension = ".so";
#endif

DllHandle LoadDll(const char* path)
{
    return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}

void UnloadDll(DllHandle handle)
{
    dlclose(handle);
}

bool IsSharedLibrary(const std::string& filename)
{
    const auto len = filename.size();
    const auto extLen = std::strlen(kDylibExtension);
    return (len > extLen && filename.compare(len - extLen, extLen, kDylibExtension) == 0);
}

#endif // _WIN32

} // namespace

void PluginManager::RegisterPlugin(std::shared_ptr<IPlugin>       plugin,
                                    unsigned int                    priority,
                                    const std::vector<std::string>& urlPaths)
{
    const std::string pluginName = plugin->GetName();

    PluginEntry entry;
    entry.plugin   = std::move(plugin);
    entry.urlPaths = urlPaths;
    entry.dlHandle = nullptr;

    m_plugins[pluginName] = entry;

    for (const auto& path : urlPaths)
    {
        m_routes[path] = pluginName;
    }

    m_initQueue.insert({priority, pluginName});
    m_registrationOrder.push_back(pluginName);

    SPDLOG_DEBUG("Registered plugin '{}' with {} path(s), priority {}",
                 pluginName, urlPaths.size(), priority);
}

void PluginManager::RegisterHandler(const std::string& method,
                                     const std::string& urlPath,
                                     const std::string& functionName,
                                     const HandlerFn&   fn,
                                     const std::string& ownerPluginName,
                                     unsigned int       priority)
{
    HandlerEntry entry;
    entry.fn              = fn;
    entry.method          = method;
    entry.ownerPluginName = ownerPluginName;
    entry.functionName    = functionName;
    entry.hash            = gnus::hash::Fnv1a(functionName);

    // Key is "METHOD /path" for exact match lookup
    std::string key = method + " " + urlPath;
    m_handlers[key][priority] = entry;

    // Also store in pattern list for fallback matching
    m_patternHandlers.push_back({method, urlPath, priority, entry});

    SPDLOG_DEBUG("Registered handler {} {} -> {} (priority {}, owner: {})",
                 method, urlPath, functionName, priority, ownerPluginName);
}

void PluginManager::LoadAllPlugins(const std::string& pluginDir)
{
    using CreatePluginFn = IPlugin* (*)();

    SPDLOG_INFO("Scanning for plugins in: {}", pluginDir);

#ifdef _WIN32
    std::string searchPath = pluginDir + "\\*.dll";
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        return;
    }

    do
    {
        std::string fullPath = pluginDir + "\\" + findData.cFileName;
        DllHandle handle = LoadDll(fullPath.c_str());
        if (handle != nullptr)
        {
            auto createFn = reinterpret_cast<CreatePluginFn>(
                GetSymbol(handle, "CreatePlugin"));
            if (createFn != nullptr)
            {
                IPlugin* rawPlugin = createFn();
                if (rawPlugin != nullptr)
                {
                    auto plugin = std::shared_ptr<IPlugin>(rawPlugin);
                    RegisterPlugin(plugin, plugin->GetPriority(), plugin->GetUrlPaths());
                    auto it = m_plugins.find(plugin->GetName());
                    if (it != m_plugins.end())
                    {
                        it->second.dlHandle = static_cast<void*>(handle);
                    }
                }
            }
        }
    }
    while (FindNextFileA(hFind, &findData) != 0);

    FindClose(hFind);
#else
    DIR* dir = opendir(pluginDir.c_str());
    if (dir == nullptr)
    {
        return;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_type != DT_REG && entry->d_type != DT_LNK)
        {
            continue;
        }

        if (!IsSharedLibrary(entry->d_name))
        {
            continue;
        }

        std::string fullPath = pluginDir + "/" + entry->d_name;
        DllHandle handle = LoadDll(fullPath.c_str());
        if (handle != nullptr)
        {
            auto createFn = reinterpret_cast<CreatePluginFn>(
                dlsym(handle, "CreatePlugin"));
            if (createFn != nullptr)
            {
                IPlugin* rawPlugin = createFn();
                if (rawPlugin != nullptr)
                {
                    auto plugin = std::shared_ptr<IPlugin>(rawPlugin);
                    RegisterPlugin(plugin, plugin->GetPriority(), plugin->GetUrlPaths());
                    auto it = m_plugins.find(plugin->GetName());
                    if (it != m_plugins.end())
                    {
                        it->second.dlHandle = static_cast<void*>(handle);
                    }
                }
            }
        }
    }

    closedir(dir);
#endif
}

void PluginManager::InitializeAll(IServiceLocator& manager)
{
    for (const auto& kv : m_initQueue)
    {
        const auto& pluginName = kv.second;
        auto it = m_plugins.find(pluginName);
        if (it != m_plugins.end() && it->second.plugin)
        {
            it->second.plugin->Initialize(manager);
        }
    }
}

void PluginManager::ShutdownAll()
{
    SPDLOG_INFO("Shutting down {} plugin(s)...", m_plugins.size());

    for (auto it = m_initQueue.rbegin(); it != m_initQueue.rend(); ++it)
    {
        const auto& pluginName = it->second;
        auto pit = m_plugins.find(pluginName);
        if (pit != m_plugins.end() && pit->second.plugin)
        {
            pit->second.plugin->Shutdown();
            pit->second.plugin->DeInit();
        }
    }

    for (auto& kv : m_handlers)
    {
        auto& priorityMap = kv.second;
        auto it = priorityMap.begin();
        while (it != priorityMap.end())
        {
            if (m_plugins.find(it->second.ownerPluginName) == m_plugins.end())
            {
                it = priorityMap.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    // Destroy plugin shared_ptrs while .dylib code is still loaded,
    // then dlclose the handles.
    std::vector<DllHandle> handles;
    for (auto& kv : m_plugins)
    {
        if (kv.second.dlHandle != nullptr)
        {
            handles.push_back(static_cast<DllHandle>(kv.second.dlHandle));
        }
    }

    m_plugins.clear();
    m_routes.clear();
    m_handlers.clear();
    m_patternHandlers.clear();
    m_initQueue.clear();
    m_registrationOrder.clear();

    for (auto handle : handles)
    {
        UnloadDll(handle);
    }
}

bool PluginManager::MatchesPattern(const std::string& pattern,
                                    const std::string& requestPath)
{
    // Split both by '/' and compare segment by segment
    size_t pi = 0;
    size_t ri = 0;
    while (pi < pattern.size() && ri < requestPath.size())
    {
        // Find next segment in pattern
        size_t pNext = pattern.find('/', pi);
        if (pNext == std::string::npos)
        {
            pNext = pattern.size();
        }
        std::string pSeg = pattern.substr(pi, pNext - pi);

        // Find next segment in request
        size_t rNext = requestPath.find('/', ri);
        if (rNext == std::string::npos)
        {
            rNext = requestPath.size();
        }
        std::string rSeg = requestPath.substr(ri, rNext - ri);

        // If pattern segment is not a param, must match exactly
        if (!(pSeg.size() >= 2 && pSeg.front() == '{' && pSeg.back() == '}'))
        {
            if (pSeg != rSeg)
            {
                return false;
            }
        }
        else
        {
            // Param segment: must be non-empty
            if (rSeg.empty())
            {
                return false;
            }
        }

        pi = pNext + (pNext < pattern.size() ? 1 : 0);
        ri = rNext + (rNext < requestPath.size() ? 1 : 0);
    }

    // Both must be fully consumed
    return pi >= pattern.size() && ri >= requestPath.size();
}

std::string PluginManager::Route(const RequestContext& ctx,
                                  const std::string& method,
                                  const std::string& urlPath,
                                  const std::string& jsonBody)
{
    // Try exact match first: "METHOD /path"
    std::string key = method + " " + urlPath;
    auto handlerIt = m_handlers.find(key);
    if (handlerIt != m_handlers.end() && !handlerIt->second.empty())
    {
        const auto& topHandler = handlerIt->second.rbegin()->second;
        return topHandler.fn(ctx, method, urlPath, jsonBody);
    }

    // Fallback: pattern matching
    const HandlerEntry* bestMatch = nullptr;
    unsigned int bestPriority = 0;

    for (const auto& pe : m_patternHandlers)
    {
        if (pe.method != method)
        {
            continue;
        }
        if (!MatchesPattern(pe.pattern, urlPath))
        {
            continue;
        }
        if (pe.priority > bestPriority)
        {
            bestPriority = pe.priority;
            bestMatch = &pe.handler;
        }
    }

    if (bestMatch != nullptr)
    {
        return bestMatch->fn(ctx, method, urlPath, jsonBody);
    }

    return std::string();
}

size_t PluginManager::GetPluginCount() const noexcept
{
    return m_plugins.size();
}

size_t PluginManager::GetHandlerCount() const noexcept
{
    size_t count = 0;
    for (const auto& kv : m_handlers)
    {
        count += kv.second.size();
    }
    return count;
}
