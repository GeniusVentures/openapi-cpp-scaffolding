/**
 * @file       KeyBuilder.hpp
 * @brief      Utility for constructing and parsing storage keys
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#ifndef KEYBUILDER_HPP
#define KEYBUILDER_HPP

#include <string>
#include "outcome/outcome.hpp"
#include "storage/StorageError.hpp"

///
/// Parsed components of a storage key.
///
struct KeyParts
{
    std::string domain;  ///< API domain (e.g. "hrm", "commerce")
    std::string entity;  ///< Entity type (e.g. "employees", "orders")
    std::string id;      ///< Entity ID as hex string (32 chars, no hyphens)
};

///
/// Utility class for constructing and parsing storage keys.
/// Key format: {domain}/{entity}/{hex_id}
/// All methods are static. Cannot be instantiated.
///
class KeyBuilder
{
public:
    KeyBuilder() = delete;

    ///
    /// Build a storage key from domain, entity, and UUID.
    /// Strips hyphens from the UUID. Validates all segments.
    /// @param  domain  The API domain (e.g. "hrm")
    /// @param  entity  The entity type (e.g. "employees")
    /// @param  id      The entity UUID (e.g. "a1b2c3d4-e5f6-7890-abcd-ef1234567890")
    /// @return The constructed key on success, or StorageError on failure
    ///
    static outcome::result<std::string> Build(
        const std::string& domain,
        const std::string& entity,
        const std::string& id) noexcept;

    ///
    /// Parse a storage key into its component parts.
    /// @param  key     The storage key to parse (e.g. "hrm/employees/a1b2c3d4...")
    /// @return KeyParts on success, or StorageError if format is invalid
    ///
    static outcome::result<KeyParts> Parse(const std::string& key) noexcept;

    ///
    /// Build a prefix for scanning all entities of a given type.
    /// @param  domain  The API domain (e.g. "hrm")
    /// @param  entity  The entity type (optional, e.g. "employees")
    /// @return The prefix string (e.g. "hrm/" or "hrm/employees/")
    ///
    static std::string MakePrefix(
        const std::string& domain,
        const std::string& entity = "") noexcept;

    ///
    /// Remove all hyphen characters from a string.
    /// @param  uuid    The UUID string (with or without hyphens)
    /// @return The string with hyphens removed
    ///
    static std::string StripHyphens(const std::string& uuid) noexcept;

    ///
    /// Validate a key segment. Rejects empty strings, "..", and strings containing '/'.
    /// @param  seg     The segment to validate
    /// @return true if valid, false otherwise
    ///
    static bool IsValidSegment(const std::string& seg) noexcept;
};

#endif // KEYBUILDER_HPP
