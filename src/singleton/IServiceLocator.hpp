/**
 * @file       IServiceLocator.hpp
 * @brief      Type-safe service locator interface
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#ifndef ISERVICELOCATOR_HPP
#define ISERVICELOCATOR_HPP

#include <cstdint>

///
/// Service locator interface. Implementations store and retrieve
/// manager instances by FNV-1a hash of their name.
/// Provides type-safe access via templated GetService<T>().
///
class IServiceLocator
{
public:
    virtual ~IServiceLocator() = default;

    ///
    /// Retrieve a service by its hash key, cast to the requested type.
    /// @tparam T       The expected service type
    /// @param  hash    FNV-1a hash of the service name (e.g. Fnv1a("PluginManager"))
    /// @return         Pointer to the service, or nullptr if not found
    ///
    template<typename T>
    T* GetService(uint64_t hash) noexcept
    {
        return static_cast<T*>(GetServiceRaw(hash));
    }

protected:
    ///
    /// Raw service retrieval — implemented by concrete classes.
    /// @param  hash    FNV-1a hash of the service name
    /// @return         Void pointer to the service, or nullptr if not found
    ///
    virtual void* GetServiceRaw(uint64_t hash) noexcept = 0;
};

#endif // ISERVICELOCATOR_HPP
