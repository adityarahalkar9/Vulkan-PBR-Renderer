#pragma once
// Dedicated uploader thread: staging allocation, device-local copies and
// mipmap blit chains are produced off the render thread. Submissions share
// the graphics queue (guarded by the context submit mutex) and each task is
// fenced, so the main thread only ever sees assets that are fully resident.

#include "assets/assettypes.hpp"
#include "core/threadqueue.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

class VulkanContext;

class UploadQueue {
public:
    void init(VulkanContext& context);
    void start();
    void shutdown();                  // drains remaining tasks, then joins
    void enqueue(UploadTask&& task);
    UploadResult processSync(UploadTask&& task); // used for startup defaults

    ThreadQueue<UploadResult>& results() { return results_; }
    size_t pending() const { return pendingTasks_.load(std::memory_order_relaxed); }
    uint64_t uploadsCompleted() const { return completed_.load(std::memory_order_relaxed); }
    uint64_t bytesUploaded() const { return bytesUploaded_.load(std::memory_order_relaxed); }

private:
    void run();
    void doProcess(UploadTask& task, UploadResult& result);

    VulkanContext* ctx_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    std::thread worker_;
    std::deque<UploadTask> queue_;
    std::mutex mutex_;
    std::condition_variable cvWork_;
    bool stopping_ = false;
    bool running_ = false;
    std::atomic<uint64_t> pendingTasks_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> bytesUploaded_{0};
    ThreadQueue<UploadResult> results_;
};