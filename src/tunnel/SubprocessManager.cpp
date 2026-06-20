/**
 * @file       SubprocessManager.cpp
 * @brief      Cross-platform subprocess lifecycle manager — spawn, stop, restart, log capture
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "tunnel/SubprocessManager.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <sstream>
#include <thread>

#include <boost/process.hpp>

namespace bp = boost::process;

// ============================================================================
// Platform-specific child handle (hides boost::process from the header)
// ============================================================================

struct SubprocessManager::ChildHandle
{
    bp::child process;
    bp::ipstream stderrStream;
};

// ============================================================================
// Construction / Destruction
// ============================================================================

SubprocessManager::SubprocessManager() noexcept
{
}

SubprocessManager::~SubprocessManager() noexcept
{
    if (m_running.load())
    {
        Stop();
    }
}

// ============================================================================
// Public Interface
// ============================================================================

bool SubprocessManager::Spawn(
    const std::string& binaryPath,
    const std::vector<std::string>& args) noexcept
{
    // Stop any existing process first
    if (m_running.load())
    {
        spdlog::debug("SubprocessManager::Spawn — stopping existing process before respawn");
        Stop();
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_binaryPath = binaryPath;
    m_args = args;

    // Build command-line string for logging
    std::ostringstream cmdLine;
    cmdLine << binaryPath;
    for (const auto& arg : args)
    {
        cmdLine << " " << arg;
    }
    spdlog::debug("SubprocessManager::Spawn — launching: {}", cmdLine.str());

    try
    {
        auto handle = std::make_unique<ChildHandle>();

        // Spawn child process with stderr piped to our stream
        handle->process = bp::child(
            binaryPath,
            bp::args(args),
            bp::std_err > handle->stderrStream);

        if (!handle->process.valid())
        {
            spdlog::error("SubprocessManager::Spawn — child process is not valid");
            m_lastError = "Child process is not valid";
            return false;
        }

        m_child = std::move(handle);
        m_running.store(true);

        // Launch monitor thread that reads stderr and watches for exit
        m_monitorThread = std::thread(&SubprocessManager::MonitorLoop, this);

        spdlog::info("SubprocessManager::Spawn — process started (PID: {})",
                     m_child->process.id());
        return true;
    }
    catch (const std::exception& ex)
    {
        spdlog::error("SubprocessManager::Spawn — failed to start: {}", ex.what());
        std::lock_guard<std::mutex> errLock(m_mutex);
        m_lastError = std::string("Failed to start process: ") + ex.what();
        return false;
    }
}

void SubprocessManager::Stop() noexcept
{
    if (!m_running.load())
    {
        return;
    }

    spdlog::debug("SubprocessManager::Stop — initiating graceful shutdown");
    m_running.store(false);

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_child && m_child->process.valid())
    {
        // Send SIGTERM for graceful shutdown
        spdlog::debug("SubprocessManager::Stop — sending SIGTERM");
        std::error_code ec;
        m_child->process.terminate(ec);
        if (ec)
        {
            spdlog::warn("SubprocessManager::Stop — SIGTERM failed: {}", ec.message());
        }

        // Wait up to kStopTimeoutSeconds for the process to exit
        auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(kStopTimeoutSeconds);
        while (m_child->process.running()
               && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        bool exited = !m_child->process.running();

        if (!exited)
        {
            // Force kill if still running
            spdlog::warn("SubprocessManager::Stop — process did not exit after {}s, "
                         "sending SIGKILL",
                         kStopTimeoutSeconds);
            m_child->process.terminate(ec);
            m_child->process.wait();  // Block until killed
        }

        int exitCode = m_child->process.exit_code();
        spdlog::info("SubprocessManager::Stop — process exited with code {}", exitCode);
    }

    // Join the monitor thread
    if (m_monitorThread.joinable())
    {
        m_monitorThread.join();
    }

    m_child.reset();
    spdlog::info("SubprocessManager::Stop — shutdown complete");
}

void SubprocessManager::Restart() noexcept
{
    spdlog::info("SubprocessManager::Restart — restarting process");

    // Capture current args before Stop() clears state
    std::string binaryPath;
    std::vector<std::string> args;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        binaryPath = m_binaryPath;
        args = m_args;
    }

    Stop();
    Spawn(binaryPath, args);
}

ProcessStatus SubprocessManager::GetStatus() const noexcept
{
    ProcessStatus status;
    std::lock_guard<std::mutex> lock(m_mutex);

    status.running = m_running.load();
    status.restartCount = m_restartCount.load();

    if (m_child && m_child->process.valid())
    {
        std::error_code ec;
        status.running = m_child->process.running(ec);
    }

    status.lastError = m_lastError;
    return status;
}

void SubprocessManager::SetAutoRestart(bool enabled) noexcept
{
    m_autoRestart.store(enabled);
    spdlog::debug("SubprocessManager::SetAutoRestart — auto-restart {}",
                  enabled ? "enabled" : "disabled");
}

void SubprocessManager::SetLogCallback(
    std::function<void(const std::string&)> callback) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_logCallback = std::move(callback);
}

// ============================================================================
// Monitor Thread
// ============================================================================

void SubprocessManager::MonitorLoop() noexcept
{
    spdlog::debug("SubprocessManager::MonitorLoop — monitor thread started");

    std::string line;
    while (m_running.load())
    {
        // Read stderr line-by-line (blocks until data available or pipe closes)
        if (m_child && std::getline(m_child->stderrStream, line))
        {
            // Invoke log callback if set
            std::function<void(const std::string&)> callback;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                callback = m_logCallback;
            }
            if (callback)
            {
                callback(line);
            }

            spdlog::debug("SubprocessManager::MonitorLoop — stderr: {}", line);
        }
        else
        {
            // Pipe closed — process likely exited
            break;
        }
    }

    // Process has exited
    int exitCode = 0;
    if (m_child)
    {
        std::error_code ec;
        m_child->process.wait();
        exitCode = m_child->process.exit_code();
    }

    bool abnormalExit = (exitCode != 0);
    if (abnormalExit)
    {
        spdlog::warn("SubprocessManager::MonitorLoop — process exited abnormally "
                     "(code: {})",
                     exitCode);
    }
    else
    {
        spdlog::info("SubprocessManager::MonitorLoop — process exited normally");
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (abnormalExit)
        {
            m_lastError = "Process exited with code " + std::to_string(exitCode);
        }
    }

    m_running.store(false);

    // Auto-restart if enabled and exit was abnormal
    if (m_autoRestart.load() && abnormalExit)
    {
        m_restartCount.fetch_add(1);
        spdlog::info("SubprocessManager::MonitorLoop — auto-restart #{} "
                     "in {} seconds",
                     m_restartCount.load(), kRestartDelaySeconds);

        // Delay before restart (interruptible via m_running)
        for (unsigned int i = 0; i < kRestartDelaySeconds; ++i)
        {
            if (!m_autoRestart.load())
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (m_autoRestart.load())
        {
            spdlog::info("SubprocessManager::MonitorLoop — initiating auto-restart");
            Spawn(m_binaryPath, m_args);
        }
    }

    spdlog::debug("SubprocessManager::MonitorLoop — monitor thread exiting");
}
