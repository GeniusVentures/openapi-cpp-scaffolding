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

enum class OrderReferenceState
{
    Found,
    InvalidKey,
    UnknownReference,
    CorruptRow
};

struct OrderReference
{
    OrderReferenceState state = OrderReferenceState::UnknownReference;
    std::string errorCode;
    std::string errorMessage;
};

///
/// Validates that an order id references a stored order owned by the
/// caller's tenant, without exposing Commerce storage keys or generated
/// models (D-09: the status-rich result preserves the distinct invalid-key,
/// missing/cross-tenant, and corrupt-row outcomes consumers froze). This
/// narrow locator capability crosses a dlopen boundary; provider absence is
/// meaningful and plugins have no post-construction wiring point. See
/// .planning/research/CROSS-PROJECT.md section A1.
///
class IOrderReferenceLookup
{
public:
    inline static constexpr uint64_t kServiceKey = gnus::hash::Fnv1a("IOrderReferenceLookup");

    virtual ~IOrderReferenceLookup() = default;

    virtual OrderReference Lookup(
        const std::string& orderId,
        const std::string& tenantId) noexcept = 0;
};

#endif // IORDERREFERENCELOOKUP_HPP