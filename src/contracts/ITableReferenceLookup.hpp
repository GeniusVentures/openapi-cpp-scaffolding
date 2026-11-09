/**
 * @file       ITableReferenceLookup.hpp
 * @brief      Plain-value tenant-scoped table-reference validation capability
 * @date       2026-11-09
 */
#ifndef ITABLEREFERENCELOOKUP_HPP
#define ITABLEREFERENCELOOKUP_HPP

#include <cstdint>
#include <string>

#include "singleton/fnv1a.hpp"

enum class TableReferenceState
{
    Resolved,
    InvalidKey,
    UnknownReference,
    CorruptRow
};

struct TableReference
{
    TableReferenceState state = TableReferenceState::UnknownReference;
    std::string tableKey;
    std::string errorCode;
    std::string errorMessage;
};

///
/// Validates that a table id references a stored table owned by the
/// caller's tenant, without exposing generated models. The resolved
/// tableKey is the registering vertical's own storage key for that row —
/// carried as a plain value so the consumer can complete its interim
/// post-persist transition without constructing foreign keys itself.
/// This locator capability crosses dlopen boundaries; when no provider is
/// registered the caller must fail closed on a supplied table id (treat it
/// as an unknown reference) rather than accepting the order as tableless.
/// See .planning/research/CROSS-PROJECT.md section A1 for the rationale.
///
class ITableReferenceLookup
{
public:
    inline static constexpr uint64_t kServiceKey = gnus::hash::Fnv1a("ITableReferenceLookup");

    virtual ~ITableReferenceLookup() = default;

    virtual TableReference ResolveTable(
        const std::string& tableId,
        const std::string& tenantId) noexcept = 0;
};

#endif // ITABLEREFERENCELOOKUP_HPP
