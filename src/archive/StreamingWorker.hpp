/**
 * @file       StreamingWorker.hpp
 * @brief      Background worker that drains a thread-safe change queue and flushes JSONL batches to S3
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */
#ifndef STREAMINGWORKER_HPP
#define STREAMINGWORKER_HPP

#include "archive/S3Client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

///
/// A single storage change entry captured from the change callback.
///
struct ChangeEntry
{
    std::string key;        ///< Storage key that was modified
    std::string value;      ///< New value (empty for deletes)
    bool isDelete = false;  ///< true if this is a delete operation
    std::string timestamp;  ///< ISO 8601 UTC timestamp of the change
};

///
/// Snapshot of the worker's current operational status.
///
struct WorkerStatus
{
    size_t queue_size = 0;          ///< Number of entries waiting in the queue
    bool is_paused = false;         ///< Whether the worker is currently paused
    bool is_connected = false;      ///< Whether the S3 client is connected
    std::string last_error;         ///< Most recent error message
    uint64_t total_streamed = 0;    ///< Total entries successfully streamed
    uint64_t total_errors = 0;      ///< Total errors encountered
};

///
/// Background worker that drains a thread-safe queue of ChangeEntry items,
/// batches them into JSONL files, and uploads each batch to S3 via S3Client.
///
/// Lifecycle: Construct -> Start() -> (Enqueue changes) -> Stop()
/// Pause()/Resume() control whether the worker drains the queue.
///
/// Non-copyable, non-movable (owns a std::thread).
///
class StreamingWorker
{
public:
    ///
    /// Maximum number of entries per batch flush.
    ///
    static constexpr size_t kMaxBatchSize = 1000;

    ///
    /// Flush interval in seconds — the worker flushes even if the batch is not full.
    ///
    static constexpr unsigned int kFlushIntervalSeconds = 5;

    ///
    /// Maximum queue size before entries are dropped.
    ///
    static constexpr size_t kMaxQueueSize = 100000;

    ///
    /// Construct a StreamingWorker with the given S3 configuration.
    /// Does not start the worker thread — call Start() after construction.
    /// @param  config  S3-compatible endpoint configuration
    ///
    explicit StreamingWorker(const ArchiveConfig& config) noexcept;

    ///
    /// Destructor. Calls Stop() if the worker is still running.
    ///
    ~StreamingWorker() noexcept;

    // Non-copyable, non-movable
    StreamingWorker(const StreamingWorker&) = delete;
    StreamingWorker& operator=(const StreamingWorker&) = delete;
    StreamingWorker(StreamingWorker&&) = delete;
    StreamingWorker& operator=(StreamingWorker&&) = delete;

    ///
    /// Enqueue a storage change for streaming.
    /// If the queue is full (kMaxQueueSize), the entry is dropped and totalErrors is incremented.
    /// Thread-safe — can be called from any thread (e.g. the storage callback thread).
    /// @param  key       The storage key that was modified
    /// @param  value     The new value (empty for deletes)
    /// @param  isDelete  true if this is a delete operation
    ///
    void Enqueue(const std::string& key, const std::string& value, bool isDelete) noexcept;

    ///
    /// Start the background worker thread.
    /// The worker will attempt to connect to S3 and begin draining the queue.
    ///
    void Start() noexcept;

    ///
    /// Stop the background worker thread.
    /// Drains any remaining entries before joining the thread.
    ///
    void Stop() noexcept;

    ///
    /// Pause the worker — the queue continues to accept entries but they are not drained.
    ///
    void Pause() noexcept;

    ///
    /// Resume the worker after a pause.
    ///
    void Resume() noexcept;

    ///
    /// Get a snapshot of the worker's current status.
    /// Thread-safe.
    /// @return WorkerStatus with current queue size, pause state, connection state, etc.
    ///
    WorkerStatus GetStatus() const noexcept;

private:
    ///
    /// Background thread entry point. Runs until m_running is set to false.
    ///
    void WorkerLoop() noexcept;

    ///
    /// Format a batch of ChangeEntry items as JSONL and upload to S3.
    /// On failure, re-queues the batch for retry.
    /// @param  batch  Vector of entries to flush (will be cleared after processing)
    ///
    void FlushBatch(std::vector<ChangeEntry>& batch) noexcept;

    ArchiveConfig m_config;
    S3Client m_client;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    std::queue<ChangeEntry> m_queue;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_paused{false};
    std::atomic<uint64_t> m_totalStreamed{0};
    std::atomic<uint64_t> m_totalErrors{0};
    std::string m_lastError;
    uint64_t m_fileSequence = 0;
};

#endif // STREAMINGWORKER_HPP
