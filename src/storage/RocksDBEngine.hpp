/**
 * @file       RocksDBEngine.hpp
 * @brief      Concrete RocksDB-backed storage engine implementing IStorageEngine
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#ifndef ROCKSDBENGINE_HPP
#define ROCKSDBENGINE_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rocksdb/options.h>

#include "storage/IStorageEngine.hpp"
#include "outcome/outcome.hpp"

namespace rocksdb
{
class DB;
} // namespace rocksdb

///
/// Concrete storage engine backed by RocksDB.
/// Created via the static factory method Create(). Provides CRUD operations,
/// prefix scans with a custom '/'-delimited prefix extractor, and atomic batch writes.
///
class RocksDBEngine : public IStorageEngine
{
public:
    ~RocksDBEngine() override;

    ///
    /// Factory method to open or create a RocksDB database at the given path.
    /// @param  dbPath  Filesystem path for the database directory
    /// @return A unique_ptr to the engine on success, or StorageError on failure
    ///
    static outcome::result<std::unique_ptr<RocksDBEngine>> Create(
        const std::string& dbPath) noexcept;

    ///
    /// Store a key-value pair. Overwrites any existing value for the key.
    /// @param  key     The storage key (format: {domain}/{entity}/{hex_id})
    /// @param  value   The value to store (typically a JSON string)
    /// @return true on success, false on failure
    ///
    bool Put(const std::string& key, const std::string& value) noexcept override;

    ///
    /// Retrieve the value for a given key.
    /// @param  key         The storage key to look up
    /// @param  value_out   Output parameter — receives the stored value
    /// @return true if found, false if not found or on error
    ///
    bool Get(const std::string& key, std::string& value_out) noexcept override;

    ///
    /// Delete the value for a given key.
    /// @param  key     The storage key to delete
    /// @return true on success, false on failure
    ///
    bool Delete(const std::string& key) noexcept override;

    ///
    /// Scan all keys matching a given prefix. Returns key-value pairs in lexicographic order.
    /// @param  prefix  The key prefix to scan (e.g. "hrm/employees/")
    /// @return Vector of (key, value) pairs matching the prefix
    ///
    std::vector<std::pair<std::string, std::string>>
        Scan(const std::string& prefix) noexcept override;

    ///
    /// Execute multiple put and delete operations as an atomic batch.
    /// All operations succeed or all fail.
    /// @param  puts      Vector of (key, value) pairs to write
    /// @param  deletes   Vector of keys to delete
    /// @return true on success, false on failure
    ///
    bool WriteBatch(
        const std::vector<std::pair<std::string, std::string>>& puts,
        const std::vector<std::string>& deletes) noexcept override;

    ///
    /// Register a callback to be invoked after every successful Put, Delete,
    /// or WriteBatch operation. Multiple callbacks can be registered.
    /// @param  callback  The callback function to register
    ///
    void RegisterChangeCallback(ChangeCallback callback) noexcept override;

private:
    ///
    /// Private constructor — use Create() factory method.
    /// @param  db  Pointer to the opened RocksDB instance (takes ownership)
    ///
    explicit RocksDBEngine(rocksdb::DB* db) noexcept;

    ///
    /// Fire all registered change callbacks for a storage operation.
    /// Each callback is wrapped in a try/catch to prevent one bad callback
    /// from crashing the engine or blocking other callbacks.
    /// @param  key       The storage key that was modified
    /// @param  value     The new value (empty string for deletes)
    /// @param  isDelete  true if the operation was a delete
    ///
    void FireChangeCallbacks(
        const std::string& key,
        const std::string& value,
        bool isDelete) noexcept;

    std::unique_ptr<rocksdb::DB> m_db;          ///< Owned RocksDB instance
    rocksdb::ReadOptions m_readOptions;          ///< Default read options
    rocksdb::WriteOptions m_writeOptions;        ///< Default write options (sync=true)
    std::vector<ChangeCallback> m_changeCallbacks; ///< Registered change callbacks
};

#endif // ROCKSDBENGINE_HPP
