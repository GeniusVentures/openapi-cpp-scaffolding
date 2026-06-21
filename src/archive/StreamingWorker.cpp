/**
 * @file       StreamingWorker.cpp
 * @brief      Background worker implementation — queue draining, batch flushing, S3 upload
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "archive/StreamingWorker.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ============================================================================
// Constants
// ============================================================================

constexpr unsigned int kConnectionRetrySeconds = 30;
constexpr unsigned int kPausedWaitSeconds = 1;

// ============================================================================
// Construction / Destruction
// ============================================================================

StreamingWorker::StreamingWorker(const ArchiveConfig& config) noexcept
    : m_config(config)
    , m_client(config)
{
}

StreamingWorker::~StreamingWorker() noexcept
{
    if (m_running.load())
    {
        Stop();
    }
}

// ============================================================================
// Public Interface
// ============================================================================

void StreamingWorker::Enqueue(
    const std::string& key,
    const std::string& value,
    bool isDelete) noexcept
{
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_queue.size() >= kMaxQueueSize)
    {
        SPDLOG_WARN("StreamingWorker::Enqueue — queue full ({}), dropping entry for key: {}",
                     kMaxQueueSize, key);
        m_totalErrors.fetch_add(1);
        return;
    }

    ChangeEntry entry;
    entry.key = key;
    entry.value = value;
    entry.isDelete = isDelete;

    // Generate ISO 8601 UTC timestamp
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utcTime{};
    const auto* tmPtr = std::gmtime(&time);
    if (tmPtr) { utcTime = *tmPtr; }
    std::ostringstream oss;
    oss << std::put_time(&utcTime, "%Y-%m-%dT%H:%M:%SZ");
    entry.timestamp = oss.str();

    m_queue.push(std::move(entry));
    m_condition.notify_one();
}

void StreamingWorker::Start() noexcept
{
    if (m_running.load())
    {
        SPDLOG_WARN("StreamingWorker::Start — worker already running");
        return;
    }

    m_running.store(true);
    m_thread = std::thread(&StreamingWorker::WorkerLoop, this);
    SPDLOG_INFO("StreamingWorker::Start — worker thread launched");
}

void StreamingWorker::Stop() noexcept
{
    if (!m_running.load())
    {
        return;
    }

    SPDLOG_INFO("StreamingWorker::Stop — stopping worker thread");
    m_running.store(false);
    m_condition.notify_one();

    if (m_thread.joinable())
    {
        m_thread.join();
    }

    SPDLOG_INFO("StreamingWorker::Stop — worker thread joined. "
                "Total streamed: {}, Total errors: {}",
                m_totalStreamed.load(), m_totalErrors.load());
}

void StreamingWorker::Pause() noexcept
{
    m_paused.store(true);
    SPDLOG_INFO("StreamingWorker::Pause — worker paused");
}

void StreamingWorker::Resume() noexcept
{
    m_paused.store(false);
    m_condition.notify_one();
    SPDLOG_INFO("StreamingWorker::Resume — worker resumed");
}

WorkerStatus StreamingWorker::GetStatus() const noexcept
{
    WorkerStatus status;
    std::lock_guard<std::mutex> lock(m_mutex);
    status.queue_size = m_queue.size();
    status.is_paused = m_paused.load();
    status.is_connected = m_client.IsConnected();
    status.last_error = m_lastError;
    status.total_streamed = m_totalStreamed.load();
    status.total_errors = m_totalErrors.load();
    return status;
}

// ============================================================================
// Background Worker Loop
// ============================================================================

void StreamingWorker::WorkerLoop() noexcept
{
    // Attempt initial connection to S3
    while (m_running.load() && !m_client.ConnectToEndpoint())
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = "Failed to connect to S3 endpoint, retrying in "
                          + std::to_string(kConnectionRetrySeconds) + "s";
        }
        SPDLOG_ERROR("StreamingWorker::WorkerLoop — {}", m_lastError);

        // Wait before retry (check m_running every second)
        for (unsigned int i = 0; i < kConnectionRetrySeconds && m_running.load(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    if (!m_running.load())
    {
        return;
    }

    SPDLOG_INFO("StreamingWorker::WorkerLoop — connected to S3, entering main loop");

    while (m_running.load())
    {
        // Handle pause state
        if (m_paused.load())
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait_for(lock, std::chrono::seconds(kPausedWaitSeconds),
                [this]() { return !m_running.load(); });
            continue;
        }

        // Wait for entries or flush timeout
        std::vector<ChangeEntry> batch;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait_for(lock,
                std::chrono::seconds(kFlushIntervalSeconds),
                [this]() { return !m_running.load() || !m_queue.empty(); });

            // Drain up to kMaxBatchSize entries
            while (!m_queue.empty() && batch.size() < kMaxBatchSize)
            {
                batch.push_back(std::move(m_queue.front()));
                m_queue.pop();
            }
        }

        // Flush batch if non-empty and connected
        if (!batch.empty() && m_client.IsConnected())
        {
            FlushBatch(batch);
        }
    }

    // Drain remaining entries on shutdown
    std::vector<ChangeEntry> remainingBatch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_queue.empty())
        {
            remainingBatch.push_back(std::move(m_queue.front()));
            m_queue.pop();
        }
    }

    if (!remainingBatch.empty() && m_client.IsConnected())
    {
        SPDLOG_INFO("StreamingWorker::WorkerLoop — flushing {} remaining entries on shutdown",
                     remainingBatch.size());
        FlushBatch(remainingBatch);
    }
}

// ============================================================================
// Batch Flushing
// ============================================================================

void StreamingWorker::FlushBatch(std::vector<ChangeEntry>& batch) noexcept
{
    // Build JSONL content
    std::string jsonlContent;
    jsonlContent.reserve(batch.size() * 256); // rough estimate

    for (const auto& entry : batch)
    {
        json line;
        line["op"] = entry.isDelete ? "delete" : "put";
        line["key"] = entry.key;
        if (!entry.isDelete)
        {
            line["value"] = entry.value;
        }
        line["timestamp"] = entry.timestamp;
        jsonlContent += line.dump();
        jsonlContent += '\n';
    }

    // Compute S3 key: prefix/YYYY/MM/DD/changes-NNNN.jsonl
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utcTime{};
    const auto* tmPtr = std::gmtime(&time);
    if (tmPtr) { utcTime = *tmPtr; }

    char datePath[11];
    std::strftime(datePath, sizeof(datePath), "%Y/%m/%d", &utcTime);

    std::ostringstream keyStream;
    keyStream << m_config.prefix << "/"
              << datePath << "/changes-"
              << std::setw(6) << std::setfill('0') << m_fileSequence
              << ".jsonl";
    std::string s3Key = keyStream.str();
    m_fileSequence++;

    // Upload to S3
    if (m_client.PutObject(s3Key, jsonlContent))
    {
        m_totalStreamed.fetch_add(batch.size());
        SPDLOG_DEBUG("StreamingWorker::FlushBatch — uploaded {} entries to {}",
                      batch.size(), s3Key);
    }
    else
    {
        m_totalErrors.fetch_add(batch.size());
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = "Failed to upload batch to S3 key: " + s3Key;
        }
        SPDLOG_ERROR("StreamingWorker::FlushBatch — {}", m_lastError);

        // Re-queue entries for retry (push back to front of queue)
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = batch.rbegin(); it != batch.rend(); ++it)
        {
            if (m_queue.size() < kMaxQueueSize)
            {
                m_queue.push(std::move(*it));
            }
            else
            {
                m_totalErrors.fetch_add(1);
                SPDLOG_WARN("StreamingWorker::FlushBatch — queue full during retry re-queue, "
                            "dropping entry for key: {}", it->key);
            }
        }
    }

    batch.clear();
}
