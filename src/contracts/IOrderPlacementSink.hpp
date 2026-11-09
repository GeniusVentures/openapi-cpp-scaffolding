/**
 * @file       IOrderPlacementSink.hpp
 * @brief      Plain-value order-placement notification capability
 * @date       2026-11-06
 */
#ifndef IORDERPLACEMENTSINK_HPP
#define IORDERPLACEMENTSINK_HPP

#include <cstdint>
#include <string>

#include "singleton/fnv1a.hpp"

///
/// Receives the post-persist order placement notification for table lifecycle
/// ownership. The transition is synchronous and void-returning (D-03/D-08):
/// success is reported through the plain-value out-parameter so the caller
/// can preserve its frozen partial-commit response when the owning vertical's
/// table update fails after the order is already durable. A provider that
/// cannot run the transition leaves transitionSucceeded false. This narrow
/// locator capability crosses a dlopen boundary; provider absence is
/// meaningful and plugins have no post-construction wiring point. See
/// .planning/research/CROSS-PROJECT.md section A1.
///
class IOrderPlacementSink
{
public:
    inline static constexpr uint64_t kServiceKey = gnus::hash::Fnv1a("IOrderPlacementSink");

    virtual ~IOrderPlacementSink() = default;

    virtual void OnOrderPlaced(
        const std::string& orderId,
        const std::string& tableId,
        const std::string& tenantId,
        bool& transitionSucceeded) = 0;
};

#endif // IORDERPLACEMENTSINK_HPP