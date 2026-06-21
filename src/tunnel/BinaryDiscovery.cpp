/**
 * @file       BinaryDiscovery.cpp
 * @brief      Three-tier binary search — config path, plugin-relative, system PATH
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "tunnel/BinaryDiscovery.hpp"

#include <spdlog/spdlog.h>

#include <boost/process.hpp>

#include <filesystem>

namespace fs = std::filesystem;
namespace bp = boost::process;

// ============================================================================
// Constants
// ============================================================================

static constexpr char kBinaryName[] = "cloudflared";

// ============================================================================
// Public Interface
// ============================================================================

std::string BinaryDiscovery::FindBinary(
    const std::string& configPath,
    const std::string& pluginDir) noexcept
{
    // Tier 1: Config-specified path
    if (!configPath.empty())
    {
        // Reject paths with directory traversal (threat T-15-01)
        if (configPath.find("..") != std::string::npos)
        {
            spdlog::warn("BinaryDiscovery::FindBinary — rejecting config path "
                         "with '..' traversal: {}",
                         configPath);
        }
        else
        {
            spdlog::debug("BinaryDiscovery::FindBinary — checking config path: {}",
                          configPath);
            if (ValidateBinary(configPath))
            {
                spdlog::info("BinaryDiscovery::FindBinary — found via config path: {}",
                             configPath);
                return configPath;
            }
            spdlog::debug("BinaryDiscovery::FindBinary — config path not valid: {}",
                          configPath);
        }
    }

    // Tier 2: Plugin-relative path
    if (!pluginDir.empty())
    {
        fs::path pluginRelative = fs::path(pluginDir) / kBinaryName;
        std::string relativeStr = pluginRelative.string();
        spdlog::debug("BinaryDiscovery::FindBinary — checking plugin-relative: {}",
                      relativeStr);
        if (ValidateBinary(relativeStr))
        {
            spdlog::info("BinaryDiscovery::FindBinary — found via plugin-relative: {}",
                         relativeStr);
            return relativeStr;
        }
        spdlog::debug("BinaryDiscovery::FindBinary — plugin-relative not valid: {}",
                      relativeStr);
    }

    // Tier 3: System PATH via boost::process::search_path
    spdlog::debug("BinaryDiscovery::FindBinary — searching system PATH for {}",
                  kBinaryName);
    auto systemResult = bp::search_path(kBinaryName);
    if (!systemResult.empty())
    {
        std::string systemStr = systemResult.string();
        spdlog::info("BinaryDiscovery::FindBinary — found via system PATH: {}",
                     systemStr);
        return systemStr;
    }

    spdlog::warn("BinaryDiscovery::FindBinary — cloudflared not found in any search tier");
    return std::string();
}

bool BinaryDiscovery::ValidateBinary(const std::string& binaryPath) noexcept
{
    std::error_code ec;

    if (!fs::exists(binaryPath, ec) || ec)
    {
        return false;
    }

    return fs::is_regular_file(binaryPath, ec) && !ec;
}

const char* BinaryDiscovery::GetBinaryName() noexcept
{
    return kBinaryName;
}
