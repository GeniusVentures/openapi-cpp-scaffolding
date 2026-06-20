/**
 * @file       fnv1a.hpp
 * @brief      Compile-time FNV-1a 64-bit hash for plugin handler lookup keys
 * @date       2026-05-25
 * @author     Kenneth L. Hurley
 */
#ifndef FNV1A_HPP
#define FNV1A_HPP

#include <cstdint>

namespace gnus
{
namespace hash
{

constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime       = 1099511628211ULL;

///
/// Compile-time FNV-1a 64-bit hash of a null-terminated string.
///
constexpr uint64_t Fnv1a(const char* str, uint64_t hash = kFnvOffsetBasis) noexcept
{
    return *str ? Fnv1a(str + 1, (hash ^ static_cast<uint64_t>(*str)) * kFnvPrime)
                : hash;
}

///
/// Runtime FNV-1a 64-bit hash of a std::string.
///
inline uint64_t Fnv1a(const std::string& str) noexcept
{
    uint64_t hash = kFnvOffsetBasis;
    for (const char c : str)
    {
        hash ^= static_cast<uint64_t>(c);
        hash *= kFnvPrime;
    }
    return hash;
}

} // namespace hash
} // namespace gnus

#endif // FNV1A_HPP
