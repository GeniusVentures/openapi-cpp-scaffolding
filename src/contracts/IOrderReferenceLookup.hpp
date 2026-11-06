/**
 * @file       IOrderReferenceLookup.hpp
 * @brief      Plain-value tenant-scoped order-reference lookup capability
 * @date       2026-11-06
 */
#ifndef IORDERREFERENCELOOKUP_HPP
#define IORDERREFERENCELOOKUP_HPP

#include <cstdint>
#include <string>

#include "singleton/fnv1a.hpp"

///
/// Answers whether an order exists for a tenant without exposing Commerce
/// storage keys or generated models. This narrow locator capability crosses
/// a dlopen boundary; provider absence is meaningful and plugins have no
/// post-construction wiring point. See .planning/research/CROSS-PROJECT.md
/// section A1.
///
class IOrderReferenceLookup
{
public:
    inline static constexpr uint64_t kServiceKey = gnus::hash::Fnv1a("IOrderReferenceLookup");

    virtual ~IOrderReferenceLookup() = default;

    virtual bool Exists(
        const std::string& orderId,
        const std::string& tenantId) noexcept = 0;
};

#endif // IORDERREFERENCELOOKUP_HPP