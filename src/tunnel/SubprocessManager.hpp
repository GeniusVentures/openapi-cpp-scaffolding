/**
 * @file       SubprocessManager.hpp
 * @brief      Cross-platform subprocess lifecycle manager with health monitoring and log capture
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */
#ifndef SUBPROCESSMANAGER_HPP
#define SUBPROCESSMANAGER_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

///
/// Snapshot of a managed subprocess's current operational status.
///
struct ProcessStatus
{
    bool running = false;               ///< Whether the subprocess is currently running
    int exitCode = 0;                   ///< Exit code of the last terminated process
    bool crashed = false;               ///< true if the last exit was abnormal (non-zero)
    std::string lastError;              ///< Most recent error message
    uint64_t restartCount = 0;          ///< Number of times the process has been restarted
};

///
/// Manages a child process lifecycle: spawn, graceful stop, auto-restart on crash,
/// and stderr log capture via a background monitoring thread.
///
/// Designed for managing cloudflared as a subprocess. Uses boost::process for
/// cross-platform process creation and control.
///
/// Lifecycle: Construct -> Spawn() -> (monitoring runs) -> Stop()
///
/// Non-copyable, non-movable (owns a child process handle and monitoring thread).
///
class SubprocessManager
{
public:
    ///
    /// Timeout in seconds to wait for graceful shutdown before force-killing.
    ///
    static constexpr unsigned int kStopTimeoutSeconds = 5;

    ///
    /// Delay in seconds before attempting an auto-restart after a crash.
    ///
    static constexpr unsigned int kRestartDelaySeconds = 2;

    ///
    /// Construct a SubprocessManager.
    /// Does not spawn any process — call Spawn() after construction.
    ///
    SubprocessManager() noexcept;

    ///
    /// Destructor. Calls Stop() if a subprocess is running.
    ///
    ~SubprocessManager() noexcept;

    // Non-copyable, non-movable
    SubprocessManager(const SubprocessManager&) = delete;
    SubprocessManager& operator=(const SubprocessManager&) = delete;
    SubprocessManager(SubprocessManager&&) = delete;
    SubprocessManager& operator=(SubprocessManager&&) = delete;

    ///
    /// Launch a child process with the given binary path and arguments.
    /// If a process is already running, Stop() is called first.
    /// A background thread monitors stderr and the process exit.
    /// @param  binaryPath  Absolute path to the executable
    /// @param  args        Command-line arguments (not including the binary name)
    /// @return true on success, false if the process could not be started
    ///
    bool Spawn(const std::string& binaryPath, const std::vector<std::string>& args) noexcept;

    ///
    /// Stop the running subprocess.
    /// Sends SIGTERM, waits up to kStopTimeoutSeconds, then sends SIGKILL if still alive.
    /// Joins the monitoring thread before returning.
    ///
    void Stop() noexcept;

    ///
    /// Restart the subprocess: Stop() then Spawn() with the same binary path and arguments.
    ///
    void Restart() noexcept;

    ///
    /// Get a snapshot of the current process status.
    /// Thread-safe.
    /// @return ProcessStatus with running state, exit code, crash flag, restart count
    ///
    ProcessStatus GetStatus() const noexcept;

    ///
    /// Enable or disable automatic restart on abnormal exit (non-zero exit code).
    /// @param  enabled  true to auto-restart on crash, false to disable
    ///
    void SetAutoRestart(bool enabled) noexcept;

    ///
    /// Set a callback that receives stderr output lines from the subprocess.
    /// Called from the monitoring thread — the callback must be thread-safe.
    /// @param  callback  Function called with each stderr line
    ///
    void SetLogCallback(std::function<void(const std::string&)> callback) noexcept;

private:
    ///
    /// Background thread entry point. Reads stderr and monitors process exit.
    ///
    void MonitorLoop() noexcept;

    mutable std::mutex m_mutex;
    std::thread m_monitorThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_autoRestart{false};
    std::atomic<uint64_t> m_restartCount{0};
    std::string m_lastError;
    std::string m_binaryPath;
    std::vector<std::string> m_args;
    std::function<void(const std::string&)> m_logCallback;

    // Platform-specific child process handle (opaque forward-declared in .cpp)
    struct ChildHandle;
    std::unique_ptr<ChildHandle> m_child;
};

#endif // SUBPROCESSMANAGER_HPP
