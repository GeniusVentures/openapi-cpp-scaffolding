/**
 * @file       RocksDBEngine.cpp
 * @brief      Implementation of the RocksDB-backed storage engine
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#include "storage/RocksDBEngine.hpp"
#include "storage/StorageError.hpp"

#include <spdlog/spdlog.h>

#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

#include <algorithm>
#include <cassert>

namespace
{

///
/// Custom prefix extractor that captures {domain}/{entity}/ from storage keys.
/// Keys follow the format: {domain}/{entity}/{hex_id}
/// This extractor finds the second '/' and returns everything up to and including it.
///
class SlashPrefixTransform : public rocksdb::SliceTransform
{
public:
    const char* Name() const override
    {
        return "SlashPrefixTransform";
    }

    ///
    /// Extract the {domain}/{entity/ prefix from a key.
    /// Returns everything up to and including the second '/'.
    /// @param  key  The full storage key
    /// @return The prefix slice (e.g. "hrm/employees/")
    ///
    rocksdb::Slice Transform(const rocksdb::Slice& key) const override
    {
        // Find first '/'
        const char* data = key.data();
        size_t size = key.size();

        auto firstSlash = static_cast<const char*>(
            std::find( data, data + size, '/' ) );
        if ( firstSlash == data + size )
        {
            return key;
        }

        // Find second '/'
        auto secondSlash = static_cast<const char*>(
            std::find( firstSlash + 1, data + size, '/' ) );
        if ( secondSlash == data + size )
        {
            return key;
        }

        // Return up to and including the second '/'
        size_t prefixLen = static_cast<size_t>( secondSlash - data + 1 );
        return rocksdb::Slice( data, prefixLen );
    }

    ///
    /// Check if the key is in the domain of this transform.
    /// A key is in domain if it has at least 2 '/' characters.
    /// @param  key  The storage key to check
    /// @return true if the key has at least 2 slashes
    ///
    bool InDomain(const rocksdb::Slice& key) const override
    {
        const char* data = key.data();
        size_t size = key.size();
        int slashCount = 0;

        for ( size_t i = 0; i < size; ++i )
        {
            if ( data[i] == '/' )
            {
                ++slashCount;
                if ( slashCount >= 2 )
                {
                    return true;
                }
            }
        }

        return false;
    }
};

///
/// Map a RocksDB status to a StorageError code.
/// @param  status  The RocksDB status to map
/// @return The corresponding StorageError
///
StorageError MapRocksDbStatus(const rocksdb::Status& status)
{
    if ( status.ok() )
    {
        return StorageError::OK;
    }
    if ( status.IsNotFound() )
    {
        return StorageError::NOT_FOUND;
    }
    if ( status.IsCorruption() )
    {
        return StorageError::CORRUPTION;
    }
    if ( status.IsIOError() )
    {
        return StorageError::IO_ERROR;
    }
    if ( status.IsInvalidArgument() )
    {
        return StorageError::INVALID_ARGUMENT;
    }
    return StorageError::UNKNOWN;
}

} // anonymous namespace

RocksDBEngine::RocksDBEngine(rocksdb::DB* db) noexcept
    : m_db( db )
    , m_readOptions()
    , m_writeOptions()
{
    m_writeOptions.sync = true;
}

RocksDBEngine::~RocksDBEngine() = default;

outcome::result<std::unique_ptr<RocksDBEngine>> RocksDBEngine::Create(
    const std::string& dbPath) noexcept
{
    // Validate path — reject path traversal
    if ( dbPath.find( ".." ) != std::string::npos )
    {
        return StorageError::INVALID_ARGUMENT;
    }

    rocksdb::Options options;
    options.create_if_missing = true;

    // Snappy compression (available in thirdparty build)
    options.compression = rocksdb::kSnappyCompression;

    // Block-based table options for bloom filter
    rocksdb::BlockBasedTableOptions tableOptions;
    tableOptions.filter_policy.reset( rocksdb::NewBloomFilterPolicy( 10, false ) );
    tableOptions.whole_key_filtering = true;
    options.table_factory.reset( rocksdb::NewBlockBasedTableFactory( tableOptions ) );

    // Custom prefix extractor for {domain}/{entity}/ prefix scans
    options.prefix_extractor.reset( new SlashPrefixTransform() );

    // Open the database
    rocksdb::DB* db = nullptr;
    rocksdb::Status status = rocksdb::DB::Open( options, dbPath, &db );

    if ( !status.ok() )
    {
        return MapRocksDbStatus( status );
    }

    return std::unique_ptr<RocksDBEngine>( new RocksDBEngine( db ) );
}

bool RocksDBEngine::Put(
    const std::string& key,
    const std::string& value) noexcept
{
    rocksdb::Status status = m_db->Put( m_writeOptions, key, value );
    if ( status.ok() )
    {
        FireChangeCallbacks( key, value, false );
    }
    return status.ok();
}

bool RocksDBEngine::Get(
    const std::string& key,
    std::string& value_out) noexcept
{
    rocksdb::Status status = m_db->Get( m_readOptions, key, &value_out );

    if ( status.IsNotFound() )
    {
        return false;
    }

    return status.ok();
}

bool RocksDBEngine::Delete(const std::string& key) noexcept
{
    rocksdb::Status status = m_db->Delete( m_writeOptions, key );
    if ( status.ok() )
    {
        FireChangeCallbacks( key, "", true );
    }
    return status.ok();
}

std::vector<std::pair<std::string, std::string>>
RocksDBEngine::Scan(const std::string& prefix) noexcept
{
    std::vector<std::pair<std::string, std::string>> results;

    std::unique_ptr<rocksdb::Iterator> iter( m_db->NewIterator( m_readOptions ) );

    for ( iter->Seek( prefix ); iter->Valid(); iter->Next() )
    {
        rocksdb::Slice key = iter->key();

        // Check if the key starts with the prefix
        if ( key.size() < prefix.size() )
        {
            break;
        }

        if ( rocksdb::Slice( key.data(), prefix.size() ) != rocksdb::Slice( prefix ) )
        {
            break;
        }

        results.emplace_back(
            key.ToString(),
            iter->value().ToString() );
    }

    return results;
}

bool RocksDBEngine::WriteBatch(
    const std::vector<std::pair<std::string, std::string>>& puts,
    const std::vector<std::string>& deletes) noexcept
{
    rocksdb::WriteBatch batch;

    for ( const auto& put : puts )
    {
        batch.Put( put.first, put.second );
    }

    for ( const auto& del : deletes )
    {
        batch.Delete( del );
    }

    rocksdb::Status status = m_db->Write( m_writeOptions, &batch );
    if ( status.ok() )
    {
        for ( const auto& put : puts )
        {
            FireChangeCallbacks( put.first, put.second, false );
        }
        for ( const auto& del : deletes )
        {
            FireChangeCallbacks( del, "", true );
        }
    }
    return status.ok();
}

void RocksDBEngine::RegisterChangeCallback(ChangeCallback callback) noexcept
{
    m_changeCallbacks.push_back( std::move( callback ) );
}

void RocksDBEngine::FireChangeCallbacks(
    const std::string& key,
    const std::string& value,
    bool isDelete) noexcept
{
    for ( const auto& callback : m_changeCallbacks )
    {
        try
        {
            callback( key, value, isDelete );
        }
        catch ( const std::exception& ex )
        {
            spdlog::error( "ChangeCallback threw exception for key '{}': {}",
                           key, ex.what() );
        }
        catch ( ... )
        {
            spdlog::error( "ChangeCallback threw unknown exception for key '{}'",
                           key );
        }
    }
}
