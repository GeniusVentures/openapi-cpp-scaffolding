/**
 * @file       StorageError.hpp
 * @brief      Error codes for the storage engine layer
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#ifndef STORAGEERROR_HPP
#define STORAGEERROR_HPP

#include <cstdint>
#include "outcome/outcome.hpp"

///
/// Error codes for storage operations.
/// Maps to RocksDB status codes and key validation failures.
///
enum class StorageError : uint8_t
{
    OK = 0,              ///< Operation succeeded
    NOT_FOUND,           ///< Key does not exist in the database
    CORRUPTION,          ///< Data corruption detected
    IO_ERROR,            ///< Filesystem or I/O failure
    INVALID_ARGUMENT,    ///< Bad parameter passed to storage method
    INVALID_KEY,         ///< Key format validation failed (e.g. contains '..' or '/')
    UNKNOWN              ///< Unrecognized error
};

OUTCOME_HPP_DECLARE_ERROR(StorageError)

#endif // STORAGEERROR_HPP
