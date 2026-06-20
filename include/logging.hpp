/**
 * @file       logging.hpp
 * @brief      Centralized logging configuration for spdlog.
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 *
 * Include this header in any file that needs logging.
 * Call InitLogging() once at startup in main().
 */
#ifndef GENIUS_AI_BOSS_LOGGING_HPP
#define GENIUS_AI_BOSS_LOGGING_HPP

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

///
/// Initialize the global spdlog logger.
/// Call once in main() before any logging.
/// @param level  Log level (default: info). Use "debug" for verbose output.
/// @param logFile  Optional file path for log output. Empty = console only.
///
inline void InitLogging(
    spdlog::level::level_enum level = spdlog::level::info,
    const std::string& logFile = "")
{
    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    if (!logFile.empty())
    {
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile, true));
    }

    auto logger = std::make_shared<spdlog::logger>("genius", sinks.begin(), sinks.end());
    logger->set_level(level);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    spdlog::set_default_logger(logger);
}

///
/// Convenience macros for logging throughout the codebase.
/// Usage: SPDLOG_INFO("Server started on port {}", port);
///        SPDLOG_DEBUG("Handler dispatched: {}", path);
///        SPDLOG_ERROR("Failed to open DB: {}", error);
///
/// These are already defined by spdlog — listed here for reference:
///   SPDLOG_TRACE(...)
///   SPDLOG_DEBUG(...)
///   SPDLOG_INFO(...)
///   SPDLOG_WARN(...)
///   SPDLOG_ERROR(...)
///   SPDLOG_CRITICAL(...)

#endif // GENIUS_AI_BOSS_LOGGING_HPP
