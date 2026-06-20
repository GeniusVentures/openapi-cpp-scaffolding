/**
 * @file       CServiceLocator.hpp
 * @brief      Concrete service locator — stores and retrieves services by hash key
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#ifndef CSERVICELOCATOR_HPP
#define CSERVICELOCATOR_HPP

#include <cstdint>
#include <unordered_map>

#include "singleton/IServiceLocator.hpp"

///
/// Concrete service locator. Stores service pointers keyed by FNV-1a hash.
/// Thread-safe for reads after initialization (services registered during
/// startup, queried during request handling).
///
class CServiceLocator : public IServiceLocator
{
    std::unordered_map<uint64_t, void*> m_services;

public:
    CServiceLocator() = default;
    ~CServiceLocator() override = default;

    // Non-copyable, non-movable — single instance per server
    CServiceLocator(const CServiceLocator&) = delete;
    CServiceLocator& operator=(const CServiceLocator&) = delete;

    ///
    /// Register a service by its hash key.
    /// @param  hash    FNV-1a hash of the service name
    /// @param  service Pointer to the service instance
    ///
    void RegisterService(uint64_t hash, void* service) noexcept
    {
        m_services[hash] = service;
    }

    ///
    /// Check if a service is registered.
    /// @param  hash    FNV-1a hash of the service name
    /// @return         true if registered
    ///
    bool HasService(uint64_t hash) const noexcept
    {
        return m_services.find(hash) != m_services.end();
    }

protected:
    void* GetServiceRaw(uint64_t hash) noexcept override
    {
        auto it = m_services.find(hash);
        if (it != m_services.end())
        {
            return it->second;
        }
        return nullptr;
    }
};

#endif // CSERVICELOCATOR_HPP
