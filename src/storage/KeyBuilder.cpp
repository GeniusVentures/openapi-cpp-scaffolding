/**
 * @file       KeyBuilder.cpp
 * @brief      Implementation of storage key construction and parsing
 * @date       2026-05-29
 * @author     Kenneth L. Hurley
 */
#include "storage/KeyBuilder.hpp"
#include <algorithm>

outcome::result<std::string> KeyBuilder::Build(
    const std::string& domain,
    const std::string& entity,
    const std::string& id) noexcept
{
    if ( !IsValidSegment( domain ) )
    {
        return StorageError::INVALID_KEY;
    }

    if ( !IsValidSegment( entity ) )
    {
        return StorageError::INVALID_KEY;
    }

    std::string hexId = StripHyphens( id );

    if ( !IsValidSegment( hexId ) )
    {
        return StorageError::INVALID_KEY;
    }

    return domain + "/" + entity + "/" + hexId;
}

outcome::result<KeyParts> KeyBuilder::Parse(const std::string& key) noexcept
{
    // Find first slash
    auto firstSlash = key.find( '/' );
    if ( firstSlash == std::string::npos )
    {
        return StorageError::INVALID_KEY;
    }

    // Find second slash
    auto secondSlash = key.find( '/', firstSlash + 1 );
    if ( secondSlash == std::string::npos )
    {
        return StorageError::INVALID_KEY;
    }

    // Reject if there's a third slash (in the id segment)
    if ( key.find( '/', secondSlash + 1 ) != std::string::npos )
    {
        return StorageError::INVALID_KEY;
    }

    std::string domain = key.substr( 0, firstSlash );
    std::string entity = key.substr( firstSlash + 1, secondSlash - firstSlash - 1 );
    std::string id     = key.substr( secondSlash + 1 );

    if ( !IsValidSegment( domain ) || !IsValidSegment( entity ) || !IsValidSegment( id ) )
    {
        return StorageError::INVALID_KEY;
    }

    return KeyParts{ domain, entity, id };
}

std::string KeyBuilder::MakePrefix(
    const std::string& domain,
    const std::string& entity) noexcept
{
    if ( entity.empty() )
    {
        return domain + "/";
    }
    return domain + "/" + entity + "/";
}

std::string KeyBuilder::StripHyphens(const std::string& uuid) noexcept
{
    std::string result;
    result.reserve( uuid.size() );
    for ( char c : uuid )
    {
        if ( c != '-' )
        {
            result.push_back( c );
        }
    }
    return result;
}

bool KeyBuilder::IsValidSegment(const std::string& seg) noexcept
{
    if ( seg.empty() )
    {
        return false;
    }

    // Reject ".." as a segment
    if ( seg == ".." )
    {
        return false;
    }

    // Reject segments containing '/'
    if ( seg.find( '/' ) != std::string::npos )
    {
        return false;
    }

    return true;
}
