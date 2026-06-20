/**
 * @file       IStorageEngine.hpp
 * @brief      Abstract interface for the key-value storage engine
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#ifndef ISTORAGEENGINE_HPP
#define ISTORAGEENGINE_HPP

#include <functional>
#include <string>
#include <utility>
#include <vector>

///
/// Abstract storage engine interface.
/// Concrete implementations (e.g. RocksDBEngine) provide the persistence layer.
/// All methods are noexcept — errors are reported via return values.
///
class IStorageEngine
{
public:
    ///
    /// Callback signature for storage change notifications.
    /// Invoked after a successful Put, Delete, or WriteBatch operation.
    /// @param  key       The storage key that was modified
    /// @param  value     The new value (empty string for deletes)
    /// @param  isDelete  true if the operation was a delete, false for puts
    ///
    using ChangeCallback = std::function<void(
        const std::string& key,
        const std::string& value,
        bool isDelete)>;

    virtual ~IStorageEngine() = default;

    ///
    /// Store a key-value pair. Overwrites any existing value for the key.
    /// @param  key     The storage key (format: {domain}/{entity}/{hex_id})
    /// @param  value   The value to store (typically a JSON string)
    /// @return true on success, false on failure
    ///
    virtual bool Put(const std::string& key, const std::string& value) noexcept = 0;

    ///
    /// Retrieve the value for a given key.
    /// @param  key         The storage key to look up
    /// @param  value_out   Output parameter — receives the stored value
    /// @return true if found, false if not found or on error
    ///
    virtual bool Get(const std::string& key, std::string& value_out) noexcept = 0;

    ///
    /// Delete the value for a given key.
    /// @param  key     The storage key to delete
    /// @return true on success, false on failure
    ///
    virtual bool Delete(const std::string& key) noexcept = 0;

    ///
    /// Scan all keys matching a given prefix. Returns key-value pairs in lexicographic order.
    /// @param  prefix  The key prefix to scan (e.g. "hrm/employees/")
    /// @return Vector of (key, value) pairs matching the prefix
    ///
    virtual std::vector<std::pair<std::string, std::string>>
        Scan(const std::string& prefix) noexcept = 0;

    ///
    /// Execute multiple put and delete operations as an atomic batch.
    /// All operations succeed or all fail.
    /// @param  puts      Vector of (key, value) pairs to write
    /// @param  deletes   Vector of keys to delete
    /// @return true on success, false on failure
    ///
    virtual bool WriteBatch(
        const std::vector<std::pair<std::string, std::string>>& puts,
        const std::vector<std::string>& deletes) noexcept = 0;

    ///
    /// Register a callback to be invoked after every successful Put, Delete,
    /// or WriteBatch operation. Multiple callbacks can be registered.
    /// @param  callback  The callback function to register
    ///
    virtual void RegisterChangeCallback(ChangeCallback callback) noexcept = 0;
};

#endif // ISTORAGEENGINE_HPP
