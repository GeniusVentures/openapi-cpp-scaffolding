/**
 * @file       test_streaming_worker.cpp
 * @brief      Unit tests for StreamingWorker
 * @date       2026-05-31
 * @author     Kenneth L. Hurley
 */

#include "archive/StreamingWorker.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

// ============================================================================
// Helper
// ============================================================================

///
/// Create a test ArchiveConfig with dummy values.
///
static ArchiveConfig MakeTestConfig()
{
    ArchiveConfig config;
    config.endpoint = "https://s3.amazonaws.com";
    config.bucket = "test-bucket";
    config.access_key = "AKIAIOSFODNN7EXAMPLE";
    config.secret_key = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    config.region = "us-east-1";
    config.prefix = "test-archive";
    return config;
}

// ============================================================================
// Construction Tests
// ============================================================================

/**
 * @brief  StreamingWorker constructed with config has empty queue
 */
TEST(StreamingWorkerTest, Construct_WithConfig_QueueIsEmpty)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    WorkerStatus status = worker.GetStatus();
    EXPECT_EQ(status.queue_size, 0u);
    EXPECT_FALSE(status.is_paused);
    EXPECT_FALSE(status.is_connected);
    EXPECT_EQ(status.total_streamed, 0u);
    EXPECT_EQ(status.total_errors, 0u);
}

// ============================================================================
// Enqueue Tests
// ============================================================================

/**
 * @brief  Enqueue stores a ChangeEntry in the queue
 */
TEST(StreamingWorkerTest, Enqueue_StoresEntry_InQueue)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    worker.Enqueue("test/key", "test-value", false);

    WorkerStatus status = worker.GetStatus();
    EXPECT_EQ(status.queue_size, 1u);
}

/**
 * @brief  Enqueue multiple entries increases queue size
 */
TEST(StreamingWorkerTest, Enqueue_MultipleEntries_IncreasesQueueSize)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    worker.Enqueue("key1", "value1", false);
    worker.Enqueue("key2", "value2", false);
    worker.Enqueue("key3", "", true);

    WorkerStatus status = worker.GetStatus();
    EXPECT_EQ(status.queue_size, 3u);
}

/**
 * @brief  Enqueue respects kMaxQueueSize and drops excess entries
 */
TEST(StreamingWorkerTest, Enqueue_DropsEntries_WhenQueueFull)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    // Fill the queue to kMaxQueueSize
    for (size_t i = 0; i < StreamingWorker::kMaxQueueSize; ++i)
    {
        worker.Enqueue("key/" + std::to_string(i), "value", false);
    }

    WorkerStatus status = worker.GetStatus();
    EXPECT_EQ(status.queue_size, StreamingWorker::kMaxQueueSize);

    // This should be dropped
    worker.Enqueue("dropped-key", "dropped-value", false);

    status = worker.GetStatus();
    EXPECT_EQ(status.queue_size, StreamingWorker::kMaxQueueSize);
    EXPECT_GT(status.total_errors, 0u);
}

// ============================================================================
// Pause/Resume Tests
// ============================================================================

/**
 * @brief  Pause sets is_paused to true
 */
TEST(StreamingWorkerTest, Pause_SetsIsPausedTrue)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    worker.Pause();
    WorkerStatus status = worker.GetStatus();
    EXPECT_TRUE(status.is_paused);
}

/**
 * @brief  Resume sets is_paused to false
 */
TEST(StreamingWorkerTest, Resume_SetsIsPausedFalse)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    worker.Pause();
    EXPECT_TRUE(worker.GetStatus().is_paused);

    worker.Resume();
    EXPECT_FALSE(worker.GetStatus().is_paused);
}

// ============================================================================
// Start/Stop Tests
// ============================================================================

/**
 * @brief  Start and Stop complete cleanly without crash
 *
 * Note: The worker will fail to connect to S3 (dummy config),
 * but Start/Stop should not crash or hang.
 */
TEST(StreamingWorkerTest, StartStop_CompletesCleanly)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    worker.Start();
    // Give the worker thread a moment to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Stop should join the thread cleanly
    // The worker will fail to connect, but Stop() should still work
    worker.Stop();

    // Verify we can call Stop again safely
    worker.Stop();
}

/**
 * @brief  Start when already running is a no-op
 */
TEST(StreamingWorkerTest, Start_WhenAlreadyRunning_IsNoOp)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    worker.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Second Start should not crash
    worker.Start();

    worker.Stop();
}

// ============================================================================
// GetStatus Tests
// ============================================================================

/**
 * @brief  GetStatus returns correct state after enqueue operations
 */
TEST(StreamingWorkerTest, GetStatus_CorrectAfterEnqueue)
{
    ArchiveConfig config = MakeTestConfig();
    StreamingWorker worker(config);

    worker.Enqueue("key1", "value1", false);
    worker.Enqueue("key2", "", true);

    WorkerStatus status = worker.GetStatus();
    EXPECT_EQ(status.queue_size, 2u);
    EXPECT_FALSE(status.is_paused);
    EXPECT_FALSE(status.is_connected); // not started, so not connected
    EXPECT_EQ(status.total_streamed, 0u);
    EXPECT_EQ(status.total_errors, 0u);
}
