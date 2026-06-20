/**
 * @file       StorageError.cpp
 * @brief      Error category implementation for StorageError
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#include "storage/StorageError.hpp"

OUTCOME_CPP_DEFINE_CATEGORY(StorageError, e)
{
    switch (e)
    {
        case StorageError::OK:
            return "success";
        case StorageError::NOT_FOUND:
            return "not found";
        case StorageError::CORRUPTION:
            return "data corruption";
        case StorageError::IO_ERROR:
            return "I/O error";
        case StorageError::INVALID_ARGUMENT:
            return "invalid argument";
        case StorageError::INVALID_KEY:
            return "invalid key format";
        case StorageError::UNKNOWN:
            return "unknown error";
        default:
            return "unknown error";
    }
}
