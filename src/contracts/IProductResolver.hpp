/**
 * @file       IProductResolver.hpp
 * @brief      Plain-value product reference and price resolution capability
 * @date       2026-11-06
 */
#ifndef IPRODUCTRESOLVER_HPP
#define IPRODUCTRESOLVER_HPP

#include <cstdint>
#include <string>

#include "singleton/fnv1a.hpp"

enum class ProductResolutionState
{
    Resolved,
    Unknown,
    NoProvider
};

struct ProductResolution
{
    ProductResolutionState state = ProductResolutionState::Unknown;
    int32_t amount = 0;
    std::string currency;
    std::string errorCode;
    std::string errorMessage;
};

///
/// Resolves a product id to its tenant-scoped catalog price.
/// The service key is shared by the registering vertical and Commerce lookup.
/// This locator capability crosses dlopen boundaries; its absence is a
/// first-class state and plugins have no post-construction wiring point.
/// See .planning/research/CROSS-PROJECT.md section A1 for the rationale.
///
class IProductResolver
{
public:
    inline static constexpr uint64_t kServiceKey = gnus::hash::Fnv1a("IProductResolver");

    virtual ~IProductResolver() = default;

    virtual ProductResolution Resolve(
        const std::string& productId,
        const std::string& tenantId) noexcept = 0;
};

#endif // IPRODUCTRESOLVER_HPP