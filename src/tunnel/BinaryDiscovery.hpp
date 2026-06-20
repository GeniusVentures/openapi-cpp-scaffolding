/**
 * @file       BinaryDiscovery.hpp
 * @brief      Three-tier binary search for locating the cloudflared executable
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */
#ifndef BINARYDISCOVERY_HPP
#define BINARYDISCOVERY_HPP

#include <string>

///
/// Locates the cloudflared binary through a three-tier search:
/// 1. User-configured path (from config)
/// 2. Plugin-relative path (next to the plugin .dylib/.so)
/// 3. System PATH (via boost::process::search_path)
///
/// All methods are static — no instances needed.
///
class BinaryDiscovery
{
public:
    BinaryDiscovery() = delete;

    ///
    /// Search for the cloudflared binary using the three-tier strategy.
    /// Logs each search attempt via spdlog::debug for diagnostics.
    /// @param  configPath  User-configured path (empty if not set)
    /// @param  pluginDir   Directory containing the plugin binary
    /// @return Absolute path to the cloudflared binary, or empty string if not found
    ///
    static std::string FindBinary(
        const std::string& configPath,
        const std::string& pluginDir) noexcept;

    ///
    /// Validate that a binary path exists and is executable.
    /// On POSIX, checks owner-execute permission. On Windows, checks existence only.
    /// @param  binaryPath  Path to validate
    /// @return true if the file exists and (on POSIX) has execute permission
    ///
    static bool ValidateBinary(const std::string& binaryPath) noexcept;

private:
    ///
    /// Canonical binary name (without extension on POSIX, with .exe on Windows).
    ///
    static const char* GetBinaryName() noexcept;
};

#endif // BINARYDISCOVERY_HPP
